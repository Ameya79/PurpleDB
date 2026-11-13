#pragma once

#include <cassert>
#include "core/include/purpledb/write_batch.h"
#include "purpledb/db.h"

namespace purpledb {

class SnapshotImpl : public Snapshot {
 public:
  SequenceNumber sequence_number() const { return sequence_number_; }

 private:
  friend class SnapshotList;
  SequenceNumber sequence_number_{0};
  SnapshotImpl* prev_{nullptr};
  SnapshotImpl* next_{nullptr};
};

/**
 * @brief Circular doubly linked list tracking all active point-in-time snapshots.
 *
 * Compaction inspects oldest() to determine the minimum visible sequence number;
 * any overwritten version older than oldest() can be safely garbage collected.
 */
class SnapshotList {
 public:
  SnapshotList() {
    list_.prev_ = &list_;
    list_.next_ = &list_;
  }

  bool empty() const { return list_.next_ == &list_; }

  SnapshotImpl* oldest() const {
    assert(!empty());
    return list_.next_;
  }

  SnapshotImpl* New(SequenceNumber seq) {
    SnapshotImpl* s = new SnapshotImpl();
    s->sequence_number_ = seq;
    s->next_ = &list_;
    s->prev_ = list_.prev_;
    s->prev_->next_ = s;
    s->next_->prev_ = s;
    return s;
  }

  void Delete(const SnapshotImpl* s) {
    SnapshotImpl* snap = const_cast<SnapshotImpl*>(s);
    snap->prev_->next_ = snap->next_;
    snap->next_->prev_ = snap->prev_;
    delete snap;
  }

 private:
  SnapshotImpl list_;
};

}  // namespace purpledb

