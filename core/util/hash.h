#pragma once

#include <cstddef>
#include <cstdint>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief Fast, uniform hash function for hash tables and Bloom filters.
 *
 * Uses MurmurHash2 principles: fast 4-byte mixing step with good avalanche
 * properties across all bits.
 */
uint32_t Hash(const char* data, size_t n, uint32_t seed);

inline uint32_t Hash(const Slice& s, uint32_t seed) {
  return Hash(s.data(), s.size(), seed);
}

}  // namespace purpledb

