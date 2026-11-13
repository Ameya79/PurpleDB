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

TEST(MemTableTest, Iterator) {
  MemTable mem;
  mem.Add(1, kTypeValue, Slice("apple"), Slice("red"));
  mem.Add(2, kTypeValue, Slice("banana"), Slice("yellow"));
  mem.Add(3, kTypeValue, Slice("cherry"), Slice("dark red"));

  auto iter = mem.NewIterator();
  ASSERT_FALSE(iter->Valid());

  iter->SeekToFirst();
  ASSERT_TRUE(iter->Valid());
  // Internal key has user_key + 8 byte tag
  Slice k1 = iter->key();
  ASSERT_EQ(Slice(k1.data(), k1.size() - 8).ToString(), "apple");
  ASSERT_EQ(iter->value().ToString(), "red");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  Slice k2 = iter->key();
  ASSERT_EQ(Slice(k2.data(), k2.size() - 8).ToString(), "banana");
  ASSERT_EQ(iter->value().ToString(), "yellow");

  iter->Next();
  ASSERT_TRUE(iter->Valid());
  Slice k3 = iter->key();
  ASSERT_EQ(Slice(k3.data(), k3.size() - 8).ToString(), "cherry");
  ASSERT_EQ(iter->value().ToString(), "dark red");

  iter->Next();
  ASSERT_FALSE(iter->Valid());

  // Backward traversal
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(Slice(iter->key().data(), iter->key().size() - 8).ToString(), "cherry");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(Slice(iter->key().data(), iter->key().size() - 8).ToString(), "banana");

  iter->Prev();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(Slice(iter->key().data(), iter->key().size() - 8).ToString(), "apple");

  iter->Prev();
  ASSERT_FALSE(iter->Valid());
}

}  // namespace purpledb
