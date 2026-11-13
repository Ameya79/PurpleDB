#include "core/storage/version_set.h"
#include "core/storage/env.h"
#include "core/storage/filename.h"
#include "util/test_harness.h"

#include <string>

namespace purpledb {

TEST(VersionSetTest, VersionEditEncodeDecode) {
  VersionEdit edit;
  edit.SetComparatorName(Slice("leveldb.BytewiseComparator"));
  edit.SetLogNumber(100);
  edit.SetPrevLogNumber(90);
  edit.SetNextFileNumber(105);
  edit.SetLastSequence(1000);

  edit.AddFile(0, 101, 4096, Slice("a\0\0\0\0\0\0\0\1", 9),
               Slice("z\0\0\0\0\0\0\0\1", 9));
  edit.AddFile(1, 102, 8192, Slice("m\0\0\0\0\0\0\0\1", 9),
               Slice("n\0\0\0\0\0\0\0\1", 9));
  edit.RemoveFile(0, 99);

  std::string encoded;
  edit.EncodeTo(&encoded);

  VersionEdit decoded;
  ASSERT_STATUS_OK(decoded.DecodeFrom(Slice(encoded)));

  ASSERT_TRUE(decoded.has_comparator());
  ASSERT_EQ(decoded.comparator(), "leveldb.BytewiseComparator");
  ASSERT_TRUE(decoded.has_log_number());
  ASSERT_EQ(decoded.log_number(), 100u);
  ASSERT_TRUE(decoded.has_prev_log_number());
  ASSERT_EQ(decoded.prev_log_number(), 90u);
  ASSERT_TRUE(decoded.has_next_file_number());
  ASSERT_EQ(decoded.next_file_number(), 105u);
  ASSERT_TRUE(decoded.has_last_sequence());
  ASSERT_EQ(decoded.last_sequence(), 1000u);

  ASSERT_EQ(decoded.deleted_files().size(), 1u);
  ASSERT_EQ(decoded.new_files().size(), 2u);
  ASSERT_EQ(decoded.new_files()[0].second.number, 101u);
  ASSERT_EQ(decoded.new_files()[0].second.file_size, 4096u);
  ASSERT_EQ(decoded.new_files()[1].second.number, 102u);
  ASSERT_EQ(decoded.new_files()[1].second.file_size, 8192u);
}

TEST(VersionSetTest, ManifestLogAndRecover) {
  Env* env = Env::Default();
  std::string dbdir = "test_manifest_db";
  env->DeleteDir(dbdir);
  ASSERT_STATUS_OK(env->CreateDirIfMissing(dbdir));

  {
    VersionSet vset(dbdir, env);
    vset.SetLastSequence(500);

    VersionEdit edit;
    edit.SetLogNumber(10);
    edit.SetNextFileNumber(20);
    edit.AddFile(0, 15, 2048, Slice("apple"), Slice("banana"));

    ASSERT_STATUS_OK(vset.LogAndApply(&edit));
    ASSERT_EQ(vset.current()->NumLevelFiles(0), 1u);
    ASSERT_EQ(vset.current()->files(0)[0].number, 15u);
  }

  // Verify CURRENT exists and points to MANIFEST
  ASSERT_TRUE(env->FileExists(CurrentFileName(dbdir)));

  // Reopen and recover
  {
    VersionSet vset2(dbdir, env);
    ASSERT_STATUS_OK(vset2.Recover());

    ASSERT_EQ(vset2.current()->NumLevelFiles(0), 1u);
    ASSERT_EQ(vset2.current()->files(0)[0].number, 15u);
    ASSERT_EQ(vset2.current()->files(0)[0].file_size, 2048u);
    ASSERT_EQ(vset2.NextFileNumber(), 20u);
    ASSERT_EQ(vset2.LastSequence(), 500u);
  }

  // Cleanup
  env->DeleteDir(dbdir);
}

}  // namespace purpledb
