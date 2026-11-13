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
  3. `ValueType` descending (`kTypeValue > kTypeDeletion`).
  This allows `lower_bound` lookups to immediately position on the newest version $\le$ the query sequence number.

---

## 5. SSTable File Format (Format Version 1)

Sorted String Tables (SSTables, stored with extension `.sst` or `.ldb`) are immutable, sorted on-disk files storing persistent key-value pairs.

### 5.1 High-Level File Layout
```
+-------------------------------------------------------------+
| Data Block 0                                                |
+-------------------------------------------------------------+
| Data Block 1                                                |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Data Block N - 1                                            |
+-------------------------------------------------------------+
| Index Block                                                 |
+-------------------------------------------------------------+
| Footer (Fixed 48 Bytes)                                     |
+-------------------------------------------------------------+
```

### 5.2 Block Handle
A `BlockHandle` points to a block within an SSTable file:
```
+---------------------------------+----------------------------------+
| Offset in File (Varint64)       | Size in Bytes (Varint64)         |
+---------------------------------+----------------------------------+
```
`kMaxEncodedLength = 20` bytes (two 10-byte Varint64s).

### 5.3 Block Internal Structure & Prefix Compression
Each block (data block or index block) contains a series of key-value entries with prefix delta compression, followed by a restart array:

```
+-------------------------------------------------------------+
| Entry 0: [shared=0][unshared][val_len][key_delta][val]      |
+-------------------------------------------------------------+
| Entry 1: [shared][unshared][val_len][key_delta][val]        |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Entry M - 1: [shared][unshared][val_len][key_delta][val]    |
+-------------------------------------------------------------+
| Restart Array: Fixed32 offset 0                             |
|                Fixed32 offset 1                             |
|                ...                                          |
|                Fixed32 offset K - 1                         |
+-------------------------------------------------------------+
| Restart Count: Fixed32 (K)                                  |
+-------------------------------------------------------------+
| Compression Type (1 Byte, 0 = None)                         |
+-------------------------------------------------------------+
| Checksum: Masked CRC-32C (4 Bytes, Fixed32)                 |
+-------------------------------------------------------------+
```

#### Entry Encoding:
- `shared_bytes`: Varint32. Number of bytes shared with the immediately preceding key. At restart points, `shared_bytes == 0`.
- `unshared_bytes`: Varint32. Number of new key bytes specific to this entry.
- `value_length`: Varint32. Length of the value payload.
- `key_delta`: Byte sequence of length `unshared_bytes`.
- `value`: Byte sequence of length `value_length`.

#### Restart Array:
- By default, every 16 keys (`block_restart_interval = 16`), prefix compression restarts (`shared = 0`), storing the full key.
- The 4-byte offset of each restart entry within the block is appended to the restart array at the end of the block.
- Binary search within a block is performed across restart points, followed by a linear scan of at most 16 keys.

### 5.4 Index Block
- An index block contains one entry per data block in the SSTable.
- The key is a separator string $\ge$ the last key of that data block and $<$ the first key of the subsequent data block (or the exact last key).
- The value is the serialized `BlockHandle` of that data block.

### 5.5 Table Footer (Fixed 48 Bytes)
Every SSTable ends with a fixed 48-byte footer:
```
+-----------------------------------+-----------------------------------+
| Metaindex Handle (Encoded Handle) | Index Handle (Encoded Handle)     |
| Padded to 20 Bytes                | Padded to 20 Bytes                |
+-----------------------------------+-----------------------------------+
| Padding (0 Bytes)                 | Magic Number (8 Bytes, Fixed64)   |
| 0 to 4 Bytes                      | 0xdb477572706c6564ull             |
+-----------------------------------+-----------------------------------+
```
- **Magic Number**: `0xdb477572706c6564ull` (ASCII byte sequence: `0xdb`, `'p'`, `'u'`, `'r'`, `'p'`, `'l'`, `'e'`, `'d'`).
- The fixed 48-byte size allows readers to seek directly to `file_size - 48` to parse the index block location upon opening.

---

## 6. MANIFEST & CURRENT Architecture (Format Version 1)

PurpleDB tracks active SSTables and database version evolution using an append-only `MANIFEST` log coordinated by an atomic `CURRENT` file pointer.

### 6.1 CURRENT File
- **Path**: `<dbname>/CURRENT`
- **Content**: Plain ASCII string containing the active manifest filename followed by a newline:
  ```
  MANIFEST-000001\n
  ```
- **Atomic Pointer Update**: When rotating manifest files, the engine writes `<dbname>/CURRENT.tmp` and renames it to `CURRENT`, providing atomic POSIX / Win32 directory switchover.

### 6.2 MANIFEST File
- **Path**: `<dbname>/MANIFEST-<file_number>`
- **Format**: Standard PurpleDB WAL log format (32 KiB blocks with 9-byte headers and CRC-32C Castagnoli checksums).
- **Content**: A sequence of `VersionEdit` records. On recovery, replaying these records restores the active `VersionSet`.

### 6.3 VersionEdit Binary Encoding
A `VersionEdit` serializes state deltas using tag-value pairs:

| Tag | Name | Type | Description |
| :--- | :--- | :--- | :--- |
| `0x01` | `kComparator` | Varint32 + String | Name of the active user key comparator. |
| `0x02` | `kLogNumber` | Varint64 | Active log file number for mutations. |
| `0x03` | `kPrevLogNumber`| Varint64 | Obsolete / previous log file number. |
| `0x04` | `kNextFileNumber`| Varint64 | Next file number generator sequence. |
| `0x05` | `kLastSequence` | Varint64 | Highest sequence number applied. |
| `0x06` | `kDeletedFile` | Varint32 + Varint64 | Level number + Deleted SSTable file number. |
| `0x07` | `kNewFile` | Varint32 + Varint64 + Varint64 + Slice + Slice | Level number + File number + File size + Smallest InternalKey + Largest InternalKey. |

---

## 7. Bloom Filter & Metaindex Block Format (Format Version 1)

PurpleDB SSTables incorporate Block-Framed Bloom Filters to eliminate disk I/O on negative point lookups (`NotFound`).

### 7.1 Filter Block Wire Layout
A Filter Block aggregates independent Bloom filter arrays for contiguous $2\text{ KiB}$ ($2^{11}\text{ bytes}$, $\text{base\_lg} = 11$) segments of data blocks:
```
+-------------------------------------------------------------+
| Filter 0 (Bytes for Data Offset 0 .. 2047)                  |
+-------------------------------------------------------------+
| Filter 1 (Bytes for Data Offset 2048 .. 4095)               |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Filter N - 1                                                |
+-------------------------------------------------------------+
| Filter 0 Byte Offset (4 Bytes, Fixed32)                     |
+-------------------------------------------------------------+
| Filter 1 Byte Offset (4 Bytes, Fixed32)                     |
+-------------------------------------------------------------+
| ...                                                         |
+-------------------------------------------------------------+
| Filter N Byte Offset (End of Filters) (4 Bytes, Fixed32)    |
+-------------------------------------------------------------+
| Offset Array Starting Offset (4 Bytes, Fixed32)             |
+-------------------------------------------------------------+
| Base Lg (1 Byte uint8 = 11)                                 |
+-------------------------------------------------------------+
```
- **Filter Chunk Indexing**: Given a data block at file offset $O$, its filter index is:
  $$\text{Index} = O \gg \text{base\_lg} = O \gg 11$$
- **Offset Array Lookup**:
  - Filter Start: `DecodeFixed32(offset_array + index * 4)`
  - Filter Limit: `DecodeFixed32(offset_array + index * 4 + 4)`
  - Filter Length: $\text{Limit} - \text{Start}$
  - If $\text{Start} == \text{Limit}$, no keys were added in that $2\text{ KiB}$ window (lookup returns `false`).

### 7.2 Built-in Bloom Filter Algorithm (Double Hashing)
- **Policy Name**: `filter.purpledb.BuiltinBloomFilter2`
- **Bits Per Key**: 10 bits/key default ($\approx 1\%\text{ false positive rate}$).
- **Probes**: $k = \text{round}(m/n \times \ln 2) = \text{round}(10 \times 0.693) = 7\text{ hash functions}$.
- **Double Hashing (Kirsch-Mitzenmacher)**:
  Instead of evaluating $k$ distinct hash functions, two 32-bit hashes ($h_1$ and $h_2$) are derived from MurmurHash2:
  $$h = \text{MurmurHash2}(\text{user\_key}, \text{seed}=\text{0xbc9f1d34})$$
  $$\text{delta} = (h \gg 17) \mid (h \ll 15)$$
  $$g_i = h + i \times \text{delta} \pmod m \quad \text{for } i \in [0, k-1]$$
- **Trailing Byte**: The number of hash probes $k$ (1 byte) is appended to the end of the filter byte string:
  $$\text{Filter Buffer} = [\text{Bit Array of } m/8\text{ Bytes}][k\text{ (1 Byte uint8)}]$$

### 7.3 Metaindex Block Layout
The Metaindex Block is a standard SSTable block referenced by the `metaindex_handle` in the 48-byte table footer.
It contains one entry per table metadata block:
- **Key**: Filter policy name slice (e.g. `filter.purpledb.BuiltinBloomFilter2`).
- **Value**: Encoded `BlockHandle` pointing directly to the Filter Block in the SSTable file.

---

## 8. Leveled Storage Hierarchy & Lifecycle

### 8.1 File Name Conventions
Files in the database directory `<dbname>` follow strict naming rules:
- `<dbname>/CURRENT`: ASCII text pointing to active `MANIFEST-<number>`.
- `<dbname>/MANIFEST-<number>`: Binary log storing `VersionEdit` records.
- `<dbname>/<number>.sst`: Static sorted string tables (data blocks, filter, metaindex, index, footer).
- `<dbname>/<number>.log`: Write-ahead log chunks storing mutations.
- `<dbname>/<number>.dbtmp`: Ephemeral write targets for atomic file replacement.

### 8.2 Level Properties
| Level | Key Range Overlap | File Size Target | Level Size Target | Compaction Trigger |
| :--- | :--- | :--- | :--- | :--- |
| **Level 0** | Overlapping (files ordered newest to oldest) | Flushed memtable size ($\approx 4\text{ MiB}$) | N/A | $\ge 4\text{ files}$ |
| **Level 1** | Strictly Disjoint (sorted by user key) | $2\text{ MiB}$ chunks | $64\text{ MB}$ | Total bytes $> 64\text{ MB}$ |
| **Level 2** | Strictly Disjoint (sorted by user key) | $2\text{ MiB}$ chunks | $640\text{ MB}$ | Total bytes $> 640\text{ MB}$ |
| **Level 3..6**| Strictly Disjoint (sorted by user key) | $2\text{ MiB}$ chunks | $10\times$ previous level | Score $> 1.0$ |


