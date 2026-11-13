#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief BlockBuilder accumulates key-value pairs into a single block with prefix compression.
 *
 * CONCEPT FOR LEARNERS:
 * Keys in a block are sorted, meaning adjacent keys often share long prefixes
 * (e.g., "users/1000/name" and "users/1000/photo"). BlockBuilder stores only
 * the delta (shared_len, unshared_len, value_len, unshared_key, value).
 * Every N entries (e.g. 16), a "restart point" is emitted where shared_len=0.
 * This allows binary searching restart points without decompressing the entire block!
 */
class BlockBuilder {
 public:
  explicit BlockBuilder(int block_restart_interval = 16);

  BlockBuilder(const BlockBuilder&) = delete;
  BlockBuilder& operator=(const BlockBuilder&) = delete;

  // Reset the builder to an empty state.
  void Reset();

  // Appends key and value to the block.
  // REQUIRES: key is greater than any previously added key.
  // REQUIRES: Finish() has not been called since the last Reset().
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the block contents.
  // The returned slice is valid until the next call to Reset() or Add().
  Slice Finish();

  // Returns an estimate of the current size of the block in bytes.
  size_t CurrentSizeEstimate() const;

  bool empty() const { return buffer_.empty(); }

 private:
  const int block_restart_interval_;
  std::string buffer_;
  std::vector<uint32_t> restarts_;
  int counter_{0};
  bool finished_{false};
  std::string last_key_;
};

}  // namespace purpledb
