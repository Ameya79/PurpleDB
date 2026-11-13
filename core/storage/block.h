#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include "purpledb/iterator.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Block represents an immutable data or index block in memory.
 */
class Block {
 public:
  // Comparator interface for keys in a block.
  // Defaults to standard bytewise Slice comparison.
  struct Comparator {
    virtual int Compare(const Slice& a, const Slice& b) const {
      return a.compare(b);
    }
    virtual ~Comparator() = default;
  };

  // Takes ownership of data if owned is true
  Block(const char* data, size_t size, bool owned = false);
  ~Block();

  Block(const Block&) = delete;
  Block& operator=(const Block&) = delete;

  size_t size() const { return size_; }

  // Returns an iterator over the block entries.
  // If cmp is null, standard bytewise comparison is used.
  std::unique_ptr<Iterator> NewIterator(const Comparator* cmp = nullptr) const;

 private:
  class Iter;

  const char* data_;
  size_t size_;
  uint32_t restart_offset_;
  bool owned_;
};

}  // namespace purpledb

