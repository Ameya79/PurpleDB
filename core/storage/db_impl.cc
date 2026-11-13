#include "core/storage/db_impl.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <unordered_set>
#include "core/storage/compaction.h"
#include "core/storage/db_iter.h"
#include "core/storage/filename.h"
#include "core/storage/merger.h"
#include "core/storage/table_builder.h"
#include "core/storage/wal_reader.h"
#include "core/util/coding.h"
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
      versions_(std::make_unique<VersionSet>(dbname_, env_, &options_)),
      mem_(std::make_unique<MemTable>()),
      last_sequence_(0),
      block_cache_(options.block_cache ? options.block_cache : NewLRUCache(8 * 1024 * 1024)) {
  bg_thread_ = std::thread(&DBImpl::BackgroundThreadEntry, this);
}

DBImpl::~DBImpl() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutting_down_ = true;
    bg_cv_.notify_all();
    if (log_file_) {
      log_file_->Close();
    }
  }
  if (bg_thread_.joinable()) {
    bg_thread_.join();
  }
}

Status DBImpl::Recover() {
  std::lock_guard<std::mutex> lock(mutex_);

  const bool exists = env_->FileExists(dbname_);
  if (!exists) {
    if (!options_.create_if_missing) {
      return Status::InvalidArgument(dbname_, "database directory does not exist");
    }
    Status s = env_->CreateDirIfMissing(dbname_);
    if (!s.ok()) return s;
  } else if (options_.error_if_exists) {
    return Status::InvalidArgument(dbname_, "database directory already exists");
  }

  const bool has_current = env_->FileExists(CurrentFileName(dbname_));
  if (!has_current) {
    // Fresh database initialization
    log_number_ = versions_->NewFileNumber();
    VersionEdit new_db;
    new_db.SetComparatorName(Slice("leveldb.BytewiseComparator"));
    new_db.SetLogNumber(log_number_);
    new_db.SetNextFileNumber(log_number_ + 1);
    new_db.SetLastSequence(0);

    Status s = versions_->LogAndApply(&new_db);
    if (!s.ok()) return s;

    s = env_->NewWritableFile(LogFileName(dbname_, log_number_), &log_file_);
    if (!s.ok()) return s;

    log_ = std::make_unique<wal::WalWriter>(log_file_.get());
    recovered_ = true;
    return Status::OK();
  }

  // Recover existing VersionSet from MANIFEST
  Status s = versions_->Recover();
  if (!s.ok()) return s;

  last_sequence_ = versions_->LastSequence();
  log_number_ = versions_->LogNumber();

  // Replay existing WAL log
  const std::string log_name = LogFileName(dbname_, log_number_);
  if (env_->FileExists(log_name)) {
    std::unique_ptr<SequentialFile> sf;
    s = env_->NewSequentialFile(log_name, &sf);
    if (!s.ok()) return s;

    wal::WalReader reader(sf.get(), nullptr, options_.sync, 0);
    Slice record;
    std::string scratch;

    while (reader.ReadRecord(&record, &scratch)) {
      if (record.size() < 12) continue;

      WriteBatch batch;
      batch.SetData(record);
      SequenceNumber seq = batch.Sequence();
      MemTableInserter inserter(seq, mem_.get());
      batch.Iterate(&inserter);

      last_sequence_ = std::max(last_sequence_, seq + batch.Count() - 1);
    }
    sf.reset();
  }

  versions_->SetLastSequence(last_sequence_);

  // If memtable has recovered entries from WAL, flush them to L0 SSTable
  if (mem_->Count() > 0) {
    uint64_t new_log = versions_->NewFileNumber();
    s = FlushMemTableToOutputFile(mem_.get(), new_log);
    if (!s.ok()) return s;
    mem_ = std::make_unique<MemTable>();
    env_->DeleteFile(log_name);
    log_number_ = new_log;
  } else {
    // Roll to a fresh WAL log for subsequent mutations
    uint64_t new_log = versions_->NewFileNumber();
    VersionEdit edit;
    edit.SetLogNumber(new_log);
    s = versions_->LogAndApply(&edit);
    if (!s.ok()) return s;
    log_number_ = new_log;
  }

  s = env_->NewWritableFile(LogFileName(dbname_, log_number_), &log_file_);
  if (!s.ok()) return s;

  log_ = std::make_unique<wal::WalWriter>(log_file_.get());
  versions_->SetLastSequence(last_sequence_);

  RemoveOrphanFiles();
  recovered_ = true;
  MaybeScheduleCompaction();

  return Status::OK();
}

Status DBImpl::FlushMemTableToOutputFile(MemTable* mem, uint64_t new_log_number) {
  if (mem == nullptr || mem->Count() == 0) {
    return Status::OK();
  }

  uint64_t file_number = versions_->NewFileNumber();
  std::string sst_name = TableFileName(dbname_, file_number);
  std::unique_ptr<WritableFile> file;
  Status s = env_->NewWritableFile(sst_name, &file);
  if (!s.ok()) return s;

  static const InternalKeyBlockComparator kInternalKeyBlockComparator;
  TableBuilderOptions tb_opts;
  tb_opts.block_size = options_.block_size;
  tb_opts.comparator = &kInternalKeyBlockComparator;
  tb_opts.filter_policy = options_.filter_policy;
  TableBuilder builder(tb_opts, file.get());

  auto iter = mem->NewIterator();
  std::string smallest_key;
  std::string largest_key;

  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    Slice k = iter->key();
    Slice v = iter->value();
    if (smallest_key.empty()) {
      smallest_key = k.ToString();
    }
    largest_key = k.ToString();
    builder.Add(k, v);
  }

  s = builder.Finish();
  if (s.ok()) {
    s = file->Close();
  }
  file.reset();

  if (s.ok()) {
    VersionEdit edit;
    edit.SetLogNumber(new_log_number);
    if (builder.NumEntries() > 0) {
      edit.AddFile(0, file_number, builder.FileSize(), Slice(smallest_key),
                   Slice(largest_key));
    }
    s = versions_->LogAndApply(&edit);
  }

  return s;
}

Status DBImpl::MaybeFlushMemTable() {
  if (mem_->Count() == 0 || mem_->ApproximateMemoryUsage() < options_.write_buffer_size) {
    return Status::OK();
  }

  // Switch active memtable to immutable
  imm_ = std::move(mem_);
  mem_ = std::make_unique<MemTable>();

  // Roll WAL
  uint64_t new_log_number = versions_->NewFileNumber();
  if (log_file_) {
    log_file_->Close();
  }
  log_.reset();
  log_file_.reset();

  Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &log_file_);
  if (!s.ok()) return s;
  log_ = std::make_unique<wal::WalWriter>(log_file_.get());

  // Dump imm_ to SSTable
  s = FlushMemTableToOutputFile(imm_.get(), new_log_number);
  if (s.ok()) {
    env_->DeleteFile(LogFileName(dbname_, log_number_));
    log_number_ = new_log_number;
    imm_.reset();
  }
  MaybeScheduleCompaction();

  return s;
}

Status DBImpl::FlushMemTable() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mem_->Count() == 0) {
    return Status::OK();
  }

  imm_ = std::move(mem_);
  mem_ = std::make_unique<MemTable>();

  uint64_t new_log_number = versions_->NewFileNumber();
  if (log_file_) {
    log_file_->Close();
  }
  log_.reset();
  log_file_.reset();

  Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &log_file_);
  if (!s.ok()) return s;
  log_ = std::make_unique<wal::WalWriter>(log_file_.get());

  s = FlushMemTableToOutputFile(imm_.get(), new_log_number);
  if (s.ok()) {
    env_->DeleteFile(LogFileName(dbname_, log_number_));
    log_number_ = new_log_number;
    imm_.reset();
  }
  MaybeScheduleCompaction();
  return s;
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

  std::unique_lock<std::mutex> lock(mutex_);

  // Backpressure & write throttling:
  // If Level 0 has too many files, compaction is falling behind.
  // Slow down writes at >= 8 files, hard stop/stall at >= 12 files.
  while (!shutting_down_ && versions_->NumLevelFiles(0) >= 12) {
    MaybeScheduleCompaction();
    bg_cv_.wait(lock);
  }
  if (shutting_down_) {
    return Status::Corruption("database is shutting down");
  }
  if (versions_->NumLevelFiles(0) >= 8) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Check if MemTable needs flush
  Status s = MaybeFlushMemTable();
  if (!s.ok()) return s;

  const SequenceNumber seq = last_sequence_ + 1;
  updates->SetSequence(seq);
  last_sequence_ += updates->Count();

  // 1. Write mutations to WAL
  s = log_->AddRecord(Slice(updates->Data()));
  if (!s.ok()) return s;

  if (options.sync || options_.sync) {
    s = log_file_->Sync();
    if (!s.ok()) return s;
  }

  // 2. Apply mutations to active MemTable
  MemTableInserter inserter(seq, mem_.get());
  s = updates->Iterate(&inserter);
  if (s.ok()) {
    versions_->SetLastSequence(last_sequence_);
  }
  MaybeScheduleCompaction();
  return s;
}

Status DBImpl::Get(const ReadOptions& options, const Slice& key, std::string* value) {
  std::lock_guard<std::mutex> lock(mutex_);
  const SequenceNumber snapshot_seq = (options.snapshot != nullptr)
      ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
      : last_sequence_;

  Status s;
  // 1. Check active MemTable
  if (mem_->Get(key, snapshot_seq, value, &s)) {
    return s;
  }

  // 2. Check immutable MemTable (if pending flush)
  if (imm_ && imm_->Get(key, snapshot_seq, value, &s)) {
    return s;
  }

  // 3. Check Level 0 SSTables
  if (versions_->current()->Get(key, snapshot_seq, value, &s)) {
    return s;
  }

  return Status::NotFound(key, "key not found");
}

const Snapshot* DBImpl::GetSnapshot() {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshots_.New(last_sequence_);
}

void DBImpl::ReleaseSnapshot(const Snapshot* snapshot) {
  if (snapshot == nullptr) return;
  std::lock_guard<std::mutex> lock(mutex_);
  snapshots_.Delete(static_cast<const SnapshotImpl*>(snapshot));
}

std::unique_ptr<Iterator> DBImpl::NewIterator(const ReadOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  const SequenceNumber snapshot_seq = (options.snapshot != nullptr)
      ? static_cast<const SnapshotImpl*>(options.snapshot)->sequence_number()
      : last_sequence_;

  std::vector<std::unique_ptr<Iterator>> children;
  children.push_back(mem_->NewIterator());
  if (imm_ != nullptr) {
    children.push_back(imm_->NewIterator());
  }

  // Add SSTables across all levels (L0 to L6)
  static const InternalKeyBlockComparator kInternalKeyBlockComparator;
  versions_->current()->AddIterators(&children);

  auto internal_iter = NewMergingIterator(&kInternalKeyBlockComparator, std::move(children));
  return NewDBIterator(std::move(internal_iter), snapshot_seq);
}

Status DBImpl::CompactOnce() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto c = versions_->PickCompaction();
  if (c == nullptr) {
    return Status::OK();
  }
  Status s = DoCompactionWork(c.get());
  bg_cv_.notify_all();
  return s;
}

Status DBImpl::DoCompactionWork(Compaction* c) {
  if (c == nullptr || (c->inputs(0).empty() && c->inputs(1).empty())) {
    return Status::OK();
  }

  const SequenceNumber smallest_snapshot = snapshots_.empty()
      ? last_sequence_
      : snapshots_.oldest()->sequence_number();

  std::vector<std::unique_ptr<Iterator>> input_iters;
  static const InternalKeyBlockComparator kInternalKeyBlockComparator;
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs(which)) {
      std::unique_ptr<Table> table;
      Status s = versions_->GetTable(f.number, f.file_size, &table);
      if (!s.ok()) return s;
      std::shared_ptr<Table> shared_table = std::move(table);
      input_iters.push_back(Table::NewIterator(shared_table, &kInternalKeyBlockComparator));
    }
  }

  auto input = NewMergingIterator(&kInternalKeyBlockComparator, std::move(input_iters));

  const int out_level = c->level() + 1;
  std::unique_ptr<WritableFile> outfile;
  std::unique_ptr<TableBuilder> builder;
  uint64_t current_file_number = 0;
  std::string current_smallest;
  std::string current_largest;
  Status s;

  auto open_compaction_output = [&]() -> Status {
    current_file_number = versions_->NewFileNumber();
    std::string fname = TableFileName(dbname_, current_file_number);
    Status st = env_->NewWritableFile(fname, &outfile);
    if (!st.ok()) return st;

    TableBuilderOptions tb_opts;
    tb_opts.block_size = options_.block_size;
    tb_opts.comparator = &kInternalKeyBlockComparator;
    tb_opts.filter_policy = options_.filter_policy;
    builder = std::make_unique<TableBuilder>(tb_opts, outfile.get());
    current_smallest.clear();
    current_largest.clear();
    return Status::OK();
  };

  auto finish_compaction_output = [&]() -> Status {
    if (builder != nullptr && builder->NumEntries() > 0) {
      Status st = builder->Finish();
      if (!st.ok()) return st;
      st = outfile->Close();
      if (!st.ok()) return st;

      c->edit()->AddFile(out_level, current_file_number, builder->FileSize(),
                         Slice(current_smallest), Slice(current_largest));
      builder.reset();
      outfile.reset();
    }
    return Status::OK();
  };

  std::string last_user_key;
  bool has_last_user_key = false;

  for (input->SeekToFirst(); input->Valid(); input->Next()) {
    Slice ikey = input->key();
    Slice val = input->value();
    if (ikey.size() < 8) continue;

    Slice ukey(ikey.data(), ikey.size() - 8);
    uint64_t tag = DecodeFixed64(ikey.data() + ikey.size() - 8);
    SequenceNumber seq = tag >> 8;
    ValueType type = static_cast<ValueType>(tag & 0xff);

    bool drop = false;
    if (!has_last_user_key || ukey != Slice(last_user_key)) {
      last_user_key = ukey.ToString();
      has_last_user_key = true;
    } else {
      // Overwritten older version of the same user key
      if (seq <= smallest_snapshot) {
        drop = true;
      }
    }

    if (!drop && type == kTypeDeletion) {
      if (seq <= smallest_snapshot && c->IsBaseLevelForKey(ukey)) {
        drop = true;
      }
    }

    if (!drop) {
      if (builder == nullptr) {
        s = open_compaction_output();
        if (!s.ok()) return s;
      }
      if (current_smallest.empty()) {
        current_smallest = ikey.ToString();
      }
      current_largest = ikey.ToString();
      builder->Add(ikey, val);

      if (builder->FileSize() >= 2 * 1024 * 1024) {
        s = finish_compaction_output();
        if (!s.ok()) return s;
      }
    }
  }

  s = finish_compaction_output();
  if (!s.ok()) return s;

  // Release all input iterators and tables so open file handles are closed
  input.reset();
  input_iters.clear();

  // Mark input files for deletion in manifest
  for (int which = 0; which < 2; ++which) {
    int lvl = (which == 0) ? c->level() : c->level() + 1;
    for (const auto& f : c->inputs(which)) {
      c->edit()->RemoveFile(lvl, f.number);
    }
  }

  // Preserve current log number in compaction edit
  c->edit()->SetLogNumber(log_number_);

  // Atomically apply compaction edit to VersionSet
  s = versions_->LogAndApply(c->edit());
  if (!s.ok()) return s;

  // Delete obsolete on-disk files
  for (int which = 0; which < 2; ++which) {
    for (const auto& f : c->inputs(which)) {
      env_->DeleteFile(TableFileName(dbname_, f.number));
    }
  }

  return Status::OK();
}

void DBImpl::MaybeScheduleCompaction() {
  if (bg_compaction_scheduled_) {
    return;
  }
  if (shutting_down_ || !recovered_) {
    return;
  }
  if (imm_ == nullptr && versions_->PickCompactionLevel() < 0) {
    return;
  }
  bg_compaction_scheduled_ = true;
  bg_cv_.notify_one();
}

void DBImpl::BackgroundThreadEntry() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!shutting_down_) {
    while (!shutting_down_ && (!recovered_ || !bg_compaction_scheduled_)) {
      bg_cv_.wait(lock);
    }
    if (shutting_down_) {
      break;
    }

    if (imm_ != nullptr) {
      uint64_t new_log_number = versions_->NewFileNumber();
      if (log_file_) {
        log_file_->Close();
      }
      log_.reset();
      log_file_.reset();
      Status s = env_->NewWritableFile(LogFileName(dbname_, new_log_number), &log_file_);
      if (s.ok()) {
        log_ = std::make_unique<wal::WalWriter>(log_file_.get());
        s = FlushMemTableToOutputFile(imm_.get(), new_log_number);
        if (s.ok()) {
          env_->DeleteFile(LogFileName(dbname_, log_number_));
          log_number_ = new_log_number;
          imm_.reset();
        }
      }
    } else {
      auto c = versions_->PickCompaction();
      if (c != nullptr) {
        DoCompactionWork(c.get());
      }
    }

    bg_compaction_scheduled_ = false;
    bg_cv_.notify_all();

    // Check if more compaction work is needed
    if (imm_ != nullptr || versions_->PickCompactionLevel() >= 0) {
      bg_compaction_scheduled_ = true;
    }
  }
}

void DBImpl::RemoveOrphanFiles() {
  std::vector<std::string> filenames;
  Status s = env_->GetChildren(dbname_, &filenames);
  if (!s.ok()) {
    return;
  }

  std::unordered_set<uint64_t> live_sstables;
  if (versions_->current() != nullptr) {
    for (int l = 0; l < kNumLevels; ++l) {
      for (const auto& f : versions_->current()->files(l)) {
        live_sstables.insert(f.number);
      }
    }
  }

  uint64_t current_manifest = versions_->ManifestFileNumber();
  uint64_t current_log = log_number_;
  uint64_t prev_log = versions_->PrevLogNumber();

  for (const auto& fname : filenames) {
    uint64_t number = 0;
    FileType type;
    if (ParseFileName(fname, &number, &type)) {
      bool keep = true;
      switch (type) {
        case kLogFile:
          keep = (number == current_log || (prev_log > 0 && number == prev_log));
          break;
        case kDescriptorFile:
          keep = (number >= current_manifest);
          break;
        case kTableFile:
          keep = (live_sstables.find(number) != live_sstables.end());
          break;
        case kTempFile:
          keep = false;
          break;
        case kCurrentFile:
        case kDBLockFile:
        case kInfoLogFile:
          keep = true;
          break;
      }

      if (!keep) {
        env_->DeleteFile(dbname_ + "/" + fname);
      }
    }
  }
}

}  // namespace purpledb
