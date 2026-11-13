#pragma once

#include <memory>
#include <mutex>
#include <string>
#include "core/storage/env.h"
#include "core/storage/memtable.h"
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

 private:
  std::string CurrentLogFilename() const;

  Options options_;
  std::string dbname_;
  Env* env_;

  std::mutex mutex_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<WritableFile> log_file_;
  std::unique_ptr<wal::WalWriter> log_;
  SequenceNumber last_sequence_{0};
};

}  // namespace purpledb
