#include "purpledb/filter_policy.h"
#include "core/util/hash.h"

#include <algorithm>
#include <vector>

namespace purpledb {

namespace {

static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy : public FilterPolicy {
 public:
  explicit BloomFilterPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce the number of hash functions,
    // which accelerates filter generation and membership checking.
    // k = ln(2) * (m/n) ≈ 0.69 * bits_per_key
    k_ = static_cast<size_t>(bits_per_key * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  const char* Name() const override {
    return "purpledb.BuiltinBloomFilter2";
  }

  void CreateFilter(const Slice* keys, int n, std::string* dst) const override {
    // Compute number of bits needed
    size_t bits = static_cast<size_t>(n * bits_per_key_);

    // Minimum filter size of 64 bits to avoid high false positive rates for tiny blocks
    if (bits < 64) bits = 64;

    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    // Append number of hash functions k as the final trailer byte
    dst->push_back(static_cast<char>(k_));

    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; ++i) {
      // Use double-hashing (Kirsch-Mitzenmacher optimization) to generate k hash values
      uint32_t h = BloomHash(keys[i]);
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      for (size_t j = 0; j < k_; ++j) {
        const uint32_t bitpos = h % bits;
        array[bitpos / 8] |= static_cast<char>(1 << (bitpos % 8));
        h += delta;
      }
    }
  }

  bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const override {
    const size_t len = bloom_filter.size();
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8;

    // Last byte holds the number of hash functions
    const size_t k = static_cast<uint8_t>(array[len - 1]);
    if (k > 30) {
      // Reserved for new encodings; treat as potential match
      return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);
    for (size_t j = 0; j < k; ++j) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) {
        return false;
      }
      h += delta;
    }
    return true;
  }

 private:
  size_t bits_per_key_;
  size_t k_;
};

}  // namespace

std::unique_ptr<FilterPolicy> NewBloomFilterPolicy(int bits_per_key) {
  return std::make_unique<BloomFilterPolicy>(bits_per_key);
}

}  // namespace purpledb

