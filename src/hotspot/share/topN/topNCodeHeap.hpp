#ifndef SHARE_VM_TOPN_TONCODEHEAP_HPP
#define SHARE_VM_TOPN_TONCODEHEAP_HPP

#include "runtime/globals.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/macros.hpp"

#include "memory/heap.hpp"
#include "runtime/atomic.hpp"

class TopNCodeHeap : public CodeHeap {
private:
  HeapBlock* create_new_heapblock(size_t begin, size_t end);
  HeapBlock* create_new_freeblock(size_t begin, size_t end);
  HeapBlock* search_block_in_freelist(size_t begin, size_t length);
  HeapBlock* split_block_with_range(FreeBlock *b, size_t begin, size_t end, FreeBlock* prev);
public:
  TopNCodeHeap(const char* name, const int code_blob_type);
  // Memory allocation
  // Allocate 'size' bytes in the code cache at offset(in bytes) or return NULL
  void* allocate_at_offset (size_t size, size_t offset);
  // void  deallocate(void* p);    // Deallocate memory
};

#endif