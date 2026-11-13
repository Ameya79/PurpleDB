#include "core/storage/merger.h"
#include "core/storage/snapshot.h"
#include "purpledb/iterator.h"
#include "util/test_harness.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace purpledb {

namespace {

// In-memory test iterator over a std::map
class MapIterator : public Iterator {
 public:
  explicit MapIterator(std::map<std::string, std::string> map)
      : map_(std::move(map)), it_(map_.end()) {}

  bool Valid() const override { return it_ != map_.end(); }

  void SeekToFirst() override { it_ = map_.begin(); }
  void SeekToLast() override {
    if (map_.empty()) {
      it_ = map_.end();
    } else {
      it_ = std::prev(map_.end());
    }
  }

  void Seek(const Slice& target) override {
    it_ = map_.lower_bound(target.ToString());
  }

  void Next() override {
    assert(Valid());
    ++it_;
  }

  void Prev() override {
    assert(Valid());
    if (it_ == map_.begin()) {
      it_ = map_.end();
    } else {
      --it_;
    }
  }

  Slice key() const override {
    assert(Valid());
    return Slice(it_->first);
  }

  Slice value() const override {
    assert(Valid());
    return Slice(it_->second);
  }

  Status status() const override { return Status::OK(); }

 private:
  std::map<std::string, std::string> map_;
  std::map<std::string, std::string>::iterator it_;
};

}  // namespace

TEST(MergingIteratorTest, Empty) {
  std::vector<std::unique_ptr<Iterator>> children;
  auto iter = NewMergingIterator(nullptr, std::move(children));
  ASSERT_FALSE(iter->Valid());

  // Children that are empty
  children.clear();
  children.push_back(std::make_unique<MapIterator>(std::map<std::string, std::string>{}));
  children.push_back(std::make_unique<MapIterator>(std::map<std::string, std::string>{}));
  auto iter2 = NewMergingIterator(nullptr, std::move(children));
  iter2->SeekToFirst();
  ASSERT_FALSE(iter2->Valid());
  iter2->SeekToLast();
  ASSERT_FALSE(iter2->Valid());
}

TEST(MergingIteratorTest, DisjointChildrenForwardAndBackward) {
  std::map<std::string, std::string> map1 = {{"a", "1"}, {"c", "3"}, {"e", "5"}};
  std::map<std::string, std::string> map2 = {{"b", "2"}, {"d", "4"}, {"f", "6"}};

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<MapIterator>(map1));
  children.push_back(std::make_unique<MapIterator>(map2));

  auto iter = NewMergingIterator(nullptr, std::move(children));

  // 1. Forward scan
  iter->SeekToFirst();
  std::string forward_keys;
  while (iter->Valid()) {
    forward_keys += iter->key().ToString();
    iter->Next();
  }
  ASSERT_EQ(forward_keys, "abcdef");

  // 2. Backward scan
  iter->SeekToLast();
  std::string backward_keys;
  while (iter->Valid()) {
    backward_keys += iter->key().ToString();
    iter->Prev();
  }
  ASSERT_EQ(backward_keys, "fedcba");

  // 3. Point seek
  iter->Seek(Slice("c"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key(), Slice("c"));
  ASSERT_EQ(iter->value(), Slice("3"));

  iter->Seek(Slice("cz"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key(), Slice("d"));
}

TEST(MergingIteratorTest, OverlappingChildren) {
  std::map<std::string, std::string> map1 = {{"k1", "v1_a"}, {"k3", "v3"}, {"k5", "v5"}};
  std::map<std::string, std::string> map2 = {{"k1", "v1_b"}, {"k2", "v2"}, {"k4", "v4"}};
  std::map<std::string, std::string> map3 = {{"k0", "v0"}, {"k1", "v1_c"}, {"k6", "v6"}};

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(std::make_unique<MapIterator>(map1));
  children.push_back(std::make_unique<MapIterator>(map2));
  children.push_back(std::make_unique<MapIterator>(map3));

  auto iter = NewMergingIterator(nullptr, std::move(children));

  std::vector<std::string> seen;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    seen.push_back(iter->key().ToString());
  }

  ASSERT_EQ(seen.size(), 9u);  // k0, k1, k1, k1, k2, k3, k4, k5, k6 (9 total entries)
  ASSERT_EQ(seen[0], "k0");
  ASSERT_EQ(seen[1], "k1");
  ASSERT_EQ(seen[2], "k1");
  ASSERT_EQ(seen[3], "k1");
  ASSERT_EQ(seen[4], "k2");
  ASSERT_EQ(seen[5], "k3");
  ASSERT_EQ(seen[6], "k4");
  ASSERT_EQ(seen[7], "k5");
  ASSERT_EQ(seen[8], "k6");
}

TEST(SnapshotTest, ListOrderingAndOldest) {
  SnapshotList list;
  ASSERT_TRUE(list.empty());

  auto* s1 = list.New(100);
  auto* s2 = list.New(200);
  auto* s3 = list.New(300);

  ASSERT_FALSE(list.empty());
  ASSERT_EQ(list.oldest()->sequence_number(), 100u);

  list.Delete(s1);
  ASSERT_EQ(list.oldest()->sequence_number(), 200u);

  list.Delete(s2);
  ASSERT_EQ(list.oldest()->sequence_number(), 300u);

  list.Delete(s3);
  ASSERT_TRUE(list.empty());
}

}  // namespace purpledb
