#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "core/embed/embedder.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Represents a coherent temporal scene / moment in a video.
 */
struct Moment {
  std::string video_id;
  std::string moment_id;
  double start_sec{0.0};
  double end_sec{0.0};
  std::vector<float> embedding;  // Unit L2-normalized centroid embedding [dim]
  int medoid_frame_index{0};     // Index of representative thumbnail frame
  ImageRef thumbnail;

  double duration() const {
    return (end_sec > start_sec) ? (end_sec - start_sec) : 0.0;
  }
};

struct MomentDetectorOptions {
  double threshold{0.90};          // Cosine similarity cutoff (default 0.90)
  double default_interval{1.0};    // Sampling frame interval in seconds
};

/**
 * @brief Anchor-based moment boundary clustering.
 *
 * Algorithmic Flow:
 * 1. The first frame of a moment establishes the anchor embedding.
 * 2. Subsequent frames are compared to the anchor via cosine similarity.
 * 3. While similarity >= threshold, frames are clustered into the active moment.
 * 4. When similarity drops below threshold, the active moment closes:
 *    - Computes L2-normalized centroid vector across cluster frames.
 *    - Finds the medoid frame (closest to centroid) to serve as thumbnail.
 * 5. A new moment initializes with the subsequent frame as the new anchor.
 */
class MomentDetector {
 public:
  explicit MomentDetector(const MomentDetectorOptions& options = MomentDetectorOptions{});

  Status DetectMoments(const std::string& video_id,
                      const std::vector<double>& timestamps,
                      const std::vector<ImageRef>& frames,
                      const std::vector<float>& embeddings,
                      size_t dim,
                      std::vector<Moment>* out_moments) const;

  double threshold() const { return options_.threshold; }
  void SetThreshold(double t) { options_.threshold = t; }

 private:
  MomentDetectorOptions options_;
};

}  // namespace purpledb
