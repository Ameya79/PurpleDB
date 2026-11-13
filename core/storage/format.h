#pragma once

#include <cstdint>
#include <string>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief BlockHandle points to a physical block in an SSTable.
 *
 * CONCEPT FOR LEARNERS:
 * An SSTable file consists of data blocks, an index block, and a footer.
 * A BlockHandle is a two-tuple (offset, size) that specifies the byte range
 * of a block within the file. It is varint-encoded to save space.
 */
class BlockHandle {
 public:
  enum { kMaxEncodedLength = 10 + 10 };

  BlockHandle();

  uint64_t offset() const { return offset_; }
  void set_offset(uint64_t offset) { offset_ = offset; }

  uint64_t size() const { return size_; }
  void set_size(uint64_t size) { size_ = size; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  uint64_t offset_;
  uint64_t size_;
};

/**
 * @brief Footer is stored at the very end of every SSTable file.
 *
 * It contains BlockHandles to the metaindex block and the index block,
 * padded with null bytes, and terminates with a 64-bit magic number:
 * 0xdb477572706c6564ull ("purpled").
 * Fixed encoded length: 48 bytes.
 */
class Footer {
 public:
  enum {
    // 2 * BlockHandle::kMaxEncodedLength (40 bytes) + 8-byte magic
    kEncodedLength = 48
  };

  // Magic number: "purpled"
  static constexpr uint64_t kTableMagicNumber = 0xdb477572706c6564ull;

  Footer();

  const BlockHandle& metaindex_handle() const { return metaindex_handle_; }
  void set_metaindex_handle(const BlockHandle& h) { metaindex_handle_ = h; }

  const BlockHandle& index_handle() const { return index_handle_; }
  void set_index_handle(const BlockHandle& h) { index_handle_ = h; }

  void EncodeTo(std::string* dst) const;
  Status DecodeFrom(Slice* input);

 private:
  BlockHandle metaindex_handle_;
  BlockHandle index_handle_;
};

// Block trailer size: 1 byte compression type + 4 bytes masked CRC-32C
static const size_t kBlockTrailerSize = 5;

enum CompressionType {
  kNoCompression = 0x0,
  kSnappyCompression = 0x1,
};

}  // namespace purpledb

