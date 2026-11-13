#include "purpledb/purpledb_c.h"

#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "core/embed/local_embedder.h"
#include "core/query/query_engine.h"

using namespace purpledb;

struct pdb_t {
  std::unique_ptr<QueryEngine> engine;
};

struct pdb_results_t {
  std::vector<MomentHit> hits;
};

static thread_local std::string tls_last_error;

static void SetLastError(const std::string& err) {
  tls_last_error = err;
}

static void ClearLastError() {
  tls_last_error.clear();
}

extern "C" {

int pdb_abi_version(void) {
  return 1;
}

const char* pdb_last_error(void) {
  return tls_last_error.c_str();
}

void pdb_options_init(pdb_options_t* options) {
  if (!options) return;
  options->create_if_missing = 1;
  options->error_if_exists = 0;
  options->write_buffer_size = 4 * 1024 * 1024;
  options->block_cache_size = 8 * 1024 * 1024;
  options->enable_hnsw = 1;
  options->hnsw_M = 16;
  options->hnsw_efSearch = 64;
}

int pdb_open(const char* dbname, const char* embedder_type, pdb_t** out_db) {
  return pdb_open_with_options(dbname, nullptr, embedder_type, out_db);
}

int pdb_open_with_options(const char* dbname,
                          const pdb_options_t* options,
                          const char* embedder_type,
                          pdb_t** out_db) {
  (void)embedder_type;
  ClearLastError();
  if (!dbname || !out_db) {
    SetLastError("Invalid argument: dbname and out_db cannot be null");
    return PDB_INVALID_ARGUMENT;
  }
  *out_db = nullptr;

  try {
    QueryOptions q_opts;
    if (options) {
      q_opts.vector_options.kv_options.create_if_missing = (options->create_if_missing != 0);
      q_opts.vector_options.kv_options.error_if_exists = (options->error_if_exists != 0);
      if (options->write_buffer_size > 0) {
        q_opts.vector_options.kv_options.write_buffer_size = options->write_buffer_size;
      }
      q_opts.vector_options.enable_hnsw = (options->enable_hnsw != 0);
      if (options->hnsw_M > 0) {
        q_opts.vector_options.hnsw_M = static_cast<uint16_t>(options->hnsw_M);
      }
      if (options->hnsw_efSearch > 0) {
        q_opts.vector_options.hnsw_efSearch = options->hnsw_efSearch;
      }
    }

    // Default to LocalEmbedder
    LocalEmbedderOptions emb_opts;
    emb_opts.dim = 32;
    auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

    std::unique_ptr<QueryEngine> engine;
    Status s = QueryEngine::Open(std::string(dbname), embedder, q_opts, &engine);
    if (!s.ok()) {
      SetLastError(s.ToString());
      return s.IsNotFound() ? PDB_NOT_FOUND : PDB_ERROR;
    }

    auto handle = std::make_unique<pdb_t>();
    handle->engine = std::move(engine);
    *out_db = handle.release();
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_open: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_open");
    return PDB_ERROR;
  }
}

void pdb_close(pdb_t* db) {
  ClearLastError();
  if (!db) return;
  try {
    delete db;
  } catch (...) {
    // Suppress all destructor exceptions in C ABI
  }
}

static QueryFilter ConvertFilter(const pdb_filter_t* f) {
  QueryFilter qf;
  if (f) {
    if (f->video_id && f->video_id[0] != '\0') {
      qf.video_id = f->video_id;
    }
    qf.start_sec = f->start_sec;
    qf.end_sec = f->end_sec;
  }
  return qf;
}

int pdb_search_text(pdb_t* db,
                    const char* text_query,
                    int top_k,
                    const pdb_filter_t* filter,
                    pdb_results_t** out_results) {
  ClearLastError();
  if (!db || !text_query || !out_results) {
    SetLastError("Invalid argument in pdb_search_text");
    return PDB_INVALID_ARGUMENT;
  }
  *out_results = nullptr;

  try {
    QueryFilter qf = ConvertFilter(filter);
    std::vector<MomentHit> hits;
    Status s = db->engine->SearchText(std::string(text_query), top_k, qf, &hits);
    if (!s.ok()) {
      SetLastError(s.ToString());
      return PDB_ERROR;
    }

    auto res = std::make_unique<pdb_results_t>();
    res->hits = std::move(hits);
    *out_results = res.release();
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_search_text: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_search_text");
    return PDB_ERROR;
  }
}

int pdb_search_image(pdb_t* db,
                     const uint8_t* rgb_data,
                     int width,
                     int height,
                     int channels,
                     int top_k,
                     const pdb_filter_t* filter,
                     pdb_results_t** out_results) {
  ClearLastError();
  if (!db || !rgb_data || width <= 0 || height <= 0 || channels != 3 || !out_results) {
    SetLastError("Invalid argument in pdb_search_image");
    return PDB_INVALID_ARGUMENT;
  }
  *out_results = nullptr;

  try {
    QueryFilter qf = ConvertFilter(filter);
    ImageRef img = ImageRef::FromRgb(rgb_data, width, height, channels);

    std::vector<MomentHit> hits;
    Status s = db->engine->SearchByExample(img, top_k, qf, &hits);
    if (!s.ok()) {
      SetLastError(s.ToString());
      return PDB_ERROR;
    }

    auto res = std::make_unique<pdb_results_t>();
    res->hits = std::move(hits);
    *out_results = res.release();
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_search_image: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_search_image");
    return PDB_ERROR;
  }
}

int pdb_results_count(const pdb_results_t* results) {
  if (!results) return 0;
  return static_cast<int>(results->hits.size());
}

int pdb_results_get(const pdb_results_t* results,
                    int index,
                    const char** out_video_id,
                    uint32_t* out_moment_id,
                    double* out_start_sec,
                    double* out_end_sec,
                    float* out_score,
                    const char** out_thumbnail_path,
                    const char** out_caption) {
  ClearLastError();
  if (!results || index < 0 || index >= static_cast<int>(results->hits.size())) {
    SetLastError("Invalid index or null results handle in pdb_results_get");
    return PDB_INVALID_ARGUMENT;
  }

  const auto& h = results->hits[static_cast<size_t>(index)];
  if (out_video_id) *out_video_id = h.video_id.c_str();
  if (out_moment_id) *out_moment_id = h.moment_id;
  if (out_start_sec) *out_start_sec = h.start_sec;
  if (out_end_sec) *out_end_sec = h.end_sec;
  if (out_score) *out_score = h.score;
  if (out_thumbnail_path) *out_thumbnail_path = h.thumbnail_path.c_str();
  if (out_caption) *out_caption = h.caption.c_str();

  return PDB_OK;
}

void pdb_results_destroy(pdb_results_t* results) {
  ClearLastError();
  if (!results) return;
  try {
    delete results;
  } catch (...) {
    // Suppress destructor exception
  }
}

int pdb_add_video(pdb_t* db,
                  const char* video_path,
                  char* out_video_id_buf,
                  size_t buf_len) {
  ClearLastError();
  if (!db || !video_path || !out_video_id_buf || buf_len == 0) {
    SetLastError("Invalid argument in pdb_add_video");
    return PDB_INVALID_ARGUMENT;
  }

  try {
    std::string vid;
    Status s = db->engine->AddVideo(std::string(video_path), &vid);
    if (!s.ok()) {
      SetLastError(s.ToString());
      return PDB_ERROR;
    }

    if (vid.size() + 1 > buf_len) {
      SetLastError("Output buffer too small for video_id");
      return PDB_INVALID_ARGUMENT;
    }

    std::memcpy(out_video_id_buf, vid.c_str(), vid.size() + 1);
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_add_video: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_add_video");
    return PDB_ERROR;
  }
}

int pdb_delete_video(pdb_t* db, const char* video_id) {
  ClearLastError();
  if (!db || !video_id) {
    SetLastError("Invalid argument in pdb_delete_video");
    return PDB_INVALID_ARGUMENT;
  }

  try {
    Status s = db->engine->DeleteVideo(std::string(video_id));
    if (!s.ok()) {
      SetLastError(s.ToString());
      return PDB_ERROR;
    }
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_delete_video: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_delete_video");
    return PDB_ERROR;
  }
}

int pdb_get_stats(pdb_t* db, pdb_stats_t* out_stats) {
  ClearLastError();
  if (!db || !out_stats) {
    SetLastError("Invalid argument in pdb_get_stats");
    return PDB_INVALID_ARGUMENT;
  }

  try {
    DatabaseStats s;
    Status status = db->engine->GetStats(&s);
    if (!status.ok()) {
      SetLastError(status.ToString());
      return PDB_ERROR;
    }

    out_stats->num_videos = s.num_videos;
    out_stats->num_moments = s.num_moments;
    out_stats->num_vectors = s.num_vectors;
    out_stats->disk_size_bytes = s.disk_size_bytes;
    return PDB_OK;
  } catch (const std::exception& e) {
    SetLastError(std::string("Exception in pdb_get_stats: ") + e.what());
    return PDB_ERROR;
  } catch (...) {
    SetLastError("Unknown C++ exception in pdb_get_stats");
    return PDB_ERROR;
  }
}

}  // extern "C"
