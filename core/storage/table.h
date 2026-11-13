#pragma once

#include <cstdint>
#include <memory>
#include "core/storage/block.h"
#include "core/storage/env.h"
#include "core/storage/format.h"
#include "purpledb/iterator.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Table represents an opened SSTable file for reads.
 */
class Table {
 public:
  static Status Open(RandomAccessFile* file, uint64_t size,
                     std::unique_ptr<Table>* table);

  ~Table();

  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;

  // Returns a two-level iterator over all entries in the table.
  std::unique_ptr<Iterator> NewIterator(const Block::Comparator* cmp = nullptr) const;

  // Reads a block identified by handle and verifies its checksum.
  Status ReadBlock(const BlockHandle& handle, std::unique_ptr<Block>* result) const;

  const Block* index_block() const { return index_block_.get(); }
  const Footer& footer() const { return footer_; }

 private:
  Table(RandomAccessFile* file, Footer footer, std::unique_ptr<Block> index_block);

  RandomAccessFile* file_;
  Footer footer_;
  std::unique_ptr<Block> index_block_;
};

}  // namespace purpledb
