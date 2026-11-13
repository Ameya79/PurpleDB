#include "core/storage/db_impl.h"

#include <algorithm>
#include "core/storage/wal_reader.h"
#include "core/util/logging.h"

namespace purpledb {

namespace {

class MemTableInserter : public WriteBatch::Handler {
 public:
  MemTableInserter(SequenceNumber seq, MemTable* mem)
      : seq_(seq), mem_(mem) {}

  Status Put(const Slice& key, const Slice& value) override {
    mem_->Add(seq_++, kTypeValue, key, value);
    return Status::OK();
  }

  Status Delete(const Slice& key) override {
    mem_->Add(seq_++, kTypeDeletion, key, Slice());
    return Status::OK();
  }

  SequenceNumber seq_;
  MemTable* mem_;
};

}  // namespace

Status DB::Open(const Options& options, const std::string& name,
                std::unique_ptr<DB>* dbptr) {
  dbptr->reset();
  std::unique_ptr<DBImpl> impl(new DBImpl(options, name));
  Status s = impl->Recover();
  if (s.ok()) {
    *dbptr = std::move(impl);
  }
  return s;
}

DBImpl::DBImpl(const Options& options, std::string dbname)
    : options_(options),
      dbname_(std::move(dbname)),
      env_(options.env ? options.env : Env::Default()),
      mem_(std::make_unique<MemTable>()),
      last_sequence_(0) {}

DBImpl::~DBImpl() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (log_file_) {
    log_file_->Close();
  }
}

std::string DBImpl::CurrentLogFilename() const {
  return dbname_ + "/000001.log";
}

Status DBImpl::Recover() {
  std::lock_guard<std::mutex> lock(mutex_);

  const bool exists = env_->FileExists(dbname_);
  if (!exists) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_, "database directory does not exist");
    }
    Status s = env_->CreateDirIfMissing(dbname_);
    if (!s.ok()) {
      return s;
    }
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "database directory already exists");
  }

  const std::string log_name = CurrentLogFilename();
  uint64_t log_size = 0;

  if (env_->FileExists(log_name)) {
    // Replay existing WAL to recover memtable
    std::unique_ptr<SequentialFile> sf;
    Status s = env_->NewSequentialFile(log_name, &sf);
    if (!s.ok()) {
      return s;
    }

    wal::WalReader reader(sf.get(), nullptr, options_.sync);
    Slice record;
    std::string scratch;

    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < 12) {
        continue;
      }
      WriteBatch batch;
      batch.SetData(record);
      SequenceNumber seq = batch.Sequence();
      MemTableInserter inserter(seq, mem_.get());
      batch.Iterate(&inserter);

      last_sequence_ = std::max(last_sequence_, seq + batch.Count() - 1);
    }

    sf.reset();  // Close file handle before opening appendable file

    env_->GetFileSize(log_name, &log_size);
    s = env_->NewAppendableFile(log_name, &log_file_);
    if (!s.ok()) {
      return s;
    }
    log_ = std::make_unique<wal::WalWriter>(log_file_.get(), log_size);
  } else {
    // Fresh WAL file
    Status s = env_->NewWritableFile(log_name, &log_file_);
    if (!s.ok()) {
      return s;
    }
    log_ = std::make_unique<wal::WalWriter>(log_file_.get());
  }

  return Status::OK();
}

Status DBImpl::Put(const WriteOptions& options, const Slice& key, const Slice& value) {
  WriteBatch batch;
  batch.Put(key, value);
  return Write(options, &batch);
}

Status DBImpl::Delete(const WriteOptions& options, const Slice& key) {
  WriteBatch batch;
  batch.Delete(key);
  return Write(options, &batch);
}

Status DBImpl::Write(const WriteOptions& options, WriteBatch* updates) {
  if (updates == nullptr || updates->Count() == 0) {
    return Status::OK();
  }

  std::lock_guard<std::mutex> lock(mutex_);

  const SequenceNumber seq = last_sequence_ + 1;
  updates->SetSequence(seq);
  last_sequence_ += updates->Count();

  // 1. Write mutations to the Write-Ahead Log
  Status s = log_->AddRecord(Slice(updates->Data()));
  if (!s.ok()) {
    return s;
  }

  if (options.sync || options_.sync) {
    s = log_file_->Sync();
    if (!s.ok()) {
      return s;
    }
  }

  // 2. Apply mutations to the MemTable
  MemTableInserter inserter(seq, mem_.get());
  s = updates->Iterate(&inserter);
  return s;
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  SequenceNumber snapshot_seq;
  MemTable* mem;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_seq = (options.snapshot == 0) ? last_sequence_ : options.snapshot;
    mem = mem_.get();
  }

  Status s;
  if (mem->Get(key, snapshot_seq, value, &s)) {
    return s;
  }

  return Status::NotFound(key, "key not found");
}

}  // namespace purpledb
