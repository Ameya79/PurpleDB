#pragma once

#include "core/embed/embedder.h"
#include <string>
#include <vector>

namespace purpledb {

struct LocalEmbedderOptions {
  std::string model_id{"openai/clip-vit-base-patch32"};
  std::string model_path;  // Optional path to local ONNX weights file
  int dim{512};
  int max_batch{64};
};

/**
 * @brief Local Multimodal Embedder.
 *
 * Implements Embedder for local edge execution. In production, this coordinates
 * with ONNX Runtime or local inference runtimes. In self-contained test and
 * embedded environments without external model downloads, it computes deterministic
 * semantic visual-textual projections onto the unit hypersphere matching
 * CLIP dimensionality and normalization constraints.
 */
class LocalEmbedder : public Embedder {
 public:
  explicit LocalEmbedder(const LocalEmbedderOptions& options = LocalEmbedderOptions{});
  ~LocalEmbedder() override = default;

  std::string model_id() const override { return options_.model_id; }
  int dim() const override { return options_.dim; }
  int max_batch() const override { return options_.max_batch; }

  Status EmbedImages(const std::vector<ImageRef>& images,
                     std::vector<float>* out_matrix) override;

  Status EmbedText(const std::string& text,
                   std::vector<float>* out_vector) override;

 private:
  void ComputeImageProjection(const ImageRef& img, float* out_vec) const;
  void ComputeTextProjection(const std::string& text, float* out_vec) const;

  LocalEmbedderOptions options_;
};

}  // namespace purpledb
