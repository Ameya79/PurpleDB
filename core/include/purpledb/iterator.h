#pragma once

#include <memory>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Abstract iterator interface for sequential and seekable scans.
 *
 * CONCEPT FOR LEARNERS:
 * In LSM storage engines, data lives across multiple tiers: active MemTable,
 * immutable MemTable, and SSTable files at Level 0 to Level N.
 * The Iterator interface provides a uniform abstraction across all these tiers.
 * Higher-level components (like a MergingIterator) can blend multiple iterators
 * into a single sorted stream without caring where the data is stored.
 */
class Iterator {
 public:
  Iterator() = default;
  virtual ~Iterator() = default;

  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  // An iterator is either positioned at a key/value pair, or not valid.
  virtual bool Valid() const = 0;

  // Position at the first key in the source.
  virtual void SeekToFirst() = 0;

  // Position at the last key in the source.
  virtual void SeekToLast() = 0;

  // Position at the first key in the source that is at or past target.
  virtual void Seek(const Slice& target) = 0;

  // Moves to the next entry in the source.
  // REQUIRES: Valid()
  virtual void Next() = 0;

  // Moves to the previous entry in the source.
  // REQUIRES: Valid()
  virtual void Prev() = 0;

  // Return the key for the current entry.
  // REQUIRES: Valid()
  virtual Slice key() const = 0;

  // Return the value for the current entry.
  // REQUIRES: Valid()
  virtual Slice value() const = 0;

  // If an error has occurred, return it. Else return an ok status.
  virtual Status status() const = 0;
};

// Return an empty iterator (always invalid).
std::unique_ptr<Iterator> NewEmptyIterator();

// Return an error iterator.
std::unique_ptr<Iterator> NewErrorIterator(const Status& status);

}  // namespace purpledb

