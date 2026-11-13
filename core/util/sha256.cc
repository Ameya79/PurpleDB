#include "core/util/sha256.h"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace purpledb {

namespace {

inline uint32_t RotR(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (~x & z);
}

inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) {
  return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t Sigma0(uint32_t x) {
  return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22);
}

inline uint32_t Sigma1(uint32_t x) {
  return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25);
}

inline uint32_t Gamma0(uint32_t x) {
  return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3);
}

inline uint32_t Gamma1(uint32_t x) {
  return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10);
}

static const uint32_t K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

}  // namespace

SHA256::SHA256() {
  state_[0] = 0x6a09e667;
  state_[1] = 0xbb67ae85;
  state_[2] = 0x3c6ef372;
  state_[3] = 0xa54ff53a;
  state_[4] = 0x510e527f;
  state_[5] = 0x9b05688c;
  state_[6] = 0x1f83d9ab;
  state_[7] = 0x5be0cd19;
  bit_len_ = 0;
  buffer_len_ = 0;
}

void SHA256::Transform(const uint8_t* chunk) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(chunk[i * 4 + 0]) << 24) |
           (static_cast<uint32_t>(chunk[i * 4 + 1]) << 16) |
           (static_cast<uint32_t>(chunk[i * 4 + 2]) << 8)  |
           (static_cast<uint32_t>(chunk[i * 4 + 3]));
  }
  for (int i = 16; i < 64; ++i) {
    w[i] = Gamma1(w[i - 2]) + w[i - 7] + Gamma0(w[i - 15]) + w[i - 16];
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];

  for (int i = 0; i < 64; ++i) {
    uint32_t t1 = h + Sigma1(e) + Ch(e, f, g) + K[i] + w[i];
    uint32_t t2 = Sigma0(a) + Maj(a, b, c);
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void SHA256::Update(const void* data, size_t len) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    buffer_[buffer_len_++] = p[i];
    if (buffer_len_ == 64) {
      Transform(buffer_);
      bit_len_ += 512;
      buffer_len_ = 0;
    }
  }
}

std::string SHA256::Final() {
  bit_len_ += buffer_len_ * 8;
  buffer_[buffer_len_++] = 0x80;

  if (buffer_len_ > 56) {
    while (buffer_len_ < 64) buffer_[buffer_len_++] = 0x00;
    Transform(buffer_);
    buffer_len_ = 0;
  }

  while (buffer_len_ < 56) buffer_[buffer_len_++] = 0x00;

  // Append 64-bit length in big-endian
  for (int i = 7; i >= 0; --i) {
    buffer_[buffer_len_++] = static_cast<uint8_t>((bit_len_ >> (i * 8)) & 0xff);
  }
  Transform(buffer_);

  std::ostringstream ss;
  ss << std::hex << std::setfill('0');
  for (int i = 0; i < 8; ++i) {
    ss << std::setw(8) << state_[i];
  }
  return ss.str();
}

std::string SHA256::Hash(const void* data, size_t len) {
  SHA256 ctx;
  ctx.Update(data, len);
  return ctx.Final();
}

std::string SHA256::HashString(const std::string& str) {
  return Hash(str.data(), str.size());
}

Status SHA256::HashFile(Env* env, const std::string& path, std::string* out_hex) {
  std::unique_ptr<SequentialFile> file;
  Status s = env->NewSequentialFile(path, &file);
  if (!s.ok()) {
    return s;
  }

  SHA256 ctx;
  const size_t kBufSize = 64 * 1024;
  std::vector<char> buf(kBufSize);

  while (true) {
    Slice result;
    s = file->Read(kBufSize, &result, buf.data());
    if (!s.ok()) {
      return s;
    }
    if (result.empty()) {
      break;
    }
    ctx.Update(result.data(), result.size());
  }

  *out_hex = ctx.Final();
  return Status::OK();
}

}  // namespace purpledb
