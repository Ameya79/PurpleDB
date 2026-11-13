#include "core/storage/merger.h"
#include <cassert>

namespace purpledb {

namespace {

class MergingIterator : public Iterator {
 public:
  MergingIterator(const Block::Comparator* cmp,
                  std::vector<std::unique_ptr<Iterator>> children)
      : cmp_(cmp),
        children_(std::move(children)),
        current_(nullptr),
        direction_(kForward) {}

  ~MergingIterator() override = default;

  bool Valid() const override {
    return current_ != nullptr && current_->Valid();
  }

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    direction_ = kForward;
    FindSmallest();
  }

  void SeekToLast() override {
    for (auto& child : children_) {
      child->SeekToLast();
    }
    direction_ = kReverse;
    FindLargest();
  }

  void Seek(const Slice& target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    direction_ = kForward;
    FindSmallest();
  }

  void Next() override {
    assert(Valid());

    // Ensure all children are positioned after key()
    if (direction_ != kForward) {
      for (auto& child : children_) {
        if (child.get() != current_) {
          child->Seek(key());
          if (child->Valid() && Compare(child->key(), key()) == 0) {
            child->Next();
          }
        }
      }
      direction_ = kForward;
    }

    current_->Next();
    FindSmallest();
  }

  void Prev() override {
    assert(Valid());

    // Ensure all children are positioned before key()
    if (direction_ != kReverse) {
      for (auto& child : children_) {
        if (child.get() != current_) {
          child->Seek(key());
          if (child->Valid()) {
            // Child is at or after key(). Step back one.
            child->Prev();
          } else {
            // Child has no entries >= key(). Position at last entry.
            child->SeekToLast();
          }
        }
      }
      direction_ = kReverse;
    }

    current_->Prev();
    FindLargest();
  }

  Slice key() const override {
    assert(Valid());
    return current_->key();
  }

  Slice value() const override {
    assert(Valid());
    return current_->value();
  }

  Status status() const override {
    for (const auto& child : children_) {
      if (!child->status().ok()) {
        return child->status();
      }
    }
    return Status::OK();
  }

 private:
  enum Direction { kForward, kReverse };

  int Compare(const Slice& a, const Slice& b) const {
    if (cmp_ != nullptr) {
      return cmp_->Compare(a, b);
    }
    return a.compare(b);
  }

  void FindSmallest() {
    Iterator* smallest = nullptr;
    for (auto& child : children_) {
      if (child->Valid()) {
        if (smallest == nullptr || Compare(child->key(), smallest->key()) < 0) {
          smallest = child.get();
        }
      }
    }
    current_ = smallest;
  }

  void FindLargest() {
    Iterator* largest = nullptr;
    for (auto& child : children_) {
      if (child->Valid()) {
        if (largest == nullptr || Compare(child->key(), largest->key()) > 0) {
          largest = child.get();
        }
      }
    }
    current_ = largest;
  }

  const Block::Comparator* cmp_;
  std::vector<std::unique_ptr<Iterator>> children_;
  Iterator* current_;
  Direction direction_;
};

}  // namespace

std::unique_ptr<Iterator> NewMergingIterator(
    const Block::Comparator* cmp,
    std::vector<std::unique_ptr<Iterator>> children) {
  if (children.empty()) {
    return NewEmptyIterator();
  } else if (children.size() == 1) {
    return std::move(children[0]);
  }
  return std::make_unique<MergingIterator>(cmp, std::move(children));
}

}  // namespace purpledb

