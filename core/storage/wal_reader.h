#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "core/storage/env.h"
#include "core/storage/wal_format.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {
namespace wal {

class WalReader {
 public:
  // Interface for reporting dropped bytes or corruptions encountered during reading
  class Reporter {
   public:
    virtual ~Reporter() = default;
    virtual void Corruption(size_t bytes, const Status& status) = 0;
  };

  // Create a reader that will read from "*file".
  // "*file" must remain valid for the lifetime of this WalReader.
  WalReader(SequentialFile* file, Reporter* reporter, bool checksum,
            uint64_t initial_offset = 0);

  ~WalReader() = default;

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;

  // Read the next logical record into "*record".
  // Returns true on success, false on end of file or unrecoverable error.
  // If the record was fragmented, "*scratch" is used to reconstruct the full payload.
  bool ReadRecord(Slice* record, std::string* scratch);

  // Return the physical offset in the file up to which records have been read.
  uint64_t LastRecordOffset() const { return last_record_offset_; }

 private:
  // Internal record types returned by ReadPhysicalRecord
  enum {
    kEof = 100,
    kBadRecord = 101,
    kBadRecordLen = 102,
    kBadRecordChecksum = 103
  };

  unsigned int ReadPhysicalRecord(Slice* result);
  void ReportCorruption(size_t bytes, const char* reason);
  void ReportDrop(size_t bytes, const Status& status);

  SequentialFile* file_;
  Reporter* reporter_;
  bool checksum_;
  char backing_store_[kBlockSize];
  Slice buffer_;
  bool eof_;

  // Offset of the last record returned by ReadRecord
  uint64_t last_record_offset_;
  // Offset of the end of the last record read by ReadPhysicalRecord
  uint64_t end_of_buffer_offset_;
};

}  // namespace wal
}  // namespace purpledb
