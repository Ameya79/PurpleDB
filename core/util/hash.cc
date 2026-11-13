#include "core/util/hash.h"
#include "core/util/coding.h"

namespace purpledb {

uint32_t Hash(const char* data, size_t n, uint32_t seed) {
  // Similar to MurmurHash2
  const uint32_t m = 0x5bd1e995;
  const int r = 24;
  uint32_t h = seed ^ static_cast<uint32_t>(n);

  while (n >= 4) {
    uint32_t k = DecodeFixed32(data);
    k *= m;
    k ^= k >> r;
    k *= m;
    h *= m;
    h ^= k;
    data += 4;
    n -= 4;
  }

  switch (n) {
    case 3:
      h ^= static_cast<uint8_t>(data[2]) << 16;
      [[fallthrough]];
    case 2:
      h ^= static_cast<uint8_t>(data[1]) << 8;
      [[fallthrough]];
    case 1:
      h ^= static_cast<uint8_t>(data[0]);
      h *= m;
      break;
    default:
      break;
  }

  h ^= h >> 13;
  h *= m;
  h ^= h >> 15;

  return h;
}

}  // namespace purpledb

