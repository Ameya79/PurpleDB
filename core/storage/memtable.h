#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "core/storage/block.h"
#include "core/storage/skiplist.h"
#include "core/util/arena.h"
#include "purpledb/iterator.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"
#include "purpledb/write_batch.h"

namespace purpledb {

/**
 * @brief InternalKey comparator: user key ascending, sequence number descending.
 *
 * CONCEPT FOR LEARNERS:
 * InternalKey layout: [user_key bytes][8-byte tag].
 * The tag packs the sequence number in the high 56 bits and the ValueType
 * (kTypeValue or kTypeDeletion) in the low 8 bits:
 *   tag = (seq << 8) | type
 *
 * By sorting sequence numbers descending, lower_bound/Seek immediately finds
 * the newest version of the key visible to a given sequence number.
 */
struct InternalKeyComparator {
  int Compare(const Slice& a, const Slice& b) const;
  bool operator()(const std::string& a, const std::string& b) const;
};

class InternalKeyBlockComparator : public Block::Comparator {
 public:
  int Compare(const Slice& a, const Slice& b) const override {
    return icmp_.Compare(a, b);
  }
 private:
  InternalKeyComparator icmp_;
};

class MemTable {
 public:
  struct KeyComparator {
    const InternalKeyComparator comparator;
    int operator()(const char* a, const char* b) const;
  };
  using Table = SkipList<const char*, KeyComparator>;

  MemTable();
  ~MemTable() = default;

  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // Add an entry (put or delete) to the memtable.
  void Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value);

  // Look up a key at a given snapshot sequence number.
  // If found and value: sets *value, *s = Status::OK(), returns true.
  // If found and deleted (tombstone): sets *s = Status::NotFound, returns true.
  // If not found in this memtable: returns false.
  bool Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value, Status* s);

  // Return an iterator over the contents of the memtable.
  // The caller owns the returned iterator.
  std::unique_ptr<Iterator> NewIterator();

  // Approximate memory usage of the memtable in bytes.
  size_t ApproximateMemoryUsage() const { return arena_.MemoryUsage(); }

  size_t Count() const { return count_; }

 private:
  KeyComparator comparator_;
  Arena arena_;
  Table table_;
  size_t count_{0};
};

}  // namespace purpledb
