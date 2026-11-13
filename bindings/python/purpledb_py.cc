#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/numpy.h>

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/embed/local_embedder.h"
#include "core/pipeline/frame_decoder.h"
#include "core/query/query_engine.h"
#include "purpledb/status.h"

namespace py = pybind11;
using namespace purpledb;

static void ThrowIfError(const Status& s) {
  if (s.ok()) return;
  if (s.IsNotFound()) {
    throw py::key_error(s.ToString());
  } else if (s.IsInvalidArgument()) {
    throw py::value_error(s.ToString());
  } else {
    throw std::runtime_error(s.ToString());
  }
}

class PyPurpleDB {
 public:
  PyPurpleDB(const std::string& db_path,
             bool create_if_missing,
             bool error_if_exists,
             int dim,
             bool enable_hnsw,
             int hnsw_m,
             int hnsw_ef_search,
             double fps)
      : closed_(false) {
    QueryOptions q_opts;
    q_opts.vector_options.kv_options.create_if_missing = create_if_missing;
    q_opts.vector_options.kv_options.error_if_exists = error_if_exists;
    q_opts.vector_options.enable_hnsw = enable_hnsw;
    if (hnsw_m > 0) {
      q_opts.vector_options.hnsw_M = static_cast<uint16_t>(hnsw_m);
    }
    if (hnsw_ef_search > 0) {
      q_opts.vector_options.hnsw_efSearch = hnsw_ef_search;
    }
    if (fps > 0.0) {
      q_opts.ingest_options.fps = fps;
    }

    LocalEmbedderOptions emb_opts;
    emb_opts.dim = dim > 0 ? dim : 512;
    auto embedder = std::make_shared<LocalEmbedder>(emb_opts);

    Status s;
    {
      py::gil_scoped_release release;
      s = QueryEngine::Open(db_path, embedder, q_opts, &engine_);
    }
    ThrowIfError(s);
  }

  ~PyPurpleDB() {
    CloseInternal();
  }

  std::string AddVideo(const std::string& video_path) {
    CheckNotClosed();
    std::string video_id;
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->AddVideo(video_path, &video_id);
    }
    ThrowIfError(s);
    return video_id;
  }

  size_t AddSyntheticVideo(const std::string& video_id, int count, double fps, int width, int height) {
    CheckNotClosed();
    std::vector<DecodedFrame> frames;
    Status s = FrameDecoder::GenerateSyntheticFrames(count, fps, width, height, &frames);
    ThrowIfError(s);
    size_t moments_count = 0;
    {
      py::gil_scoped_release release;
      s = engine_->AddFrames(video_id, frames, &moments_count);
    }
    ThrowIfError(s);
    return moments_count;
  }

  std::vector<MomentHit> Search(const std::string& query,
                                int top_k,
                                const std::string& video_id,
                                double start_sec,
                                double end_sec) {
    CheckNotClosed();
    QueryFilter filter;
    filter.video_id = video_id;
    filter.start_sec = start_sec;
    filter.end_sec = end_sec;
    std::vector<MomentHit> hits;
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->SearchText(query, top_k, filter, &hits);
    }
    ThrowIfError(s);
    return hits;
  }

  std::vector<MomentHit> SearchByImage(py::buffer rgb_buffer,
                                       int width,
                                       int height,
                                       int channels,
                                       int top_k,
                                       const std::string& video_id,
                                       double start_sec,
                                       double end_sec) {
    CheckNotClosed();
    py::buffer_info info = rgb_buffer.request();
    size_t expected_size = static_cast<size_t>(width * height * channels);
    if (static_cast<size_t>(info.size) < expected_size) {
      throw py::value_error("Buffer size is smaller than width * height * channels");
    }
    const uint8_t* ptr = static_cast<const uint8_t*>(info.ptr);
    ImageRef img = ImageRef::FromRgb(ptr, width, height, channels);

    QueryFilter filter;
    filter.video_id = video_id;
    filter.start_sec = start_sec;
    filter.end_sec = end_sec;
    std::vector<MomentHit> hits;
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->SearchByExample(img, top_k, filter, &hits);
    }
    ThrowIfError(s);
    return hits;
  }

  void DeleteVideo(const std::string& video_id) {
    CheckNotClosed();
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->DeleteVideo(video_id);
    }
    ThrowIfError(s);
  }

  DatabaseStats Stats() {
    CheckNotClosed();
    DatabaseStats st;
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->GetStats(&st);
    }
    ThrowIfError(s);
    return st;
  }

  void Sync() {
    CheckNotClosed();
    Status s;
    {
      py::gil_scoped_release release;
      s = engine_->Sync();
    }
    ThrowIfError(s);
  }

  void Close() {
    CloseInternal();
  }

  bool IsClosed() const {
    return closed_;
  }

 private:
  void CheckNotClosed() const {
    if (closed_ || !engine_) {
      throw std::runtime_error("Database is closed");
    }
  }

  void CloseInternal() {
    if (!closed_ && engine_) {
      py::gil_scoped_release release;
      engine_->Sync();
      engine_.reset();
      closed_ = true;
    }
  }

  std::unique_ptr<QueryEngine> engine_;
  bool closed_{false};
};

PYBIND11_MODULE(_purpledb, m) {
  m.doc() = "PurpleDB native C++ engine extension";

  py::class_<MomentHit>(m, "MomentHit")
      .def_readonly("video_id", &MomentHit::video_id)
      .def_readonly("moment_id", &MomentHit::moment_id)
      .def_readonly("start_sec", &MomentHit::start_sec)
      .def_readonly("end_sec", &MomentHit::end_sec)
      .def_readonly("score", &MomentHit::score)
      .def_readonly("thumbnail_path", &MomentHit::thumbnail_path)
      .def_readonly("caption", &MomentHit::caption)
      .def_property_readonly("duration", &MomentHit::duration)
      .def("__repr__", [](const MomentHit& h) {
        std::ostringstream oss;
        oss << "<MomentHit video='" << h.video_id
            << "' moment=" << h.moment_id
            << " range=[" << h.start_sec << "s - " << h.end_sec << "s]"
            << " score=" << h.score << ">";
        return oss.str();
      })
      .def("to_dict", [](const MomentHit& h) {
        py::dict d;
        d["video_id"] = h.video_id;
        d["moment_id"] = h.moment_id;
        d["start_sec"] = h.start_sec;
        d["end_sec"] = h.end_sec;
        d["score"] = h.score;
        d["thumbnail_path"] = h.thumbnail_path;
        d["caption"] = h.caption;
        d["duration"] = h.duration();
        return d;
      });

  py::class_<DatabaseStats>(m, "DatabaseStats")
      .def_readonly("num_videos", &DatabaseStats::num_videos)
      .def_readonly("num_moments", &DatabaseStats::num_moments)
      .def_readonly("num_vectors", &DatabaseStats::num_vectors)
      .def_readonly("disk_size_bytes", &DatabaseStats::disk_size_bytes)
      .def("__repr__", [](const DatabaseStats& s) {
        std::ostringstream oss;
        oss << "<DatabaseStats videos=" << s.num_videos
            << " moments=" << s.num_moments
            << " vectors=" << s.num_vectors
            << " disk_bytes=" << s.disk_size_bytes << ">";
        return oss.str();
      })
      .def("to_dict", [](const DatabaseStats& s) {
        py::dict d;
        d["num_videos"] = s.num_videos;
        d["num_moments"] = s.num_moments;
        d["num_vectors"] = s.num_vectors;
        d["disk_size_bytes"] = s.disk_size_bytes;
        return d;
      });

  py::class_<PyPurpleDB>(m, "PurpleDBEngine")
      .def(py::init<const std::string&, bool, bool, int, bool, int, int, double>(),
           py::arg("db_path"),
           py::arg("create_if_missing") = true,
           py::arg("error_if_exists") = false,
           py::arg("dim") = 512,
           py::arg("enable_hnsw") = true,
           py::arg("hnsw_m") = 16,
           py::arg("hnsw_ef_search") = 64,
           py::arg("fps") = 1.0)
      .def("add_video", &PyPurpleDB::AddVideo, py::arg("video_path"))
      .def("add_synthetic_video", &PyPurpleDB::AddSyntheticVideo,
           py::arg("video_id"),
           py::arg("count") = 10,
           py::arg("fps") = 1.0,
           py::arg("width") = 224,
           py::arg("height") = 224)
      .def("search", &PyPurpleDB::Search,
           py::arg("query"),
           py::arg("top_k") = 5,
           py::arg("video_id") = "",
           py::arg("start_sec") = -1.0,
           py::arg("end_sec") = -1.0)
      .def("search_by_image", &PyPurpleDB::SearchByImage,
           py::arg("rgb_buffer"),
           py::arg("width"),
           py::arg("height"),
           py::arg("channels") = 3,
           py::arg("top_k") = 5,
           py::arg("video_id") = "",
           py::arg("start_sec") = -1.0,
           py::arg("end_sec") = -1.0)
      .def("delete_video", &PyPurpleDB::DeleteVideo, py::arg("video_id"))
      .def("stats", &PyPurpleDB::Stats)
      .def("sync", &PyPurpleDB::Sync)
      .def("close", &PyPurpleDB::Close)
      .def("is_closed", &PyPurpleDB::IsClosed)
      .def("__enter__", [](PyPurpleDB* self) { return self; })
      .def("__exit__", [](PyPurpleDB* self, py::object exc_type, py::object exc_val, py::object exc_tb) {
        (void)exc_type;
        (void)exc_val;
        (void)exc_tb;
        self->Close();
      });
}
