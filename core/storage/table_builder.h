#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "core/storage/block.h"
#include "core/storage/block_builder.h"
#include "core/storage/env.h"
#include "core/storage/filter_block.h"
#include "core/storage/format.h"
#include "purpledb/filter_policy.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

struct TableBuilderOptions {
  size_t block_size{4096};
  int block_restart_interval{16};
  const Block::Comparator* comparator{nullptr};
  const FilterPolicy* filter_policy{nullptr};
};

/**
 * @brief TableBuilder writes an immutable SSTable file.
 *
 * CONCEPT FOR LEARNERS:
 * An SSTable (Sorted String Table) is an append-only file containing:
 * 1. Data blocks: contiguous 4 KiB blocks of sorted key-value pairs.
 * 2. Filter block (Milestone M4): Bloom filter for fast negative lookups.
 * 3. Index block: one entry per data block, storing (last_key -> BlockHandle).
 * 4. Metaindex block: points to filter and metadata blocks.
 * 5. Footer: 48 bytes at EOF pointing to index & metaindex with magic number.
 */
class TableBuilder {
 public:
  TableBuilder(const TableBuilderOptions& options, WritableFile* file);
  ~TableBuilder();

  TableBuilder(const TableBuilder&) = delete;
  TableBuilder& operator=(const TableBuilder&) = delete;

  // Add key, value to the table being constructed.
  // REQUIRES: key is after any previously added key.
  // REQUIRES: Finish() or Abandon() has not been called.
  void Add(const Slice& key, const Slice& value);

  // Flush any buffered key/value pairs to file.
  void Flush();

  // Return non-ok status if some error occurred during building.
  Status status() const { return status_; }

  // Finish building the table. Flushes all blocks, writes index and footer.
  Status Finish();

  // Indicate that the contents of this builder should be abandoned.
  void Abandon();

  // Number of calls to Add() so far.
  uint64_t NumEntries() const { return num_entries_; }

  // Size of the file generated so far.
  uint64_t FileSize() const { return offset_; }

 private:
  void WriteBlock(BlockBuilder* block, BlockHandle* handle);

  TableBuilderOptions options_;
  WritableFile* file_;
  uint64_t offset_{0};
  Status status_;
  BlockBuilder data_block_;
  BlockBuilder index_block_;
  std::unique_ptr<FilterBlockBuilder> filter_block_;
  std::string last_key_;
  uint64_t num_entries_{0};
  bool closed_{false};
  bool pending_index_entry_{false};
  BlockHandle pending_handle_;
};

}  // namespace purpledb
