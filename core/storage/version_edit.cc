#include "core/storage/version_edit.h"

#include "core/util/coding.h"

namespace purpledb {

namespace {

enum Tag {
  kComparator = 1,
  kLogNumber = 2,
  kNextFileNumber = 3,
  kLastSequence = 4,
  kDeletedFile = 6,
  kNewFile = 7,
  kPrevLogNumber = 9
};

}  // namespace

void VersionEdit::Clear() {
  comparator_.clear();
  log_number_ = 0;
  prev_log_number_ = 0;
  next_file_number_ = 0;
  last_sequence_ = 0;
  has_comparator_ = false;
  has_log_number_ = false;
  has_prev_log_number_ = false;
  has_next_file_number_ = false;
  has_last_sequence_ = false;
  deleted_files_.clear();
  new_files_.clear();
}

void VersionEdit::EncodeTo(std::string* dst) const {
  if (has_comparator_) {
    PutVarint32(dst, kComparator);
    PutLengthPrefixedSlice(dst, Slice(comparator_));
  }
  if (has_log_number_) {
    PutVarint32(dst, kLogNumber);
    PutVarint64(dst, log_number_);
  }
  if (has_prev_log_number_) {
    PutVarint32(dst, kPrevLogNumber);
    PutVarint64(dst, prev_log_number_);
  }
  if (has_next_file_number_) {
    PutVarint32(dst, kNextFileNumber);
    PutVarint64(dst, next_file_number_);
  }
  if (has_last_sequence_) {
    PutVarint32(dst, kLastSequence);
    PutVarint64(dst, last_sequence_);
  }

  for (const auto& df : deleted_files_) {
    PutVarint32(dst, kDeletedFile);
    PutVarint32(dst, df.first);   // level
    PutVarint64(dst, df.second);  // file number
  }

  for (const auto& nf : new_files_) {
    PutVarint32(dst, kNewFile);
    PutVarint32(dst, nf.first);  // level
    PutVarint64(dst, nf.second.number);
    PutVarint64(dst, nf.second.file_size);
    PutLengthPrefixedSlice(dst, Slice(nf.second.smallest));
    PutLengthPrefixedSlice(dst, Slice(nf.second.largest));
  }
}

Status VersionEdit::DecodeFrom(const Slice& src) {
  Clear();
  Slice input = src;

  while (!input.empty()) {
    uint32_t tag;
    if (!GetVarint32(&input, &tag)) {
      return Status::Corruption("VersionEdit: cannot read tag");
    }

    switch (tag) {
      case kComparator: {
        Slice cmp;
        if (!GetLengthPrefixedSlice(&input, &cmp)) {
          return Status::Corruption("VersionEdit: bad comparator");
        }
        comparator_ = cmp.ToString();
        has_comparator_ = true;
        break;
      }
      case kLogNumber: {
        if (!GetVarint64(&input, &log_number_)) {
          return Status::Corruption("VersionEdit: bad log number");
        }
        has_log_number_ = true;
        break;
      }
      case kPrevLogNumber: {
        if (!GetVarint64(&input, &prev_log_number_)) {
          return Status::Corruption("VersionEdit: bad prev log number");
        }
        has_prev_log_number_ = true;
        break;
      }
      case kNextFileNumber: {
        if (!GetVarint64(&input, &next_file_number_)) {
          return Status::Corruption("VersionEdit: bad next file number");
        }
        has_next_file_number_ = true;
        break;
      }
      case kLastSequence: {
        if (!GetVarint64(&input, &last_sequence_)) {
          return Status::Corruption("VersionEdit: bad last sequence");
        }
        has_last_sequence_ = true;
        break;
      }
      case kDeletedFile: {
        uint32_t level;
        uint64_t file;
        if (!GetVarint32(&input, &level) || !GetVarint64(&input, &file)) {
          return Status::Corruption("VersionEdit: bad deleted file");
        }
        deleted_files_.insert({static_cast<int>(level), file});
        break;
      }
      case kNewFile: {
        uint32_t level;
        uint64_t number;
        uint64_t file_size;
        Slice smallest;
        Slice largest;
        if (!GetVarint32(&input, &level) ||
            !GetVarint64(&input, &number) ||
            !GetVarint64(&input, &file_size) ||
            !GetLengthPrefixedSlice(&input, &smallest) ||
            !GetLengthPrefixedSlice(&input, &largest)) {
          return Status::Corruption("VersionEdit: bad new file");
        }
        AddFile(static_cast<int>(level), number, file_size, smallest, largest);
        break;
      }
      default:
        return Status::Corruption("VersionEdit: unknown tag");
    }
  }

  return Status::OK();
}

}  // namespace purpledb

