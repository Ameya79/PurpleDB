#include "core/vector/hnsw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {

HNSW::HNSW(const HNSWOptions& options)
    : options_(options) {
  double m = (options_.M > 1) ? static_cast<double>(options_.M) : 2.0;
  ml_ = 1.0 / std::log(m);
  rng_.seed(options_.random_seed);
}

uint32_t HNSW::GetRandomLevel() {
  double r = uniform_dist_(rng_);
  if (r <= 0.0) r = 1e-9;
  double lvl = -std::log(r) * ml_;
  return static_cast<uint32_t>(lvl);
}

float HNSW::ComputeDistance(const float* a, const float* b) const {
  if (options_.metric == MetricType::kCosine) {
    float dot = DotProduct(a, b, options_.dim);
    // Since vectors are unit-normalized, distance = 1.0 - dot (range: 0.0 to 2.0)
    return 1.0f - dot;
  } else {
    return L2DistanceSquared(a, b, options_.dim);
  }
}

float HNSW::ComputeDistanceToNode(const float* vec, uint64_t node_id) const {
  const float* node_vec = flat_vectors_.data() + node_id * options_.dim;
  return ComputeDistance(vec, node_vec);
}

void HNSW::SearchLayer(const float* query,
                       const std::vector<uint64_t>& enter_points,
                       size_t ef, uint32_t level,
                       std::vector<DistNode>* out_candidates) const {
  std::vector<bool> visited(nodes_.size(), false);

  // Candidates min-heap: closest candidate at top
  std::priority_queue<DistNode, std::vector<DistNode>, std::greater<DistNode>> candidates;
  // Nearest max-heap: furthest element at top, bounded to size ef
  std::priority_queue<DistNode, std::vector<DistNode>, std::less<DistNode>> nearest;

  for (uint64_t ep : enter_points) {
    if (ep < nodes_.size()) {
      visited[ep] = true;
      float d = ComputeDistanceToNode(query, ep);
      candidates.push({d, ep});
      nearest.push({d, ep});
    }
  }

  while (!candidates.empty()) {
    DistNode curr = candidates.top();
    candidates.pop();

    DistNode furthest = nearest.top();
    if (curr.dist > furthest.dist) {
      break;
    }

    if (curr.id >= nodes_.size() || level >= nodes_[curr.id].neighbors.size()) {
      continue;
    }

    const auto& nbrs = nodes_[curr.id].neighbors[level];
    for (uint64_t nbr : nbrs) {
      if (nbr < visited.size() && !visited[nbr]) {
        visited[nbr] = true;
        furthest = nearest.top();
        float d = ComputeDistanceToNode(query, nbr);

        if (d < furthest.dist || nearest.size() < ef) {
          candidates.push({d, nbr});
          nearest.push({d, nbr});

          if (nearest.size() > ef) {
            nearest.pop();
          }
        }
      }
    }
  }

  out_candidates->clear();
  out_candidates->reserve(nearest.size());
  while (!nearest.empty()) {
    out_candidates->push_back(nearest.top());
    nearest.pop();
  }
  // Sort ascending: closest first
  std::sort(out_candidates->begin(), out_candidates->end());
}

void HNSW::SelectNeighborsSimple(const float* query,
                                 std::vector<DistNode>& candidates,
                                 size_t max_m,
                                 std::vector<uint64_t>* selected) const {
  (void)query;
  std::sort(candidates.begin(), candidates.end());
  selected->clear();
  size_t count = std::min(max_m, candidates.size());
  selected->reserve(count);
  for (size_t i = 0; i < count; ++i) {
    selected->push_back(candidates[i].id);
  }
}

Status HNSW::Insert(uint64_t node_id, const float* vector, size_t dim) {
  if (dim != options_.dim) {
    return Status::InvalidArgument("Vector dimension mismatch");
  }

  uint32_t l = GetRandomLevel();

  if (node_id >= nodes_.size()) {
    nodes_.resize(node_id + 1);
  }

  nodes_[node_id].id = node_id;
  nodes_[node_id].level = l;
  nodes_[node_id].neighbors.resize(l + 1);

  // Store vector in flat cache
  size_t expected_offset = node_id * options_.dim;
  if (flat_vectors_.size() <= expected_offset) {
    flat_vectors_.resize(expected_offset + options_.dim);
  }
  std::memcpy(flat_vectors_.data() + expected_offset, vector, options_.dim * sizeof(float));

  num_nodes_++;

  if (!has_enter_node_) {
    enter_node_ = node_id;
    max_level_ = l;
    has_enter_node_ = true;
    return Status::OK();
  }

  uint64_t curr_node = enter_node_;
  float curr_dist = ComputeDistanceToNode(vector, curr_node);

  // 1. Greedy search down to level l + 1
  for (uint32_t level = max_level_; level > l; --level) {
    bool changed = true;
    while (changed) {
      changed = false;
      if (curr_node < nodes_.size() && level < nodes_[curr_node].neighbors.size()) {
        for (uint64_t nbr : nodes_[curr_node].neighbors[level]) {
          float d = ComputeDistanceToNode(vector, nbr);
          if (d < curr_dist) {
            curr_dist = d;
            curr_node = nbr;
            changed = true;
          }
        }
      }
    }
  }

  // 2. Beam search and connect from min(l, max_level_) down to 0
  uint32_t start_level = std::min(l, max_level_);
  for (int level = static_cast<int>(start_level); level >= 0; --level) {
    std::vector<DistNode> layer_candidates;
    SearchLayer(vector, {curr_node}, options_.efConstruction, static_cast<uint32_t>(level),
                &layer_candidates);

    size_t max_m = (level == 0) ? options_.max_M0 : options_.M;
    std::vector<uint64_t> neighbors;
    SelectNeighborsSimple(vector, layer_candidates, max_m, &neighbors);

    nodes_[node_id].neighbors[level] = neighbors;

    // Add bidirectional connections
    for (uint64_t nbr : neighbors) {
      if (nbr < nodes_.size() && static_cast<size_t>(level) < nodes_[nbr].neighbors.size()) {
        auto& nbr_list = nodes_[nbr].neighbors[level];
        nbr_list.push_back(node_id);

        // Shrink neighbor list if it exceeds capacity
        if (nbr_list.size() > max_m) {
          const float* nbr_vec = flat_vectors_.data() + nbr * options_.dim;
          std::vector<DistNode> to_sort;
          to_sort.reserve(nbr_list.size());
          for (uint64_t cand_id : nbr_list) {
            to_sort.push_back({ComputeDistanceToNode(nbr_vec, cand_id), cand_id});
          }
          std::sort(to_sort.begin(), to_sort.end());
          nbr_list.clear();
          for (size_t s = 0; s < max_m; ++s) {
            nbr_list.push_back(to_sort[s].id);
          }
        }
      }
    }

    if (!layer_candidates.empty()) {
      curr_node = layer_candidates.front().id;
    }
  }

  if (l > max_level_) {
    max_level_ = l;
    enter_node_ = node_id;
  }

  return Status::OK();
}

Status HNSW::Search(const float* query, size_t dim, size_t k,
                    size_t ef_search, const DeleteBitmap* tombstones,
                    std::vector<HNSWSearchResult>* results) const {
  if (dim != options_.dim) {
    return Status::InvalidArgument("Query dimension mismatch");
  }
  results->clear();
  if (!has_enter_node_ || num_nodes_ == 0 || k == 0) {
    return Status::OK();
  }

  uint64_t curr_node = enter_node_;
  float curr_dist = ComputeDistanceToNode(query, curr_node);

  // 1. Greedy search from max_level_ down to 1
  for (uint32_t level = max_level_; level > 0; --level) {
    bool changed = true;
    while (changed) {
      changed = false;
      if (curr_node < nodes_.size() && level < nodes_[curr_node].neighbors.size()) {
        for (uint64_t nbr : nodes_[curr_node].neighbors[level]) {
          float d = ComputeDistanceToNode(query, nbr);
          if (d < curr_dist) {
            curr_dist = d;
            curr_node = nbr;
            changed = true;
          }
        }
      }
    }
  }

  // 2. Beam search at Layer 0 with efSearch
  size_t ef = std::max(k, ef_search);
  std::vector<DistNode> candidates;
  SearchLayer(query, {curr_node}, ef, 0, &candidates);

  // Filter deleted tombstones and collect top K
  std::sort(candidates.begin(), candidates.end());
  results->reserve(k);

  for (const auto& cand : candidates) {
    if (tombstones && tombstones->IsDeleted(cand.id)) {
      continue;
    }

    float score = (options_.metric == MetricType::kCosine)
                      ? (1.0f - cand.dist)
                      : -cand.dist;

    results->push_back({cand.id, score, cand.dist});
    if (results->size() >= k) {
      break;
    }
  }

  return Status::OK();
}

Status HNSW::SaveSnapshot(Env* env, const std::string& snapshot_path) const {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(snapshot_path, &file);
  if (!s.ok()) return s;

  // Header: 64 bytes
  std::string hdr;
  hdr.reserve(kHeaderSize);
  PutFixed64(&hdr, kMagic);
  PutFixed32(&hdr, kVersion);
  PutFixed32(&hdr, options_.dim);
  hdr.push_back(static_cast<char>(options_.metric));
  PutFixed16(&hdr, options_.M);
  PutFixed16(&hdr, options_.max_M0);
  PutFixed32(&hdr, options_.efConstruction);
  PutFixed64(&hdr, enter_node_);
  PutFixed32(&hdr, max_level_);
  PutFixed64(&hdr, num_nodes_);

  // Pad to 60 bytes
  hdr.resize(60, 0);

  // Masked CRC-32C over header
  uint32_t header_crc = crc32c::Mask(crc32c::Value(hdr.data(), hdr.size()));
  PutFixed32(&hdr, header_crc);

  s = file->Append(Slice(hdr));
  if (!s.ok()) return s;

  // Node adjacency serialization
  std::string node_buf;
  uint32_t body_crc = 0;

  for (const auto& node : nodes_) {
    if (node.neighbors.empty()) continue;

    PutFixed64(&node_buf, node.id);
    PutFixed32(&node_buf, node.level);

    for (uint32_t l = 0; l <= node.level; ++l) {
      const auto& nbrs = node.neighbors[l];
      PutFixed16(&node_buf, static_cast<uint16_t>(nbrs.size()));
      for (uint64_t nbr : nbrs) {
        PutFixed64(&node_buf, nbr);
      }
    }

    if (node_buf.size() >= 64 * 1024) {
      body_crc = crc32c::Extend(body_crc, node_buf.data(), node_buf.size());
      s = file->Append(Slice(node_buf));
      if (!s.ok()) return s;
      node_buf.clear();
    }
  }

  if (!node_buf.empty()) {
    body_crc = crc32c::Extend(body_crc, node_buf.data(), node_buf.size());
    s = file->Append(Slice(node_buf));
    if (!s.ok()) return s;
  }

  // Footer CRC
  char footer_crc_bytes[4];
  EncodeFixed32(footer_crc_bytes, crc32c::Mask(body_crc));
  s = file->Append(Slice(footer_crc_bytes, 4));
  if (!s.ok()) return s;

  s = file->Flush();
  if (!s.ok()) return s;
  return file->Sync();
}

Status HNSW::LoadSnapshot(Env* env, const std::string& snapshot_path) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(snapshot_path, &file);
  if (!s.ok()) return s;

  // Read 64-byte header
  char hdr_buf[kHeaderSize];
  Slice hdr_slice;
  s = file->Read(kHeaderSize, &hdr_slice, hdr_buf);
  if (!s.ok() || hdr_slice.size() < kHeaderSize) {
    return Status::Corruption("HNSW snapshot: truncated header");
  }

  uint32_t expected_hdr_crc = DecodeFixed32(hdr_slice.data() + 60);
  uint32_t actual_hdr_crc = crc32c::Mask(crc32c::Value(hdr_slice.data(), 60));
  if (expected_hdr_crc != actual_hdr_crc) {
    return Status::Corruption("HNSW snapshot: header checksum mismatch");
  }

  const char* p = hdr_slice.data();
  uint64_t magic = DecodeFixed64(p); p += 8;
  if (magic != kMagic) return Status::Corruption("HNSW snapshot: invalid magic");

  uint32_t version = DecodeFixed32(p); p += 4;
  if (version != kVersion) return Status::Corruption("HNSW snapshot: unsupported version");

  uint32_t dim = DecodeFixed32(p); p += 4;
  if (dim != options_.dim) return Status::InvalidArgument("HNSW snapshot: dimension mismatch");

  p += 1; // skip metric
  options_.M = DecodeFixed16(p); p += 2;
  options_.max_M0 = DecodeFixed16(p); p += 2;
  options_.efConstruction = DecodeFixed32(p); p += 4;
  enter_node_ = DecodeFixed64(p); p += 8;
  max_level_ = DecodeFixed32(p); p += 4;
  num_nodes_ = DecodeFixed64(p); p += 8;

  has_enter_node_ = (num_nodes_ > 0);

  // Read full remainder of file into memory for node reconstruction
  uint64_t file_size = 0;
  env->GetFileSize(snapshot_path, &file_size);
  if (file_size < kHeaderSize + 4) {
    return Status::Corruption("HNSW snapshot: file too small");
  }

  size_t body_size = static_cast<size_t>(file_size - kHeaderSize - 4);
  std::vector<char> body(body_size);
  Slice body_slice;
  s = file->Read(body_size, &body_slice, body.data());
  if (!s.ok() || body_slice.size() < body_size) {
    return Status::Corruption("HNSW snapshot: truncated body");
  }

  char footer_buf[4];
  Slice footer_slice;
  s = file->Read(4, &footer_slice, footer_buf);
  if (!s.ok() || footer_slice.size() < 4) {
    return Status::Corruption("HNSW snapshot: missing footer checksum");
  }

  uint32_t expected_footer_crc = DecodeFixed32(footer_buf);
  uint32_t actual_footer_crc = crc32c::Mask(crc32c::Value(body.data(), body_size));
  if (expected_footer_crc != actual_footer_crc) {
    return Status::Corruption("HNSW snapshot: footer checksum mismatch");
  }

  // Parse nodes
  const char* bp = body.data();
  const char* limit = bp + body_size;

  nodes_.clear();
  nodes_.resize(num_nodes_);

  while (bp < limit) {
    if (limit - bp < 12) break;
    uint64_t nid = DecodeFixed64(bp); bp += 8;
    uint32_t lvl = DecodeFixed32(bp); bp += 4;

    if (nid >= nodes_.size()) {
      nodes_.resize(nid + 1);
    }

    nodes_[nid].id = nid;
    nodes_[nid].level = lvl;
    nodes_[nid].neighbors.resize(lvl + 1);

    for (uint32_t l = 0; l <= lvl; ++l) {
      if (limit - bp < 2) break;
      uint16_t deg = DecodeFixed16(bp); bp += 2;
      nodes_[nid].neighbors[l].resize(deg);
      for (uint16_t d = 0; d < deg; ++d) {
        if (limit - bp < 8) break;
        nodes_[nid].neighbors[l][d] = DecodeFixed64(bp);
        bp += 8;
      }
    }
  }

  return Status::OK();
}

Status HNSW::ReplayTail(uint64_t from_id, const std::vector<float>& tail_vectors, size_t dim) {
  if (dim != options_.dim) return Status::InvalidArgument("Dimension mismatch");
  size_t count = tail_vectors.size() / dim;
  for (size_t i = 0; i < count; ++i) {
    uint64_t nid = from_id + i;
    const float* v = tail_vectors.data() + i * dim;
    Status s = Insert(nid, v, dim);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status HNSW::Rebuild(const DeleteBitmap& tombstones,
                     const std::vector<float>& vectors,
                     size_t dim) {
  if (dim != options_.dim) return Status::InvalidArgument("Dimension mismatch");

  // Reset graph topology
  nodes_.clear();
  flat_vectors_.clear();
  num_nodes_ = 0;
  max_level_ = 0;
  enter_node_ = 0;
  has_enter_node_ = false;

  size_t total_vecs = vectors.size() / dim;
  for (size_t vid = 0; vid < total_vecs; ++vid) {
    if (tombstones.IsDeleted(vid)) {
      continue;
    }
    const float* v = vectors.data() + vid * dim;
    Status s = Insert(vid, v, dim);
    if (!s.ok()) return s;
  }

  return Status::OK();
}

}  // namespace purpledb
