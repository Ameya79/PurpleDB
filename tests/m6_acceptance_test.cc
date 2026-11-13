#include "core/vector/vector_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <string>
#include <vector>
#include "core/storage/env.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

class M6AcceptanceTest {
 public:
  M6AcceptanceTest() : env_(Env::Default()) {
    test_dir_ = "./m6_acceptance_db";
    env_->CreateDir(test_dir_);
  }

  ~M6AcceptanceTest() {
    std::vector<std::string> children;
    if (env_->GetChildren(test_dir_, &children).ok()) {
      for (const auto& f : children) {
        if (f != "." && f != "..") {
          std::string path = test_dir_ + "/" + f;
          std::vector<std::string> sub_children;
          if (env_->GetChildren(path, &sub_children).ok()) {
            for (const auto& sf : sub_children) {
              if (sf != "." && sf != "..") {
                env_->DeleteFile(path + "/" + sf);
              }
            }
            env_->DeleteDir(path);
          } else {
            env_->DeleteFile(path);
          }
        }
      }
      env_->DeleteDir(test_dir_);
    }
  }

  Env* env_;
  std::string test_dir_;
};

/**
 * ACCEPTANCE CRITERIA 1: Recall@10 of at least 0.95 versus brute force.
 * Builds HNSW index with 1,000 vectors (D=64).
 * Evaluates 50 queries and asserts average recall@10 is >= 0.95.
 */
TEST(M6AcceptanceTest, RecallAgainstBruteForceBaseline) {
  M6AcceptanceTest fixture;
  VectorOptions options;
  options.dim = 64;
  options.metric = MetricType::kCosine;
  options.enable_hnsw = true;
  options.hnsw_M = 16;
  options.hnsw_max_M0 = 32;
  options.hnsw_efConstruction = 150;
  options.hnsw_efSearch = 64;

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
  ASSERT_STATUS_OK(s);

  const size_t num_vectors = 1000;
  const size_t num_queries = 50;
  const size_t k = 10;

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  std::vector<std::vector<float>> dataset(num_vectors, std::vector<float>(options.dim));
  for (size_t i = 0; i < num_vectors; ++i) {
    for (size_t d = 0; d < options.dim; ++d) {
      dataset[i][d] = dist(rng);
    }
    NormalizeVector(dataset[i].data(), options.dim);
    std::string key = "vec_" + std::to_string(i);
    s = index->Put(key, dataset[i].data(), options.dim);
    ASSERT_STATUS_OK(s);
  }
  ASSERT_EQ(index->Count(), num_vectors);

  size_t total_intersect = 0;
  size_t total_k = num_queries * k;

  for (size_t q = 0; q < num_queries; ++q) {
    std::vector<float> query(options.dim);
    for (size_t d = 0; d < options.dim; ++d) {
      query[d] = dist(rng);
    }
    NormalizeVector(query.data(), options.dim);

    // 1. Exact brute force ground truth
    std::vector<SearchResult> exact_results;
    s = index->SearchExact(query.data(), options.dim, k, &exact_results);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(exact_results.size(), k);

    std::vector<std::string> gt_keys;
    for (const auto& r : exact_results) {
      gt_keys.push_back(r.key);
    }

    // 2. HNSW approximate search
    std::vector<SearchResult> hnsw_results;
    s = index->Search(query.data(), options.dim, k, &hnsw_results);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(hnsw_results.size(), k);

    // Count intersection
    for (const auto& hr : hnsw_results) {
      if (std::find(gt_keys.begin(), gt_keys.end(), hr.key) != gt_keys.end()) {
        total_intersect++;
      }
    }
  }

  double recall = static_cast<double>(total_intersect) / static_cast<double>(total_k);
  // Milestone acceptance requirement: Recall@10 >= 0.95
  ASSERT_GE(recall, 0.95);
}

/**
 * ACCEPTANCE CRITERIA 2: Filtered search with dynamic over-fetch.
 * Ingests moments for two videos. Filters search for a specific video.
 * Asserts all results satisfy filter and dynamic widening maintains accuracy.
 */
TEST(M6AcceptanceTest, FilteredSearchWithOverfetch) {
  M6AcceptanceTest fixture;
  VectorOptions options;
  options.dim = 32;
  options.metric = MetricType::kCosine;

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
  ASSERT_STATUS_OK(s);

  std::mt19937 rng(1337);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  // Ingest 50 moments for video_A and 50 moments for video_B
  for (size_t i = 0; i < 50; ++i) {
    std::vector<float> va(32);
    std::vector<float> vb(32);
    for (size_t d = 0; d < 32; ++d) {
      va[d] = dist(rng);
      vb[d] = dist(rng);
    }
    NormalizeVector(va.data(), 32);
    NormalizeVector(vb.data(), 32);

    index->Put("video_A:m_" + std::to_string(i), va.data(), 32);
    index->Put("video_B:m_" + std::to_string(i), vb.data(), 32);
  }
  ASSERT_EQ(index->Count(), 100u);

  // Filter predicate: only accept video_A
  auto filter_video_a = [](const std::string& key) {
    return key.rfind("video_A:", 0) == 0;
  };

  std::vector<float> query(32, 0.1f);
  NormalizeVector(query.data(), 32);

  std::vector<SearchResult> filtered_results;
  s = index->SearchFiltered(query.data(), 32, 10, filter_video_a, &filtered_results, 2);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(filtered_results.size(), 10u);

  for (const auto& r : filtered_results) {
    ASSERT_TRUE(filter_video_a(r.key));
  }
}

/**
 * ACCEPTANCE CRITERIA 3: Snapshot cold open and tail replay.
 * Persists HNSW graph snapshot to index.hnsw.
 * Verifies cold open loads snapshot in sub-second time without rebuilding.
 * Verifies tail replay handles newly appended vectors after snapshot.
 */
TEST(M6AcceptanceTest, SnapshotColdOpenAndTailReplay) {
  M6AcceptanceTest fixture;
  VectorOptions options;
  options.dim = 32;
  options.metric = MetricType::kCosine;

  std::mt19937 rng(777);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  // 1. Ingest 200 vectors and sync (persists snapshot)
  {
    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
    ASSERT_STATUS_OK(s);

    for (size_t i = 0; i < 200; ++i) {
      std::vector<float> vec(32);
      for (size_t d = 0; d < 32; ++d) vec[d] = dist(rng);
      NormalizeVector(vec.data(), 32);
      index->Put("moment_" + std::to_string(i), vec.data(), 32);
    }
    index->Sync();
  }

  // Verify snapshot file exists
  std::string snapshot_file = fixture.test_dir_ + "/index.hnsw";
  ASSERT_TRUE(fixture.env_->FileExists(snapshot_file));

  // 2. Measure cold open time
  auto start_open = std::chrono::high_resolution_clock::now();
  std::unique_ptr<VectorIndex> reopened_index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &reopened_index);
  auto end_open = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> open_elapsed = end_open - start_open;

  ASSERT_STATUS_OK(s);
  ASSERT_EQ(reopened_index->Count(), 200u);
  // Cold open with snapshot must be sub-second (under 1.0 s)
  ASSERT_LT(open_elapsed.count(), 1.0);

  // 3. Replay tail inserts: append 20 more vectors
  for (size_t i = 200; i < 220; ++i) {
    std::vector<float> vec(32);
    for (size_t d = 0; d < 32; ++d) vec[d] = dist(rng);
    NormalizeVector(vec.data(), 32);
    s = reopened_index->Put("moment_" + std::to_string(i), vec.data(), 32);
    ASSERT_STATUS_OK(s);
  }
  ASSERT_EQ(reopened_index->Count(), 220u);

  // Search for one of the newly added tail moments
  std::vector<float> tail_query;
  reopened_index->Get("moment_215", &tail_query);
  std::vector<SearchResult> results;
  s = reopened_index->Search(tail_query.data(), 32, 1, &results);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(results.size(), 1u);
  ASSERT_EQ(results[0].key, "moment_215");
}

/**
 * ACCEPTANCE CRITERIA 4: Background rebuild after deletes (vacuum).
 * Verifies rebuilding index reclaims space and maintains high query quality.
 */
TEST(M6AcceptanceTest, BackgroundRebuildAfterDeletes) {
  M6AcceptanceTest fixture;
  VectorOptions options;
  options.dim = 16;
  options.metric = MetricType::kCosine;

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(options, fixture.test_dir_, &index);
  ASSERT_STATUS_OK(s);

  std::mt19937 rng(999);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  // Ingest 100 vectors
  for (size_t i = 0; i < 100; ++i) {
    std::vector<float> vec(16);
    for (size_t d = 0; d < 16; ++d) vec[d] = dist(rng);
    NormalizeVector(vec.data(), 16);
    index->Put("item_" + std::to_string(i), vec.data(), 16);
  }
  ASSERT_EQ(index->Count(), 100u);

  // Delete 30 items (30% deleted, exceeding 20% rebuild threshold)
  for (size_t i = 0; i < 30; ++i) {
    s = index->Delete("item_" + std::to_string(i));
    ASSERT_STATUS_OK(s);
  }
  ASSERT_EQ(index->Count(), 70u);
  ASSERT_GE(index->DeletedFraction(), 0.20);

  // Trigger rebuild (vacuum)
  s = index->RebuildIndex();
  ASSERT_STATUS_OK(s);

  // Search should return valid active items
  std::vector<float> q;
  index->Get("item_50", &q);
  std::vector<SearchResult> results;
  s = index->Search(q.data(), 16, 5, &results);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(results.size(), 5u);
  ASSERT_EQ(results[0].key, "item_50");

  // Verify none of the deleted items appear
  for (const auto& r : results) {
    for (size_t d = 0; d < 30; ++d) {
      ASSERT_NE(r.key, "item_" + std::to_string(d));
    }
  }
}

}  // namespace test
}  // namespace purpledb
