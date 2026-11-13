#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

using SequenceNumber = uint64_t;

enum ValueType : uint8_t {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1
};

/**
 * @brief WriteBatch holds a collection of atomic updates (Puts and Deletes).
 *
 * CONCEPT FOR LEARNERS:
 * Atomicity (the 'A' in ACID) ensures that a group of updates either all succeed
 * or none do. WriteBatch serializes multiple mutations into a single contiguous
 * binary buffer with an 8-byte sequence number and a 4-byte entry count.
 * Writing this batch to the WAL in a single AddRecord call ensures that even if
 * the machine loses power mid-write, partial updates are never applied.
 */
class WriteBatch {
 public:
  class Handler {
   public:
    virtual ~Handler() = default;
    virtual Status Put(const Slice& key, const Slice& value) = 0;
    virtual Status Delete(const Slice& key) = 0;
  };

  WriteBatch();
  ~WriteBatch() = default;

  // Add a put operation to the batch.
  void Put(const Slice& key, const Slice& value);

  // Add a delete operation to the batch.
  void Delete(const Slice& key);

  // Clear all updates in the batch.
  void Clear();

  // Return the byte size of the serialized batch.
  size_t ApproximateSize() const { return rep_.size(); }

  // Return the number of mutations in this batch.
  int Count() const;
  void SetCount(int n);

  // Sequence number accessor
  SequenceNumber Sequence() const;
  void SetSequence(SequenceNumber seq);

  // Iterate over all entries in the batch, invoking the handler for each.
  Status Iterate(Handler* handler) const;

  const std::string& Data() const { return rep_; }
  void SetData(const Slice& s) { rep_.assign(s.data(), s.size()); }

 private:
  std::string rep_;  // [SequenceNumber 8B][Count 4B][Entries...]
};

}  // namespace purpledb
