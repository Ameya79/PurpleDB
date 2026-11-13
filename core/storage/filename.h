#pragma once

#include <cstdint>
#include <string>
#include "core/storage/env.h"
#include "purpledb/status.h"

namespace purpledb {

std::string LogFileName(const std::string& dbname, uint64_t number);
std::string TableFileName(const std::string& dbname, uint64_t number);
std::string ManifestFileName(const std::string& dbname, uint64_t number);
std::string CurrentFileName(const std::string& dbname);
std::string TempFileName(const std::string& dbname, uint64_t number);

// Atomically point CURRENT to MANIFEST-<manifest_number>
Status SetCurrentFile(Env* env, const std::string& dbname, uint64_t manifest_number);

}  // namespace purpledb

