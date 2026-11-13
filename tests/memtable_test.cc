#include "core/storage/memtable.h"
#include "util/test_harness.h"

#include <string>

namespace purpledb {

TEST(MemTableTest, PutAndGet) {
  MemTable mem;
  mem.Add(1, kTypeValue, Slice("key1"), Slice("value1"));
  mem.Add(2, kTypeValue, Slice("key2"), Slice("value2"));

  std::string value;
  Status s;
  ASSERT_TRUE(mem.Get(Slice("key1"), 2, &value, &s));
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(value, "value1");

  ASSERT_TRUE(mem.Get(Slice("key2"), 2, &value, &s));
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(value, "value2");

  // Non-existent key
  ASSERT_FALSE(mem.Get(Slice("key3"), 2, &value, &s));
}

TEST(MemTableTest, OverwriteAndSequenceVisibility) {
  MemTable mem;
  mem.Add(10, kTypeValue, Slice("key"), Slice("old_val"));
  mem.Add(20, kTypeValue, Slice("key"), Slice("new_val"));

  std::string value;
  Status s;

  // Reading at sequence 15 sees "old_val"
  ASSERT_TRUE(mem.Get(Slice("key"), 15, &value, &s));
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(value, "old_val");

  // Reading at sequence 25 sees "new_val"
  ASSERT_TRUE(mem.Get(Slice("key"), 25, &value, &s));
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(value, "new_val");

  // Reading before sequence 10 sees nothing
  ASSERT_FALSE(mem.Get(Slice("key"), 5, &value, &s));
}

TEST(MemTableTest, DeletionTombstone) {
  MemTable mem;
  mem.Add(1, kTypeValue, Slice("key"), Slice("val"));
  mem.Add(2, kTypeDeletion, Slice("key"), Slice(""));

  std::string value;
  Status s;

  // At seq 1: exists
  ASSERT_TRUE(mem.Get(Slice("key"), 1, &value, &s));
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(value, "val");

  // At seq 2: tombstone found, Status is NotFound
  ASSERT_TRUE(mem.Get(Slice("key"), 2, &value, &s));
  ASSERT_TRUE(s.IsNotFound());
}

}  // namespace purpledb
