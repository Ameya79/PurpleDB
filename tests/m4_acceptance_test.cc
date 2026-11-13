#include "purpledb/db.h"
#include "core/storage/compaction.h"
#include "core/storage/db_impl.h"
#include "core/storage/env.h"
#include "core/storage/filename.h"
#include "purpledb/filter_policy.h"
#include "util/test_harness.h"

#include <chrono>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace purpledb {

TEST(M4AcceptanceTest, ModelTestWithCompaction) {
  Env* env = Env::Default();
  std::string dbname = "test_m4_model";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 4096;  // Small write buffer for frequent flushes
  options.block_size = 512;
  auto filter = NewBloomFilterPolicy(10);
  options.filter_policy = filter.get();

  std::map<std::string, std::string> oracle;
  std::mt19937 rnd(12345);

  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
  auto* impl = static_cast<DBImpl*>(db.get());

  const int kTotalOps = 1500;
  for (int i = 0; i < kTotalOps; ++i) {
    int op = rnd() % 10;
    int key_num = rnd() % 150;
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "k%04d", key_num);
    std::string key(kbuf);

    if (op < 5) {
      // 50% Put
      char vbuf[64];
      std::snprintf(vbuf, sizeof(vbuf), "v%04d_i%d", key_num, i);
      std::string val(vbuf);
      ASSERT_STATUS_OK(db->Put(Slice(key), Slice(val)));
      oracle[key] = val;
    } else if (op < 7) {
      // 20% Delete
      ASSERT_STATUS_OK(db->Delete(Slice(key)));
      oracle.erase(key);
    } else if (op < 9) {
      // 20% Point Get
      std::string val;
      Status s = db->Get(Slice(key), &val);
      auto it = oracle.find(key);
      if (it == oracle.end()) {
        ASSERT_TRUE(s.IsNotFound());
      } else {
        ASSERT_STATUS_OK(s);
        ASSERT_EQ(val, it->second);
      }
    } else {
      // 10% Manual or Background Compaction trigger
      impl->CompactOnce();
    }

    // Periodically reopen DB to verify manifest replay & orphan file safety
    if (i == 500 || i == 1000) {
      db.reset();
      ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
      impl = static_cast<DBImpl*>(db.get());
    }
  }

  // Full scan verification via DBIter
  auto iter = db->NewIterator(ReadOptions());
  iter->SeekToFirst();
  auto oracle_it = oracle.begin();
  size_t count = 0;

  while (iter->Valid() && oracle_it != oracle.end()) {
    ASSERT_EQ(iter->key().ToString(), oracle_it->first);
    ASSERT_EQ(iter->value().ToString(), oracle_it->second);
    iter->Next();
    ++oracle_it;
    ++count;
  }

  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(oracle_it == oracle.end());
  ASSERT_EQ(count, oracle.size());

  iter.reset();
  db.reset();
  env->DeleteDir(dbname);
}

TEST(M4AcceptanceTest, SpaceAmplificationBoundedOnDeletes) {
  Env* env = Env::Default();
  std::string dbname = "test_m4_space_amp";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.write_buffer_size = 16384;
  options.block_size = 1024;
  auto filter = NewBloomFilterPolicy(10);
  options.filter_policy = filter.get();

  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
  auto* impl = static_cast<DBImpl*>(db.get());

  // 1. Write 3,000 keys with 120-byte payload
  const int kNumKeys = 3000;
  for (int i = 0; i < kNumKeys; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i);
    std::string val(120, static_cast<char>('A' + (i % 26)));
    ASSERT_STATUS_OK(db->Put(Slice(kbuf), Slice(val)));
  }
  ASSERT_STATUS_OK(impl->FlushMemTable());

  auto get_sstable_bytes = [&]() -> uint64_t {
    std::vector<std::string> files;
    env->GetChildren(dbname, &files);
    uint64_t total = 0;
    for (const auto& f : files) {
      if (f.size() > 4 && f.substr(f.size() - 4) == ".sst") {
        uint64_t sz = 0;
        env->GetFileSize(dbname + "/" + f, &sz);
        total += sz;
      }
    }
    return total;
  };

  uint64_t initial_bytes = get_sstable_bytes();
  ASSERT_GT(initial_bytes, 0u);

  // 2. Delete 2,700 keys (90% deleted)
  for (int i = 0; i < 2700; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i);
    ASSERT_STATUS_OK(db->Delete(Slice(kbuf)));
  }
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // 3. Compact L0 into L1 and let leveled compaction deduplicate and drop tombstones
  for (int c = 0; c < 10; ++c) {
    impl->CompactOnce();
  }

  uint64_t compacted_bytes = get_sstable_bytes();

  // Space amplification bounded: with 90% of keys deleted, SSTable size must shrink
  // significantly (compacted size must be less than 50% of peak initial size)
  ASSERT_LT(compacted_bytes, initial_bytes / 2);

  // 4. Verify remaining 300 keys are still intact
  for (int i = 2700; i < kNumKeys; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i);
    std::string val;
    ASSERT_STATUS_OK(db->Get(Slice(kbuf), &val));
    std::string expected(120, static_cast<char>('A' + (i % 26)));
    ASSERT_EQ(val, expected);
  }

  // 5. Verify deleted keys return NotFound
  for (int i = 0; i < 2700; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key_%05d", i);
    std::string val;
    Status s = db->Get(Slice(kbuf), &val);
    ASSERT_TRUE(s.IsNotFound());
  }

  db.reset();
  env->DeleteDir(dbname);
}

TEST(M4AcceptanceTest, KillLoopWithCompaction) {
  Env* env = Env::Default();
  std::string dbname = "test_m4_kill_loop";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.sync = true;
  options.write_buffer_size = 4096;
  auto filter = NewBloomFilterPolicy(10);
  options.filter_policy = filter.get();

  std::map<std::string, std::string> committed_oracle;
  std::mt19937 rng(54321);
  std::uniform_int_distribution<int> key_dist(0, 39);
  std::uniform_int_distribution<int> val_dist(0, 99999);
  std::uniform_int_distribution<int> batch_size_dist(1, 5);

  const int kIterations = 500;
  for (int iter = 0; iter < kIterations; ++iter) {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    auto* impl = static_cast<DBImpl*>(db.get());

    // Verify current state
    for (int k = 0; k < 40; ++k) {
      std::string key = "k" + std::to_string(k);
      std::string val;
      Status s = db->Get(Slice(key), &val);
      auto it = committed_oracle.find(key);
      if (it == committed_oracle.end()) {
        ASSERT_TRUE(s.IsNotFound());
      } else {
        ASSERT_STATUS_OK(s);
        ASSERT_EQ(val, it->second);
      }
    }

    // Apply 1 to 5 mutations
    int batch_size = batch_size_dist(rng);
    WriteBatch batch;
    for (int b = 0; b < batch_size; ++b) {
      std::string k = "k" + std::to_string(key_dist(rng));
      if ((iter + b) % 3 == 0) {
        batch.Delete(Slice(k));
        committed_oracle.erase(k);
      } else {
        std::string v = "v_" + std::to_string(val_dist(rng));
        batch.Put(Slice(k), Slice(v));
        committed_oracle[k] = v;
      }
    }

    WriteOptions wo;
    wo.sync = true;
    ASSERT_STATUS_OK(db->Write(wo, &batch));

    if (iter % 7 == 0) {
      impl->FlushMemTable();
    }
    if (iter % 13 == 0) {
      impl->CompactOnce();
    }

    // Abrupt kill / close
    db.reset();
  }

  // Final verification
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    for (int k = 0; k < 40; ++k) {
      std::string key = "k" + std::to_string(k);
      std::string val;
      Status s = db->Get(Slice(key), &val);
      auto it = committed_oracle.find(key);
      if (it == committed_oracle.end()) {
        ASSERT_TRUE(s.IsNotFound());
      } else {
        ASSERT_STATUS_OK(s);
        ASSERT_EQ(val, it->second);
      }
    }
    db.reset();
  }

  env->DeleteDir(dbname);
}

}  // namespace purpledb
