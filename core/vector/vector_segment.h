#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "core/storage/env.h"
#include "core/vector/distance.h"
#include "purpledb/slice.h"
#include "purpledb/status.h"

namespace purpledb {

/**
 * @brief Header specification for PurpleDB .vec segment files.
 */
struct VectorSegmentHeader {
  static constexpr uint64_t kMagic = 0xdb564543544f5231ULL;  // "\xdbVECTOR1"
  static constexpr uint32_t kVersion = 1;
  static constexpr size_t kHeaderSize = 64;

  uint64_t magic{kMagic};
  uint32_t version{kVersion};
  uint32_t dim{512};
  VectorDataType dtype{VectorDataType::kFloat32};
  MetricType metric{MetricType::kCosine};
  uint8_t reserved[6]{0};
  uint32_t chunk_size{64};
  uint16_t model_id_length{0};
  char model_id[30]{0};
  uint32_t crc32c{0};

  std::string Encode() const;
  static Status Decode(const Slice& src, VectorSegmentHeader* out);
};

/**
 * @brief Appends vectors to a durable .vec file in CRC-checksummed chunks.
 */
class VectorSegmentWriter {
 public:
  static Status Create(Env* env, const std::string& filename,
                       uint32_t dim,
                       VectorDataType dtype = VectorDataType::kFloat32,
                       MetricType metric = MetricType::kCosine,
                       uint32_t chunk_size = 64,
                       const std::string& model_id = "",
                       std::unique_ptr<VectorSegmentWriter>* out = nullptr);

  ~VectorSegmentWriter();

  // Append a vector to the active chunk. When chunk is full, flushed to disk with CRC.
  Status Append(const float* vector, size_t dim);

  // Flush partial chunk to disk with CRC
  Status Flush();

  // Durably fsync segment file to disk
  Status Sync();

  // Close the segment file
  Status Close();

  uint64_t VectorCount() const { return vector_count_; }
  uint64_t FileSize() const { return file_size_; }
  const VectorSegmentHeader& header() const { return header_; }

 private:
  VectorSegmentWriter(Env* env, std::unique_ptr<WritableFile> file,
                      const VectorSegmentHeader& header);

  size_t VectorSizeBytes() const;
  Status WriteFullChunk();
  Status WritePartialChunk();

  Env* env_;
  std::unique_ptr<WritableFile> file_;
  VectorSegmentHeader header_;
  uint64_t vector_count_{0};
  uint64_t file_size_{0};

  // Buffer for vectors in active uncommitted chunk
  std::vector<char> chunk_buffer_;
  uint32_t vectors_in_chunk_{0};
};

/**
 * @brief Reads vectors from a .vec segment file with CRC verification.
 */
class VectorSegmentReader {
 public:
  static Status Open(Env* env, const std::string& filename,
                     std::unique_ptr<VectorSegmentReader>* out);

  ~VectorSegmentReader() = default;

  const VectorSegmentHeader& header() const { return header_; }
  uint32_t dim() const { return header_.dim; }
  VectorDataType dtype() const { return header_.dtype; }
  MetricType metric() const { return header_.metric; }
  uint32_t chunk_size() const { return header_.chunk_size; }
  std::string model_id() const {
    return std::string(header_.model_id, header_.model_id_length);
  }
  uint64_t VectorCount() const { return vector_count_; }

  // Read a single vector by 0-based index into float32 (auto-dequantizing if int8)
  Status ReadVector(uint64_t index, std::vector<float>* out) const;

  // Read all vectors into a contiguous flat float32 array [N * dim] for SIMD search
  Status ReadAllVectors(std::vector<float>* flat_vectors) const;

 private:
  VectorSegmentReader(Env* env, std::unique_ptr<SequentialFile> seq_file,
                      const VectorSegmentHeader& header);

  Env* env_;
  std::string filename_;
  VectorSegmentHeader header_;
  uint64_t vector_count_{0};

  // In-memory cached contiguous vector store
  std::vector<float> cached_vectors_;
};

}  // namespace purpledb
