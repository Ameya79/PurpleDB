#include "core/storage/wal_format.h"
#include "core/storage/wal_reader.h"
#include "core/storage/wal_writer.h"
#include "core/storage/env.h"
#include "util/test_harness.h"

#include <string>
#include <vector>

namespace purpledb {
namespace wal {

class TestReporter : public WalReader::Reporter {
 public:
  void Corruption(size_t bytes, const Status& status) override {
    dropped_bytes_ += bytes;
    corruption_status_ = status;
  }
  size_t dropped_bytes_{0};
  Status corruption_status_;
};

TEST(WalTest, EmptyLog) {
  Env* env = Env::Default();
  std::string fname = "test_wal_empty.log";
  env->DeleteFile(fname);

  std::unique_ptr<WritableFile> wf;
  ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
  ASSERT_STATUS_OK(wf->Close());

  std::unique_ptr<SequentialFile> sf;
  ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
  TestReporter reporter;
  WalReader reader(sf.get(), &reporter, true);

  Slice record;
  std::string scratch;
  ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
  ASSERT_EQ(reporter.dropped_bytes_, 0u);

  env->DeleteFile(fname);
}

TEST(WalTest, ShortRecordsRoundTrip) {
  Env* env = Env::Default();
  std::string fname = "test_wal_short.log";
  env->DeleteFile(fname);

  std::vector<std::string> expected;
  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
    WalWriter writer(wf.get());

    for (int i = 0; i < 200; ++i) {
      std::string rec = "record_" + std::to_string(i) + "_data_payload";
      expected.push_back(rec);
      ASSERT_STATUS_OK(writer.AddRecord(Slice(rec)));
    }
    ASSERT_STATUS_OK(wf->Close());
  }

  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
    TestReporter reporter;
    WalReader reader(sf.get(), &reporter, true);

    Slice record;
    std::string scratch;
    for (size_t i = 0; i < expected.size(); ++i) {
      ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
      ASSERT_EQ(record.ToString(), expected[i]);
    }
    ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
    ASSERT_EQ(reporter.dropped_bytes_, 0u);
  }

  env->DeleteFile(fname);
}

TEST(WalTest, LargeFragmentedRecords) {
  Env* env = Env::Default();
  std::string fname = "test_wal_large.log";
  env->DeleteFile(fname);

  // Large records spanning multiple 32 KiB blocks
  std::vector<std::string> expected;
  expected.push_back(std::string(10000, 'A'));   // Fits in block
  expected.push_back(std::string(40000, 'B'));   // Spans 2 blocks
  expected.push_back(std::string(100000, 'C'));  // Spans 4 blocks
  expected.push_back(std::string(500, 'D'));

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
    WalWriter writer(wf.get());

    for (const auto& rec : expected) {
      ASSERT_STATUS_OK(writer.AddRecord(Slice(rec)));
    }
    ASSERT_STATUS_OK(wf->Close());
  }

  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
    TestReporter reporter;
    WalReader reader(sf.get(), &reporter, true);

    Slice record;
    std::string scratch;
    for (size_t i = 0; i < expected.size(); ++i) {
      ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
      ASSERT_EQ(record.size(), expected[i].size());
      ASSERT_EQ(record.ToString(), expected[i]);
    }
    ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
  }

  env->DeleteFile(fname);
}

TEST(WalTest, ChecksumCorruptionDetection) {
  Env* env = Env::Default();
  std::string fname = "test_wal_corrupt.log";
  env->DeleteFile(fname);

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
    WalWriter writer(wf.get());
    ASSERT_STATUS_OK(writer.AddRecord(Slice("valid_record_1")));
    ASSERT_STATUS_OK(writer.AddRecord(Slice("valid_record_2")));
    ASSERT_STATUS_OK(wf->Close());
  }

  // Corrupt record 2 payload by reading raw bytes, modifying one, writing back
  std::string content;
  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
    char scratch[4096];
    Slice res;
    ASSERT_STATUS_OK(sf->Read(sizeof(scratch), &res, scratch));
    content = res.ToString();
  }

  // Flip a byte in the second record
  ASSERT_GT(content.size(), 30u);
  content[content.size() - 2] ^= 0x55;

  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
    ASSERT_STATUS_OK(wf->Append(Slice(content)));
    ASSERT_STATUS_OK(wf->Close());
  }

  {
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
    TestReporter reporter;
    WalReader reader(sf.get(), &reporter, true);

    Slice record;
    std::string scratch;
    // First record should still succeed
    ASSERT_TRUE(reader.ReadRecord(&record, &scratch));
    ASSERT_EQ(record.ToString(), "valid_record_1");

    // Second record should fail due to checksum corruption
    ASSERT_FALSE(reader.ReadRecord(&record, &scratch));
    ASSERT_GT(reporter.dropped_bytes_, 0u);
  }

  env->DeleteFile(fname);
}

TEST(WalTest, TailTruncationRecoversConsistentPrefix) {
  // Acceptance criterion: truncating the WAL tail at any byte recovers a consistent prefix.
  Env* env = Env::Default();
  std::string fname = "test_wal_trunc.log";
  env->DeleteFile(fname);

  std::vector<std::string> records;
  for (int i = 0; i < 50; ++i) {
    records.push_back("record_prefix_test_" + std::to_string(i) + "_" + std::string(i * 30, 'z'));
  }

  std::string full_wal;
  {
    std::unique_ptr<WritableFile> wf;
    ASSERT_STATUS_OK(env->NewWritableFile(fname, &wf));
    WalWriter writer(wf.get());
    for (const auto& r : records) {
      ASSERT_STATUS_OK(writer.AddRecord(Slice(r)));
    }
    ASSERT_STATUS_OK(wf->Close());

    uint64_t sz = 0;
    ASSERT_STATUS_OK(env->GetFileSize(fname, &sz));
    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sf));
    std::vector<char> buf(sz);
    Slice res;
    ASSERT_STATUS_OK(sf->Read(sz, &res, buf.data()));
    full_wal = res.ToString();
  }

  const size_t total_size = full_wal.size();
  ASSERT_GT(total_size, 1000u);

  // Truncate at step increments across the entire file
  const size_t step = std::max<size_t>(1, total_size / 200);
  for (size_t trunc_len = 0; trunc_len <= total_size; trunc_len += step) {
    std::string trunc_fname = "test_wal_trunc_sub.log";
    {
      std::unique_ptr<WritableFile> wf;
      ASSERT_STATUS_OK(env->NewWritableFile(trunc_fname, &wf));
      if (trunc_len > 0) {
        ASSERT_STATUS_OK(wf->Append(Slice(full_wal.data(), trunc_len)));
      }
      ASSERT_STATUS_OK(wf->Close());
    }

    std::unique_ptr<SequentialFile> sf;
    ASSERT_STATUS_OK(env->NewSequentialFile(trunc_fname, &sf));
    TestReporter reporter;
    WalReader reader(sf.get(), &reporter, true);

    Slice record;
    std::string scratch;
    size_t count = 0;
    while (reader.ReadRecord(&record, &scratch)) {
      ASSERT_LT(count, records.size());
      ASSERT_EQ(record.ToString(), records[count]);
      count++;
    }

    env->DeleteFile(trunc_fname);
  }

  env->DeleteFile(fname);
}

}  // namespace wal
}  // namespace purpledb
