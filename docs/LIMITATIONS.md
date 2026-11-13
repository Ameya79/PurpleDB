# PurpleDB Operational Limitations & Concurrency Boundaries (v1.0)

This document details the operational constraints, resource bounds, and architectural concurrency boundaries of **PurpleDB v1.0**.

---

## 1. Concurrency Boundaries

### 1.1 Single-Process Embedded Architecture
- **Constraint**: Like SQLite, DuckDB, and RocksDB, PurpleDB is an **embedded** database engine designed to run inside the host application's address space.
- **Cross-Process Access**: Multiple operating system processes must **not** attempt to open the same database directory concurrently. Concurrent writes across multiple processes will corrupt the Write-Ahead Log (`.log`) and Manifest files.
- **Multi-Process Deployment Pattern**: For multi-process architectures (e.g. distributed web clusters or Gunicorn workers), run PurpleDB within a dedicated single-process microservice (such as the provided [FastAPI example](../examples/fastapi_app.py)) that exposes REST/gRPC endpoints to other processes.

### 1.2 Thread Concurrency Model
- **Concurrent Readers**: Readers are 100% concurrent and lock-free across MemTables, SSTable block caches, and append-only vector segments. Multiple worker threads can query simultaneously with zero contention.
- **Single-Writer Serialization**: Mutation operations (`AddVideo`, `AddFrames`, `DeleteVideo`, `Put`, `Delete`) serialize through an internal write mutex to guarantee strict Write-Ahead Log sequencing and deterministic LSM state transitions.
- **Background Worker**: A dedicated background thread executes MemTable flushes, leveled compactions, and segment vacuuming asynchronously without stalling concurrent read operations.
- **Python GIL Release (ADR-036)**: All Python client methods (`add_video`, `search`, `search_by_image`, `delete_video`, `sync`, `close`) release the Python Global Interpreter Lock (GIL) via `py::gil_scoped_release`. Asynchronous frameworks (FastAPI / `asyncio`) and multi-threaded worker pools (Celery, Flask, Django) query and ingest without blocking Python event loops.

---

## 2. Resource & Memory Bounds

### 2.1 Memory Footprint & Tuning
- **MemTable Footprint**: Governed by `Options::write_buffer_size` (default: 4 MiB). Peak transient memory usage during high-throughput ingestion is bounded by $2\times$ `write_buffer_size` during active flush cycles.
- **Block Cache Footprint**: Bounded by `Options::block_cache_size` (default: 8 MiB) using 16-way sharded LRU caching.
- **Vector Segment Memory**: Append-only `.vec` segment files are read using positioned file I/O (`RandomAccessFile::Read`). Only actively traversed vector embeddings are loaded into memory, avoiding monolithic memory bloat.
- **HNSW Graph Overhead**: For an index with $N$ vectors and link parameter $M=16$, the in-memory graph index occupies $\approx 1.2\text{ KB}$ per vector. A collection of 100,000 video moments requires $\approx 120\text{ MB}$ of graph RAM.
- **Scalar Quantization (int8)**: When int8 quantization is enabled, high-dimensional vector representations are compressed by $4\times$ (512 bytes vs 2,048 bytes for 512-dim CLIP vectors) with $< 1.5\%$ recall loss.

### 2.2 Recommended Scale Limits
- **Optimal Collection Size**: Up to 10,000,000 indexed moments per single database directory on standard NVMe SSDs.
- **Key & Value Limits**:
  - Maximum metadata key length: 4 KiB.
  - Maximum metadata value length: 10 MiB.
  - Vectors bypass the LSM tree entirely via the WiscKey KV separation pattern (ADR-021), eliminating write amplification.

---

## 3. Media Ingestion & Codec Boundaries

### 3.1 Subprocess FFmpeg Isolation (ADR-031)
- Video frame decoding is executed within an isolated `ffmpeg` child process. Untrusted, malformed, or hostile video bitstreams cannot trigger memory safety faults in the database host process.
- **Dependency**: Ingestion requires the `ffmpeg` binary to be available in the system `PATH` or configured via `FrameDecoderOptions::ffmpeg_binary`. If `ffmpeg` is absent, synthetic frame ingestion (`AddFrames` / `add_synthetic_video`) remains fully operational for offline testing and synthetic pipelines.

### 3.2 Supported Formats
- Compatible with all video containers and codecs supported by FFmpeg, including MP4 (H.264 / HEVC / AV1), MKV, WebM (VP8 / VP9), MOV, and AVI.
