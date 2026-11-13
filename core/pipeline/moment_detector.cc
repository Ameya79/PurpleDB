#include "core/pipeline/moment_detector.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include "core/vector/distance.h"

namespace purpledb {

MomentDetector::MomentDetector(const MomentDetectorOptions& options)
    : options_(options) {}

Status MomentDetector::DetectMoments(const std::string& video_id,
                                    const std::vector<double>& timestamps,
                                    const std::vector<ImageRef>& frames,
                                    const std::vector<float>& embeddings,
                                    size_t dim,
                                    std::vector<Moment>* out_moments) const {
  if (out_moments == nullptr) {
    return Status::InvalidArgument("out_moments cannot be nullptr");
  }
  out_moments->clear();

  size_t n_frames = frames.size();
  if (n_frames == 0) {
    return Status::OK();
  }

  if (timestamps.size() != n_frames) {
    return Status::InvalidArgument("timestamps and frames count mismatch");
  }
  if (embeddings.size() != n_frames * dim) {
    return Status::InvalidArgument("embeddings size mismatch with frames and dim");
  }

  double interval = options_.default_interval;
  if (n_frames > 1) {
    double diff = timestamps[1] - timestamps[0];
    if (diff > 0.0) {
      interval = diff;
    }
  }

  std::vector<size_t> cluster_indices;
  cluster_indices.push_back(0);

  const float* anchor_emb = embeddings.data();

  auto finalize_cluster = [&](const std::vector<size_t>& indices,
                              size_t moment_idx) -> Moment {
    Moment m;
    m.video_id = video_id;
    m.moment_id = "m_" + std::to_string(moment_idx);
    m.start_sec = timestamps[indices.front()];
    m.end_sec = timestamps[indices.back()] + interval;

    // 1. Calculate centroid embedding across cluster
    m.embedding.assign(dim, 0.0f);
    for (size_t idx : indices) {
      const float* emb = embeddings.data() + idx * dim;
      for (size_t d = 0; d < dim; ++d) {
        m.embedding[d] += emb[d];
      }
    }
    NormalizeVector(m.embedding.data(), dim);

    // 2. Select medoid frame closest to the centroid
    float best_sim = -2.0f;
    size_t best_idx = indices.front();

    for (size_t idx : indices) {
      const float* emb = embeddings.data() + idx * dim;
      float sim = DotProduct(emb, m.embedding.data(), dim);
      if (sim > best_sim) {
        best_sim = sim;
        best_idx = idx;
      }
    }

    m.medoid_frame_index = static_cast<int>(best_idx);
    m.thumbnail = frames[best_idx];
    return m;
  };

  size_t moment_counter = 0;

  for (size_t i = 1; i < n_frames; ++i) {
    const float* curr_emb = embeddings.data() + i * dim;
    float sim = DotProduct(anchor_emb, curr_emb, dim);

    if (sim >= options_.threshold) {
      // Continue extending active cluster
      cluster_indices.push_back(i);
    } else {
      // Cut boundary: finalize active moment
      out_moments->push_back(finalize_cluster(cluster_indices, moment_counter++));

      // Reset active cluster with new anchor
      cluster_indices.clear();
      cluster_indices.push_back(i);
      anchor_emb = curr_emb;
    }
  }

  // Finalize terminal moment
  if (!cluster_indices.empty()) {
    out_moments->push_back(finalize_cluster(cluster_indices, moment_counter++));
  }

  return Status::OK();
}

}  // namespace purpledb
