#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief SequentialFile represents a readable file that can only be accessed sequentially.
 */
class SequentialFile {
 public:
  virtual ~SequentialFile() = default;

  // Read up to "n" bytes from the file.
  // "scratch[0..n-1]" may be used by the implementation.
  // Sets "*result" to the data that was read (may be pointing into scratch or internal buffer).
  // If end of file is reached, result->size() will be < n.
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

  // Skip "n" bytes from the file.
  virtual Status Skip(uint64_t n) = 0;
};

/**
 * @brief RandomAccessFile represents a readable file that can be read at arbitrary byte offsets.
 *
 * Implementations must be thread-safe for concurrent Read() calls.
 */
class RandomAccessFile {
 public:
  virtual ~RandomAccessFile() = default;

  // Read up to "n" bytes from the file starting at "offset".
  // "scratch[0..n-1]" may be used by the implementation.
  // Sets "*result" to the data that was read.
  virtual Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const = 0;
};

/**
 * @brief WritableFile represents an append-only file for writes.
 */
class WritableFile {
 public:
  virtual ~WritableFile() = default;

  virtual Status Append(const Slice& data) = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;
  virtual Status Close() = 0;
};

/**
 * @brief Env abstracts the operating system environment (filesystem, clocks, etc.).
 *
 * CONCEPT FOR LEARNERS:
 * Operating system abstraction layers (like LevelDB's Env or SQLite's VFS)
 * decouple database storage code from concrete OS APIs (Windows vs POSIX).
 * This makes the core engine 100% portable and enables fault-injecting test
 * wrappers that can simulate disk failures, dropped writes, and kernel crashes.
 */
class Env {
 public:
  virtual ~Env() = default;

  // Return the default platform environment.
  static Env* Default();

  virtual Status NewSequentialFile(const std::string& fname,
                                   std::unique_ptr<SequentialFile>* result) = 0;
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     std::unique_ptr<RandomAccessFile>* result) = 0;
  virtual Status NewWritableFile(const std::string& fname,
                                 std::unique_ptr<WritableFile>* result) = 0;
  virtual Status NewAppendableFile(const std::string& fname,
                                   std::unique_ptr<WritableFile>* result) = 0;

  virtual bool FileExists(const std::string& fname) = 0;
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;
  virtual Status DeleteFile(const std::string& fname) = 0;
  virtual Status CreateDir(const std::string& dirname) = 0;
  virtual Status CreateDirIfMissing(const std::string& dirname) = 0;
  virtual Status DeleteDir(const std::string& dirname) = 0;
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) = 0;
  virtual Status RenameFile(const std::string& src, const std::string& target) = 0;
};

}  // namespace purpledb
