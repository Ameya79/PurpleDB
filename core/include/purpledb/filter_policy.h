#pragma once

#include <memory>
#include <string>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief FilterPolicy provides probabilistic set membership testing to avoid
 * unnecessary disk I/O when looking up keys that do not exist in an SSTable.
 *
 * CONCEPT FOR LEARNERS:
 * A Bloom filter is a space-efficient probabilistic data structure. It tells you:
 * - "Definitely NO": the key is definitely not in the SSTable (0% false negatives).
 *   In this case, we can completely skip reading the data blocks from disk!
 * - "PROBABLY YES": the key might be in the SSTable (e.g. ~1% false positive rate
 *   with 10 bits per key). We then read the block to find the real key.
 */
class FilterPolicy {
 public:
  virtual ~FilterPolicy() = default;

  // Returns the name of this policy (e.g., "purpledb.BuiltinBloomFilter2").
  // If the filter encoding changes in an incompatible way, this name is changed.
  virtual const char* Name() const = 0;

  // Appends a filter that summarizes the given keys to *dst.
  // keys[0..n-1] contains a list of keys (packed consecutively).
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const = 0;

  // "filter" contains the data appended by a preceding call to CreateFilter().
  // Returns true if the key may match, false if the key definitely does not match.
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const = 0;
};

// Returns a new filter policy that uses a bloom filter with approximately
// the specified number of bits per key. A good value for bits_per_key
// is 10, which yields a filter with ~ 1% false positive rate.
std::unique_ptr<FilterPolicy> NewBloomFilterPolicy(int bits_per_key);

}  // namespace purpledb

