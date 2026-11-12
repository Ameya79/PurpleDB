#include "util/coding.h"
#include "util/test_harness.h"

#include <random>
#include <vector>

namespace purpledb {

TEST(CodingTest, Fixed32RoundTrip) {
  uint32_t test_values[] = {
      0u,
      1u,
      127u,
      128u,
      16383u,
      16384u,
      0x12345678u,
      0x7fffffffu,
      0x80000000u,
      0xffffffffu,
  };

  std::string s;
  for (uint32_t v : test_values) {
    PutFixed32(&s, v);
  }

  ASSERT_EQ(s.size(), sizeof(test_values));

  const char* p = s.data();
  for (uint32_t expected : test_values) {
    uint32_t actual = DecodeFixed32(p);
    ASSERT_EQ(actual, expected);
    p += sizeof(uint32_t);
  }
}

TEST(CodingTest, Fixed64RoundTrip) {
  uint64_t test_values[] = {
      0ull,
      1ull,
      127ull,
      128ull,
      0x123456789abcdef0ull,
      0x7fffffffffffffffull,
      0x8000000000000000ull,
      0xffffffffffffffffull,
  };

  std::string s;
  for (uint64_t v : test_values) {
    PutFixed64(&s, v);
  }

  ASSERT_EQ(s.size(), sizeof(test_values));

  const char* p = s.data();
  for (uint64_t expected : test_values) {
    uint64_t actual = DecodeFixed64(p);
    ASSERT_EQ(actual, expected);
    p += sizeof(uint64_t);
  }
}

TEST(CodingTest, Varint32Boundaries) {
  // Check exact byte lengths across powers of 2
  ASSERT_EQ(VarintLength(0u), 1);
  ASSERT_EQ(VarintLength(127u), 1);
  ASSERT_EQ(VarintLength(128u), 2);
  ASSERT_EQ(VarintLength(16383u), 2);
  ASSERT_EQ(VarintLength(16384u), 3);
  ASSERT_EQ(VarintLength(2097151u), 3);
  ASSERT_EQ(VarintLength(2097152u), 4);
  ASSERT_EQ(VarintLength(268435455u), 4);
  ASSERT_EQ(VarintLength(268435456u), 5);
  ASSERT_EQ(VarintLength(0xffffffffu), 5);

  uint32_t boundary_values[] = {
      0u, 1u, 127u, 128u, 16383u, 16384u, 2097151u, 2097152u,
      268435455u, 268435456u, 0x7fffffffu, 0x80000000u, 0xffffffffu
  };

  for (uint32_t expected : boundary_values) {
    char buf[5];
    char* end = EncodeVarint32(buf, expected);
    ASSERT_EQ(end - buf, VarintLength(expected));

    uint32_t actual = 0;
    const char* next = GetVarint32Ptr(buf, end, &actual);
    ASSERT_EQ(next, end);
    ASSERT_EQ(actual, expected);
  }
}

TEST(CodingTest, Varint64Boundaries) {
  uint64_t boundary_values[] = {
      0ull,
      1ull,
      127ull,
      128ull,
      16383ull,
      16384ull,
      (1ull << 21) - 1,
      (1ull << 21),
      (1ull << 28) - 1,
      (1ull << 28),
      (1ull << 35) - 1,
      (1ull << 35),
      (1ull << 42) - 1,
      (1ull << 42),
      (1ull << 49) - 1,
      (1ull << 49),
      (1ull << 56) - 1,
      (1ull << 56),
      (1ull << 63) - 1,
      (1ull << 63),
      0xffffffffffffffffull,
  };

  for (uint64_t expected : boundary_values) {
    char buf[10];
    char* end = EncodeVarint64(buf, expected);
    ASSERT_EQ(end - buf, VarintLength(expected));

    uint64_t actual = 0;
    const char* next = GetVarint64Ptr(buf, end, &actual);
    ASSERT_EQ(next, end);
    ASSERT_EQ(actual, expected);
  }
}

TEST(CodingTest, VarintStreamSequential) {
  std::string s;
  std::vector<uint32_t> values;
  for (uint32_t i = 0; i < 1000; ++i) {
    uint32_t val = (i * 104729u) ^ 0x55555555u;
    values.push_back(val);
    PutVarint32(&s, val);
  }

  Slice slice(s);
  for (uint32_t expected : values) {
    uint32_t actual = 0;
    bool ok = GetVarint32(&slice, &actual);
    ASSERT_TRUE(ok);
    ASSERT_EQ(actual, expected);
  }
  ASSERT_TRUE(slice.empty());
}

TEST(CodingTest, TruncatedVarintSafety) {
  uint32_t val = 100000;  // Multi-byte varint
  std::string s;
  PutVarint32(&s, val);
  ASSERT_GT(s.size(), 1u);

  // Truncate to less than required bytes
  for (size_t len = 0; len < s.size() - 1; ++len) {
    uint32_t decoded = 0;
    const char* p = GetVarint32Ptr(s.data(), s.data() + len, &decoded);
    ASSERT_TRUE(p == nullptr);

    Slice slice(s.data(), len);
    ASSERT_FALSE(GetVarint32(&slice, &decoded));
  }
}

TEST(CodingTest, LengthPrefixedSlice) {
  std::string s;
  PutLengthPrefixedSlice(&s, Slice(""));
  PutLengthPrefixedSlice(&s, Slice("hello"));
  PutLengthPrefixedSlice(&s, Slice("purpledb_database_engine"));

  Slice input(s);
  Slice res;

  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &res));
  ASSERT_EQ(res.ToString(), "");

  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &res));
  ASSERT_EQ(res.ToString(), "hello");

  ASSERT_TRUE(GetLengthPrefixedSlice(&input, &res));
  ASSERT_EQ(res.ToString(), "purpledb_database_engine");

  ASSERT_TRUE(input.empty());
  ASSERT_FALSE(GetLengthPrefixedSlice(&input, &res));
}

TEST(CodingTest, RandomFuzzRoundTrip) {
  std::mt19937_64 rng(42);
  std::string stream;
  std::vector<uint64_t> expected;

  for (int i = 0; i < 5000; ++i) {
    uint64_t v = rng();
    expected.push_back(v);
    PutVarint64(&stream, v);
  }

  Slice input(stream);
  for (uint64_t exp : expected) {
    uint64_t act = 0;
    ASSERT_TRUE(GetVarint64(&input, &act));
    ASSERT_EQ(act, exp);
  }
  ASSERT_TRUE(input.empty());
}

}  // namespace purpledb
