#include "core/storage/memtable.h"

#include <cassert>
#include <cstring>
#include "core/util/coding.h"

namespace purpledb {

namespace {

uint64_t PackSequenceAndType(SequenceNumber seq, ValueType type) {
  assert(seq <= (static_cast<uint64_t>(1) << 56) - 1);
  return (seq << 8) | static_cast<uint8_t>(type);
}

}  // namespace

bool InternalKeyComparator::operator()(const std::string& a, const std::string& b) const {
  assert(a.size() >= 8 && b.size() >= 8);
  Slice user_key_a(a.data(), a.size() - 8);
  Slice user_key_b(b.data(), b.size() - 8);

  int r = user_key_a.compare(user_key_b);
  if (r != 0) {
    return r < 0;
  }

  // Same user key: sort sequence number DESCENDING
  uint64_t tag_a = DecodeFixed64(a.data() + a.size() - 8);
  uint64_t tag_b = DecodeFixed64(b.data() + b.size() - 8);
  return tag_a > tag_b;
}

MemTable::MemTable() : memory_usage_(0) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  std::string internal_key;
  internal_key.reserve(key.size() + 8);
  internal_key.append(key.data(), key.size());
  PutFixed64(&internal_key, PackSequenceAndType(seq, type));

  std::string val_str(value.data(), value.size());
  memory_usage_ += internal_key.size() + val_str.size() + sizeof(void*) * 4;

  table_[std::move(internal_key)] = std::move(val_str);
}

bool MemTable::Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value, Status* s) {
  std::string target;
  target.reserve(key.size() + 8);
  target.append(key.data(), key.size());
  // Search with the highest possible tag for this sequence number so lower_bound lands on newest version <= snapshot_seq
  PutFixed64(&target, PackSequenceAndType(snapshot_seq, static_cast<ValueType>(0xff)));

  auto it = table_.lower_bound(target);
  if (it == table_.end()) {
    return false;
  }

  const std::string& found_key = it->first;
  if (found_key.size() < 8) {
    return false;
  }

  Slice user_key(found_key.data(), found_key.size() - 8);
  if (user_key != key) {
    return false;
  }

  uint64_t tag = DecodeFixed64(found_key.data() + found_key.size() - 8);
  ValueType type = static_cast<ValueType>(tag & 0xff);

  if (type == kTypeValue) {
    *value = it->second;
    *s = Status::OK();
    return true;
  } else if (type == kTypeDeletion) {
    *s = Status::NotFound(key, "deleted");
    return true;
  }

  return false;
}

}  // namespace purpledb
