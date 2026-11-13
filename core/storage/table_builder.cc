#include "core/storage/table_builder.h"

#include <cassert>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {

TableBuilder::TableBuilder(const TableBuilderOptions& options, WritableFile* file)
    : options_(options),
      file_(file),
      offset_(0),
      status_(Status::OK()),
      data_block_(options.block_restart_interval),
      index_block_(1),  // Index entries are not frequent, restart interval 1
      filter_block_(options.filter_policy != nullptr
                        ? std::make_unique<FilterBlockBuilder>(options.filter_policy)
                        : nullptr),
      num_entries_(0),
      closed_(false),
      pending_index_entry_(false) {
  if (filter_block_ != nullptr) {
    filter_block_->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(closed_);
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  assert(!closed_);
  if (!status_.ok()) return;

  if (num_entries_ > 0) {
    if (options_.comparator != nullptr) {
      assert(options_.comparator->Compare(key, Slice(last_key_)) > 0);
    } else {
      assert(key.compare(Slice(last_key_)) > 0);
    }
  }

  if (pending_index_entry_) {
    assert(data_block_.empty());
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  if (data_block_.empty()) {
    if (filter_block_ != nullptr) {
      filter_block_->StartBlock(offset_);
    }
  }

  last_key_.assign(key.data(), key.size());
  num_entries_++;
  data_block_.Add(key, value);
  if (filter_block_ != nullptr) {
    // If internal key (>= 8 bytes tag trailer), extract user key for Bloom filtering
    Slice user_key = (key.size() >= 8) ? Slice(key.data(), key.size() - 8) : key;
    filter_block_->AddKey(user_key);
  }

  const size_t estimated_block_size = data_block_.CurrentSizeEstimate();
  if (estimated_block_size >= options_.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  assert(!closed_);
  if (!status_.ok()) return;
  if (data_block_.empty()) return;

  assert(!pending_index_entry_);
  WriteBlock(&data_block_, &pending_handle_);
  if (status_.ok()) {
    pending_index_entry_ = true;
    status_ = file_->Flush();
  }
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  Slice raw = block->Finish();
  handle->set_offset(offset_);
  handle->set_size(raw.size());
  status_ = file_->Append(raw);
  if (!status_.ok()) return;

  char trailer[kBlockTrailerSize];
  trailer[0] = kNoCompression;
  uint32_t crc = crc32c::Value(raw.data(), raw.size());
  crc = crc32c::Extend(crc, trailer, 1);
  EncodeFixed32(trailer + 1, crc32c::Mask(crc));

  status_ = file_->Append(Slice(trailer, kBlockTrailerSize));
  if (status_.ok()) {
    offset_ += raw.size() + kBlockTrailerSize;
  }
  block->Reset();
}

Status TableBuilder::Finish() {
  assert(!closed_);

  // Flush remaining data
  Flush();
  closed_ = true;

  if (pending_index_entry_) {
    std::string handle_encoding;
    pending_handle_.EncodeTo(&handle_encoding);
    index_block_.Add(Slice(last_key_), Slice(handle_encoding));
    pending_index_entry_ = false;
  }

  // Write filter block if configured
  BlockHandle filter_handle;
  if (status_.ok() && filter_block_ != nullptr) {
    Slice filter_contents = filter_block_->Finish();
    filter_handle.set_offset(offset_);
    filter_handle.set_size(filter_contents.size());
    status_ = file_->Append(filter_contents);
    if (status_.ok()) {
      char trailer[kBlockTrailerSize];
      trailer[0] = kNoCompression;
      uint32_t crc = crc32c::Value(filter_contents.data(), filter_contents.size());
      crc = crc32c::Extend(crc, trailer, 1);
      EncodeFixed32(trailer + 1, crc32c::Mask(crc));
      status_ = file_->Append(Slice(trailer, kBlockTrailerSize));
      if (status_.ok()) {
        offset_ += filter_contents.size() + kBlockTrailerSize;
      }
    }
  }

  // Write metaindex block
  BlockHandle metaindex_handle;
  if (status_.ok()) {
    BlockBuilder metaindex_block(1);
    if (filter_block_ != nullptr) {
      std::string filter_key = "filter." + std::string(options_.filter_policy->Name());
      std::string handle_encoding;
      filter_handle.EncodeTo(&handle_encoding);
      metaindex_block.Add(Slice(filter_key), Slice(handle_encoding));
    }
    WriteBlock(&metaindex_block, &metaindex_handle);
  }

  // Write index block
  BlockHandle index_handle;
  if (status_.ok()) {
    WriteBlock(&index_block_, &index_handle);
  }

  // Write footer
  if (status_.ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_handle);
    footer.set_index_handle(index_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    status_ = file_->Append(Slice(footer_encoding));
    if (status_.ok()) {
      offset_ += footer_encoding.size();
    }
  }

  if (status_.ok()) {
    status_ = file_->Sync();
  }

  return status_;
}

void TableBuilder::Abandon() {
  assert(!closed_);
  closed_ = true;
}

}  // namespace purpledb
