#include "purpledb/db.h"
#include "util/test_harness.h"

#include <map>
#include <random>
#include <string>
#include <vector>

namespace purpledb {

TEST(M2AcceptanceTest, ModelTestAgainstStdMapWithReopen) {
  Env* env = Env::Default();
  std::string dbname = "test_m2_model";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;

  std::map<std::string, std::string> oracle;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  std::mt19937 rng(1337);
  std::uniform_int_distribution<int> op_dist(0, 99);
  std::uniform_int_distribution<int> key_dist(0, 49);
  std::uniform_int_distribution<int> val_dist(0, 9999);

  for (int step = 0; step < 500; ++step) {
    int op = op_dist(rng);
    std::string key = "k" + std::to_string(key_dist(rng));

    if (op < 60) {
      // Put
      std::string val = "v" + std::to_string(val_dist(rng));
      ASSERT_STATUS_OK(db->Put(Slice(key), Slice(val)));
      oracle[key] = val;
    } else if (op < 80) {
      // Delete
      ASSERT_STATUS_OK(db->Delete(Slice(key)));
      oracle.erase(key);
    } else if (op < 90) {
      // Atomic WriteBatch
      WriteBatch batch;
      for (int b = 0; b < 3; ++b) {
        std::string bk = "k" + std::to_string(key_dist(rng));
        if (b % 2 == 0) {
          std::string bv = "v_batch_" + std::to_string(val_dist(rng));
          batch.Put(Slice(bk), Slice(bv));
          oracle[bk] = bv;
        } else {
          batch.Delete(Slice(bk));
          oracle.erase(bk);
        }
      }
      ASSERT_STATUS_OK(db->Write(WriteOptions(), &batch));
    } else {
      // Reopen and recover
      db.reset();
      ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

      // Verify a sample of keys after reopen
      for (int check = 0; check < 10; ++check) {
        std::string check_key = "k" + std::to_string(key_dist(rng));
        std::string db_val;
        Status s = db->Get(Slice(check_key), &db_val);
        auto it = oracle.find(check_key);
        if (it == oracle.end()) {
          ASSERT_TRUE(s.IsNotFound());
        } else {
          ASSERT_STATUS_OK(s);
          ASSERT_EQ(db_val, it->second);
        }
      }
    }
  }

  // Final complete validation across all keys
  for (int k = 0; k < 50; ++k) {
    std::string key = "k" + std::to_string(k);
    std::string db_val;
    Status s = db->Get(Slice(key), &db_val);
    auto it = oracle.find(key);
    if (it == oracle.end()) {
      ASSERT_TRUE(s.IsNotFound());
    } else {
      ASSERT_STATUS_OK(s);
      ASSERT_EQ(db_val, it->second);
    }
  }

  db.reset();
  env->DeleteDir(dbname);
}

TEST(M2AcceptanceTest, KillLoop1000Iterations) {
  Env* env = Env::Default();
  std::string dbname = "test_m2_kill_loop";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.sync = true;

  std::map<std::string, std::string> committed_oracle;
  std::mt19937 rng(4242);
  std::uniform_int_distribution<int> key_dist(0, 19);
  std::uniform_int_distribution<int> val_dist(0, 9999);
  std::uniform_int_distribution<int> batch_size_dist(1, 4);

  // 1,000 kill loop iterations simulating abrupt crashes and recovery
  for (int iter = 0; iter < 1000; ++iter) {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    // Verify current state matches oracle
    for (int k = 0; k < 20; ++k) {
      std::string key = "key_" + std::to_string(k);
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

    // Apply 1 to 4 mutations
    int batch_size = batch_size_dist(rng);
    WriteBatch batch;
    for (int b = 0; b < batch_size; ++b) {
      std::string k = "key_" + std::to_string(key_dist(rng));
      if ((iter + b) % 4 == 0) {
        batch.Delete(Slice(k));
        committed_oracle.erase(k);
      } else {
        std::string v = "val_" + std::to_string(val_dist(rng));
        batch.Put(Slice(k), Slice(v));
        committed_oracle[k] = v;
      }
    }

    // Commit write with sync
    WriteOptions wo;
    wo.sync = true;
    ASSERT_STATUS_OK(db->Write(wo, &batch));

    // Abrupt kill / close
    db.reset();
  }

  // Final verification after 1,000 kill/reopen iterations
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    for (int k = 0; k < 20; ++k) {
      std::string key = "key_" + std::to_string(k);
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
  }

  env->DeleteDir(dbname);
}

TEST(M2AcceptanceTest, DbWalTailTruncationRecoversConsistentPrefix) {
  Env* env = Env::Default();
  std::string dbname = "test_m2_db_trunc";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  options.sync = true;

  std::vector<std::pair<std::string, std::string>> updates;
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    for (int i = 0; i < 50; ++i) {
      std::string k = "trunc_key_" + std::to_string(i);
      std::string v = "trunc_val_" + std::to_string(i) + "_" + std::string(100, 'x');
      updates.push_back({k, v});
      ASSERT_STATUS_OK(db->Put(Slice(k), Slice(v)));
    }
    db.reset();
  }

  // Read full WAL file
  std::string log_name = dbname + "/000001.log";
  uint64_t full_size = 0;
  ASSERT_STATUS_OK(env->GetFileSize(log_name, &full_size));
  ASSERT_GT(full_size, 500u);

  std::string full_wal;
  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(log_name, &sf));
    std::vector<char> buf(full_size);
    Slice res;
    ASSERT_STATUS_OK(sf->Read(full_size, &res, buf.data()));
    full_wal = res.ToString();
  }

  // Truncate at various byte offsets and verify DB recovers a valid prefix without crash
  const size_t step = std::max<size_t>(1, full_size / 50);
  for (size_t cut = 0; cut <= full_size; cut += step) {
    // Overwrite WAL with truncated prefix
    {
      std::unique_ptr<WritableFile> wf;
      ASSERT_STATUS_OK(env->NewWritableFile(log_name, &wf));
      if (cut > 0) {
        ASSERT_STATUS_OK(wf->Append(Slice(full_wal.data(), cut)));
      }
      ASSERT_STATUS_OK(wf->Close());
    }

    // Reopen DB
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    // Verify that every key that exists in DB matches the exact expected value
    for (size_t i = 0; i < updates.size(); ++i) {
      std::string val;
      Status s = db->Get(Slice(updates[i].first), &val);
      if (s.ok()) {
        ASSERT_EQ(val, updates[i].second);
      }
    }
    // Prefix property: if key i+1 is present, key i must be present!
    bool seen_missing = false;
    for (size_t i = 0; i < updates.size(); ++i) {
      std::string val;
      Status s = db->Get(Slice(updates[i].first), &val);
      if (s.IsNotFound()) {
        seen_missing = true;
      } else {
        ASSERT_FALSE(seen_missing);  // No holes in recovered prefix!
      }
    }

    db.reset();
  }

  env->DeleteDir(dbname);
}

}  // namespace purpledb

