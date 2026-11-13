#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include "core/storage/cache.h"
#include "core/storage/env.h"
#include "core/storage/memtable.h"
#include "core/storage/snapshot.h"
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

  const Snapshot* GetSnapshot() override;
  void ReleaseSnapshot(const Snapshot* snapshot) override;
  std::unique_ptr<Iterator> NewIterator(const ReadOptions& options) override;

  // Recover existing database files or initialize a fresh database
  Status Recover();

  // Explicitly trigger a MemTable flush to Level 0 SSTable (useful for testing and syncs)
  Status FlushMemTable();

  // Run compaction on whatever level needs it
  Status CompactOnce();

 private:
  Status MaybeFlushMemTable();
  Status FlushMemTableToOutputFile(MemTable* mem, uint64_t new_log_number);
  Status DoCompactionWork(Compaction* c);
  void MaybeScheduleCompaction();
  void BackgroundThreadEntry();
  void RemoveOrphanFiles();

  Options options_;
  std::string dbname_;
  Env* env_;

  std::mutex mutex_;
  std::condition_variable bg_cv_;
  std::thread bg_thread_;
  bool shutting_down_{false};
  bool bg_compaction_scheduled_{false};
  bool recovered_{false};
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<MemTable> mem_;
  std::unique_ptr<MemTable> imm_;
  std::unique_ptr<WritableFile> log_file_;
  std::unique_ptr<wal::WalWriter> log_;
  uint64_t log_number_{0};
  SequenceNumber last_sequence_{0};
  std::shared_ptr<Cache> block_cache_;
  SnapshotList snapshots_;
};

}  // namespace purpledb
