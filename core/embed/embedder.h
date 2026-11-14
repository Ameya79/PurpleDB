#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Representation of an image input for multimodal embedding.
 *
 * An ImageRef can either reference an image file on disk (e.g. temporary JPEG
 * extracted by ffmpeg) or in-memory RGB pixel bytes (width x height x channels).
 *
 * SYSTEMS CONCEPT:
 * In zero-copy systems programming, we avoid duplicating large pixel buffers.
 * ImageRef allows referencing raw decoded frame bytes or disk paths without
 * premature decoding or memory allocation.
 */
class ImageRef {
 public:
  ImageRef() : width_(0), height_(0), channels_(0), is_file_(false) {}

  static ImageRef FromFile(const std::string& file_path) {
    ImageRef ref;
    ref.file_path_ = file_path;
    ref.is_file_ = true;
    return ref;
  }

  static ImageRef FromRgb(const uint8_t* data, int width, int height, int channels = 3) {
    ImageRef ref;
    ref.width_ = width;
    ref.height_ = height;
    ref.channels_ = channels;
    ref.is_file_ = false;
    if (data && width > 0 && height > 0 && channels > 0) {
      size_t size = static_cast<size_t>(width * height * channels);
      ref.data_.assign(data, data + size);
    }
    return ref;
  }

  static ImageRef FromRgbMove(std::vector<uint8_t>&& data, int width, int height, int channels = 3) {
    ImageRef ref;
    ref.width_ = width;
    ref.height_ = height;
    ref.channels_ = channels;
    ref.is_file_ = false;
    ref.data_ = std::move(data);
    return ref;
  }

  bool is_file() const { return is_file_; }
  const std::string& file_path() const { return file_path_; }
  const uint8_t* data() const { return data_.data(); }
  size_t data_size() const { return data_.size(); }
  int width() const { return width_; }
  int height() const { return height_; }
  int channels() const { return channels_; }

 private:
  std::string file_path_;
  std::vector<uint8_t> data_;
  int width_{0};
  int height_{0};
  int channels_{0};
  bool is_file_{false};
};

/**
 * @brief Abstract interface for multimodal embedding models.
 *
 * Translates images and text queries into a shared high-dimensional vector space.
 *
 * ARCHITECTURAL CONCEPT:
 * Following Dependency Inversion (ADR-028), PurpleDB's storage engine and
 * query pipeline depend strictly on the abstract `Embedder` interface rather than
 * concrete inference libraries (ONNX Runtime, PyTorch, or vendor cloud APIs).
 * This enables:
 * 1. Offline, deterministic unit testing without multi-hundred megabyte model binaries.
 * 2. Pluggable inference backends (Local ONNX, Apple CoreML, or remote API endpoints).
 * 3. Strict dimension, metric, and model_id validation before indexing.
 */
class Embedder {
 public:
  virtual ~Embedder() = default;

  // Unique model identifier (e.g. "openai/clip-vit-base-patch32" or "google/siglip-base-patch16-224")
  virtual std::string model_id() const = 0;

  // Dimensionality of the produced vectors (e.g. 512 for CLIP ViT-B/32, 768 for ViT-L)
  virtual int dim() const = 0;

  // Maximum recommended batch size for image embedding
  virtual int max_batch() const = 0;

  // Generate unit L2-normalized embeddings for a batch of images.
  // Output matrix is flattened [images.size() * dim()].
  virtual Status EmbedImages(const std::vector<ImageRef>& images,
                             std::vector<float>* out_matrix) = 0;

  // Generate unit L2-normalized embedding for a text query.
  // Output vector has length dim().
  virtual Status EmbedText(const std::string& text,
                           std::vector<float>* out_vector) = 0;
};

}  // namespace purpledb
