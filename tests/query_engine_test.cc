#include "core/query/query_engine.h"
#include "core/embed/local_embedder.h"
#include "util/test_harness.h"

#include <vector>

namespace purpledb {
namespace test {

class QueryEngineTest {
 public:
  QueryEngineTest() : env_(Env::Default()) {
    test_dir_ = "./query_engine_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~QueryEngineTest() {
    std::vector<std::string> children;
    if (env_->GetChildren(test_dir_, &children).ok()) {
      for (const auto& f : children) {
        if (f != "." && f != "..") {
          env_->DeleteFile(test_dir_ + "/" + f);
        }
      }
      env_->DeleteDir(test_dir_);
    }
  }

  Env* env_;
  std::string test_dir_;
};

TEST(QueryEngineTest, BasicOpenAndSearch) {
  QueryEngineTest fixture;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

  QueryOptions q_opts;
  q_opts.vector_options.dim = 32;
  q_opts.vector_options.enable_hnsw = true;

  std::unique_ptr<QueryEngine> engine;
  ASSERT_STATUS_OK(QueryEngine::Open(fixture.test_dir_, embedder, q_opts, &engine));

  // Generate synthetic frames with chromatic differences
  std::vector<DecodedFrame> frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(15, 1.0, 32, 32, &frames));

  std::string video_id = "vid_alpha";
  size_t moments_committed = 0;
  ASSERT_STATUS_OK(engine->AddFrames(video_id, frames, &moments_committed));
  ASSERT_GT(moments_committed, 0u);

  // Search text
  std::vector<MomentHit> hits;
  QueryFilter filter;
  ASSERT_STATUS_OK(engine->SearchText("bright blue chromatic sky", 5, filter, &hits));
  ASSERT_FALSE(hits.empty());
  ASSERT_LE(hits.size(), 5u);

  for (const auto& h : hits) {
    ASSERT_EQ(h.video_id, video_id);
    ASSERT_GE(h.end_sec, h.start_sec);
  }

  // Check stats
  DatabaseStats stats;
  ASSERT_STATUS_OK(engine->GetStats(&stats));
  ASSERT_EQ(stats.num_vectors, moments_committed);
  ASSERT_EQ(stats.num_moments, moments_committed);
  ASSERT_GT(stats.disk_size_bytes, 0u);
}

TEST(QueryEngineTest, SearchByExample) {
  QueryEngineTest fixture;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

  std::unique_ptr<QueryEngine> engine;
  ASSERT_STATUS_OK(QueryEngine::Open(fixture.test_dir_, embedder, &engine));

  std::vector<DecodedFrame> frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(10, 1.0, 32, 32, &frames));

  std::string video_id = "vid_example";
  ASSERT_STATUS_OK(engine->AddFrames(video_id, frames));

  // Search with the first frame as the visual query
  std::vector<MomentHit> hits;
  QueryFilter filter;
  ASSERT_STATUS_OK(engine->SearchByExample(frames[0].image, 3, filter, &hits));
  ASSERT_FALSE(hits.empty());
  // The first hit should be from vid_example and score should be very high
  ASSERT_EQ(hits[0].video_id, video_id);
  ASSERT_GT(hits[0].score, 0.70f);
}

TEST(QueryEngineTest, MetadataFiltering) {
  QueryEngineTest fixture;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

  std::unique_ptr<QueryEngine> engine;
  ASSERT_STATUS_OK(QueryEngine::Open(fixture.test_dir_, embedder, &engine));

  // Ingest video 1 (10 frames)
  std::vector<DecodedFrame> frames1;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(10, 1.0, 32, 32, &frames1));
  ASSERT_STATUS_OK(engine->AddFrames("video_one", frames1));

  // Ingest video 2 (10 frames)
  std::vector<DecodedFrame> frames2;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(10, 1.0, 32, 32, &frames2));
  ASSERT_STATUS_OK(engine->AddFrames("video_two", frames2));

  // 1. Filter by video_id = "video_one"
  QueryFilter filter1;
  filter1.video_id = "video_one";
  std::vector<MomentHit> hits1;
  ASSERT_STATUS_OK(engine->SearchText("color", 10, filter1, &hits1));
  for (const auto& h : hits1) {
    ASSERT_EQ(h.video_id, "video_one");
  }

  // 2. Filter by video_id = "video_two"
  QueryFilter filter2;
  filter2.video_id = "video_two";
  std::vector<MomentHit> hits2;
  ASSERT_STATUS_OK(engine->SearchText("color", 10, filter2, &hits2));
  for (const auto& h : hits2) {
    ASSERT_EQ(h.video_id, "video_two");
  }

  // 3. Filter by temporal interval (start_sec >= 2.0, end_sec <= 6.0)
  QueryFilter filter_time;
  filter_time.start_sec = 2.0;
  filter_time.end_sec = 6.0;
  std::vector<MomentHit> hits_time;
  ASSERT_STATUS_OK(engine->SearchText("color", 10, filter_time, &hits_time));
  for (const auto& h : hits_time) {
    ASSERT_GE(h.end_sec, 2.0);
    ASSERT_LE(h.start_sec, 6.0);
  }
}

TEST(QueryEngineTest, DeleteVideoCascading) {
  QueryEngineTest fixture;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

  std::unique_ptr<QueryEngine> engine;
  ASSERT_STATUS_OK(QueryEngine::Open(fixture.test_dir_, embedder, &engine));

  std::vector<DecodedFrame> frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(12, 1.0, 32, 32, &frames));
  ASSERT_STATUS_OK(engine->AddFrames("video_doomed", frames));

  // Verify searchable
  std::vector<MomentHit> hits;
  QueryFilter filter;
  ASSERT_STATUS_OK(engine->SearchText("test", 5, filter, &hits));
  ASSERT_FALSE(hits.empty());

  // Delete the video
  ASSERT_STATUS_OK(engine->DeleteVideo("video_doomed"));

  // Verify no longer searchable
  std::vector<MomentHit> after_hits;
  ASSERT_STATUS_OK(engine->SearchText("test", 5, filter, &after_hits));
  ASSERT_TRUE(after_hits.empty());

  // Stats should reflect 0 active moments
  DatabaseStats stats;
  ASSERT_STATUS_OK(engine->GetStats(&stats));
  ASSERT_EQ(stats.num_moments, 0u);
  ASSERT_EQ(stats.num_vectors, 0u);
}

}  // namespace test
}  // namespace purpledb
