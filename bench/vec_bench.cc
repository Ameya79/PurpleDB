#include "core/storage/env.h"
#include "core/vector/vector_index.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace purpledb {
namespace bench {

class VectorBenchmark {
 public:
  VectorBenchmark(int num_vectors, int dim, int num_queries)
      : num_vectors_(num_vectors), dim_(dim), num_queries_(num_queries) {}

  void Run() {
    Env* env = Env::Default();
    std::string dbdir = "bench_purpledb_vec_data";
    // Clean old benchmark dir
    std::vector<std::string> children;
    if (env->GetChildren(dbdir, &children).ok()) {
      for (const auto& c : children) {
        if (c != "." && c != "..") {
          env->DeleteFile(dbdir + "/" + c);
        }
      }
      env->DeleteDir(dbdir);
    }

    VectorOptions options;
    options.dim = dim_;
    options.chunk_size = 64;
    options.metric = MetricType::kCosine;
    options.model_id = "openai/clip-vit-base-patch32";

    std::filesystem::create_directories("bench/results");
    std::ofstream out("bench/results/m5_benchmark.txt");

    auto log_and_print = [&](const std::string& line) {
      std::cout << line << "\n";
      out << line << "\n";
    };

    log_and_print("=================================================");
    log_and_print("       PurpleDB Vector Engine Benchmark (M5)     ");
    log_and_print("=================================================");
    log_and_print("Vectors: " + std::to_string(num_vectors_) +
                  " | Dimension: " + std::to_string(dim_));
    double raw_mb = (static_cast<double>(num_vectors_) * dim_ * sizeof(float)) / (1024.0 * 1024.0);
    log_and_print("Raw Vector Payload: " + std::to_string(raw_mb) + " MB");
    log_and_print("Metric: Cosine Similarity | Format: Append-Only .vec (WiscKey Separation)");
    log_and_print("-------------------------------------------------");

    // Generate deterministic test vectors on unit sphere
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    std::vector<float> data(num_vectors_ * dim_);
    for (int i = 0; i < num_vectors_; ++i) {
      float* ptr = data.data() + i * dim_;
      for (int d = 0; d < dim_; ++d) {
        ptr[d] = dist(rng);
      }
      NormalizeVector(ptr, dim_);
    }

    std::vector<float> query_data(num_queries_ * dim_);
    for (int q = 0; q < num_queries_; ++q) {
      float* qptr = query_data.data() + q * dim_;
      for (int d = 0; d < dim_; ++d) {
        qptr[d] = dist(rng);
      }
      NormalizeVector(qptr, dim_);
    }

    std::unique_ptr<VectorIndex> index;
    Status s = VectorIndex::Open(options, dbdir, &index);
    if (!s.ok()) {
      std::cerr << "Failed to open benchmark vector index: " << s.ToString() << "\n";
      return;
    }

    // 1. Vector Ingestion Benchmark
    {
      auto start = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < num_vectors_; ++i) {
        std::string key = "vid_0:moment_" + std::to_string(i);
        const float* vec_ptr = data.data() + i * dim_;
        Status st = index->Put(key, vec_ptr, dim_);
        if (!st.ok()) {
          std::cerr << "Put failed: " << st.ToString() << "\n";
          return;
        }
      }
      index->Sync();
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = end - start;
      double vps = num_vectors_ / elapsed.count();
      double mbps = raw_mb / elapsed.count();

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(1);
      ss << "vec_ingest              : " << std::setw(8) << elapsed.count() << " sec | "
         << std::setw(10) << vps << " vectors/sec | "
         << std::setw(8) << mbps << " MB/sec";
      log_and_print(ss.str());
    }

    // 2. Brute-Force SIMD Cosine Top-10 Search
    {
      const size_t k = 10;
      auto start = std::chrono::high_resolution_clock::now();
      for (int q = 0; q < num_queries_; ++q) {
        const float* qptr = query_data.data() + q * dim_;
        std::vector<SearchResult> results;
        Status st = index->Search(qptr, dim_, k, &results);
        if (!st.ok() || results.size() != k) {
          std::cerr << "Search failed\n";
          return;
        }
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = end - start;
      double qps = num_queries_ / elapsed.count();
      double avg_latency_ms = (elapsed.count() * 1000.0) / num_queries_;

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(2);
      ss << "vec_search_top10 (SIMD) : " << std::setw(8) << elapsed.count() << " sec | "
         << std::setw(10) << qps << " QPS         | "
         << std::setw(8) << avg_latency_ms << " ms/query";
      log_and_print(ss.str());
    }

    // 3. Sequential Segment Scan & CRC Verification
    {
      std::string segment_file = dbdir + "/000001.vec";
      auto start = std::chrono::high_resolution_clock::now();
      std::unique_ptr<VectorSegmentReader> reader;
      s = VectorSegmentReader::Open(env, segment_file, &reader);
      if (!s.ok()) {
        std::cerr << "Reader open failed: " << s.ToString() << "\n";
        return;
      }
      std::vector<float> read_all;
      s = reader->ReadAllVectors(&read_all);
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> elapsed = end - start;
      double vps = reader->VectorCount() / elapsed.count();
      double mbps = raw_mb / elapsed.count();

      std::ostringstream ss;
      ss << std::fixed << std::setprecision(1);
      ss << "vec_segment_scan (CRC)  : " << std::setw(8) << elapsed.count() << " sec | "
         << std::setw(10) << vps << " vectors/sec | "
         << std::setw(8) << mbps << " MB/sec";
      log_and_print(ss.str());
    }

    log_and_print("-------------------------------------------------");
    log_and_print("Benchmark completed successfully.");

    // Cleanup
    if (env->GetChildren(dbdir, &children).ok()) {
      for (const auto& c : children) {
        if (c != "." && c != "..") {
          env->DeleteFile(dbdir + "/" + c);
        }
      }
      env->DeleteDir(dbdir);
    }
  }

 private:
  int num_vectors_;
  int dim_;
  int num_queries_;
};

}  // namespace bench
}  // namespace purpledb

int main(int argc, char** argv) {
  int num_vectors = 10000;
  int dim = 512;
  int num_queries = 200;

  if (argc > 1) num_vectors = std::atoi(argv[1]);
  if (argc > 2) dim = std::atoi(argv[2]);
  if (argc > 3) num_queries = std::atoi(argv[3]);

  purpledb::bench::VectorBenchmark bm(num_vectors, dim, num_queries);
  bm.Run();
  return 0;
}

