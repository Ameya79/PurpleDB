#include "core/storage/cache.h"
#include "util/test_harness.h"

#include <string>
#include <thread>
#include <vector>

namespace purpledb {

namespace {

static void CustomDeleter(const Slice& /*key*/, void* value) {
  int* p = reinterpret_cast<int*>(value);
  delete p;
}

}  // namespace

TEST(CacheTest, HitAndMiss) {
  auto cache = NewLRUCache(1000);

  int* v1 = new int(100);
  auto h1 = cache->Insert(Slice("k1"), v1, 1, CustomDeleter);
  ASSERT_TRUE(h1 != nullptr);
  ASSERT_EQ(*reinterpret_cast<int*>(cache->Value(h1)), 100);
  cache->Release(h1);

  auto h2 = cache->Lookup(Slice("k1"));
  ASSERT_TRUE(h2 != nullptr);
  ASSERT_EQ(*reinterpret_cast<int*>(cache->Value(h2)), 100);
  cache->Release(h2);

  auto h3 = cache->Lookup(Slice("non_existent"));
  ASSERT_TRUE(h3 == nullptr);
}

TEST(CacheTest, Erase) {
  auto cache = NewLRUCache(1000);

  int* v1 = new int(42);
  auto h = cache->Insert(Slice("key"), v1, 1, CustomDeleter);
  cache->Release(h);

  cache->Erase(Slice("key"));
  auto h_after = cache->Lookup(Slice("key"));
  ASSERT_TRUE(h_after == nullptr);
}

TEST(CacheTest, Eviction) {
  // Capacity = 16 shards * 1 charge per shard = 16 items
  const size_t capacity = 16;
  auto cache = NewLRUCache(capacity);

  // Insert 100 items (charge 1 each)
  for (int i = 0; i < 100; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "k%d", i);
    int* val = new int(i);
    auto h = cache->Insert(Slice(buf), val, 1, CustomDeleter);
    cache->Release(h);
  }

  // Total charge in cache must not exceed capacity
  ASSERT_LE(cache->TotalCharge(), capacity);
}

TEST(CacheTest, RefcountKeepsAliveDuringEviction) {
  auto cache = NewLRUCache(16);

  int* val = new int(999);
  auto h_kept = cache->Insert(Slice("sticky"), val, 1, CustomDeleter);

  // Insert 200 items to cause massive eviction
  for (int i = 0; i < 200; ++i) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "item%d", i);
    int* v = new int(i);
    auto h = cache->Insert(Slice(buf), v, 1, CustomDeleter);
    cache->Release(h);
  }

  // The kept handle should still be valid with value 999
  ASSERT_EQ(*reinterpret_cast<int*>(cache->Value(h_kept)), 999);
  cache->Release(h_kept);
}

TEST(CacheTest, ConcurrentAccess) {
  auto cache = NewLRUCache(256);
  const int kThreads = 4;
  const int kOps = 500;
  std::vector<std::thread> threads;

  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&cache, t]() {
      for (int i = 0; i < kOps; ++i) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "t%d_key%d", t, i % 50);
        int* v = new int(t * 1000 + i);
        auto h = cache->Insert(Slice(buf), v, 1, CustomDeleter);
        cache->Release(h);

        auto lookup = cache->Lookup(Slice(buf));
        if (lookup != nullptr) {
          int val = *reinterpret_cast<int*>(cache->Value(lookup));
          ASSERT_GE(val, 0);
          cache->Release(lookup);
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }
}

}  // namespace purpledb

