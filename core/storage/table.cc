#include "core/storage/table.h"

#include <cassert>
#include <iostream>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"
#include "purpledb/write_batch.h"

namespace purpledb {

namespace {

class TwoLevelIterator : public Iterator {
 public:
  TwoLevelIterator(const Table* table, std::shared_ptr<const Table> owned_table,
                   std::unique_ptr<Iterator> index_iter,
                   const Block::Comparator* cmp)
      : table_(table),
        owned_table_(std::move(owned_table)),
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
  std::shared_ptr<const Table> owned_table_;
  std::unique_ptr<Iterator> index_iter_;
  const Block::Comparator* cmp_;
  std::unique_ptr<Block> current_data_block_;
  std::unique_ptr<Iterator> data_iter_;
  Status status_;
};

}  // namespace

Table::Table(std::unique_ptr<RandomAccessFile> file, Footer footer,
             std::unique_ptr<Block> index_block,
             std::unique_ptr<char[]> filter_data,
             std::unique_ptr<FilterBlockReader> filter_reader)
    : file_(std::move(file)),
      footer_(footer),
      index_block_(std::move(index_block)),
      filter_data_(std::move(filter_data)),
      filter_reader_(std::move(filter_reader)) {}

Table::~Table() = default;

Status Table::Open(std::unique_ptr<RandomAccessFile> file, uint64_t size,
                   std::unique_ptr<Table>* table,
                   const FilterPolicy* filter_policy) {
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

  // Read metaindex block and filter block if filter_policy is present
  std::unique_ptr<char[]> filter_data;
  std::unique_ptr<FilterBlockReader> filter_reader;
  if (filter_policy != nullptr && footer.metaindex_handle().size() > 0) {
    size_t meta_n = static_cast<size_t>(footer.metaindex_handle().size());
    char* meta_buf = new char[meta_n + kBlockTrailerSize];
    Slice meta_contents;
    s = file->Read(footer.metaindex_handle().offset(), meta_n + kBlockTrailerSize,
                   &meta_contents, meta_buf);
    if (s.ok() && meta_contents.size() == meta_n + kBlockTrailerSize) {
      uint32_t meta_expected_crc = DecodeFixed32(meta_buf + meta_n + 1);
      uint32_t meta_actual_crc = crc32c::Value(meta_buf, meta_n);
      meta_actual_crc = crc32c::Extend(meta_actual_crc, meta_buf + meta_n, 1);
      if (crc32c::Mask(meta_actual_crc) == meta_expected_crc) {
        Block meta_block(meta_buf, meta_n, true);
        auto meta_iter = meta_block.NewIterator();
        std::string filter_key = "filter." + std::string(filter_policy->Name());
        meta_iter->Seek(Slice(filter_key));
        if (meta_iter->Valid() && meta_iter->key() == Slice(filter_key)) {
          Slice handle_slice = meta_iter->value();
          BlockHandle filter_handle;
          if (filter_handle.DecodeFrom(&handle_slice).ok()) {
            size_t filter_len = static_cast<size_t>(filter_handle.size());
            char* fbuf = new char[filter_len + kBlockTrailerSize];
            Slice fcontents;
            s = file->Read(filter_handle.offset(), filter_len + kBlockTrailerSize,
                           &fcontents, fbuf);
            if (s.ok() && fcontents.size() == filter_len + kBlockTrailerSize) {
              uint32_t f_expected_crc = DecodeFixed32(fbuf + filter_len + 1);
              uint32_t f_actual_crc = crc32c::Value(fbuf, filter_len);
              f_actual_crc = crc32c::Extend(f_actual_crc, fbuf + filter_len, 1);
              if (crc32c::Mask(f_actual_crc) == f_expected_crc) {
                filter_data.reset(fbuf);
                filter_reader = std::make_unique<FilterBlockReader>(
                    filter_policy, Slice(fbuf, filter_len));
              } else {
                delete[] fbuf;
              }
            } else {
              delete[] fbuf;
            }
          }
        }
      } else {
        delete[] meta_buf;
      }
    } else {
      delete[] meta_buf;
    }
  }

  table->reset(new Table(std::move(file), footer, std::move(index_block),
                         std::move(filter_data), std::move(filter_reader)));
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
  return std::make_unique<TwoLevelIterator>(this, nullptr, index_block_->NewIterator(cmp), cmp);
}

std::unique_ptr<Iterator> Table::NewIterator(std::shared_ptr<const Table> table,
                                             const Block::Comparator* cmp) {
  if (table == nullptr) return NewEmptyIterator();
  const Table* raw = table.get();
  return std::make_unique<TwoLevelIterator>(raw, std::move(table), raw->index_block()->NewIterator(cmp), cmp);
}

bool Table::InternalGet(const Slice& key, const Block::Comparator* cmp,
                        std::string* val, Status* s, Cache* /*cache*/) const {
  auto index_iter = index_block_->NewIterator(cmp);
  index_iter->Seek(key);
  if (!index_iter->Valid()) {
    return false;
  }

  Slice handle_slice = index_iter->value();
  BlockHandle handle;
  Status decode_status = handle.DecodeFrom(&handle_slice);
  if (!decode_status.ok()) {
    *s = decode_status;
    return true;
  }

  // Fast Bloom filter rejection check!
  if (filter_reader_ != nullptr) {
    // If internal key (>= 8 bytes), extract user key for Bloom filter test
    Slice user_key = (key.size() >= 8) ? Slice(key.data(), key.size() - 8) : key;
    if (!filter_reader_->KeyMayMatch(handle.offset(), user_key)) {
      // Bloom filter guarantees user key does NOT exist in this data block
      return false;
    }
  }

  // Load data block
  std::unique_ptr<Block> data_block;
  Status read_status = ReadBlock(handle, &data_block);
  if (!read_status.ok()) {
    *s = read_status;
    return true;
  }

  auto data_iter = data_block->NewIterator(cmp);
  data_iter->Seek(key);
  if (data_iter->Valid()) {
    Slice found_key = data_iter->key();
    bool match = false;
    if (key.size() >= 8 && found_key.size() >= 8) {
      Slice user_target(key.data(), key.size() - 8);
      Slice user_found(found_key.data(), found_key.size() - 8);
      match = (user_target == user_found);
    } else {
      match = (found_key == key);
    }

    if (match) {
      if (found_key.size() >= 8) {
        uint64_t tag = DecodeFixed64(found_key.data() + found_key.size() - 8);
        ValueType type = static_cast<ValueType>(tag & 0xff);
        if (type == kTypeValue) {
          *val = data_iter->value().ToString();
          *s = Status::OK();
          return true;
        } else if (type == kTypeDeletion) {
          *s = Status::NotFound(Slice(found_key.data(), found_key.size() - 8), "deleted");
          return true;
        }
      } else {
        *val = data_iter->value().ToString();
        *s = Status::OK();
        return true;
      }
    }
  }

  return false;
}

}  // namespace purpledb
