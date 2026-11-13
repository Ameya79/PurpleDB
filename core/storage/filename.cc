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

bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type) {
  // Strip any leading directories
  std::string base = filename;
  size_t last_slash = base.find_last_of("/\\");
  if (last_slash != std::string::npos) {
    base = base.substr(last_slash + 1);
  }

  if (base == "CURRENT") {
    *number = 0;
    *type = kCurrentFile;
    return true;
  }

  if (base.rfind("MANIFEST-", 0) == 0) {
    try {
      *number = std::stoull(base.substr(9));
      *type = kDescriptorFile;
      return true;
    } catch (...) {
      return false;
    }
  }

  if (base.size() > 4 && base.substr(base.size() - 4) == ".sst") {
    try {
      *number = std::stoull(base.substr(0, base.size() - 4));
      *type = kTableFile;
      return true;
    } catch (...) {
      return false;
    }
  }

  if (base.size() > 4 && base.substr(base.size() - 4) == ".log") {
    try {
      *number = std::stoull(base.substr(0, base.size() - 4));
      *type = kLogFile;
      return true;
    } catch (...) {
      return false;
    }
  }

  if (base.size() > 6 && base.substr(base.size() - 6) == ".dbtmp") {
    try {
      *number = std::stoull(base.substr(0, base.size() - 6));
      *type = kTempFile;
      return true;
    } catch (...) {
      return false;
    }
  }

  return false;
}

}  // namespace purpledb

