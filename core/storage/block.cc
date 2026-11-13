#include "core/storage/block.h"

#include <cassert>
#include <cstring>
#include "core/util/coding.h"

namespace purpledb {

namespace {

struct DefaultComparator : public Block::Comparator {
  int Compare(const Slice& a, const Slice& b) const override {
    return a.compare(b);
  }
};

static const DefaultComparator kDefaultComparator;

}  // namespace

class Block::Iter : public Iterator {
 public:
  Iter(const char* data, size_t size, uint32_t restart_offset, uint32_t num_restarts,
       const Block::Comparator* cmp)
      : data_(data),
        size_(size),
        restarts_(restart_offset),
        num_restarts_(num_restarts),
        current_(restarts_),
        restart_index_(num_restarts_),
        status_(Status::OK()),
        cmp_(cmp ? cmp : &kDefaultComparator) {}

  bool Valid() const override { return current_ < restarts_; }

  Status status() const override { return status_; }

  Slice key() const override {
    assert(Valid());
    return Slice(key_);
  }

  Slice value() const override {
    assert(Valid());
    return value_;
  }

  void Next() override {
    assert(Valid());
    uint32_t next_offset = NextEntryOffset();
    if (next_offset >= restarts_) {
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return;
    }
    current_ = next_offset;
    ParseNextKey();
  }

  void Prev() override {
    assert(Valid());

    const uint32_t original = current_;
    while (GetRestartPoint(restart_index_) >= original) {
      if (restart_index_ == 0) {
        // No more entries prior to this
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    while (NextEntryOffset() < original) {
      current_ = NextEntryOffset();
      if (!ParseNextKey()) break;
    }
  }

  void Seek(const Slice& target) override {
    if (num_restarts_ == 0) {
      current_ = restarts_;
      restart_index_ = 0;
      return;
    }

    // Binary search across restarts array to find highest restart point < target
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
      uint32_t mid = left + (right - left + 1) / 2;
      uint32_t region_offset = GetRestartPoint(mid);
      uint32_t shared, non_shared, value_length;
      const char* key_ptr;
      if (!DecodeEntry(data_ + region_offset, data_ + restarts_, &shared,
                       &non_shared, &value_length, &key_ptr) ||
          shared != 0) {
        CorruptionError();
        return;
      }
      Slice mid_key(key_ptr, non_shared);
      if (cmp_->Compare(mid_key, target) < 0) {
        left = mid;
      } else {
        right = mid - 1;
      }
    }

    SeekToRestartPoint(left);
    while (Valid()) {
      if (cmp_->Compare(key(), target) >= 0) {
        return;
      }
      uint32_t next_offset = NextEntryOffset();
      if (next_offset >= restarts_) {
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      current_ = next_offset;
      if (!ParseNextKey()) {
        return;
      }
    }
  }

  void SeekToFirst() override {
    if (num_restarts_ == 0) {
      current_ = restarts_;
      restart_index_ = 0;
      return;
    }
    SeekToRestartPoint(0);
  }

  void SeekToLast() override {
    if (num_restarts_ == 0) {
      current_ = restarts_;
      restart_index_ = 0;
      return;
    }
    SeekToRestartPoint(num_restarts_ - 1);
    if (!Valid()) return;
    while (NextEntryOffset() < restarts_) {
      current_ = NextEntryOffset();
      if (!ParseNextKey()) break;
    }
  }

 private:
  uint32_t GetRestartPoint(uint32_t index) const {
    assert(index < num_restarts_);
    return DecodeFixed32(data_ + restarts_ + index * sizeof(uint32_t));
  }

  void SeekToRestartPoint(uint32_t index) {
    key_.clear();
    restart_index_ = index;
    current_ = GetRestartPoint(index);
    value_ = Slice();
    ParseNextKey();
  }

  uint32_t NextEntryOffset() const {
    return static_cast<uint32_t>((value_.data() + value_.size()) - data_);
  }

  bool CorruptionError() {
    current_ = restarts_;
    restart_index_ = num_restarts_;
    status_ = Status::Corruption("bad entry in block");
    key_.clear();
    value_ = Slice();
    return false;
  }

  static bool DecodeEntry(const char* p, const char* limit, uint32_t* shared,
                          uint32_t* non_shared, uint32_t* value_length,
                          const char** key_ptr) {
    if (limit - p < 3) return false;
    p = GetVarint32Ptr(p, limit, shared);
    if (!p) return false;
    p = GetVarint32Ptr(p, limit, non_shared);
    if (!p) return false;
    p = GetVarint32Ptr(p, limit, value_length);
    if (!p) return false;

    if (static_cast<size_t>(limit - p) < (*non_shared + *value_length)) {
      return false;
    }
    *key_ptr = p;
    return true;
  }

  bool ParseNextKey() {
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;
    if (p >= limit) {
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    uint32_t shared, non_shared, value_length;
    const char* key_ptr;
    if (!DecodeEntry(p, limit, &shared, &non_shared, &value_length, &key_ptr)) {
      return CorruptionError();
    }
    if (key_.size() < shared) {
      return CorruptionError();
    }

    key_.resize(shared);
    key_.append(key_ptr, non_shared);
    value_ = Slice(key_ptr + non_shared, value_length);

    while (restart_index_ + 1 < num_restarts_ &&
           GetRestartPoint(restart_index_ + 1) <= current_) {
      restart_index_++;
    }
    return true;
  }

  const char* data_;
  size_t size_;
  uint32_t restarts_;
  uint32_t num_restarts_;
  uint32_t current_;
  uint32_t restart_index_;
  std::string key_;
  Slice value_;
  Status status_;
  const Block::Comparator* cmp_;
};

Block::Block(const char* data, size_t size, bool owned)
    : data_(data), size_(size), restart_offset_(0), owned_(owned) {
  if (size_ < sizeof(uint32_t)) {
    size_ = 0;  // Error marker
  } else {
    size_t max_restarts_allowed = (size_ - sizeof(uint32_t)) / sizeof(uint32_t);
    uint32_t num_restarts = DecodeFixed32(data_ + size_ - sizeof(uint32_t));
    if (num_restarts > max_restarts_allowed) {
      size_ = 0;
    } else {
      restart_offset_ = static_cast<uint32_t>(size_ - (1 + num_restarts) * sizeof(uint32_t));
    }
  }
}

Block::~Block() {
  if (owned_ && data_ != nullptr) {
    delete[] data_;
  }
}

std::unique_ptr<Iterator> Block::NewIterator(const Block::Comparator* cmp) const {
  if (size_ < sizeof(uint32_t)) {
    return NewErrorIterator(Status::Corruption("bad block contents"));
  }
  uint32_t num_restarts = DecodeFixed32(data_ + size_ - sizeof(uint32_t));
  if (num_restarts == 0) {
    return NewEmptyIterator();
  }
  return std::make_unique<Block::Iter>(data_, size_, restart_offset_, num_restarts, cmp);
}

}  // namespace purpledb
