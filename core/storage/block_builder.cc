#include "core/storage/block_builder.h"

#include <algorithm>
#include <cassert>
#include "core/util/coding.h"

namespace purpledb {

BlockBuilder::BlockBuilder(int block_restart_interval)
    : block_restart_interval_(block_restart_interval) {
  assert(block_restart_interval_ >= 1);
  restarts_.push_back(0);  // First restart point is at offset 0
}

void BlockBuilder::Reset() {
  buffer_.clear();
  restarts_.clear();
  restarts_.push_back(0);
  counter_ = 0;
  finished_ = false;
  last_key_.clear();
}

size_t BlockBuilder::CurrentSizeEstimate() const {
  return (buffer_.size() +                        // Raw data buffer
          restarts_.size() * sizeof(uint32_t) +   // Restart array
          sizeof(uint32_t));                      // Restart array length
}

Slice BlockBuilder::Finish() {
  // Append restart array offsets
  for (uint32_t offset : restarts_) {
    PutFixed32(&buffer_, offset);
  }
  // Append restart array count
  PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));
  finished_ = true;
  return Slice(buffer_);
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
  assert(!finished_);
  assert(counter_ <= block_restart_interval_);

  size_t shared = 0;
  if (counter_ < block_restart_interval_) {
    // Calculate shared prefix with last key
    const size_t min_len = std::min(last_key_.size(), key.size());
    while ((shared < min_len) && (last_key_[shared] == key[shared])) {
      shared++;
    }
  } else {
    // Restart compression interval
    restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
    counter_ = 0;
  }

  const size_t non_shared = key.size() - shared;

  // Header: <shared> <non_shared> <value_size>
  PutVarint32(&buffer_, static_cast<uint32_t>(shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
  PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));

  // Payload: key_delta + value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  counter_++;
}

}  // namespace purpledb
