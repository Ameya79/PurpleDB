#include "core/storage/block.h"
#include "core/storage/block_builder.h"
#include "util/test_harness.h"

#include <string>
#include <vector>

namespace purpledb {

TEST(BlockTest, EmptyBlock) {
  BlockBuilder builder(16);
  Slice contents = builder.Finish();

  Block block(contents.data(), contents.size(), false);
  auto iter = block.NewIterator();
  ASSERT_FALSE(iter->Valid());

  iter->SeekToFirst();
  ASSERT_FALSE(iter->Valid());

  iter->Seek(Slice("any"));
  ASSERT_FALSE(iter->Valid());
}

TEST(BlockTest, SingleEntry) {
  BlockBuilder builder(16);
  builder.Add(Slice("hello"), Slice("world"));
  Slice contents = builder.Finish();

  Block block(contents.data(), contents.size(), false);
  auto iter = block.NewIterator();

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "hello");
  ASSERT_EQ(iter->value().ToString(), "world");

  iter->Next();
  ASSERT_FALSE(iter->Valid());

  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "hello");
  ASSERT_EQ(iter->value().ToString(), "world");

  iter->Prev();
  ASSERT_FALSE(iter->Valid());
}

TEST(BlockTest, PrefixCompressionAndTraversal) {
  // Use restart interval of 4 so multiple restart points are created
  BlockBuilder builder(4);

  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < 50; ++i) {
    char key_buf[32];
    std::snprintf(key_buf, sizeof(key_buf), "prefix/%04d", i);
    char val_buf[32];
    std::snprintf(val_buf, sizeof(val_buf), "val_%04d", i);
    kv.push_back({key_buf, val_buf});
    builder.Add(Slice(key_buf), Slice(val_buf));
  }

  Slice contents = builder.Finish();
  Block block(contents.data(), contents.size(), false);
  auto iter = block.NewIterator();

  // Forward traversal
  iter->SeekToFirst();
  for (size_t i = 0; i < kv.size(); ++i) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().ToString(), kv[i].first);
    ASSERT_EQ(iter->value().ToString(), kv[i].second);
    iter->Next();
  }
  ASSERT_FALSE(iter->Valid());

  // Backward traversal
  iter->SeekToLast();
  for (int i = static_cast<int>(kv.size()) - 1; i >= 0; --i) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().ToString(), kv[i].first);
    ASSERT_EQ(iter->value().ToString(), kv[i].second);
    iter->Prev();
  }
  ASSERT_FALSE(iter->Valid());

  // Seek tests
  // Exact match
  iter->Seek(Slice("prefix/0025"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "prefix/0025");
  ASSERT_EQ(iter->value().ToString(), "val_0025");

  // In-between match
  iter->Seek(Slice("prefix/0025a"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "prefix/0026");

  // Before first
  iter->Seek(Slice("aaa"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "prefix/0000");

  // Past last
  iter->Seek(Slice("zzz"));
  ASSERT_FALSE(iter->Valid());
}

TEST(BlockTest, CorruptionSafety) {
  // Truncated block
  const char corrupt_bytes[] = "short";
  Block corrupt_block(corrupt_bytes, sizeof(corrupt_bytes), false);
  auto iter = corrupt_block.NewIterator();
  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(iter->status().IsCorruption());
}

}  // namespace purpledb
