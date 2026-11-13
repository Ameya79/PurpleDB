#include "core/storage/format.h"

#include "core/util/coding.h"

namespace purpledb {

BlockHandle::BlockHandle()
    : offset_(~static_cast<uint64_t>(0)),
      size_(~static_cast<uint64_t>(0)) {}

void BlockHandle::EncodeTo(std::string* dst) const {
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) && GetVarint64(input, &size_)) {
    return Status::OK();
  }
  return Status::Corruption("bad block handle");
}

Footer::Footer() {}

void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  metaindex_handle_.EncodeTo(dst);
  index_handle_.EncodeTo(dst);
  // Pad with zeroes so the magic number lands exactly at the end of 48 bytes
  dst->resize(original_size + 2 * BlockHandle::kMaxEncodedLength, 0);
  PutFixed64(dst, kTableMagicNumber);
}

Status Footer::DecodeFrom(Slice* input) {
  if (input->size() < kEncodedLength) {
    return Status::Corruption("footer too short");
  }

  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint64_t magic = DecodeFixed64(magic_ptr);
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an SSTable (bad magic number)");
  }

  Status s = metaindex_handle_.DecodeFrom(input);
  if (!s.ok()) return s;
  s = index_handle_.DecodeFrom(input);
  if (!s.ok()) return s;

  // Consume remainder of the 48 bytes
  *input = Slice(magic_ptr + 8, input->data() + input->size() - (magic_ptr + 8));
  return Status::OK();
}

}  // namespace purpledb
