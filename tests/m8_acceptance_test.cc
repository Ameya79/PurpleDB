#include "purpledb/purpledb_c.h"
#include "core/query/query_engine.h"
#include "core/embed/local_embedder.h"
#include "util/test_harness.h"

#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

namespace purpledb {
namespace test {

class M8AcceptanceTest {
 public:
  M8AcceptanceTest() : env_(Env::Default()) {
    test_dir_ = "./m8_acceptance_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~M8AcceptanceTest() {
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

/**
 * @brief Acceptance Criterion 1: Complete end-to-end query workflow
 * executed EXCLUSIVELY through the stable C ABI (core/include/purpledb/purpledb_c.h).
 */
TEST(M8AcceptanceTest, CompleteWorkflowExclusivelyThroughCAbi) {
  M8AcceptanceTest fixture;

  // Verify ABI Version
  ASSERT_EQ(pdb_abi_version(), 1);

  // 1. Initialize options and open database via C ABI
  pdb_options_t options;
  pdb_options_init(&options);
  options.create_if_missing = 1;
  options.enable_hnsw = 1;

  pdb_t* db = nullptr;
  int rc = pdb_open_with_options(fixture.test_dir_.c_str(), &options, "local", &db);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(db != nullptr);

  // 2. Create a dummy .synth video file (recognized by FrameDecoder)
  std::string synth_video = fixture.test_dir_ + "/sample.synth";
  {
    std::ofstream ofs(synth_video);
    ofs << "synthetic_video_payload";
  }

  // 3. Ingest video via C ABI
  char video_id_buf[128];
  std::memset(video_id_buf, 0, sizeof(video_id_buf));
  rc = pdb_add_video(db, synth_video.c_str(), video_id_buf, sizeof(video_id_buf));
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_GT(std::strlen(video_id_buf), 0u);

  std::string video_id = video_id_buf;

  // 4. Search text via C ABI
  pdb_results_t* text_results = nullptr;
  rc = pdb_search_text(db, "chromatic gradient horizon", 5, nullptr, &text_results);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(text_results != nullptr);

  int hit_count = pdb_results_count(text_results);
  ASSERT_GT(hit_count, 0);

  for (int i = 0; i < hit_count; ++i) {
    const char* hit_vid = nullptr;
    uint32_t moment_id = 0;
    double start_sec = 0.0;
    double end_sec = 0.0;
    float score = 0.0f;
    const char* thumb = nullptr;
    const char* caption = nullptr;

    rc = pdb_results_get(text_results, i, &hit_vid, &moment_id,
                         &start_sec, &end_sec, &score, &thumb, &caption);
    ASSERT_EQ(rc, PDB_OK);
    ASSERT_EQ(std::string(hit_vid), video_id);
    ASSERT_GE(end_sec, start_sec);
  }
  pdb_results_destroy(text_results);

  // 5. Search by visual example image via C ABI
  std::vector<uint8_t> query_rgb(32 * 32 * 3, 120);
  pdb_results_t* img_results = nullptr;
  rc = pdb_search_image(db, query_rgb.data(), 32, 32, 3, 3, nullptr, &img_results);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(img_results != nullptr);
  ASSERT_GT(pdb_results_count(img_results), 0);
  pdb_results_destroy(img_results);

  // 6. Filtered search via C ABI
  pdb_filter_t filter;
  filter.video_id = video_id.c_str();
  filter.start_sec = 0.0;
  filter.end_sec = 20.0;

  pdb_results_t* filtered_results = nullptr;
  rc = pdb_search_text(db, "chromatic", 5, &filter, &filtered_results);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(filtered_results != nullptr);
  ASSERT_GT(pdb_results_count(filtered_results), 0);
  pdb_results_destroy(filtered_results);

  // 7. Inspect database statistics via C ABI
  pdb_stats_t stats;
  rc = pdb_get_stats(db, &stats);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_EQ(stats.num_videos, 1u);
  ASSERT_GT(stats.num_moments, 0u);
  ASSERT_GT(stats.disk_size_bytes, 0u);

  // 8. Delete video via C ABI
  rc = pdb_delete_video(db, video_id.c_str());
  ASSERT_EQ(rc, PDB_OK);

  // Verify moments no longer returned
  pdb_results_t* after_delete_results = nullptr;
  rc = pdb_search_text(db, "chromatic", 5, nullptr, &after_delete_results);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_EQ(pdb_results_count(after_delete_results), 0);
  pdb_results_destroy(after_delete_results);

  // Stats should now reflect 0 active moments
  rc = pdb_get_stats(db, &stats);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_EQ(stats.num_moments, 0u);

  // 9. Close and reopen to verify clean shutdown and restart
  pdb_close(db);

  pdb_t* reopened_db = nullptr;
  rc = pdb_open(fixture.test_dir_.c_str(), "local", &reopened_db);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(reopened_db != nullptr);

  rc = pdb_get_stats(reopened_db, &stats);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_EQ(stats.num_moments, 0u);

  pdb_close(reopened_db);
}

/**
 * @brief Acceptance Criterion 2: Concurrent readers during ingestion
 * verified clean under ThreadSanitizer (TSan).
 */
TEST(M8AcceptanceTest, ConcurrentReadersDuringIngestionUnderTSan) {
  M8AcceptanceTest fixture;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

  QueryOptions q_opts;
  q_opts.vector_options.dim = 32;
  q_opts.vector_options.enable_hnsw = true;

  std::unique_ptr<QueryEngine> engine;
  ASSERT_STATUS_OK(QueryEngine::Open(fixture.test_dir_, embedder, q_opts, &engine));

  // Pre-populate with base video
  std::vector<DecodedFrame> base_frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(8, 1.0, 32, 32, &base_frames));
  ASSERT_STATUS_OK(engine->AddFrames("base_video", base_frames));

  std::atomic<bool> stop_flag{false};
  std::atomic<int> read_queries_completed{0};
  std::atomic<int> write_batches_completed{0};

  // Background writer thread: continuously ingests batches of frames
  std::thread writer_thread([&]() {
    int batch_idx = 0;
    while (!stop_flag.load()) {
      std::vector<DecodedFrame> frames;
      if (FrameDecoder::GenerateSyntheticFrames(6, 1.0, 32, 32, &frames).ok()) {
        std::string vid = "concurrent_video_" + std::to_string(batch_idx++);
        Status s = engine->AddFrames(vid, frames);
        if (s.ok()) {
          write_batches_completed.fetch_add(1);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (batch_idx >= 25) break;  // 25 batches ingested
    }
  });

  // Multiple concurrent reader threads: querying text and images
  auto reader_func = [&](int reader_id) {
    std::vector<uint8_t> query_rgb(32 * 32 * 3, static_cast<uint8_t>(reader_id * 30));
    ImageRef query_img = ImageRef::FromRgb(query_rgb.data(), 32, 32, 3);

    while (!stop_flag.load()) {
      // 1. Text search
      std::vector<MomentHit> text_hits;
      QueryFilter qf;
      Status s = engine->SearchText("chromatic colors", 5, qf, &text_hits);
      (void)s;

      // 2. Visual example search
      std::vector<MomentHit> img_hits;
      s = engine->SearchByExample(query_img, 3, qf, &img_hits);
      (void)s;

      read_queries_completed.fetch_add(2);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back(reader_func, i);
  }

  // Wait for writer to finish all batches
  writer_thread.join();

  // Allow readers to run a bit more, then signal stop
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  stop_flag.store(true);

  for (auto& r : readers) {
    r.join();
  }

  // Verify progress
  ASSERT_GE(write_batches_completed.load(), 20);
  ASSERT_GE(read_queries_completed.load(), 50);

  // Final database state check
  DatabaseStats stats;
  ASSERT_STATUS_OK(engine->GetStats(&stats));
  ASSERT_GT(stats.num_moments, 0u);
  ASSERT_GT(stats.num_vectors, 0u);
}

}  // namespace test
}  // namespace purpledb
