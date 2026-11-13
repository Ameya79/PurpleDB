#include "purpledb/filter_policy.h"
#include "core/storage/filter_block.h"
#include "util/test_harness.h"

#include <cstdio>
#include <string>
#include <vector>

namespace purpledb {

TEST(FilterPolicyTest, Empty) {
  auto policy = NewBloomFilterPolicy(10);
  std::string filter;
  policy->CreateFilter(nullptr, 0, &filter);
  ASSERT_GE(filter.size(), 2u);
  // Empty filter should not match non-empty key
  ASSERT_FALSE(policy->KeyMayMatch(Slice("hello"), Slice(filter)));
}

TEST(FilterPolicyTest, SingleKey) {
  auto policy = NewBloomFilterPolicy(10);
  std::string filter;
  Slice key("purpledb_key");
  policy->CreateFilter(&key, 1, &filter);

  ASSERT_TRUE(policy->KeyMayMatch(Slice("purpledb_key"), Slice(filter)));
  ASSERT_FALSE(policy->KeyMayMatch(Slice("different_key"), Slice(filter)));
  ASSERT_FALSE(policy->KeyMayMatch(Slice("purpledb"), Slice(filter)));
}

TEST(FilterPolicyTest, FalsePositiveRate) {
  auto policy = NewBloomFilterPolicy(10);
  const int kNumKeys = 10000;
  std::vector<std::string> keys(kNumKeys);
  std::vector<Slice> key_slices(kNumKeys);

  for (int i = 0; i < kNumKeys; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%07d", i);
    keys[i] = buf;
    key_slices[i] = Slice(keys[i]);
  }

  std::string filter;
  policy->CreateFilter(key_slices.data(), kNumKeys, &filter);

  // 1. Verify 0% False Negatives: All existing keys must match
  for (int i = 0; i < kNumKeys; ++i) {
    ASSERT_TRUE(policy->KeyMayMatch(key_slices[i], Slice(filter)));
  }

  // 2. Measure False Positive Rate: Test keys not in the set
  int false_positives = 0;
  for (int i = 0; i < kNumKeys; ++i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "missing_%07d", i);
    if (policy->KeyMayMatch(Slice(buf), Slice(filter))) {
      ++false_positives;
    }
  }

  double fp_rate = static_cast<double>(false_positives) / kNumKeys;
  // With 10 bits per key, expected FP rate is ~1%. Must be strictly under 2%.
  ASSERT_LT(fp_rate, 0.02);
  ASSERT_GT(fp_rate, 0.001);  // Should not be zero for 10,000 keys
}

TEST(FilterBlockTest, MultiBlockOffsets) {
  auto policy = NewBloomFilterPolicy(10);
  FilterBlockBuilder builder(policy.get());

  // Block 1 at offset 0
  builder.StartBlock(0);
  builder.AddKey(Slice("cat"));
  builder.AddKey(Slice("dog"));

  // Block 2 at offset 2000 (still same filter chunk, base is 2048)
  builder.StartBlock(2000);
  builder.AddKey(Slice("elephant"));

  // Block 3 at offset 4100 (jumps to filter chunk 2)
  builder.StartBlock(4100);
  builder.AddKey(Slice("whale"));

  // Block 4 at offset 8200 (jumps to filter chunk 4)
  builder.StartBlock(8200);
  builder.AddKey(Slice("zebra"));

  Slice filter_contents = builder.Finish();
  FilterBlockReader reader(policy.get(), filter_contents);

  // Offset 0 and 2000 belong to chunk 0
  ASSERT_TRUE(reader.KeyMayMatch(0, Slice("cat")));
  ASSERT_TRUE(reader.KeyMayMatch(0, Slice("dog")));
  ASSERT_TRUE(reader.KeyMayMatch(0, Slice("elephant")));
  ASSERT_TRUE(reader.KeyMayMatch(2000, Slice("cat")));
  ASSERT_FALSE(reader.KeyMayMatch(0, Slice("whale")));
  ASSERT_FALSE(reader.KeyMayMatch(0, Slice("giraffe")));

  // Chunk 2 (offset 4100)
  ASSERT_TRUE(reader.KeyMayMatch(4100, Slice("whale")));
  ASSERT_FALSE(reader.KeyMayMatch(4100, Slice("cat")));

  // Chunk 4 (offset 8200)
  ASSERT_TRUE(reader.KeyMayMatch(8200, Slice("zebra")));
  ASSERT_FALSE(reader.KeyMayMatch(8200, Slice("elephant")));
}

}  // namespace purpledb

