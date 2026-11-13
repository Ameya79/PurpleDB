#pragma once

#include <cstdint>
#include <memory>
#include "core/storage/env.h"
#include "core/storage/wal_format.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {
namespace wal {

/**
 * @brief WalWriter appends records to a Write-Ahead Log file.
 *
 * CONCEPT FOR LEARNERS:
 * Records are segmented across 32 KiB blocks.
 * If a record is smaller than the block space left, it is written as a single
 * kFullType record. If it exceeds the block space, it is fragmented into
 * kFirstType, zero or more kMiddleType, and kLastType records.
 *
 * Each physical fragment has a 9-byte header with a masked CRC-32C checksum
 * covering the fragment type and payload bytes. This allows recovery to detect
 * partial (torn) disk writes at block boundaries.
 */
class WalWriter {
 public:
  // Create a writer that will append to "*dest".
  // "*dest" must remain valid for the lifetime of this WalWriter.
  explicit WalWriter(WritableFile* dest);

  // Create a writer that will append to "*dest", starting at a specified block offset.
  WalWriter(WritableFile* dest, uint64_t dest_length);

  ~WalWriter() = default;

  WalWriter(const WalWriter&) = delete;
  WalWriter& operator=(const WalWriter&) = delete;

  Status AddRecord(const Slice& slice);

 private:
  Status EmitPhysicalRecord(RecordType t, const char* ptr, size_t length);

  WritableFile* dest_;
  int block_offset_;  // Current offset within the 32 KiB block (0 .. kBlockSize)
};

}  // namespace wal
}  // namespace purpledb
