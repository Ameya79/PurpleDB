#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "core/storage/crc32c.h"
#include "core/util/coding.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Persistent moment metadata record.
 *
 * Stored in LSM engine with key prefix "info:" + key
 * where key is <video_id>:m_<moment_id>.
 */
struct MomentMetadata {
  static constexpr uint16_t kVersion = 1;

  uint16_t version{kVersion};
  std::string video_id;
  uint32_t moment_id{0};
  double start_sec{0.0};
  double end_sec{0.0};
  std::string thumbnail_path;
  std::string caption;

  std::string Serialize() const {
    std::string dst;
    PutFixed16(&dst, version);
    PutLengthPrefixedSlice(&dst, Slice(video_id));
    PutFixed32(&dst, moment_id);

    uint64_t start_bits = 0;
    std::memcpy(&start_bits, &start_sec, sizeof(double));
    PutFixed64(&dst, start_bits);

    uint64_t end_bits = 0;
    std::memcpy(&end_bits, &end_sec, sizeof(double));
    PutFixed64(&dst, end_bits);

    PutLengthPrefixedSlice(&dst, Slice(thumbnail_path));
    PutLengthPrefixedSlice(&dst, Slice(caption));

    uint32_t crc = crc32c::Mask(crc32c::Value(dst.data(), dst.size()));
    PutFixed32(&dst, crc);
    return dst;
  }

  static Status Deserialize(Slice src, MomentMetadata* out) {
    if (src.size() < 2 + 1 + 4 + 8 + 8 + 1 + 1 + 4) {
      return Status::Corruption("MomentMetadata payload too short");
    }

    const char* data = src.data();
    size_t size = src.size();

    // Verify CRC
    uint32_t expected_crc = DecodeFixed32(data + size - 4);
    uint32_t actual_crc = crc32c::Mask(crc32c::Value(data, size - 4));
    if (expected_crc != actual_crc) {
      return Status::Corruption("MomentMetadata CRC mismatch");
    }

    Slice input(data, size - 4);
    if (input.size() < 2) return Status::Corruption("Truncated version in MomentMetadata");
    out->version = DecodeFixed16(input.data());
    input.remove_prefix(2);

    Slice vid_slice;
    if (!GetLengthPrefixedSlice(&input, &vid_slice)) {
      return Status::Corruption("Truncated video_id in MomentMetadata");
    }
    out->video_id = vid_slice.ToString();

    if (input.size() < 4 + 8 + 8) {
      return Status::Corruption("Truncated numeric fields in MomentMetadata");
    }
    out->moment_id = DecodeFixed32(input.data());
    input.remove_prefix(4);

    uint64_t start_bits = DecodeFixed64(input.data());
    std::memcpy(&out->start_sec, &start_bits, sizeof(double));
    input.remove_prefix(8);

    uint64_t end_bits = DecodeFixed64(input.data());
    std::memcpy(&out->end_sec, &end_bits, sizeof(double));
    input.remove_prefix(8);

    Slice thumb_slice;
    if (!GetLengthPrefixedSlice(&input, &thumb_slice)) {
      return Status::Corruption("Truncated thumbnail_path in MomentMetadata");
    }
    out->thumbnail_path = thumb_slice.ToString();

    Slice cap_slice;
    if (!GetLengthPrefixedSlice(&input, &cap_slice)) {
      return Status::Corruption("Truncated caption in MomentMetadata");
    }
    out->caption = cap_slice.ToString();

    return Status::OK();
  }
};

}  // namespace purpledb
