#include "core/storage/table.h"

#include <cassert>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {

namespace {

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(const Table* table, std::unique_ptr<Iterator> index_iter,
                   const Block::Comparator* cmp)
      : table_(table),
        index_iter_(std::move(index_iter)),
        cmp_(cmp),
        status_(Status::OK()) {}

  bool Valid() const override {
    return data_iter_ && data_iter_->Valid();
  }

  void SeekToFirst() override {
    index_iter_->SeekToFirst();
    InitDataBlock();
    if (data_iter_) {
      data_iter_->SeekToFirst();
    }
    SkipEmptyDataBlocksForward();
  }

  void SeekToLast() override {
    index_iter_->SeekToLast();
    InitDataBlock();
    if (data_iter_) {
      data_iter_->SeekToLast();
    }
    SkipEmptyDataBlocksBackward();
  }

  void Seek(const Slice& target) override {
    index_iter_->Seek(target);
    InitDataBlock();
    if (data_iter_) {
      data_iter_->Seek(target);
    }
    SkipEmptyDataBlocksForward();
  }

  void Next() override {
    assert(Valid());
    data_iter_->Next();
    SkipEmptyDataBlocksForward();
  }

  void Prev() override {
    assert(Valid());
    data_iter_->Prev();
    SkipEmptyDataBlocksBackward();
  }

  Slice key() const override {
    assert(Valid());
    return data_iter_->key();
  }

  Slice value() const override {
    assert(Valid());
    return data_iter_->value();
  }

  Status status() const override {
    if (!status_.ok()) return status_;
    if (data_iter_ && !data_iter_->status().ok()) return data_iter_->status();
    return index_iter_->status();
  }

 private:
  void InitDataBlock() {
    if (!index_iter_->Valid()) {
      data_iter_.reset();
      current_data_block_.reset();
      return;
    }

    Slice handle_slice = index_iter_->value();
    BlockHandle handle;
    Status s = handle.DecodeFrom(&handle_slice);
    if (!s.ok()) {
      status_ = s;
      data_iter_ = NewErrorIterator(s);
      return;
    }

    s = table_->ReadBlock(handle, &current_data_block_);
    if (!s.ok()) {
      status_ = s;
      data_iter_ = NewErrorIterator(s);
      return;
    }

    data_iter_ = current_data_block_->NewIterator(cmp_);
  }

  void SkipEmptyDataBlocksForward() {
    while (data_iter_ && !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        break;
      }
      index_iter_->Next();
      InitDataBlock();
      if (data_iter_) {
        data_iter_->SeekToFirst();
      }
    }
  }

  void SkipEmptyDataBlocksBackward() {
    while (data_iter_ && !data_iter_->Valid()) {
      if (!index_iter_->Valid()) {
        data_iter_.reset();
        break;
      }
      index_iter_->Prev();
      InitDataBlock();
      if (data_iter_) {
        data_iter_->SeekToLast();
      }
    }
  }

  const Table* table_;
  std::unique_ptr<Iterator> index_iter_;
  const Block::Comparator* cmp_;
  std::unique_ptr<Block> current_data_block_;
  std::unique_ptr<Iterator> data_iter_;
  Status status_;
};

}  // namespace

Table::Table(RandomAccessFile* file, Footer footer, std::unique_ptr<Block> index_block)
    : file_(file), footer_(footer), index_block_(std::move(index_block)) {}

Table::~Table() = default;

Status Table::Open(RandomAccessFile* file, uint64_t size, std::unique_ptr<Table>* table) {
  table->reset();
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file too short to be an SSTable");
  }

  char footer_space[Footer::kEncodedLength];
  Slice footer_slice;
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_slice, footer_space);
  if (!s.ok()) return s;

  Footer footer;
  s = footer.DecodeFrom(&footer_slice);
  if (!s.ok()) return s;

  // Read the index block
  std::unique_ptr<Block> index_block;
  size_t n = static_cast<size_t>(footer.index_handle().size());
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;
  s = file->Read(footer.index_handle().offset(), n + kBlockTrailerSize,
                 &contents, buf);
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated index block read");
  }

  // Checksum verification
  const char* data = contents.data();
  uint32_t expected_crc = DecodeFixed32(data + n + 1);
  uint32_t actual_crc = crc32c::Value(data, n);
  actual_crc = crc32c::Extend(actual_crc, data + n, 1);
  if (crc32c::Mask(actual_crc) != expected_crc) {
    delete[] buf;
    return Status::Corruption("index block checksum mismatch");
  }

  index_block = std::make_unique<Block>(buf, n, true);
  table->reset(new Table(file, footer, std::move(index_block)));
  return Status::OK();
}

Status Table::ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* result) const {
  result->reset();
  size_t n = static_cast<size_t>(handle.size());
  char* buf = new char[n + kBlockTrailerSize];
  Slice contents;
  Status s = file_->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated data block read");
  }

  // Checksum verification
  const char* data = contents.data();
  uint32_t expected_crc = DecodeFixed32(data + n + 1);
  uint32_t actual_crc = crc32c::Value(data, n);
  actual_crc = crc32c::Extend(actual_crc, data + n, 1);
  if (crc32c::Mask(actual_crc) != expected_crc) {
    delete[] buf;
    return Status::Corruption("data block checksum mismatch");
  }

  result->reset(new Block(buf, n, true));
  return Status::OK();
}

std::unique_ptr<Iterator> Table::NewIterator(const Block::Comparator* cmp) const {
  return std::make_unique<TwoLevelIterator>(this, index_block_->NewIterator(cmp), cmp);
}

}  // namespace purpledb
