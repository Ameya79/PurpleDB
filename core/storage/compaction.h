#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include "core/storage/version_edit.h"
#include "core/storage/version_set.h"
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief Compaction encapsulates the state and inputs of a single leveled compaction step.
 *
 * CONCEPT FOR LEARNERS:
 * Compaction is the garbage collector of an LSM database.
 * Flushed SSTables in Level 0 have overlapping key ranges.
 * Compaction picks Level L and Level L+1 files, merges them into a single sorted run
 * using K-way merge, discards shadowed older versions and dead tombstones, and
 * outputs non-overlapping sorted SSTables at Level L+1.
 */
class Compaction {
 public:
  Compaction(int level, Version* input_version);
  ~Compaction();

  int level() const { return level_; }

  // Inputs: 0 for level, 1 for level + 1
  const std::vector<FileMetaData>& inputs(int which) const {
    return inputs_[which];
  }

  // Returns true if no level >= (level_ + 2) contains user_key.
  // In this case, deletion tombstones for user_key can be safely dropped!
  bool IsBaseLevelForKey(const Slice& user_key) const;

  // Add input files to this compaction
  void AddInput(int which, const FileMetaData& f) {
    inputs_[which].push_back(f);
  }

  VersionEdit* edit() { return &edit_; }

 private:
  int level_;
  Version* input_version_;
  std::vector<FileMetaData> inputs_[2];
  VersionEdit edit_;
};

}  // namespace purpledb
