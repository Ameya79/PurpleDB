#include "core/util/sha256.h"
#include "core/storage/env.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

TEST(Sha256Test, StandardNistVectors) {
  // Empty string
  ASSERT_EQ(SHA256::HashString(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  // "abc"
  ASSERT_EQ(SHA256::HashString("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // Standard multi-block message
  std::string msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  ASSERT_EQ(SHA256::HashString(msg),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256Test, FileHashingMatchesString) {
  Env* env = Env::Default();
  std::string test_file = "test_sha256_file.tmp";
  std::string content = "PurpleDB: Embedded multimodal vector database for video meaning.";

  std::unique_ptr<WritableFile> wf;
  ASSERT_STATUS_OK(env->NewWritableFile(test_file, &wf));
  ASSERT_STATUS_OK(wf->Append(content));
  ASSERT_STATUS_OK(wf->Close());

  std::string file_hex;
  ASSERT_STATUS_OK(SHA256::HashFile(env, test_file, &file_hex));
  ASSERT_EQ(file_hex, SHA256::HashString(content));

  env->DeleteFile(test_file);
}

}  // namespace test
}  // namespace purpledb
