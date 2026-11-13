#include "core/storage/filename.h"

#include <cstdio>
#include "purpledb/slice.h"

namespace purpledb {

namespace {

std::string MakeFileName(const std::string& dbname, uint64_t number, const char* prefix,
                         const char* suffix) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "/%s%06llu%s", prefix,
                static_cast<unsigned long long>(number), suffix);
  return dbname + buf;
}

}  // namespace

std::string LogFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "", ".log");
}

std::string TableFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "", ".sst");
}

std::string ManifestFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "MANIFEST-", "");
}

std::string CurrentFileName(const std::string& dbname) {
  return dbname + "/CURRENT";
}

std::string TempFileName(const std::string& dbname, uint64_t number) {
  return MakeFileName(dbname, number, "", ".dbtmp");
}

Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t manifest_number) {
  char manifest_buf[64];
  std::snprintf(manifest_buf, sizeof(manifest_buf), "MANIFEST-%06llu",
                static_cast<unsigned long long>(manifest_number));

  std::string contents = std::string(manifest_buf) + "\n";
  std::string tmp_file = TempFileName(dbname, manifest_number);
  std::string current_file = CurrentFileName(dbname);

  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(tmp_file, &file);
  if (!s.ok()) return s;

  s = file->Append(Slice(contents));
  if (s.ok()) {
    s = file->Sync();
  }
  if (s.ok()) {
    s = file->Close();
  }
  file.reset();

  if (s.ok()) {
    s = env->RenameFile(tmp_file, current_file);
  }
  if (!s.ok()) {
    env->DeleteFile(tmp_file);
  }
  return s;
}

}  // namespace purpledb

