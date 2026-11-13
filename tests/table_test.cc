#include "core/storage/table.h"
#include "core/storage/table_builder.h"
#include "core/storage/env.h"
#include "util/test_harness.h"

#include <string>
#include <vector>

namespace purpledb {

TEST(TableTest, EmptyTable) {
  Env* env = Env::Default();
  std::string fname = "test_empty.sst";
  std::unique_ptr<WritableFile> wfile;
  ASSERT_STATUS_OK(env->NewWritableFile(fname, &wfile));

  TableBuilderOptions opts;
  TableBuilder builder(opts, wfile.get());
  ASSERT_STATUS_OK(builder.Finish());
  ASSERT_STATUS_OK(wfile->Close());

  uint64_t fsize = 0;
  ASSERT_STATUS_OK(env->GetFileSize(fname, &fsize));

  std::unique_ptr<RandomAccessFile> rfile;
  ASSERT_STATUS_OK(env->NewRandomAccessFile(fname, &rfile));

  std::unique_ptr<Table> table;
  ASSERT_STATUS_OK(Table::Open(std::move(rfile), fsize, &table));

  auto iter = table->NewIterator();
  ASSERT_FALSE(iter->Valid());

  iter->SeekToFirst();
  ASSERT_FALSE(iter->Valid());

  iter->Seek(Slice("any"));
  ASSERT_FALSE(iter->Valid());

  // Clean up
  table.reset();
  env->DeleteFile(fname);
}

TEST(TableTest, MultiBlockReadWriteAndIterate) {
  Env* env = Env::Default();
  std::string fname = "test_multiblock.sst";
  std::unique_ptr<WritableFile> wfile;
  ASSERT_STATUS_OK(env->NewWritableFile(fname, &wfile));

  TableBuilderOptions opts;
  opts.block_size = 512;  // Small block size to force multiple blocks
  opts.block_restart_interval = 4;
  TableBuilder builder(opts, wfile.get());

  const int N = 300;
  std::vector<std::pair<std::string, std::string>> kv;
  for (int i = 0; i < N; ++i) {
    char kbuf[32];
    std::snprintf(kbuf, sizeof(kbuf), "key/%06d", i);
    char vbuf[64];
    std::snprintf(vbuf, sizeof(vbuf), "value_payload_for_%06d", i);
    kv.push_back({kbuf, vbuf});
    builder.Add(Slice(kbuf), Slice(vbuf));
  }

  ASSERT_EQ(builder.NumEntries(), static_cast<uint64_t>(N));
  ASSERT_STATUS_OK(builder.Finish());
  ASSERT_STATUS_OK(wfile->Close());

  uint64_t fsize = 0;
  ASSERT_STATUS_OK(env->GetFileSize(fname, &fsize));
  ASSERT_GT(fsize, 0u);

  std::unique_ptr<RandomAccessFile> rfile;
  ASSERT_STATUS_OK(env->NewRandomAccessFile(fname, &rfile));

  std::unique_ptr<Table> table;
  ASSERT_STATUS_OK(Table::Open(std::move(rfile), fsize, &table));

  auto iter = table->NewIterator();

  // Forward traversal
  iter->SeekToFirst();
  for (int i = 0; i < N; ++i) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().ToString(), kv[i].first);
    ASSERT_EQ(iter->value().ToString(), kv[i].second);
    iter->Next();
  }
  ASSERT_FALSE(iter->Valid());

  // Backward traversal
  iter->SeekToLast();
  for (int i = N - 1; i >= 0; --i) {
    ASSERT_TRUE(iter->Valid());
    ASSERT_EQ(iter->key().ToString(), kv[i].first);
    ASSERT_EQ(iter->value().ToString(), kv[i].second);
    iter->Prev();
  }
  ASSERT_FALSE(iter->Valid());

  // Seek exact
  iter->Seek(Slice("key/000150"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "key/000150");

  // Seek in-between
  iter->Seek(Slice("key/000150.5"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "key/000151");

  // Seek before first
  iter->Seek(Slice("aaa"));
  ASSERT_TRUE(iter->Valid());
  ASSERT_EQ(iter->key().ToString(), "key/000000");

  // Seek after last
  iter->Seek(Slice("zzz"));
  ASSERT_FALSE(iter->Valid());

  // Clean up
  table.reset();
  env->DeleteFile(fname);
}

TEST(TableTest, CorruptionDetection) {
  Env* env = Env::Default();
  std::string fname = "test_corrupt.sst";
  std::unique_ptr<WritableFile> wfile;
  ASSERT_STATUS_OK(env->NewWritableFile(fname, &wfile));

  TableBuilderOptions opts;
  opts.block_size = 256;
  TableBuilder builder(opts, wfile.get());
  builder.Add(Slice("k1"), Slice("v1"));
  builder.Add(Slice("k2"), Slice("v2"));
  ASSERT_STATUS_OK(builder.Finish());
  ASSERT_STATUS_OK(wfile->Close());

  uint64_t fsize = 0;
  ASSERT_STATUS_OK(env->GetFileSize(fname, &fsize));

  // Read whole file into buffer, corrupt a byte in data block, write back
  std::unique_ptr<SequentialFile> sfile;
  ASSERT_STATUS_OK(env->NewSequentialFile(fname, &sfile));
  std::vector<char> file_data(fsize);
  Slice read_result;
  ASSERT_STATUS_OK(sfile->Read(fsize, &read_result, file_data.data()));
  sfile.reset();

  // Flip bits in the data block (offset 0)
  file_data[0] ^= 0x55;

  // Write corrupted data back
  std::unique_ptr<WritableFile> corrupt_wfile;
  ASSERT_STATUS_OK(env->NewWritableFile(fname, &corrupt_wfile));
  ASSERT_STATUS_OK(corrupt_wfile->Append(Slice(file_data.data(), file_data.size())));
  ASSERT_STATUS_OK(corrupt_wfile->Close());

  // Try to open and read
  std::unique_ptr<RandomAccessFile> rfile;
  ASSERT_STATUS_OK(env->NewRandomAccessFile(fname, &rfile));
  std::unique_ptr<Table> table;
  ASSERT_STATUS_OK(Table::Open(std::move(rfile), fsize, &table));

  auto iter = table->NewIterator();
  iter->SeekToFirst();
  // Corrupted data block causes checksum mismatch
  ASSERT_FALSE(iter->Valid());
  ASSERT_TRUE(iter->status().IsCorruption());

  // Clean up
  table.reset();
  env->DeleteFile(fname);
}

}  // namespace purpledb
