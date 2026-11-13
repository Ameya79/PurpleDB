#include "core/storage/wal_writer.h"

#include <cassert>
#include <cstring>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {
namespace wal {

WalWriter::WalWriter(WritableFile* dest)
    : dest_(dest), block_offset_(0) {}

WalWriter::WalWriter(WritableFile* dest, uint64_t dest_length)
    : dest_(dest), block_offset_(static_cast<int>(dest_length % kBlockSize)) {}

Status WalWriter::AddRecord(const Slice& slice) {
  const char* ptr = slice.data();
  size_t left = slice.size();

  // Fragment the record if it cannot fit in the remainder of the current block
  Status s;
  bool is_begin = true;
  do {
    const int leftover = static_cast<int>(kBlockSize - block_offset_);
    assert(leftover >= 0);
    if (leftover < static_cast<int>(kHeaderSize)) {
      // Pad trailer with zeroes
      if (leftover > 0) {
        static const char zeroes[kHeaderSize] = {0};
        s = dest_->Append(Slice(zeroes, leftover));
        if (!s.ok()) {
          return s;
        }
      }
      block_offset_ = 0;
    }

    // Invariant: we have at least kHeaderSize space left in block
    const size_t avail = kBlockSize - block_offset_ - kHeaderSize;
    const size_t fragment_length = (left <= avail) ? left : avail;

    RecordType type;
    const bool is_end = (left == fragment_length);
    if (is_begin && is_end) {
      type = kFullType;
    } else if (is_begin) {
      type = kFirstType;
    } else if (is_end) {
      type = kLastType;
    } else {
      type = kMiddleType;
    }

    s = EmitPhysicalRecord(type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    is_begin = false;
  } while (s.ok() && left > 0);

  return s;
}

Status WalWriter::EmitPhysicalRecord(RecordType t, const char* ptr, size_t length) {
  assert(length <= 0xffffffffu);
  assert(block_offset_ + kHeaderSize + length <= kBlockSize);

  // Format header:
  // [crc32c u32][length u32][type u8]
  char buf[kHeaderSize];
  EncodeFixed32(buf + 4, static_cast<uint32_t>(length));
  buf[8] = static_cast<char>(t);

  // Compute CRC32C of type + payload
  uint32_t crc = crc32c::Extend(0, &buf[8], 1);
  crc = crc32c::Extend(crc, ptr, length);
  crc = crc32c::Mask(crc);
  EncodeFixed32(buf, crc);

  // Write header and payload
  Status s = dest_->Append(Slice(buf, kHeaderSize));
  if (s.ok()) {
    s = dest_->Append(Slice(ptr, length));
    if (s.ok()) {
      s = dest_->Flush();
    }
  }
  block_offset_ += static_cast<int>(kHeaderSize + length);
  return s;
}

}  // namespace wal
}  // namespace purpledb
