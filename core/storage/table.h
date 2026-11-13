#pragma once

#include <cstdint>
#include <memory>
#include "core/storage/block.h"
#include "core/storage/cache.h"
#include "core/storage/env.h"
#include "core/storage/filter_block.h"
#include "core/storage/format.h"
#include "purpledb/filter_policy.h"
#include "purpledb/iterator.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Table represents an opened SSTable file for reads.
 */
class Table {
 public:
  static Status Open(std::unique_ptr<RandomAccessFile> file, uint64_t size,
                     std::unique_ptr<Table>* table,
                     const FilterPolicy* filter_policy = nullptr);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Returns a two-level iterator over all entries in the table.
  std::unique_ptr<Iterator> NewIterator(const Block::Comparator* cmp = nullptr) const;

  // Reads a block identified by handle and verifies its checksum.
  Status ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* result) const;

  // Fast point lookup using Bloom filter and index block.
  // Returns true if key is found (or tombstone found), false if key not present.
  bool InternalGet(const Slice& key, const Block::Comparator* cmp,
                   std::string* val, Status* s, Cache* cache = nullptr) const;

  const Block* index_block() const { return index_block_.get(); }
  const Footer& footer() const { return footer_; }
  const FilterBlockReader* filter_reader() const { return filter_reader_.get(); }

 private:
  Table(std::unique_ptr<RandomAccessFile> file, Footer footer,
        std::unique_ptr<Block> index_block,
        std::unique_ptr<char[]> filter_data,
        std::unique_ptr<FilterBlockReader> filter_reader);

  std::unique_ptr<RandomAccessFile> file_;
  Footer footer_;
  std::unique_ptr<Block> index_block_;
  std::unique_ptr<char[]> filter_data_;
  std::unique_ptr<FilterBlockReader> filter_reader_;
};

}  // namespace purpledb
