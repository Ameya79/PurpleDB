#include "purpledb/purpledb_c.h"
#include "util/test_harness.h"
#include "core/storage/env.h"

#include <cstring>
#include <vector>

namespace purpledb {
namespace test {

class CAbiTest {
 public:
  CAbiTest() : env_(Env::Default()) {
    test_dir_ = "./c_abi_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~CAbiTest() {
    std::vector<std::string> children;
    if (env_->GetChildren(test_dir_, &children).ok()) {
      for (const auto& f : children) {
        if (f != "." && f != "..") {
          env_->DeleteFile(test_dir_ + "/" + f);
        }
      }
      env_->DeleteDir(test_dir_);
    }
  }

  Env* env_;
  std::string test_dir_;
};

TEST(CAbiTest, AbiVersion) {
  ASSERT_EQ(pdb_abi_version(), 1);
}

TEST(CAbiTest, OptionsInitialization) {
  pdb_options_t opts;
  pdb_options_init(&opts);
  ASSERT_EQ(opts.create_if_missing, 1);
  ASSERT_EQ(opts.enable_hnsw, 1);
  ASSERT_EQ(opts.hnsw_M, 16u);
  ASSERT_EQ(opts.hnsw_efSearch, 64u);
}

TEST(CAbiTest, InvalidArgumentsAndLastError) {
  pdb_t* db = nullptr;
  int rc = pdb_open(nullptr, "local", &db);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);
  ASSERT_NE(std::strlen(pdb_last_error()), 0u);

  rc = pdb_search_text(nullptr, "query", 5, nullptr, nullptr);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);
  ASSERT_NE(std::strlen(pdb_last_error()), 0u);

  rc = pdb_search_image(nullptr, nullptr, 0, 0, 0, 5, nullptr, nullptr);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);

  rc = pdb_add_video(nullptr, nullptr, nullptr, 0);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);

  rc = pdb_delete_video(nullptr, nullptr);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);

  rc = pdb_get_stats(nullptr, nullptr);
  ASSERT_EQ(rc, PDB_INVALID_ARGUMENT);
}

TEST(CAbiTest, OpenEmptyDbAndSearch) {
  CAbiTest fixture;

  pdb_t* db = nullptr;
  int rc = pdb_open(fixture.test_dir_.c_str(), "local", &db);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(db != nullptr);

  // Search on empty database
  pdb_results_t* res = nullptr;
  rc = pdb_search_text(db, "ocean waves", 10, nullptr, &res);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_TRUE(res != nullptr);
  ASSERT_EQ(pdb_results_count(res), 0);

  pdb_results_destroy(res);

  // Check stats
  pdb_stats_t stats;
  rc = pdb_get_stats(db, &stats);
  ASSERT_EQ(rc, PDB_OK);
  ASSERT_EQ(stats.num_moments, 0u);
  ASSERT_EQ(stats.num_videos, 0u);

  pdb_close(db);
}

}  // namespace test
}  // namespace purpledb
