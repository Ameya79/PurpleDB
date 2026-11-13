#include "purpledb/iterator.h"

namespace purpledb {

namespace {

class EmptyIterator : public Iterator {
 public:
  explicit EmptyIterator(Status s) : status_(std::move(s)) {}

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void SeekToLast() override {}
  void Seek(const Slice& /*target*/) override {}
  void Next() override {}
  void Prev() override {}
  Slice key() const override { return Slice(); }
  Slice value() const override { return Slice(); }
  Status status() const override { return status_; }

 private:
  Status status_;
};

}  // namespace

std::unique_ptr<Iterator> NewEmptyIterator() {
  return std::make_unique<EmptyIterator>(Status::OK());
}

std::unique_ptr<Iterator> NewErrorIterator(const Status& status) {
  return std::make_unique<EmptyIterator>(status);
}

}  // namespace purpledb
