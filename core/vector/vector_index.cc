#include "core/vector/vector_index.h"

#include <algorithm>
#include <queue>
#include "core/util/coding.h"
#include "purpledb/write_batch.h"

namespace purpledb {

static const char kPrefixMoments = 'm';
static const char kPrefixVectorMap = 'v';
static const char kPrefixSystem = 's';

VectorIndex::VectorIndex(const VectorOptions& options, const std::string& db_path,
                         std::unique_ptr<DB> db)
    : options_(options), db_path_(db_path), db_(std::move(db)) {}

VectorIndex::~VectorIndex() {
  if (segment_writer_) {
    segment_writer_->Close();
  }
  if (hnsw_) {
    SaveHNSWSnapshot();
  }
}

Status VectorIndex::Open(const VectorOptions& options, const std::string& db_path,
                         std::unique_ptr<VectorIndex>* out) {
  std::unique_ptr<DB> db;
  Status s = DB::Open(options.kv_options, db_path, &db);
  if (!s.ok()) {
    return s;
  }

  auto index = std::unique_ptr<VectorIndex>(new VectorIndex(options, db_path, std::move(db)));
  s = index->Recover();
  if (!s.ok()) {
    return s;
  }

  *out = std::move(index);
  return Status::OK();
}

Status VectorIndex::Recover() {
  // 1. Check or establish Model Lock in LSM system keyspace
  std::string model_lock_key;
  model_lock_key.push_back(kPrefixSystem);
  model_lock_key.append("model_lock");

  std::string stored_lock;
  Status s = db_->Get(Slice(model_lock_key), &stored_lock);
  if (s.ok()) {
    // Model lock exists, verify compatibility
    if (stored_lock.size() < 10) {
      return Status::Corruption("VectorIndex: invalid model lock record");
    }
    const char* p = stored_lock.data();
    uint32_t stored_dim = DecodeFixed32(p); p += 4;
    auto stored_dtype = static_cast<VectorDataType>(*reinterpret_cast<const uint8_t*>(p)); p += 1;
    auto stored_metric = static_cast<MetricType>(*reinterpret_cast<const uint8_t*>(p)); p += 1;
    uint32_t model_len = DecodeFixed32(p); p += 4;
    std::string stored_model_id(p, model_len);

    if (stored_dim != options_.dim || stored_dtype != options_.dtype ||
        stored_metric != options_.metric || stored_model_id != options_.model_id) {
      return Status::InvalidArgument("VectorIndex: database model lock mismatch");
    }
  } else if (s.IsNotFound()) {
    // Save model lock
    std::string lock_val;
    PutFixed32(&lock_val, options_.dim);
    lock_val.push_back(static_cast<char>(options_.dtype));
    lock_val.push_back(static_cast<char>(options_.metric));
    PutFixed32(&lock_val, static_cast<uint32_t>(options_.model_id.size()));
    lock_val.append(options_.model_id);
    s = db_->Put(Slice(model_lock_key), Slice(lock_val));
    if (!s.ok()) return s;
  } else {
    return s;
  }

  // 2. Recover next_vector_id
  std::string next_vid_key;
  next_vid_key.push_back(kPrefixSystem);
  next_vid_key.append("next_vector_id");

  std::string next_vid_str;
  s = db_->Get(Slice(next_vid_key), &next_vid_str);
  if (s.ok() && next_vid_str.size() == 8) {
    next_vector_id_ = DecodeFixed64(next_vid_str.data());
  } else {
    next_vector_id_ = 0;
  }

  // 3. Recover DeleteBitmap
  std::string bm_key;
  bm_key.push_back(kPrefixSystem);
  bm_key.append("delete_bitmap");

  std::string bm_str;
  s = db_->Get(Slice(bm_key), &bm_str);
  if (s.ok()) {
    delete_bitmap_.Deserialize(Slice(bm_str));
  } else {
    delete_bitmap_.Reset();
  }
  delete_bitmap_.SetTotalCount(next_vector_id_);

  // 4. Load vector segment file
  std::string segment_path = db_path_ + "/000001.vec";
  Env* env = options_.kv_options.env;
  if (env->FileExists(segment_path)) {
    std::unique_ptr<VectorSegmentReader> reader;
    s = VectorSegmentReader::Open(env, segment_path, &reader);
    if (!s.ok()) return s;
    s = reader->ReadAllVectors(&flat_vectors_);
    if (!s.ok()) return s;
  }

  uint64_t recovered_vec_count = flat_vectors_.size() / options_.dim;
  if (next_vector_id_ > recovered_vec_count) {
    // A crash or torn write occurred: truncate next_vector_id_ to match durable vectors
    next_vector_id_ = recovered_vec_count;
  }

  // Re-open active segment writer in append mode or create
  s = VectorSegmentWriter::Create(env, segment_path, options_.dim,
                                  options_.dtype, options_.metric,
                                  options_.chunk_size, options_.model_id,
                                  &segment_writer_);
  if (!s.ok()) return s;

  // Re-populate existing vectors into the writer buffer so future appends align
  for (size_t i = 0; i < recovered_vec_count; ++i) {
    segment_writer_->Append(flat_vectors_.data() + i * options_.dim, options_.dim);
  }
  segment_writer_->Flush();

  // 5. Rebuild key_to_id_ and id_to_key_ from LSM tree
  std::unique_ptr<Iterator> it = db_->NewIterator(ReadOptions());
  std::string moment_prefix;
  moment_prefix.push_back(kPrefixMoments);

  for (it->Seek(Slice(moment_prefix)); it->Valid(); it->Next()) {
    Slice k = it->key();
    if (k.empty() || k[0] != kPrefixMoments) break;

    std::string user_key = k.ToString().substr(1);
    Slice v = it->value();
    if (v.size() >= 8) {
      uint64_t vid = DecodeFixed64(v.data());
      if (vid < recovered_vec_count && !delete_bitmap_.IsDeleted(vid)) {
        key_to_id_[user_key] = vid;
        id_to_key_[vid] = user_key;
      } else if (vid >= recovered_vec_count) {
        delete_bitmap_.MarkDeleted(vid);
      }
    }
  }

  // 6. Initialize or restore HNSW graph index
  if (options_.enable_hnsw) {
    HNSWOptions hnsw_opts;
    hnsw_opts.dim = options_.dim;
    hnsw_opts.metric = options_.metric;
    hnsw_opts.M = options_.hnsw_M;
    hnsw_opts.max_M0 = options_.hnsw_max_M0;
    hnsw_opts.efConstruction = options_.hnsw_efConstruction;
    hnsw_opts.efSearch = options_.hnsw_efSearch;

    hnsw_ = std::make_unique<HNSW>(hnsw_opts);

    std::string snapshot_path = db_path_ + "/index.hnsw";
    if (env->FileExists(snapshot_path)) {
      Status snap_s = hnsw_->LoadSnapshot(env, snapshot_path);
      if (snap_s.ok()) {
        hnsw_->SetVectors(flat_vectors_);
        uint64_t snap_nodes = hnsw_->NodeCount();
        if (snap_nodes < recovered_vec_count) {
          // Replay tail inserts from recovered_vec_count
          std::vector<float> tail(flat_vectors_.begin() + snap_nodes * options_.dim,
                                  flat_vectors_.end());
          hnsw_->ReplayTail(snap_nodes, tail, options_.dim);
        }
      } else {
        // Corrupt snapshot: rebuild cleanly from flat vectors
        hnsw_->Rebuild(delete_bitmap_, flat_vectors_, options_.dim);
      }
    } else {
      // No snapshot yet: build from flat vectors
      hnsw_->Rebuild(delete_bitmap_, flat_vectors_, options_.dim);
    }
  }

  return Status::OK();
}

Status VectorIndex::Put(const std::string& key, const float* vector, size_t dim) {
  if (dim != options_.dim) {
    return Status::InvalidArgument("Vector dimension mismatch");
  }

  // Check if overwriting an existing key
  auto existing = key_to_id_.find(key);
  if (existing != key_to_id_.end()) {
    delete_bitmap_.MarkDeleted(existing->second);
    id_to_key_.erase(existing->second);
  }

  uint64_t vid = next_vector_id_++;
  delete_bitmap_.ClearDeleted(vid);
  delete_bitmap_.SetTotalCount(next_vector_id_);

  // Append to segment writer
  Status s = segment_writer_->Append(vector, dim);
  if (!s.ok()) return s;

  // Append to in-memory flat vector cache
  flat_vectors_.insert(flat_vectors_.end(), vector, vector + dim);
  key_to_id_[key] = vid;
  id_to_key_[vid] = key;

  // Insert into HNSW graph index if enabled
  if (hnsw_) {
    Status hs = hnsw_->Insert(vid, vector, dim);
    if (!hs.ok()) return hs;
  }

  // WiscKey separation: commit metadata and pointer to LSM
  WriteBatch batch;
  std::string m_key;
  m_key.push_back(kPrefixMoments);
  m_key.append(key);

  char vid_buf[8];
  EncodeFixed64(vid_buf, vid);
  batch.Put(Slice(m_key), Slice(vid_buf, 8));

  std::string v_key;
  v_key.push_back(kPrefixVectorMap);
  v_key.append(vid_buf, 8);
  batch.Put(Slice(v_key), Slice(key));

  // Update next_vector_id
  std::string next_vid_key;
  next_vid_key.push_back(kPrefixSystem);
  next_vid_key.append("next_vector_id");
  char next_vid_buf[8];
  EncodeFixed64(next_vid_buf, next_vector_id_);
  batch.Put(Slice(next_vid_key), Slice(next_vid_buf, 8));

  // Update delete_bitmap
  std::string bm_key;
  bm_key.push_back(kPrefixSystem);
  bm_key.append("delete_bitmap");
  batch.Put(Slice(bm_key), Slice(delete_bitmap_.Serialize()));

  WriteOptions write_opts;
  write_opts.sync = options_.kv_options.sync;
  return db_->Write(write_opts, &batch);
}

Status VectorIndex::Get(const std::string& key, std::vector<float>* vector_out) {
  auto it = key_to_id_.find(key);
  if (it == key_to_id_.end()) {
    return Status::NotFound("Key not found");
  }

  uint64_t vid = it->second;
  if (delete_bitmap_.IsDeleted(vid)) {
    return Status::NotFound("Key was deleted");
  }

  vector_out->resize(options_.dim);
  const float* src = flat_vectors_.data() + vid * options_.dim;
  std::memcpy(vector_out->data(), src, options_.dim * sizeof(float));
  return Status::OK();
}

Status VectorIndex::Delete(const std::string& key) {
  auto it = key_to_id_.find(key);
  if (it == key_to_id_.end()) {
    return Status::NotFound("Key not found");
  }

  uint64_t vid = it->second;
  delete_bitmap_.MarkDeleted(vid);
  key_to_id_.erase(it);
  id_to_key_.erase(vid);

  WriteBatch batch;
  std::string m_key;
  m_key.push_back(kPrefixMoments);
  m_key.append(key);
  batch.Delete(Slice(m_key));

  char vid_buf[8];
  EncodeFixed64(vid_buf, vid);
  std::string v_key;
  v_key.push_back(kPrefixVectorMap);
  v_key.append(vid_buf, 8);
  batch.Delete(Slice(v_key));

  std::string bm_key;
  bm_key.push_back(kPrefixSystem);
  bm_key.append("delete_bitmap");
  batch.Put(Slice(bm_key), Slice(delete_bitmap_.Serialize()));

  WriteOptions write_opts;
  write_opts.sync = options_.kv_options.sync;
  return db_->Write(write_opts, &batch);
}

bool VectorIndex::Contains(const std::string& key) {
  auto it = key_to_id_.find(key);
  if (it == key_to_id_.end()) return false;
  return !delete_bitmap_.IsDeleted(it->second);
}

uint64_t VectorIndex::Count() const {
  return key_to_id_.size();
}

Status VectorIndex::Search(const float* query_vector, size_t dim, size_t k,
                           std::vector<SearchResult>* results) {
  if (dim != options_.dim) {
    return Status::InvalidArgument("Query dimension mismatch");
  }
  if (k == 0) {
    results->clear();
    return Status::OK();
  }

  // Fast path: use HNSW approximate search if available
  if (hnsw_ && hnsw_->NodeCount() >= k) {
    std::vector<HNSWSearchResult> hnsw_res;
    Status hs = hnsw_->Search(query_vector, dim, k, options_.hnsw_efSearch,
                              &delete_bitmap_, &hnsw_res);
    if (hs.ok() && !hnsw_res.empty()) {
      results->clear();
      results->reserve(hnsw_res.size());
      for (const auto& r : hnsw_res) {
        auto kit = id_to_key_.find(r.node_id);
        if (kit != id_to_key_.end()) {
          results->push_back({kit->second, r.node_id, r.score});
        }
      }
      return Status::OK();
    }
  }

  // Fallback to exact brute-force SIMD search
  return SearchExact(query_vector, dim, k, results);
}

Status VectorIndex::SearchExact(const float* query_vector, size_t dim, size_t k,
                                std::vector<SearchResult>* results) {
  if (dim != options_.dim) {
    return Status::InvalidArgument("Query dimension mismatch");
  }
  if (k == 0) {
    results->clear();
    return Status::OK();
  }

  // Min-heap to maintain Top-K results
  std::priority_queue<SearchResult, std::vector<SearchResult>, std::greater<SearchResult>> top_k;

  size_t total_vecs = flat_vectors_.size() / options_.dim;
  for (size_t vid = 0; vid < total_vecs; ++vid) {
    if (delete_bitmap_.IsDeleted(vid)) {
      continue;
    }

    const float* candidate = flat_vectors_.data() + vid * options_.dim;
    float score = 0.0f;
    if (options_.metric == MetricType::kCosine) {
      score = CosineSimilarity(query_vector, candidate, dim);
    } else {
      score = -L2DistanceSquared(query_vector, candidate, dim);
    }

    auto key_it = id_to_key_.find(vid);
    if (key_it == id_to_key_.end()) {
      continue;
    }

    SearchResult res{key_it->second, vid, score};
    if (top_k.size() < k) {
      top_k.push(res);
    } else if (score > top_k.top().score) {
      top_k.pop();
      top_k.push(res);
    }
  }

  results->resize(top_k.size());
  for (size_t i = top_k.size(); i > 0; --i) {
    (*results)[i - 1] = top_k.top();
    top_k.pop();
  }

  return Status::OK();
}

Status VectorIndex::SearchFiltered(
    const float* query_vector, size_t dim, size_t k,
    const std::function<bool(const std::string& key)>& filter_fn,
    std::vector<SearchResult>* results,
    size_t overfetch) {
  results->clear();
  if (k == 0) return Status::OK();

  size_t fetch_k = k * overfetch;
  std::vector<SearchResult> candidates;
  Status s = Search(query_vector, dim, fetch_k, &candidates);
  if (!s.ok()) return s;

  for (const auto& c : candidates) {
    if (filter_fn(c.key)) {
      results->push_back(c);
      if (results->size() >= k) {
        return Status::OK();
      }
    }
  }

  // If overfetch didn't find k matches, fall back to exact scan
  if (results->size() < k) {
    std::vector<SearchResult> all_cands;
    s = SearchExact(query_vector, dim, flat_vectors_.size() / options_.dim, &all_cands);
    if (!s.ok()) return s;
    results->clear();
    for (const auto& c : all_cands) {
      if (filter_fn(c.key)) {
        results->push_back(c);
        if (results->size() >= k) break;
      }
    }
  }
  return Status::OK();
}

Status VectorIndex::RebuildIndex() {
  if (hnsw_) {
    Status s = hnsw_->Rebuild(delete_bitmap_, flat_vectors_, options_.dim);
    if (!s.ok()) return s;
    return SaveHNSWSnapshot();
  }
  return Status::OK();
}

Status VectorIndex::SaveHNSWSnapshot() {
  if (hnsw_ && hnsw_->NodeCount() > 0) {
    std::string snapshot_path = db_path_ + "/index.hnsw";
    return hnsw_->SaveSnapshot(options_.kv_options.env, snapshot_path);
  }
  return Status::OK();
}

Status VectorIndex::Flush() {
  if (segment_writer_) {
    Status s = segment_writer_->Flush();
    if (!s.ok()) return s;
  }
  if (hnsw_) {
    SaveHNSWSnapshot();
  }
  return Status::OK();
}

Status VectorIndex::Sync() {
  if (segment_writer_) {
    Status s = segment_writer_->Sync();
    if (!s.ok()) return s;
  }
  if (hnsw_) {
    SaveHNSWSnapshot();
  }
  return Status::OK();
}

}  // namespace purpledb
