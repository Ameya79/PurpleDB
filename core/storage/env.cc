#include "core/storage/env.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef DeleteFile
#include <io.h>
#else
#include <fcntl.h>
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

#if defined(_WIN32)
class WindowsRandomAccessFile : public RandomAccessFile {
 public:
  WindowsRandomAccessFile(std::string fname, HANDLE handle)
      : filename_(std::move(fname)), handle_(handle) {}

  ~WindowsRandomAccessFile() override {
    if (handle_ != INVALID_HANDLE_VALUE) {
      ::CloseHandle(handle_);
    }
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    OVERLAPPED ol;
    std::memset(&ol, 0, sizeof(ol));
    ol.Offset = static_cast<DWORD>(offset & 0xffffffff);
    ol.OffsetHigh = static_cast<DWORD>(offset >> 32);

    DWORD bytes_read = 0;
    if (!::ReadFile(handle_, scratch, static_cast<DWORD>(n), &bytes_read, &ol)) {
      DWORD err = ::GetLastError();
      if (err != ERROR_HANDLE_EOF) {
        return Status::IOError(filename_, "ReadFile failed");
      }
    }
    *result = Slice(scratch, bytes_read);
    return Status::OK();
  }

 private:
  std::string filename_;
  HANDLE handle_;
};
#else
class PosixRandomAccessFile : public RandomAccessFile {
 public:
  PosixRandomAccessFile(std::string fname, int fd)
      : filename_(std::move(fname)), fd_(fd) {}

  ~PosixRandomAccessFile() override {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  Status Read(uint64_t offset, size_t n, Slice* result, char* scratch) const override {
    ssize_t r = ::pread(fd_, scratch, n, static_cast<off_t>(offset));
    if (r < 0) {
      return Status::IOError(filename_, "pread failed");
    }
    *result = Slice(scratch, r);
    return Status::OK();
  }

 private:
  std::string filename_;
  int fd_;
};
#endif

// 64 KiB buffer for sequential writes
constexpr size_t kWritableFileBufferSize = 65536;

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

    // Write large chunks directly
    if (write_size < kWritableFileBufferSize) {
      std::memcpy(buf_, write_data, write_size);
      pos_ = write_size;
      return Status::OK();
    }

    return WriteUnbuffered(write_data, write_size);
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
    if (fd < 0) {
      return Status::IOError(filename_, "_fileno failed");
    }
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    if (h == INVALID_HANDLE_VALUE) {
      return Status::IOError(filename_, "_get_osfhandle failed");
    }
    if (!FlushFileBuffers(h)) {
      return Status::IOError(filename_, "FlushFileBuffers failed");
    }
#else
    int fd = fileno(file_);
    if (fd < 0) {
      return Status::IOError(filename_, "fileno failed");
    }
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) < 0) {
      return Status::IOError(filename_, "fcntl(F_FULLFSYNC) failed");
    }
#else
    if (fdatasync(fd) < 0) {
      return Status::IOError(filename_, "fdatasync failed");
    }
#endif
#endif
    return Status::OK();
  }

  Status Close() override {
    Status s = FlushBuffer();
    if (file_) {
      if (std::fclose(file_) != 0 && s.ok()) {
        s = Status::IOError(filename_, "fclose failed");
      }
      file_ = nullptr;
    }
    return s;
  }

 private:
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

  Status NewRandomAccessFile(const std::string& fname,
                             std::unique_ptr<RandomAccessFile>* result) override {
#if defined(_WIN32)
    HANDLE handle = ::CreateFileA(
        fname.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      return Status::NotFound(fname, "cannot open file for random read");
    }
    result->reset(new WindowsRandomAccessFile(fname, handle));
    return Status::OK();
#else
    int fd = ::open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      return Status::NotFound(fname, "cannot open file for random read");
    }
    result->reset(new PosixRandomAccessFile(fname, fd));
    return Status::OK();
#endif
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
#if defined(_WIN32)
    for (int retry = 0; retry < 100; ++retry) {
      if (::MoveFileExA(src.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return Status::OK();
      }
      DWORD err = ::GetLastError();
      if (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) {
        return Status::IOError(src, "MoveFileEx error " + std::to_string(err));
      }
      ::Sleep(5);
    }
    return Status::IOError(src, "MoveFileEx error: access denied or sharing violation after retries");
#else
    std::error_code ec;
    fs::rename(src, target, ec);
    if (ec) {
      return Status::IOError(src, ec.message());
    }
    return Status::OK();
#endif
  }
};

}  // namespace

Env* Env::Default() {
  static PosixEnv default_env;
  return &default_env;
}

}  // namespace purpledb
