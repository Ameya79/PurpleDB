#pragma once

#include <memory>
#include <string>
#include "core/storage/env.h"
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
 * being inserted into the in-memory MemTable. On startup or crash recovery,
 * the database replays the WAL to reconstruct the MemTable state, safely
 * ignoring truncated or uncommitted records at the tail.
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
