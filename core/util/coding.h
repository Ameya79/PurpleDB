#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include "purpledb/slice.h"

namespace purpledb {

/**
 * @brief Endian-independent fixed-width and variable-width integer encoding.
 *
 * CONCEPT FOR LEARNERS:
 * 1. Fixed-Width Endianness:
 *    Computers store integers in different byte orders: x86 and ARM typically
 *    use Little-Endian (least significant byte first), while network protocols
 *    historically used Big-Endian. If you write raw memory structs to disk, a
 *    file written on one architecture cannot be read on another! PurpleDB
 *    standardizes on Little-Endian for all binary on-disk records.
 *
 * 2. Variable-Byte (Varint / LEB128) Compression:
 *    Most integers in databases (like key lengths, record counts, and sequence
 *    numbers) are small numbers (< 128). Storing a 32-bit integer normally
 *    takes 4 bytes (e.g. 0x00000007).
 *    Varint uses 7 bits per byte to store the value, using the 8th bit (0x80)
 *    as a flag: "1" means another byte follows, "0" means this is the final byte.
 *    Small integers (0 to 127) take only 1 single byte instead of 4 or 8!
 */

// Fixed-width encoders
void EncodeFixed32(char* dst, uint32_t value);
void EncodeFixed64(char* dst, uint64_t value);

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

// Fixed-width decoders
inline uint32_t DecodeFixed32(const char* ptr) {
  const auto* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint32_t>(buffer[0])) |
         (static_cast<uint32_t>(buffer[1]) << 8) |
         (static_cast<uint32_t>(buffer[2]) << 16) |
         (static_cast<uint32_t>(buffer[3]) << 24);
}

inline uint64_t DecodeFixed64(const char* ptr) {
  const auto* const buffer = reinterpret_cast<const uint8_t*>(ptr);
  return (static_cast<uint64_t>(buffer[0])) |
         (static_cast<uint64_t>(buffer[1]) << 8) |
         (static_cast<uint64_t>(buffer[2]) << 16) |
         (static_cast<uint64_t>(buffer[3]) << 24) |
         (static_cast<uint64_t>(buffer[4]) << 32) |
         (static_cast<uint64_t>(buffer[5]) << 40) |
         (static_cast<uint64_t>(buffer[6]) << 48) |
         (static_cast<uint64_t>(buffer[7]) << 56);
}

// Varint encoders
char* EncodeVarint32(char* dst, uint32_t value);
char* EncodeVarint64(char* dst, uint64_t value);

void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

int VarintLength(uint64_t value);

// Bounds-checked Varint decoders:
// Read from [p, limit). Return pointer past the decoded varint on success,
// or nullptr if truncated or malformed.
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);
const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value);

bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);

// Length-prefixed Slice helpers (stores varint32 length + raw bytes)
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

}  // namespace purpledb
