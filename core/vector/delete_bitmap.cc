#include "core/vector/delete_bitmap.h"

#include <cstring>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {

DeleteBitmap::DeleteBitmap(uint64_t initial_capacity) {
  if (initial_capacity > 0) {
    uint64_t num_words = (initial_capacity + 63) / 64;
    words_.resize(num_words, 0);
    total_count_ = initial_capacity;
  }
}

void DeleteBitmap::EnsureCapacity(uint64_t vector_id) {
  uint64_t word_idx = vector_id / 64;
  if (word_idx >= words_.size()) {
    words_.resize(word_idx + 1, 0);
  }
  if (vector_id >= total_count_) {
    total_count_ = vector_id + 1;
  }
}

void DeleteBitmap::MarkDeleted(uint64_t vector_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  EnsureCapacity(vector_id);
  uint64_t word_idx = vector_id / 64;
  uint64_t bit_mask = 1ULL << (vector_id % 64);
  if ((words_[word_idx] & bit_mask) == 0) {
    words_[word_idx] |= bit_mask;
    deleted_count_++;
  }
}

void DeleteBitmap::ClearDeleted(uint64_t vector_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t word_idx = vector_id / 64;
  if (word_idx >= words_.size()) {
    return;
  }
  uint64_t bit_mask = 1ULL << (vector_id % 64);
  if ((words_[word_idx] & bit_mask) != 0) {
    words_[word_idx] &= ~bit_mask;
    if (deleted_count_ > 0) {
      deleted_count_--;
    }
  }
}

bool DeleteBitmap::IsDeleted(uint64_t vector_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t word_idx = vector_id / 64;
  if (word_idx >= words_.size()) {
    return false;
  }
  uint64_t bit_mask = 1ULL << (vector_id % 64);
  return (words_[word_idx] & bit_mask) != 0;
}

uint64_t DeleteBitmap::DeletedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return deleted_count_;
}

uint64_t DeleteBitmap::TotalCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_count_;
}

void DeleteBitmap::SetTotalCount(uint64_t total) {
  std::lock_guard<std::mutex> lock(mutex_);
  total_count_ = total;
  uint64_t num_words = (total + 63) / 64;
  if (num_words > words_.size()) {
    words_.resize(num_words, 0);
  }
}

double DeleteBitmap::DeletedFraction() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (total_count_ == 0) return 0.0;
  return static_cast<double>(deleted_count_) / static_cast<double>(total_count_);
}

void DeleteBitmap::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  words_.clear();
  total_count_ = 0;
  deleted_count_ = 0;
}

std::string DeleteBitmap::Serialize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::string buf;
  // Header: Magic (4B), Version (4B), TotalCount (8B), DeletedCount (8B), NumWords (8B)
  PutFixed32(&buf, kMagic);
  PutFixed32(&buf, kVersion);
  PutFixed64(&buf, total_count_);
  PutFixed64(&buf, deleted_count_);
  PutFixed64(&buf, static_cast<uint64_t>(words_.size()));

  for (uint64_t w : words_) {
    PutFixed64(&buf, w);
  }

  // Checksum over everything up to this point
  uint32_t crc = crc32c::Mask(crc32c::Value(buf.data(), buf.size()));
  PutFixed32(&buf, crc);
  return buf;
}

Status DeleteBitmap::Deserialize(const Slice& src) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Minimum length: Magic (4) + Version (4) + Total (8) + Del (8) + Words (8) + CRC (4) = 36 bytes
  if (src.size() < 36) {
    return Status::Corruption("DeleteBitmap: buffer too small to decode");
  }

  // Verify CRC32C checksum
  size_t payload_len = src.size() - 4;
  uint32_t expected_crc = DecodeFixed32(src.data() + payload_len);
  uint32_t actual_crc = crc32c::Mask(crc32c::Value(src.data(), payload_len));
  if (expected_crc != actual_crc) {
    return Status::Corruption("DeleteBitmap: checksum mismatch");
  }

  const char* p = src.data();
  uint32_t magic = DecodeFixed32(p);
  p += 4;
  if (magic != kMagic) {
    return Status::Corruption("DeleteBitmap: invalid magic number");
  }

  uint32_t version = DecodeFixed32(p);
  p += 4;
  if (version != kVersion) {
    return Status::Corruption("DeleteBitmap: unsupported version");
  }

  uint64_t total = DecodeFixed64(p);
  p += 8;
  uint64_t deleted = DecodeFixed64(p);
  p += 8;
  uint64_t num_words = DecodeFixed64(p);
  p += 8;

  if (payload_len < 32 + num_words * 8) {
    return Status::Corruption("DeleteBitmap: truncated word array");
  }

  std::vector<uint64_t> new_words(num_words);
  for (size_t i = 0; i < num_words; ++i) {
    new_words[i] = DecodeFixed64(p);
    p += 8;
  }

  words_ = std::move(new_words);
  total_count_ = total;
  deleted_count_ = deleted;
  return Status::OK();
}

}  // namespace purpledb

