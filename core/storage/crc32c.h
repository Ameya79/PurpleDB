#pragma once

#include <cstddef>
#include <cstdint>

namespace purpledb {
namespace crc32c {

/**
 * @brief Computes the CRC-32C (Castagnoli) checksum of a buffer.
 *
 * CONCEPT FOR LEARNERS:
 * CRC-32C uses the Castagnoli polynomial 0x1EDC6F41 (reversed: 0x82F63B78).
 * Compared to IEEE 802.3 CRC-32 (used in zip/gzip/ethernet), Castagnoli has
 * significantly better error-detection properties for small payload sizes
 * (< 8 KiB), which matches database block sizes perfectly. It also has native
 * hardware instruction support on modern x86 (SSE4.2 CRC32) and ARM (ARMv8 CRC32).
 */

// Return the CRC-32C of data[0, n-1].
uint32_t Value(const char* data, size_t n);

// Extend an existing CRC-32C with additional data.
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

// Masked checksums prevent accidental self-referential collision when
// checksumming a block or record that already embeds a CRC-32C value.
static const uint32_t kMaskDelta = 0xa282ead8ul;

inline uint32_t Mask(uint32_t crc) {
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}

inline uint32_t Unmask(uint32_t masked_crc) {
  uint32_t rot = masked_crc - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

}  // namespace crc32c
}  // namespace purpledb
