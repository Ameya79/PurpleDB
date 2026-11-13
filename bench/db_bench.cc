#include "purpledb/db.h"
#include "core/storage/env.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace purpledb {
namespace bench {

class Benchmark {
 public:
  Benchmark(int num_keys, int value_size)
      : num_keys_(num_keys), value_size_(value_size) {}

  void Run() {
    Env* env = Env::Default();
    std::string dbdir = "bench_purpledb_data";
    env->DeleteDir(dbdir);

    Options options;
    options.create_if_missing = true;
    options.write_buffer_size = 4 * 1024 * 1024;  // 4 MiB
    options.block_size = 4096;

    std::filesystem::create_directories("bench/results");
    std::ofstream out("bench/results/m3_benchmark.txt");

    auto log_and_print = [&](const std::string& line) {
      std::cout << line << "\n";
      out << line << "\n";
    };

    log_and_print("=================================================");
    log_and_print("       PurpleDB Storage Engine Benchmark (M3)    ");
    log_and_print("=================================================");
    log_and_print("Keys: " + std::to_string(num_keys_) + " | Value Size: " +
                  std::to_string(value_size_) + " bytes");
    log_and_print("Write Buffer Size: 4 MiB | Block Size: 4 KiB");
    log_and_print("-------------------------------------------------");

    std::unique_ptr<DB> db;
    Status s = DB::Open(options, dbdir, &db);
    if (!s.ok()) {
      std::cerr << "Failed to open benchmark db: " << s.ToString() << "\n";
      return;
    }

    // 1. Sequential Write (fillseq)
    {
      std::string val(value_size_, 'x');
      auto start = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < num_keys_; ++i) {
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "key_%08d", i);
        db->Put(Slice(kbuf), Slice(val));
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> sec = end - start;
      double ops_per_sec = num_keys_ / sec.count();
      double mb_per_sec = (static_cast<double>(num_keys_) * (16 + value_size_)) /
                          (sec.count() * 1024 * 1024);

      char buf[128];
      std::snprintf(buf, sizeof(buf), "fillseq    : %10.1f ops/sec | %6.2f MB/sec (%.3f s)",
                    ops_per_sec, mb_per_sec, sec.count());
      log_and_print(buf);
    }

    // 2. Random Point Read (readrandom)
    {
      std::mt19937 rnd(42);
      std::string val;
      auto start = std::chrono::high_resolution_clock::now();
      int found = 0;
      for (int i = 0; i < num_keys_; ++i) {
        int k = rnd() % num_keys_;
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "key_%08d", k);
        if (db->Get(Slice(kbuf), &val).ok()) {
          found++;
        }
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> sec = end - start;
      double ops_per_sec = num_keys_ / sec.count();
      double mb_per_sec = (static_cast<double>(num_keys_) * (16 + value_size_)) /
                          (sec.count() * 1024 * 1024);

      char buf[128];
      std::snprintf(buf, sizeof(buf), "readrandom : %10.1f ops/sec | %6.2f MB/sec (found %d/%d)",
                    ops_per_sec, mb_per_sec, found, num_keys_);
      log_and_print(buf);
    }

    // 3. Random Write (fillrandom)
    {
      std::mt19937 rnd(1234);
      std::string val(value_size_, 'y');
      auto start = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < num_keys_; ++i) {
        int k = rnd() % (num_keys_ * 2);
        char kbuf[32];
        std::snprintf(kbuf, sizeof(kbuf), "key_%08d", k);
        db->Put(Slice(kbuf), Slice(val));
      }
      auto end = std::chrono::high_resolution_clock::now();
      std::chrono::duration<double> sec = end - start;
      double ops_per_sec = num_keys_ / sec.count();
      double mb_per_sec = (static_cast<double>(num_keys_) * (16 + value_size_)) /
                          (sec.count() * 1024 * 1024);

      char buf[128];
      std::snprintf(buf, sizeof(buf), "fillrandom : %10.1f ops/sec | %6.2f MB/sec (%.3f s)",
                    ops_per_sec, mb_per_sec, sec.count());
      log_and_print(buf);
    }

    log_and_print("=================================================");

    db.reset();
    env->DeleteDir(dbdir);
  }

 private:
  int num_keys_;
  int value_size_;
};

}  // namespace bench
}  // namespace purpledb

int main(int argc, char** argv) {
  int num_keys = 20000;
  int value_size = 100;
  if (argc > 1) {
    num_keys = std::atoi(argv[1]);
  }
  purpledb::bench::Benchmark b(num_keys, value_size);
  b.Run();
  return 0;
}

