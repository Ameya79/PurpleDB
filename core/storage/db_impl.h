#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "core/storage/cache.h"
#include "core/storage/env.h"
#include "core/storage/memtable.h"
#include "core/storage/version_set.h"
#include "core/storage/wal_writer.h"
#include "purpledb/db.h"

namespace purpledb {

class DBImpl : public DB {
 public:
  DBImpl(const Options& options, std::string dbname);
  ~DBImpl() override;

  Status Put(const WriteOptions& options, const Slice& key, const Slice& value) override;
  Status Delete(const WriteOptions& options, const Slice& key) override;
  Status Write(const WriteOptions& options, WriteBatch* updates) override;
  Status Get(const ReadOptions& options, const Slice& key, std::string* value) override;

  // Recover existing database files or initialize a fresh database
  Status Recover();

  // Explicitly trigger a MemTable flush to Level 0 SSTable (useful for testing and syncs)
  Status FlushMemTable();

 private:
  Status MaybeFlushMemTable();
  Status FlushMemTableToOutputFile(MemTable* mem, uint64_t new_log_number);

  Options options_;
  std::string dbname_;
  Env* env_;

  std::mutex mutex_;
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<MemTable> imm_;
  std::unique_ptr<WritableFile> log_file_;
  std::unique_ptr<wal::WalWriter> log_;
  uint64_t log_number_{0};
  SequenceNumber last_sequence_{0};
  std::shared_ptr<Cache> block_cache_;
};

}  // namespace purpledb
