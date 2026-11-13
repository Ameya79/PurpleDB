#include "core/util/arena.h"
#include "util/test_harness.h"

#include <vector>

namespace purpledb {

TEST(ArenaTest, Empty) {
  Arena arena;
  ASSERT_EQ(arena.MemoryUsage(), 0u);
}

TEST(ArenaTest, SimpleAllocations) {
  Arena arena;
  const int N = 1000;
  std::vector<std::pair<size_t, char*>> allocated;

  for (int i = 0; i < N; ++i) {
    size_t num_bytes = (i % 64) + 1;
    char* p = arena.Allocate(num_bytes);
    ASSERT_TRUE(p != nullptr);
    for (size_t b = 0; b < num_bytes; ++b) {
      p[b] = static_cast<char>(i & 0xff);
    }
    allocated.push_back({num_bytes, p});
  }

  // Verify data integrity
  for (int i = 0; i < N; ++i) {
    size_t num_bytes = allocated[i].first;
    char* p = allocated[i].second;
    for (size_t b = 0; b < num_bytes; ++b) {
      ASSERT_EQ(p[b], static_cast<char>(i & 0xff));
    }
  }

  ASSERT_GT(arena.MemoryUsage(), 0u);
}

TEST(ArenaTest, AlignedAllocations) {
  Arena arena;
  for (int i = 0; i < 100; ++i) {
    size_t num_bytes = (i % 31) + 1;
    char* p = arena.AllocateAligned(num_bytes);
    ASSERT_TRUE(p != nullptr);
    ASSERT_EQ(reinterpret_cast<uintptr_t>(p) % sizeof(void*), 0u);
  }
}

TEST(ArenaTest, LargeAllocations) {
  Arena arena;
  // Allocation larger than 1/4 of kBlockSize (4096 / 4 = 1024)
  char* p1 = arena.Allocate(2048);
  ASSERT_TRUE(p1 != nullptr);
  char* p2 = arena.AllocateAligned(8192);
  ASSERT_TRUE(p2 != nullptr);
  ASSERT_EQ(reinterpret_cast<uintptr_t>(p2) % sizeof(void*), 0u);
  ASSERT_GE(arena.MemoryUsage(), 2048u + 8192u);
}

}  // namespace purpledb
