#include "core/vector/vector_index.h"

#include <cmath>
#include <vector>
#include "core/storage/env.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

class VectorIndexTest {
 public:
  VectorIndexTest() : env_(Env::Default()) {
    test_dir_ = "./vec_index_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~VectorIndexTest() {
    std::vector<std::string> children;
    env_->GetChildren(test_dir_, &children);
    for (const auto& f : children) {
      if (f != "." && f != "..") {
        env_->DeleteFile(test_dir_ + "/" + f);
      }
    }
    env_->DeleteDir(test_dir_);
  }

  Env* env_;
  std::string test_dir_;
};

TEST(VectorIndexTest, PutGetDelete) {
  VectorIndexTest fixture;
  VectorOptions options;
  options.dim = 4;
  options.chunk_size = 4;

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
  ASSERT_STATUS_OK(s);

  std::vector<float> v1 = {1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> v2 = {0.0f, 1.0f, 0.0f, 0.0f};
  std::vector<float> v3 = {0.0f, 0.0f, 1.0f, 0.0f};

  s = index->Put("vid1:m1", v1.data(), 4);
  ASSERT_STATUS_OK(s);
  s = index->Put("vid1:m2", v2.data(), 4);
  ASSERT_STATUS_OK(s);
  s = index->Put("vid2:m1", v3.data(), 4);
  ASSERT_STATUS_OK(s);

  ASSERT_EQ(index->Count(), 3u);
  ASSERT_TRUE(index->Contains("vid1:m1"));
  ASSERT_TRUE(index->Contains("vid1:m2"));
  ASSERT_TRUE(index->Contains("vid2:m1"));
  ASSERT_FALSE(index->Contains("vid3:m1"));

  std::vector<float> read_v;
  s = index->Get("vid1:m1", &read_v);
  ASSERT_STATUS_OK(s);
  ASSERT_TRUE(read_v == v1);

  // Delete vid1:m2
  s = index->Delete("vid1:m2");
  ASSERT_STATUS_OK(s);
  ASSERT_FALSE(index->Contains("vid1:m2"));
  ASSERT_EQ(index->Count(), 2u);

  s = index->Get("vid1:m2", &read_v);
  ASSERT_TRUE(s.IsNotFound());
}

TEST(VectorIndexTest, SearchTopK) {
  VectorIndexTest fixture;
  VectorOptions options;
  options.dim = 4;

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
  ASSERT_STATUS_OK(s);

  std::vector<float> v_x = {1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> v_diag = {0.7071f, 0.7071f, 0.0f, 0.0f};
  std::vector<float> v_y = {0.0f, 1.0f, 0.0f, 0.0f};
  std::vector<float> v_z = {0.0f, 0.0f, 1.0f, 0.0f};

  index->Put("x", v_x.data(), 4);
  index->Put("diag", v_diag.data(), 4);
  index->Put("y", v_y.data(), 4);
  index->Put("z", v_z.data(), 4);

  // Search with query vector pointing along x-axis
  std::vector<float> query = {1.0f, 0.0f, 0.0f, 0.0f};
  std::vector<SearchResult> results;
  s = index->Search(query.data(), 4, 3, &results);
  ASSERT_STATUS_OK(s);

  ASSERT_EQ(results.size(), 3u);
  ASSERT_EQ(results[0].key, "x");
  ASSERT_NEAR(results[0].score, 1.0f, 1e-4f);

  ASSERT_EQ(results[1].key, "diag");
  ASSERT_NEAR(results[1].score, 0.7071f, 1e-3f);

  // Delete "x", verify "diag" becomes rank 1
  index->Delete("x");
  s = index->Search(query.data(), 4, 2, &results);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(results.size(), 2u);
  ASSERT_EQ(results[0].key, "diag");
}

TEST(VectorIndexTest, PersistenceAndModelLock) {
  VectorIndexTest fixture;
  VectorOptions options;
  options.dim = 8;
  options.model_id = "test-clip";

  {
    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
    ASSERT_STATUS_OK(s);

    std::vector<float> vec(8, 0.5f);
    index->Put("persistent_key", vec.data(), 8);
    index->Sync();
  }

  // Reopen successfully with matching options
  {
    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
    ASSERT_STATUS_OK(s);
    ASSERT_TRUE(index->Contains("persistent_key"));

    std::vector<float> vec;
    s = index->Get("persistent_key", &vec);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(vec.size(), 8u);
  }

  // Reopen with incompatible dimension must fail per model lock specification
  {
    VectorOptions bad_options = options;
    bad_options.dim = 16;  // Incompatible
    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(bad_options, fixture.test_dir_, &index);
    ASSERT_FALSE(s.ok());
  }

  // Reopen with incompatible model_id must fail
  {
    VectorOptions bad_options = options;
    bad_options.model_id = "other-clip";  // Incompatible
    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(bad_options, fixture.test_dir_, &index);
    ASSERT_FALSE(s.ok());
  }
}

}  // namespace test
}  // namespace purpledb
