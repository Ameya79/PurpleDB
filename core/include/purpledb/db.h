#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include "core/storage/cache.h"
#include "core/storage/env.h"
#include "purpledb/filter_policy.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"
#include "purpledb/write_batch.h"

namespace purpledb {

struct Options {
  // Create database files and directory if they do not exist.
  bool create_if_missing{true};

  // If true, return an error if the database already exists.
  bool error_if_exists{false};

  // Environment to use for filesystem and OS operations.
  Env* env{Env::Default()};

  // Whether all writes are synchronously flushed and fsynced by default.
  bool sync{false};

  // Amount of data to build up in memory before converting to an on-disk SSTable.
  // Default: 4 MiB.
  size_t write_buffer_size{4 * 1024 * 1024};

  // Approximate size of user data packed per block in SSTables. Default: 4 KiB.
  size_t block_size{4096};

  // Control block cache (holds uncompressed blocks in memory).
  // If nullptr, PurpleDB will automatically create a default 8 MiB cache.
  std::shared_ptr<Cache> block_cache{nullptr};

  // Filter policy to use for fast Bloom filter rejection checks on point reads.
  // Default: nullptr (no filter). If set (e.g. NewBloomFilterPolicy(10)),
  // SSTables generate Bloom filters to avoid reading blocks for missing keys.
  const FilterPolicy* filter_policy{nullptr};
};

struct WriteOptions {
  // If true, the write will be flushed and synced to disk before returning.
  bool sync{false};
};

struct ReadOptions {
  // Verify checksums on records read from storage.
  bool verify_checksums{true};

  // Sequence number to read at (0 means newest committed version).
  SequenceNumber snapshot{0};
};

/**
 * @brief DB is the persistent embedded key-value storage engine.
 *
 * CONCEPT FOR LEARNERS:
 * DB provides durable Put, Get, Delete, and atomic WriteBatch operations.
 * Mutations are appended sequentially to the Write-Ahead Log (WAL) before
 * being inserted into the in-memory MemTable. When the MemTable fills up,
 * it is converted to an immutable MemTable and flushed to a Level 0 SSTable file.
 * Point reads resolve across MemTable -> Immutable MemTable -> SSTables.
 */
class DB {
 public:
  // Open the database with the specified "name" (directory path).
  static Status Open(const Options& options, const std::string& name,
                     std::unique_ptr<DB>* dbptr);

  virtual ~DB() = default;

  // Insert or overwrite the value for "key".
  virtual Status Put(const WriteOptions& options, const Slice& key, const Slice& value) = 0;

  // Remove the database entry for "key".
  virtual Status Delete(const WriteOptions& options, const Slice& key) = 0;

  // Apply the specified updates atomically.
  virtual Status Write(const WriteOptions& options, WriteBatch* updates) = 0;

  // Retrieve the value for "key". Returns NotFound if key does not exist.
  virtual Status Get(const ReadOptions& options, const Slice& key, std::string* value) = 0;

  // Convenience overloads with default options
  Status Put(const Slice& key, const Slice& value) {
    return Put(WriteOptions(), key, value);
  }
  Status Delete(const Slice& key) {
    return Delete(WriteOptions(), key);
  }
  Status Get(const Slice& key, std::string* value) {
    return Get(ReadOptions(), key, value);
  }
};

}  // namespace purpledb
