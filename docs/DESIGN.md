# PurpleDB Architecture & Design Rationale — Milestone M0

This document explains the foundational architectural decisions and computer science concepts underpinning **PurpleDB**, an embedded multimodal vector database designed specifically for semantic video and image search.

In accordance with engineering design principles, this document focuses on **why** each choice was made over alternative approaches, rather than merely documenting what the code does.

---

## 1. Core Problem & Architectural Thesis

### 1.1 The Semantic Video Search Problem
Traditional media retrieval indexes video metadata: file names, manual tags, folder hierarchies, and occasional speech transcripts (ASR). This approach fails fundamentally when users search for visual actions, atmospheric descriptions, or scenes that were never explicitly tagged (e.g., *"a person walking through a misty pine forest at dusk"* or *"a red car turning sharply on wet asphalt"*).

To enable true semantic retrieval, a video database must:
1. Translate visual content into high-dimensional geometric representations where semantic closeness corresponds to Euclidean distance or angular proximity.
2. Segment continuous temporal media into coherent "moments" without manual keyframing.
3. Index millions of high-dimensional vectors with sub-millisecond retrieval latency.
4. Execute entirely embedded within the host application (like SQLite or RocksDB), with zero external service dependencies or cloud latency.

### 1.2 The "Glued Together" Anti-Pattern vs Native Embedded Design
A common naive approach to multimodal search is gluing three disparate systems together:
- FFmpeg scripts executing in cron jobs writing frames to disk.
- SQLite or PostgreSQL storing video metadata and timestamps.
- Chroma, Milvus, or Qdrant running as external containerized vector databases.

This decoupled architecture introduces severe operational and performance pathologies:
- **Two-Phase Commit Overhead**: Updating timestamps in SQLite while updating vector IDs in an external database creates consistency hazards on process crashes or power loss.
- **IPC & Network Serialization Penalties**: Every query serializes vectors across TCP sockets and converts between heterogeneous JSON/Protobuf formats.
- **Memory Footprint Duplication**: Both databases cache data in memory independently without coordination.

PurpleDB unifies metadata, time-interval indexing, and vector similarity into a **single, standalone, embedded C++ engine** (prototyped here in Python for Milestone M0). In PurpleDB, a temporal moment `[start_time, end_time]` and its 512-dimensional vector embedding are an atomic, first-class database record.

---

## 2. Key Architectural Decisions ("Why, Not What")

### 2.1 Why Key-Value Separation (The WiscKey Pattern)?
A standard Log-Structured Merge (LSM) tree (such as LevelDB or RocksDB) buffers writes in an in-memory Memtable and flushes them to disk as immutable Sorted String Tables (SSTables). Background compaction threads continuously read overlapping SSTables across levels, sort and merge them, and write new SSTables.

**The Write Amplification Bottleneck**:
In standard key-value workloads (key = 16 bytes, value = 100 bytes), LSM write amplification of $10\times$ to $30\times$ is acceptable. But in vector databases:
- The vector embedding itself is large: a 512-dimensional vector of 32-bit floats is $512 \times 4 = 2,048\text{ bytes}$ ($2\text{ KB}$).
- If vectors are embedded directly into SSTables, compacting $1\text{ GB}$ of data requires reading and rewriting $10\text{ GB}$ to $30\text{ GB}$ of SSD storage. This destroys flash media write endurance, saturates NVMe bus bandwidth, and causes compaction stalls that stall real-time video ingestion.

**The PurpleDB Solution (WiscKey KV Separation)**:
PurpleDB adopts the WiscKey architectural pattern:
- **LSM Tree (SSTables)**: Stores only lightweight metadata: `key = video_id:moment_id` $\rightarrow$ `value = {start_time, end_time, vector_file_offset, vector_length}` (~64 bytes).
- **Append-Only Vector Segments (`.vec` files)**: Vectors are written sequentially once to append-only segment files and never rewritten during LSM compaction.
- *Result*: LSM compaction handles tiny 64-byte records, reducing write amplification from $30\times$ down to nearly $1\times$, while sequential vector reads during queries leverage OS read-ahead buffering and memory-mapped I/O (`mmap`).

---

### 2.2 Why 1 Frame Per Second (1 fps) Default Sampling?
Standard video is recorded at 24 to 60 frames per second. However, adjacent frames spaced $\frac{1}{30}\text{th}$ of a second apart exhibit over 98% visual redundancy. 

1. **Information Density**: Human semantic actions (walking into a room, picking up an object, a car driving by) evolve over timescales of seconds, not milliseconds. Sampling at 1 fps captures the semantic trajectory of 99% of user-searchable events.
2. **Computational Feasibility**: A 1-hour video at 30 fps contains 108,000 frames. Running a Vision Transformer on 108,000 frames requires hours of compute and generates $216\text{ MB}$ of raw vectors. At 1 fps, the same video requires only 3,600 frames—a $30\times$ reduction in inference compute, memory, and storage while preserving semantic discoverability.
3. **Model Input Downscaling**: By having ffmpeg downsample directly to $224 \times 224$ pixels during decode (`-vf "fps=1,scale=224:224"`), we eliminate the memory overhead of holding high-resolution 4K/1080p pixel buffers in memory and bypass expensive resizing libraries in application space.

---

### 2.3 Why Cosine Similarity via Normalized Dot Product?
Cosine similarity measures the angle between two non-zero vectors $u$ and $v$:
$$\text{CosineSimilarity}(u, v) = \frac{u \cdot v}{\|u\|_2 \|v\|_2} = \frac{\sum_{i=1}^D u_i v_i}{\sqrt{\sum_{i=1}^D u_i^2} \sqrt{\sum_{i=1}^D v_i^2}}$$

If vectors are stored without normalization, every query must compute square roots and norms for both the query and every indexed vector, requiring $3D$ floating-point operations per candidate.

**PurpleDB Invariant: Eager Unit Normalization**:
Every vector produced by the embedder is immediately projected onto the unit hypersphere:
$$\hat{v} = \frac{v}{\|v\|_2} \implies \|\hat{v}\|_2 = 1.0$$

Because $\|\hat{u}\|_2 = 1.0$ and $\|\hat{v}\|_2 = 1.0$, the denominator collapses to $1.0$:
$$\text{CosineSimilarity}(\hat{u}, \hat{v}) = \hat{u} \cdot \hat{v} = \sum_{i=1}^{512} \hat{u}_i \hat{v}_i$$

*Engineering Benefits*:
- Eliminates millions of square roots and division operations during query execution.
- Transforms similarity search over $N$ moments into a single matrix-vector multiplication $M \cdot q$, which hardware BLAS libraries (Intel MKL, Apple Accelerate, OpenBLAS) and CPU SIMD instructions (AVX-512, AVX2, ARM NEON) can execute in microseconds.
- Inner product distance and Cosine distance become mathematically identical, allowing index structures (like HNSW) to use pure dot products.

---

### 2.4 Why Subprocess FFmpeg Instead of In-Process C Codec Bindings?
A database engine's primary directive is data integrity and process resilience. Video media files encountered in the wild (scraped web clips, mobile phone recordings, legacy AVI files) are notoriously malformed and frequently trigger edge cases in codec decoders.

1. **Crash Isolation**: If an in-process C library (`libavcodec`/`libavformat`) encounters a buffer overflow, invalid pointer dereference, or memory corruption bug while parsing a broken H.264 bitstream, it triggers a `SIGSEGV` or access violation that crashes the entire host process. By isolating video decoding in an external `ffmpeg` subprocess, a decoding failure merely returns a non-zero exit code. The database process catches the error, logs it, and continues serving queries safely.
2. **Memory Safety & Leaks**: Decoders often allocate internal frame buffers and state machines. Running them in a subprocess guarantees that when decoding finishes, the operating system tears down the process and instantly reclaims 100% of the allocated memory, eliminating memory leaks.
3. **Packaging Simplicity**: Statically bundling FFmpeg or leveraging pre-compiled platform binaries (via `imageio-ffmpeg` or OS package managers) avoids building complex C++ CMake dependencies for dozens of third-party video codecs across Windows, Linux, and macOS.

---

### 2.5 Moment Threshold Trade-offs & Parameter Sensitivity
PurpleDB segments video by comparing consecutive sampled frames to an anchor frame using cosine similarity threshold $\theta$:

| Threshold $\theta$ | Behavior | Moment Duration | Trade-off / Failure Mode |
|---|---|---|---|
| **Low ($\theta \le 0.85$)** | Under-segmentation | Long ($10\text{s}-30\text{s}+$) | Multiple distinct scenes merge into one moment. The centroid vector blurs distinct concepts together, degrading retrieval precision. |
| **Optimal ($\theta = 0.90$)** | Balanced segmentation | Medium ($3\text{s}-10\text{s}$) | Captures coherent semantic events while maintaining tight concept boundaries. Achieves 100% Hit Rate @ 5 on the benchmark. |
| **High ($\theta \ge 0.95$)** | Over-fragmentation | Very short ($1\text{s}-2\text{s}$) | Minor camera shake or lighting changes trigger premature moment boundaries. Creates $2\times$ to $5\times$ more vectors to index, increasing index memory without retrieval benefit. |

Our empirical sensitivity sweep on the synthetic golden set confirms this:
- At $\theta = 0.85$: Videos collapsed into only 6 moments; queries requiring fine-grained boundaries dropped to 94.4% hit rate.
- At $\theta = 0.90$: Yielded 9 distinct moments with 100.0% Hit Rate @ 5 and clean scene separation.
- At $\theta = 0.95$: Generated 18 fragmented moments, doubling storage while providing no increase in hit rate.

---

## 3. The 8 Core Learning Concepts

The following eight concepts represent the theoretical and practical foundation of modern multimodal retrieval and embedded storage systems.

### Concept 1: Multimodal Joint Embedding Space
Traditional search relies on token matching (BM25) between query words and document text. Multimodal embedding models (such as CLIP ViT-B/32) train a vision encoder (Vision Transformer) and a text encoder (Transformer) using contrastive learning on hundreds of millions of image-text pairs.

The contrastive objective pulls the vision vector $v_I$ and text vector $v_T$ together when they depict the same concept, while pushing non-matching pairs apart. As a result, both media types inhabit the **same 512-dimensional Euclidean space**. In this space, the text *"a golden retriever swimming in a lake"* and an image of a golden retriever in water have a high dot product (>0.30), while an unrelated image (e.g., a skyscraper) produces a near-zero or negative dot product. This geometric alignment allows querying visual media using natural language without generating intermediate textual captions.

```
       [ Text Query: "golden sunset over ocean" ]
                         │
                         ▼ (Text Encoder)
             [ Vector q ∈ ℝ^512 ] (Normalized)
                         │
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
[ Frame 1: Forest ] [ Frame 2: Sunset ] [ Frame 3: Night ]
   Dot: 0.08            Dot: 0.38           Dot: 0.04
   (Low)                (HIGH MATCH)        (Low)
```

---

### Concept 2: Temporal Redundancy & Moment Segmentation
Video is not a collection of independent images; it is a continuous temporal signal. Across a 10-second shot of a person speaking, 95% of pixel data and 99% of semantic information is repeated across frames.

Indexing every frame produces massive vector database bloat with redundant entries that crowd out diverse results in top-$k$ searches. PurpleDB clusters consecutive frames into **Moments**:
1. When a new shot begins, the first frame is designated as the **Anchor Frame**.
2. Subsequent frames are compared to the anchor using cosine similarity.
3. As long as similarity $\ge \theta$ (default 0.90), frames belong to the current moment.
4. When similarity drops below $\theta$ (a scene cut, new subject entering, or major camera pan), the active moment closes and a new anchor is established.
5. The moment is assigned a **Centroid Vector** (the normalized mean vector of all constituent frames) and a **Medoid Thumbnail** (the physical frame whose vector is closest to the centroid).

This reduces vector count by $10\times$ to $50\times$ while providing intuitive time-interval results `[start_sec, end_sec]`.

---

### Concept 3: Cosine Similarity via Normalized Dot Product
In high-dimensional spaces ($\mathbb{R}^{512}$), the absolute magnitude of an embedding vector often reflects token frequency or visual brightness rather than semantic topic. Cosine similarity measures the angle $\theta$ between vectors, making it invariant to vector scale:
$$\cos(\theta) = \frac{u \cdot v}{\|u\|_2 \|v\|_2}$$

By strictly enforcing that all vectors stored in PurpleDB are $L_2$-normalized upon creation ($\|u\|_2 = 1.0$ and $\|v\|_2 = 1.0$), the formula simplifies to:
$$\cos(\theta) = u \cdot v = \sum_{i=1}^{512} u_i v_i$$

This allows top-$k$ search over an entire database of $N$ moments to be expressed as a single matrix multiplication:
$$S = M \cdot q \quad \text{where } M \in \mathbb{R}^{N \times 512}, q \in \mathbb{R}^{512}, S \in \mathbb{R}^N$$

Modern processors evaluate this operation using SIMD vector instructions (computing 8 to 16 float multiplications per clock cycle), evaluating thousands of moment comparisons per millisecond without requiring GPU acceleration.

---

### Concept 4: LSM Write Amplification & Key-Value Separation (WiscKey)
Log-Structured Merge trees achieve high write throughput by appending all incoming mutations to a Write-Ahead Log (WAL) and buffering them in memory before flushing sorted SSTables to disk.

However, to maintain fast point lookups and range scans, LSM trees perform **leveled compaction**: SSTables in Level $L$ are merged with overlapping SSTables in Level $L+1$. In worst-case compactions, data is re-read and re-written up to 10 to 30 times during its lifetime.

For standard keys (16 bytes) and values (100 bytes), this write amplification is tolerable. But for vector records where value = 2,048 bytes, rewriting vectors 30 times rapidly degrades SSD hardware and causes I/O bottlenecks.

**The WiscKey Pattern**:
```
┌──────────────────────────────────────────────┐
│ LSM Tree (SSTables / Leveled Compaction)     │
│ Key: video_id + moment_id                    │
│ Value: {start_ms, end_ms, file_id, offset}   │ (Tiny: ~64 bytes)
└──────────────────────────────────────────────┘
                       │ (Byte Offset Pointer)
                       ▼
┌──────────────────────────────────────────────┐
│ Value Log / Append-Only Vector Segments      │
│ [.vec file: [512 floats][512 floats] ... ]   │ (Zero Compaction Writes!)
└──────────────────────────────────────────────┘
```
By storing only metadata pointers in the LSM tree and storing raw vectors in append-only segment files, PurpleDB keeps write amplification near $1.0\times$ for vector payloads.

---

### Concept 5: Subprocess Isolation vs In-Process Codec Integration
Integrating media decoders into a core database engine creates a major reliability dilemma. Native C libraries like `FFmpeg` (`libavcodec`) contain over one million lines of complex C code handling hundreds of legacy container formats. 

- **Memory Safety Vulnerabilities**: A malformed video stream containing corrupt Huffman tables or illegal slice headers can trigger heap out-of-bounds reads or stack overflows.
- **Process Fate Sharing**: When media decoding runs in-process, any decoder segfault immediately crashes the database server, terminating active user connections and risking database state corruption.

By delegating decoding to an isolated `ffmpeg` subprocess via standard OS process boundaries:
1. Decoding runs in an isolated virtual memory space.
2. If ffmpeg crashes, the host PurpleDB process captures the non-zero exit code and raises a structured exception without terminating.
3. Media frames are streamed via anonymous OS pipes or temporary disk scratchpad directories that the OS cleans up automatically.

---

### Concept 6: Hit Rate @ k and Evaluation Metrics in Information Retrieval
Unlike classification tasks where accuracy is measured as correct vs incorrect classifications, Information Retrieval (IR) systems produce ranked lists of candidate results.

Key IR metrics evaluated in PurpleDB:
1. **Hit Rate @ k (Success@k)**:
   The proportion of test queries for which at least one relevant moment appears in the top $k$ positions:
   $$\text{HitRate@}k = \frac{1}{|Q|} \sum_{q \in Q} \mathbb{I}(\text{first\_hit\_rank}(q) \le k)$$
   For PurpleDB Milestone M0, our acceptance criterion was $\text{HitRate@5} \ge 0.80$ (80%). The prototype achieved **100.0%**.

2. **Mean Reciprocal Rank (MRR@k)**:
   Measures how high up in the ranking the first relevant result appears. If the first hit is at rank 1, reciprocal rank is $1.0$; if at rank 2, $0.5$; if at rank 5, $0.2$:
   $$\text{MRR@}k = \frac{1}{|Q|} \sum_{q \in Q} \frac{1}{\text{rank}_1(q)}$$
   Our baseline prototype achieved **MRR@5 = 0.7611**, demonstrating that relevant moments typically appear at rank 1 or rank 2.

3. **Precision @ 1**:
   The percentage of queries where the single top-ranked result is an exact hit (achieved **55.6%**).

---

### Concept 7: Temporal Intersection and Continuous Window Matching
In document retrieval, matching is discrete: document ID either matches or it does not. In video retrieval, matching is continuous across the time domain. A ground-truth event may span $[5.0\text{s}, 10.0\text{s}]$, while the database returns a detected moment spanning $[4.5\text{s}, 9.8\text{s}]$.

To evaluate retrieval fidelity objectively, PurpleDB implements three complementary temporal overlap standards:
1. **Absolute Temporal Overlap**:
   $$\text{Overlap}(r, gt) = \max(0, \min(r_{\text{end}}, gt_{\text{end}}) - \max(r_{\text{start}}, gt_{\text{start}})) \ge 1.0\text{s}$$
2. **Temporal Intersection over Union (tIoU)**:
   $$\text{tIoU}(r, gt) = \frac{\text{Overlap}(r, gt)}{\max(r_{\text{end}}, gt_{\text{end}}) - \min(r_{\text{start}}, gt_{\text{start}})} \ge 0.30$$
3. **Midpoint Inclusion with Tolerance**:
   $$|t_{\text{mid\_res}} - t_{\text{mid\_gt}}| \le 2.0\text{s}$$

If any of these conditions are met and the video ID matches, the retrieved candidate is accepted as a valid Hit.

---

### Concept 8: Deterministic Content-Based Video Identification
File systems are inherently mutable: users frequently rename video files (`clip1.mp4` $\rightarrow$ `vacation_beach.mp4`), move them across directories, or download duplicate copies.

If a database keys records by filesystem path:
- Moving a file creates orphaned records in the database.
- Renaming a file causes the database to ingest the entire video a second time, doubling vector storage and returning duplicate search hits.

**The Solution: Cryptographic Content Hashing (SHA-256)**:
Upon ingestion, PurpleDB computes the SHA-256 digest of the video file contents:
$$\text{video\_id} = \text{SHA256}(\text{file\_bytes})$$

*Guarantees*:
- **Idempotency**: Ingesting the same file multiple times with different filenames immediately matches the existing `video_id`, enabling zero-cost deduplication.
- **Resumability**: If ingestion is interrupted by a system restart, the pipeline detects which video IDs are already indexed and skips redundant processing.
- **Immutability**: The primary key is coupled to the actual media bytes, not transient filesystem paths.

---

## 4. Milestone M0 Go/No-Go Decision

### 4.1 Evaluation Summary
- **Benchmark Suite**: 6 synthetic benchmark videos with diverse color palettes, moving geometric objects, scene simulations, and material textures.
- **Queries**: 18 natural language queries evaluated with rank cutoff $k=5$.
- **Measured Hit Rate @ k=5**: **100.0%** (18/18 hits within top 5).
- **Measured MRR @ k=5**: **0.7611**.
- **Measured Precision @ 1**: **55.6%**.
- **Clustering Threshold Sensitivity**: Robust across $\theta \in [0.85, 0.95]$ with hit rates $\ge 94.4\%$. Optimal balance achieved at $\theta = 0.90$.
- **Sampling Rate Sensitivity**: Verified across $0.5$, $1.0$, and $2.0$ fps. 1.0 fps provides optimal temporal precision with minimal moment count.

### 4.2 Recommendation
**DECISION: GO FOR MILESTONE M1**

The Python exploration prototype demonstrates that:
1. CLIP ViT-B/32 multimodal embeddings provide strong zero-shot cross-modal alignment between natural language queries and video moments.
2. Anchor-based clustering with threshold $\theta = 0.90$ effectively segments video into coherent semantic intervals without cumulative drift.
3. Unit $L_2$ normalization and dot-product matrix search achieve instant retrieval with zero numerical instability.

The architectural foundation is validated and ready for transition to high-performance C++ storage engine development in Milestone M1.


---

# Milestone M1: C++ Foundations & Storage Mechanics

## 1. Non-Owning Memory Views (`Slice` vs. `std::string`)
In C++, `std::string` manages its own heap memory. Whenever you pass a `std::string` by value or create a substring (`s.substr(start, len)`), a new heap allocation and buffer copy occurs. In an embedded database processing millions of keys and multi-megabyte SSTables:
- Allocations cause severe heap fragmentation and lock contention on the memory allocator.
- `Slice` is a **non-owning view**: it holds only `const char* data_` (8 bytes) and `size_t size_` (8 bytes).
- Passing, copying, or shrinking a `Slice` (`remove_prefix(n)`) takes $O(1)$ operations and zero heap allocations.
- **Safety Rule**: The underlying memory must outlive the `Slice`.

## 2. Zero-Cost Success Pattern in `Status`
Throwing and catching C++ exceptions requires stack unwinding, table lookups, and compiler runtime overhead. In high-performance systems:
- Operations return a `Status` object by value.
- To eliminate heap allocations on the 99.9% common success path, `Status` holds a single pointer `const char* state_`.
- On success (`kOk`), `state_ == nullptr`. Returning `Status::OK()` compiles down to loading 0 into a register (`xor eax, eax`).
- Only when an error occurs is a heap buffer allocated with the message and code.

## 3. Endianness & Hardware Serialization
CPUs load integers according to native byte order (endianness).
- On **x86-64 and modern ARM (little-endian)**, the value `0x12345678` is stored in memory as `[0x78, 0x56, 0x34, 0x12]`.
- Directly casting structs to `char*` and writing to disk creates non-portable files that corrupt when read on systems with different endianness or struct packing.
- `core/util/coding.h` explicitly serializes numbers with bit shifts:
  ```cpp
  buffer[0] = static_cast<uint8_t>(value);
  buffer[1] = static_cast<uint8_t>(value >> 8);
  buffer[2] = static_cast<uint8_t>(value >> 16);
  buffer[3] = static_cast<uint8_t>(value >> 24);
  ```
  This is 100% portable across all CPU architectures and compiler optimizations.

## 4. Variable-Byte (Varint / LEB128) Compression
Database keys and values often have small lengths (e.g. 5 to 64 bytes).
- Storing length as a standard 32-bit integer uses 4 bytes per record. For 10,000,000 keys, storing 4-byte lengths wastes 30 MB of disk and cache space.
- Varint stores 7 bits of value per byte. The 8th bit (`0x80`) is a continuation flag:
  - If value $< 128$: encoded in **1 single byte**.
  - If value $< 16384$: encoded in **2 bytes**.
- All decoders must check buffer boundaries (`limit`) to avoid buffer overruns on corrupt inputs.

## 5. Castagnoli CRC-32C and Masking
Disk drives and memory hardware occasionally experience silent bit flips (bit rot) or partial page writes (torn writes).
- CRC-32C Castagnoli detects all 1-, 2-, and 3-bit errors in blocks up to 64 KiB.
- **Masking**: If a block stores its own CRC, and that block is subsequently checksummed by an outer layer, identical checksum values can cancel out. Rotating by 15 bits and adding a constant (`kMaskDelta = 0xa282ead8`) prevents self-canceling checksum collisions.

## 6. Static Registration in C++ Test Frameworks
Our test harness in `core/util/test_harness.h` uses static initialization:
```cpp
#define TEST(suite, name) \
  void suite##_##name##_Test(); \
  static ::purpledb::test::TestRegistrar suite##_##name##_Registrar( \
      #suite, #name, suite##_##name##_Test); \
  void suite##_##name##_Test()
```
When the program starts, the C++ runtime initializes all global and file-scope static variables before `main()` executes. The constructor of `TestRegistrar` registers the function pointer with `TestRunner::Instance()`. `main()` simply calls `TestRunner::Instance().RunAll()`.

---

## What You Should Understand Now (Milestone M1 Concepts)
1. **Value-Semantic Error Handling (`Status`)**: Why databases avoid exceptions on hot paths, and how a pointer-sized `Status` with null state provides zero-cost success paths.
2. **Non-Owning Memory Views (`Slice`)**: The memory layout of `const char*` + `size_t`, why it eliminates substring copying, and how to maintain buffer lifetime safety.
3. **Deterministic Serialization (Little-Endian)**: Why writing raw memory structs to disk breaks portability, and how explicit bit-shifting ensures multi-platform binary compatibility.
4. **Variable-Byte Integer Compression (LEB128 Varint)**: How the 7-bit continuation flag encodes small integers in 1 byte, saving up to 75% space on metadata.
5. **CRC-32C Checksums & Masking**: The Castagnoli polynomial for error detection in storage blocks, and why checksum masking prevents self-referential collisions.
6. **Bounds-Checked Binary Parsing**: How to prevent buffer overflow vulnerabilities by passing pointer and limit boundaries (`p` and `limit`) to all binary decoders.
7. **Static Registration in Test Frameworks**: How global static initializers register test cases before `main()` without requiring manual test lists.
8. **Sanitizers (ASan & UBSan)**: How compiler instrumentation detects memory leaks, out-of-bounds array accesses, and undefined behavior at test runtime.


---

# Milestone M2: Minimal Durable KV & WAL Mechanics

## 1. Write-Ahead Logging (WAL) & Durability
In memory-first storage engines (like LSM trees), updating memory directly is fast but dangerous: a power loss or crash destroys all volatile state.
- **WAL Invariant**: Before any key-value mutation is applied to the in-memory MemTable, it is first serialized and appended to an append-only disk log (the WAL).
- **Sequential Disk Writes**: Disk drives and NVMe SSDs excel at sequential appends (GB/s bandwidth) compared to random updates. WAL appends give disk durability with near-memory write latency.

## 2. 32 KiB Block Framing & Torn Write Isolation
Operating systems and physical drives write data in pages or sectors (typically 4 KiB).
- When a crash occurs in the middle of writing a 16 KiB record, the drive might complete the first 4 KiB and fail the rest (a **torn write**).
- By grouping the WAL into fixed 32 KiB blocks with individual record headers and checksums, PurpleDB detects exactly which fragment was partially written.
- If fewer than 9 bytes remain in a block, the rest of the block is padded with zeroes (`kZeroType`), ensuring headers never straddle block boundaries without a fragment type flag.

## 3. Atomic WriteBatch & Serialization
A database transaction often updates multiple records (e.g. video moment index entry + thumbnail metadata + ingestion job checkpoint).
- If the system crashed after writing record 1 of 3, the database state would be internally inconsistent.
- `WriteBatch` serializes all operations into a single contiguous byte buffer with a starting sequence number and an operation count.
- The entire batch is written to the WAL as a **single logical record**. If the record is incomplete on disk, recovery rejects the whole record, preserving atomicity.

## 4. MVCC Internal Keys & Descending Sequence Ordering
PurpleDB uses Multi-Version Concurrency Control (MVCC):
- An internal key is formed by packing the user key and an 8-byte tag:
  $$\text{Tag} = (\text{SequenceNumber} \ll 8) \mid \text{ValueType}$$
- When comparing two internal keys with the same user key, sequence numbers are sorted **in descending order** ($100 > 99 > \dots > 1$).
- **Why descending?** When searching for user key `"profile"` at snapshot sequence 50, a binary search (`lower_bound`) targeting `("profile", 50)` immediately lands on the newest visible version at or before sequence 50, without scanning past older historical versions.

## 5. Tombstone Deletions in Append-Only Storage
In an append-only storage engine, deleting a key cannot seek to arbitrary disk locations to erase old bytes.
- Instead, `Delete(key)` writes a **tombstone** record (`kTypeDeletion`).
- In the MemTable, the tombstone has a newer sequence number than any previous `Put`. Lookups find the tombstone and return `NotFound`.
- Obsolete values and tombstones are only physically discarded during SSTable compaction (Milestone M4) once no older snapshots can reference them.

## 6. Crash Recovery & Tail Prefix Invariance
- On database restart, `DBImpl::Recover()` opens the active WAL file and sequentially verifies each record fragment's CRC-32C.
- If power failed while appending a record, the tail will contain partial bytes or a checksum mismatch.
- **Recovery Rule**: Read until the first corrupted record, safely ignore the tail, and rebuild the MemTable from the valid committed prefix.

---

## What You Should Understand Now (Milestone M2 Concepts)
1. **Write-Ahead Logging (WAL)**: Why database engines write mutations to an append-only sequential log before touching in-memory data structures.
2. **Block Fragmenting (32 KiB Framing)**: How segmenting records across fixed-size blocks isolates torn writes and allows recovery to detect damaged disk sectors.
3. **Atomic Multi-Key Batches (`WriteBatch`)**: How packing multiple operations into a single contiguous WAL record prevents partial updates during crashes.
4. **Internal Key Layout & Descending Sequence Order**: Why internal keys sort user keys ascending but sequence numbers descending, and how this accelerates `lower_bound` lookups.
5. **Tombstone Deletions**: Why append-only databases treat deletions as mutations rather than in-place erasures.
6. **Sync Policies (`fsync` vs. Page Cache)**: The performance and durability trade-offs between OS-buffered appends and hardware disk syncs (`_commit` / `fsync`).
7. **Consistent Prefix Recovery**: How the WAL reader guarantees that truncating a log file at any arbitrary byte offset always restores a consistent database state.
8. **Pluggable Filesystem Abstraction (`Env`)**: Why isolating OS file and directory operations behind abstract interfaces simplifies testing and ensures cross-platform reliability.

---

# Milestone M3: SSTables, SkipList MemTable, Block Cache, and Background Flush

## 1. Bump-Pointer Arena Allocation
General-purpose memory allocators (`malloc`, `operator new`) incur metadata overhead (8 to 16 bytes per allocation) and suffer from thread contention and heap fragmentation when handling hundreds of thousands of tiny node allocations.
- **Arena Design**: An `Arena` pre-allocates large memory blocks (4 KiB each). Allocations advance a simple bump pointer:
  ```cpp
  char* result = alloc_ptr_;
  alloc_ptr_ += bytes;
  return result;
  ```
- **Alignment**: Hardware architectures require aligned memory access (e.g. 8-byte alignment for 64-bit pointers). The Arena rounds allocations up to 8-byte boundaries using `(bytes + 7) & ~7`.
- **$O(1)$ Teardown**: Individual nodes in the MemTable never need individual `free()` or `delete` calls. When a MemTable is flushed to an SSTable, destroying the `Arena` frees all blocks in a single loop over its vector of buffers.

## 2. Lock-Free Reader SkipList
Balanced trees (like red-black trees in `std::map`) require complex node rotations during insertion, necessitating global locks that stall concurrent readers.
- **SkipList Structure**: A SkipList consists of probabilistic linked lists arranged in vertical levels ($0 \dots \text{MaxHeight}-1$). Level 0 links every node; higher levels skip intermediate nodes with probability $p = \frac{1}{4}$.
- **Lock-Free Concurrency**:
  - Node forward pointers are stored as `std::atomic<Node*>`.
  - Writers update pointers using `release` memory order; readers traverse using `acquire` memory order.
  - Readers can search and iterate concurrently with a single writer without taking any mutex locks.

## 3. Prefix Delta Compression and Restart Points
Keys in sorted SSTables frequently share substantial prefixes (e.g. `video_001_moment_0001`, `video_001_moment_0002`).
- **Delta Encoding**: Each entry stores:
  - `shared_bytes`: Number of leading bytes identical to the previous key.
  - `unshared_bytes`: Number of remaining bytes unique to this key.
  - `value_length`: Byte length of the value.
  - `key_delta` + `value`: Raw payload.
  This compresses key storage by 50% to 70%.
- **Restart Points**: Pure delta encoding would force a reader to decode every entry from the beginning of a block to find a key. To preserve fast random lookups:
  - Every 16 keys (`block_restart_interval = 16`), prefix compression restarts (`shared = 0`), storing the full key.
  - An array of restart offsets is stored at the end of the block.
  - Seeking a key binary searches across the restart array, then scans linearly through at most 16 entries.

## 4. Two-Level Table Indexing & Footers
Reading an entire SSTable into memory would overwhelm RAM.
- **Two-Level Indexing**:
  1. Data is partitioned into small, independently checksummed **Data Blocks** (~4 KiB each).
  2. An **Index Block** stores one entry per data block containing the block's highest key and its `BlockHandle` (`offset` and `size`).
  3. A two-level iterator first navigates the index block to find the data block containing the target key range, then loads that single data block.
- **Fixed 48-Byte Footer**:
  - The SSTable ends with a fixed 48-byte trailer containing the index block handle and a magic number (`0xdb477572706c6564ull`).
  - To open an SSTable, the engine reads the last 48 bytes of the file, discovers the index block, and loads the index into cache.

## 5. 16-Way Sharded LRU Block Cache
Repeatedly reading 4 KiB data blocks from disk incurs substantial I/O overhead.
- **Sharding for Concurrency**: Guarding an LRU cache with a single global mutex creates a serialization bottleneck under multi-threaded read workloads. PurpleDB splits the cache into 16 independent shards using `hash(key) >> 28`. Each shard has its own mutex and doubly-linked LRU list.
- **Handle Refcounting & Zero-Copy Safety**:
  - When a query reads a block from the cache, the cache returns a `Cache::Handle` that increments the block's reference count.
  - Even if the LRU eviction algorithm evicts the block from the cache hash table to free memory, the underlying memory remains valid until the reader drops its handle (`Release()`).

## 6. Manifest, VersionEdit, and CURRENT Atomic Switch
An LSM database creates new SSTables during MemTable flushes and compaction. Tracking these files safely across crashes requires transactional metadata management:
- **`VersionEdit`**: A record that represents incremental state changes (files added at a level, files deleted, next file number, sequence number).
- **`MANIFEST` Log**: An append-only WAL file that records `VersionEdit`s. On startup, replaying the manifest reconstructs the active set of SSTables without needing to inspect file directory timestamps.
- **`CURRENT` File Pointer**: Points to the active manifest file. Updating `CURRENT` is performed by writing `CURRENT.tmp` and renaming it to `CURRENT`, guaranteeing atomic filesystem directory switchover.

## 7. Level 0 Overlapping SSTables & Reverse Scan Precedence
When MemTables fill up, they are flushed directly to Level 0 (L0).
- Unlike levels 1 through 6, where key ranges between SSTables are strictly disjoint, **L0 SSTables can have overlapping key ranges** because each flushed table covers the key range present in its MemTable.
- **Precedence Rule**: When searching for a key, L0 files must be searched in **reverse chronological order** (newest file to oldest file). If a key exists in an older L0 file and a newer L0 file, the version in the newer file takes precedence.
- Once the key is found or a tombstone is encountered, searching stops.

## 8. Safe Recovery MemTable Flush
When a database reopens after a shutdown:
- Recovery replays the WAL into the in-memory MemTable.
- If the WAL contained committed mutations, PurpleDB immediately flushes the recovered MemTable into a new L0 SSTable, logs the change in `MANIFEST`, deletes the old WAL, and opens a fresh WAL.
- This bounds recovery time on subsequent restarts and ensures SSTable state is always clean.

---

## What You Should Understand Now (Milestone M3 Concepts)
1. **Bump-Pointer Arena Allocation**: Why arena allocators eliminate heap fragmentation, provide 8-byte memory alignment, and allow $O(1)$ bulk deallocation of complex in-memory indexes.
2. **Lock-Free Reader SkipLists**: How probabilistic multi-level linked lists achieve $O(\log N)$ search and insertion, and how `acquire`/`release` atomics enable concurrent reader access without locks.
3. **Prefix Delta Compression & Restart Intervals**: How delta encoding compresses sorted string keys by factoring out common prefixes, and why restart arrays maintain $O(\log N)$ binary search performance.
4. **Two-Level Indexing in Storage Files**: How index blocks map key ranges to byte offsets (`BlockHandle`), allowing point lookups to inspect exactly one 4 KiB data block rather than scanning the entire file.
5. **Fixed Trailer Footers & Magic Numbers**: Why database file formats place a fixed-size header or footer with a magic number to enable random-access bootstrapping without reading full files.
6. **Sharded LRU Caching & Handle Refcounting**: How sharding minimizes lock contention across CPU cores, and how reference-counted handles prevent use-after-free bugs during concurrent block eviction.
7. **Append-Only Manifest Logging & Atomic CURRENT**: Why database engines never rewrite directory metadata in-place, and how WAL-backed manifests with atomic file renames guarantee crash-consistent state transitions.
8. **Level 0 Overlapping File Semantics**: Why flushed L0 SSTables have overlapping key intervals and must be queried in reverse chronological order to satisfy snapshot visibility.

---

# PurpleDB Architecture & Design Rationale — Milestone M4

This section documents the architectural principles behind **Milestone M4: Compaction, Bloom Filters, and Snapshots**.

## 1. Block-Framed Bloom Filters & Double Hashing
In an LSM storage engine, a query for a non-existent key must search the MemTable, immutable MemTable, and every level of the LSM tree before returning `NotFound`. Without filtering, each SSTable inspected requires fetching index and data blocks from disk.
- **Bloom Filter Principle**: A space-efficient probabilistic data structure that can return false positives (saying a key *might* be present when it isn't), but **never false negatives** (if it says a key is absent, it is guaranteed not to exist).
- **Block-Framed Filters (2 KiB Base)**: Rather than building one massive Bloom filter per SSTable, PurpleDB builds one filter per 2 KiB chunk of data blocks. Given a block offset $O$, the engine computes `index = O >> 11`, loads only that filter chunk, and tests the user key.
- **Kirsch-Mitzenmacher Double Hashing**: Generating $k$ independent hashes normally requires $k$ hash functions. Kirsch and Mitzenmacher (2006) proved that two hash values $h_1$ and $h_2$ suffice to simulate $k$ independent hash functions without degrading asymptotic false-positive rates:
  $$g_i = h_1 + i \times h_2 \pmod m$$
  PurpleDB derives $h_1$ from MurmurHash2 and $h_2$ from bit rotation `(h >> 17) | (h << 15)`. At 10 bits/key and $k = 7$, the false positive rate is $< 1\%$.
- **User Key vs Internal Key Indexing**: Bloom filters must index **user keys** rather than internal keys. Since users query user keys across arbitrary sequence numbers, indexing user keys enables single-check rejection regardless of snapshot visibility.

## 2. K-Way MergingIterator
In an LSM engine, data is partitioned across multiple sorted streams: active MemTable, immutable MemTable, Level 0 files, and Level 1..6 files.
- **K-Way Merge**: `MergingIterator` provides a single sorted view over $K$ child iterators.
- **Forward Traversal**: Moves the child with the smallest current key forward and repositions.
- **Backward Traversal**: Locates the child with the largest current key and steps backwards.
- **Direction Reversal**: When switching traversal directions (e.g. `Next()` followed by `Prev()`), child iterators must be repositioned to skip duplicate keys and maintain sorted invariants.

## 3. Snapshot Isolation and User Sequence Visibility
PurpleDB provides point-in-time Snapshot Isolation:
- **Snapshot Implementation**: Snapshots are tracked in a circular doubly-linked list (`SnapshotList`). Each snapshot records a sequence number representing the maximum visible committed mutation at the moment it was acquired.
- **Visibility Invariant**: A query or iterator assigned snapshot sequence number $S$ can see only internal keys with `sequence_number <= S`.
- **`DBIter` Wrapper**: Wraps the internal `MergingIterator`. It skips versions newer than $S$, deduplicates older shadowed versions of the same user key, and transparently skips deletion tombstones (`kTypeDeletion`).
- **RAII Table Lifetimes**: `TwoLevelIterator` retains a `std::shared_ptr<const Table>`, preventing active iterators from crashing if a concurrent background compaction deletes an underlying SSTable from the active VersionSet.

## 4. Leveled Compaction Architecture
Flushed Level 0 SSTables have overlapping key ranges, degrading read throughput if left unchecked.
- **Leveled Hierarchy**: Levels 1 through 6 maintain **strictly disjoint, non-overlapping sorted files**. Candidate files can be located in $O(\log F)$ time via binary search, guaranteeing at most 1 table read per level.
- **Compaction Picking**:
  - Level 0 triggers compaction when `NumLevelFiles(0) >= 4`.
  - Levels 1..5 trigger compaction when total bytes exceed level limits. Target capacity grows by $10\times$ per level (Level 1: 64 MB, Level 2: 640 MB, etc.).
- **Atomic Manifest Updates**: Compaction outputs (chunked into 2 MiB files) and deleted input files are applied atomically to the `VersionSet` via a single `VersionEdit` written to `MANIFEST`.

## 5. Tombstone Dropping Invariants
A deletion tombstone cannot be dropped simply because it shadowed an older version during compaction. If an even older version of the key exists in a deeper level ($L+2 \dots 6$), dropping the tombstone would cause the older version to "resurrect" into visible reality.
- **The Tombstone Dropping Rule**: A tombstone (`kTypeDeletion`) is physically discarded during compaction IF AND ONLY IF:
  1. `seq <= smallest_snapshot`: No active snapshot can see the tombstone or any older version.
  2. `Compaction::IsBaseLevelForKey(user_key)` is true: No level deeper than the compaction output level contains any file overlapping `user_key`.

## 6. Write Throttling and Backpressure
If write throughput exceeds compaction bandwidth, Level 0 files multiply, causing read amplification to skyrocket and exhausting OS file descriptors.
- **Two-Stage Backpressure**:
  - Soft slowdown at $\ge 8$ L0 files: Writer sleeps 1ms, yielding CPU slices to the compaction worker thread.
  - Hard stall at $\ge 12$ L0 files: Writer suspends on a condition variable (`bg_cv_`) until background compaction reduces L0 files below 12.

## 7. Orphan File Purging on Open
System crashes or abrupt power loss during compaction or flushing can leave uncommitted `.sst`, `.dbtmp`, or `.log` files on disk.
- On startup, after replaying the manifest to establish the live file set, PurpleDB sweeps the database directory and immediately unlinks any SSTables not referenced in the active `VersionSet`, purging orphaned disk remnants before serving queries.

---

## What You Should Understand Now (Milestone M4 Concepts)
1. **Kirsch-Mitzenmacher Double Hashing**: How two independent hash values ($h_1, h_2$) simulate $k$ distinct hash functions ($g_i = h_1 + i \cdot h_2$), eliminating hash computation overhead while preserving the optimal Bloom filter false-positive curve.
2. **Block-Framed Bloom Filters**: Why grouping filters by 2 KiB data block offsets bounds filter loading overhead and enables negative point lookup rejection without decoding data blocks.
3. **K-Way Merge Iteration**: How multi-stream iterators coordinate forward and backward traversal across disjoint and overlapping inputs, and how direction switching is handled.
4. **Snapshot Isolation & Sequence Visibility**: How integer sequence numbers attached to internal keys implement multi-version concurrency control (MVCC) without table-level read locking.
5. **Leveled Compaction & Geometric Sizing**: Why non-overlapping key ranges at Levels 1..6 bound read amplification to $O(\log F)$ per level, and how $10\times$ level capacity scaling balances write amplification against space amplification.
6. **Tombstone Dropping Invariants**: Why tombstones can only be safely discarded when `seq <= smallest_snapshot` AND no deeper level contains overlapping key ranges, preventing data resurrection bugs.
7. **Write Backpressure & Two-Tier Throttling**: Why high-throughput LSM engines use soft sleep slowdowns followed by hard condition-variable stalls to prevent write storms from overwhelming compaction threads.
8. **Crash-Safe Orphan Purging**: How manifest-driven file set recovery identifies and unlinks abandoned crash artifacts on database startup.

---

# PurpleDB Architecture & Design Rationale — Milestone M5 (Vector Storage & Brute-Force Search)

Milestone M5 marks the transition from a pure key-value store (`purpledb-kv`) to a multimodal vector database.

## 1. WiscKey Vector-LSM Key-Value Separation
In standard LSM databases, storing large values (e.g., 512-dimensional float32 vector embeddings = 2,048 bytes) directly inside SSTables creates catastrophic write amplification. During leveled compaction, an SSTable is rewritten up to 30 times across Levels 0 through 6. For 1 GB of vectors, that translates to 30 GB of SSD flash writes, exhausting hardware endurance and bottlenecking ingestion.

- **The Solution**: In PurpleDB, vectors are written once sequentially into append-only `.vec` segment files.
- **The LSM Role**: The LSM tree stores only lightweight 8-byte `VectorId` integers, user keys, and moment timestamps.
- **Result**: Compaction write amplification for vector data drops from $30\times$ to strictly $1\times$.

## 2. SIMD Acceleration (AVX2 and ARM NEON)
Vector search requires computing geometric distances across thousands or millions of dimensions.
- **AVX2 (Advanced Vector Extensions 2)**: Uses 256-bit registers (`__m256`) to process 8 float32 values simultaneously per instruction cycle. Fused Multiply-Add (FMA) executes $a \times b + c$ in a single CPU clock cycle.
- **ARM NEON**: Uses 128-bit registers (`float32x4_t`) on Apple Silicon and modern ARM64 servers, processing 4 floats simultaneously.
- **Auto-Vectorization Friendly Fallback**: Clean loop unrolling ensures compilers generate vectorized instructions even on platforms without manual intrinsics.

## 3. Symmetric Int8 Scalar Quantization
Storing millions of 512-dimensional vectors in RAM requires significant memory bandwidth (20.48 MB per 10,000 vectors).
- **Quantization Formula**: Maps continuous 32-bit floating point values into 8-bit signed integers:
  $$\text{scale} = \frac{\max_i |v_i|}{127.0f}, \quad q_i = \text{clamp}\left(\text{round}\left(\frac{v_i}{\text{scale}}\right), -127, 127\right)$$
- **Memory Footprint**: Compresses vector size from 2,048 bytes down to 516 bytes (a $4\times$ reduction).
- **Accuracy**: Quantized cosine similarity achieves $< 0.02$ error on unit hyperspheres, retaining near-identical ranking for nearest neighbor search.

## 4. DeleteBitmap: O(1) Tombstone Filtering
Physically removing a vector from an append-only segment or re-indexing after every delete operation would cause severe random I/O and latency spikes.
- **Bitset Representation**: Each vector is indexed by a 64-bit integer `VectorId`. A dense array of 64-bit words stores a bit per vector.
- **Bit Test**: During search, before computing dot products or touching disk, the query evaluates `IsDeleted(vid)`. If the bit is set (`1`), the vector is skipped in $O(1)$ CPU time.
- **Rebuild Trigger**: Tracking `DeletedFraction()` enables background reorganization when tombstones exceed operational thresholds (e.g. 20%).

## 5. Chunk-Framed Castagnoli CRC-32C Verification
To prevent partial writes or power failures from corrupting vector segments:
- Writes are grouped into chunks of $K$ vectors (e.g. 64 vectors).
- Each chunk is immediately followed by a 4-byte Castagnoli CRC-32C checksum.
- On recovery, readers sequentially verify each chunk's checksum. If a crash leaves a partial or torn chunk at the tail of the file, recovery halts cleanly at the last intact chunk, discarding the corrupt tail.

---

## What You Should Understand Now (Milestone M5 Concepts)
1. **WiscKey Key-Value Separation**: Why separating large values (vector embeddings) from index keys eliminates LSM compaction write amplification ($30\times \rightarrow 1\times$).
2. **SIMD Vectorization**: How CPU vector registers (AVX2 256-bit and ARM NEON 128-bit) compute 8 or 4 floating point multiplications in parallel in a single hardware clock cycle.
3. **Scalar Quantization**: How symmetric int8 quantization compresses 32-bit floats by $4\times$ with negligible loss in cosine similarity ranking.
4. **Delete Bitmap Pattern**: Why high-throughput vector stores avoid physical deletes on disk, using fast in-memory bit tests to tombstone vectors in $O(1)$ time.
5. **Chunk-Framed Checksums**: How grouping append-only writes into verified chunks allows deterministic recovery and torn-write detection without write-ahead logging overhead for bulk data.
6. **Model Lock Safety**: Why multimodal databases must enforce invariant checks on vector dimension, metric, and model ID to prevent corrupting similarity spaces across different embedding models.
7. **Top-K Bounded Priority Queue**: How a min-heap of size $K$ extracts the top $K$ nearest neighbors in $O(N \log K)$ time and $O(K)$ auxiliary memory without sorting all $N$ candidates.

---

# PurpleDB Architecture & Design Rationale — Milestone M6 (HNSW Proximity Graph Indexing)

Milestone M6 delivers the core indexing engine of PurpleDB: the **Hierarchical Navigable Small World (HNSW)** proximity graph. HNSW slashes vector retrieval latency from $O(N)$ linear scans down to logarithmic $O(\log N)$ graph traversals while maintaining $> 0.95$ Recall@10.

## 1. The Proximity Graph & Multi-Layer Skip-List Analogy
Brute-force exact search compares a query against every single vector in the database. While SIMD acceleration achieves sub-50 ms latencies for 10,000 vectors, searching 1,000,000 video moments would require 5 seconds per query—completely unacceptable for interactive user search.

HNSW organizes vectors as a multi-layer graph, directly analogous to a probabilistic **Skip List**:
- **Upper Layers (Sparse)**: Contain very few nodes connected by long-range geometric edges ("express lanes"). Greedy routing quickly traverses vast regions of the vector space in a handful of hops.
- **Base Layer 0 (Dense)**: Contains all $N$ nodes connected by dense, local proximity edges. A beam search explores the immediate neighborhood with beam width $efSearch$ to locate the true nearest neighbors.

## 2. Exponential Level Distribution
Each inserted vector is assigned a maximum layer $l$ using an exponential probability distribution parameterized by scale factor $m_L = 1 / \ln(M)$:
$$l = \lfloor - \ln(\text{uniform}(0, 1]) \cdot m_L \rfloor$$
- For $M=16$, $m_L \approx 0.36067$.
- The probability that a node reaches layer $l$ decays exponentially as $1 / M^l$.
- This guarantees logarithmic diameter and logarithmic search complexity ($O(\log N)$) without centralized balancing algorithms.

## 3. Greedy Routing vs Beam Search (efConstruction & efSearch)
HNSW combines two distinct graph navigation algorithms:
1. **Greedy 1-Best Search (Upper Layers)**: At layer $l > 0$, navigation is strictly greedy: from current node $v$, evaluate distance to all neighbors of $v$. If any neighbor is closer to query $q$, move there. When no neighbor is closer, descend to layer $l - 1$.
2. **Beam Search / SearchLayer (Base Layer)**: At layer 0, a priority queue maintains the $ef$ closest candidates discovered so far. The algorithm expands the most promising unexplored candidate until the closest unvisited candidate is further than the furthest element in the priority queue.
   - During index construction: $ef = efConstruction = 200$.
   - During query execution: $ef = efSearch = 64$.
   - Larger $ef$ improves recall at the expense of evaluating more distance metrics.

## 4. Bidirectional Edge Pruning & Small-World Clustering
When node $u$ connects to neighbor $v$, a reciprocal edge $v \rightarrow u$ is also added.
- **Degree Bounding**: To prevent high-density clusters from growing unbounded neighbor lists (which would degrade search latency), each node is capped at $M$ edges on upper layers and $M_0 = 2M$ edges on Layer 0.
- **Neighbor Pruning**: If adding reciprocal edges causes node $v$ to exceed its capacity, its neighbor list is sorted by distance to $v$, and only the closest $M$ (or $M_0$) neighbors are retained. This maintains the "small-world" clustering coefficient across the entire graph.

## 5. Snapshot Persistence & Tail Replay Recovery
Rebuilding an HNSW graph across millions of vectors on database restart would take minutes of heavy CPU computation.
- **Snapshot File (`index.hnsw`)**: PurpleDB serializes the entire graph topology (node levels, degrees, and adjacency lists) directly to disk, secured by a trailing Castagnoli CRC-32C checksum.
- **Incremental Tail Replay**: When opening a database:
  1. Load and verify `index.hnsw` in $< 1\text{ second}$.
  2. If the snapshot contains $K$ vectors and the durable append-only segment contains $N > K$ vectors, the recovery engine replays only the remaining $N - K$ tail vectors into the in-memory graph.
  3. This guarantees instant cold starts without sacrificing consistency.

## 6. Filtered Search with Dynamic Over-Fetch
Video retrieval frequently requires metadata filtering (e.g. searching only within a specific `video_id` or timestamp range).
- **Post-Filtering with Over-Fetch**: The query engine queries HNSW for $k \times \text{overfetch}$ candidates (default overfetch factor $= 2$).
- **Dynamic Expansion**: The filter predicate is evaluated against candidate keys. If fewer than $k$ matching candidates remain, the engine dynamically widens the overfetch factor or falls back to an exact scan over the filtered subset. This prevents graph navigation dead-ends while preserving 100% filter correctness.

## 7. Background Rebuild & Vacuuming
Deleting vectors from a proximity graph without breaking navigation paths is complex.
- **Tombstoning**: Vectors are marked as deleted in the `DeleteBitmap` in $O(1)$ time. Search queries filter out tombstoned nodes during result gathering.
- **Vacuum Trigger**: When the fraction of deleted nodes exceeds 20% (`DeletedFraction() >= 0.20`), PurpleDB triggers `RebuildIndex()`: active nodes are re-inserted into a clean graph, orphan connections are pruned, and a fresh snapshot is atomically committed.

---

## What You Should Understand Now (Milestone M6 Concepts)
1. **Geometric Skip-List Principle**: How hierarchical layers of decreasing density provide logarithmic routing diameter ($O(\log N)$) through continuous high-dimensional vector spaces.
2. **Beam Search vs Greedy Routing**: Why greedy 1-best routing is optimal for coarse upper-layer navigation, while multi-candidate beam search ($efSearch$) is required to achieve high recall on the dense base layer.
3. **The Role of $M$ and $ef$ Hyperparameters**: How $M$ controls graph connectivity and memory consumption ($M$ edges per node), while $efConstruction$ and $efSearch$ trade build and query CPU time for nearest neighbor recall.
4. **Snapshot Cold Restarts**: Why serializing graph topology with checksums avoids expensive graph re-indexing on process restart, and how tail replaying handles newly appended un-snapshotted vectors.
5. **Over-Fetch for Post-Filtering**: How querying $2\times$ to $4\times$ candidates balances graph navigation efficiency against restrictive metadata predicates without modifying the graph topology.
6. **Tombstone Vacuuming**: Why proximity graphs favor bitset tombstones for online deletes, followed by asynchronous vacuum rebuilds when tombstone density degrades navigation paths.
7. **Recall@K Evaluation**: How to evaluate an approximate nearest neighbor index against an exact brute-force ground truth using set intersection over Top-$K$ results.

---

# PurpleDB Architecture & Design Rationale — Milestone M7 (Embedders & Ingest Pipeline)

Milestone M7 bridges raw video media and high-dimensional vector indexing, delivering an automated, crash-resilient ingestion pipeline and pluggable multimodal embedding architecture.

## 1. Embedder Interface & Modality Decoupling
To avoid hardcoding external machine learning frameworks into the database storage engine, PurpleDB employs the **Dependency Inversion Principle**:
- **The Core Interface (`core/embed/embedder.h`)**: `Embedder` defines an abstract contract (`model_id()`, `dim()`, `max_batch()`, `EmbedImages()`, and `EmbedText()`).
- **Zero-Copy Image Views (`ImageRef`)**: Carries either raw RGB in-memory pixel buffers or on-disk file references without unnecessary memory copying.
- **Invariant Enforcement**: Every vector emitted by any embedder adapter is immediately verified and projected onto the unit hypersphere ($\|v\|_2 = 1.0$), ensuring mathematical equivalence between cosine similarity and dot product across the engine.

## 2. API Adapter Resiliency: Rate Limiting & Jittered Backoff
Remote embedding APIs (OpenAI, Voyage, Google Cloud) introduce network flakiness and strict rate quotas.
- **HTTP 429 Rate Limits**: When an endpoint returns HTTP 429 (Too Many Requests), the adapter parses optional `Retry-After` headers and backs off before retrying.
- **Full Jitter Exponential Backoff**: To avoid the "thundering herd" problem where multiple concurrent threads retry simultaneously:
  $$t_{\text{sleep}} = \min(t_{\text{max}}, t_{\text{base}} \times 2^{\text{attempt}}) + \text{uniform}(0, \text{jitter})$$
- **Mock Transport Decoupling**: An abstract `HttpTransport` interface allows deterministic unit and acceptance testing of network failure modes with zero external internet dependencies.

## 3. Crash Resilience & Resumable Ingest Jobs
Ingesting large video files requires processing thousands of frames. A sudden power failure or process kill (`kill -9`) must not corrupt the database or force a full re-ingestion from second zero.
- **LSM Job State Tracking (`j|<video_id>`)**: Ingest progress is tracked in the LSM engine under key prefix `'j'`. The record stores `status` (Pending, Running, Completed), `last_committed_pts`, and `moments_count`, sealed with a Castagnoli CRC-32C checksum.
- **Atomic Batch Commits**: Ingestion processes video frames in discrete batches (e.g. 16 frames). Each batch atomically commits:
  1. Moment metadata records (`m|<video_id>:<moment_id>`).
  2. Embeddings to the append-only `.vec` segment and HNSW graph.
  3. Updated `last_committed_pts` in the job state record.
- **Idempotency on Recovery**: Reopening the database and resuming an ingest job skips all frames where `timestamp <= last_committed_pts`. Re-running on a completed video returns immediately with zero duplicate vectors or moments.

## 4. Subprocess FFmpeg Isolation
Video codecs (`libavcodec`/`libavformat`) contain complex C bitstream parsers vulnerable to memory safety faults when processing malformed media from untrusted sources.
- **Process Isolation**: PurpleDB isolates video decoding into an external `ffmpeg` subprocess (`fps=1,scale=224:224`). If an invalid video triggers a codec fault, only the child process dies; the database catches the return code, reports a clean error status, and remains running.
- **Deterministic Content Addressing**: A SHA-256 hash of the video bitstream serves as the permanent `video_id`. Renaming, copying, or moving a video file resolves to the identical database identity, preventing duplicate vector storage.

## 5. Centroid & Medoid Moment Representation
- **Centroid Embedding**: A moment's semantic vector is the normalized vector sum of all its constituent frame embeddings: $\hat{c} = \frac{\sum v_i}{\|\sum v_i\|}$.
- **Medoid Thumbnail**: Rather than picking an arbitrary frame (such as the first frame, which may be a transition or black screen), the pipeline selects the **medoid frame**—the actual video frame whose embedding exhibits maximum cosine similarity to the centroid. This guarantees semantically representative thumbnails for user search interfaces.

---

## What You Should Understand Now (Milestone M7 Concepts)
1. **Dependency Inversion in AI Systems**: Why decoupling abstract inference contracts (`Embedder`) from concrete inference engines (ONNX Runtime, cloud REST endpoints) makes database storage engines portable, testable, and future-proof.
2. **Exponential Backoff with Full Jitter**: Why decorrelated random jitter is mathematically essential when retrying rate-limited cloud API calls to prevent synchronized retry spikes.
3. **Resumable Write Batches**: How coordinating LSM state checkpoints with append-only data files ensures zero-duplicate crash recovery under abrupt process termination.
4. **Subprocess Sandboxing for Untrusted Media**: Why high-reliability database engines isolate video/image decoding outside the main server address space to prevent codec crashes from terminating the database.
5. **Content-Addressed Entity Hashing**: How SHA-256 bitstream hashes eliminate duplicate storage when media files are renamed, moved, or uploaded via different client paths.
6. **Centroid vs Medoid Selection**: The difference between an average geometric vector (centroid) and the most representative actual physical observation (medoid) for thumbnail generation.
7. **Two-Phase Multimodal Retrieval**: How text queries and image frames projected into a shared cosine hypersphere enable instant cross-modal search without intermediate text tagging.





