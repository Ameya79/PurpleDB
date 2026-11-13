#include "core/vector/vector_segment.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {

static constexpr uint32_t kPartialChunkTag = 0xffffffff;

std::string VectorSegmentHeader::Encode() const {
  std::string buf;
  buf.reserve(kHeaderSize);

  PutFixed64(&buf, magic);
  PutFixed32(&buf, version);
  PutFixed32(&buf, dim);
  buf.push_back(static_cast<char>(dtype));
  buf.push_back(static_cast<char>(metric));
  buf.append(reinterpret_cast<const char*>(reserved), sizeof(reserved));
  PutFixed32(&buf, chunk_size);
  PutFixed16(&buf, model_id_length);

  char padded_model[30] = {0};
  size_t copy_len = std::min(static_cast<size_t>(model_id_length), sizeof(padded_model));
  std::memcpy(padded_model, model_id, copy_len);
  buf.append(padded_model, sizeof(padded_model));

  // Compute CRC32C over the first 60 bytes
  uint32_t crc = crc32c::Mask(crc32c::Value(buf.data(), buf.size()));
  PutFixed32(&buf, crc);

  return buf;
}

Status VectorSegmentHeader::Decode(const Slice& src, VectorSegmentHeader* out) {
  if (src.size() < kHeaderSize) {
    return Status::Corruption("VectorSegmentHeader: slice too short");
  }

  // Verify CRC32C
  uint32_t expected_crc = DecodeFixed32(src.data() + 60);
  uint32_t actual_crc = crc32c::Mask(crc32c::Value(src.data(), 60));
  if (expected_crc != actual_crc) {
    return Status::Corruption("VectorSegmentHeader: CRC32C checksum mismatch");
  }

  const char* p = src.data();
  out->magic = DecodeFixed64(p); p += 8;
  if (out->magic != kMagic) {
    return Status::Corruption("VectorSegmentHeader: invalid magic number");
  }

  out->version = DecodeFixed32(p); p += 4;
  if (out->version != kVersion) {
    return Status::Corruption("VectorSegmentHeader: unsupported format version");
  }

  out->dim = DecodeFixed32(p); p += 4;
  out->dtype = static_cast<VectorDataType>(*reinterpret_cast<const uint8_t*>(p)); p += 1;
  out->metric = static_cast<MetricType>(*reinterpret_cast<const uint8_t*>(p)); p += 1;
  std::memcpy(out->reserved, p, 6); p += 6;
  out->chunk_size = DecodeFixed32(p); p += 4;
  out->model_id_length = DecodeFixed16(p); p += 2;

  std::memcpy(out->model_id, p, 30); p += 30;
  out->crc32c = expected_crc;

  return Status::OK();
}

// ---------------------------------------------------------------------------
// VectorSegmentWriter Implementation
// ---------------------------------------------------------------------------

VectorSegmentWriter::VectorSegmentWriter(Env* env,
                                         std::unique_ptr<WritableFile> file,
                                         const VectorSegmentHeader& header)
    : env_(env), file_(std::move(file)), header_(header) {
  chunk_buffer_.reserve(header_.chunk_size * VectorSizeBytes());
}

VectorSegmentWriter::~VectorSegmentWriter() {
  if (file_) {
    Close();
  }
}

size_t VectorSegmentWriter::VectorSizeBytes() const {
  if (header_.dtype == VectorDataType::kFloat32) {
    return header_.dim * sizeof(float);
  } else {
    // Int8: 4 bytes float scale + dim * 1 byte int8_t
    return sizeof(float) + header_.dim;
  }
}

Status VectorSegmentWriter::Create(Env* env, const std::string& filename,
                                   uint32_t dim, VectorDataType dtype,
                                   MetricType metric, uint32_t chunk_size,
                                   const std::string& model_id,
                                   std::unique_ptr<VectorSegmentWriter>* out) {
  std::unique_ptr<WritableFile> file;
  Status s = env->NewWritableFile(filename, &file);
  if (!s.ok()) {
    return s;
  }

  VectorSegmentHeader hdr;
  hdr.dim = dim;
  hdr.dtype = dtype;
  hdr.metric = metric;
  hdr.chunk_size = chunk_size;
  hdr.model_id_length = static_cast<uint16_t>(std::min(model_id.size(), size_t{30}));
  std::memcpy(hdr.model_id, model_id.data(), hdr.model_id_length);

  std::string encoded_hdr = hdr.Encode();
  s = file->Append(encoded_hdr);
  if (!s.ok()) {
    return s;
  }

  auto writer = std::unique_ptr<VectorSegmentWriter>(
      new VectorSegmentWriter(env, std::move(file), hdr));
  writer->file_size_ = encoded_hdr.size();
  *out = std::move(writer);
  return Status::OK();
}

Status VectorSegmentWriter::Append(const float* vector, size_t dim) {
  if (dim != header_.dim) {
    return Status::InvalidArgument("Vector dimension mismatch");
  }

  if (header_.dtype == VectorDataType::kFloat32) {
    const char* bytes = reinterpret_cast<const char*>(vector);
    chunk_buffer_.insert(chunk_buffer_.end(), bytes, bytes + dim * sizeof(float));
  } else {
    // Quantize to int8
    std::vector<int8_t> quantized(dim);
    float scale = 0.0f;
    QuantizeVector(vector, dim, quantized.data(), &scale);

    const char* scale_bytes = reinterpret_cast<const char*>(&scale);
    chunk_buffer_.insert(chunk_buffer_.end(), scale_bytes, scale_bytes + sizeof(float));
    const char* q_bytes = reinterpret_cast<const char*>(quantized.data());
    chunk_buffer_.insert(chunk_buffer_.end(), q_bytes, q_bytes + dim);
  }

  vectors_in_chunk_++;
  vector_count_++;

  if (vectors_in_chunk_ >= header_.chunk_size) {
    return WriteFullChunk();
  }
  return Status::OK();
}

Status VectorSegmentWriter::WriteFullChunk() {
  if (vectors_in_chunk_ == 0) return Status::OK();

  uint32_t crc = crc32c::Mask(crc32c::Value(chunk_buffer_.data(), chunk_buffer_.size()));
  char crc_bytes[4];
  EncodeFixed32(crc_bytes, crc);

  Status s = file_->Append(Slice(chunk_buffer_.data(), chunk_buffer_.size()));
  if (!s.ok()) return s;
  s = file_->Append(Slice(crc_bytes, 4));
  if (!s.ok()) return s;

  file_size_ += chunk_buffer_.size() + 4;
  chunk_buffer_.clear();
  vectors_in_chunk_ = 0;
  return Status::OK();
}

Status VectorSegmentWriter::WritePartialChunk() {
  if (vectors_in_chunk_ == 0) return Status::OK();

  // Partial chunk format:
  // [kPartialChunkTag (4B)] [Count (4B)] [Payload] [CRC (4B)]
  std::string header_buf;
  PutFixed32(&header_buf, kPartialChunkTag);
  PutFixed32(&header_buf, vectors_in_chunk_);

  uint32_t crc = crc32c::Value(header_buf.data(), header_buf.size());
  crc = crc32c::Extend(crc, chunk_buffer_.data(), chunk_buffer_.size());
  uint32_t masked_crc = crc32c::Mask(crc);

  char crc_bytes[4];
  EncodeFixed32(crc_bytes, masked_crc);

  Status s = file_->Append(Slice(header_buf.data(), header_buf.size()));
  if (!s.ok()) return s;
  s = file_->Append(Slice(chunk_buffer_.data(), chunk_buffer_.size()));
  if (!s.ok()) return s;
  s = file_->Append(Slice(crc_bytes, 4));
  if (!s.ok()) return s;

  file_size_ += header_buf.size() + chunk_buffer_.size() + 4;
  chunk_buffer_.clear();
  vectors_in_chunk_ = 0;
  return Status::OK();
}

Status VectorSegmentWriter::Flush() {
  if (vectors_in_chunk_ > 0) {
    Status s = WritePartialChunk();
    if (!s.ok()) return s;
  }
  return file_->Flush();
}

Status VectorSegmentWriter::Sync() {
  Status s = Flush();
  if (!s.ok()) return s;
  return file_->Sync();
}

Status VectorSegmentWriter::Close() {
  Status s = Flush();
  if (file_) {
    file_->Close();
    file_.reset();
  }
  return s;
}

// ---------------------------------------------------------------------------
// VectorSegmentReader Implementation
// ---------------------------------------------------------------------------

VectorSegmentReader::VectorSegmentReader(Env* env,
                                         std::unique_ptr<SequentialFile> seq_file,
                                         const VectorSegmentHeader& header)
    : env_(env), header_(header) {
  (void)seq_file;
}

Status VectorSegmentReader::Open(Env* env, const std::string& filename,
                                 std::unique_ptr<VectorSegmentReader>* out) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(filename, &file);
  if (!s.ok()) return s;

  // Read 64-byte header
  char header_buf[VectorSegmentHeader::kHeaderSize];
  Slice header_slice;
  s = file->Read(VectorSegmentHeader::kHeaderSize, &header_slice, header_buf);
  if (!s.ok() || header_slice.size() < VectorSegmentHeader::kHeaderSize) {
    return Status::Corruption("VectorSegment: truncated header or unreadable file");
  }

  VectorSegmentHeader hdr;
  s = VectorSegmentHeader::Decode(header_slice, &hdr);
  if (!s.ok()) return s;

  auto reader = std::unique_ptr<VectorSegmentReader>(
      new VectorSegmentReader(env, std::move(file), hdr));
  reader->filename_ = filename;

  // Now reopen and sequentially parse all valid chunks into memory cache
  std::unique_ptr<SequentialFile> parse_file;
  s = env->NewSequentialFile(filename, &parse_file);
  if (!s.ok()) return s;

  // Skip header
  Slice skip_slice;
  parse_file->Read(VectorSegmentHeader::kHeaderSize, &skip_slice, header_buf);

  size_t vec_size = (hdr.dtype == VectorDataType::kFloat32)
                        ? (hdr.dim * sizeof(float))
                        : (sizeof(float) + hdr.dim);

  size_t full_chunk_payload_size = hdr.chunk_size * vec_size;
  std::vector<char> chunk_raw(full_chunk_payload_size + 4);

  while (true) {
    // Read the first 4 bytes of next chunk (either tag or first 4 bytes of vector 0)
    char tag_buf[4];
    Slice tag_slice;
    s = parse_file->Read(4, &tag_slice, tag_buf);
    if (!s.ok() || tag_slice.size() == 0) {
      // Reached clean EOF
      break;
    }
    if (tag_slice.size() < 4) {
      // Torn tail: incomplete chunk tag
      break;
    }

    uint32_t tag = DecodeFixed32(tag_buf);
    if (tag == kPartialChunkTag) {
      // Partial chunk: next 4 bytes is count
      char count_buf[4];
      Slice count_slice;
      s = parse_file->Read(4, &count_slice, count_buf);
      if (!s.ok() || count_slice.size() < 4) break;

      uint32_t count = DecodeFixed32(count_buf);
      size_t payload_size = count * vec_size;
      std::vector<char> partial_payload(payload_size);
      Slice payload_slice;
      s = parse_file->Read(payload_size, &payload_slice, partial_payload.data());
      if (!s.ok() || payload_slice.size() < payload_size) break;

      char crc_buf[4];
      Slice crc_slice;
      s = parse_file->Read(4, &crc_slice, crc_buf);
      if (!s.ok() || crc_slice.size() < 4) break;

      uint32_t expected_crc = DecodeFixed32(crc_buf);
      std::string header_data;
      PutFixed32(&header_data, kPartialChunkTag);
      PutFixed32(&header_data, count);
      uint32_t crc = crc32c::Value(header_data.data(), header_data.size());
      crc = crc32c::Extend(crc, payload_slice.data(), payload_slice.size());
      if (expected_crc != crc32c::Mask(crc)) {
        // Corrupted tail chunk, halt recovery at last intact chunk
        break;
      }

      // Unpack vectors
      const char* p = payload_slice.data();
      for (uint32_t i = 0; i < count; ++i) {
        if (hdr.dtype == VectorDataType::kFloat32) {
          const float* fvec = reinterpret_cast<const float*>(p);
          reader->cached_vectors_.insert(reader->cached_vectors_.end(), fvec, fvec + hdr.dim);
          p += hdr.dim * sizeof(float);
        } else {
          float scale = *reinterpret_cast<const float*>(p);
          p += sizeof(float);
          const int8_t* qvec = reinterpret_cast<const int8_t*>(p);
          size_t cur_size = reader->cached_vectors_.size();
          reader->cached_vectors_.resize(cur_size + hdr.dim);
          DequantizeVector(qvec, scale, hdr.dim, reader->cached_vectors_.data() + cur_size);
          p += hdr.dim;
        }
        reader->vector_count_++;
      }
    } else {
      // Full chunk: tag_buf is the first 4 bytes of vector 0 payload
      std::memcpy(chunk_raw.data(), tag_buf, 4);
      size_t remaining_payload = full_chunk_payload_size - 4;

      Slice rem_slice;
      s = parse_file->Read(remaining_payload, &rem_slice, chunk_raw.data() + 4);
      if (!s.ok() || rem_slice.size() < remaining_payload) break;

      char crc_buf[4];
      Slice crc_slice;
      s = parse_file->Read(4, &crc_slice, crc_buf);
      if (!s.ok() || crc_slice.size() < 4) break;

      uint32_t expected_crc = DecodeFixed32(crc_buf);
      uint32_t actual_crc = crc32c::Mask(crc32c::Value(chunk_raw.data(), full_chunk_payload_size));
      if (expected_crc != actual_crc) {
        // Corrupted chunk, halt recovery
        break;
      }

      // Unpack all vectors in full chunk
      const char* p = chunk_raw.data();
      for (uint32_t i = 0; i < hdr.chunk_size; ++i) {
        if (hdr.dtype == VectorDataType::kFloat32) {
          const float* fvec = reinterpret_cast<const float*>(p);
          reader->cached_vectors_.insert(reader->cached_vectors_.end(), fvec, fvec + hdr.dim);
          p += hdr.dim * sizeof(float);
        } else {
          float scale = *reinterpret_cast<const float*>(p);
          p += sizeof(float);
          const int8_t* qvec = reinterpret_cast<const int8_t*>(p);
          size_t cur_size = reader->cached_vectors_.size();
          reader->cached_vectors_.resize(cur_size + hdr.dim);
          DequantizeVector(qvec, scale, hdr.dim, reader->cached_vectors_.data() + cur_size);
          p += hdr.dim;
        }
        reader->vector_count_++;
      }
    }
  }

  *out = std::move(reader);
  return Status::OK();
}

Status VectorSegmentReader::ReadVector(uint64_t index, std::vector<float>* out) const {
  if (index >= vector_count_) {
    return Status::NotFound("Vector index out of range");
  }
  out->resize(header_.dim);
  const float* src = cached_vectors_.data() + index * header_.dim;
  std::memcpy(out->data(), src, header_.dim * sizeof(float));
  return Status::OK();
}

Status VectorSegmentReader::ReadAllVectors(std::vector<float>* flat_vectors) const {
  *flat_vectors = cached_vectors_;
  return Status::OK();
}

}  // namespace purpledb
