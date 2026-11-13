#include "core/query/query_engine.h"

#include <algorithm>
#include <iostream>
#include "core/pipeline/ingest_job.h"
#include "core/storage/db_impl.h"

namespace purpledb {

QueryEngine::QueryEngine(const std::string& db_path,
                         std::shared_ptr<Embedder> embedder,
                         const QueryOptions& options,
                         std::unique_ptr<VectorIndex> index)
    : db_path_(db_path),
      embedder_(std::move(embedder)),
      options_(options),
      index_(std::move(index)) {
  pipeline_ = std::make_unique<IngestPipeline>(index_.get(), embedder_.get(),
                                               options_.ingest_options);
}

QueryEngine::~QueryEngine() = default;

Status QueryEngine::Open(const std::string& db_path,
                         std::shared_ptr<Embedder> embedder,
                         const QueryOptions& options,
                         std::unique_ptr<QueryEngine>* out) {
  if (!embedder) {
    return Status::InvalidArgument("Embedder cannot be nullptr");
  }
  if (!out) {
    return Status::InvalidArgument("out cannot be nullptr");
  }

  VectorOptions v_opts = options.vector_options;
  v_opts.dim = static_cast<uint32_t>(embedder->dim());
  v_opts.model_id = embedder->model_id();

  std::unique_ptr<VectorIndex> index;
  Status s = VectorIndex::Open(v_opts, db_path, &index);
  if (!s.ok()) {
    return s;
  }

  *out = std::unique_ptr<QueryEngine>(
      new QueryEngine(db_path, embedder, options, std::move(index)));
  return Status::OK();
}

Status QueryEngine::Open(const std::string& db_path,
                         std::shared_ptr<Embedder> embedder,
                         std::unique_ptr<QueryEngine>* out) {
  return Open(db_path, std::move(embedder), QueryOptions{}, out);
}

Status QueryEngine::SearchText(const std::string& query,
                               int top_k,
                               const QueryFilter& filter,
                               std::vector<MomentHit>* out_hits) {
  if (!out_hits) return Status::InvalidArgument("out_hits cannot be nullptr");
  out_hits->clear();
  if (top_k <= 0) return Status::OK();

  std::shared_lock<std::shared_mutex> lock(rw_mutex_);

  if (index_->Count() == 0) {
    return Status::OK();
  }

  std::vector<float> query_vec;
  Status s = embedder_->EmbedText(query, &query_vec);
  if (!s.ok()) return s;

  // Determine over-fetch factor to satisfy metadata predicates
  size_t overfetch = (!filter.video_id.empty() || filter.start_sec >= 0.0 || filter.end_sec >= 0.0) ? 8 : 2;
  size_t candidate_k = std::max(size_t{32}, static_cast<size_t>(top_k) * overfetch);

  std::vector<SearchResult> raw_candidates;
  s = index_->Search(query_vec.data(), embedder_->dim(), candidate_k, &raw_candidates);
  if (!s.ok()) return s;

  return FilterAndHydrate(raw_candidates, top_k, filter, out_hits);
}

Status QueryEngine::SearchByExample(const ImageRef& image,
                                    int top_k,
                                    const QueryFilter& filter,
                                    std::vector<MomentHit>* out_hits) {
  if (!out_hits) return Status::InvalidArgument("out_hits cannot be nullptr");
  out_hits->clear();
  if (top_k <= 0) return Status::OK();

  std::shared_lock<std::shared_mutex> lock(rw_mutex_);

  if (index_->Count() == 0) {
    return Status::OK();
  }

  std::vector<ImageRef> batch = {image};
  std::vector<float> query_vec;
  Status s = embedder_->EmbedImages(batch, &query_vec);
  if (!s.ok()) return s;

  size_t overfetch = (!filter.video_id.empty() || filter.start_sec >= 0.0 || filter.end_sec >= 0.0) ? 8 : 2;
  size_t candidate_k = std::max(size_t{32}, static_cast<size_t>(top_k) * overfetch);

  std::vector<SearchResult> raw_candidates;
  s = index_->Search(query_vec.data(), embedder_->dim(), candidate_k, &raw_candidates);
  if (!s.ok()) return s;

  return FilterAndHydrate(raw_candidates, top_k, filter, out_hits);
}

Status QueryEngine::FilterAndHydrate(const std::vector<SearchResult>& raw_candidates,
                                     int top_k,
                                     const QueryFilter& filter,
                                     std::vector<MomentHit>* out_hits) {
  // Snapshot isolation: query metadata under a frozen snapshot (ADR-032)
  const Snapshot* snap = index_->db()->GetSnapshot();
  ReadOptions ro;
  ro.snapshot = snap;

  std::vector<MomentHit> hits;

  for (const auto& cand : raw_candidates) {
    std::string info_key = "info:" + cand.key;
    std::string info_val;
    Status s = index_->db()->Get(ro, Slice(info_key), &info_val);

    MomentHit hit;
    hit.score = cand.score;

    if (s.ok()) {
      MomentMetadata meta;
      if (MomentMetadata::Deserialize(Slice(info_val), &meta).ok()) {
        if (!filter.video_id.empty() && meta.video_id != filter.video_id) {
          continue;
        }
        if (filter.start_sec >= 0.0 && meta.end_sec < filter.start_sec) {
          continue;
        }
        if (filter.end_sec >= 0.0 && meta.start_sec > filter.end_sec) {
          continue;
        }
        hit.video_id = meta.video_id;
        hit.moment_id = meta.moment_id;
        hit.start_sec = meta.start_sec;
        hit.end_sec = meta.end_sec;
        hit.thumbnail_path = meta.thumbnail_path;
        hit.caption = meta.caption;
      } else {
        size_t colon = cand.key.find(':');
        hit.video_id = (colon != std::string::npos) ? cand.key.substr(0, colon) : cand.key;
        if (!filter.video_id.empty() && hit.video_id != filter.video_id) continue;
      }
    } else {
      // Fallback: parse user key format <video_id>:m_<moment_id>
      size_t colon = cand.key.find(':');
      hit.video_id = (colon != std::string::npos) ? cand.key.substr(0, colon) : cand.key;
      if (!filter.video_id.empty() && hit.video_id != filter.video_id) continue;
      if (colon != std::string::npos && cand.key.size() > colon + 3 && cand.key.substr(colon + 1, 2) == "m_") {
        hit.moment_id = static_cast<uint32_t>(std::stoul(cand.key.substr(colon + 3)));
      }
    }

    hits.push_back(std::move(hit));
    if (static_cast<int>(hits.size()) >= top_k) {
      break;
    }
  }

  index_->db()->ReleaseSnapshot(snap);
  *out_hits = std::move(hits);
  return Status::OK();
}

Status QueryEngine::AddVideo(const std::string& video_path, std::string* out_video_id) {
  if (!out_video_id) return Status::InvalidArgument("out_video_id cannot be nullptr");

  std::unique_lock<std::shared_mutex> lock(rw_mutex_);

  size_t moments_count = 0;
  Status s = pipeline_->IngestVideo(video_path, out_video_id, &moments_count);
  if (!s.ok()) return s;

  index_->Sync();
  return Status::OK();
}

Status QueryEngine::AddFrames(const std::string& video_id,
                              const std::vector<DecodedFrame>& frames,
                              size_t* out_moments_count) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);

  size_t count = 0;
  Status s = pipeline_->IngestFrames(video_id, frames, &count);
  if (!s.ok()) return s;

  if (out_moments_count) {
    *out_moments_count = count;
  }

  index_->Sync();
  return Status::OK();
}

Status QueryEngine::DeleteVideo(const std::string& video_id) {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);

  // Scan LSM for all moment keys belonging to this video
  std::string prefix = "m" + video_id + ":";
  std::unique_ptr<Iterator> it(index_->db()->NewIterator(ReadOptions()));
  std::vector<std::string> keys_to_delete;

  for (it->Seek(Slice(prefix)); it->Valid(); it->Next()) {
    Slice k = it->key();
    if (k.size() < prefix.size() || k.ToString().substr(0, prefix.size()) != prefix) {
      break;
    }
    // k is "m" + user_key; extract user_key
    keys_to_delete.push_back(k.ToString().substr(1));
  }

  for (const auto& key : keys_to_delete) {
    index_->Delete(key);
    index_->db()->Delete(WriteOptions(), Slice("info:" + key));
  }

  // Delete job state
  index_->db()->Delete(WriteOptions(), Slice("j" + video_id));
  index_->Sync();

  // If deleted fraction exceeds 20%, trigger index rebuild vacuum (ADR-033)
  if (index_->DeletedFraction() > 0.20) {
    index_->RebuildIndex();
  }

  return Status::OK();
}

Status QueryEngine::GetStats(DatabaseStats* out_stats) {
  if (!out_stats) return Status::InvalidArgument("out_stats cannot be nullptr");

  std::shared_lock<std::shared_mutex> lock(rw_mutex_);

  out_stats->num_moments = index_->Count();
  out_stats->num_vectors = index_->Count();

  // Count unique completed videos from 'j' job records
  uint64_t vid_count = 0;
  std::unique_ptr<Iterator> it(index_->db()->NewIterator(ReadOptions()));
  for (it->Seek(Slice("j")); it->Valid(); it->Next()) {
    Slice k = it->key();
    if (k.empty() || k[0] != 'j') break;
    JobState js;
    if (JobState::Deserialize(it->value(), &js).ok() && js.status == JobStatus::kCompleted) {
      vid_count++;
    }
  }
  out_stats->num_videos = vid_count;

  // Measure total disk size
  uint64_t total_size = 0;
  std::vector<std::string> children;
  Env* env = options_.vector_options.kv_options.env ? options_.vector_options.kv_options.env : Env::Default();
  if (env->GetChildren(db_path_, &children).ok()) {
    for (const auto& child : children) {
      if (child != "." && child != "..") {
        uint64_t fsz = 0;
        env->GetFileSize(db_path_ + "/" + child, &fsz);
        total_size += fsz;
      }
    }
  }
  out_stats->disk_size_bytes = total_size;

  return Status::OK();
}

Status QueryEngine::Sync() {
  std::unique_lock<std::shared_mutex> lock(rw_mutex_);
  return index_->Sync();
}

}  // namespace purpledb
