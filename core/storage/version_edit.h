#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

struct FileMetaData {
  uint64_t number{0};
  uint64_t file_size{0};
  std::string smallest;  // Smallest internal key
  std::string largest;   // Largest internal key
};

/**
 * @brief VersionEdit records incremental metadata changes to the LSM tree.
 *
 * CONCEPT FOR LEARNERS:
 * Instead of rewriting the entire database manifest on every write/flush,
 * LSM engines log incremental diffs called VersionEdits.
 * A VersionEdit records files added, files deleted, the active WAL file number,
 * and the sequence number high water mark.
 */
class VersionEdit {
 public:
  VersionEdit() = default;
  ~VersionEdit() = default;

  void Clear();

  void SetComparatorName(const Slice& name) {
    has_comparator_ = true;
    comparator_ = name.ToString();
  }
  void SetLogNumber(uint64_t num) {
    has_log_number_ = true;
    log_number_ = num;
  }
  void SetPrevLogNumber(uint64_t num) {
    has_prev_log_number_ = true;
    prev_log_number_ = num;
  }
  void SetNextFileNumber(uint64_t num) {
    has_next_file_number_ = true;
    next_file_number_ = num;
  }
  void SetLastSequence(uint64_t seq) {
    has_last_sequence_ = true;
    last_sequence_ = seq;
  }

  void AddFile(int level, uint64_t file, uint64_t file_size,
               const Slice& smallest, const Slice& largest) {
    FileMetaData f;
    f.number = file;
    f.file_size = file_size;
    f.smallest = smallest.ToString();
    f.largest = largest.ToString();
    new_files_.push_back({level, f});
  }

  void RemoveFile(int level, uint64_t file) {
    deleted_files_.insert({level, file});
  }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(const Slice& src);

  bool has_comparator() const { return has_comparator_; }
  const std::string& comparator() const { return comparator_; }

  bool has_log_number() const { return has_log_number_; }
  uint64_t log_number() const { return log_number_; }

  bool has_prev_log_number() const { return has_prev_log_number_; }
  uint64_t prev_log_number() const { return prev_log_number_; }

  bool has_next_file_number() const { return has_next_file_number_; }
  uint64_t next_file_number() const { return next_file_number_; }

  bool has_last_sequence() const { return has_last_sequence_; }
  uint64_t last_sequence() const { return last_sequence_; }

  const std::vector<std::pair<int, FileMetaData>>& new_files() const {
    return new_files_;
  }

  const std::set<std::pair<int, uint64_t>>& deleted_files() const {
    return deleted_files_;
  }

 private:
  std::string comparator_;
  uint64_t log_number_{0};
  uint64_t prev_log_number_{0};
  uint64_t next_file_number_{0};
  uint64_t last_sequence_{0};
  bool has_comparator_{false};
  bool has_log_number_{false};
  bool has_prev_log_number_{false};
  bool has_next_file_number_{false};
  bool has_last_sequence_{false};

  std::set<std::pair<int, uint64_t>> deleted_files_;
  std::vector<std::pair<int, FileMetaData>> new_files_;
};

}  // namespace purpledb
