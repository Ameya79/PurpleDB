#include "core/pipeline/ingest_job.h"

#include <cstring>
#include "core/query/moment_metadata.h"
#include "core/storage/crc32c.h"
#include "core/storage/db_impl.h"
#include "core/util/coding.h"

namespace purpledb {

// ---------------------------------------------------------------------------
// JobState Serialization (FORMATS.md Section 11)
// ---------------------------------------------------------------------------

std::string JobState::Serialize() const {
  std::string buf;
  buf.resize(kSerializedSize);
  char* p = &buf[0];

  EncodeFixed16(p, version); p += 2;
  *reinterpret_cast<uint8_t*>(p) = static_cast<uint8_t>(status); p += 1;
  EncodeFixed64(p, last_committed_pts); p += 8;
  EncodeFixed32(p, moments_count); p += 4;
  EncodeFixed32(p, frames_processed); p += 4;
  EncodeFixed64(p, total_duration_pts); p += 8;
  std::memset(p, 0, 12); p += 12;

  // Masked Castagnoli CRC-32C over bytes 0..38 (39 bytes)
  uint32_t crc = crc32c::Mask(crc32c::Value(buf.data(), 39));
  EncodeFixed32(p, crc);

  return buf;
}

Status JobState::Deserialize(Slice src, JobState* out) {
  if (src.size() < kSerializedSize) {
    return Status::Corruption("JobState: slice too short");
  }

  uint32_t expected_crc = DecodeFixed32(src.data() + 39);
  uint32_t actual_crc = crc32c::Mask(crc32c::Value(src.data(), 39));
  if (expected_crc != actual_crc) {
    return Status::Corruption("JobState: CRC checksum mismatch");
  }

  const char* p = src.data();
  out->version = DecodeFixed16(p); p += 2;
  out->status = static_cast<JobStatus>(*reinterpret_cast<const uint8_t*>(p)); p += 1;
  out->last_committed_pts = DecodeFixed64(p); p += 8;
  out->moments_count = DecodeFixed32(p); p += 4;
  out->frames_processed = DecodeFixed32(p); p += 4;
  out->total_duration_pts = DecodeFixed64(p);

  return Status::OK();
}

// ---------------------------------------------------------------------------
// IngestPipeline Coordinator
// ---------------------------------------------------------------------------

IngestPipeline::IngestPipeline(VectorIndex* index,
                               Embedder* embedder,
                               const IngestOptions& options)
    : index_(index), embedder_(embedder), options_(options) {
  MomentDetectorOptions md_opts;
  md_opts.threshold = options_.moment_threshold;
  md_opts.default_interval = 1.0 / (options_.fps > 0 ? options_.fps : 1.0);
  detector_ = MomentDetector(md_opts);

  FrameDecoderOptions fd_opts;
  fd_opts.fps = options_.fps;
  decoder_ = FrameDecoder(Env::Default(), fd_opts);
}

Status IngestPipeline::GetJobState(const std::string& video_id, JobState* out_state) {
  std::string job_key = "j" + video_id;
  std::string val;
  Status s = index_->db()->Get(Slice(job_key), &val);
  if (!s.ok()) {
    return s;
  }
  return JobState::Deserialize(Slice(val), out_state);
}

Status IngestPipeline::IngestVideo(const std::string& video_path,
                                   std::string* out_video_id,
                                   size_t* out_moments_count) {
  if (!out_video_id || !out_moments_count) {
    return Status::InvalidArgument("output pointers cannot be nullptr");
  }

  // 1. Content hashing for idempotent video identification
  std::string video_id;
  Status s = SHA256::HashFile(Env::Default(), video_path, &video_id);
  if (!s.ok()) {
    return s;
  }
  *out_video_id = video_id;

  // 2. Decode frames via isolated subprocess
  std::vector<DecodedFrame> frames;
  s = decoder_.ExtractFrames(video_path, &frames);
  if (!s.ok()) {
    return s;
  }

  return IngestFrames(video_id, frames, out_moments_count);
}

Status IngestPipeline::IngestFrames(const std::string& video_id,
                                    const std::vector<DecodedFrame>& frames,
                                    size_t* out_moments_count) {
  if (frames.empty()) {
    if (out_moments_count) *out_moments_count = 0;
    return Status::OK();
  }

  // Check existing job state
  JobState job;
  Status s = GetJobState(video_id, &job);
  if (s.ok() && job.status == JobStatus::kCompleted) {
    // Idempotent: already completed
    if (out_moments_count) *out_moments_count = job.moments_count;
    return Status::OK();
  }

  if (!s.ok()) {
    // Fresh job
    job.version = JobState::kVersion;
    job.status = JobStatus::kRunning;
    job.last_committed_pts = 0;
    job.moments_count = 0;
    job.frames_processed = 0;
    job.total_duration_pts = static_cast<uint64_t>(frames.back().timestamp_sec * 1e6);
  } else {
    // Resuming from partial state
    job.status = JobStatus::kRunning;
  }

  // Find index of first frame beyond last_committed_pts
  size_t start_idx = 0;
  if (job.last_committed_pts > 0) {
    double last_sec = static_cast<double>(job.last_committed_pts) / 1e6;
    while (start_idx < frames.size() && frames[start_idx].timestamp_sec <= last_sec) {
      start_idx++;
    }
  }

  size_t batch_size = std::max(size_t{1}, options_.batch_size);
  size_t batch_num = 0;

  for (size_t cur = start_idx; cur < frames.size(); cur += batch_size) {
    size_t end = std::min(frames.size(), cur + batch_size);

    std::vector<ImageRef> batch_images;
    std::vector<double> batch_timestamps;
    for (size_t i = cur; i < end; ++i) {
      batch_images.push_back(frames[i].image);
      batch_timestamps.push_back(frames[i].timestamp_sec);
    }

    // Embed batch
    std::vector<float> matrix;
    s = embedder_->EmbedImages(batch_images, &matrix);
    if (!s.ok()) {
      job.status = JobStatus::kFailed;
      index_->db()->Put(Slice("j" + video_id), Slice(job.Serialize()));
      return s;
    }

    // Detect moments in batch
    std::vector<Moment> batch_moments;
    s = detector_.DetectMoments(video_id, batch_timestamps, batch_images,
                               matrix, embedder_->dim(), &batch_moments);
    if (!s.ok()) {
      job.status = JobStatus::kFailed;
      index_->db()->Put(Slice("j" + video_id), Slice(job.Serialize()));
      return s;
    }

    // Commit moments and updated job record atomically
    for (const auto& m : batch_moments) {
      std::string m_key = video_id + ":m_" + std::to_string(job.moments_count);
      s = index_->Put(m_key, m.embedding.data(), embedder_->dim());
      if (!s.ok()) {
        job.status = JobStatus::kFailed;
        index_->db()->Put(Slice("j" + video_id), Slice(job.Serialize()));
        return s;
      }

      MomentMetadata meta;
      meta.video_id = video_id;
      meta.moment_id = job.moments_count;
      meta.start_sec = m.start_sec;
      meta.end_sec = m.end_sec;
      meta.thumbnail_path = "";
      meta.caption = "";
      index_->db()->Put(Slice("info:" + m_key), Slice(meta.Serialize()));

      job.moments_count++;
    }

    job.last_committed_pts = static_cast<uint64_t>(batch_timestamps.back() * 1e6);
    job.frames_processed += static_cast<uint32_t>(end - cur);

    // Save job state in LSM
    s = index_->db()->Put(Slice("j" + video_id), Slice(job.Serialize()));
    if (!s.ok()) {
      return s;
    }
    index_->Sync();

    // Check fault injection hook (for crash testing)
    if (options_.fault_hook && !options_.fault_hook(batch_num, batch_moments.size())) {
      return Status::IOError("Simulated crash via fault hook");
    }
    batch_num++;
  }

  // Mark completed
  job.status = JobStatus::kCompleted;
  s = index_->db()->Put(Slice("j" + video_id), Slice(job.Serialize()));
  if (!s.ok()) {
    return s;
  }
  index_->Sync();

  if (out_moments_count) {
    *out_moments_count = job.moments_count;
  }
  return Status::OK();
}

}  // namespace purpledb
