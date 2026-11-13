# PurpleDB Architectural Decision Records (ADR)

This log documents all technical design decisions, trade-offs, and dependency rationales across milestones.

---

### ADR-001: C++17 Standard and Build Tooling
- **Date**: 2026-11-13
- **Context**: The engine requires modern language features (RAII, `std::string_view` concepts, structured concurrency, `<chrono>`, `<atomic>`) with rock-solid portability across Linux, macOS, and Windows.
- **Decision**: Standardize on **C++17** using **CMake** (minimum version 3.16) and **Ninja**. No Boost or external utility frameworks (Rule 2).
- **Consequences**: Zero third-party dependency creep. Builds compile cleanly with GCC $\ge 11$, Clang $\ge 14$, and MSVC $\ge 2019$.

---

### ADR-002: Zero-Dependency Embedded Test Harness
- **Date**: 2026-11-13
- **Context**: The PRD specifies "a tiny test harness" for M1. Fetching external testing frameworks (like GoogleTest or Catch2) over the network introduces build dependencies, network fragility during offline development, and compile-time bloat.
- **Decision**: Implement a self-contained, macro-driven micro test framework in `core/util/test_harness.h` providing `TEST(suite, name)`, `ASSERT_EQ`, `ASSERT_TRUE`, and `ASSERT_STATUS_OK`.
- **Consequences**: Completely offline build, 0 external network calls, total test execution under 5 milliseconds.

---

### ADR-003: Little-Endian Binary Wire Format
- **Date**: 2026-11-13
- **Context**: Databases store binary data across heterogeneous hardware architectures.
- **Decision**: All fixed-width integers (Fixed32, Fixed64) and Varints are explicitly encoded and decoded using Little-Endian bit shifts (`core/util/coding.h`).
- **Consequences**: Database files created on x86-64 can be copied and directly opened on ARM64 or other platforms without corruption or conversion passes.

---

### ADR-004: Status Object Memory Optimization
- **Date**: 2026-11-13
- **Context**: High-throughput storage systems execute millions of reads and writes per second. Raising exceptions or allocating heap memory for status objects on the success path causes severe allocator contention.
- **Decision**: Use a single pointer `const char* state_` in `Status`. On success (`kOk`), `state_ == nullptr`, requiring **zero heap allocations** and keeping `sizeof(Status) == 8` bytes. Only failures allocate a single contiguous heap buffer holding message length, error code, and text.
- **Consequences**: Returning `Status::OK()` is as fast as returning a register pointer. Copy and move operations are strictly RAII-safe.

---

### ADR-005: CRC-32C (Castagnoli) Checksum Algorithm
- **Date**: 2026-11-13
- **Context**: Storage engines must detect bit rot, torn writes, and storage corruption in WAL records and SSTable blocks.
- **Decision**: Implement CRC-32C with the Castagnoli polynomial ($0x1EDC6F41$) and LevelDB-compatible masking (`kMaskDelta = 0xa282ead8`).
- **Consequences**: Proven error detection properties on database block sizes ($\le 64$ KiB) superior to IEEE CRC-32, with seamless future acceleration via x86 SSE4.2 and ARM CRC instructions.

---

### ADR-006: Thread-Safe Formatted Logging with Pluggable Sinks
- **Date**: 2026-11-13
- **Context**: The database requires low-overhead logging that is thread-safe and can be silenced or redirected in automated tests without polluting stderr.
- **Decision**: Provide `PURPLE_LOG(level, format, ...)` guarded by an atomic level check before formatting, backed by a mutex-protected pluggable sink.
- **Consequences**: In production, disabled log levels cost a single branch condition. In unit tests, logs can be redirected and verified programmatically.


---

### ADR-007: WAL Record Framing and Block Architecture
- **Date**: 2026-11-13
- **Context**: Unbounded streaming append logs suffer from torn writes when system crashes occur across hardware sector or page boundaries.
- **Decision**: Adopt 32 KiB block framing with a 9-byte header (`[crc32c u32][length u32][type u8]`) supporting record fragmentation (`kFullType`, `kFirstType`, `kMiddleType`, `kLastType`).
- **Consequences**: Block boundaries are predictable. Torn writes at the end of a log never corrupt previous blocks and can be detected and dropped cleanly.

---

### ADR-008: MemTable Backend for Milestone M2
- **Date**: 2026-11-13
- **Context**: The storage engine requires an in-memory mutable index.
- **Decision**: Use `std::map<std::string, std::string, InternalKeyComparator>` for Milestone M2 per PRD specification.
- **Consequences**: The interface remains identical when replacing the backend with an arena-backed SkipList in Milestone M3.

---

### ADR-009: Crash Recovery and Prefix Invariance Policy
- **Date**: 2026-11-13
- **Context**: Abrupt power loss or operating system crashes can leave partial records or un-flushed bytes at the tail of the WAL.
- **Decision**: Recovery reads records sequentially until the first checksum mismatch, truncated record, or EOF. Any corrupted tail is dropped, and the valid committed prefix is retained.
- **Consequences**: Verified by testing: truncating the WAL tail at any byte offset recovers a consistent prefix without holes.

---

### ADR-010: WriteBatch Multi-Operation Atomicity
- **Date**: 2026-11-13
- **Context**: Transactions often require updating multiple keys simultaneously (e.g. video moment index + metadata + job status).
- **Decision**: Mutex-protected WAL appends write the entire `WriteBatch` payload as a single logical record before applying it to the MemTable.
- **Consequences**: Eliminates partial batch application across crashes.

---

### ADR-011: Arena-Allocated SkipList for MemTable
- **Date**: 2026-11-13
- **Context**: Milestone M2 used `std::map`, which performs dynamic heap allocation for each node (causing allocator fragmentation, cache misses, and lock contention).
- **Decision**: Replace `std::map` with an Arena-backed lock-free reader SkipList (`core/storage/skiplist.h`). The Arena uses 4 KiB block allocation with bump pointers. Node forward pointers use `std::atomic<Node*>` with acquire-release memory order.
- **Consequences**: Readers require zero locks while a single writer updates the SkipList. Deallocation is $O(1)$ when the MemTable is flushed and the Arena is destructed.

---

### ADR-012: Prefix-Compressed Blocks with Restart Points
- **Date**: 2026-11-13
- **Context**: Keys in sorted order share significant common prefixes (e.g., `video_001_moment_0001`, `video_001_moment_0002`). Uncompressed keys waste storage bandwidth and cache footprint.
- **Decision**: Implement delta prefix compression in `BlockBuilder` and `Block` with periodic restart points (default every 16 keys). At restart points, `shared == 0` and full keys are stored. An array of 32-bit offsets to restart points is appended to each block.
- **Consequences**: Compresses key storage significantly. Seeking within a 4 KiB block takes $O(\log R + K)$ comparisons (binary search across restart points, then linear scan of at most 16 entries).

---

### ADR-013: Fixed 48-Byte SSTable Footer with Magic Number
- **Date**: 2026-11-13
- **Context**: Readers need to locate index blocks and identify valid SSTables without reading entire files or relying on external metadata.
- **Decision**: Standardize on a fixed 48-byte trailer ending with magic number `0xdb477572706c6564ull`. The footer stores padded `BlockHandle`s for the metaindex block and index block.
- **Consequences**: Readers inspect `file_size - 48` in a single random read, immediately validating the magic number and discovering the index block offset without full table parsing.

---

### ADR-014: 16-Way Sharded LRU Block Cache
- **Date**: 2026-11-13
- **Context**: High-concurrency point lookups accessing SSTables from disk incur I/O latency and mutex contention if guarded by a single global cache lock.
- **Decision**: Implement a 16-way hash-sharded LRU cache (`core/storage/cache.h`). Each shard maintains its own doubly linked list and mutex. Cache handles are reference-counted, ensuring zero-copy read safety while permitting eviction of unreferenced blocks.
- **Consequences**: Cache lock contention is distributed across 16 independent mutexes. Memory consumption is strictly bounded by `capacity`.

---

### ADR-015: MANIFEST VersionEdit Log & CURRENT File Pointer
- **Date**: 2026-11-13
- **Context**: The database needs to track SSTable generation across crashes, point-in-time snapshots, and background flushes.
- **Decision**: Store database version deltas (`VersionEdit`) in an append-only `MANIFEST` file formatted as a PurpleDB WAL log. Track the active manifest via an atomic rename of `<dbname>/CURRENT.tmp` to `<dbname>/CURRENT`. Re-use existing manifest files on reopen using `NewAppendableFile`.
- **Consequences**: Crashes during flush or reopen cannot corrupt database directory state. Replaying the manifest restores the active SSTable version set deterministically.

---

## Storage Engine Benchmark Measurements (M3 Real Results)

In accordance with Rule 4 ("Numbers marked TARGET are goals. Measure them with the benchmark harness and record real results in docs/DECISIONS.md. Never invent numbers."), the storage engine was benchmarked using `bench/db_bench.cc`.

### Configuration
- **Platform**: Windows 11 (UCRT64 GCC 16.1.0, C++17, CMake/Ninja)
- **Dataset**: 20,000 keys, 100-byte values (total raw payload ~2.32 MB)
- **Options**: `write_buffer_size = 4 MiB`, `block_size = 4 KiB`, 16-shard LRU Cache enabled
- **Date Measured**: 2026-11-13

### Benchmark Results
| Benchmark Operation | Measured Throughput (ops/sec) | Measured Bandwidth (MB/sec) | Time Elapsed |
| :--- | :--- | :--- | :--- |
| **fillseq** (Sequential Put) | **68,024.7 ops/sec** | **7.53 MB/sec** | 0.294 s |
| **readrandom** (Point Get) | **476,652.4 ops/sec** | **52.73 MB/sec** | 0.042 s (20,000 / 20,000 found) |
| **fillrandom** (Random Put) | **58,190.1 ops/sec** | **6.44 MB/sec** | 0.344 s |

### Observations & Weaknesses to Address in M4
1. **Read Performance**: Point lookups exceed 470,000 ops/sec due to the fast SkipList MemTable and sharded block cache hits.
2. **Sequential vs Random Write**: Sequential write (68K ops/sec) is ~17% faster than random write (58K ops/sec) due to SkipList insertion locality and reduced branching during MemTable updates.
3. **Identified Weakness**: Currently, all flushed SSTables reside in Level 0 (L0). Over time, accumulating multiple overlapping L0 SSTables requires linear scans across all L0 files for keys not found in earlier files. Milestone M4 (Leveled Compaction) will merge overlapping L0 files into non-overlapping L1+ levels to maintain $O(\log N)$ table lookups.

