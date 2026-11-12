#include "core/storage/crc32c.h"

#include <array>

namespace purpledb {
namespace crc32c {

namespace {

constexpr uint32_t kCrc32cPoly = 0x82F63B78u;

constexpr auto MakeCrc32cTable() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int j = 0; j < 8; ++j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ kCrc32cPoly;
      } else {
        crc >>= 1;
      }
    }
    table[i] = crc;
  }
  return table;
}

const auto& GetCrc32cTable() {
  static constexpr auto table = MakeCrc32cTable();
  return table;
}

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
  const auto& table = GetCrc32cTable();
  const auto* p = reinterpret_cast<const uint8_t*>(data);
  uint32_t l = init_crc ^ 0xffffffffu;

  while (n--) {
    l = table[(l ^ *p++) & 0xff] ^ (l >> 8);
  }

  return l ^ 0xffffffffu;
}

uint32_t Value(const char* data, size_t n) {
  return Extend(0, data, n);
}

}  // namespace crc32c
}  // namespace purpledb
