#pragma once

#include <cstddef>
#include <cstdint>

namespace purpledb {
namespace wal {

enum RecordType : uint8_t {
  // Zero is reserved for preallocated or padded space
  kZeroType = 0,
  kFullType = 1,
  // For fragments:
  kFirstType = 2,
  kMiddleType = 3,
  kLastType = 4
};

static constexpr size_t kBlockSize = 32768;  // 32 KiB blocks

// Record Header:
//   crc32c: 4 bytes (uint32, little-endian, masked)
//   length: 4 bytes (uint32, little-endian)
//   type:   1 byte  (uint8)
// Total header size = 9 bytes
static constexpr size_t kHeaderSize = 4 + 4 + 1;

// Maximum payload size that can fit into a single block
static constexpr size_t kMaxRecordSizePerBlock = kBlockSize - kHeaderSize;

}  // namespace wal
}  // namespace purpledb
