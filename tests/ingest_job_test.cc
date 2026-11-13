#include "core/pipeline/ingest_job.h"
#include "core/embed/local_embedder.h"
#include "core/vector/vector_index.h"
#include "util/test_harness.h"

#include <vector>

namespace purpledb {
namespace test {

class IngestJobTest {
 public:
  IngestJobTest() : env_(Env::Default()) {
    test_dir_ = "./ingest_job_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~IngestJobTest() {
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

TEST(IngestJobTest, JobStateSerializationRoundTrip) {
  JobState state;
  state.version = 1;
  state.status = JobStatus::kRunning;
  state.last_committed_pts = 15000000;  // 15 seconds
  state.moments_count = 42;
  state.frames_processed = 120;
  state.total_duration_pts = 60000000;

  std::string bytes = state.Serialize();
  ASSERT_EQ(bytes.size(), JobState::kSerializedSize);

  JobState recovered;
  Status s = JobState::Deserialize(Slice(bytes), &recovered);
  ASSERT_STATUS_OK(s);

  ASSERT_EQ(recovered.version, 1u);
  ASSERT_EQ(recovered.status, JobStatus::kRunning);
  ASSERT_EQ(recovered.last_committed_pts, 15000000u);
  ASSERT_EQ(recovered.moments_count, 42u);
  ASSERT_EQ(recovered.frames_processed, 120u);
  ASSERT_EQ(recovered.total_duration_pts, 60000000u);

  // Checksum corruption rejection
  bytes[10] ^= 0x55;
  Status bad_s = JobState::Deserialize(Slice(bytes), &recovered);
  ASSERT_TRUE(bad_s.IsCorruption());
}

TEST(IngestJobTest, ResumableIngestWithSimulatedKill) {
  IngestJobTest fixture;
  VectorOptions v_opts;
  v_opts.dim = 32;
  v_opts.metric = MetricType::kCosine;
  v_opts.enable_hnsw = true;

  LocalEmbedderOptions emb_opts;
  emb_opts.dim = 32;
  LocalEmbedder embedder(emb_opts);

  std::string video_id = "test_video_123";
  std::vector<DecodedFrame> frames;
  ASSERT_STATUS_OK(FrameDecoder::GenerateSyntheticFrames(12, 1.0, 32, 32, &frames));

  size_t committed_moments_pass1 = 0;

  // 1. First execution: simulate crash (kill -9) after batch 0
  {
    std::unique_ptr<VectorIndex> index;
    ASSERT_STATUS_OK(VectorIndex::Open(v_opts, fixture.test_dir_, &index));

    IngestOptions ingest_opts;
    ingest_opts.batch_size = 4;
    // Abort after batch 0
    ingest_opts.fault_hook = [](size_t batch_idx, size_t) {
      return (batch_idx == 0); // abort after batch 0
    };

    IngestPipeline pipeline(index.get(), &embedder, ingest_opts);
    Status s = pipeline.IngestFrames(video_id, frames, &committed_moments_pass1);
    ASSERT_TRUE(s.IsIOError());  // Aborted by fault hook

    // Verify partial job state persisted in LSM
    JobState partial_job;
    ASSERT_STATUS_OK(pipeline.GetJobState(video_id, &partial_job));
    ASSERT_EQ(partial_job.status, JobStatus::kRunning);
    ASSERT_GT(partial_job.moments_count, 0u);
    ASSERT_GT(partial_job.last_committed_pts, 0u);
    committed_moments_pass1 = partial_job.moments_count;

    // Simulate process death
    index.reset();
  }

  // 2. Recovery execution: reopen database and resume ingest
  {
    std::unique_ptr<VectorIndex> recovered_index;
    ASSERT_STATUS_OK(VectorIndex::Open(v_opts, fixture.test_dir_, &recovered_index));

    // Notice: count before resume equals partial moments committed
    ASSERT_EQ(recovered_index->Count(), committed_moments_pass1);

    IngestOptions resume_opts;
    resume_opts.batch_size = 4;
    resume_opts.fault_hook = nullptr;  // No crash on pass 2

    IngestPipeline resume_pipeline(recovered_index.get(), &embedder, resume_opts);
    size_t final_moments_count = 0;
    Status s = resume_pipeline.IngestFrames(video_id, frames, &final_moments_count);
    ASSERT_STATUS_OK(s);
    ASSERT_GT(final_moments_count, committed_moments_pass1);

    // Verify job is now completed
    JobState final_job;
    ASSERT_STATUS_OK(resume_pipeline.GetJobState(video_id, &final_job));
    ASSERT_EQ(final_job.status, JobStatus::kCompleted);
    ASSERT_EQ(final_job.moments_count, final_moments_count);
    ASSERT_EQ(recovered_index->Count(), final_moments_count);

    // 3. Idempotency check: re-running on completed video does nothing and adds zero duplicates
    size_t idempotent_count = 0;
    s = resume_pipeline.IngestFrames(video_id, frames, &idempotent_count);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(idempotent_count, final_moments_count);
    ASSERT_EQ(recovered_index->Count(), final_moments_count);
  }
}

}  // namespace test
}  // namespace purpledb
