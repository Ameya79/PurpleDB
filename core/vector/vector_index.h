#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "core/storage/env.h"
#include "core/vector/delete_bitmap.h"
#include "core/vector/distance.h"
#include "core/vector/hnsw.h"
#include "core/vector/vector_segment.h"
#include "purpledb/db.h"
#include "purpledb/status.h"

namespace purpledb {

struct SearchResult {
  std::string key;
  uint64_t vector_id{0};
  float score{0.0f};

  bool operator>(const SearchResult& other) const {
    return score > other.score;
  }
};

struct VectorOptions {
  Options kv_options;
  uint32_t dim{512};
  VectorDataType dtype{VectorDataType::kFloat32};
  MetricType metric{MetricType::kCosine};
  uint32_t chunk_size{64};
  std::string model_id{"openai/clip-vit-base-patch32"};

  // HNSW Approximate Nearest Neighbor parameters (Milestone M6)
  bool enable_hnsw{true};
  uint16_t hnsw_M{16};
  uint16_t hnsw_max_M0{32};
  uint32_t hnsw_efConstruction{200};
  uint32_t hnsw_efSearch{64};
};

/**
 * @brief VectorIndex unifies LSM KV metadata with append-only vector storage.
 *
 * Implements the WiscKey architecture:
 * - High-dimensional vector payloads reside in append-only .vec files.
 * - Moment metadata and vector references reside in the LSM tree.
 * - Deletes are tombstoned in an O(1) in-memory DeleteBitmap.
 * - Nearest neighbor search uses HNSW graph indexing with fallback to SIMD flat scan.
 * - Post-filtering with dynamic over-fetch satisfies metadata constraints.
 */
class VectorIndex {
 public:
  static Status Open(const VectorOptions& options, const std::string& db_path,
                     std::unique_ptr<VectorIndex>* out);

  ~VectorIndex();

  // Store a vector embedding mapped to user key (e.g. "video_1:moment_12")
  Status Put(const std::string& key, const float* vector, size_t dim);

  // Retrieve vector embedding by user key
  Status Get(const std::string& key, std::vector<float>* vector_out);

  // Mark user key as deleted
  Status Delete(const std::string& key);

  // Search for top K closest vectors using HNSW index (or exact SIMD fallback)
  Status Search(const float* query_vector, size_t dim, size_t k,
                std::vector<SearchResult>* results);

  // Exact brute-force SIMD search for top K closest vectors (ground truth baseline)
  Status SearchExact(const float* query_vector, size_t dim, size_t k,
                     std::vector<SearchResult>* results);

  // Filtered vector search with dynamic over-fetch
  Status SearchFiltered(const float* query_vector, size_t dim, size_t k,
                        const std::function<bool(const std::string& key)>& filter_fn,
                        std::vector<SearchResult>* results,
                        size_t overfetch = 2);

  // Rebuild the HNSW graph omitting deleted nodes (vacuum)
  Status RebuildIndex();

  // Check if key exists and is not deleted
  bool Contains(const std::string& key);

  // Number of active (non-deleted) vectors
  uint64_t Count() const;

  // Total vectors ever registered
  uint64_t TotalCount() const { return next_vector_id_; }

  // Fraction of deleted vectors
  double DeletedFraction() const { return delete_bitmap_.DeletedFraction(); }

  // Flush in-flight writes and persist HNSW snapshot
  Status Flush();

  // Fsync storage
  Status Sync();

  const VectorOptions& options() const { return options_; }
  HNSW* hnsw() { return hnsw_.get(); }

 private:
  VectorIndex(const VectorOptions& options, const std::string& db_path,
              std::unique_ptr<DB> db);

  Status Recover();
  Status PersistSystemMetadata();
  Status SaveHNSWSnapshot();

  VectorOptions options_;
  std::string db_path_;
  std::unique_ptr<DB> db_;
  std::unique_ptr<VectorSegmentWriter> segment_writer_;
  std::unique_ptr<HNSW> hnsw_;

  DeleteBitmap delete_bitmap_;
  uint64_t next_vector_id_{0};

  // In-memory cache of flat vectors [N * dim] for SIMD search
  std::vector<float> flat_vectors_;
  std::unordered_map<std::string, uint64_t> key_to_id_;
  std::unordered_map<uint64_t, std::string> id_to_key_;
};

}  // namespace purpledb

