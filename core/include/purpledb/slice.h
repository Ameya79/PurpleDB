#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <iosfwd>
#include <string>

namespace purpledb {

/**
 * @brief Slice is a simple structure containing a pointer to an external byte
 * array and its length.
 *
 * CONCEPT FOR LEARNERS:
 * Slice is a "non-owning view" into memory (similar to C++17 std::string_view).
 * Unlike std::string, Slice NEVER allocates memory on the heap and NEVER copies
 * the underlying bytes. Copying a Slice is as cheap as copying two pointer-sized
 * integers (16 bytes on 64-bit systems).
 *
 * LIFETIME SAFETY WARNING:
 * Because Slice does not own its memory, the caller MUST ensure that the
 * underlying buffer (e.g. std::string or buffer on stack/heap) remains valid
 * for the entire duration of the Slice's usage.
 */
class Slice {
 public:
  // Create an empty slice referring to an empty string.
  constexpr Slice() : data_(""), size_(0) {}

  // Create a slice that refers to d[0, n-1].
  constexpr Slice(const char* d, size_t n) : data_(d), size_(n) {}

  // Create a slice that refers to the contents of "s".
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

  // Create a slice that refers to s[0, strlen(s)-1].
  Slice(const char* s) : data_(s), size_(s == nullptr ? 0 : std::strlen(s)) {}

  // Return a pointer to the beginning of the referenced data.
  constexpr const char* data() const { return data_; }

  // Return the length (in bytes) of the referenced data.
  constexpr size_t size() const { return size_; }

  // Return true iff the length of the referenced data is zero.
  constexpr bool empty() const { return size_ == 0; }

  // Return the ith byte in the referenced data.
  char operator[](size_t n) const {
    assert(n < size());
    return data_[n];
  }

  // Change this slice to refer to an empty array.
  void clear() {
    data_ = "";
    size_ = 0;
  }

  // Drop the first "n" bytes from this slice.
  void remove_prefix(size_t n) {
    assert(n <= size());
    data_ += n;
    size_ -= n;
  }

  // Return a string that contains the copy of the referenced data.
  std::string ToString() const { return std::string(data_, size_); }

  // Three-way comparison:
  //   <  0 iff "*this" <  "b",
  //   == 0 iff "*this" == "b",
  //   >  0 iff "*this" >  "b"
  int compare(const Slice& b) const;

  // Return true iff "x" is a prefix of "*this".
  bool StartsWith(const Slice& x) const {
    return ((size_ >= x.size_) && (std::memcmp(data_, x.data_, x.size_) == 0));
  }

 private:
  const char* data_;
  size_t size_;
};

inline bool operator==(const Slice& x, const Slice& y) {
  return ((x.size() == y.size()) &&
          (std::memcmp(x.data(), y.data(), x.size()) == 0));
}

inline bool operator!=(const Slice& x, const Slice& y) { return !(x == y); }

inline bool operator<(const Slice& x, const Slice& y) {
  return x.compare(y) < 0;
}

inline bool operator<=(const Slice& x, const Slice& y) {
  return x.compare(y) <= 0;
}

inline bool operator>(const Slice& x, const Slice& y) {
  return x.compare(y) > 0;
}

inline bool operator>=(const Slice& x, const Slice& y) {
  return x.compare(y) >= 0;
}

std::ostream& operator<<(std::ostream& os, const Slice& s);

inline int Slice::compare(const Slice& b) const {
  const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
  int r = std::memcmp(data_, b.data_, min_len);
  if (r == 0) {
    if (size_ < b.size_) {
      r = -1;
    } else if (size_ > b.size_) {
      r = +1;
    }
  }
  return r;
}

}  // namespace purpledb
