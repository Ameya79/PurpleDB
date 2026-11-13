# PurpleDB Versioned On-Disk Formats

This document serves as the authoritative specification for all on-disk and binary wire formats used in PurpleDB. All formats are versioned from day one (Rule 5).

---

## 1. Fundamental Primitives (Format Version 1)

### 1.1 Byte Order (Endianness)
All multi-byte numeric values written to disk (WAL records, SSTables, manifest files, vector segments, and index snapshots) are strictly **Little-Endian**.
This ensures deterministic binary equivalence across architectures (e.g. x86-64, ARM64, and RISC-V).

### 1.2 Fixed-Width Numeric Encoding
- **Fixed32**: 4 bytes, unsigned 32-bit integer.
  $$\text{Value} = B_0 | (B_1 \ll 8) | (B_2 \ll 16) | (B_3 \ll 24)$$
- **Fixed64**: 8 bytes, unsigned 64-bit integer.
  $$\text{Value} = \sum_{i=0}^7 B_i \ll (8 \times i)$$

### 1.3 Variable-Length Integer Encoding (Varint / LEB128)
Varints compress unsigned integers by using 7 bits per byte for payload data, reserving the Most Significant Bit (MSB, `0x80`) as a continuation flag:
- `MSB == 1`: More bytes follow.
- `MSB == 0`: Final byte of the integer.

#### Byte Length Capacities:
| Type | Value Range | Encoded Bytes |
| :--- | :--- | :--- |
| **Varint32** | $0 \dots 127$ ($2^7 - 1$) | 1 byte |
| | $128 \dots 16,383$ ($2^{14} - 1$) | 2 bytes |
| | $16,384 \dots 2,097,151$ ($2^{21} - 1$) | 3 bytes |
| | $2,097,152 \dots 268,435,455$ ($2^{28} - 1$) | 4 bytes |
| | $268,435,456 \dots 4,294,967,295$ ($2^{32} - 1$) | 5 bytes |
| **Varint64** | $0 \dots 2^{64}-1$ | 1 to 10 bytes |

Bounds Checking: All Varint decoders (`GetVarint32Ptr`, `GetVarint64Ptr`) enforce buffer limit bounds checks and reject truncated or out-of-range sequences with `nullptr`.

### 1.4 Length-Prefixed Byte Slices
Serialized binary sequences (e.g. user keys, moment metadata, and string attributes) use length prefixing:
```
+---------------------------+-----------------------------------+
|  Length (Varint32, 1-5 B) |       Raw Payload Data (N B)      |
+---------------------------+-----------------------------------+
```

### 1.5 Checksums (CRC-32C Castagnoli)
All data blocks and record headers verify data integrity using CRC-32C:
- **Polynomial**: Castagnoli $0x1EDC6F41$ (reflected / reversed bit order: $0x82F63B78$).
- **Storage size**: 4 bytes, stored as a Fixed32 value.
- **Masking Transformation**:
  When a checksum is stored within or adjacent to a header, it is masked to avoid self-referential checksum collisions:
  $$\text{Masked}(C) = ((C \gg 15) \mid (C \ll 17)) + \text{0xa282ead8}$$
  $$\text{Unmask}(M) = ((M - \text{0xa282ead8}) \gg 17) \mid ((M - \text{0xa282ead8}) \ll 15)$$

### 1.6 Error Status Memory & Wire Layout
PurpleDB `Status` instances encode status state in a compact binary buffer:
- Successful operations (`kOk`): `state_ == nullptr` (0 bytes allocated).
- Error operations (`kNotFound`, `kCorruption`, etc.):
```
+--------------------+--------------+------------------------------------+
| Length (4 B uint32)| Code (1 B u8)| Formatted Message String (Length B)|
+--------------------+--------------+------------------------------------+
```
- Codes:
  - `0`: kOk
  - `1`: kNotFound
  - `2`: kCorruption
  - `3`: kNotSupported
  - `4`: kInvalidArgument
  - `5`: kIOError
  - `6`: kBusy
  - `7`: kTimedOut


---

## 2. Write-Ahead Log (WAL) Format (Format Version 1)

### 2.1 Block Architecture
The Write-Ahead Log is divided into contiguous 32 KiB ($32,768$ byte) blocks. Records never cross block boundaries without fragmentation.
```
+-------------------------------------------------------------+
|                          WAL File                           |
+-----------------------------+-------------------------------+
|     Block 0 (32,768 B)      |      Block 1 (32,768 B)       |
+-----------------------------+-------------------------------+
```

### 2.2 Record Header Layout
Every physical record fragment in a WAL block begins with a 9-byte header:
```
+-------------------+---------------------+------------------+-----------------------+
|  crc32c (4 Bytes) |  Length (4 Bytes)   |  Type (1 Byte)   |  Payload (Length B)   |
|  Fixed32, Masked  |  Fixed32 (uint32)   |  RecordType uint8|  Binary slice bytes   |
+-------------------+---------------------+------------------+-----------------------+
```

### 2.3 Record Types
- `kZeroType = 0`: Reserved for block trailer padding. When remaining block space is $< 9$ bytes, the trailer is padded with zero bytes and a new block begins.
- `kFullType = 1`: The entire logical record is contained within this single block.
- `kFirstType = 2`: The first fragment of a record that spans multiple blocks.
- `kMiddleType = 3`: An intermediate fragment of a multi-block record.
- `kLastType = 4`: The final terminating fragment of a multi-block record.

### 2.4 Integrity & Masked Checksums
The checksum stored in bytes $0 \dots 3$ of the header covers the 1-byte `Type` field followed by the `Payload` bytes. It is masked via:
$$\text{Masked}(C) = ((C \gg 15) \mid (C \ll 17)) + \text{0xa282ead8}$$
During recovery, readers verify the checksum of each fragment. If a torn write, bit flip, or truncated record occurs at the end of the log, the reader halts and safely drops the uncommitted tail.

---

## 3. WriteBatch Binary Layout (Format Version 1)

A `WriteBatch` encodes an atomic collection of puts and deletes into a contiguous binary representation:

```
+-----------------------------+-----------------------+-----------------------------+
|  SequenceNumber (8 Bytes)   |    Count (4 Bytes)    |     Entries (Variable)      |
|  Fixed64 (Starting Seq)     |    Fixed32 (uint32)   |     Sequence of mutations   |
+-----------------------------+-----------------------+-----------------------------+
```

### 3.1 Entry Types
- **Put Entry** (`kTypeValue = 0x1`):
  `[0x1][Key Length (Varint32)][Key Bytes][Value Length (Varint32)][Value Bytes]`
- **Delete Entry** (`kTypeDeletion = 0x0`):
  `[0x0][Key Length (Varint32)][Key Bytes]`

---

## 4. MemTable Internal Key Encoding (Format Version 1)

Every user key in the MemTable is wrapped into an `InternalKey`:
```
+------------------------------------+------------------------------------+
|         User Key Bytes             |          Tag (8 Bytes)             |
|         Arbitrary user bytes       |          Fixed64                   |
+------------------------------------+------------------------------------+
```
- **Tag Formulation**:
  $$\text{Tag} = (\text{SequenceNumber} \ll 8) \mid \text{ValueType}$$
- **Sorting Invariant**:
  1. `User Key` ascending (lexicographical bytewise order).
  2. `SequenceNumber` **descending** (higher sequence numbers appear first).
  This allows `lower_bound` lookups to immediately position on the newest version $\le$ the query sequence number.
