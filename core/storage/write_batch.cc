#include "purpledb/write_batch.h"

#include "core/util/coding.h"

namespace purpledb {

// WriteBatch header has 8 bytes of sequence number followed by 4 bytes of count.
static const size_t kHeader = 12;

WriteBatch::WriteBatch() {
  Clear();
}

void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader, 0);
}

int WriteBatch::Count() const {
  return static_cast<int>(DecodeFixed32(rep_.data() + 8));
}

void WriteBatch::SetCount(int n) {
  EncodeFixed32(&rep_[8], static_cast<uint32_t>(n));
}

SequenceNumber WriteBatch::Sequence() const {
  return SequenceNumber(DecodeFixed64(rep_.data()));
}

void WriteBatch::SetSequence(SequenceNumber seq) {
  EncodeFixed64(&rep_[0], seq);
}

void WriteBatch::Put(const Slice& key, const Slice& value) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

void WriteBatch::Delete(const Slice& key) {
  SetCount(Count() + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too short)");
  }
  input.remove_prefix(kHeader);

  Slice key, value;
  int found = 0;
  while (!input.empty()) {
    found++;
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          Status s = handler->Put(key, value);
          if (!s.ok()) return s;
        } else {
          return Status::Corruption("bad WriteBatch Put entry");
        }
        break;
      case kTypeDeletion:
        if (GetLengthPrefixedSlice(&input, &key)) {
          Status s = handler->Delete(key);
          if (!s.ok()) return s;
        } else {
          return Status::Corruption("bad WriteBatch Delete entry");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  if (found != Count()) {
    return Status::Corruption("WriteBatch count mismatch");
  }
  return Status::OK();
}

}  // namespace purpledb
