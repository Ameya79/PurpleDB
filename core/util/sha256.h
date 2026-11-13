#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "core/storage/env.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Standard FIPS 180-4 cryptographic SHA-256 hashing.
 *
 * Used for content-addressed video identification (`video_id` = SHA-256 of media bitstream)
 * so file renames, copies, or alternate folder paths map to the identical
 * database entity without duplicate ingestion.
 */
class SHA256 {
 public:
  SHA256();
  void Update(const void* data, size_t len);
  std::string Final();

  // Convenience static methods
  static std::string Hash(const void* data, size_t len);
  static std::string HashString(const std::string& str);
  static Status HashFile(Env* env, const std::string& path, std::string* out_hex);

 private:
  void Transform(const uint8_t* chunk);

  uint32_t state_[8];
  uint64_t bit_len_{0};
  uint8_t buffer_[64];
  size_t buffer_len_{0};
};

}  // namespace purpledb
