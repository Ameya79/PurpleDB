#include "purpledb/slice.h"

#include <iostream>

namespace purpledb {

std::ostream& operator<<(std::ostream& os, const Slice& s) {
  os.write(s.data(), static_cast<std::streamsize>(s.size()));
  return os;
}

}  // namespace purpledb
