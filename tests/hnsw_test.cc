#include "core/vector/hnsw.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>
#include "core/storage/env.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

class HNSWTest {
 public:
  HNSWTest() : env_(Env::Default()) {
    test_dir_ = "./hnsw_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~HNSWTest() {
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

TEST(HNSWTest, LevelDistribution) {
  HNSWOptions options;
  options.M = 16;
  HNSW hnsw(options);

  // Sample 1,000 random levels
  std::vector<int> level_counts(10, 0);
  for (int i = 0; i < 1000; ++i) {
    uint32_t l = hnsw.Insert(i, std::vector<float>(options.dim, 0.0f).data(), options.dim).ok() ? 0 : 0;
    (void)l;
  }

  // Max level should be around 2-4 for 1,000 nodes with M=16
  ASSERT_GT(hnsw.MaxLevel(), 0u);
  ASSERT_LT(hnsw.MaxLevel(), 8u);
}

TEST(HNSWTest, HighRecallAgainstBruteForce) {
  HNSWOptions options;
  options.dim = 32;
  options.M = 16;
  options.max_M0 = 32;
  options.efConstruction = 100;
  options.efSearch = 64;

  HNSW hnsw(options);

  const size_t num_vectors = 500;
  const size_t num_queries = 20;
  const size_t k = 10;

  std::mt19937 rng(1337);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  std::vector<std::vector<float>> dataset(num_vectors, std::vector<float>(options.dim));
  for (size_t i = 0; i < num_vectors; ++i) {
    for (size_t d = 0; d < options.dim; ++d) {
      dataset[i][d] = dist(rng);
    }
    NormalizeVector(dataset[i].data(), options.dim);
    Status s = hnsw.Insert(i, dataset[i].data(), options.dim);
    ASSERT_STATUS_OK(s);
  }

  size_t total_intersect = 0;
  size_t total_k = num_queries * k;

  for (size_t q = 0; q < num_queries; ++q) {
    std::vector<float> query(options.dim);
    for (size_t d = 0; d < options.dim; ++d) {
      query[d] = dist(rng);
    }
    NormalizeVector(query.data(), options.dim);

    // Compute exact brute-force Top K
    std::vector<std::pair<float, uint64_t>> ground_truth;
    ground_truth.reserve(num_vectors);
    for (size_t i = 0; i < num_vectors; ++i) {
      float dot = DotProduct(query.data(), dataset[i].data(), options.dim);
      ground_truth.push_back({dot, i});
    }
    std::sort(ground_truth.begin(), ground_truth.end(), std::greater<std::pair<float, uint64_t>>());

    std::vector<uint64_t> gt_ids;
    for (size_t i = 0; i < k; ++i) {
      gt_ids.push_back(ground_truth[i].second);
    }

    // Run HNSW search
    std::vector<HNSWSearchResult> hnsw_results;
    Status s = hnsw.Search(query.data(), options.dim, k, nullptr, &hnsw_results);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(hnsw_results.size(), k);

    // Count intersection
    for (const auto& res : hnsw_results) {
      if (std::find(gt_ids.begin(), gt_ids.end(), res.node_id) != gt_ids.end()) {
        total_intersect++;
      }
    }
  }

  double recall = static_cast<double>(total_intersect) / static_cast<double>(total_k);
  // HNSW must achieve at least 95% recall@10 on this dataset
  ASSERT_GE(recall, 0.95);
}

TEST(HNSWTest, SnapshotSaveLoadAndTailReplay) {
  HNSWTest fixture;
  HNSWOptions options;
  options.dim = 16;
  options.M = 8;
  options.efConstruction = 50;

  std::string snapshot_file = fixture.test_dir_ + "/test.hnsw";

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  std::vector<float> data(50 * 16);
  for (size_t i = 0; i < 50; ++i) {
    for (size_t d = 0; d < 16; ++d) {
      data[i * 16 + d] = dist(rng);
    }
    NormalizeVector(data.data() + i * 16, 16);
  }

  // Build original graph with 50 nodes
  {
    HNSW hnsw(options);
    for (size_t i = 0; i < 50; ++i) {
      hnsw.Insert(i, data.data() + i * 16, 16);
    }
    Status s = hnsw.SaveSnapshot(fixture.env_, snapshot_file);
    ASSERT_STATUS_OK(s);
  }

  // Load snapshot into fresh HNSW instance
  HNSW recovered(options);
  Status s = recovered.LoadSnapshot(fixture.env_, snapshot_file);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(recovered.NodeCount(), 50u);

  // Supply flat vectors to recovered instance
  std::vector<float> tail_data(20 * 16);
  for (size_t i = 0; i < 20; ++i) {
    for (size_t d = 0; d < 16; ++d) {
      tail_data[i * 16 + d] = dist(rng);
    }
    NormalizeVector(tail_data.data() + i * 16, 16);
  }

  // Replay tail inserts (nodes 50 to 69)
  s = recovered.ReplayTail(50, tail_data, 16);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(recovered.NodeCount(), 70u);

  // Query should successfully return results
  std::vector<HNSWSearchResult> results;
  s = recovered.Search(tail_data.data(), 16, 5, nullptr, &results);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(results.size(), 5u);
  // Nearest node to tail_data[0] should be node 50 (itself)
  ASSERT_EQ(results[0].node_id, 50u);
  ASSERT_NEAR(results[0].score, 1.0f, 1e-4f);
}

TEST(HNSWTest, DeleteTombstoneFiltering) {
  HNSWOptions options;
  options.dim = 8;
  HNSW hnsw(options);

  std::vector<float> v0 = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> v1 = {0.9f, 0.1f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  std::vector<float> v2 = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  NormalizeVector(v1.data(), 8);

  hnsw.Insert(0, v0.data(), 8);
  hnsw.Insert(1, v1.data(), 8);
  hnsw.Insert(2, v2.data(), 8);

  DeleteBitmap tombstones;
  // Without tombstones: v0 is closest to query v0
  std::vector<HNSWSearchResult> results;
  hnsw.Search(v0.data(), 8, 2, &tombstones, &results);
  ASSERT_EQ(results.size(), 2u);
  ASSERT_EQ(results[0].node_id, 0u);

  // Tombstone node 0: now node 1 must become top result
  tombstones.MarkDeleted(0);
  hnsw.Search(v0.data(), 8, 2, &tombstones, &results);
  ASSERT_EQ(results.size(), 2u);
  ASSERT_EQ(results[0].node_id, 1u);
  ASSERT_EQ(results[1].node_id, 2u);
}

}  // namespace test
}  // namespace purpledb
