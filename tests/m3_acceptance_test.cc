#include "purpledb/db.h"
#include "core/storage/block.h"
#include "core/storage/db_impl.h"
#include "core/storage/env.h"
#include "core/storage/filename.h"
#include "core/storage/table.h"
#include "util/test_harness.h"

#include <map>
#include <random>
#include <string>
#include <vector>

namespace purpledb {

TEST(M3AcceptanceTest, ModelTestWithFlushAndReopen) {
  Env* env = Env::Default();
  std::string dbname = "test_m3_model";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 4096;  // Small buffer to trigger frequent flushes
  options.block_size = 512;

  std::map<std::string, std::string> oracle;
  std::mt19937 rnd(12345);

  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  const int kTotalOps = 1000;
  for (int i = 0; i < kTotalOps; ++i) {
    int op = rnd() % 10;
    int key_num = rnd() % 100;
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%03d", key_num);
    std::string key(kbuf);

    if (op < 6) {
      // 60% Put
      char vbuf[64];
      std::snprintf(vbuf, sizeof(vbuf), "val_%03d_iter_%d", key_num, i);
      std::string val(vbuf);
      ASSERT_STATUS_OK(db->Put(Slice(key), Slice(val)));
      oracle[key] = val;
    } else if (op < 8) {
      // 20% Delete
      ASSERT_STATUS_OK(db->Delete(Slice(key)));
      oracle.erase(key);
    } else {
      // 20% Point Get verification
      std::string val;
      Status s = db->Get(Slice(key), &val);
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        ASSERT_TRUE(s.IsNotFound());
      } else {
        ASSERT_STATUS_OK(s);
        ASSERT_EQ(val, it->second);
      }
    }

    // Periodically reopen the database (every 200 ops)
    if (i > 0 && i % 200 == 0) {
      db.reset();
      ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    }
  }

  // Final reopen and exhaustive verification against the oracle
  db.reset();
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  for (int key_num = 0; key_num < 100; ++key_num) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%03d", key_num);
    std::string key(kbuf);

    std::string val;
    Status s = db->Get(Slice(key), &val);
    auto it = oracle.find(key);
    if (it == oracle.end()) {
      ASSERT_TRUE(s.IsNotFound());
    } else {
      ASSERT_STATUS_OK(s);
      ASSERT_EQ(val, it->second);
    }
  }

  db.reset();
  env->DeleteDir(dbname);
}

TEST(M3AcceptanceTest, SSTableCorruptionSafety) {
  Env* env = Env::Default();
  std::string dbname = "test_m3_corruption";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;

  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  // Insert data and flush to SSTable
  for (int i = 0; i < 50; ++i) {
    char kbuf[32], vbuf[64];
    std::snprintf(kbuf, sizeof(kbuf), "k%04d", i);
    std::snprintf(vbuf, sizeof(vbuf), "v%04d", i);
    ASSERT_STATUS_OK(db->Put(Slice(kbuf), Slice(vbuf)));
  }

  DBImpl* impl = static_cast<DBImpl*>(db.get());
  ASSERT_STATUS_OK(impl->FlushMemTable());
  db.reset();

  // Find the SSTable file
  std::vector<std::string> children;
  ASSERT_STATUS_OK(env->GetChildren(dbname, &children));
  std::string sst_path;
  for (const auto& file : children) {
    if (file.find(".sst") != std::string::npos) {
      sst_path = dbname + "/" + file;
      break;
    }
  }
  ASSERT_FALSE(sst_path.empty());

  // Corrupt a byte in the SSTable file
  uint64_t file_size = 0;
  ASSERT_STATUS_OK(env->GetFileSize(sst_path, &file_size));
  std::vector<char> file_data(file_size);
  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(sst_path, &sf));
    Slice result;
    ASSERT_STATUS_OK(sf->Read(file_size, &result, file_data.data()));
  }

  // Flip bits in the data block
  file_data[10] ^= 0xff;

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(sst_path, &wf));
    ASSERT_STATUS_OK(wf->Append(Slice(file_data.data(), file_size)));
    ASSERT_STATUS_OK(wf->Close());
  }

  // Reopen and attempt to read: engine must return error or NotFound, NEVER crash
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
  std::string val;
  Status s = db->Get(Slice("k0010"), &val);
  // Must fail gracefully with corruption error or not found
  ASSERT_TRUE(s.IsCorruption() || s.IsNotFound());

  db.reset();
  env->DeleteDir(dbname);
}

TEST(M3AcceptanceTest, BlockAndTableFuzzer) {
  // Fuzz Block reader with 5,000 random malformed payloads
  std::mt19937 rnd(42);
  for (int iter = 0; iter < 5000; ++iter) {
    size_t len = (rnd() % 512) + 1;
    std::vector<char> random_bytes(len);
    for (size_t b = 0; b < len; ++b) {
      random_bytes[b] = static_cast<char>(rnd() & 0xff);
    }

    Block block(random_bytes.data(), random_bytes.size(), false);
    auto it = block.NewIterator();
    if (it != nullptr) {
      it->SeekToFirst();
      if (it->Valid()) {
        it->Next();
      }
      it->SeekToLast();
      if (it->Valid()) {
        it->Prev();
      }
      it->Seek(Slice("fuzzy_target"));
    }
  }
}

}  // namespace purpledb
