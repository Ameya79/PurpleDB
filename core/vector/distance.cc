#include "core/vector/distance.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace purpledb {

void QuantizeVector(const float* src, size_t dim, int8_t* dst, float* scale) {
  if (dim == 0) {
    if (scale) *scale = 1.0f;
    return;
  }

  // Find maximum absolute value
  float max_val = 0.0f;
  for (size_t i = 0; i < dim; ++i) {
    float abs_v = std::fabs(src[i]);
    if (abs_v > max_val) {
      max_val = abs_v;
    }
  }

  // Calculate scaling factor to map [-max_val, max_val] to [-127, 127]
  float s = (max_val > 1e-8f) ? (max_val / 127.0f) : 1.0f;
  if (scale) {
    *scale = s;
  }

  float inv_s = 1.0f / s;
  for (size_t i = 0; i < dim; ++i) {
    float scaled = std::round(src[i] * inv_s);
    if (scaled > 127.0f) scaled = 127.0f;
    if (scaled < -127.0f) scaled = -127.0f;
    dst[i] = static_cast<int8_t>(scaled);
  }
}

void DequantizeVector(const int8_t* src, float scale, size_t dim, float* dst) {
  for (size_t i = 0; i < dim; ++i) {
    dst[i] = static_cast<float>(src[i]) * scale;
  }
}

float VectorNorm(const float* vec, size_t dim) {
  float sum = DotProduct(vec, vec, dim);
  return std::sqrt(sum);
}

void NormalizeVector(float* vec, size_t dim) {
  float norm = VectorNorm(vec, dim);
  if (norm > 1e-8f) {
    float inv_norm = 1.0f / norm;
    for (size_t i = 0; i < dim; ++i) {
      vec[i] *= inv_norm;
    }
  }
}

// ---------------------------------------------------------------------------
// Scalar Reference Implementations
// ---------------------------------------------------------------------------

float DotProductScalar(const float* a, const float* b, size_t dim) {
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;

  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    sum0 += a[i] * b[i];
    sum1 += a[i + 1] * b[i + 1];
    sum2 += a[i + 2] * b[i + 2];
    sum3 += a[i + 3] * b[i + 3];
  }
  float total = (sum0 + sum1) + (sum2 + sum3);
  for (; i < dim; ++i) {
    total += a[i] * b[i];
  }
  return total;
}

float L2DistanceSquaredScalar(const float* a, const float* b, size_t dim) {
  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;

  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    float d0 = a[i] - b[i];
    float d1 = a[i + 1] - b[i + 1];
    float d2 = a[i + 2] - b[i + 2];
    float d3 = a[i + 3] - b[i + 3];
    sum0 += d0 * d0;
    sum1 += d1 * d1;
    sum2 += d2 * d2;
    sum3 += d3 * d3;
  }
  float total = (sum0 + sum1) + (sum2 + sum3);
  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
  }
  return total;
}

int32_t DotProductInt8Scalar(const int8_t* a, const int8_t* b, size_t dim) {
  int32_t sum0 = 0;
  int32_t sum1 = 0;
  int32_t sum2 = 0;
  int32_t sum3 = 0;

  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    sum0 += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
    sum1 += static_cast<int32_t>(a[i + 1]) * static_cast<int32_t>(b[i + 1]);
    sum2 += static_cast<int32_t>(a[i + 2]) * static_cast<int32_t>(b[i + 2]);
    sum3 += static_cast<int32_t>(a[i + 3]) * static_cast<int32_t>(b[i + 3]);
  }
  int32_t total = (sum0 + sum1) + (sum2 + sum3);
  for (; i < dim; ++i) {
    total += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
  }
  return total;
}

// ---------------------------------------------------------------------------
// SIMD Optimized Implementations with Fallback
// ---------------------------------------------------------------------------

float DotProduct(const float* a, const float* b, size_t dim) {
#if defined(__AVX2__)
  __m256 sum256 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 7 < dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
#if defined(__FMA__)
    sum256 = _mm256_fmadd_ps(va, vb, sum256);
#else
    sum256 = _mm256_add_ps(sum256, _mm256_mul_ps(va, vb));
#endif
  }

  // Horizontal sum across 8 lanes of __m256
  __m128 lo = _mm256_castps256_ps128(sum256);
  __m128 hi = _mm256_extractf128_ps(sum256, 1);
  __m128 sum128 = _mm_add_ps(lo, hi);
  sum128 = _mm_hadd_ps(sum128, sum128);
  sum128 = _mm_hadd_ps(sum128, sum128);
  float total = _mm_cvtss_f32(sum128);

  for (; i < dim; ++i) {
    total += a[i] * b[i];
  }
  return total;
#elif defined(__ARM_NEON)
  float32x4_t sum_vec = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    float32x4_t va = vld1q_f32(a + i);
    float32x4_t vb = vld1q_f32(b + i);
#if defined(__aarch64__)
    sum_vec = vfmaq_f32(sum_vec, va, vb);
#else
    sum_vec = vaddq_f32(sum_vec, vmulq_f32(va, vb));
#endif
  }
#if defined(__aarch64__)
  float total = vaddvq_f32(sum_vec);
#else
  float32x2_t r = vadd_f32(vget_low_f32(sum_vec), vget_high_f32(sum_vec));
  r = vpadd_f32(r, r);
  float total = vget_lane_f32(r, 0);
#endif
  for (; i < dim; ++i) {
    total += a[i] * b[i];
  }
  return total;
#else
  return DotProductScalar(a, b, dim);
#endif
}

float CosineSimilarity(const float* a, const float* b, size_t dim) {
  float dot = DotProduct(a, b, dim);
  float norm_a = VectorNorm(a, dim);
  float norm_b = VectorNorm(b, dim);
  if (norm_a < 1e-8f || norm_b < 1e-8f) {
    return 0.0f;
  }
  return dot / (norm_a * norm_b);
}

float L2DistanceSquared(const float* a, const float* b, size_t dim) {
#if defined(__AVX2__)
  __m256 sum256 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 7 < dim; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 diff = _mm256_sub_ps(va, vb);
#if defined(__FMA__)
    sum256 = _mm256_fmadd_ps(diff, diff, sum256);
#else
    sum256 = _mm256_add_ps(sum256, _mm256_mul_ps(diff, diff));
#endif
  }

  __m128 lo = _mm256_castps256_ps128(sum256);
  __m128 hi = _mm256_extractf128_ps(sum256, 1);
  __m128 sum128 = _mm_add_ps(lo, hi);
  sum128 = _mm_hadd_ps(sum128, sum128);
  sum128 = _mm_hadd_ps(sum128, sum128);
  float total = _mm_cvtss_f32(sum128);

  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
  }
  return total;
#elif defined(__ARM_NEON)
  float32x4_t sum_vec = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 3 < dim; i += 4) {
    float32x4_t va = vld1q_f32(a + i);
    float32x4_t vb = vld1q_f32(b + i);
    float32x4_t diff = vsubq_f32(va, vb);
#if defined(__aarch64__)
    sum_vec = vfmaq_f32(sum_vec, diff, diff);
#else
    sum_vec = vaddq_f32(sum_vec, vmulq_f32(diff, diff));
#endif
  }
#if defined(__aarch64__)
  float total = vaddvq_f32(sum_vec);
#else
  float32x2_t r = vadd_f32(vget_low_f32(sum_vec), vget_high_f32(sum_vec));
  r = vpadd_f32(r, r);
  float total = vget_lane_f32(r, 0);
#endif
  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
  }
  return total;
#else
  return L2DistanceSquaredScalar(a, b, dim);
#endif
}

int32_t DotProductInt8(const int8_t* a, const int8_t* b, size_t dim) {
#if defined(__AVX2__)
  __m256i sum256 = _mm256_setzero_si256();
  size_t i = 0;
  // Process 16 int8s per step by sign-extending to int16, then madd
  for (; i + 15 < dim; i += 16) {
    __m128i va8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a + i));
    __m128i vb8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(b + i));

    __m256i va16 = _mm256_cvtepi8_epi16(va8);
    __m256i vb16 = _mm256_cvtepi8_epi16(vb8);

    __m256i prod32 = _mm256_madd_epi16(va16, vb16);
    sum256 = _mm256_add_epi32(sum256, prod32);
  }

  // Horizontal reduction
  __m128i lo = _mm256_castsi256_si128(sum256);
  __m128i hi = _mm256_extracti128_si256(sum256, 1);
  __m128i sum128 = _mm_add_epi32(lo, hi);
  sum128 = _mm_hadd_epi32(sum128, sum128);
  sum128 = _mm_hadd_epi32(sum128, sum128);
  int32_t total = _mm_cvtsi128_si32(sum128);

  for (; i < dim; ++i) {
    total += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
  }
  return total;
#else
  return DotProductInt8Scalar(a, b, dim);
#endif
}

float CosineSimilarityInt8(const int8_t* a, float scale_a,
                           const int8_t* b, float scale_b, size_t dim) {
  int32_t int_dot = DotProductInt8(a, b, dim);
  return static_cast<float>(int_dot) * (scale_a * scale_b);
}

}  // namespace purpledb
