#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief DeleteBitmap tracks tombstoned vector IDs for O(1) filtered search.
 *
 * CONCEPT FOR LEARNERS:
 * In multimodal search engines, vectors are large and stored in append-only
 * segments. Updating or removing a vector physically from disk on every Delete()
 * call would cause extreme random disk I/O and trigger expensive index rebuilds.
 * 
 * Instead, we use a tombstone bitmap:
 * - Each vector is assigned an integer `VectorId` (0, 1, 2, ...).
 * - Bit `i` in the bitmap corresponds to `VectorId == i`.
 * - When a user deletes a video moment, we set bit `i = 1`.
 * - During search, before scoring or returning candidate vectors, we test bit `i`.
 *   If it is 1, the candidate is skipped with zero disk access!
 * - When the fraction of deleted vectors exceeds a threshold (e.g. 20%),
 *   a background rebuild can vacuum the segment.
 */
class DeleteBitmap {
 public:
  static constexpr uint32_t kMagic = 0xdb424d50;  // 0xdb 'B' 'M' 'P'
  static constexpr uint32_t kVersion = 1;

  explicit DeleteBitmap(uint64_t initial_capacity = 0);
  ~DeleteBitmap() = default;

  // Mark vector_id as deleted (tombstone)
  void MarkDeleted(uint64_t vector_id);

  // Clear deletion tombstone (restore vector_id)
  void ClearDeleted(uint64_t vector_id);

  // Check if vector_id has been deleted (O(1) bit test)
  bool IsDeleted(uint64_t vector_id) const;

  // Return total number of deleted vectors
  uint64_t DeletedCount() const;

  // Return total number of active tracked vectors
  uint64_t TotalCount() const;

  // Set total count of vectors tracked
  void SetTotalCount(uint64_t total);

  // Ratio of deleted vectors to total vectors (0.0 to 1.0)
  double DeletedFraction() const;

  // Clear all tombstones and reset counts
  void Reset();

  // Binary serialization matching FORMATS.md Section 9.2
  std::string Serialize() const;

  // Reconstruct bitmap from serialized bytes
  Status Deserialize(const Slice& src);

 private:
  void EnsureCapacity(uint64_t vector_id);

  mutable std::mutex mutex_;
  std::vector<uint64_t> words_;
  uint64_t total_count_{0};
  uint64_t deleted_count_{0};
};

}  // namespace purpledb
