#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>
#include "core/storage/env.h"
#include "core/vector/delete_bitmap.h"
#include "core/vector/distance.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

struct HNSWOptions {
  uint32_t dim{512};
  MetricType metric{MetricType::kCosine};
  uint16_t M{16};             // Max outgoing edges per node at levels >= 1
  uint16_t max_M0{32};        // Max outgoing edges per node at level 0
  uint32_t efConstruction{200}; // Beam search width during insertion
  uint32_t efSearch{64};      // Beam search width during query
  uint32_t random_seed{42};
};

struct HNSWSearchResult {
  uint64_t node_id{0};
  float score{0.0f};  // Higher is closer (cosine similarity)
  float distance{0.0f}; // Lower is closer (1.0 - cosine)

  bool operator>(const HNSWSearchResult& other) const {
    return distance < other.distance; // Min-heap compares smaller distance as greater priority
  }
};

/**
 * @brief Hierarchical Navigable Small World (HNSW) proximity graph index.
 *
 * CONCEPT FOR LEARNERS:
 * Brute-force nearest neighbor search compares a query vector to all N vectors (O(N) time).
 * For 1M moments, brute force takes several seconds per query.
 *
 * HNSW constructs a multi-layer graph analogous to a Skip List in geometric space:
 * - Layer 0 (the bottom layer) contains all indexed vectors.
 * - Upper layers contain exponentially fewer nodes, creating long-range "express highways".
 * - Query routing starts at the sparse top layer, taking large geometric leaps toward the query,
 *   then progressively descends down to Layer 0 to fine-tune the local search.
 * This slashes query time from O(N) down to O(log N), returning Top-10 nearest neighbors
 * in single-digit milliseconds!
 */
class HNSW {
 public:
  static constexpr uint64_t kMagic = 0xdb484e535731ULL;  // "\xdbHNSW1"
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kHeaderSize = 64;

  explicit HNSW(const HNSWOptions& options);
  ~HNSW() = default;

  // Insert a new vector into the HNSW graph
  Status Insert(uint64_t node_id, const float* vector, size_t dim);

  // Approximate nearest neighbor search for Top K closest vectors
  Status Search(const float* query, size_t dim, size_t k,
                size_t ef_search, const DeleteBitmap* tombstones,
                std::vector<HNSWSearchResult>* results) const;

  // Convenience Search overload using default efSearch
  Status Search(const float* query, size_t dim, size_t k,
                const DeleteBitmap* tombstones,
                std::vector<HNSWSearchResult>* results) const {
    return Search(query, dim, k, options_.efSearch, tombstones, results);
  }

  // Persist graph to a verified binary snapshot file (<dbname>/index.hnsw)
  Status SaveSnapshot(Env* env, const std::string& snapshot_path) const;

  // Reconstruct graph from a binary snapshot file with CRC verification
  Status LoadSnapshot(Env* env, const std::string& snapshot_path);

  // Replay tail vector inserts appended after the snapshot was saved
  Status ReplayTail(uint64_t from_id, const std::vector<float>& tail_vectors, size_t dim);

  // Rebuild graph from scratch omitting deleted nodes to vacuum space
  Status Rebuild(const DeleteBitmap& tombstones, const std::vector<float>& vectors, size_t dim);

  // Number of indexed nodes
  uint64_t NodeCount() const { return num_nodes_; }
  uint32_t MaxLevel() const { return max_level_; }
  uint64_t EntryPoint() const { return enter_node_; }
  const HNSWOptions& options() const { return options_; }

 private:
  struct Node {
    uint64_t id{0};
    uint32_t level{0};
    // neighbors[l] is the list of neighbor node IDs at layer l (0 <= l <= level)
    std::vector<std::vector<uint64_t>> neighbors;
  };

  // Helper structures for layer search
  struct DistNode {
    float dist;
    uint64_t id;

    bool operator<(const DistNode& other) const {
      return dist < other.dist;
    }
    bool operator>(const DistNode& other) const {
      return dist > other.dist;
    }
  };

  uint32_t GetRandomLevel();
  float ComputeDistance(const float* a, const float* b) const;
  float ComputeDistanceToNode(const float* vec, uint64_t node_id) const;

  void SearchLayer(const float* query, const std::vector<uint64_t>& enter_points,
                   size_t ef, uint32_t level,
                   std::vector<DistNode>* out_candidates) const;

  void SelectNeighborsSimple(const float* query,
                             std::vector<DistNode>& candidates,
                             size_t max_m,
                             std::vector<uint64_t>* selected) const;

  HNSWOptions options_;
  double ml_{0.0};
  uint32_t max_level_{0};
  uint64_t enter_node_{0};
  uint64_t num_nodes_{0};
  bool has_enter_node_{false};

  std::mt19937 rng_;
  std::uniform_real_distribution<double> uniform_dist_{0.0, 1.0};

  // Node adjacency storage
  std::vector<Node> nodes_;
  // Flat vector storage for SIMD distance computations [num_nodes * dim]
  std::vector<float> flat_vectors_;
};

}  // namespace purpledb
