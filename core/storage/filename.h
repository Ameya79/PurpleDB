#pragma once

#include <cstdint>
#include <string>
#include "core/storage/env.h"
#include "purpledb/status.h"

namespace purpledb {

enum FileType {
  kLogFile,
  kDBLockFile,
  kTableFile,
  kDescriptorFile,
  kCurrentFile,
  kTempFile,
  kInfoLogFile
};

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);

// Atomically point CURRENT to MANIFEST-<manifest_number>
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t manifest_number);

// Parse filename into file number and type
bool ParseFileName(const std::string& filename, uint64_t* number, FileType* type);

}  // namespace purpledb

