#include "core/storage/env.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace purpledb {

namespace {

namespace fs = std::filesystem;

class StdioSequentialFile : public SequentialFile {
 public:
  StdioSequentialFile(std::string fname, std::FILE* f)
      : filename_(std::move(fname)), file_(f) {}

  ~StdioSequentialFile() override {
    if (file_) {
      std::fclose(file_);
    }
  }

  Status Read(size_t n, Slice* result, char* scratch) override {
    size_t r = std::fread(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (std::feof(file_)) {
        // Normal EOF
        return Status::OK();
      }
      return Status::IOError(filename_, "fread failed");
    }
    return Status::OK();
  }

  Status Skip(uint64_t n) override {
    if (std::fseek(file_, static_cast<long>(n), SEEK_CUR) != 0) {
      return Status::IOError(filename_, "fseek failed");
    }
    return Status::OK();
  }

 private:
  std::string filename_;
  std::FILE* file_;
};

class StdioWritableFile : public WritableFile {
 public:
  StdioWritableFile(std::string fname, std::FILE* f)
      : filename_(std::move(fname)), file_(f), pos_(0) {}

  ~StdioWritableFile() override {
    if (file_) {
      Close();
    }
  }

  Status Append(const Slice& data) override {
    size_t write_size = data.size();
    const char* write_data = data.data();

    // Fill buffer if possible
    size_t copy_size = std::min(write_size, kWritableFileBufferSize - pos_);
    std::memcpy(buf_ + pos_, write_data, copy_size);
    write_data += copy_size;
    write_size -= copy_size;
    pos_ += copy_size;

    if (write_size == 0) {
      return Status::OK();
    }

    // Flush full buffer
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }

    // Direct write if remaining data is larger than buffer
    if (write_size >= kWritableFileBufferSize) {
      return WriteUnbuffered(write_data, write_size);
    }

    std::memcpy(buf_, write_data, write_size);
    pos_ = write_size;
    return Status::OK();
  }

  Status Flush() override {
    Status s = FlushBuffer();
    if (!s.ok()) {
      return s;
    }
    if (std::fflush(file_) != 0) {
      return Status::IOError(filename_, "fflush failed");
    }
    return Status::OK();
  }

  Status Sync() override {
    Status s = Flush();
    if (!s.ok()) {
      return s;
    }
#if defined(_WIN32)
    int fd = _fileno(file_);
    if (fd >= 0 && _commit(fd) != 0) {
      return Status::IOError(filename_, "_commit failed");
    }
#else
    int fd = fileno(file_);
    if (fd >= 0 && fsync(fd) != 0) {
      return Status::IOError(filename_, "fsync failed");
    }
#endif
    return Status::OK();
  }

  Status Close() override {
    Status s = Flush();
    if (file_) {
      if (std::fclose(file_) != 0 && s.ok()) {
        s = Status::IOError(filename_, "fclose failed");
      }
      file_ = nullptr;
    }
    return s;
  }

 private:
  static constexpr size_t kWritableFileBufferSize = 65536;

  Status FlushBuffer() {
    Status s = WriteUnbuffered(buf_, pos_);
    pos_ = 0;
    return s;
  }

  Status WriteUnbuffered(const char* data, size_t size) {
    if (size == 0) return Status::OK();
    size_t written = std::fwrite(data, 1, size, file_);
    if (written != size) {
      return Status::IOError(filename_, "fwrite failed");
    }
    return Status::OK();
  }

  std::string filename_;
  std::FILE* file_;
  char buf_[kWritableFileBufferSize];
  size_t pos_;
};

class PosixEnv : public Env {
 public:
  Status NewSequentialFile(const std::string& fname,
                           std::unique_ptr<SequentialFile>* result) override {
    std::FILE* f = std::fopen(fname.c_str(), "rb");
    if (!f) {
      return Status::NotFound(fname, "cannot open file for reading");
    }
    result->reset(new StdioSequentialFile(fname, f));
    return Status::OK();
  }

  Status NewWritableFile(const std::string& fname,
                         std::unique_ptr<WritableFile>* result) override {
    std::FILE* f = std::fopen(fname.c_str(), "wb");
    if (!f) {
      return Status::IOError(fname, "cannot open file for writing");
    }
    result->reset(new StdioWritableFile(fname, f));
    return Status::OK();
  }

  Status NewAppendableFile(const std::string& fname,
                           std::unique_ptr<WritableFile>* result) override {
    std::FILE* f = std::fopen(fname.c_str(), "ab");
    if (!f) {
      return Status::IOError(fname, "cannot open file for appending");
    }
    result->reset(new StdioWritableFile(fname, f));
    return Status::OK();
  }

  bool FileExists(const std::string& fname) override {
    std::error_code ec;
    return fs::exists(fname, ec);
  }

  Status GetChildren(const std::string& dir,
                     std::vector<std::string>* result) override {
    result->clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
      result->push_back(entry.path().filename().string());
    }
    if (ec) {
      return Status::IOError(dir, ec.message());
    }
    return Status::OK();
  }

  Status DeleteFile(const std::string& fname) override {
    std::error_code ec;
    if (!fs::remove(fname, ec)) {
      if (ec) {
        return Status::IOError(fname, ec.message());
      }
      return Status::NotFound(fname, "file does not exist");
    }
    return Status::OK();
  }

  Status CreateDir(const std::string& dirname) override {
    std::error_code ec;
    if (!fs::create_directory(dirname, ec)) {
      if (ec) {
        return Status::IOError(dirname, ec.message());
      }
      return Status::IOError(dirname, "directory could not be created");
    }
    return Status::OK();
  }

  Status CreateDirIfMissing(const std::string& dirname) override {
    std::error_code ec;
    fs::create_directories(dirname, ec);
    if (ec) {
      return Status::IOError(dirname, ec.message());
    }
    return Status::OK();
  }

  Status DeleteDir(const std::string& dirname) override {
    std::error_code ec;
    fs::remove_all(dirname, ec);
    if (ec) {
      return Status::IOError(dirname, ec.message());
    }
    return Status::OK();
  }

  Status GetFileSize(const std::string& fname, uint64_t* size) override {
    std::error_code ec;
    auto s = fs::file_size(fname, ec);
    if (ec) {
      return Status::IOError(fname, ec.message());
    }
    *size = static_cast<uint64_t>(s);
    return Status::OK();
  }

  Status RenameFile(const std::string& src, const std::string& target) override {
    std::error_code ec;
    fs::rename(src, target, ec);
    if (ec) {
      return Status::IOError(src, ec.message());
    }
    return Status::OK();
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv default_env;
  return &default_env;
}

}  // namespace purpledb
