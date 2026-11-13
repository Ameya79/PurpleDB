#pragma once

#include <memory>
#include <vector>
#include "core/storage/block.h"
#include "purpledb/iterator.h"

namespace purpledb {

/**
 * @brief Returns an iterator that provides the sorted merge of multiple child iterators.
 *
 * CONCEPT FOR LEARNERS:
 * In an LSM tree, data is distributed across multiple sources:
 * - Active MemTable (in memory)
 * - Immutable MemTable (flushing to disk)
 * - Level 0 SSTables (overlapping key ranges)
 * - Level 1 to 6 SSTables (non-overlapping key ranges per level)
 * MergingIterator performs a K-way merge across all these sources in O(log K) per step,
 * producing a single sorted key-value stream.
 */
std::unique_ptr<Iterator> NewMergingIterator(
    const Block::Comparator* cmp,
    std::vector<std::unique_ptr<Iterator>> children);

}  // namespace purpledb

