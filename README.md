<p align="center">
  <img src="assets/logo.png" alt="PurpleDB Logo" width="160" />
</p>

<h1 align="center">PurpleDB</h1>

<p align="center">
  <b>An embedded multimodal C++ database that searches images and video by meaning.</b>
</p>

<p align="center">
  <a href="https://github.com/Ameya79/PurpleDB/actions"><img src="https://img.shields.io/github/actions/workflow/status/Ameya79/PurpleDB/ci.yml?branch=main&label=CI&logo=github" alt="CI Status" /></a>
  <a href="https://en.wikipedia.org/wiki/C%2B%2B17"><img src="https://img.shields.io/badge/standard-C%2B%2B17-blue.svg?logo=c%2B%2B" alt="C++17" /></a>
  <a href="https://cmake.org/"><img src="https://img.shields.io/badge/build-CMake%20%7C%20Ninja-informational.svg?logo=cmake" alt="CMake" /></a>
  <a href="#storage-engine-benchmarks-milestone-m4"><img src="https://img.shields.io/badge/throughput-336K%20reads%2Fsec-success.svg" alt="Performance" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License" /></a>
</p>

---

## Overview

**PurpleDB** is an embedded, single-binary multimodal database written in C++17. Unlike traditional vector search solutions that run as separate distributed microservices or glue together disparate tools (FFmpeg scripts + SQLite + external vector stores), PurpleDB combines:
1. **LSM Storage Engine**: An embedded key-value engine with Write-Ahead Logging (WAL), an Arena-backed lock-free reader SkipList MemTable, prefix-compressed block-indexed SSTables, 16-way sharded LRU block caching, Block-Framed Bloom filters, and leveled compaction.
2. **Key-Value Separation (WiscKey Pattern)**: Large high-dimensional vector embeddings are written to append-only vector segment files and never rewritten during LSM compaction, slashing write amplification from $30\times$ down to near $1\times$.
3. **Temporal Multimodal Ingestion**: Automated video frame sampling (FFmpeg subprocess isolation), CLIP vision embeddings, and dynamic scene boundary detection combining temporal intervals and vectors into atomic, first-class records.

---

## Storage Engine Architecture

```
                                  +---------------------------------------+
                                  |         User Application (C++)        |
                                  +-------------------+-------------------+
                                                      |
                                          Put / Get / Delete / Scan
                                                      |
                                                      v
                                        +---------------------------+
                                        |      DBImpl Interface     |
                                        +-------------+-------------+
                                                      |
                         +----------------------------+----------------------------+
                         |                                                         |
                         v                                                         v
              +---------------------+                                   +---------------------+
              |   Write-Ahead Log   |                                   |  SkipList MemTable  |
              |   (32 KiB Blocks)   |                                   |  (Lock-Free Read)   |
              +---------------------+                                   +----------+----------+
                         |                                                         |
                     Recovery                                                    Flush
                         |                                                         |
                         v                                                         v
              +---------------------+                                   +---------------------+
              |    MANIFEST Log     |<==== [VersionEdit Atomic Update] =|  Level 0 SSTables   |
              |    (CURRENT ptr)    |                                   |  (Overlapping Keys) |
              +---------------------+                                   +----------+----------+
                                                                                   |
                                                                          Leveled Compaction
                                                                        (K-Way Merge Iterator)
                                                                                   |
                                                                                   v
                                                                        +---------------------+
                                                                        |  Level 1..6 SSTables|
                                                                        | (Disjoint, Bloom,   |
                                                                        |  Prefix Compressed) |
                                                                        +---------------------+
```

---

## Storage Engine Benchmarks (Milestone M4)

Benchmarks measured with `bench/db_bench.cc` on Windows 11 (UCRT64 GCC 16.1.0 C++17) across 20,000 keys with 100-byte values and 10 bits/key Bloom filters:

| Benchmark Operation | Throughput | Bandwidth | Notes |
| :--- | :--- | :--- | :--- |
| **fillseq** (Sequential Put) | **81,821 ops/sec** | **9.05 MB/sec** | Sequential MemTable & WAL append |
| **readrandom** (Point Get) | **336,414 ops/sec** | **37.22 MB/sec** | 20,000 / 20,000 hits via SkipList + Block Cache |
| **fillrandom** (Random Put) | **47,066 ops/sec** | **5.21 MB/sec** | Random write under background compaction |
| **readmissing** (Bloom Negative Lookup) | **328,295 ops/sec** | **36.32 MB/sec** | **100% negative rejection** (0 disk data block accesses) |

---

## Vector Storage & SIMD Search Benchmarks (Milestone M5)

Benchmarks measured with `bench/vec_bench.cc` across 10,000 512-dimensional vectors (CLIP ViT-B/32 geometry, 19.53 MB raw float32 payload) with append-only `.vec` segment files:

| Benchmark Operation | Measured Throughput | Measured Bandwidth | Latency / Time |
| :--- | :--- | :--- | :--- |
| **vec_ingest** (Put + Sync) | **2,998 vectors/sec** | **5.9 MB/sec** | 3.3 sec total for 10K 512-d vectors |
| **vec_search_top10 (SIMD)** | **20.00 QPS** | — | **50.00 ms/query** (exact brute force across 10K vectors) |
| **vec_segment_scan (CRC)** | **26,006 vectors/sec** | **50.8 MB/sec** | 0.4 sec total scan & Castagnoli CRC verify |

---

## HNSW Proximity Graph Indexing Benchmarks (Milestone M6)

Benchmarks measured with `bench/vec_bench.cc` across 5,000 512-dimensional vectors ($M=16, M_0=32, efConstruction=200, efSearch=64$):

| Benchmark Operation | Measured Throughput | Latency / Time | Speedup vs Exact | Notes |
| :--- | :--- | :--- | :--- | :--- |
| **HNSW Build & Ingest** | **89.3 vectors/sec** | 56.0 sec | — | Builds multi-layer skip-list graph |
| **SIMD Exact Top-10 Baseline** | 22.36 QPS | 44.72 ms/query | 1.0x (baseline) | Exact linear scan ($O(N)$) |
| **HNSW Top-10 Search (ef=64)** | **146.60 QPS** | **6.82 ms/query** | **6.5x speedup** | Logarithmic search ($O(\log N)$) |
| **HNSW Snapshot Cold Open** | **Instant restart** | **1.017 sec** | — | Zero full-graph rebuild on restart |
| **Recall@10 Accuracy** | **> 0.97** | — | — | Surpasses target ($\ge 0.95$) |

---

## Milestone Progress

| Milestone | Title | Focus & Deliverables | Status |
| :---: | :--- | :--- | :---: |
| **M0** | Python Prototype | Frame sampling, CLIP embeddings, cosine similarity, golden set runner | `m0-done` |
| **M1** | Test Harness & Primitives | Micro-test framework, Slice, Status, CRC-32C, Little-Endian coding, logger | `m1-done` |
| **M2** | Durable KV Store | WAL with 32 KiB framing, WriteBatch atomicity, crash recovery, prefix invariance | `m2-done` |
| **M3** | SSTables & MemTable | SkipList, prefix compression, 48-byte footers, sharded LRU cache, MANIFEST | `m3-done` |
| **M4** | Compaction & Bloom Filters | Leveled compaction, Bloom filters, K-way merge, snapshots, write throttling | `m4-done` |
| **M5** | Vector Storage & Search | Append-only `.vec` segment files, WiscKey separation, SIMD flat cosine search | `m5-done` |
| **M6** | Vector Indexing (HNSW) | Multi-layer graph, efConstruction/efSearch, snapshot persistence, over-fetch | `m6-done` |
| **M7** | Metadata & Ingest Pipeline | Embedder interface, ONNX/API adapters, FFmpeg decode, moment detector, jobs | `m7-done` |
| **M8** | Query Layer & C ABI | Text/example search, filters, delete video, stats, stable C ABI | `m8-done` |
| **M9** | Python Client & v1.0 | pybind11 module, wheels for Linux/macOS/Windows, examples, v1.0 release | Planned |

---

## Getting Started

### Prerequisites
- **C++ Compiler**: GCC $\ge 11$, Clang $\ge 14$, or MSVC $\ge 2019$ supporting **C++17**
- **CMake**: Version $\ge 3.16$
- **Ninja**: Fast build system (recommended)

### 1. Build the Engine
```bash
# Clone the repository
git clone https://github.com/Ameya79/PurpleDB.git
cd PurpleDB

# Configure CMake with Ninja
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Build core static library, test suite, and benchmark runner
cmake --build build --config Release
```

### 2. Run the Test Suite
The unit and acceptance test suite includes 136 self-contained tests across all modules and acceptance criteria with zero external dependencies:
```bash
./build/purpledb_tests
```

Filter for specific test suites:
```bash
./build/purpledb_tests M8AcceptanceTest
./build/purpledb_tests QueryEngineTest
./build/purpledb_tests CAbiTest
```

### 3. C ABI Quickstart (C99 / C++17)

```c
#include <purpledb/purpledb_c.h>
#include <stdio.h>

int main() {
    pdb_t* db = NULL;
    pdb_open("./mydb", "local", &db);

    char video_id[128];
    pdb_add_video(db, "vacation.mp4", video_id, sizeof(video_id));

    pdb_results_t* results = NULL;
    pdb_search_text(db, "swimming in turquoise water", 5, NULL, &results);

    int count = pdb_results_count(results);
    for (int i = 0; i < count; ++i) {
        const char* vid = NULL;
        uint32_t moment_id = 0;
        double start_sec = 0.0, end_sec = 0.0;
        float score = 0.0f;
        pdb_results_get(results, i, &vid, &moment_id, &start_sec, &end_sec, &score, NULL, NULL);
        printf("Hit: %s (%.1fs - %.1fs) score=%.3f\n", vid, start_sec, end_sec, score);
    }

    pdb_results_destroy(results);
    pdb_close(db);
    return 0;
}
```

### 4. Run Storage Engine Benchmarks
```bash
./build/db_bench 20000
```

---

## Python Prototype (Milestone M0)

The self-contained Python reference implementation is available in `prototype/`:
```bash
cd prototype
pip install -r requirements.txt

# Run the end-to-end golden set semantic video evaluation
python -m src.golden_set_runner
```

---

## Repository Structure

```
PurpleDB/
├── assets/                  # Logos and visual documentation assets
├── bench/                   # Storage engine benchmark harnesses
│   ├── db_bench.cc          # High-throughput KV benchmark (fillseq, readrandom, fillrandom, readmissing)
│   ├── vec_bench.cc         # Vector benchmark (ingest, SIMD brute-force search, segment scan)
│   └── results/             # Real measured benchmark outputs across milestones
├── core/
│   ├── include/purpledb/    # Public C++ and C ABI headers
│   │   ├── purpledb_c.h     # Stable C ABI header (FFI boundary, opaque handles)
│   │   ├── db.h             # Core DB interface (Put, Get, Delete, NewIterator, GetSnapshot)
│   │   ├── filter_policy.h  # Bloom filter policy abstraction
│   │   ├── iterator.h       # Iterator interface
│   │   ├── slice.h          # Zero-copy string/binary slice view
│   │   ├── status.h         # Memory-optimized status results
│   │   └── write_batch.h    # Atomic multi-mutation batches
│   ├── query/               # Multimodal query engine
│   │   ├── query_engine.cc/h # Multimodal search, metadata hydration, and cascade deletes
│   │   └── moment_metadata.h # Persistent moment descriptor serialization
│   ├── purpledb_c.cc        # Stable C ABI implementation with zero-exception shield
│   ├── storage/             # Engine internals
│   │   ├── block.cc/h       # SSTable data block reader
│   │   ├── block_builder.cc/h # Prefix delta compression block builder
│   │   ├── cache.cc/h       # 16-way sharded LRU block cache
│   │   ├── compaction.cc/h  # Leveled compaction picker and executor
│   │   ├── db_impl.cc/h     # Main database implementation & background worker thread
│   │   ├── db_iter.cc/h     # Snapshot visibility and tombstone-skipping iterator
│   │   ├── env.cc/h         # OS abstraction layer (POSIX / Win32 files, locks, clocks)
│   │   ├── filter_block.cc/h # 2 KiB block-framed Bloom filter builder & reader
│   │   ├── filter_policy.cc # Kirsch-Mitzenmacher double-hashing Bloom implementation
│   │   ├── memtable.cc/h    # MemTable wrapper around SkipList
│   │   ├── merger.cc/h      # K-way MergingIterator across SSTable levels
│   │   ├── skiplist.h       # Arena-backed lock-free reader SkipList
│   │   ├── table.cc/h       # SSTable reader with index block & Bloom integration
│   │   ├── table_builder.cc/h # SSTable serializer with metaindex and footer
│   │   ├── version_set.cc/h # MANIFEST replay, VersionEdit, and active file tracking
│   │   └── wal_writer.cc/h  # 32 KiB block-framed Write-Ahead Log writer
│   ├── vector/              # Vector engine internals (WiscKey Architecture)
│   │   ├── delete_bitmap.cc/h # O(1) in-memory tombstone bitset
│   │   ├── distance.cc/h    # SIMD AVX2/NEON dot product, cosine, and int8 quantization
│   │   ├── hnsw.cc/h        # Hierarchical Navigable Small World proximity graph
│   │   ├── vector_index.cc/h # Unified LSM KV metadata and vector store
│   │   └── vector_segment.cc/h # Append-only .vec segment writer & reader
│   ├── embed/               # Multimodal embedding adapters
│   │   ├── embedder.h       # Abstract Embedder & ImageRef interface
│   │   ├── api_adapter.cc/h # Cloud API adapter with jittered backoff & rate limiting
│   │   └── local_embedder.cc/h # Local inference & semantic projection adapter
│   ├── pipeline/            # Automated video ingestion pipeline
│   │   ├── frame_decoder.cc/h # Subprocess FFmpeg frame extraction & downscaling
│   │   ├── moment_detector.cc/h # Anchor-based moment clustering & medoids
│   │   └── ingest_job.cc/h  # Resumable atomic batch ingest & LSM job state
│   └── util/                # Core utilities
│       ├── arena.cc/h       # 4 KiB bump-pointer memory allocator
│       ├── coding.cc/h      # Little-Endian Fixed32/64 and Varint32/64 encoders
│       ├── crc32c.cc/h      # Castagnoli CRC-32C checksum implementation
│       ├── hash.cc/h        # MurmurHash2 implementation
│       └── sha256.cc/h      # FIPS 180-4 SHA-256 content addressing
├── docs/                    # Technical architecture & versioned specifications
│   ├── PRD.md               # Product Requirements Document & milestone specifications
│   ├── DESIGN.md            # In-depth architectural designs & educational concepts
│   ├── FORMATS.md           # Authoritative on-disk versioned binary wire formats
│   ├── DECISIONS.md         # Architectural Decision Records (ADRs) & benchmark logs
│   └── LIMITATIONS.md       # Operational boundaries and concurrency constraints
├── prototype/               # Milestone M0 Python prototype
└── tests/                   # Micro test suite (136 unit & acceptance tests)
```

---

## Documentation & References

- [Product Requirements Document (PRD)](docs/PRD.md)
- [System Architecture & Design Rationale](docs/DESIGN.md)
- [Versioned On-Disk Binary Formats](docs/FORMATS.md)
- [Architectural Decision Records (ADR)](docs/DECISIONS.md)
- [Release Gate 1 Engine Limitations](docs/LIMITATIONS.md)

---

## License

PurpleDB is open-source software licensed under the [MIT License](LICENSE).
