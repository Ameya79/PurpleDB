#include "purpledb/db.h"
#include "core/storage/db_impl.h"
#include "util/test_harness.h"

#include <string>

namespace purpledb {

TEST(DBTest, BasicPutGetDelete) {
  Env* env = Env::Default();
  std::string dbname = "test_db_basic";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  // Put
  ASSERT_STATUS_OK(db->Put(Slice("hello"), Slice("world")));
  ASSERT_STATUS_OK(db->Put(Slice("purple"), Slice("db")));

  // Get
  std::string val;
  ASSERT_STATUS_OK(db->Get(Slice("hello"), &val));
  ASSERT_EQ(val, "world");

  ASSERT_STATUS_OK(db->Get(Slice("purple"), &val));
  ASSERT_EQ(val, "db");

  // Non-existent key
  Status s = db->Get(Slice("missing"), &val);
  ASSERT_TRUE(s.IsNotFound());

  // Overwrite
  ASSERT_STATUS_OK(db->Put(Slice("hello"), Slice("everyone")));
  ASSERT_STATUS_OK(db->Get(Slice("hello"), &val));
  ASSERT_EQ(val, "everyone");

  // Delete
  ASSERT_STATUS_OK(db->Delete(Slice("hello")));
  s = db->Get(Slice("hello"), &val);
  ASSERT_TRUE(s.IsNotFound());

  // Key that was deleted
  ASSERT_STATUS_OK(db->Get(Slice("purple"), &val));
  ASSERT_EQ(val, "db");

  db.reset();
  env->DeleteDir(dbname);
}

TEST(DBTest, ReopenPersistence) {
  Env* env = Env::Default();
  std::string dbname = "test_db_reopen";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;

  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    for (int i = 0; i < 100; ++i) {
      std::string k = "key_" + std::to_string(i);
      std::string v = "val_" + std::to_string(i);
      ASSERT_STATUS_OK(db->Put(Slice(k), Slice(v)));
    }

    // Delete some keys
    ASSERT_STATUS_OK(db->Delete(Slice("key_10")));
    ASSERT_STATUS_OK(db->Delete(Slice("key_50")));

    // Close database by destroying unique_ptr
    db.reset();
  }

  // Reopen
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    std::string val;
    for (int i = 0; i < 100; ++i) {
      std::string k = "key_" + std::to_string(i);
      if (i == 10 || i == 50) {
        Status s = db->Get(Slice(k), &val);
        ASSERT_TRUE(s.IsNotFound());
      } else {
        ASSERT_STATUS_OK(db->Get(Slice(k), &val));
        ASSERT_EQ(val, "val_" + std::to_string(i));
      }
    }

    // Append more data in reopened DB
    ASSERT_STATUS_OK(db->Put(Slice("key_new"), Slice("val_new")));
    db.reset();
  }

  // Second reopen
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    std::string val;
    ASSERT_STATUS_OK(db->Get(Slice("key_new"), &val));
    ASSERT_EQ(val, "val_new");
    db.reset();
  }

  env->DeleteDir(dbname);
}

TEST(DBTest, AtomicWriteBatch) {
  Env* env = Env::Default();
  std::string dbname = "test_db_batch";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  WriteBatch batch;
  batch.Put(Slice("a"), Slice("1"));
  batch.Put(Slice("b"), Slice("2"));
  batch.Put(Slice("c"), Slice("3"));
  batch.Delete(Slice("b"));

  ASSERT_STATUS_OK(db->Write(WriteOptions(), &batch));

  std::string val;
  ASSERT_STATUS_OK(db->Get(Slice("a"), &val));
  ASSERT_EQ(val, "1");

  ASSERT_TRUE(db->Get(Slice("b"), &val).IsNotFound());

  ASSERT_STATUS_OK(db->Get(Slice("c"), &val));
  ASSERT_EQ(val, "3");

  db.reset();
  env->DeleteDir(dbname);
}

TEST(DBTest, MemTableFlushOnFullAndSSTableRead) {
  Env* env = Env::Default();
  std::string dbname = "test_db_flush";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  // Set tiny write buffer (4 KiB) so that writing a few hundred entries triggers multiple flushes
  options.write_buffer_size = 4096;
  options.block_size = 512;

  const int N = 300;
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    for (int i = 0; i < N; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k%05d", i);
      char vbuf[64];
      std::snprintf(vbuf, sizeof(vbuf), "value_string_payload_%05d", i);
      ASSERT_STATUS_OK(db->Put(Slice(kbuf), Slice(vbuf)));
    }

    // Read back while database is still open (some keys in MemTable, some in SSTables)
    for (int i = 0; i < N; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k%05d", i);
      char expected[64];
      std::snprintf(expected, sizeof(expected), "value_string_payload_%05d", i);

      std::string val;
      ASSERT_STATUS_OK(db->Get(Slice(kbuf), &val));
      ASSERT_EQ(val, expected);
    }

    db.reset();
  }

  // Verify that SSTables were actually generated on disk
  std::vector<std::string> children;
  ASSERT_STATUS_OK(env->GetChildren(dbname, &children));
  int sst_count = 0;
  for (const auto& f : children) {
    if (f.find(".sst") != std::string::npos) {
      sst_count++;
    }
  }
  ASSERT_GT(sst_count, 0);

  // Reopen and read everything back from SSTables + MANIFEST
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

    for (int i = 0; i < N; ++i) {
      char kbuf[32];
      std::snprintf(kbuf, sizeof(kbuf), "k%05d", i);
      char expected[64];
      std::snprintf(expected, sizeof(expected), "value_string_payload_%05d", i);

      std::string val;
      ASSERT_STATUS_OK(db->Get(Slice(kbuf), &val));
      ASSERT_EQ(val, expected);
    }
    db.reset();
  }

  env->DeleteDir(dbname);
}

TEST(DBTest, OverlappingL0Precedence) {
  Env* env = Env::Default();
  std::string dbname = "test_db_overlapping_l0";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;

  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    DBImpl* impl = static_cast<DBImpl*>(db.get());

    // 1. Put key="user1", val="v1" -> flush to L0 file 1
    ASSERT_STATUS_OK(db->Put(Slice("user1"), Slice("v1")));
    ASSERT_STATUS_OK(impl->FlushMemTable());

    std::string val;
    ASSERT_STATUS_OK(db->Get(Slice("user1"), &val));
    ASSERT_EQ(val, "v1");

    // 2. Put key="user1", val="v2" -> flush to L0 file 2
    ASSERT_STATUS_OK(db->Put(Slice("user1"), Slice("v2")));
    ASSERT_STATUS_OK(impl->FlushMemTable());

    // Must resolve to "v2" because file 2 is newer than file 1
    ASSERT_STATUS_OK(db->Get(Slice("user1"), &val));
    ASSERT_EQ(val, "v2");

    // 3. Delete key="user1" -> flush to L0 file 3
    ASSERT_STATUS_OK(db->Delete(Slice("user1")));
    ASSERT_STATUS_OK(impl->FlushMemTable());

    // Must resolve to NotFound because newest L0 has tombstone
    Status s = db->Get(Slice("user1"), &val);
    ASSERT_TRUE(s.IsNotFound());

    db.reset();
  }

  // Reopen and verify tombstone precedence survives restart
  {
    std::unique_ptr<DB> db;
    ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
    std::string val;
    Status s = db->Get(Slice("user1"), &val);
    ASSERT_TRUE(s.IsNotFound());
    db.reset();
  }

  env->DeleteDir(dbname);
}

TEST(DBTest, SnapshotIsolation) {
  Env* env = Env::Default();
  std::string dbname = "test_db_snapshot";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  ASSERT_STATUS_OK(db->Put(Slice("k1"), Slice("v1")));
  ASSERT_STATUS_OK(db->Put(Slice("k2"), Slice("v2")));

  // Take snapshot at state {k1: v1, k2: v2}
  const Snapshot* snap1 = db->GetSnapshot();
  ASSERT_TRUE(snap1 != nullptr);

  // Subsequent mutations
  ASSERT_STATUS_OK(db->Put(Slice("k1"), Slice("v1_new")));
  ASSERT_STATUS_OK(db->Delete(Slice("k2")));
  ASSERT_STATUS_OK(db->Put(Slice("k3"), Slice("v3")));

  // 1. Current reads reflect latest mutations
  std::string val;
  ASSERT_STATUS_OK(db->Get(Slice("k1"), &val));
  ASSERT_EQ(val, "v1_new");

  Status s = db->Get(Slice("k2"), &val);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_STATUS_OK(db->Get(Slice("k3"), &val));
  ASSERT_EQ(val, "v3");

  // 2. Snapshot reads reflect immutable historical state
  ReadOptions snap_opts;
  snap_opts.snapshot = snap1;

  ASSERT_STATUS_OK(db->Get(snap_opts, Slice("k1"), &val));
  ASSERT_EQ(val, "v1");

  ASSERT_STATUS_OK(db->Get(snap_opts, Slice("k2"), &val));
  ASSERT_EQ(val, "v2");

  s = db->Get(snap_opts, Slice("k3"), &val);
  ASSERT_TRUE(s.IsNotFound());

  db->ReleaseSnapshot(snap1);
  db.reset();
  env->DeleteDir(dbname);
}

TEST(DBTest, IteratorTraversalAcrossMemTableAndSSTables) {
  Env* env = Env::Default();
  std::string dbname = "test_db_iter_traversal";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
  DBImpl* impl = static_cast<DBImpl*>(db.get());

  // L0 SSTable: {a: 1, c: 3}
  ASSERT_STATUS_OK(db->Put(Slice("a"), Slice("1")));
  ASSERT_STATUS_OK(db->Put(Slice("c"), Slice("3")));
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // MemTable: {b: 2, d: 4, e: 5, delete c}
  ASSERT_STATUS_OK(db->Put(Slice("b"), Slice("2")));
  ASSERT_STATUS_OK(db->Put(Slice("d"), Slice("4")));
  ASSERT_STATUS_OK(db->Put(Slice("e"), Slice("5")));
  ASSERT_STATUS_OK(db->Delete(Slice("c")));

  auto iter = db->NewIterator(ReadOptions());
  ASSERT_TRUE(iter != nullptr);

  // Forward scan: must see a, b, d, e (c is deleted)
  std::vector<std::pair<std::string, std::string>> entries;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    entries.push_back({iter->key().ToString(), iter->value().ToString()});
  }

  ASSERT_EQ(entries.size(), 4u);
  ASSERT_EQ(entries[0].first, "a");
  ASSERT_EQ(entries[0].second, "1");
  ASSERT_EQ(entries[1].first, "b");
  ASSERT_EQ(entries[1].second, "2");
  ASSERT_EQ(entries[2].first, "d");
  ASSERT_EQ(entries[2].second, "4");
  ASSERT_EQ(entries[3].first, "e");
  ASSERT_EQ(entries[3].second, "5");

  // Seek("c") should land on "d"
  iter->Seek(Slice("c"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key(), Slice("d"));

  // SeekToLast() should land on "e"
  iter->SeekToLast();
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key(), Slice("e"));

  iter.reset();
  db.reset();
  env->DeleteDir(dbname);
}

TEST(DBTest, BackgroundCompactionClearsL0) {
  Env* env = Env::Default();
  std::string dbname = "test_db_bg_compaction";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));
  auto* impl = static_cast<DBImpl*>(db.get());

  for (int i = 0; i < 5; ++i) {
    ASSERT_STATUS_OK(db->Put(Slice("k" + std::to_string(i)), Slice("v" + std::to_string(i))));
    ASSERT_STATUS_OK(impl->FlushMemTable());
  }

  // After 5 flushes, L0 has >= 4 files which triggers background compaction.
  // Wait up to 2 seconds for background thread to compact L0 to L1.
  for (int retry = 0; retry < 200; ++retry) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // Verify all keys are readable
  for (int i = 0; i < 5; ++i) {
    std::string val;
    ASSERT_STATUS_OK(db->Get(Slice("k" + std::to_string(i)), &val));
    ASSERT_EQ(val, "v" + std::to_string(i));
  }

  db.reset();
  env->DeleteDir(dbname);
}

TEST(DBTest, OrphanFileCleanupOnOpen) {
  Env* env = Env::Default();
  std::string dbname = "test_db_orphan_cleanup";
  env->DeleteDir(dbname);

  Options options;
  options.create_if_missing = true;
  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  ASSERT_STATUS_OK(db->Put(Slice("real_key"), Slice("real_val")));
  auto* impl = static_cast<DBImpl*>(db.get());
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // Close DB
  db.reset();

  // Create orphan files: a fake .sst with high number and a .dbtmp file
  std::string orphan_sst = dbname + "/999999.sst";
  std::string orphan_tmp = dbname + "/888888.dbtmp";
  std::unique_ptr<WritableFile> wf;
  ASSERT_STATUS_OK(env->NewWritableFile(orphan_sst, &wf));
  ASSERT_STATUS_OK(wf->Append(Slice("garbage sst data")));
  ASSERT_STATUS_OK(wf->Close());

  ASSERT_STATUS_OK(env->NewWritableFile(orphan_tmp, &wf));
  ASSERT_STATUS_OK(wf->Append(Slice("incomplete tmp data")));
  ASSERT_STATUS_OK(wf->Close());

  ASSERT_TRUE(env->FileExists(orphan_sst));
  ASSERT_TRUE(env->FileExists(orphan_tmp));

  // Reopen DB - Recover() should trigger RemoveOrphanFiles()
  ASSERT_STATUS_OK(DB::Open(options, dbname, &db));

  // The orphan files should have been wiped
  ASSERT_FALSE(env->FileExists(orphan_sst));
  ASSERT_FALSE(env->FileExists(orphan_tmp));

  // Real data must still be accessible
  std::string val;
  ASSERT_STATUS_OK(db->Get(Slice("real_key"), &val));
  ASSERT_EQ(val, "real_val");

  db.reset();
  env->DeleteDir(dbname);
}

}  // namespace purpledb
