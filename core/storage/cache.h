#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief Thread-safe sharded LRU cache for SSTable data blocks.
 *
 * CONCEPT FOR LEARNERS:
 * Reading blocks from disk involves OS syscalls, buffer copies, and checksum validation.
 * A Block Cache stores decompressed, parsed Block objects in memory.
 * To avoid multi-threaded lock contention, the cache is sharded into 16 independent
 * LRU sub-caches based on key hash.
 */
class Cache {
 public:
  struct Handle {};

  Cache() = default;
  virtual ~Cache() = default;

  Cache(const Cache&) = delete;
  Cache& operator=(const Cache&) = delete;

  // Insert a mapping from key->value into the cache with a specified charge.
  // Returns a handle. The caller must call Release(handle) when done.
  virtual Handle* Insert(const Slice& key, void* value, size_t charge,
                         void (*deleter)(const Slice& key, void* value)) = 0;

  // Lookup key. Returns handle if found, else nullptr.
  // The caller must call Release(handle) when done.
  virtual Handle* Lookup(const Slice& key) = 0;

  // Release a handle returned by Insert() or Lookup().
  virtual void Release(Handle* handle) = 0;

  // Return the value encapsulated in a handle.
  virtual void* Value(Handle* handle) = 0;

  // Erase the entry for key from the cache.
  virtual void Erase(const Slice& key) = 0;

  // Return a new unique numeric id for partitioning key spaces.
  virtual uint64_t NewId() = 0;

  // Remove all entries that are not actively in use.
  virtual void Prune() = 0;

  // Return total charge of all elements in the cache.
  virtual size_t TotalCharge() const = 0;
};

// Create a new sharded LRU cache with the specified capacity in bytes.
std::shared_ptr<Cache> NewLRUCache(size_t capacity);

}  // namespace purpledb

