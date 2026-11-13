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

TEST(VersionSetTest, LevelOrderingAndCompactionPicker) {
  Env* env = Env::Default();
  std::string dbdir = "test_version_levels";
  env->DeleteDir(dbdir);
  ASSERT_STATUS_OK(env->CreateDirIfMissing(dbdir));

  {
    VersionSet vset(dbdir, env);

    // 1. Add files to Level 1 in unsorted order: [i, z], [a, d], [e, h]
    VersionEdit edit;
    edit.AddFile(1, 101, 1000, Slice("i\0\0\0\0\0\0\0\1", 9), Slice("z\0\0\0\0\0\0\0\1", 9));
    edit.AddFile(1, 102, 2000, Slice("a\0\0\0\0\0\0\0\1", 9), Slice("d\0\0\0\0\0\0\0\1", 9));
    edit.AddFile(1, 103, 3000, Slice("e\0\0\0\0\0\0\0\1", 9), Slice("h\0\0\0\0\0\0\0\1", 9));
    ASSERT_STATUS_OK(vset.LogAndApply(&edit));

    // Verify L1 files are automatically sorted by smallest key: [a, d], [e, h], [i, z]
    const auto& l1 = vset.current()->files(1);
    ASSERT_EQ(l1.size(), 3u);
    ASSERT_EQ(l1[0].number, 102u);  // [a, d]
    ASSERT_EQ(l1[1].number, 103u);  // [e, h]
    ASSERT_EQ(l1[2].number, 101u);  // [i, z]

    ASSERT_EQ(vset.current()->TotalBytes(1), 6000u);

    // 2. Overlapping inputs
    std::vector<FileMetaData> overlaps;
    // [b, f] should overlap with [a, d] (file 102) and [e, h] (file 103)
    vset.current()->GetOverlappingInputs(1, Slice("b"), Slice("f"), &overlaps);
    ASSERT_EQ(overlaps.size(), 2u);
    ASSERT_EQ(overlaps[0].number, 102u);
    ASSERT_EQ(overlaps[1].number, 103u);

    // [j, k] should overlap only with [i, z] (file 101)
    vset.current()->GetOverlappingInputs(1, Slice("j"), Slice("k"), &overlaps);
    ASSERT_EQ(overlaps.size(), 1u);
    ASSERT_EQ(overlaps[0].number, 101u);

    // [0, 9] should overlap with no files
    vset.current()->GetOverlappingInputs(1, Slice("0"), Slice("9"), &overlaps);
    ASSERT_EQ(overlaps.size(), 0u);

    // 3. Compaction picker
    // Currently 0 files in L0, and 6000 bytes in L1 (< 64 MB). No compaction needed.
    ASSERT_EQ(vset.PickCompactionLevel(), -1);

    // Add 4 files to L0 -> triggers Level 0 compaction!
    VersionEdit edit_l0;
    edit_l0.AddFile(0, 201, 100, Slice("k1"), Slice("k2"));
    edit_l0.AddFile(0, 202, 100, Slice("k1"), Slice("k2"));
    edit_l0.AddFile(0, 203, 100, Slice("k1"), Slice("k2"));
    edit_l0.AddFile(0, 204, 100, Slice("k1"), Slice("k2"));
    ASSERT_STATUS_OK(vset.LogAndApply(&edit_l0));
    ASSERT_EQ(vset.PickCompactionLevel(), 0);
  }

  env->DeleteDir(dbdir);
}

}  // namespace purpledb

