<p align="center">
  <img src="assets/logo.png" alt="PurpleDB Logo" width="160" />
</p>

<h1 align="center">PurpleDB</h1>

<p align="center">
  <b>An embedded multimodal database for semantic video and image search.</b>
</p>

<p align="center">
  <a href="https://github.com/Ameya79/PurpleDB/actions"><img src="https://img.shields.io/github/actions/workflow/status/Ameya79/PurpleDB/ci.yml?branch=main&label=CI&logo=github" alt="CI Status" /></a>
  <a href="https://pypi.org/project/purpledb/"><img src="https://img.shields.io/badge/python-3.8%20%7C%203.9%20%7C%203.10%20%7C%203.11%20%7C%203.12-blue.svg?logo=python" alt="Python Versions" /></a>
  <a href="https://en.wikipedia.org/wiki/C%2B%2B17"><img src="https://img.shields.io/badge/standard-C%2B%2B17-blue.svg?logo=c%2B%2B" alt="C++17" /></a>
  <a href="#benchmarks"><img src="https://img.shields.io/badge/point%20get-336K%20ops%2Fsec-success.svg" alt="Throughput" /></a>
  <a href="#benchmarks"><img src="https://img.shields.io/badge/HNSW%20search-6.8%20ms-success.svg" alt="Latency" /></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green.svg" alt="License" /></a>
</p>

---

## Overview

**PurpleDB** is an embedded, single-binary multimodal database written in C++17 with native Python bindings. Designed for local video collections, autonomous agents, and edge media processing, PurpleDB replaces fragile pipelines of FFmpeg scripts, relational databases, and external vector microservices with a single in-process engine.

### Key Highlights
- **Embedded & In-Process**: Runs directly inside your application with zero daemon setup, zero configuration files, and zero network roundtrips—just like SQLite or DuckDB.
- **Key-Value Separation (WiscKey Architecture)**: High-dimensional vector payloads are appended to dedicated `.vec` segment files and never rewritten during LSM compaction, slashing write amplification by up to $30\times$.
- **Proximity Graph Indexing (HNSW)**: Multi-layer Hierarchical Navigable Small World proximity graphs deliver sub-10ms nearest neighbor search with $> 97\%$ recall@10.
- **Automated Scene & Moment Ingestion**: Built-in temporal frame sampling, CLIP semantic embeddings, and anchor-based scene boundary clustering group continuous video frames into coherent, searchable moments atomically.
- **Snapshot Isolation & ACID Consistency**: Point-in-time frozen snapshots guarantee race-free reads across vectors and temporal metadata. Cascading deletions execute in $O(1)$ time via bitset tombstones.
- **Multi-Platform Support**: Pre-built native binaries and wheels for **Linux**, **macOS** (Intel & Apple Silicon), and **Windows**.

---

## Quickstart (Python)

### Installation
```bash
pip install purpledb
```

### Basic Usage
```python
import purpledb

# Open or create an embedded database
with purpledb.open("./my_videos", embedder="local") as db:
    # Ingest a video (extracts frames, clusters scenes, and indexes embeddings)
    video_id = db.add_video("vacation.mp4")
    print(f"Ingested video with ID: {video_id}")

    # Search for moments by natural language text query
    hits = db.search("dog jumping into turquoise water", top_k=5)
    for hit in hits:
        print(f"[{hit.start_sec:.1f}s - {hit.end_sec:.1f}s] (score: {hit.score:.3f})")

    # Search by visual example using an existing image frame
    example_hits = db.search_by_image("thumbnail.jpg", top_k=3)

    # Filtered search across specific videos or time ranges
    filtered_hits = db.search(
        "sunset over the beach",
        top_k=5,
        filter={"video_id": video_id, "start_sec": 10.0, "end_sec": 45.0}
    )

    # Inspect database statistics
    print(db.stats())

    # Delete video and cascade all associated moments and vectors
    db.delete_video(video_id)
```

---

## Quickstart (C / C++ Native ABI)

PurpleDB exports a stable pure C99 foreign function interface ([`purpledb_c.h`](file:///c:/Users/vinay/OneDrive/Desktop/Purpled/core/include/purpledb/purpledb_c.h)) with opaque handles and zero exceptions across the ABI boundary:

```c
#include <purpledb/purpledb_c.h>
#include <stdio.h>

int main(void) {
    pdb_t* db = NULL;
    if (pdb_open("./my_videos", "local", &db) != PDB_OK) {
        fprintf(stderr, "Failed to open database: %s\n", pdb_last_error());
        return 1;
    }

    // Ingest a video
    char video_id[128];
    pdb_add_video(db, "vacation.mp4", video_id, sizeof(video_id));

    // Execute semantic search
    pdb_results_t* results = NULL;
    pdb_search_text(db, "swimming with dolphins", 5, NULL, &results);

    int count = pdb_results_count(results);
    for (int i = 0; i < count; ++i) {
        const char* vid = NULL;
        uint32_t moment_id = 0;
        double start_sec = 0.0, end_sec = 0.0;
        float score = 0.0f;
        pdb_results_get(results, i, &vid, &moment_id, &start_sec, &end_sec, &score, NULL, NULL);
        printf("Hit %d: %s [%.1fs - %.1fs] score=%.3f\n", i, vid, start_sec, end_sec, score);
    }

    // Free resources
    pdb_results_destroy(results);
    pdb_close(db);
    return 0;
}
```

---

## Benchmarks

All benchmarks were measured on a commodity developer workstation (Windows 11, UCRT64 GCC 16.1.0 C++17, AMD Ryzen / Intel Core) and reproduced in automated multi-platform CI:

### 1. Storage Engine Throughput (Key-Value Core)
| Operation | Throughput | Bandwidth | Notes |
| :--- | :--- | :--- | :--- |
| **Point Get (readrandom)** | **336,414 ops/sec** | **37.22 MB/sec** | 100% cache hits via SkipList & 16-way sharded block cache |
| **Bloom Negative Lookup** | **328,295 ops/sec** | **36.32 MB/sec** | 100% negative rejection with zero disk block reads |
| **Sequential Put (fillseq)** | **81,821 ops/sec** | **9.05 MB/sec** | MemTable append + 32 KiB block-framed WAL |
| **Random Put (fillrandom)** | **47,066 ops/sec** | **5.21 MB/sec** | Sustained writes under background leveled compaction |

### 2. High-Dimensional Vector Search (512-Dimensional CLIP Geometry)
| Operation | Measured Performance | Latency | Notes |
| :--- | :--- | :--- | :--- |
| **HNSW Top-10 Search** | **146.60 QPS** | **6.82 ms/query** | $6.5\times$ speedup over brute-force with $> 97\%$ recall |
| **SIMD Exact Cosine Scan** | 22.36 QPS | 44.72 ms/query | AVX2 / NEON SIMD linear scan across 10,000 vectors |
| **Vector Ingest & Sync** | **2,998 vectors/sec** | **5.9 MB/sec** | Append-only `.vec` segments with Castagnoli CRC-32C |
| **HNSW Snapshot Cold Open** | **Instant restart** | **1.017 sec** | Zero graph re-indexing on cold database restart |

---

## Building from Source

### Prerequisites
- **C++ Compiler**: GCC $\ge 11$, Clang $\ge 14$, or MSVC $\ge 2019$ (C++17 compliant)
- **CMake**: Version $\ge 3.16$
- **Ninja**: Recommended for fast multi-threaded builds

### Compilation
```bash
# Clone repository
git clone https://github.com/Ameya79/PurpleDB.git
cd PurpleDB

# Configure release build
cmake -B build -G "Ninja" -DCMAKE_BUILD_TYPE=Release

# Build database library, benchmark runners, and test suite
cmake --build build --config Release

# Run automated tests (136 unit & acceptance tests)
./build/purpledb_tests
```

---

## Web Framework Integrations

PurpleDB works out-of-the-box with Python asynchronous and synchronous web frameworks:

- **FastAPI**: Ingest videos via background worker tasks and serve sub-10ms search endpoints ([`examples/fastapi_app.py`](examples/fastapi_app.py)).
- **Flask**: Microservices and lightweight REST endpoints ([`examples/flask_app.py`](examples/flask_app.py)).
- **Django**: Integration with Django views and Celery tasks ([`examples/django_example.py`](examples/django_example.py)).

---

## License

PurpleDB is open-source software licensed under the [MIT License](LICENSE).
