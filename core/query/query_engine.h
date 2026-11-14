#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/embed/embedder.h"
#include "core/pipeline/ingest_job.h"
#include "core/query/moment_metadata.h"
#include "core/vector/vector_index.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Search filter predicates across metadata dimensions.
 */
struct QueryFilter {
  std::string video_id;     // Optional video filter (empty string for any)
  double start_sec{-1.0};   // Optional time interval start (< 0 for unbounded)
  double end_sec{-1.0};     // Optional time interval end (< 0 for unbounded)
};

/**
 * @brief Represents a scored moment hit returned by multimodal query.
 */
struct MomentHit {
  std::string video_id;
  uint32_t moment_id{0};
  double start_sec{0.0};
  double end_sec{0.0};
  float score{0.0f};
  std::string thumbnail_path;
  std::string caption;

  double duration() const {
    return (end_sec > start_sec) ? (end_sec - start_sec) : 0.0;
  }
};

/**
 * @brief Global database statistics.
 */
struct DatabaseStats {
  uint64_t num_videos{0};
  uint64_t num_moments{0};
  uint64_t num_vectors{0};
  uint64_t disk_size_bytes{0};
};

/**
 * @brief Query engine configuration options.
 */
struct QueryOptions {
  VectorOptions vector_options;
  IngestOptions ingest_options;
};

/**
 * @brief Multimodal Query Engine.
 *
 * Implements:
 * - Natural language text search (projected into embedding space)
 * - Visual search by example (ImageRef query frame)
 * - Snapshot-isolated metadata candidate post-filtering (ADR-032)
 * - Cascade video deletion with O(1) tombstones and background vacuum (ADR-033)
 * - Thread-safe read/write concurrency during ingest (ADR-035)
 */
class QueryEngine {
 public:
  static Status Open(const std::string& db_path,
                     std::shared_ptr<Embedder> embedder,
                     const QueryOptions& options,
                     std::unique_ptr<QueryEngine>* out);

  static Status Open(const std::string& db_path,
                     std::shared_ptr<Embedder> embedder,
                     std::unique_ptr<QueryEngine>* out);

  ~QueryEngine();

  // Natural language query search
  Status SearchText(const std::string& query,
                    int top_k,
                    const QueryFilter& filter,
                    std::vector<MomentHit>* out_hits);

  // Visual query search (search by image example)
  Status SearchByExample(const ImageRef& image,
                         int top_k,
                         const QueryFilter& filter,
                         std::vector<MomentHit>* out_hits);

  // Ingest video from disk, assigning content-addressed video_id
  Status AddVideo(const std::string& video_path, std::string* out_video_id);

  // Ingest synthetic/in-memory frames (used for test suites)
  Status AddFrames(const std::string& video_id,
                   const std::vector<DecodedFrame>& frames,
                   size_t* out_moments_count = nullptr);

  // Delete video and cascade O(1) tombstones across vectors and LSM metadata
  Status DeleteVideo(const std::string& video_id);

  // Retrieve global engine statistics
  Status GetStats(DatabaseStats* out_stats);

  // Flush memory tables and sync to durable disk
  Status Sync();

  // Accessors
  VectorIndex* index() const { return index_.get(); }
  Embedder* embedder() const { return embedder_.get(); }
  const std::string& db_path() const { return db_path_; }

 private:
  QueryEngine(const std::string& db_path,
              std::shared_ptr<Embedder> embedder,
              const QueryOptions& options,
              std::unique_ptr<VectorIndex> index);

  Status FilterAndHydrate(const std::vector<SearchResult>& raw_candidates,
                          int top_k,
                          const QueryFilter& filter,
                          std::vector<MomentHit>* out_hits);

  std::string db_path_;
  std::shared_ptr<Embedder> embedder_;
  QueryOptions options_;
  std::unique_ptr<VectorIndex> index_;
  std::unique_ptr<IngestPipeline> pipeline_;
  mutable std::shared_mutex rw_mutex_;
};

}  // namespace purpledb
