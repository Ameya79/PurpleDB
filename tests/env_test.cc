#include "core/storage/env.h"
#include "util/test_harness.h"

#include <string>
#include <vector>

namespace purpledb {

TEST(EnvTest, DirectoryAndFileOperations) {
  Env* env = Env::Default();
  std::string test_dir = "test_env_dir";
  env->DeleteDir(test_dir);

  ASSERT_FALSE(env->FileExists(test_dir));
  ASSERT_STATUS_OK(env->CreateDirIfMissing(test_dir));
  ASSERT_TRUE(env->FileExists(test_dir));

  std::string test_file = test_dir + "/sample.txt";
  ASSERT_FALSE(env->FileExists(test_file));

  // Write file
  std::unique_ptr<WritableFile> wf;
  ASSERT_STATUS_OK(env->NewWritableFile(test_file, &wf));
  ASSERT_STATUS_OK(wf->Append(Slice("Hello, ")));
  ASSERT_STATUS_OK(wf->Append(Slice("PurpleDB!")));
  ASSERT_STATUS_OK(wf->Sync());
  ASSERT_STATUS_OK(wf->Close());

  ASSERT_TRUE(env->FileExists(test_file));

  uint64_t file_size = 0;
  ASSERT_STATUS_OK(env->GetFileSize(test_file, &file_size));
  ASSERT_EQ(file_size, 16u);

  // Read file sequentially
  std::unique_ptr<SequentialFile> sf;
  ASSERT_STATUS_OK(env->NewSequentialFile(test_file, &sf));
  char scratch[64];
  Slice result;
  ASSERT_STATUS_OK(sf->Read(64, &result, scratch));
  ASSERT_EQ(result.ToString(), "Hello, PurpleDB!");

  // Appendable file
  std::unique_ptr<WritableFile> af;
  ASSERT_STATUS_OK(env->NewAppendableFile(test_file, &af));
  ASSERT_STATUS_OK(af->Append(Slice(" More data.")));
  ASSERT_STATUS_OK(af->Close());

  ASSERT_STATUS_OK(env->GetFileSize(test_file, &file_size));
  ASSERT_EQ(file_size, 27u);

  // Read again
  ASSERT_STATUS_OK(env->NewSequentialFile(test_file, &sf));
  ASSERT_STATUS_OK(sf->Read(64, &result, scratch));
  ASSERT_EQ(result.ToString(), "Hello, PurpleDB! More data.");
  sf.reset();  // Release file handle before renaming on Windows

  // Rename
  std::string renamed = test_dir + "/renamed.txt";
  ASSERT_STATUS_OK(env->RenameFile(test_file, renamed));
  ASSERT_FALSE(env->FileExists(test_file));
  ASSERT_TRUE(env->FileExists(renamed));

  // Cleanup
  ASSERT_STATUS_OK(env->DeleteFile(renamed));
  ASSERT_FALSE(env->FileExists(renamed));
  ASSERT_STATUS_OK(env->DeleteDir(test_dir));
  ASSERT_FALSE(env->FileExists(test_dir));
}

}  // namespace purpledb
