#include "core/vector/vector_segment.h"

#include <cmath>
#include <random>
#include <vector>
#include "core/storage/env.h"
#include "util/test_harness.h"

namespace purpledb {
namespace test {

class VectorSegmentTest {
 public:
  VectorSegmentTest() : env_(Env::Default()) {
    test_dir_ = "./vec_seg_test_dir";
    env_->CreateDir(test_dir_);
  }

  ~VectorSegmentTest() {
    // Cleanup files in test dir
    std::vector<std::string> children;
    env_->GetChildren(test_dir_, &children);
    for (const auto& f : children) {
      if (f != "." && f != "..") {
        env_->DeleteFile(test_dir_ + "/" + f);
      }
    }
    env_->DeleteDir(test_dir_);
  }

  Env* env_;
  std::string test_dir_;
};

TEST(VectorSegmentTest, HeaderEncodeDecode) {
  VectorSegmentHeader hdr;
  hdr.dim = 512;
  hdr.dtype = VectorDataType::kFloat32;
  hdr.metric = MetricType::kCosine;
  hdr.chunk_size = 64;
  std::string model = "clip-vit-base-patch32";
  hdr.model_id_length = static_cast<uint16_t>(model.size());
  std::memcpy(hdr.model_id, model.data(), model.size());

  std::string encoded = hdr.Encode();
  ASSERT_EQ(encoded.size(), VectorSegmentHeader::kHeaderSize);

  VectorSegmentHeader decoded;
  Status s = VectorSegmentHeader::Decode(Slice(encoded), &decoded);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(decoded.dim, 512u);
  ASSERT_TRUE(decoded.dtype == VectorDataType::kFloat32);
  ASSERT_TRUE(decoded.metric == MetricType::kCosine);
  ASSERT_EQ(decoded.chunk_size, 64u);
  ASSERT_EQ(decoded.model_id_length, static_cast<uint16_t>(model.size()));
  ASSERT_EQ(std::string(decoded.model_id, decoded.model_id_length), model);

  // Check corruption detection
  encoded[10] ^= 0x33;
  Status s_corrupt = VectorSegmentHeader::Decode(Slice(encoded), &decoded);
  ASSERT_FALSE(s_corrupt.ok());
}

TEST(VectorSegmentTest, Float32ChunkedRoundtrip) {
  VectorSegmentTest fixture;
  std::string filepath = fixture.test_dir_ + "/test_f32.vec";
  const uint32_t dim = 16;
  const uint32_t chunk_size = 4;
  const size_t total_vectors = 11;  // 2 full chunks (8 vectors) + 1 partial chunk (3 vectors)

  std::unique_ptr<VectorSegmentWriter> writer;
  Status s = VectorSegmentWriter::Create(fixture.env_, filepath, dim,
                                         VectorDataType::kFloat32,
                                         MetricType::kCosine, chunk_size,
                                         "test-model", &writer);
  ASSERT_STATUS_OK(s);

  std::vector<std::vector<float>> expected(total_vectors, std::vector<float>(dim));
  for (size_t i = 0; i < total_vectors; ++i) {
    for (size_t d = 0; d < dim; ++d) {
      expected[i][d] = static_cast<float>(i * 100 + d);
    }
    s = writer->Append(expected[i].data(), dim);
    ASSERT_STATUS_OK(s);
  }

  s = writer->Close();
  ASSERT_STATUS_OK(s);

  // Reopen and verify with reader
  std::unique_ptr<VectorSegmentReader> reader;
  s = VectorSegmentReader::Open(fixture.env_, filepath, &reader);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(reader->VectorCount(), total_vectors);
  ASSERT_EQ(reader->dim(), dim);
  ASSERT_EQ(reader->model_id(), "test-model");

  for (size_t i = 0; i < total_vectors; ++i) {
    std::vector<float> vec;
    s = reader->ReadVector(i, &vec);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(vec.size(), dim);
    for (size_t d = 0; d < dim; ++d) {
      ASSERT_EQ(vec[d], expected[i][d]);
    }
  }
}

TEST(VectorSegmentTest, Int8QuantizedRoundtrip) {
  VectorSegmentTest fixture;
  std::string filepath = fixture.test_dir_ + "/test_int8.vec";
  const uint32_t dim = 32;
  const uint32_t chunk_size = 4;
  const size_t total_vectors = 9;

  std::unique_ptr<VectorSegmentWriter> writer;
  Status s = VectorSegmentWriter::Create(fixture.env_, filepath, dim,
                                         VectorDataType::kInt8,
                                         MetricType::kCosine, chunk_size,
                                         "quant-model", &writer);
  ASSERT_STATUS_OK(s);

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  std::vector<std::vector<float>> originals(total_vectors, std::vector<float>(dim));
  for (size_t i = 0; i < total_vectors; ++i) {
    for (size_t d = 0; d < dim; ++d) {
      originals[i][d] = dist(rng);
    }
    NormalizeVector(originals[i].data(), dim);
    s = writer->Append(originals[i].data(), dim);
    ASSERT_STATUS_OK(s);
  }

  s = writer->Close();
  ASSERT_STATUS_OK(s);

  // Read back and verify dequantized values match within quantization tolerance
  std::unique_ptr<VectorSegmentReader> reader;
  s = VectorSegmentReader::Open(fixture.env_, filepath, &reader);
  ASSERT_STATUS_OK(s);
  ASSERT_EQ(reader->VectorCount(), total_vectors);
  ASSERT_TRUE(reader->dtype() == VectorDataType::kInt8);

  for (size_t i = 0; i < total_vectors; ++i) {
    std::vector<float> vec;
    s = reader->ReadVector(i, &vec);
    ASSERT_STATUS_OK(s);
    for (size_t d = 0; d < dim; ++d) {
      ASSERT_NEAR(vec[d], originals[i][d], 0.02f);
    }
  }
}

TEST(VectorSegmentTest, CrashTruncationRecovery) {
  VectorSegmentTest fixture;
  std::string filepath = fixture.test_dir_ + "/test_crash.vec";
  const uint32_t dim = 8;
  const uint32_t chunk_size = 4;

  std::unique_ptr<VectorSegmentWriter> writer;
  Status s = VectorSegmentWriter::Create(fixture.env_, filepath, dim,
                                         VectorDataType::kFloat32,
                                         MetricType::kCosine, chunk_size,
                                         "crash-test", &writer);
  ASSERT_STATUS_OK(s);

  // Write 2 full chunks (8 vectors) + 2 vectors towards chunk 3
  std::vector<float> vec(dim, 1.0f);
  for (size_t i = 0; i < 10; ++i) {
    writer->Append(vec.data(), dim);
  }
  writer->Flush();  // Partial chunk written (2 vectors)

  // Get file size
  uint64_t initial_size = 0;
  fixture.env_->GetFileSize(filepath, &initial_size);
  ASSERT_GT(initial_size, 0u);

  // Reopen: should read all 10
  {
    std::unique_ptr<VectorSegmentReader> reader;
    s = VectorSegmentReader::Open(fixture.env_, filepath, &reader);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(reader->VectorCount(), 10u);
  }

  // Simulate torn write: truncate last 5 bytes (corrupting the partial chunk's CRC)
  std::string content;
  {
    std::unique_ptr<SequentialFile> f;
    fixture.env_->NewSequentialFile(filepath, &f);
    char buf[4096];
    Slice sl;
    f->Read(initial_size, &sl, buf);
    content = std::string(sl.data(), sl.size());
  }

  // Write truncated file (truncated mid-CRC of chunk 3)
  std::string truncated_path = fixture.test_dir_ + "/test_truncated.vec";
  {
    std::unique_ptr<WritableFile> wf;
    fixture.env_->NewWritableFile(truncated_path, &wf);
    wf->Append(Slice(content.data(), initial_size - 6));
    wf->Close();
  }

  // Reader must recover the 8 vectors from the 2 intact chunks and safely discard the torn chunk!
  {
    std::unique_ptr<VectorSegmentReader> reader;
    s = VectorSegmentReader::Open(fixture.env_, truncated_path, &reader);
    ASSERT_STATUS_OK(s);
    ASSERT_EQ(reader->VectorCount(), 8u);
  }
}

}  // namespace test
}  // namespace purpledb

