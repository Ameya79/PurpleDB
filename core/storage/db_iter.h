#pragma once

#include <memory>
#include "purpledb/iterator.h"
#include "purpledb/write_batch.h"

namespace purpledb {

/**
 * @brief Returns a user-facing Iterator that wraps an internal MergingIterator,
 * enforcing snapshot visibility, skipping tombstones, and hiding sequence tags.
 */
std::unique_ptr<Iterator> NewDBIterator(
    std::unique_ptr<Iterator> internal_iter,
    SequenceNumber sequence);

}  // namespace purpledb

