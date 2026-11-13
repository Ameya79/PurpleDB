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

Slice GetLengthPrefixedSliceFromRaw(const char* data) {
  uint32_t len;
  const char* p = GetVarint32Ptr(data, data + 5, &len);
  return Slice(p, len);
}

class MemTableIterator : public Iterator {
 public:
  explicit MemTableIterator(MemTable::Table* table) : iter_(table) {}

  bool Valid() const override { return iter_.Valid(); }

  void SeekToFirst() override { iter_.SeekToFirst(); }

  void SeekToLast() override { iter_.SeekToLast(); }

  void Seek(const Slice& target) override {
    tmp_.clear();
    PutLengthPrefixedSlice(&tmp_, target);
    iter_.Seek(tmp_.data());
  }

  void Next() override { iter_.Next(); }

  void Prev() override { iter_.Prev(); }

  Slice key() const override {
    assert(Valid());
    return GetLengthPrefixedSliceFromRaw(iter_.key());
  }

  Slice value() const override {
    assert(Valid());
    Slice k = key();
    const char* p = k.data() + k.size();
    uint32_t vlen;
    p = GetVarint32Ptr(p, p + 5, &vlen);
    return Slice(p, vlen);
  }

  Status status() const override { return Status::OK(); }

 private:
  MemTable::Table::Iterator iter_;
  std::string tmp_;
};

}  // namespace

int InternalKeyComparator::Compare(const Slice& a, const Slice& b) const {
  assert(a.size() >= 8 && b.size() >= 8);
  Slice user_key_a(a.data(), a.size() - 8);
  Slice user_key_b(b.data(), b.size() - 8);

  int r = user_key_a.compare(user_key_b);
  if (r != 0) {
    return r;
  }

  // Same user key: sort sequence number DESCENDING
  uint64_t tag_a = DecodeFixed64(a.data() + a.size() - 8);
  uint64_t tag_b = DecodeFixed64(b.data() + b.size() - 8);
  if (tag_a > tag_b) return -1;
  if (tag_a < tag_b) return +1;
  return 0;
}

bool InternalKeyComparator::operator()(const std::string& a, const std::string& b) const {
  return Compare(Slice(a), Slice(b)) < 0;
}

int MemTable::KeyComparator::operator()(const char* a, const char* b) const {
  Slice key_a = GetLengthPrefixedSliceFromRaw(a);
  Slice key_b = GetLengthPrefixedSliceFromRaw(b);
  return comparator.Compare(key_a, key_b);
}

MemTable::MemTable()
    : comparator_(),
      arena_(),
      table_(comparator_, &arena_),
      count_(0) {}

void MemTable::Add(SequenceNumber seq, ValueType type, const Slice& key, const Slice& value) {
  uint32_t key_size = static_cast<uint32_t>(key.size());
  uint32_t val_size = static_cast<uint32_t>(value.size());
  uint32_t internal_key_size = key_size + 8;
  size_t encoded_len = VarintLength(internal_key_size) + internal_key_size +
                       VarintLength(val_size) + val_size;

  char* buf = arena_.Allocate(encoded_len);
  char* p = EncodeVarint32(buf, internal_key_size);
  std::memcpy(p, key.data(), key_size);
  p += key_size;
  EncodeFixed64(p, PackSequenceAndType(seq, type));
  p += 8;
  p = EncodeVarint32(p, val_size);
  std::memcpy(p, value.data(), val_size);

  table_.Insert(buf);
  count_++;
}

bool MemTable::Get(const Slice& key, SequenceNumber snapshot_seq, std::string* value, Status* s) {
  // Construct lookup key: user key + max tag for this seq number (kTypeValue)
  std::string lookup;
  uint32_t internal_key_size = static_cast<uint32_t>(key.size() + 8);
  PutVarint32(&lookup, internal_key_size);
  lookup.append(key.data(), key.size());
  PutFixed64(&lookup, PackSequenceAndType(snapshot_seq, kTypeValue));

  Table::Iterator iter(&table_);
  iter.Seek(lookup.data());

  if (iter.Valid()) {
    const char* entry = iter.key();
    Slice internal_key = GetLengthPrefixedSliceFromRaw(entry);
    if (internal_key.size() >= 8) {
      Slice found_user_key(internal_key.data(), internal_key.size() - 8);
      if (found_user_key == key) {
        uint64_t tag = DecodeFixed64(internal_key.data() + internal_key.size() - 8);
        ValueType type = static_cast<ValueType>(tag & 0xff);
        if (type == kTypeValue) {
          const char* p = internal_key.data() + internal_key.size();
          uint32_t vlen;
          p = GetVarint32Ptr(p, p + 5, &vlen);
          value->assign(p, vlen);
          *s = Status::OK();
          return true;
        } else if (type == kTypeDeletion) {
          *s = Status::NotFound(key, "deleted");
          return true;
        }
      }
    }
  }

  return false;
}

std::unique_ptr<Iterator> MemTable::NewIterator() {
  return std::make_unique<MemTableIterator>(&table_);
}

}  // namespace purpledb
