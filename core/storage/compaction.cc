#include "core/storage/compaction.h"

namespace purpledb {

Compaction::Compaction(int level, Version* input_version)
    : level_(level), input_version_(input_version) {
  if (input_version_ != nullptr) {
    input_version_->Ref();
  }
}

Compaction::~Compaction() {
  if (input_version_ != nullptr) {
    input_version_->Unref();
  }
}

bool Compaction::IsBaseLevelForKey(const Slice& user_key) const {
  if (input_version_ == nullptr) return true;

  // Check levels deeper than output level (level_ + 1)
  for (int lvl = level_ + 2; lvl < kNumLevels; ++lvl) {
    const auto& files = input_version_->files(lvl);
    for (const auto& f : files) {
      if (f.smallest.size() >= 8 && f.largest.size() >= 8) {
        Slice f_smallest(f.smallest.data(), f.smallest.size() - 8);
        Slice f_largest(f.largest.data(), f.largest.size() - 8);
        if (user_key.compare(f_smallest) >= 0 && user_key.compare(f_largest) <= 0) {
          // An older version of this key might exist at level lvl!
          return false;
        }
      }
    }
  }
  return true;
}

}  // namespace purpledb

