#include "core/storage/db_iter.h"
#include <cassert>
#include "core/util/coding.h"

namespace purpledb {

namespace {

class DBIter : public Iterator {
 public:
  DBIter(std::unique_ptr<Iterator> internal_iter, SequenceNumber sequence)
      : iter_(std::move(internal_iter)),
        sequence_(sequence),
        valid_(false) {}

  ~DBIter() override = default;

  bool Valid() const override { return valid_; }

  Slice key() const override {
    assert(Valid());
    return Slice(saved_key_);
  }

  Slice value() const override {
    assert(Valid());
    return Slice(saved_value_);
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    return iter_->status();
  }

  void SeekToFirst() override {
    iter_->SeekToFirst();
    FindNextUserEntry(true);
  }

  void SeekToLast() override {
    // For simplicity and correctness with internal key sequence order,
    // scanning forward until end or iterating from SeekToFirst
    SeekToFirst();
    if (!Valid()) return;
    while (true) {
      std::string prev_key = saved_key_;
      std::string prev_val = saved_value_;
      Next();
      if (!Valid()) {
        saved_key_ = prev_key;
        saved_value_ = prev_val;
        valid_ = true;
        break;
      }
    }
  }

  void Seek(const Slice& target) override {
    std::string lookup_key;
    lookup_key.append(target.data(), target.size());
    PutFixed64(&lookup_key, (sequence_ << 8) | static_cast<uint8_t>(kTypeValue));
    iter_->Seek(Slice(lookup_key));
    FindNextUserEntry(true);
  }

  void Next() override {
    assert(Valid());
    // Advance past any remaining older versions of the current user key
    std::string current_key = saved_key_;
    while (iter_->Valid()) {
      iter_->Next();
      if (!iter_->Valid()) break;
      Slice ikey = iter_->key();
      if (ikey.size() >= 8) {
        Slice ukey(ikey.data(), ikey.size() - 8);
        if (ukey != Slice(current_key)) {
          break;
        }
      }
    }
    FindNextUserEntry(false);
  }

  void Prev() override {
    // Forward-only for user queries is typical, but we support Prev by seeking to target
    assert(Valid());
    std::string target = saved_key_;
    SeekToFirst();
    std::string last_k, last_v;
    bool found_prev = false;
    while (Valid() && key().compare(Slice(target)) < 0) {
      last_k = key().ToString();
      last_v = value().ToString();
      found_prev = true;
      Next();
    }
    if (found_prev) {
      saved_key_ = last_k;
      saved_value_ = last_v;
      valid_ = true;
    } else {
      valid_ = false;
    }
  }

 private:
  void FindNextUserEntry(bool /*clear_saved_key*/) {
    while (iter_->Valid()) {
      Slice ikey = iter_->key();
      if (ikey.size() < 8) {
        status_ = Status::Corruption("malformed internal key in iterator");
        valid_ = false;
        return;
      }

      Slice ukey(ikey.data(), ikey.size() - 8);
      uint64_t tag = DecodeFixed64(ikey.data() + ikey.size() - 8);
      SequenceNumber seq = tag >> 8;
      ValueType type = static_cast<ValueType>(tag & 0xff);

      if (seq <= sequence_) {
        if (type == kTypeValue) {
          saved_key_ = ukey.ToString();
          saved_value_ = iter_->value().ToString();
          valid_ = true;
          return;
        } else if (type == kTypeDeletion) {
          // Key was deleted at or before snapshot. Skip older versions of this key.
          std::string del_key = ukey.ToString();
          while (iter_->Valid()) {
            iter_->Next();
            if (!iter_->Valid()) break;
            Slice next_ikey = iter_->key();
            if (next_ikey.size() >= 8) {
              Slice next_ukey(next_ikey.data(), next_ikey.size() - 8);
              if (next_ukey != Slice(del_key)) {
                break;
              }
            }
          }
          continue;
        }
      } else {
        // Version is newer than snapshot sequence, skip to older version
        iter_->Next();
      }
    }
    valid_ = false;
  }

  std::unique_ptr<Iterator> iter_;
  SequenceNumber sequence_;
  std::string saved_key_;
  std::string saved_value_;
  Status status_;
  bool valid_;
};

}  // namespace

std::unique_ptr<Iterator> NewDBIterator(
    std::unique_ptr<Iterator> internal_iter,
    SequenceNumber sequence) {
  return std::make_unique<DBIter>(std::move(internal_iter), sequence);
}

}  // namespace purpledb

