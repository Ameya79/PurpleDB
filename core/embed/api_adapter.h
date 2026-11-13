#pragma once

#include "core/embed/embedder.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace purpledb {

/**
 * @brief Abstract HTTP transport for network operations.
 *
 * SYSTEM CONCEPT:
 * Decoupling the network transport via an interface allows the API adapter
 * to be tested thoroughly with deterministic mock servers (injecting 429
 * rate limits, network timeouts, and 503 errors) without requiring external
 * internet access or live API credits.
 */
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;

  struct Response {
    int status_code{200};
    std::string body;
    std::string retry_after;  // "Retry-After" header value (seconds)
    std::string error_message;
  };

  virtual Response Post(const std::string& endpoint,
                        const std::string& api_key,
                        const std::string& payload,
                        int timeout_ms) = 0;
};

// Operating modes for remote inference
enum class ApiMode {
  kDirectEmbedding,  // Multimodal endpoint embedding images and text into shared space
  kCaptionMode,      // Vision LLM generates caption, then text model embeds caption
};

struct ApiAdapterOptions {
  std::string model_id{"api/clip-vit-base-patch32"};
  std::string endpoint{"https://api.purpledb.ai/v1/embed"};
  std::string api_key;
  int dim{512};
  int max_batch{16};
  ApiMode mode{ApiMode::kDirectEmbedding};
  int max_retries{4};
  int initial_backoff_ms{50};
  int max_backoff_ms{2000};
  int timeout_ms{5000};
  std::shared_ptr<HttpTransport> transport;
};

struct ApiAdapterStats {
  uint64_t total_requests{0};
  uint64_t total_retries{0};
  uint64_t rate_limit_hits{0};
  uint64_t total_images_embedded{0};
  uint64_t total_texts_embedded{0};
  uint64_t total_failures{0};
};

/**
 * @brief Cloud API Embedder Adapter.
 *
 * Implements Embedder for remote REST / HTTP inference providers with:
 * 1. Automatic request batching bounded by max_batch.
 * 2. Exponential backoff with random jitter for transient failures (HTTP 5xx).
 * 3. Explicit HTTP 429 Rate Limit handling honoring Retry-After headers.
 * 4. Runtime operational statistics and retry telemetry.
 */
class ApiEmbedder : public Embedder {
 public:
  explicit ApiEmbedder(const ApiAdapterOptions& options);
  ~ApiEmbedder() override = default;

  std::string model_id() const override { return options_.model_id; }
  int dim() const override { return options_.dim; }
  int max_batch() const override { return options_.max_batch; }

  Status EmbedImages(const std::vector<ImageRef>& images,
                     std::vector<float>* out_matrix) override;

  Status EmbedText(const std::string& text,
                   std::vector<float>* out_vector) override;

  // Access operational metrics
  ApiAdapterStats GetStats() const;
  void ResetStats();

  // Sleeper function hook for testing without real clock delays
  void SetSleeper(std::function<void(int ms)> sleeper) {
    sleeper_ = std::move(sleeper);
  }

 private:
  Status ExecuteWithRetry(const std::string& payload,
                          std::string* out_response);

  int ComputeBackoffMs(int attempt, const std::string& retry_after);

  ApiAdapterOptions options_;
  mutable ApiAdapterStats stats_;
  std::function<void(int ms)> sleeper_;
  std::mt19937 rng_;
};

}  // namespace purpledb
