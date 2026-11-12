#pragma once

#include <cassert>
#include <string>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief Status encapsulates the result of an operation in PurpleDB.
 *
 * CONCEPT FOR LEARNERS:
 * In systems programming and database engines, returning error codes via C++
 * exceptions is avoided on performance-critical paths because exception handling
 * introduces hidden control flow, code bloat, and stack unwinding overhead.
 * Instead, operations return an explicit Status object by value.
 *
 * Memory & Cache Optimization:
 * For successful operations (kOk), state_ is nullptr.
 * This guarantees:
 * 1. sizeof(Status) == sizeof(void*) (8 bytes on 64-bit systems).
 * 2. Zero heap allocations on the success path.
 * 3. Returning Status::OK() compiles down to returning a null pointer in a register.
 *
 * On error, state_ points to a heap-allocated buffer:
 *   state_[0..3] == length of message (uint32_t)
 *   state_[4]    == Status::Code
 *   state_[5..]  == formatted message
 */
class Status {
 public:
  enum Code {
    kOk = 0,
    kNotFound = 1,
    kCorruption = 2,
    kNotSupported = 3,
    kInvalidArgument = 4,
    kIOError = 5,
    kBusy = 6,
    kTimedOut = 7
  };

  // Create an OK status.
  Status() noexcept : state_(nullptr) {}
  ~Status() { delete[] state_; }

  // Copy semantics: deep copy of the message buffer if present.
  Status(const Status& rhs);
  Status& operator=(const Status& rhs);

  // Move semantics: transfer ownership of the pointer with zero allocations.
  Status(Status&& rhs) noexcept : state_(rhs.state_) { rhs.state_ = nullptr; }
  Status& operator=(Status&& rhs) noexcept {
    if (this != &rhs) {
      delete[] state_;
      state_ = rhs.state_;
      rhs.state_ = nullptr;
    }
    return *this;
  }

  // Returns true iff the status indicates success.
  bool ok() const { return state_ == nullptr; }

  // Status predicate checks
  bool IsNotFound() const { return code() == kNotFound; }
  bool IsCorruption() const { return code() == kCorruption; }
  bool IsNotSupported() const { return code() == kNotSupported; }
  bool IsInvalidArgument() const { return code() == kInvalidArgument; }
  bool IsIOError() const { return code() == kIOError; }
  bool IsBusy() const { return code() == kBusy; }
  bool IsTimedOut() const { return code() == kTimedOut; }

  // Return status code
  Code code() const {
    return (state_ == nullptr) ? kOk : static_cast<Code>(state_[4]);
  }

  // Return a human-readable string representation of this status.
  std::string ToString() const;

  // Static constructors
  static Status OK() { return Status(); }
  static Status NotFound(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotFound, msg, msg2);
  }
  static Status Corruption(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kCorruption, msg, msg2);
  }
  static Status NotSupported(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kInvalidArgument, msg, msg2);
  }
  static Status IOError(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kIOError, msg, msg2);
  }
  static Status Busy(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kBusy, msg, msg2);
  }
  static Status TimedOut(const Slice& msg, const Slice& msg2 = Slice()) {
    return Status(kTimedOut, msg, msg2);
  }

 private:
  const char* state_;

  Status(Code code, const Slice& msg, const Slice& msg2);
  static const char* CopyState(const char* state);
};

inline Status::Status(const Status& rhs) {
  state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
}

inline Status& Status::operator=(const Status& rhs) {
  if (state_ != rhs.state_) {
    delete[] state_;
    state_ = (rhs.state_ == nullptr) ? nullptr : CopyState(rhs.state_);
  }
  return *this;
}

}  // namespace purpledb
