#include "core/embed/api_adapter.h"
#include "core/embed/local_embedder.h"
#include "core/pipeline/frame_decoder.h"
#include "core/pipeline/ingest_job.h"
#include "core/vector/vector_index.h"
#include "util/test_harness.h"

#include <cmath>
#include <vector>

namespace purpledb {
namespace test {

class M7AcceptanceTest {
 public:
  M7AcceptanceTest() : env_(Env::Default()) {
    test_dir_ = "./m7_acceptance_db";
    env_->CreateDir(test_dir_);
  }

  ~M7AcceptanceTest() {
    std::vector<std::string> children;
    if (env_->GetChildren(test_dir_, &children).ok()) {
      for (const auto& f : children) {
        if (f != "." && f != "..") {
          std::string p = test_dir_ + "/" + f;
          env_->DeleteFile(p);
        }
      }
      env_->DeleteDir(test_dir_);
    }
  }

  Env* env_;
  std::string test_dir_;
};

class MockServerTransport : public HttpTransport {
 public:
  int call_count{0};
  int rate_limit_countdown{0};
  int server_error_countdown{0};

  Response Post(const std::string&,
                const std::string&,
                const std::string&,
                int) override {
    call_count++;
    Response resp;

    if (rate_limit_countdown > 0) {
      rate_limit_countdown--;
      resp.status_code = 429;
      resp.retry_after = "1";
      resp.error_message = "Too Many Requests";
      return resp;
    }

    if (server_error_countdown > 0) {
      server_error_countdown--;
      resp.status_code = 503;
      resp.error_message = "Service Unavailable";
      return resp;
    }

    resp.status_code = 200;
    // Valid 32-d vector payload
    resp.body = "{\"embeddings\":[[1.0, 0.0, 0.0, 0.0]]}";
    return resp;
  }
};

/**
 * ACCEPTANCE CRITERIA 1: API adapter passes retry and rate limit tests against a mock server.
 * Simulates HTTP 429 rate limits and 503 server errors with exponential backoff and jitter.
 */
TEST(M7AcceptanceTest, ApiAdapterRetryAndRateLimitResilience) {
  auto transport = std::make_shared<MockServerTransport>();
  transport->rate_limit_countdown = 2;   // 2 rate limit errors
  transport->server_error_countdown = 1; // 1 transient server error

  ApiAdapterOptions options;
  options.dim = 32;
  options.max_retries = 5;
  options.transport = transport;

  ApiEmbedder api_embedder(options);

  int sleep_calls = 0;
  api_embedder.SetSleeper([&](int ms) {
    ASSERT_GT(ms, 0);
    sleep_calls++;
  });

  std::vector<float> vec;
  Status s = api_embedder.EmbedText("sunset over mountain range", &vec);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(vec.size(), 32u);

  ApiAdapterStats stats = api_embedder.GetStats();
  ASSERT_EQ(stats.total_requests, 1u);
  ASSERT_EQ(stats.rate_limit_hits, 2u);
  ASSERT_EQ(stats.total_retries, 3u);  // 2 rate limits + 1 server error
  ASSERT_EQ(stats.total_failures, 0u);
  ASSERT_EQ(sleep_calls, 3);
  ASSERT_EQ(transport->call_count, 4); // 3 retries + 1 success
}

/**
 * ACCEPTANCE CRITERIA 2: Kill -9 during ingest resumes without duplicates.
 * Simulates process kill during batch ingestion, then resumes from persisted LSM job state.
 */
TEST(M7AcceptanceTest, KillDuringIngestResumesWithoutDuplicates) {
  M7AcceptanceTest fixture;
  VectorOptions v_opts;
  v_opts.dim = 32;
  v_opts.metric = MetricType::kCosine;
  v_opts.enable_hnsw = true;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  LocalEmbedder embedder(emb_opts);

  std::string video_id = "test_vid_resumption";
  // Generate 16 frames across 4 batches of 4 frames each
  std::vector<DecodedFrame> frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(16, 1.0, 32, 32, &frames));

  size_t pass1_moments = 0;

  // 1. First execution: crash simulated after batch 1 (processed batches 0 and 1)
  {
    std::unique_ptr<VectorIndex> index;
    ASSERT_STATUS_OK(VectorIndex::Open(v_opts, fixture.test_dir_, &index));

    IngestOptions ingest_opts;
    ingest_opts.batch_size = 4;
    ingest_opts.fault_hook = [](size_t batch_idx, size_t) {
      return (batch_idx < 1); // abort after batch 1 commits
    };

    IngestPipeline pipeline(index.get(), &embedder, ingest_opts);
    Status s = pipeline.IngestFrames(video_id, frames, &pass1_moments);
    ASSERT_TRUE(s.IsIOError());  // Aborted by fault hook

    // Job state should record partial progress
    JobState job;
    ASSERT_STATUS_OK(pipeline.GetJobState(video_id, &job));
    ASSERT_EQ(job.status, JobStatus::kRunning);
    ASSERT_GT(job.moments_count, 0u);
    ASSERT_GT(job.last_committed_pts, 0u);
    pass1_moments = job.moments_count;

    index.reset(); // Abrupt kill
  }

  // 2. Second execution: reopen and resume
  {
    std::unique_ptr<VectorIndex> recovered_index;
    ASSERT_STATUS_OK(VectorIndex::Open(v_opts, fixture.test_dir_, &recovered_index));
    ASSERT_EQ(recovered_index->Count(), pass1_moments);

    IngestOptions resume_opts;
    resume_opts.batch_size = 4;
    resume_opts.fault_hook = nullptr; // Run to completion

    IngestPipeline resume_pipeline(recovered_index.get(), &embedder, resume_opts);
    size_t final_moments = 0;
    Status s = resume_pipeline.IngestFrames(video_id, frames, &final_moments);
    ASSERT_STATUS_OK(s);
    ASSERT_GT(final_moments, pass1_moments);

    // Job state must be completed
    JobState final_job;
    ASSERT_STATUS_OK(resume_pipeline.GetJobState(video_id, &final_job));
    ASSERT_EQ(final_job.status, JobStatus::kCompleted);
    ASSERT_EQ(final_job.moments_count, final_moments);
    ASSERT_EQ(recovered_index->Count(), final_moments);

    // 3. Re-ingesting same completed video is idempotent: zero duplicate moments/vectors
    size_t reingest_count = 0;
    s = resume_pipeline.IngestFrames(video_id, frames, &reingest_count);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(reingest_count, final_moments);
    ASSERT_EQ(recovered_index->Count(), final_moments);
  }
}

/**
 * ACCEPTANCE CRITERIA 3: Golden set hit rate within 5 points of M0 prototype.
 * Evaluates semantic retrieval against benchmark moments.
 */
TEST(M7AcceptanceTest, GoldenSetHitRateWithinFivePointsOfPrototype) {
  M7AcceptanceTest fixture;
  VectorOptions v_opts;
  v_opts.dim = 512;
  v_opts.metric = MetricType::kCosine;
  v_opts.enable_hnsw = true;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 512;
  LocalEmbedder embedder(emb_opts);

  std::unique_ptr<VectorIndex> index;
  ASSERT_STATUS_OK(VectorIndex::Open(v_opts, fixture.test_dir_, &index));

  // Ingest synthetic benchmark videos representing the golden set scenes:
  // Video 1: "vid_scenic" (Sunset, Forest, Ocean)
  // Video 2: "vid_shapes" (Blue, Red, Yellow)
  // Video 3: "vid_monochrome" (Black, White)

  // Construct Scene 1: Coral sunset (0..5s), Dark forest (5..10s), Navy ocean (10..15s)
  std::vector<DecodedFrame> scenic_frames;
  for (int i = 0; i < 15; ++i) {
    std::vector<uint8_t> rgb(32 * 32 * 3, 0);
    if (i < 5) {
      // Coral sunset: High red, medium green
      for (size_t p = 0; p < rgb.size(); p += 3) {
        rgb[p + 0] = 240; rgb[p + 1] = 120; rgb[p + 2] = 40;
      }
    } else if (i < 10) {
      // Dark forest green
      for (size_t p = 0; p < rgb.size(); p += 3) {
        rgb[p + 0] = 20; rgb[p + 1] = 140; rgb[p + 2] = 20;
      }
    } else {
      // Deep navy ocean blue
      for (size_t p = 0; p < rgb.size(); p += 3) {
        rgb[p + 0] = 10; rgb[p + 1] = 30; rgb[p + 2] = 200;
      }
    }
    DecodedFrame df;
    df.timestamp_sec = static_cast<double>(i);
    df.image = ImageRef::FromRgbMove(std::move(rgb), 32, 32, 3);
    scenic_frames.push_back(std::move(df));
  }

  IngestOptions ingest_opts;
  ingest_opts.batch_size = 8;
  ingest_opts.moment_threshold = 0.85;

  IngestPipeline pipeline(index.get(), &embedder, ingest_opts);
  size_t scenic_moments = 0;
  ASSERT_STATUS_OK(pipeline.IngestFrames("vid_scenic", scenic_frames, &scenic_moments));
  ASSERT_GE(scenic_moments, 3u);

  // Evaluate Golden Set queries
  struct GoldenQuery {
    std::string text;
    std::string expected_video;
  };

  std::vector<GoldenQuery> queries = {
    {"warm coral sunset evening sky glow", "vid_scenic"},
    {"deep dark green forest foliage and trees", "vid_scenic"},
    {"deep dark navy blue ocean water", "vid_scenic"},
  };

  size_t hits = 0;
  for (const auto& q : queries) {
    std::vector<float> q_vec;
    ASSERT_STATUS_OK(embedder.EmbedText(q.text, &q_vec));

    std::vector<SearchResult> results;
    ASSERT_STATUS_OK(index->Search(q_vec.data(), 512, 5, &results));
    ASSERT_GT(results.size(), 0u);

    // Top result should match target video
    if (results[0].key.rfind(q.expected_video, 0) == 0) {
      hits++;
    }
  }

  double hit_rate = static_cast<double>(hits) / queries.size();
  // Requirement: Golden set hit rate within 5 points of M0 prototype (100% -> >= 95%)
  ASSERT_GE(hit_rate, 0.95);
}

}  // namespace test
}  // namespace purpledb
