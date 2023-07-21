#include "code/codeCache.hpp"
#include "topN/topNCodeHeap.hpp"
#include "logging/log.hpp"


TopNCodeHeap::TopNCodeHeap (const char* name, const int code_blob_type) : CodeHeap(name, code_blob_type) {}

HeapBlock* TopNCodeHeap::create_new_heapblock(size_t begin, size_t end) {
  // assert(end <= _next_segment, "sanity check");
  log_debug(topn)("create_new_heapblock: [%d, %d)", (int)begin, (int)end);
  HeapBlock* block = block_at(begin);
  block->set_used();
  block->set_length(end-begin);
  mark_segmap_as_used(begin, end, false);
  return block;
}

HeapBlock* TopNCodeHeap::create_new_freeblock(size_t begin, size_t end) {
  // assert(end <= _next_segment, "sanity check");
  // log_debug(topn)("create_new_freeblock: [%d, %d), freelist_length: %d", (int)begin, (int)end, _freelist_length);
  HeapBlock* block = block_at(begin);
  block->set_free();
  block->set_length(end-begin);
  mark_segmap_as_used(begin, end, false);
  // TODO: add some logic to maintain, read deallocate && deallocate_tail
  add_to_freelist(block);
  return block;
}

// split block b to maybe 3 segments: [0, begin), [begin, end), [end, limit)
HeapBlock* TopNCodeHeap::split_block_with_range(FreeBlock *b, size_t begin, size_t end, FreeBlock* prev) {
  if (b == NULL) return NULL;
  size_t limit = b->length();
  assert(segment_for(b) + limit <= _next_segment, "sanity check");
  assert((begin < end), "interval begin(%d) >= end(%d)", (int)begin, (int)end);
  assert((end <= limit), "split position (%d) out of range [0,%d)", (int)end, (int)limit);

  // Don't leave anything on the freelist smaller than CodeCacheMinBlockLength.
  HeapBlock* res;

  if (begin == 0) {
    // has no head freeblock, delete b from _freelist
    _freelist_length--;
    if (prev == NULL) {
      assert(_freelist == b, "sanity check");
      _freelist = _freelist->link();
    } else {
      assert((prev->link() == b), "sanity check");
      // Unmap element
      prev->set_link(b->link());
    }
    DEBUG_ONLY(memset((void*)b->allocated_space(), badCodeHeapNewVal, sizeof(FreeBlock) - sizeof(HeapBlock)));
  } else {
    b->set_length(begin);
  }

  if (end < limit) {
    // has tail free block
    HeapBlock* tailb = create_new_freeblock(segment_for(b) + end, segment_for(b) + limit);
  }

  b->set_length(begin);
  res = create_new_heapblock(segment_for(b) + begin, segment_for(b) + end);
  _freelist_segments -= res->length();

  return res;
}


/**
 * Search freelist for an entry on the list with property segment indices [begin, end).
 * @return NULL, if no one was found
 */
HeapBlock* TopNCodeHeap::search_block_in_freelist(size_t begin, size_t end) {
  assert((begin < end), "begin(%d) >= end(%d)", (int)begin, (int)end);
  FreeBlock* found_block  = NULL;
  FreeBlock* found_prev   = NULL;
  size_t found_begin = _next_segment;

  HeapBlock* res  = NULL;
  FreeBlock* prev = NULL;
  FreeBlock* cur  = _freelist;

  // Search for properly address block
  while(cur != NULL) {
    size_t cur_begin = segment_for(cur);
    size_t cur_end = cur_begin + cur->length();
    if (begin >= cur_begin && end <= cur_end) {
      found_block = cur;
      found_prev = prev;
      found_begin = cur_begin;
      break;
    }
    // Next element in list
    prev = cur;
    cur  = cur->link();
  }

  if (found_block == NULL) {
    // None found
    return NULL;
  }

  res = split_block_with_range(found_block, begin-found_begin, end-found_begin, found_prev);
  return res;
}


void* TopNCodeHeap::allocate_at_offset (size_t size, size_t offset){
  size_t number_of_segments = size_to_segments(size + header_size());
  assert(segments_to_size(number_of_segments) >= sizeof(FreeBlock), "not enough room for FreeList");
  assert_locked_or_safepoint(CodeCache_lock);

  // Ensure minimum size for allocation to the heap.
  number_of_segments = MAX2(CodeCacheMinBlockLength, number_of_segments);
  size_t aligned_offset = align_down(offset, CodeCacheSegmentSize);
  size_t begin = segment_for(low() + aligned_offset);
  size_t end = begin + number_of_segments;

  NOT_PRODUCT(verify());
  HeapBlock* block = search_block_in_freelist(begin, end);
  NOT_PRODUCT(verify());

  if (block != NULL) {
    log_debug(topn)("allocate_at_offset: [%d, %d), _next_segment: %d, find block in freelist", (int)begin, (int)end, (int)_next_segment);
    assert(!block->free(), "must not be marked free");
    guarantee((char*) block >= _memory.low_boundary() && (char*) block < _memory.high(),
              "The newly allocated block " INTPTR_FORMAT " is not within the heap "
              "starting with "  INTPTR_FORMAT " and ending with "  INTPTR_FORMAT,
              p2i(block), p2i(_memory.low_boundary()), p2i(_memory.high()));
    _max_allocated_capacity = MAX2(_max_allocated_capacity, allocated_capacity());
    _blob_count++;
    return block->allocated_space();
  }

  // if end >= _nubmer_of_committed_segments, need codeheap expand, return NULL
  if (end < _number_of_committed_segments) {
    // FIXME: assert begin >= _next_segment, _next_segment is high boundary of a method
    log_debug(topn)("allocate_at_offset: [%d, %d), _next_segment: %d, use unallocated space", (int)begin, (int)end, (int)_next_segment);
    if (begin < _next_segment) {
      log_trace(codecache)("Warning: begin(%d) < _next_segment(%d), try to allocate in NonProfiledCodeHeap", (int)begin, (int)_next_segment);
      return CodeCache::allocate(size, CodeBlobType::MethodNonProfiled);
    }
    assert(begin >= _next_segment, "sanity check %d < %d", (int)begin, (int)_next_segment);
    if (begin > _next_segment) {
      // add a free block from _next_segment to begin
      HeapBlock* newb = create_new_freeblock(_next_segment, begin);
      _freelist_segments += newb->length();
      guarantee((char*) newb >= _memory.low_boundary() && (char*) newb < _memory.high(),
        "The newly allocated block " INTPTR_FORMAT " is not within the heap "
        "starting with "  INTPTR_FORMAT " and ending with " INTPTR_FORMAT,
        p2i(newb), p2i(_memory.low_boundary()), p2i(_memory.high()));
      NOT_PRODUCT(verify());
    } else {
      begin = _next_segment;
    }
    _next_segment = MAX2(_next_segment, end);

    block = create_new_heapblock(begin, end);
    guarantee((char*) block >= _memory.low_boundary() && (char*) block < _memory.high(),
      "The newly allocated block " INTPTR_FORMAT " is not within the heap "
      "starting with "  INTPTR_FORMAT " and ending with " INTPTR_FORMAT,
      p2i(block), p2i(_memory.low_boundary()), p2i(_memory.high()));
    NOT_PRODUCT(verify());
    _max_allocated_capacity = MAX2(_max_allocated_capacity, allocated_capacity());
    _blob_count++;
    return block->allocated_space();
  } else {
    return NULL;
  }
}