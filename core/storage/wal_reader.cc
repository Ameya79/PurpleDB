#include "core/storage/wal_reader.h"

#include <cstdio>
#include "core/storage/crc32c.h"
#include "core/util/coding.h"

namespace purpledb {
namespace wal {

WalReader::WalReader(SequentialFile* file, Reporter* reporter, bool checksum,
                     uint64_t initial_offset)
    : file_(file),
      reporter_(reporter),
      checksum_(checksum),
      buffer_(),
      eof_(false),
      last_record_offset_(0),
      end_of_buffer_offset_(0) {
  if (initial_offset > 0) {
    file_->Skip(initial_offset);
    end_of_buffer_offset_ = initial_offset;
  }
}

void WalReader::ReportCorruption(size_t bytes, const char* reason) {
  ReportDrop(bytes, Status::Corruption(reason));
}

void WalReader::ReportDrop(size_t bytes, const Status& status) {
  if (reporter_ != nullptr && bytes > 0) {
    reporter_->Corruption(bytes, status);
  }
}

bool WalReader::ReadRecord(Slice* record, std::string* scratch) {
  scratch->clear();
  record->clear();
  bool in_fragmented_record = false;

  uint64_t prospective_record_offset = 0;

  Slice fragment;
  while (true) {
    const uint64_t physical_record_offset =
        end_of_buffer_offset_ - buffer_.size();

    const unsigned int record_type = ReadPhysicalRecord(&fragment);

    switch (record_type) {
      case kFullType:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "partial record without end(1)");
        }
        prospective_record_offset = physical_record_offset;
        scratch->clear();
        *record = fragment;
        last_record_offset_ = prospective_record_offset;
        return true;

      case kFirstType:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "partial record without end(2)");
        }
        prospective_record_offset = physical_record_offset;
        scratch->assign(fragment.data(), fragment.size());
        in_fragmented_record = true;
        break;

      case kMiddleType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record(1)");
        } else {
          scratch->append(fragment.data(), fragment.size());
        }
        break;

      case kLastType:
        if (!in_fragmented_record) {
          ReportCorruption(fragment.size(), "missing start of fragmented record(2)");
        } else {
          scratch->append(fragment.data(), fragment.size());
          *record = Slice(*scratch);
          last_record_offset_ = prospective_record_offset;
          return true;
        }
        break;

      case kEof:
        if (in_fragmented_record) {
          // Truncated tail at end of file
          ReportCorruption(scratch->size(), "partial record at end of file");
        }
        return false;

      case kBadRecord:
      case kBadRecordLen:
      case kBadRecordChecksum:
      default:
        if (in_fragmented_record) {
          ReportCorruption(scratch->size(), "error in middle of record");
        }
        return false;
    }
  }
}

unsigned int WalReader::ReadPhysicalRecord(Slice* result) {
  while (true) {
    if (buffer_.size() < kHeaderSize) {
      if (!eof_) {
        // Read next 32 KiB block
        Status s = file_->Read(kBlockSize, &buffer_, backing_store_);
        end_of_buffer_offset_ += buffer_.size();
        if (!s.ok()) {
          buffer_.clear();
          ReportDrop(kBlockSize, s);
          eof_ = true;
          return kEof;
        } else if (buffer_.size() < kBlockSize) {
          eof_ = true;
        }
        continue;
      } else {
        // Normal EOF or trailing zero padding
        buffer_.clear();
        return kEof;
      }
    }

    // Parse 9-byte header: [crc32c u32][length u32][type u8]
    const char* header = buffer_.data();
    const uint32_t expected_crc = DecodeFixed32(header);
    const uint32_t length = DecodeFixed32(header + 4);
    const unsigned int type = static_cast<uint8_t>(header[8]);

    if (kHeaderSize + length > buffer_.size()) {
      size_t drop_size = buffer_.size();
      buffer_.clear();
      ReportCorruption(drop_size, "truncated record at end of block");
      return kBadRecordLen;
    }

    if (type == kZeroType && length == 0) {
      // Skip the rest of this block (zero padding)
      buffer_.clear();
      continue;
    }

    // Verify CRC-32C
    if (checksum_) {
      uint32_t actual_crc = crc32c::Extend(0, header + 8, 1);  // type byte
      actual_crc = crc32c::Extend(actual_crc, header + kHeaderSize, length);
      if (crc32c::Unmask(expected_crc) != actual_crc) {
        size_t drop_size = buffer_.size();
        buffer_.clear();
        ReportCorruption(drop_size, "checksum mismatch");
        return kBadRecordChecksum;
      }
    }

    buffer_.remove_prefix(kHeaderSize + length);
    *result = Slice(header + kHeaderSize, length);
    return type;
  }
}

}  // namespace wal
}  // namespace purpledb
