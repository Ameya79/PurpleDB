#include "core/vector/distance.h"

#include <cmath>
#include <random>
#include <vector>
#include "util/test_harness.h"

namespace purpledb {
namespace test {

TEST(DistanceTest, FloatDotProductAndCosine) {
  const size_t dim = 512;
  std::vector<float> a(dim);
  std::vector<float> b(dim);

  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  for (size_t i = 0; i < dim; ++i) {
    a[i] = dist(rng);
    b[i] = dist(rng);
  }

  float dot_simd = DotProduct(a.data(), b.data(), dim);
  float dot_scalar = DotProductScalar(a.data(), b.data(), dim);
  ASSERT_NEAR(dot_simd, dot_scalar, 1e-4f);

  float l2_simd = L2DistanceSquared(a.data(), b.data(), dim);
  float l2_scalar = L2DistanceSquaredScalar(a.data(), b.data(), dim);
  ASSERT_NEAR(l2_simd, l2_scalar, 1e-4f);

  // Cosine of identical vector should be 1.0
  float cos_self = CosineSimilarity(a.data(), a.data(), dim);
  ASSERT_NEAR(cos_self, 1.0f, 1e-5f);

  // Normalize vectors
  NormalizeVector(a.data(), dim);
  NormalizeVector(b.data(), dim);
  ASSERT_NEAR(VectorNorm(a.data(), dim), 1.0f, 1e-5f);
  ASSERT_NEAR(VectorNorm(b.data(), dim), 1.0f, 1e-5f);

  // For unit vectors, Cosine == DotProduct
  float cos_norm = CosineSimilarity(a.data(), b.data(), dim);
  float dot_norm = DotProduct(a.data(), b.data(), dim);
  ASSERT_NEAR(cos_norm, dot_norm, 1e-5f);
}

TEST(DistanceTest, OddDimensionsHandling) {
  // Test dimensions that are not multiples of 8 or 4 (tail loops)
  for (size_t dim : {1, 3, 7, 15, 33, 127, 513}) {
    std::vector<float> a(dim);
    std::vector<float> b(dim);
    for (size_t i = 0; i < dim; ++i) {
      a[i] = static_cast<float>(i + 1);
      b[i] = static_cast<float>(dim - i);
    }

    float dot_simd = DotProduct(a.data(), b.data(), dim);
    float dot_scalar = DotProductScalar(a.data(), b.data(), dim);
    ASSERT_NEAR(dot_simd, dot_scalar, 1e-4f);

    float l2_simd = L2DistanceSquared(a.data(), b.data(), dim);
    float l2_scalar = L2DistanceSquaredScalar(a.data(), b.data(), dim);
    ASSERT_NEAR(l2_simd, l2_scalar, 1e-4f);
  }
}

TEST(DistanceTest, Int8QuantizationAndDotProduct) {
  const size_t dim = 512;
  std::vector<float> a(dim);
  std::vector<float> b(dim);

  std::mt19937 rng(1337);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

  for (size_t i = 0; i < dim; ++i) {
    a[i] = dist(rng);
    b[i] = dist(rng);
  }
  NormalizeVector(a.data(), dim);
  NormalizeVector(b.data(), dim);

  std::vector<int8_t> qa(dim);
  std::vector<int8_t> qb(dim);
  float scale_a = 0.0f;
  float scale_b = 0.0f;

  QuantizeVector(a.data(), dim, qa.data(), &scale_a);
  QuantizeVector(b.data(), dim, qb.data(), &scale_b);

  // Dequantize and check error
  std::vector<float> a_recon(dim);
  DequantizeVector(qa.data(), scale_a, dim, a_recon.data());

  for (size_t i = 0; i < dim; ++i) {
    ASSERT_NEAR(a[i], a_recon[i], 0.02f);
  }

  // Int8 dot product SIMD vs scalar
  int32_t dot_int8_simd = DotProductInt8(qa.data(), qb.data(), dim);
  int32_t dot_int8_scalar = DotProductInt8Scalar(qa.data(), qb.data(), dim);
  ASSERT_EQ(dot_int8_simd, dot_int8_scalar);

  // Cosine similarity comparison between fp32 and int8
  float cos_fp32 = CosineSimilarity(a.data(), b.data(), dim);
  float cos_int8 = CosineSimilarityInt8(qa.data(), scale_a, qb.data(), scale_b, dim);

  // Quantized cosine similarity should be very close to fp32 (within 0.015)
  ASSERT_NEAR(cos_fp32, cos_int8, 0.02f);
}

}  // namespace test
}  // namespace purpledb
