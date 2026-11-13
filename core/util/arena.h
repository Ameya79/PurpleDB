#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace purpledb {

/**
 * @brief Arena is a fast bump-pointer memory allocator.
 *
 * CONCEPT FOR LEARNERS:
 * In high-throughput storage engines, calling malloc/new for every single key
 * and node in an in-memory index causes heap fragmentation and severe allocator
 * lock contention. An Arena allocates large blocks (e.g. 4 KiB) from the OS
 * and hands out chunks by simply incrementing a pointer ("bump pointer allocation").
 * Memory is never freed individually; the entire Arena is deallocated in one step
 * when the MemTable is flushed and destroyed.
 */
class Arena {
 public:
  Arena();
  ~Arena();

  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // Allocate bytes with byte alignment.
  char* Allocate(size_t bytes);

  // Allocate bytes aligned to 8-byte boundaries.
  char* AllocateAligned(size_t bytes);

  // Return total bytes allocated from the OS.
  size_t MemoryUsage() const {
    return memory_usage_.load(std::memory_order_relaxed);
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  char* alloc_ptr_{nullptr};
  size_t alloc_bytes_remaining_{0};

  std::vector<char*> blocks_;
  std::atomic<size_t> memory_usage_{0};
};

inline char* Arena::Allocate(size_t bytes) {
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace purpledb
