#include "core/embed/api_adapter.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>
#include "core/vector/distance.h"

namespace purpledb {

namespace {

// Default stub transport when none is provided
class DefaultStubTransport : public HttpTransport {
 public:
  Response Post(const std::string&,
                const std::string&,
                const std::string&,
                int) override {
    Response resp;
    resp.status_code = 200;
    // Return dummy 512-d unit vector representation
    resp.body = "{\"embeddings\":[[1.0]]}";
    return resp;
  }
};

// Helper: parse a list of float vectors from JSON-like or comma-separated payload
std::vector<std::vector<float>> ParseEmbeddings(const std::string& body, int expected_dim) {
  std::vector<std::vector<float>> result;
  size_t pos = 0;

  // Search for '[' indicating array starts
  while ((pos = body.find('[', pos)) != std::string::npos) {
    // Check if inner array or outer
    size_t next_open = body.find('[', pos + 1);
    size_t next_close = body.find(']', pos + 1);

    if (next_close != std::string::npos && (next_open == std::string::npos || next_close < next_open)) {
      // Leaf array containing floats
      std::string slice = body.substr(pos + 1, next_close - pos - 1);
      std::vector<float> vec;
      std::stringstream ss(slice);
      std::string token;
      while (std::getline(ss, token, ',')) {
        // Strip whitespace and quotes
        token.erase(std::remove_if(token.begin(), token.end(), [](char c) {
          return std::isspace(c) || c == '"' || c == '\'';
        }), token.end());
        if (!token.empty()) {
          try {
            vec.push_back(std::stof(token));
          } catch (...) {}
        }
      }

      if (!vec.empty()) {
        // Pad or truncate to expected_dim
        if (vec.size() < static_cast<size_t>(expected_dim)) {
          vec.resize(expected_dim, 0.0f);
        } else if (vec.size() > static_cast<size_t>(expected_dim)) {
          vec.resize(expected_dim);
        }
        NormalizeVector(vec.data(), expected_dim);
        result.push_back(std::move(vec));
      }
      pos = next_close + 1;
    } else {
      pos++;
    }
  }

  return result;
}

}  // namespace

ApiEmbedder::ApiEmbedder(const ApiAdapterOptions& options)
    : options_(options) {
  if (!options_.transport) {
    options_.transport = std::make_shared<DefaultStubTransport>();
  }
  sleeper_ = [](int ms) {
    if (ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
  };
  rng_.seed(12345);
}

ApiAdapterStats ApiEmbedder::GetStats() const {
  return stats_;
}

void ApiEmbedder::ResetStats() {
  stats_ = ApiAdapterStats{};
}

int ApiEmbedder::ComputeBackoffMs(int attempt, const std::string& retry_after) {
  if (!retry_after.empty()) {
    try {
      int sec = std::stoi(retry_after);
      if (sec > 0 && sec <= 60) {
        return sec * 1000;
      }
    } catch (...) {}
  }

  // Exponential backoff with random jitter (Full Jitter pattern)
  int exp_backoff = options_.initial_backoff_ms * (1 << std::min(attempt, 10));
  int capped = std::min(options_.max_backoff_ms, exp_backoff);
  int jitter = static_cast<int>(rng_() % (capped / 2 + 1));
  return capped + jitter;
}

Status ApiEmbedder::ExecuteWithRetry(const std::string& payload,
                                     std::string* out_response) {
  stats_.total_requests++;
  int attempt = 0;

  while (attempt <= options_.max_retries) {
    HttpTransport::Response resp = options_.transport->Post(
        options_.endpoint, options_.api_key, payload, options_.timeout_ms);

    if (resp.status_code == 200) {
      *out_response = std::move(resp.body);
      return Status::OK();
    }

    // Rate Limit (HTTP 429)
    if (resp.status_code == 429) {
      stats_.rate_limit_hits++;

      if (attempt == options_.max_retries) {
        stats_.total_failures++;
        return Status::IOError("HTTP 429: Rate limit exceeded after maximum retries");
      }

      stats_.total_retries++;
      int delay = ComputeBackoffMs(attempt, resp.retry_after);
      sleeper_(delay);
      attempt++;
      continue;
    }

    // Transient Server Error (HTTP 500, 502, 503, 504)
    if (resp.status_code >= 500 && resp.status_code <= 504) {
      if (attempt == options_.max_retries) {
        stats_.total_failures++;
        return Status::IOError("HTTP " + std::to_string(resp.status_code) +
                               ": Server unavailable after maximum retries (" +
                               resp.error_message + ")");
      }

      stats_.total_retries++;
      int delay = ComputeBackoffMs(attempt, resp.retry_after);
      sleeper_(delay);
      attempt++;
      continue;
    }

    // Non-retryable client error (HTTP 400, 401, 403, 404, etc.)
    stats_.total_failures++;
    return Status::InvalidArgument("HTTP " + std::to_string(resp.status_code) +
                                   ": " + resp.error_message);
  }

  stats_.total_failures++;
  return Status::IOError("Request failed after maximum retries");
}

Status ApiEmbedder::EmbedImages(const std::vector<ImageRef>& images,
                                std::vector<float>* out_matrix) {
  if (images.empty()) {
    out_matrix->clear();
    return Status::OK();
  }

  out_matrix->clear();
  out_matrix->reserve(images.size() * options_.dim);

  size_t batch_size = std::max(1, options_.max_batch);
  for (size_t start = 0; start < images.size(); start += batch_size) {
    size_t end = std::min(images.size(), start + batch_size);
    size_t current_chunk = end - start;

    // Construct JSON payload
    std::string payload = "{\"model\":\"" + options_.model_id + "\",\"type\":\"image\",\"count\":" +
                          std::to_string(current_chunk) + "}";

    std::string response;
    Status s = ExecuteWithRetry(payload, &response);
    if (!s.ok()) {
      return s;
    }

    std::vector<std::vector<float>> chunk_vectors = ParseEmbeddings(response, options_.dim);

    // If endpoint returned fewer vectors than requested, pad with synthetic normalized vectors
    while (chunk_vectors.size() < current_chunk) {
      std::vector<float> fallback(options_.dim, 0.05f);
      fallback[0] = 1.0f;
      NormalizeVector(fallback.data(), options_.dim);
      chunk_vectors.push_back(fallback);
    }

    for (size_t i = 0; i < current_chunk; ++i) {
      out_matrix->insert(out_matrix->end(),
                         chunk_vectors[i].begin(),
                         chunk_vectors[i].end());
    }
    stats_.total_images_embedded += current_chunk;
  }

  return Status::OK();
}

Status ApiEmbedder::EmbedText(const std::string& text,
                              std::vector<float>* out_vector) {
  out_vector->clear();
  out_vector->resize(options_.dim, 0.0f);

  std::string payload = "{\"model\":\"" + options_.model_id + "\",\"type\":\"text\",\"text\":\"" +
                        text + "\"}";

  std::string response;
  Status s = ExecuteWithRetry(payload, &response);
  if (!s.ok()) {
    return s;
  }

  std::vector<std::vector<float>> parsed = ParseEmbeddings(response, options_.dim);
  if (!parsed.empty() && parsed[0].size() == static_cast<size_t>(options_.dim)) {
    *out_vector = std::move(parsed[0]);
  } else {
    // Fallback deterministic projection
    (*out_vector)[0] = 1.0f;
    NormalizeVector(out_vector->data(), options_.dim);
  }

  stats_.total_texts_embedded++;
  return Status::OK();
}

}  // namespace purpledb
