#include "core/embed/api_adapter.h"
#include "core/embed/local_embedder.h"
#include "core/vector/distance.h"
#include "util/test_harness.h"

#include <cmath>
#include <vector>

namespace purpledb {
namespace test {

class MockTransport : public HttpTransport {
 public:
  int calls_count{0};
  int fail_until_attempt{0};
  int fail_status_code{429};
  std::string retry_after_header;

  Response Post(const std::string&,
                const std::string&,
                const std::string&,
                int) override {
    calls_count++;
    Response r;
    if (calls_count <= fail_until_attempt) {
      r.status_code = fail_status_code;
      r.retry_after = retry_after_header;
      r.error_message = "Mock network error";
      return r;
    }
    r.status_code = 200;
    r.body = "{\"embeddings\":[[0.5, 0.5, 0.5, 0.5]]}";
    return r;
  }
};

TEST(EmbedderTest, LocalEmbedderInvariants) {
  LocalEmbedderOptions opts;
  opts.dim = 64;
  opts.model_id = "test/clip-64";
  LocalEmbedder embedder(opts);

  ASSERT_EQ(embedder.dim(), 64);
  ASSERT_EQ(embedder.model_id(), "test/clip-64");

  // Create two synthetic images (blue image and red image)
  std::vector<uint8_t> blue_pixels(10 * 10 * 3, 0);
  for (size_t i = 0; i < 100; ++i) {
    blue_pixels[i * 3 + 2] = 255;  // Blue channel
  }
  ImageRef blue_img = ImageRef::FromRgb(blue_pixels.data(), 10, 10, 3);

  std::vector<uint8_t> red_pixels(10 * 10 * 3, 0);
  for (size_t i = 0; i < 100; ++i) {
    red_pixels[i * 3 + 0] = 255;  // Red channel
  }
  ImageRef red_img = ImageRef::FromRgb(red_pixels.data(), 10, 10, 3);

  std::vector<float> matrix;
  Status s = embedder.EmbedImages({blue_img, red_img}, &matrix);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(matrix.size(), 2u * 64u);

  // Assert both vectors are unit normalized
  float norm_blue = VectorNorm(matrix.data(), 64);
  float norm_red = VectorNorm(matrix.data() + 64, 64);
  ASSERT_NEAR(norm_blue, 1.0f, 1e-4f);
  ASSERT_NEAR(norm_red, 1.0f, 1e-4f);

  // Embed matching text query
  std::vector<float> blue_text_vec;
  s = embedder.EmbedText("deep dark navy blue ocean water", &blue_text_vec);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(blue_text_vec.size(), 64u);
  ASSERT_NEAR(VectorNorm(blue_text_vec.data(), 64), 1.0f, 1e-4f);

  // Semantic alignment: Blue image and blue text should have high cosine similarity
  float blue_blue_sim = DotProduct(matrix.data(), blue_text_vec.data(), 64);
  float red_blue_sim = DotProduct(matrix.data() + 64, blue_text_vec.data(), 64);

  ASSERT_GT(blue_blue_sim, 0.40f);
  ASSERT_GT(blue_blue_sim, red_blue_sim);
}

TEST(EmbedderTest, ApiAdapterRateLimitAndBackoff) {
  auto mock_transport = std::make_shared<MockTransport>();
  mock_transport->fail_until_attempt = 2;  // Fail first 2 attempts with 429
  mock_transport->fail_status_code = 429;
  mock_transport->retry_after_header = "1";

  ApiAdapterOptions opts;
  opts.dim = 32;
  opts.max_retries = 3;
  opts.transport = mock_transport;

  ApiEmbedder embedder(opts);
  // Fast sleeper hook to avoid actual test delays
  int sleep_calls = 0;
  embedder.SetSleeper([&](int) { sleep_calls++; });

  std::vector<float> vec;
  Status s = embedder.EmbedText("sunny day on the beach", &vec);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(vec.size(), 32u);

  ApiAdapterStats stats = embedder.GetStats();
  ASSERT_EQ(stats.total_requests, 1u);
  ASSERT_EQ(stats.rate_limit_hits, 2u);
  ASSERT_EQ(stats.total_retries, 2u);
  ASSERT_EQ(stats.total_failures, 0u);
  ASSERT_EQ(sleep_calls, 2);
  ASSERT_EQ(mock_transport->calls_count, 3);
}

TEST(EmbedderTest, ApiAdapterMaxRetriesExceeded) {
  auto mock_transport = std::make_shared<MockTransport>();
  mock_transport->fail_until_attempt = 10;  // Always fail
  mock_transport->fail_status_code = 503;

  ApiAdapterOptions opts;
  opts.dim = 16;
  opts.max_retries = 2;
  opts.transport = mock_transport;

  ApiEmbedder embedder(opts);
  embedder.SetSleeper([](int) {});

  std::vector<float> vec;
  Status s = embedder.EmbedText("query", &vec);
  ASSERT_TRUE(s.IsIOError());

  ApiAdapterStats stats = embedder.GetStats();
  ASSERT_EQ(stats.total_failures, 1u);
  ASSERT_EQ(stats.total_retries, 2u);
}

TEST(EmbedderTest, ApiAdapterBatchChunking) {
  auto mock_transport = std::make_shared<MockTransport>();
  ApiAdapterOptions opts;
  opts.dim = 16;
  opts.max_batch = 10;
  opts.transport = mock_transport;

  ApiEmbedder embedder(opts);

  std::vector<ImageRef> images(25, ImageRef::FromFile("dummy.jpg"));
  std::vector<float> matrix;
  Status s = embedder.EmbedImages(images, &matrix);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(matrix.size(), 25u * 16u);

  // 25 images with max_batch=10 requires ceil(25 / 10) = 3 calls
  ASSERT_EQ(mock_transport->calls_count, 3);
  ASSERT_EQ(embedder.GetStats().total_images_embedded, 25u);
}

}  // namespace test
}  // namespace purpledb
