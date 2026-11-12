#include "purpledb/slice.h"
#include "util/test_harness.h"

#include <sstream>

namespace purpledb {

TEST(SliceTest, EmptyAndBasics) {
  Slice empty;
  ASSERT_TRUE(empty.empty());
  ASSERT_EQ(empty.size(), 0u);
  ASSERT_EQ(empty.ToString(), "");

  Slice s1("hello");
  ASSERT_FALSE(s1.empty());
  ASSERT_EQ(s1.size(), 5u);
  ASSERT_EQ(s1[0], 'h');
  ASSERT_EQ(s1[4], 'o');
  ASSERT_EQ(s1.ToString(), "hello");

  std::string str = "world";
  Slice s2(str);
  ASSERT_EQ(s2.size(), 5u);
  ASSERT_EQ(s2.ToString(), "world");
}

TEST(SliceTest, RemovePrefixAndClear) {
  Slice s("abcdef");
  s.remove_prefix(2);
  ASSERT_EQ(s.size(), 4u);
  ASSERT_EQ(s.ToString(), "cdef");

  s.clear();
  ASSERT_TRUE(s.empty());
  ASSERT_EQ(s.size(), 0u);
}

TEST(SliceTest, ComparisonAndStartsWith) {
  Slice a("abc");
  Slice b("abcd");
  Slice c("abd");
  Slice a_copy("abc");

  ASSERT_TRUE(a == a_copy);
  ASSERT_FALSE(a != a_copy);
  ASSERT_TRUE(a < b);
  ASSERT_TRUE(b < c);
  ASSERT_TRUE(a <= a_copy);
  ASSERT_TRUE(b > a);
  ASSERT_TRUE(b >= a);

  ASSERT_TRUE(b.StartsWith(a));
  ASSERT_FALSE(a.StartsWith(b));
  ASSERT_TRUE(a.StartsWith(Slice("")));
}

TEST(SliceTest, StreamOutput) {
  Slice s("stream_test");
  std::ostringstream oss;
  oss << s;
  ASSERT_EQ(oss.str(), "stream_test");
}

}  // namespace purpledb
