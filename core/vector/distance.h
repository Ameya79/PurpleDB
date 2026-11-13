#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace purpledb {

/**
 * @brief Vector distance metrics and quantization primitives.
 *
 * CONCEPT FOR LEARNERS:
 * Vector search in multimodal databases relies heavily on computing geometric
 * distances (cosine similarity or Euclidean distance) between high-dimensional
 * vectors (e.g. 512 dimensions for CLIP).
 * 
 * Computing dot products involves thousands of floating point multiplications
 * and additions. To achieve peak throughput:
 * 1. SIMD (Single Instruction Multiple Data): Process 8 floats at a time via AVX2
 *    or 4 floats at a time via ARM NEON.
 * 2. Scalar Quantization (Int8): Compress each 32-bit float into an 8-bit integer,
 *    reducing memory bandwidth by 4x and accelerating computation with integer vector instructions.
 */

// Supported vector distance metrics
enum class MetricType : uint8_t {
  kCosine = 0,  // Normalized dot product: u . v / (|u| * |v|)
  kL2 = 1,      // Euclidean distance squared: sum((u_i - v_i)^2)
};

// Supported vector storage data types
enum class VectorDataType : uint8_t {
  kFloat32 = 0,  // Standard 32-bit single-precision floating point
  kInt8 = 1,     // 8-bit signed integer scalar quantized
};

// Quantize a float32 vector into int8 with symmetric scaling:
// scale = max(|src[i]|) / 127.0f
// dst[i] = clamp(round(src[i] / scale), -127, 127)
void QuantizeVector(const float* src, size_t dim, int8_t* dst, float* scale);

// Dequantize an int8 vector back into float32:
// dst[i] = src[i] * scale
void DequantizeVector(const int8_t* src, float scale, size_t dim, float* dst);

// Normalize a float32 vector in-place such that ||v||_2 = 1.0
void NormalizeVector(float* vec, size_t dim);

// Compute L2 norm (magnitude) of a vector: sqrt(sum(v[i]^2))
float VectorNorm(const float* vec, size_t dim);

// Compute dot product between two float32 vectors: sum(a[i] * b[i])
float DotProduct(const float* a, const float* b, size_t dim);

// Compute cosine similarity between two float32 vectors.
// If vectors are already unit-normalized, this is identical to DotProduct.
float CosineSimilarity(const float* a, const float* b, size_t dim);

// Compute squared Euclidean distance (L2 squared): sum((a[i] - b[i])^2)
float L2DistanceSquared(const float* a, const float* b, size_t dim);

// Compute integer dot product between two int8 quantized vectors: sum(a[i] * b[i])
int32_t DotProductInt8(const int8_t* a, const int8_t* b, size_t dim);

// Compute approximate cosine similarity between two quantized int8 vectors:
// (scale_a * scale_b) * sum(a[i] * b[i])
float CosineSimilarityInt8(const int8_t* a, float scale_a,
                           const int8_t* b, float scale_b, size_t dim);

// Explicit scalar fallbacks (for verification and benchmarking against SIMD)
float DotProductScalar(const float* a, const float* b, size_t dim);
float L2DistanceSquaredScalar(const float* a, const float* b, size_t dim);
int32_t DotProductInt8Scalar(const int8_t* a, const int8_t* b, size_t dim);

}  // namespace purpledb

