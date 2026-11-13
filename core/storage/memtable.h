#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
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
 * By sorting sequence numbers descending, lower_bound() immediately finds
 * the newest version of the key visible to a given sequence number.
 */
struct InternalKeyComparator {
  bool operator()(const std::string& a, const std::string& b) const;
};

class MemTable {
 public:
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

  // Approximate memory usage of the memtable in bytes.
  size_t ApproximateMemoryUsage() const { return memory_usage_; }

  size_t Count() const { return table_.size(); }

 private:
  std::map<std::string, std::string, InternalKeyComparator> table_;
  size_t memory_usage_{0};
};

}  // namespace purpledb
