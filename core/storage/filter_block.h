#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "purpledb/filter_policy.h"
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief FilterBlockBuilder serializes all Bloom filters for an SSTable into
 * a single contiguous filter block.
 *
 * CONCEPT FOR LEARNERS:
 * Instead of storing one gigantic Bloom filter for the entire SSTable (which would
 * cause cache thrashing on reads), or one per data block (which requires an index),
 * the filter block generates one filter per 2 KiB slice of file data (kFilterBaseLg = 11).
 * When reading data at byte offset X, we immediately know the filter index is (X >> 11).
 */
class FilterBlockBuilder {
 public:
  explicit FilterBlockBuilder(const FilterPolicy* policy);

  FilterBlockBuilder(const FilterBlockBuilder&) = delete;
  FilterBlockBuilder& operator=(const FilterBlockBuilder&) = delete;

  void StartBlock(uint64_t block_offset);
  void AddKey(const Slice& key);
  Slice Finish();

 private:
  void GenerateFilter();

  const FilterPolicy* policy_;
  std::string keys_;             // Flattened key contents
  std::vector<size_t> start_;    // Starting index of each key in keys_
  std::string result_;           // Filter data computed so far
  std::vector<Slice> tmp_keys_;  // Temporary key slices passed to policy
  std::vector<uint32_t> filter_offsets_;
};

/**
 * @brief FilterBlockReader parses a filter block and tests key existence for a given
 * data block file offset.
 */
class FilterBlockReader {
 public:
  // contents must remain valid for the lifetime of this reader.
  FilterBlockReader(const FilterPolicy* policy, const Slice& contents);

  FilterBlockReader(const FilterBlockReader&) = delete;
  FilterBlockReader& operator=(const FilterBlockReader&) = delete;

  bool KeyMayMatch(uint64_t block_offset, const Slice& key);

 private:
  const FilterPolicy* policy_;
  const char* data_{nullptr};
  const char* offset_{nullptr};  // Beginning of filter offsets array
  size_t num_{0};                // Number of filter entries
  size_t base_lg_{0};            // Encoding parameter (log2 of filter base)
};

}  // namespace purpledb

