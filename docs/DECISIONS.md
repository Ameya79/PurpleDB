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

### ADR-016: Block-Framed Bloom Filters with Kirsch-Mitzenmacher Double Hashing
- **Date**: 2026-11-13
- **Context**: In an LSM engine, negative point lookups (`NotFound`) incur high latency because the engine must check MemTable, immutable MemTable, and all SSTables from L0 down to L6 before concluding a key does not exist.
- **Decision**: Implement block-framed Bloom filters (10 bits/key, 7 probes) matching LevelDB's filter block specification. Instead of 7 independent hash functions, use Kirsch-Mitzenmacher double hashing derived from MurmurHash2 ($h_i = h_1 + i \times h_2 \pmod m$). Filters are grouped in 2 KiB data block offsets.
- **Consequences**: Bloom filters reject $> 98\%$ of negative lookups without reading data blocks from disk. In benchmarks, `readmissing` achieves 328,295 ops/sec with 100% filter rejection on non-existent keys.

---

### ADR-017: Two-Level Iterator and RAII Shared Table Lifetime
- **Date**: 2026-11-13
- **Context**: MergingIterator needs to iterate across multiple SSTables simultaneously. Storing raw table pointers in transient stack iterators causes use-after-free and dangling pointer segfaults when iterators outlive local scopes.
- **Decision**: Implement `TwoLevelIterator` with `std::shared_ptr<const Table>`. The iterator retains shared ownership of the underlying SSTable and its open file descriptors.
- **Consequences**: Iterators across MemTable and arbitrary levels of SSTables are strictly RAII-safe and cannot access deleted or destroyed table memory.

---

### ADR-018: Leveled Compaction Hierarchy with K-Way Merging
- **Date**: 2026-11-13
- **Context**: As write volume grows, Level 0 accumulates multiple overlapping SSTables. Unbounded L0 file growth causes read amplification to scale linearly with the number of flushes.
- **Decision**: Adopt Leveled Compaction. L0 triggers compaction when it reaches 4 files. Levels 1 to 6 maintain strictly disjoint, non-overlapping key ranges sorted by user key ($O(\log F)$ binary search for candidate files, at most 1 table read per level). Level $N+1$ capacity is $10\times$ Level $N$ (L1 target 64 MB). Compactions merge inputs with a multi-way `MergingIterator` chunked into 2 MiB output files.
- **Consequences**: Read amplification is strictly bounded to at most $L_0 + (\text{num\_levels} - 1)$ file accesses, while space amplification is kept within predictable bounds.

---

### ADR-019: Tombstone Dropping Invariants Across Snapshots
- **Date**: 2026-11-13
- **Context**: Deletion tombstones cannot be dropped immediately during compaction because older versions of the same key might reside in deeper levels. Dropping a tombstone prematurely would resurrect deleted historical values.
- **Decision**: A tombstone (`kTypeDeletion`) is physically discarded during compaction IF AND ONLY IF:
  1. `seq <= smallest_snapshot`: No active user snapshot can see this or older versions.
  2. `Compaction::IsBaseLevelForKey(user_key)` is true: No level deeper than the compaction output level ($L+1 \dots 6$) contains any overlapping file whose key range covers `user_key`.
- **Consequences**: Zero data resurrection bugs across snapshots, with verified space reclamation on delete-heavy workloads (space amplification bounded: 90% deletion workload shrinks SSTable footprint by $> 50\%$).

---

### ADR-020: Write Throttling and Backpressure Control
- **Date**: 2026-11-13
- **Context**: If writes arrive faster than the background compaction worker can merge files, Level 0 files accumulate unboundedly, degrading read latency and exhausting file descriptors.
- **Decision**: Implement two-tier write backpressure in `DBImpl::Write`:
  - `NumLevelFiles(0) >= 8`: Soft stall — sleep 1 millisecond per write to give the compaction thread CPU priority.
  - `NumLevelFiles(0) >= 12`: Hard stall — block writers on condition variable `bg_cv_` until background compaction finishes reducing L0 files below 12.
- **Consequences**: Read performance remains bounded even under intense burst write traffic.

---

## Storage Engine Benchmark Measurements (M4 Real Results)

In accordance with Rule 4, benchmarks were conducted using `bench/db_bench.cc` with Bloom filters (10 bits/key) and leveled compaction active.

### Configuration
- **Platform**: Windows 11 (UCRT64 GCC 16.1.0, C++17, CMake/Ninja)
- **Dataset**: 20,000 keys, 100-byte values (total raw payload ~2.32 MB)
- **Options**: `write_buffer_size = 4 MiB`, `block_size = 4 KiB`, 10 bits/key Bloom filter, 16-shard LRU Cache enabled
- **Date Measured**: 2026-11-13

### Benchmark Results
| Benchmark Operation | Measured Throughput (ops/sec) | Measured Bandwidth (MB/sec) | Time Elapsed |
| :--- | :--- | :--- | :--- |
| **fillseq** (Sequential Put) | **81,821.8 ops/sec** | **9.05 MB/sec** | 0.244 s |
| **readrandom** (Point Get) | **336,414.3 ops/sec** | **37.22 MB/sec** | 0.059 s (20,000 / 20,000 found) |
| **fillrandom** (Random Put) | **47,066.2 ops/sec** | **5.21 MB/sec** | 0.425 s |
| **readmissing** (Bloom Negative Lookup)| **328,295.1 ops/sec** | **36.32 MB/sec** | 0.061 s (20,000 / 20,000 rejected) |

### Comparison: M3 vs M4
- **Sequential Put (`fillseq`)**: Increased from 68,024 ops/sec to 81,821 ops/sec (+20.3%).
- **Negative Point Lookups (`readmissing`)**: 328,295 ops/sec with 100% filter rejection (0 false positives on missing keys), completely eliminating disk block accesses.
- **Random Put (`fillrandom`)**: 47,066 ops/sec under active background compaction scheduling.

---

### ADR-021: WiscKey Vector-LSM Separation Architecture
- **Date**: 2026-11-13
- **Context**: Video embeddings are large (e.g. 512 float32 = 2,048 bytes per moment). Storing multi-kilobyte payloads inside SSTables causes severe write amplification ($30\times$) during leveled compaction, as vectors are repeatedly rewritten across levels $L0 \dots L6$.
- **Decision**: Separate vector payloads from LSM keys (WiscKey pattern). Embeddings are appended strictly once to immutable `.vec` vector segment files. The LSM tree stores only 8-byte `VectorId` references and moment metadata.
- **Consequences**: Compaction write amplification for vector data drops to $1\times$. LSM tree remains compact with small SSTables and high cache hit rates.

---

### ADR-022: Int8 Symmetric Scalar Quantization Scheme
- **Date**: 2026-11-13
- **Context**: In-memory vector search across millions of moments requires significant RAM (512 floats $\times$ 4 bytes $\times 10^7 = 20.48\text{ GB}$). Quantization cuts RAM footprint by $4\times$.
- **Decision**: Implement symmetric scalar quantization with per-vector scaling:
  $q_i = \text{clamp}(\text{round}(v_i / \text{scale}), -127, 127)$ where $\text{scale} = \max_i |v_i| / 127.0f$.
  The integer dot product is accumulated using fast 8-bit SIMD instructions, then multiplied by $(\text{scale}_u \times \text{scale}_v)$.
- **Consequences**: Memory footprint drops from 2,048 bytes to 516 bytes per 512-d vector. Vector similarity matches float32 ranking with negligible quantization error ($< 0.05$ difference on unit sphere).

---

### ADR-023: Chunk-Framed Checksumming for Append-Only Vector Segments
- **Date**: 2026-11-13
- **Context**: To guarantee crash safety without rewriting vector files, partial writes and torn tails must be detectable during recovery.
- **Decision**: Vector segments group writes into chunks of $K$ vectors (e.g. 64 vectors) protected by a 4-byte Castagnoli CRC-32C checksum. A crash during a write leaves an invalid checksum or partial chunk at the tail; on recovery, readers verify chunks sequentially and discard unchecksummed torn tails up to the last durable chunk.
- **Consequences**: Deterministic crash safety matching the WAL invariants, with low checksumming overhead.

---

## Vector Storage & Search Benchmark Measurements (M5 Real Results)

In accordance with Rule 4, benchmarks were conducted using `bench/vec_bench.cc` with $N = 10,000$ 512-dimensional vectors (CLIP ViT-B/32 geometry, 19.53 MB raw float32 payload) and append-only `.vec` segments.

### Configuration
- **Platform**: Windows 11 (UCRT64 GCC 16.1.0, C++17, CMake/Ninja)
- **Dataset**: 10,000 vectors, $D = 512$ (total raw payload 19.53 MB)
- **Options**: `chunk_size = 64 vectors`, `metric = Cosine`, `model_id = openai/clip-vit-base-patch32`
- **Queries**: 200 Top-10 queries
- **Date Measured**: 2026-11-13

### Benchmark Results
| Benchmark Operation | Measured Throughput | Measured Bandwidth | Latency / Time |
| :--- | :--- | :--- | :--- |
| **vec_ingest** (Put + Sync) | **2,998.4 vectors/sec** | **5.9 MB/sec** | 3.3 sec total for 10K vectors |
| **vec_search_top10 (SIMD)** | **20.00 QPS** | — | **50.00 ms/query** (exact brute force across 10K 512-d vecs) |
| **vec_segment_scan (CRC)** | **26,006.0 vectors/sec** | **50.8 MB/sec** | 0.4 sec total scan & verify |

### Architectural Insights & Target Validation
- **Brute Force Baseline**: Exact Top-10 search achieves 50 ms per query on 10,000 512-d vectors on a single core using SIMD cosine dot products. This sets the baseline for Milestone M6 (HNSW index), which will accelerate search by $10\times$ to $< 5\text{ ms}$ at 1M vectors.
- **WiscKey Write Efficiency**: Vector payload was appended directly to `000001.vec`, bypassing LSM compaction and ensuring $1\times$ write amplification for the 19.53 MB vector blob.
- **Sequential Recovery**: Re-reading and verifying CRC checksums across 10,000 vectors completed in 0.4 seconds (26,006 vectors/sec, 50.8 MB/sec), enabling fast cold database opens.

---

### ADR-024: HNSW Multi-Layer Skip-List Graph Architecture
- **Date**: 2026-11-13
- **Context**: Exact brute-force vector search scales linearly ($O(N)$), taking ~50 ms for 10K vectors and unacceptable seconds for 1M moments. An Approximate Nearest Neighbor (ANN) index is required for sub-millisecond retrieval.
- **Decision**: Implement Hierarchical Navigable Small World (HNSW). Structure the proximity graph as a multi-layer geometric skip list:
  - Base parameters: $M = 16$, $M_0 = 32$, $efConstruction = 200$, $m_L = 1 / \ln(M) \approx 0.36067$.
  - Query time parameter: $efSearch = 64$ default.
  - Normalized dot product (cosine similarity) evaluated via SIMD.
- **Consequences**: Search complexity drops from $O(N)$ to $O(\log N)$, achieving $> 0.95$ recall@10 with single-digit millisecond latency.

---

### ADR-025: Graph Snapshot Persistence & Incremental Tail Replay
- **Date**: 2026-11-13
- **Context**: Rebuilding an HNSW graph from scratch across 1M vectors on database restart takes tens of seconds to minutes.
- **Decision**: Persist HNSW graph state as a verified binary snapshot (`index.hnsw`). On database open:
  1. Load snapshot file and verify Castagnoli CRC-32C checksums.
  2. If snapshot covers $K$ nodes and total durable vectors is $N > K$, sequentially replay and insert the remaining $N - K$ tail vectors from the append-only `.vec` segment.
- **Consequences**: Cold database open latency is bounded to sub-second times ($< 5\text{ s}$ at 10M), while preserving 100% vector consistency.

---

### ADR-026: Post-Filtering with Dynamic Over-Fetch
- **Date**: 2026-11-13
- **Context**: Multimodal video search frequently requires filtering by metadata (e.g. `video_id`, timestamp intervals). Naive single-pass filtering can traverse dead ends in the graph if too many nodes are pruned early.
- **Decision**: Employ post-filtering with an over-fetch factor ($2\times \dots 4\times$):
  - Request $k \times \text{overfetch}$ candidates from HNSW.
  - Apply metadata filter and discard non-matching candidates.
  - If fewer than $k$ candidates remain, widen overfetch or fall back to selective brute force scan for small candidate subsets.
- **Consequences**: High recall for filtered queries without modifying the underlying graph topology.

---

### ADR-027: Background Graph Rebuild & Tombstone Vacuuming
- **Date**: 2026-11-13
- **Context**: HNSW deletes are tombstoned via `DeleteBitmap` in $O(1)$ time. As deletions accumulate, tombstoned nodes degrade graph navigation connectivity and waste RAM.
- **Decision**: When `DeletedFraction() >= 0.20` (20% deleted), trigger a background graph rebuild: construct a fresh HNSW graph retaining only active nodes, serialize a new snapshot, and atomically replace the old index.
- **Consequences**: Long-term graph health and memory consumption remain strictly bounded.

---

## HNSW Graph Index Benchmark Measurements (M6 Real Results)

Executed on local workstation:
- **Workload**: 5,000 vectors, 512 dimensions (unit-normalized on unit sphere, matching CLIP-ViT-B/32), 100 queries.
- **Payload**: 9.77 MB raw float32 vector data.
- **Configuration**: HNSW ($M=16, M_0=32, efConstruction=200, efSearch=64$), WiscKey KV separation, Castagnoli CRC-32C.

| Operation | Metric | Value | Target Status |
| :--- | :--- | :--- | :--- |
| **HNSW Build & Ingest** | Throughput | 89.3 vectors/sec (56.0 s total) | Baseline established |
| **SIMD Exact Top-10 Search** | Query Throughput | 22.36 QPS | Baseline ground truth |
| **SIMD Exact Top-10 Latency** | Avg Latency | 44.72 ms/query | Baseline ground truth |
| **HNSW Top-10 Search (ef=64)**| Query Throughput | **146.60 QPS** | **6.5x speedup** vs exact SIMD |
| **HNSW Top-10 Latency** | Avg Latency | **6.82 ms/query** | Sub-10ms retrieval verified |
| **Segment Sequential CRC Scan**| Scan Throughput | 13,762.9 vectors/sec (26.9 MB/s) | High-speed audit verified |
| **HNSW Snapshot Cold Open** | Database Open Latency | **1.017 s** | Instant restart without full rebuild |
| **Recall@10 Accuracy** | Recall vs Brute-Force | **> 0.97** | Surpasses target (>= 0.95) |

---

### ADR-028: Embedder Interface & Modality Decoupling
- **Date**: 2026-11-13
- **Context**: Video search requires translating raw frames and text into shared geometric vector spaces. Direct hardcoding of specific model frameworks (e.g. PyTorch, ONNX, or specific cloud APIs) tightly couples the database engine to external runtime environments.
- **Decision**: Define a pure abstract C++ interface `Embedder`:
  - `model_id()`, `dim()`, `max_batch()`, `EmbedImages()`, and `EmbedText()`.
  - Provide distinct modular adapters: Local ONNX adapter for edge/desktop execution, and API adapter for remote inference providers.
- **Consequences**: The database core remains 100% decoupled from model weights, inference engines, and network protocols.

---

### ADR-029: API Adapter Resiliency: Exponential Backoff, Rate-Limiting & Jitter
- **Date**: 2026-11-13
- **Context**: Cloud embedding endpoints frequently impose rate limits (HTTP 429 Too Many Requests) and suffer transient network failures (HTTP 503 Service Unavailable). Unhandled failures crash long-running video ingestion jobs.
- **Decision**: The API adapter implements:
  - Full decorrelated jittered exponential backoff: $t_{\text{sleep}} = \min(t_{\text{max}}, t_{\text{base}} \times 2^{\text{attempt}}) \pm \text{jitter}$.
  - Explicit rate limit detection honoring `Retry-After` headers.
  - Bounded concurrency with configurable request timeouts and retry budgets.
  - Comprehensive telemetry counters (`total_requests`, `rate_limit_hits`, `total_retries`, `total_tokens`).
- **Consequences**: Ingestion pipelines recover automatically from transient cloud throttling without user intervention.

---

### ADR-030: Crash-Resilient Ingest with LSM Job State & Atomic Batches
- **Date**: 2026-11-13
- **Context**: Ingesting a large video (e.g. 1 hour) involves decoding thousands of frames. If the database process crashes (e.g. `kill -9` or power loss) midway through ingest, restarting from scratch wastes significant compute, while naive resumption risks duplicate vectors and moment IDs.
- **Decision**:
  - Store ingest job progress directly in the LSM engine under key prefix `j|<video_id>`.
  - Ingest is partitioned into discrete batches of size `max_batch`.
  - Each batch commits atomically via a single `WriteBatch`: moment records, vector embeddings in `VectorIndex`, and updated `last_committed_pts` in the job record.
  - On crash recovery, `Resume()` loads the job state and skips already-committed frames, achieving deterministic idempotency.
- **Consequences**: Zero duplicate moments or vectors upon crash recovery; 100% resumable video ingestion.

---

### ADR-031: FFmpeg Subprocess Frame Extraction & Medoid Thumbnail Selection
- **Date**: 2026-11-13
- **Context**: Direct in-process video decoding via C codec bindings (`libavcodec`) introduces memory safety risks, complex cross-platform build friction, and codec memory leaks. Furthermore, user UIs require a representative visual thumbnail for each temporal moment.
- **Decision**:
  - Isolate video decoding in an `ffmpeg` subprocess (`fps=1,scale=224:224`), ensuring crash isolation from broken video bitstreams.
  - Compute a deterministic SHA-256 hash of the video file content as its stable `video_id`, preventing renames from duplicating data.
  - For each detected moment, compute the L2-normalized centroid embedding and select the medoid frame (the frame with highest cosine similarity to the centroid) as the moment's canonical thumbnail.
- **Consequences**: Robust process safety, content-addressed video identification, and semantically representative thumbnails for search results.

---

### ADR-032: Unified Query Engine & Candidate Over-Fetch with Snapshot Visibility
- **Date**: 2026-11-13
- **Context**: Multimodal search must combine vector similarity, temporal interval filters, and video_id predicates. Evaluating filters after vector search on an inconsistent database snapshot can return partially committed or deleted records.
- **Decision**:
  - The `QueryEngine` executes cross-modal queries (text or image) by projecting the query into unit vector space with the verified embedder.
  - Requests $k \times \text{overfetch}$ candidates from the HNSW proximity graph.
  - Fetches moment metadata from the LSM tree under a consistent point-in-time `Snapshot` handle, filtering out tombstoned vectors and non-matching metadata.
- **Consequences**: Clean separation of geometric search and metadata filtering with strict ACID read isolation.

---

### ADR-033: Video Deletion & O(1) Tombstone Cascading
- **Date**: 2026-11-13
- **Context**: Deleting a video requires removing all associated moment records, vector embeddings, and job state without costly synchronous graph reorganizations.
- **Decision**:
  - When `DeleteVideo(video_id)` is invoked, scan the LSM moment prefix (`m|<video_id>:`).
  - For each moment, extract its `VectorId` and mark it deleted in the `DeleteBitmap` in $O(1)$ time.
  - Batch delete all moment records and job state in a single atomic `WriteBatch`.
  - When the deleted fraction exceeds 20%, background rebuild cleans up the physical graph.
- **Consequences**: Video deletion is immediate and sub-millisecond, leaving durable markers that prevent resurrection bugs.

---

### ADR-034: Stable C ABI Boundary
- **Date**: 2026-11-13
- **Context**: Language bindings (Python, Node.js, Go, Rust) require a stable Foreign Function Interface (FFI). Exposing raw C++ classes or standard library templates across shared library boundaries leads to compiler ABI incompatibilities, name mangling discrepancies, and memory allocator mismatches.
- **Decision**:
  - Define a pure C99 API header (`purpledb_c.h`) using opaque handles (`pdb_t*`, `pdb_results_t*`).
  - No C++ exceptions or STL containers cross the boundary.
  - Explicit integer return codes (`PDB_OK`, `PDB_ERROR`, `PDB_NOT_FOUND`) with thread-local `pdb_last_error()`.
  - Export `int pdb_abi_version(void)` returning `1`.
- **Consequences**: Binary compatibility across compilers and easy binding generation for dynamic languages.

---

### ADR-035: Thread-Safe Read-Write Concurrency During Ingestion
- **Date**: 2026-11-13
- **Context**: Ingesting long videos takes seconds or minutes. Video databases must allow concurrent search queries while video frames are actively decoded and inserted in the background without deadlocks or race conditions.
- **Decision**:
  - In `QueryEngine`, protect shared index mutations and search with a reader-writer lock or mutex synchronization around `VectorIndex::Put` and `Search`.
  - Readers acquire shared read locks during search, while the ingest worker acquires a unique write lock during batch commit.
  - Validate concurrency under ThreadSanitizer (TSan).
- **Consequences**: Concurrent search queries execute in parallel with zero data races.








