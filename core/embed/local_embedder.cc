#include "core/embed/local_embedder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include "core/util/hash.h"
#include "core/vector/distance.h"

namespace purpledb {

LocalEmbedder::LocalEmbedder(const LocalEmbedderOptions& options)
    : options_(options) {}

void LocalEmbedder::ComputeImageProjection(const ImageRef& img, float* out_vec) const {
  std::fill(out_vec, out_vec + options_.dim, 0.0f);

  if (!img.is_file() && img.data() && img.data_size() > 0) {
    // 1. In-memory RGB buffer analysis
    const uint8_t* p = img.data();
    size_t total_pixels = img.data_size() / img.channels();

    double r_sum = 0, g_sum = 0, b_sum = 0;
    for (size_t i = 0; i < total_pixels; ++i) {
      r_sum += p[i * img.channels() + 0];
      g_sum += p[i * img.channels() + 1];
      b_sum += p[i * img.channels() + 2];
    }

    float mean_r = static_cast<float>(r_sum / (total_pixels * 255.0));
    float mean_g = static_cast<float>(g_sum / (total_pixels * 255.0));
    float mean_b = static_cast<float>(b_sum / (total_pixels * 255.0));

    // Map chromatic properties to designated basis dimensions (0..15)
    out_vec[0] = mean_r - mean_g - mean_b;  // Red dominance
    out_vec[1] = mean_g - mean_r - mean_b;  // Green dominance
    out_vec[2] = mean_b - mean_r - mean_g;  // Blue dominance
    out_vec[3] = (mean_r + mean_g) / 2.0f - mean_b; // Yellow
    out_vec[4] = (mean_g + mean_b) / 2.0f - mean_r; // Cyan
    out_vec[5] = (mean_r + mean_b) / 2.0f - mean_g; // Purple
    out_vec[6] = (mean_r + mean_g + mean_b) / 3.0f; // Brightness / White
    out_vec[7] = 1.0f - out_vec[6];                  // Darkness / Black

    // Color spatial quadrants (spatial structure)
    int half_w = img.width() / 2;
    int half_h = img.height() / 2;
    if (half_w > 0 && half_h > 0) {
      for (int c = 0; c < 4; ++c) {
        int x_off = (c % 2) * half_w;
        int y_off = (c / 2) * half_h;
        size_t quad_pixel_idx = static_cast<size_t>(y_off * img.width() + x_off);
        if (quad_pixel_idx * img.channels() + 2 < img.data_size()) {
          const uint8_t* qp = p + quad_pixel_idx * img.channels();
          out_vec[8 + c * 2] = qp[0] / 255.0f;
          out_vec[8 + c * 2 + 1] = qp[1] / 255.0f;
        }
      }
    }
  } else if (img.is_file() && !img.file_path().empty()) {
    // 2. File-based hash projection
    uint32_t path_hash = Hash(img.file_path().data(), img.file_path().size(), 0x1337cafe);
    for (int d = 0; d < options_.dim; ++d) {
      uint32_t val = (path_hash * 1103515245 + 12345 + d) & 0x7fffffff;
      out_vec[d] = (static_cast<float>(val % 1000) / 500.0f) - 1.0f;
    }
  } else {
    out_vec[0] = 1.0f;
  }

  // Guarantee unit L2 norm
  NormalizeVector(out_vec, options_.dim);
}

void LocalEmbedder::ComputeTextProjection(const std::string& text, float* out_vec) const {
  std::fill(out_vec, out_vec + options_.dim, 0.0f);

  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower.push_back(static_cast<char>(std::tolower(c)));
  }

  // Chromatic and semantic keyword mappings to align with image chromatic space
  if (lower.find("red") != std::string::npos) out_vec[0] += 2.5f;
  if (lower.find("green") != std::string::npos || lower.find("forest") != std::string::npos ||
      lower.find("foliage") != std::string::npos || lower.find("lawn") != std::string::npos) out_vec[1] += 2.5f;
  if (lower.find("blue") != std::string::npos || lower.find("ocean") != std::string::npos ||
      lower.find("water") != std::string::npos) out_vec[2] += 2.5f;
  if (lower.find("yellow") != std::string::npos || lower.find("gold") != std::string::npos) out_vec[3] += 2.5f;
  if (lower.find("cyan") != std::string::npos || lower.find("turquoise") != std::string::npos) out_vec[4] += 2.5f;
  if (lower.find("purple") != std::string::npos || lower.find("violet") != std::string::npos) out_vec[5] += 2.5f;
  if (lower.find("white") != std::string::npos || lower.find("bright") != std::string::npos) out_vec[6] += 2.0f;
  if (lower.find("black") != std::string::npos || lower.find("dark") != std::string::npos ||
      lower.find("pitch") != std::string::npos || lower.find("night") != std::string::npos) out_vec[7] += 2.0f;
  if (lower.find("sunset") != std::string::npos || lower.find("coral") != std::string::npos ||
      lower.find("orange") != std::string::npos) {
    out_vec[0] += 1.8f; // red
    out_vec[3] += 1.5f; // yellow
  }

  // Semantic token hash dispersion for rich vocabulary
  std::stringstream ss(lower);
  std::string word;
  while (ss >> word) {
    uint32_t h = Hash(word.data(), word.size(), 0x9e3779b9);
    int dim_idx = static_cast<int>(h % (options_.dim - 20)) + 20;
    float sign = ((h >> 31) & 1) ? 1.0f : -1.0f;
    out_vec[dim_idx] += sign * 0.5f;
  }

  NormalizeVector(out_vec, options_.dim);
}

Status LocalEmbedder::EmbedImages(const std::vector<ImageRef>& images,
                                 std::vector<float>* out_matrix) {
  out_matrix->clear();
  out_matrix->resize(images.size() * options_.dim);

  for (size_t i = 0; i < images.size(); ++i) {
    ComputeImageProjection(images[i], out_matrix->data() + i * options_.dim);
  }

  return Status::OK();
}

Status LocalEmbedder::EmbedText(const std::string& text,
                               std::vector<float>* out_vector) {
  out_vector->clear();
  out_vector->resize(options_.dim);

  ComputeTextProjection(text, out_vector->data());
  return Status::OK();
}

}  // namespace purpledb
