#include "core/storage/version_set.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include "core/storage/compaction.h"
#include "core/storage/filename.h"
#include "core/storage/memtable.h"
#include "core/storage/wal_reader.h"
#include "core/util/coding.h"
#include "core/util/logging.h"
#include "purpledb/db.h"
#include "purpledb/write_batch.h"

namespace purpledb {

Version::Version(VersionSet* vset) : vset_(vset), refs_(0) {}

Version::~Version() {
  assert(refs_ == 0);
  // Remove from circular linked list
  prev_->next_ = next_;
  next_->prev_ = prev_;
}

bool Version::Get(const Slice& user_key, SequenceNumber snapshot_seq,
                  std::string* val, Status* s) {
  static const InternalKeyBlockComparator kInternalKeyBlockComparator;
  std::string lookup_key;
  lookup_key.append(user_key.data(), user_key.size());
  PutFixed64(&lookup_key, (snapshot_seq << 8) | static_cast<uint8_t>(kTypeValue));
  Cache* block_cache = (vset_->options() != nullptr) ? vset_->options()->block_cache.get() : nullptr;

  // 1. Search Level 0 files in reverse order (newest to oldest, may overlap)
  for (int i = static_cast<int>(files_[0].size()) - 1; i >= 0; --i) {
    const FileMetaData& f = files_[0][i];
    if (f.smallest.size() < 8 || f.largest.size() < 8) {
      continue;
    }

    Slice smallest_user(f.smallest.data(), f.smallest.size() - 8);
    Slice largest_user(f.largest.data(), f.largest.size() - 8);

    if (user_key.compare(smallest_user) < 0 || user_key.compare(largest_user) > 0) {
      continue;
    }

    std::unique_ptr<Table> table;
    Status open_status = vset_->GetTable(f.number, f.file_size, &table);
    if (!open_status.ok()) {
      *s = open_status;
      return true;
    }

    if (table->InternalGet(Slice(lookup_key), &kInternalKeyBlockComparator, val, s, block_cache)) {
      return true;
    }
    if (!s->ok()) {
      return true;
    }
  }

  // 2. Search Level 1 to kNumLevels - 1 (non-overlapping, disjoint key ranges per level)
  for (int level = 1; level < kNumLevels; ++level) {
    if (files_[level].empty()) {
      continue;
    }

    // Binary search for candidate file covering user_key
    int left = 0;
    int right = static_cast<int>(files_[level].size()) - 1;
    int candidate = -1;
    while (left <= right) {
      int mid = left + (right - left) / 2;
      const FileMetaData& f = files_[level][mid];
      Slice largest_user = (f.largest.size() >= 8) ? Slice(f.largest.data(), f.largest.size() - 8) : Slice(f.largest);
      if (largest_user.compare(user_key) >= 0) {
        candidate = mid;
        right = mid - 1;
      } else {
        left = mid + 1;
      }
    }

    if (candidate != -1) {
      const FileMetaData& f = files_[level][candidate];
      Slice smallest_user = (f.smallest.size() >= 8) ? Slice(f.smallest.data(), f.smallest.size() - 8) : Slice(f.smallest);
      if (user_key.compare(smallest_user) >= 0) {
        // user_key is strictly within [smallest_user, largest_user]
        std::unique_ptr<Table> table;
        Status open_status = vset_->GetTable(f.number, f.file_size, &table);
        if (!open_status.ok()) {
          *s = open_status;
          return true;
        }

        if (table->InternalGet(Slice(lookup_key), &kInternalKeyBlockComparator, val, s, block_cache)) {
          return true;
        }
        if (!s->ok()) {
          return true;
        }
      }
    }
  }

  return false;
}

uint64_t Version::TotalBytes(int level) const {
  uint64_t total = 0;
  for (const auto& f : files_[level]) {
    total += f.file_size;
  }
  return total;
}

void Version::GetOverlappingInputs(int level, const Slice& smallest_user,
                                   const Slice& largest_user,
                                   std::vector<FileMetaData>* inputs) const {
  inputs->clear();
  for (const auto& f : files_[level]) {
    Slice f_smallest = (f.smallest.size() >= 8) ? Slice(f.smallest.data(), f.smallest.size() - 8) : Slice(f.smallest);
    Slice f_largest = (f.largest.size() >= 8) ? Slice(f.largest.data(), f.largest.size() - 8) : Slice(f.largest);
    if (f_smallest.compare(largest_user) <= 0 && f_largest.compare(smallest_user) >= 0) {
      inputs->push_back(f);
    }
  }
}

void Version::AddIterators(std::vector<std::unique_ptr<Iterator>>* iters) {
  static const InternalKeyBlockComparator kInternalKeyBlockComparator;
  for (int level = 0; level < kNumLevels; ++level) {
    for (const auto& f : files_[level]) {
      std::unique_ptr<Table> table;
      if (vset_->GetTable(f.number, f.file_size, &table).ok()) {
        std::shared_ptr<Table> shared_table = std::move(table);
        iters->push_back(Table::NewIterator(shared_table, &kInternalKeyBlockComparator));
      }
    }
  }
}

class VersionSet::Builder {
 public:
  Builder(VersionSet* vset, Version* base) : vset_(vset), base_(base) {
    base_->Ref();
    for (int level = 0; level < kNumLevels; level++) {
      added_files_[level] = base_->files_[level];
    }
  }

  ~Builder() {
    base_->Unref();
  }

  void Apply(const VersionEdit* edit) {
    // Delete files
    for (const auto& df : edit->deleted_files()) {
      const int level = df.first;
      const uint64_t number = df.second;
      auto& flist = added_files_[level];
      flist.erase(
          std::remove_if(flist.begin(), flist.end(),
                         [number](const FileMetaData& f) { return f.number == number; }),
          flist.end());
    }

    // Add new files
    for (const auto& nf : edit->new_files()) {
      const int level = nf.first;
      const FileMetaData& f = nf.second;
      added_files_[level].push_back(f);
    }
  }

  void SaveTo(Version* v) {
    for (int level = 0; level < kNumLevels; level++) {
      v->files_[level] = added_files_[level];
    }
    // Maintain non-overlapping sorted order for levels >= 1
    for (int level = 1; level < kNumLevels; level++) {
      std::sort(v->files_[level].begin(), v->files_[level].end(),
                [](const FileMetaData& a, const FileMetaData& b) {
                  Slice a_user = (a.smallest.size() >= 8) ? Slice(a.smallest.data(), a.smallest.size() - 8) : Slice(a.smallest);
                  Slice b_user = (b.smallest.size() >= 8) ? Slice(b.smallest.data(), b.smallest.size() - 8) : Slice(b.smallest);
                  return a_user.compare(b_user) < 0;
                });
    }
  }

 private:
  VersionSet* vset_;
  Version* base_;
  std::vector<FileMetaData> added_files_[kNumLevels];
};

VersionSet::VersionSet(std::string dbname, Env* env, const Options* options)
    : dbname_(std::move(dbname)),
      env_(env),
      options_(options),
      dummy_versions_(this) {
  AppendVersion(new Version(this));
}

VersionSet::~VersionSet() {
  current_->Unref();
  assert(dummy_versions_.next_ == &dummy_versions_);
}

void VersionSet::AppendVersion(Version* v) {
  assert(v->refs_ == 0);
  assert(v != current_);
  if (current_ != nullptr) {
    current_->Unref();
  }
  current_ = v;
  v->Ref();

  // Add to circular list before dummy_versions_
  v->prev_ = dummy_versions_.prev_;
  v->next_ = &dummy_versions_;
  v->prev_->next_ = v;
  v->next_->prev_ = v;
}

Status VersionSet::GetTable(uint64_t file_number, uint64_t file_size,
                            std::unique_ptr<Table>* table) const {
  std::string fname = TableFileName(dbname_, file_number);
  if (file_size == 0) {
    Status s = env_->GetFileSize(fname, &file_size);
    if (!s.ok()) return s;
  }

  std::unique_ptr<RandomAccessFile> file;
  Status s = env_->NewRandomAccessFile(fname, &file);
  if (!s.ok()) return s;

  const FilterPolicy* filter_policy = options_ ? options_->filter_policy : nullptr;
  return Table::Open(std::move(file), file_size, table, filter_policy);
}

int VersionSet::PickCompactionLevel() const {
  // Level 0: compact when >= 4 files
  if (current_->NumLevelFiles(0) >= 4) {
    return 0;
  }

  // Level 1 to 5: check byte size limits
  double best_score = 1.0;
  int best_level = -1;
  for (int level = 1; level < kNumLevels - 1; ++level) {
    double limit = MaxBytesForLevel(level);
    double score = static_cast<double>(current_->TotalBytes(level)) / limit;
    if (score > best_score) {
      best_score = score;
      best_level = level;
    }
  }
  return best_level;
}

double VersionSet::MaxBytesForLevel(int level) const {
  // L1 target: 64 MB. Level N+1 = 10x Level N
  double result = 64.0 * 1048576.0;
  for (int i = 1; i < level; ++i) {
    result *= 10.0;
  }
  return result;
}

std::unique_ptr<Compaction> VersionSet::PickCompaction() {
  int level = PickCompactionLevel();
  if (level < 0) {
    return nullptr;
  }

  auto c = std::make_unique<Compaction>(level, current_);
  if (level == 0) {
    std::string smallest_user;
    std::string largest_user;
    for (const auto& f : current_->files(0)) {
      c->AddInput(0, f);
      Slice f_smallest = (f.smallest.size() >= 8) ? Slice(f.smallest.data(), f.smallest.size() - 8) : Slice(f.smallest);
      Slice f_largest = (f.largest.size() >= 8) ? Slice(f.largest.data(), f.largest.size() - 8) : Slice(f.largest);
      if (smallest_user.empty() || f_smallest.compare(Slice(smallest_user)) < 0) {
        smallest_user = f_smallest.ToString();
      }
      if (largest_user.empty() || f_largest.compare(Slice(largest_user)) > 0) {
        largest_user = f_largest.ToString();
      }
    }

    std::vector<FileMetaData> overlaps;
    current_->GetOverlappingInputs(1, Slice(smallest_user), Slice(largest_user), &overlaps);
    for (const auto& f : overlaps) {
      c->AddInput(1, f);
    }
  } else {
    assert(!current_->files(level).empty());
    const auto& f = current_->files(level)[0];
    c->AddInput(0, f);
    Slice f_smallest = (f.smallest.size() >= 8) ? Slice(f.smallest.data(), f.smallest.size() - 8) : Slice(f.smallest);
    Slice f_largest = (f.largest.size() >= 8) ? Slice(f.largest.data(), f.largest.size() - 8) : Slice(f.largest);
    std::vector<FileMetaData> overlaps;
    current_->GetOverlappingInputs(level + 1, f_smallest, f_largest, &overlaps);
    for (const auto& f_next : overlaps) {
      c->AddInput(1, f_next);
    }
  }

  return c;
}

Status VersionSet::LogAndApply(VersionEdit* edit) {
  if (edit->has_next_file_number()) {
    if (edit->next_file_number() > next_file_number_) {
      next_file_number_ = edit->next_file_number();
    }
  }

  if (edit->has_log_number()) {
    assert(edit->log_number() >= log_number_);
    if (edit->log_number() >= next_file_number_) {
      next_file_number_ = edit->log_number() + 1;
    }
  } else {
    edit->SetLogNumber(log_number_);
  }

  if (!edit->has_prev_log_number()) {
    edit->SetPrevLogNumber(prev_log_number_);
  }

  edit->SetNextFileNumber(next_file_number_);
  edit->SetLastSequence(last_sequence_);

  Version* v = new Version(this);
  Builder builder(this, current_);
  builder.Apply(edit);
  builder.SaveTo(v);

  // Initialize new manifest if needed
  std::string new_manifest_file;
  Status s;
  if (descriptor_log_ == nullptr) {
    assert(descriptor_file_ == nullptr);
    new_manifest_file = ManifestFileName(dbname_, manifest_file_number_);
    edit->SetNextFileNumber(next_file_number_);

    if (env_->FileExists(new_manifest_file)) {
      uint64_t manifest_size = 0;
      env_->GetFileSize(new_manifest_file, &manifest_size);
      s = env_->NewAppendableFile(new_manifest_file, &descriptor_file_);
      if (s.ok()) {
        descriptor_log_ = std::make_unique<wal::WalWriter>(descriptor_file_.get(), manifest_size);
      }
    } else {
      s = env_->NewWritableFile(new_manifest_file, &descriptor_file_);
      if (s.ok()) {
        descriptor_log_ = std::make_unique<wal::WalWriter>(descriptor_file_.get());
      }
    }
  }

  // Write record to manifest
  if (s.ok()) {
    std::string record;
    edit->EncodeTo(&record);
    s = descriptor_log_->AddRecord(Slice(record));
    if (s.ok()) {
      s = descriptor_file_->Sync();
    }
  }

  // Atomically update CURRENT
  if (s.ok() && !new_manifest_file.empty()) {
    s = SetCurrentFile(env_, dbname_, manifest_file_number_);
  }

  // Install new version
  if (s.ok()) {
    AppendVersion(v);
    log_number_ = edit->log_number();
    prev_log_number_ = edit->prev_log_number();
  } else {
    delete v;
    if (!new_manifest_file.empty()) {
      descriptor_log_.reset();
      descriptor_file_.reset();
      env_->DeleteFile(new_manifest_file);
    }
  }

  return s;
}

Status VersionSet::Recover() {
  std::string current_path = CurrentFileName(dbname_);
  if (!env_->FileExists(current_path)) {
    // Database does not exist yet
    return Status::OK();
  }

  // Read CURRENT
  std::unique_ptr<SequentialFile> cfile;
  Status s = env_->NewSequentialFile(current_path, &cfile);
  if (!s.ok()) return s;

  char buf[256];
  Slice contents;
  s = cfile->Read(sizeof(buf), &contents, buf);
  cfile.reset();
  if (!s.ok()) return s;

  std::string manifest_line = contents.ToString();
  while (!manifest_line.empty() &&
         (manifest_line.back() == '\r' || manifest_line.back() == '\n' ||
          manifest_line.back() == ' ')) {
    manifest_line.pop_back();
  }

  if (manifest_line.empty()) {
    return Status::Corruption("CURRENT file is empty");
  }

  std::string dscname = dbname_ + "/" + manifest_line;
  std::unique_ptr<SequentialFile> mfile;
  s = env_->NewSequentialFile(dscname, &mfile);
  if (!s.ok()) return s;

  wal::WalReader reader(mfile.get(), nullptr, true /*checksum*/, 0 /*initial_offset*/);
  Slice record;
  std::string scratch;
  Builder builder(this, current_);

  uint64_t next_file = 0;
  uint64_t last_seq = 0;
  uint64_t log_num = 0;
  uint64_t prev_log_num = 0;

  while (reader.ReadRecord(&record, &scratch)) {
    VersionEdit edit;
    s = edit.DecodeFrom(record);
    if (!s.ok()) return s;

    builder.Apply(&edit);

    if (edit.has_log_number()) log_num = edit.log_number();
    if (edit.has_prev_log_number()) prev_log_num = edit.prev_log_number();
    if (edit.has_next_file_number()) next_file = edit.next_file_number();
    if (edit.has_last_sequence()) last_seq = edit.last_sequence();
  }

  // Parse manifest number from filename: MANIFEST-%06llu
  uint64_t manifest_num = 0;
  if (std::sscanf(manifest_line.c_str(), "MANIFEST-%llu",
                  reinterpret_cast<unsigned long long*>(&manifest_num)) == 1) {
    manifest_file_number_ = manifest_num;
  }

  if (next_file > 0) next_file_number_ = next_file;
  if (last_seq > 0) last_sequence_ = last_seq;
  if (log_num > 0) log_number_ = log_num;
  if (prev_log_num > 0) prev_log_number_ = prev_log_num;

  Version* v = new Version(this);
  builder.SaveTo(v);
  AppendVersion(v);

  return Status::OK();
}

}  // namespace purpledb
