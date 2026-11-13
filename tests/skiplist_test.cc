#include "core/storage/skiplist.h"
#include "core/util/arena.h"
#include "util/test_harness.h"

#include <set>
#include <thread>
#include <vector>

namespace purpledb {

struct IntComparator {
  int operator()(int a, int b) const {
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
  }
};

TEST(SkipListTest, Empty) {
  Arena arena;
  IntComparator cmp;
  SkipList<int, IntComparator> list(cmp, &arena);

  ASSERT_FALSE(list.Contains(10));

  SkipList<int, IntComparator>::Iterator iter(&list);
  ASSERT_FALSE(iter.Valid());
  iter.SeekToFirst();
  ASSERT_FALSE(iter.Valid());
  iter.Seek(100);
  ASSERT_FALSE(iter.Valid());
  iter.SeekToLast();
  ASSERT_FALSE(iter.Valid());
}

TEST(SkipListTest, InsertAndLookup) {
  Arena arena;
  IntComparator cmp;
  SkipList<int, IntComparator> list(cmp, &arena);

  const int N = 2000;
  std::set<int> keys;

  for (int i = 0; i < N; ++i) {
    int key = (i * 37) % 10000;
    if (keys.insert(key).second) {
      list.Insert(key);
    }
  }

  for (int k : keys) {
    ASSERT_TRUE(list.Contains(k));
  }
  ASSERT_FALSE(list.Contains(99999));
}

TEST(SkipListTest, IteratorTraversal) {
  Arena arena;
  IntComparator cmp;
  SkipList<int, IntComparator> list(cmp, &arena);

  std::vector<int> values = {10, 25, 40, 55, 70, 85, 100};
  for (int v : values) {
    list.Insert(v);
  }

  // Forward traversal
  SkipList<int, IntComparator>::Iterator iter(&list);
  iter.SeekToFirst();
  for (size_t i = 0; i < values.size(); ++i) {
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(iter.key(), values[i]);
    iter.Next();
  }
  ASSERT_FALSE(iter.Valid());

  // Backward traversal
  iter.SeekToLast();
  for (int i = static_cast<int>(values.size()) - 1; i >= 0; --i) {
    ASSERT_TRUE(iter.Valid());
    ASSERT_EQ(iter.key(), values[i]);
    iter.Prev();
  }
  ASSERT_FALSE(iter.Valid());

  // Seek tests
  iter.Seek(25);
  ASSERT_TRUE(iter.Valid());
  ASSERT_EQ(iter.key(), 25);

  iter.Seek(26);  // Not present, should land on 40
  ASSERT_TRUE(iter.Valid());
  ASSERT_EQ(iter.key(), 40);

  iter.Seek(101);  // Beyond last
  ASSERT_FALSE(iter.Valid());
}

TEST(SkipListTest, ConcurrentReaderSingleWriter) {
  Arena arena;
  IntComparator cmp;
  SkipList<int, IntComparator> list(cmp, &arena);

  const int kTotal = 5000;
  std::atomic<bool> done{false};

  // Reader thread continuously iterates through the skiplist
  std::thread reader([&]() {
    while (!done.load(std::memory_order_relaxed)) {
      SkipList<int, IntComparator>::Iterator iter(&list);
      iter.SeekToFirst();
      int prev = -1;
      while (iter.Valid()) {
        int cur = iter.key();
        ASSERT_GT(cur, prev);
        prev = cur;
        iter.Next();
      }
    }
  });

  // Writer thread inserts strictly increasing keys
  for (int i = 0; i < kTotal; ++i) {
    list.Insert(i);
  }

  done.store(true, std::memory_order_relaxed);
  reader.join();

  // Verify all keys present
  for (int i = 0; i < kTotal; ++i) {
    ASSERT_TRUE(list.Contains(i));
  }
}

}  // namespace purpledb
