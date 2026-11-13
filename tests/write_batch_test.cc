#include "purpledb/write_batch.h"
#include "util/test_harness.h"

#include <map>
#include <string>

namespace purpledb {

class MockHandler : public WriteBatch::Handler {
 public:
  Status Put(const Slice& key, const Slice& value) override {
    map_[key.ToString()] = value.ToString();
    return Status::OK();
  }

  Status Delete(const Slice& key) override {
    map_.erase(key.ToString());
    return Status::OK();
  }

  std::map<std::string, std::string> map_;
};

TEST(WriteBatchTest, EmptyBatch) {
  WriteBatch batch;
  ASSERT_EQ(batch.Count(), 0);
  ASSERT_EQ(batch.Sequence(), 0u);

  MockHandler handler;
  ASSERT_STATUS_OK(batch.Iterate(&handler));
  ASSERT_TRUE(handler.map_.empty());
}

TEST(WriteBatchTest, PutDeleteIterate) {
  WriteBatch batch;
  batch.SetSequence(100);
  batch.Put(Slice("k1"), Slice("v1"));
  batch.Put(Slice("k2"), Slice("v2"));
  batch.Put(Slice("k3"), Slice("v3"));
  batch.Delete(Slice("k2"));

  ASSERT_EQ(batch.Count(), 4);
  ASSERT_EQ(batch.Sequence(), 100u);

  MockHandler handler;
  ASSERT_STATUS_OK(batch.Iterate(&handler));
  ASSERT_EQ(handler.map_.size(), 2u);
  ASSERT_EQ(handler.map_["k1"], "v1");
  ASSERT_EQ(handler.map_["k3"], "v3");
  ASSERT_TRUE(handler.map_.find("k2") == handler.map_.end());
}

TEST(WriteBatchTest, ClearAndReuse) {
  WriteBatch batch;
  batch.Put(Slice("a"), Slice("1"));
  ASSERT_EQ(batch.Count(), 1);

  batch.Clear();
  ASSERT_EQ(batch.Count(), 0);

  batch.Put(Slice("b"), Slice("2"));
  ASSERT_EQ(batch.Count(), 1);

  MockHandler handler;
  ASSERT_STATUS_OK(batch.Iterate(&handler));
  ASSERT_EQ(handler.map_.size(), 1u);
  ASSERT_EQ(handler.map_["b"], "2");
}

}  // namespace purpledb
