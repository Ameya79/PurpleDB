#include "core/storage/filter_block.h"
#include <cassert>
#include "core/util/coding.h"

namespace purpledb {

// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {}

void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  uint64_t filter_index = block_offset / kFilterBase;
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  start_.push_back(keys_.size());
  keys_.append(key.data(), key.size());
}

Slice FilterBlockBuilder::Finish() {
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of filter offsets
  const uint32_t array_offset = static_cast<uint32_t>(result_.size());
  for (uint32_t offset : filter_offsets_) {
    PutFixed32(&result_, offset);
  }

  // Append start of offset array
  PutFixed32(&result_, array_offset);
  // Append kFilterBaseLg
  result_.push_back(static_cast<char>(kFilterBaseLg));

  return Slice(result_);
}

void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path for empty block range: point to current end of result_
    filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
    return;
  }

  // Sentinel to easily compute length of the last key
  start_.push_back(keys_.size());
  tmp_keys_.resize(num_keys);
  for (size_t i = 0; i < num_keys; ++i) {
    const char* base = keys_.data() + start_[i];
    size_t length = start_[i + 1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Record start offset of this filter
  filter_offsets_.push_back(static_cast<uint32_t>(result_.size()));
  policy_->CreateFilter(tmp_keys_.data(), static_cast<int>(num_keys), &result_);

  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
    : policy_(policy), data_(nullptr), offset_(nullptr), num_(0), base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte base_lg + 4 byte array_offset

  base_lg_ = static_cast<uint8_t>(contents.data()[n - 1]);
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;

  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index * 4);
    uint32_t limit = DecodeFixed32(offset_ + index * 4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filter (no keys in this block range)
      return false;
    }
  }
  return true;  // Safe fallback if block_offset exceeds filter range
}

}  // namespace purpledb

