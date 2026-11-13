#include "purpledb/db.h"
#include "core/storage/db_impl.h"
#include "core/storage/env.h"
#include "core/storage/filename.h"
#include "util/test_harness.h"

#include <string>
#include <vector>

namespace purpledb {

TEST(CompactionTest, L0ToL1MergeAndDeduplication) {
  Env* env = Env::Default();
  std::string dbdir = "test_compaction_l0_to_l1";
  env->DeleteDir(dbdir);

  Options options;
  options.create_if_missing = true;
  options.block_size = 512;

  std::unique_ptr<DB> db;
  ASSERT_STATUS_OK(DB::Open(options, dbdir, &db));
  DBImpl* impl = static_cast<DBImpl*>(db.get());

  // 1. Create 4 L0 files to trigger L0 compaction threshold
  // File 1: {k1: v1, k2: v2}
  ASSERT_STATUS_OK(db->Put(Slice("k1"), Slice("v1")));
  ASSERT_STATUS_OK(db->Put(Slice("k2"), Slice("v2")));
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // File 2: {k2: v2_new, k3: v3}
  ASSERT_STATUS_OK(db->Put(Slice("k2"), Slice("v2_new")));
  ASSERT_STATUS_OK(db->Put(Slice("k3"), Slice("v3")));
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // File 3: {k4: v4, k5: v5}
  ASSERT_STATUS_OK(db->Put(Slice("k4"), Slice("v4")));
  ASSERT_STATUS_OK(db->Put(Slice("k5"), Slice("v5")));
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // File 4: {k2: deleted, k6: v6}
  ASSERT_STATUS_OK(db->Delete(Slice("k2")));
  ASSERT_STATUS_OK(db->Put(Slice("k6"), Slice("v6")));
  ASSERT_STATUS_OK(impl->FlushMemTable());

  // 2. Trigger compaction
  ASSERT_STATUS_OK(impl->CompactOnce());

  // 3. Verify queries after compaction
  std::string val;
  ASSERT_STATUS_OK(db->Get(Slice("k1"), &val));
  ASSERT_EQ(val, "v1");

  // k2 was deleted, must return NotFound
  Status s = db->Get(Slice("k2"), &val);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_STATUS_OK(db->Get(Slice("k3"), &val));
  ASSERT_EQ(val, "v3");

  ASSERT_STATUS_OK(db->Get(Slice("k4"), &val));
  ASSERT_EQ(val, "v4");

  ASSERT_STATUS_OK(db->Get(Slice("k5"), &val));
  ASSERT_EQ(val, "v5");

  ASSERT_STATUS_OK(db->Get(Slice("k6"), &val));
  ASSERT_EQ(val, "v6");

  // 4. Verify user iterator produces clean sorted entries without tombstones or duplicates
  auto iter = db->NewIterator(ReadOptions());
  std::vector<std::string> keys;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    keys.push_back(iter->key().ToString());
  }
  ASSERT_EQ(keys.size(), 5u);
  ASSERT_EQ(keys[0], "k1");
  ASSERT_EQ(keys[1], "k3");
  ASSERT_EQ(keys[2], "k4");
  ASSERT_EQ(keys[3], "k5");
  ASSERT_EQ(keys[4], "k6");
  iter.reset();

  // 5. Reopen and verify persistence
  db.reset();
  ASSERT_STATUS_OK(DB::Open(options, dbdir, &db));

  ASSERT_STATUS_OK(db->Get(Slice("k1"), &val));
  ASSERT_EQ(val, "v1");

  s = db->Get(Slice("k2"), &val);
  ASSERT_TRUE(s.IsNotFound());

  ASSERT_STATUS_OK(db->Get(Slice("k6"), &val));
  ASSERT_EQ(val, "v6");

  db.reset();
  env->DeleteDir(dbdir);
}

}  // namespace purpledb

