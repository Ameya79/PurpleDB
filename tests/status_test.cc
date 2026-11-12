#include "purpledb/status.h"
#include "util/test_harness.h"

namespace purpledb {

TEST(StatusTest, OKStatus) {
  Status ok = Status::OK();
  ASSERT_TRUE(ok.ok());
  ASSERT_EQ(ok.code(), Status::kOk);
  ASSERT_EQ(ok.ToString(), "OK");
  ASSERT_STATUS_OK(ok);
}

TEST(StatusTest, ErrorCodesAndMessages) {
  Status s1 = Status::NotFound("file not found", "data.wal");
  ASSERT_FALSE(s1.ok());
  ASSERT_TRUE(s1.IsNotFound());
  ASSERT_EQ(s1.code(), Status::kNotFound);
  ASSERT_EQ(s1.ToString(), "NotFound: file not found: data.wal");

  Status s2 = Status::Corruption("checksum mismatch");
  ASSERT_TRUE(s2.IsCorruption());
  ASSERT_EQ(s2.ToString(), "Corruption: checksum mismatch");

  Status s3 = Status::NotSupported("lz4 compression");
  ASSERT_TRUE(s3.IsNotSupported());

  Status s4 = Status::InvalidArgument("negative vector dimension");
  ASSERT_TRUE(s4.IsInvalidArgument());

  Status s5 = Status::IOError("disk full", "/var/db");
  ASSERT_TRUE(s5.IsIOError());

  Status s6 = Status::Busy("compaction in progress");
  ASSERT_TRUE(s6.IsBusy());

  Status s7 = Status::TimedOut("lock acquisition timed out");
  ASSERT_TRUE(s7.IsTimedOut());
}

TEST(StatusTest, CopyAndMoveSemantics) {
  Status original = Status::NotFound("key missing");
  // Copy
  Status copy = original;
  ASSERT_TRUE(copy.IsNotFound());
  ASSERT_TRUE(original.IsNotFound());
  ASSERT_EQ(copy.ToString(), original.ToString());

  // Assignment
  Status assigned;
  assigned = original;
  ASSERT_TRUE(assigned.IsNotFound());
  ASSERT_EQ(assigned.ToString(), original.ToString());

  // Self assignment
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wself-assign-overload"
#endif
  assigned = assigned;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  ASSERT_TRUE(assigned.IsNotFound());

  // Move
  Status moved = std::move(copy);
  ASSERT_TRUE(moved.IsNotFound());
  ASSERT_TRUE(copy.ok());  // moved-from status becomes OK (null state)
}

}  // namespace purpledb
