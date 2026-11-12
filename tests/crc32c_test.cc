#include "core/storage/crc32c.h"
#include "util/test_harness.h"

#include <vector>

namespace purpledb {
namespace crc32c {

TEST(Crc32cTest, StandardVectors) {
  // Empty buffer
  ASSERT_EQ(Value("", 0), 0u);

  // RFC 3720 Section 8.4: "123456789" -> 0xe3069283
  const char test_digits[] = "123456789";
  ASSERT_EQ(Value(test_digits, 9), 0xe3069283u);

  // 32 zero bytes -> 0x8a9136aa
  std::vector<char> zeroes(32, 0);
  ASSERT_EQ(Value(zeroes.data(), zeroes.size()), 0x8a9136aau);

  // 32 0xff bytes -> 0x62a8ab43
  std::vector<char> ones(32, static_cast<char>(0xff));
  ASSERT_EQ(Value(ones.data(), ones.size()), 0x62a8ab43u);
}

TEST(Crc32cTest, IncrementalExtend) {
  const char test_str[] = "The quick brown fox jumps over the lazy dog";
  const size_t len = sizeof(test_str) - 1;

  uint32_t full_crc = Value(test_str, len);

  // Split into chunks: "The quick brown ", "fox jumps ", "over the lazy dog"
  uint32_t chunk_crc = Value(test_str, 16);
  chunk_crc = Extend(chunk_crc, test_str + 16, 10);
  chunk_crc = Extend(chunk_crc, test_str + 26, len - 26);

  ASSERT_EQ(chunk_crc, full_crc);
}

TEST(Crc32cTest, MaskAndUnmask) {
  uint32_t crc = 0xe3069283u;
  uint32_t masked = Mask(crc);
  ASSERT_NE(masked, crc);
  ASSERT_EQ(Unmask(masked), crc);

  // Test across various bit patterns
  uint32_t test_values[] = {0u, 1u, 0xffffffffu, 0x12345678u, 0x8a9136aau};
  for (uint32_t v : test_values) {
    ASSERT_EQ(Unmask(Mask(v)), v);
    ASSERT_NE(Mask(v), v);
  }
}

}  // namespace crc32c
}  // namespace purpledb
