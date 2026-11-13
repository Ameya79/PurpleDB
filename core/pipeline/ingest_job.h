#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "core/embed/embedder.h"
#include "core/pipeline/frame_decoder.h"
#include "core/pipeline/moment_detector.h"
#include "core/storage/crc32c.h"
#include "core/util/coding.h"
#include "core/util/sha256.h"
#include "core/vector/vector_index.h"
#include "purpledb/status.h"

namespace purpledb {

// Ingest job execution status
enum class JobStatus : uint8_t {
  kPending = 0,
  kRunning = 1,
  kCompleted = 2,
  kFailed = 3,
};

inline std::ostream& operator<<(std::ostream& os, JobStatus status) {
  return os << static_cast<int>(status);
}

/**
 * @brief Resumable Ingest Job State (FORMATS.md Section 11).
 *
 * Persisted in the LSM engine under key prefix 'j' + video_id.
 */
struct JobState {
  static constexpr uint16_t kVersion = 1;
  static constexpr size_t kSerializedSize = 43;

  uint16_t version{kVersion};
  JobStatus status{JobStatus::kPending};
  uint64_t last_committed_pts{0};  // Microseconds offset
  uint32_t moments_count{0};
  uint32_t frames_processed{0};
  uint64_t total_duration_pts{0};

  std::string Serialize() const;
  static Status Deserialize(Slice src, JobState* out);
};

struct IngestOptions {
  double fps{1.0};
  double moment_threshold{0.90};
  size_t batch_size{16};
  // Fault injection hook: if returns false during batch N, simulates process kill
  std::function<bool(size_t batch_idx, size_t moments_in_batch)> fault_hook;
};

/**
 * @brief End-to-End Ingest Pipeline Coordinator.
 *
 * Coordinates content hashing, frame extraction, batch embedding,
 * moment boundary clustering, and atomic LSM + vector commits with
 * crash resumption.
 */
class IngestPipeline {
 public:
  IngestPipeline(VectorIndex* index,
                 Embedder* embedder,
                 const IngestOptions& options = IngestOptions{});

  // Execute ingestion for a video file, resuming from previous job state if present
  Status IngestVideo(const std::string& video_path,
                     std::string* out_video_id,
                     size_t* out_moments_count);

  // Ingest synthetic/in-memory frames for tests
  Status IngestFrames(const std::string& video_id,
                      const std::vector<DecodedFrame>& frames,
                      size_t* out_moments_count);

  // Retrieve current job status for a video
  Status GetJobState(const std::string& video_id, JobState* out_state);

 private:
  VectorIndex* index_;
  Embedder* embedder_;
  IngestOptions options_;
  FrameDecoder decoder_;
  MomentDetector detector_;
};

}  // namespace purpledb
