#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/storage/env.h"
#include "core/storage/table.h"
#include "core/storage/version_edit.h"
#include "core/storage/wal_writer.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"
#include "purpledb/write_batch.h"

namespace purpledb {

struct Options;
class VersionSet;

static const int kNumLevels = 7;

/**
 * @brief Version represents a point-in-time snapshot of SSTable files across all levels.
 */
class Version {
 public:
  explicit Version(VersionSet* vset);
  ~Version();

  Version(const Version&) = delete;
  Version& operator=(const Version&) = delete;

  void Ref() { ++refs_; }
  void Unref() {
    --refs_;
    if (refs_ == 0) {
      delete this;
    }
  }

  // Look up a key at a given snapshot sequence number across all L0 files (newest to oldest).
  // If found and value: sets *val, *s = Status::OK(), returns true.
  // If found and deleted (tombstone): sets *s = Status::NotFound, returns true.
  // If not found in any SSTable: returns false.
  bool Get(const Slice& user_key, SequenceNumber snapshot_seq, std::string* val, Status* s);

  // Return the list of files at a given level.
  const std::vector<FileMetaData>& files(int level) const {
    return files_[level];
  }

  // Number of files in Level 0.
  size_t NumLevelFiles(int level) const { return files_[level].size(); }

 private:
  friend class VersionSet;
  friend class VersionBuilder;

  VersionSet* vset_;
  Version* next_{this};
  Version* prev_{this};
  int refs_{0};

  std::vector<FileMetaData> files_[kNumLevels];
};

/**
 * @brief VersionSet manages the MANIFEST log, file number generation, and Version chain.
 */
class VersionSet {
 public:
  VersionSet(std::string dbname, Env* env, const Options* options = nullptr);
  ~VersionSet();

  VersionSet(const VersionSet&) = delete;
  VersionSet& operator=(const VersionSet&) = delete;

  // Apply edit to current version and write to MANIFEST.
  Status LogAndApply(VersionEdit* edit);

  // Recover state by reading CURRENT and replaying MANIFEST.
  Status Recover();

  // Current version. Caller does not take ownership.
  Version* current() const { return current_; }

  const Options* options() const { return options_; }

  uint64_t ManifestFileNumber() const { return manifest_file_number_; }
  uint64_t NextFileNumber() const { return next_file_number_; }
  uint64_t NewFileNumber() { return next_file_number_++; }

  uint64_t LastSequence() const { return last_sequence_; }
  void SetLastSequence(uint64_t s) {
    if (s > last_sequence_) {
      last_sequence_ = s;
    }
  }

  uint64_t LogNumber() const { return log_number_; }
  uint64_t PrevLogNumber() const { return prev_log_number_; }

  // Open an SSTable for reading.
  Status GetTable(uint64_t file_number, uint64_t file_size,
                  std::unique_ptr<Table>* table) const;

 private:
  class Builder;

  void AppendVersion(Version* v);

  std::string dbname_;
  Env* env_;
  const Options* options_{nullptr};
  uint64_t next_file_number_{1};
  uint64_t manifest_file_number_{0};
  uint64_t last_sequence_{0};
  uint64_t log_number_{0};
  uint64_t prev_log_number_{0};

  std::unique_ptr<WritableFile> descriptor_file_;
  std::unique_ptr<wal::WalWriter> descriptor_log_;

  Version dummy_versions_;  // Circular doubly linked list of versions
  Version* current_{nullptr};
};

}  // namespace purpledb
