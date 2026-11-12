# PurpleDB: PRD, TRD and Build Plan

Version 0.1, 13 Nov 2026. Written to be handed to a coding agent. Every section is a requirement or a decision, not background.

## 0. How to use this document (instructions for the coding agent)

- Build strictly in the milestone order in section 12. Do not start a milestone until the previous one's acceptance tests pass in CI.
- One milestone per pull request series. Every PR must include tests. No untested storage code.
- Language: C++17 for the core, CMake for the build, no Boost. Add a dependency only if the TRD names it or you write a one paragraph justification in `docs/DECISIONS.md`.
- Never guess a number in this document. Numbers marked TARGET are goals to be measured by the benchmark harness, then revised in `docs/DECISIONS.md`.
- On-disk formats are versioned from day one. Any format change bumps the version and needs a migration note.
- Keep a `docs/DESIGN.md` that explains why, not what. Update it in the same PR as the code.
- When something here is ambiguous, pick the simplest option, record it in `docs/DECISIONS.md`, and continue.

## 1. Product requirements (PRD)

### 1.1 Summary

PurpleDB is an embedded, local-first database for searching images and video by meaning. You point it at media, it stores the meaning of each moment as a vector, and you search with plain words (or an image) and get back the exact video and second. It ships as a library (C++ core, Python client first) with no server needed.

### 1.2 Problem

People and teams own large piles of video they cannot search: event recordings, lecture archives, dashcam and camera footage, editor b-roll. Filename and tag search fail. Cloud video search exists but is not acceptable when footage is private, large, or offline. Building it yourself means gluing ffmpeg, a model, a vector index and a store, and getting frame selection, resumability and crash safety right.

### 1.3 Users

- Backend developers (Python first, then Node) who want to add "search my videos by meaning" to an app with a few lines of code.
- Local-first and privacy-sensitive apps that cannot upload media.
- Individual power users and small teams through apps built on the library.

### 1.4 Goals (v1.0)

1. `pip install purpledb`, open a folder, add videos, search by text. Working example in under 10 lines.
2. Fully offline by default using a local embedding model.
3. Optional API mode for better quality when the developer supplies a key.
4. Crash safe: a power cut or kill -9 never corrupts the database or loses acknowledged writes (per the chosen sync policy).
5. Correct and measurable: published benchmarks, recall numbers and a Limitations page.
6. Clean seams so that server mode and multi-node scale-out can be added later without rewriting the core.

### 1.5 Non-goals (v1.0)

- No SQL. No general purpose document database. No joins.
- No distributed consensus or replication in v1.
- No training or fine tuning of models.
- No video player or UI. Results are (video, start, end, score, thumbnail path, caption).
- No GPU kernels written by us. Models run through ONNX Runtime or a remote API.
- No audio or speech search in v1 (listed as future).

### 1.6 Functional requirements

- FR1: Add a video or image file. Videos are split into moments; each moment gets an embedding and optional caption.
- FR2: Search by text query, with `top_k`, optional filters (video id, time range, user metadata), and a score per result.
- FR3: Search by example (an image or a moment id).
- FR4: Delete a video and all its moments. Deletes must eventually reclaim space.
- FR5: Ingest is resumable and idempotent. Re-adding the same file creates no duplicates and a crash mid-ingest resumes where it stopped (no repeat paid API calls).
- FR6: Two embedding modes behind one interface: Local (default) and API.
- FR7: Every vector is tagged with the embedding model id. Opening a database with a different model is refused with a clear error and a documented re-embed command.
- FR8: Stats and introspection: counts, sizes, index status, background job status.
- FR9: Python client in v1.0. Node client and HTTP server in v1.x.

### 1.7 Non-functional requirements and the scalability model

"Extremely scalable at millions of users" is a requirement about architecture. It is handled on three separate axes, because they need different solutions:

- Axis A, adoption scale (millions of installs): solved by being embedded. Each user or app instance has its own database on its own machine, like SQLite. There is no central service to overload. Requirements: single self-contained wheel per platform, no required network, no telemetry by default.
- Axis B, data scale per database: TARGET 10 million moments on one machine. Requires disk based storage, a block cache, quantized vectors and a bounded memory footprint.
- Axis C, many tenants on a server (v2): a hosted service where each customer has their own database. Scale out by sharding on tenant, not on key. Tenants never need cross-shard transactions, so no distributed consensus is needed. Add nodes, place tenants on nodes by consistent hashing, replicate by shipping the WAL.

v1 delivers A and B. C is designed for (see 11) but not built. Do not claim multi-node scale until it is built and benchmarked.

Targets (to be measured, then revised):

- KV engine: within 2x of LevelDB on db\_bench style fillseq, fillrandom, readrandom at equal settings (M3 and M4 benchmark).
- Crash safety: 0 lost acknowledged writes and 0 corruption in a 10,000 iteration kill loop.
- Search at 1M moments, 512 dims, k=10, 4 cores: p95 latency under 50 ms and recall@10 of at least 0.95 versus brute force.
- Memory at 1M moments: under 1.5 GB resident with int8 vectors (raw fp32 vectors alone would be about 2 GB).
- Cold open of a 10M moment database: under 5 seconds (memory mapped, no full load).
- Platforms: Linux, macOS, Windows, x86-64 and arm64.

### 1.8 Success metrics

- Technical: all acceptance tests pass, benchmark numbers published, zero known corruption bugs.
- Adoption (leading indicators): PyPI installs, GitHub stars, number of external issues and PRs, number of apps built on it.
- Quality: recall on a small labelled golden set of moments is tracked per release.

## 2. Architecture overview (TRD)

### 2.1 Layers, bottom up

1. Storage: WAL, memtable, SSTables, manifest, compaction, block cache (the LSM engine).
2. Vector storage and index: append-only vector segments, brute force search, then HNSW.
3. Embedding: the `Embedder` interface with Local ONNX and API adapters.
4. Ingest pipeline: frame extraction, moment selection, batching, resumable jobs.
5. Query: text embed, index search, filter, fetch, rank.
6. API: a stable C ABI, then Python (pybind11) and later Node and HTTP bindings.

### 2.2 Key design decision: separate vectors from the LSM tree

The earlier explainer stored the whole record, including the vector, in the LSM value. That is simple but wasteful: a 512 float vector is 2 KB, and an LSM rewrites values every time they are compacted (write amplification). v1 therefore uses key-value separation (the idea from the WiscKey paper):

- The LSM tree stores small metadata records: video id, start and end ms, caption, thumbnail path, model id, vector location (segment id and offset), flags.
- Vectors live in append-only vector segment files and are never rewritten by LSM compaction.
- The vector index (HNSW) references vectors by dense internal id. A small LSM keyspace maps internal id to moment key and back.

Everything else in the TRD follows from this.

### 2.3 Process and threading model

- One writer thread at a time (a write mutex), many reader threads. Reads never block writes.
- Reads use a snapshot: a sequence number plus a reference counted list of immutable SSTables and the current and immutable memtable.
- One background thread for flush and compaction, one for index maintenance. Thread counts configurable.

## 3. Storage engine (LSM) specification

### 3.1 Keys and sequence numbers

- User key: arbitrary bytes, compared bytewise.
- Internal key: user key + 8 byte tag (sequence number in the high 56 bits, type in the low 8: value or deletion). Internal keys sort by user key ascending, then sequence descending, so the newest version is found first.
- Moment key layout: `m|<video_id 16 bytes>|<start_ms big-endian u64>`. Big-endian so byte order equals numeric order and a range scan returns moments in time order.
- Tenant prefix: reserve a 1 byte keyspace tag at the front of every key (metadata, moments, vector id map, jobs, system). This also gives the tenant sharding seam later.

### 3.2 Write-ahead log

- Record format: `[crc32c u32][length u32][type u8][payload]`, with fragmenting across 32 KiB blocks (LevelDB style) so a torn tail is detectable.
- Group commit: concurrent writers are batched into one append and one fsync.
- Sync policy is a config option: `always`, `interval_ms` (default 100 ms), `none`. Document exactly what each guarantees.
- Recovery: read records until the first bad checksum, ignore the tail, rebuild the memtable.
- A WAL file is deleted only after the memtable it covers is durably flushed and recorded in the manifest.

### 3.3 Memtable

- Skip list with an arena allocator, single writer, lock-free readers (acquire/release atomics).
- Default size 64 MB. When full it becomes immutable, a new memtable and WAL start, and a background flush begins.
- Start with `std::map` behind the same interface for M2, replace with the skip list in M3. The interface does not change.

### 3.4 SSTable format

- Data blocks of 4 KiB (configurable), keys prefix compressed with restart points every 16 entries. Optional compression per block (LZ4 or Snappy, off by default in v1, a TODO behind the interface).
- One checksum (crc32c) per block, verified on read.
- Index block: one entry per data block, key is the last key of the block.
- Bloom filter block: 10 bits per key (about 1 percent false positives), built per file.
- Footer, fixed size: handle to index block, handle to filter block, format version, magic number.
- Files are immutable after creation. Written to a temp name, fsynced, then added to the manifest.

### 3.5 Manifest and versioning

- MANIFEST is an append-only log of version edits (add file at level N with key range, delete file, set next file number, last sequence). The `CURRENT` file names the active manifest and is replaced by atomic rename plus a directory fsync.
- On open: read CURRENT, replay the manifest to rebuild the file set, delete any files not referenced (orphans from crashes), then recover the WAL.
- This replaces the simplified delete-then-rename compaction in the lab page, which is not crash safe.

### 3.6 Compaction

- Leveled compaction. Level 0 holds flushed files that may overlap; compact L0 into L1 when it reaches 4 files. Level N+1 target size is 10x level N (L1 default 64 MB).
- A compaction merges its inputs with a k-way merge iterator, keeps only the newest visible version per key, and writes new files, then installs them with one atomic manifest edit.
- Tombstones are dropped only when the compaction output is at the bottom level, or no older level can contain the key.
- Throttle writes (slowdown, then stop) if L0 grows past thresholds so reads do not degrade without bound.

### 3.7 Read path

1. Memtable, then immutable memtable.
2. L0 files newest to oldest (they may overlap).
3. One file per level for L1 and below, found by binary search on key ranges.
4. In each file: bloom filter, then index block, then one data block through the block cache.

- Block cache: sharded LRU, default 256 MB, configurable.
- Iterators (for scans and compaction) merge all sources and respect the snapshot.

### 3.8 Storage engine public API (internal C++)

```cpp
class KV {
 public:
  static Status Open(const Options&, const std::string& dir, std::unique_ptr<KV>* out);
  Status Put(Slice key, Slice value);
  Status Delete(Slice key);
  Status Write(const WriteBatch&);          // atomic multi-key
  Status Get(Slice key, std::string* value);
  std::unique_ptr<Iterator> NewIterator(const Snapshot* = nullptr);
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot*);
  Status Flush(); Status Compact(Slice begin, Slice end);
};
```

## 4. Vector storage and index specification

### 4.1 Vector segments

- Append-only files of fixed-size vectors with a small header (dim, dtype, model id, format version) and a checksum per chunk.
- Dtypes: fp32 (exact, for the brute force baseline and re-ranking), int8 scalar quantized (default for the index to cut memory 4x). Quantization parameters are stored with the segment.
- Segments are memory mapped read-only once sealed. The active segment is written through the WAL protocol: the LSM record that points at a vector is committed only after the vector bytes are durable.

### 4.2 Search

- M5: brute force with SIMD (AVX2 and NEON via simple intrinsics or compiler auto-vectorization, with a portable fallback). This is the ground truth for recall tests, and it stays in the product as the exact mode and for small databases.
- M6: HNSW. Defaults M=16, efConstruction=200, efSearch=64 (query time parameter). Similarity is cosine (vectors normalized at insert, so dot product).
- Persistence: the graph is serialized as a snapshot file plus a replay of vector ids inserted after the snapshot, so cold open does not rebuild the graph.
- Deletes: a tombstone bitmap hides deleted ids from results. A background rebuild reclaims space when the deleted fraction passes a threshold.
- Filtered search: for filters (video id, time range, metadata) use post-filtering with over-fetch (search `k * overfetch` then filter, widen if too few remain) and an exact scan when the filter is very selective (candidate set small enough to brute force).
- Re-ranking: when the index uses int8 vectors, re-rank the top candidates with fp32 distances if fp32 vectors are kept.
- Future (not v1): disk resident ANN for databases larger than RAM (DiskANN style or IVF-PQ).

### 4.3 Model lock

The database header stores `{model_id, dim, metric, normalize}`. Open with a different embedder fails with a typed error. A `reembed` job rebuilds all vectors under a new model and swaps atomically.

## 5. Embedding layer

```cpp
class Embedder {
 public:
  virtual std::string model_id() const = 0;
  virtual int dim() const = 0;
  virtual int max_batch() const = 0;
  virtual Status EmbedImages(const std::vector<ImageRef>&, Matrix* out) = 0;
  virtual Status EmbedText(const std::string&, Vector* out) = 0;
};
```

- Adapter 1, Local ONNX (default): CLIP or SigLIP exported to ONNX, run with ONNX Runtime's C++ API on CPU, with optional GPU execution providers. Model files downloaded on first use (explicit, with a checksum) or bundled by the app.
- Adapter 2, API, with two sub-modes because providers differ:
  - Embedding API: a provider endpoint that embeds images and text into one shared space (provider specific adapters; implement one first, then add others).
  - Caption mode: any vision capable LLM writes a caption per kept frame, then a text embedding model embeds the captions. This works with any vision LLM, including OpenAI compatible endpoints, but loses visual detail the caption omits. Store the caption either way.
- Honest scope: "supports all LLMs" means any provider that can be put behind this interface and that can read images. It does not mean every LLM works unchanged.
- API client requirements: batching, bounded concurrency, rate limit handling, retry with exponential backoff and jitter, request timeouts, cost counters in stats, API keys read from environment or passed in (never written to the database).
- Privacy: API mode sends frames to a third party. Document it prominently and make it explicit opt-in.

## 6. Ingest pipeline

1. Probe the file (ffprobe) and register the video: stable `video_id` = hash of file content (not the path), so renames do not duplicate.
2. Decode: call the `ffmpeg` binary as a subprocess in v1 (sample 1 frame per second by default, resize to the model's input size). Consider libav bindings later.
3. Select moments: compute embeddings for sampled frames, start a new moment when similarity to the last kept frame drops below a threshold (default 0.90, tunable), merge adjacent similar frames into one moment with start and end times. Keep one representative frame as the thumbnail.
4. Embed in batches sized by `max_batch`.
5. Commit per batch in one atomic WriteBatch: moment metadata records, vector location, index insert queue.
6. Job state in the LSM (`j|<video_id>`: status, last committed timestamp, counts) so a crash resumes after the last committed batch. Keys are deterministic, so replaying a batch is idempotent.

- Backpressure: bounded queues between decode, embed and commit stages so memory stays flat on huge files.
- Images: the same path without frame selection.

## 7. Query path

1. Embed the query with the database's embedder (verify model id).
2. Index search for `k * overfetch` candidates.
3. Apply filters, drop tombstoned ids.
4. Fetch moment records from the LSM tree by key (one snapshot for consistency).
5. Optional re-rank, sort by score, return top `k`.

- Optional LLM handoff stays outside the database: the caller passes captions of the top moments to its own LLM.

## 8. Public API

### 8.1 C ABI (stable, the only thing bindings depend on)

Opaque handles (`pdb_t*`), explicit error codes plus a thread-local last error string, no exceptions across the boundary, all strings UTF-8 with lengths, a `pdb_abi_version()` function. Python, Node, Go and Rust bindings all sit on this.

### 8.2 Python (v1.0)

```python
import purpledb
with purpledb.open("./mydb", embedder="local") as db:
    job = db.add_video("trip.mp4")        # returns a job handle, resumable
    job.wait()
    for h in db.search("dog jumping into water", top_k=5, filter={"video": job.video_id}):
        print(h.video_id, h.start_sec, h.end_sec, h.score, h.caption)
    db.delete_video(job.video_id)
    print(db.stats())
```

- Releases the GIL during long calls. Typed stubs and docstrings included. Wheels built with cibuildwheel for Linux, macOS, Windows.
- Works under Django, Flask, FastAPI, Express and NestJS because those only need the Python or Node client. Heavy calls such as `add_video` must run in a worker or background task, never on an async event loop.

## 9. Repository layout

```
purpledb/
  core/
    include/purpledb/      public C ABI header
    storage/               wal, memtable, sstable, manifest, compaction, cache, bloom, crc32c
    vector/                segment, brute_force, hnsw, quantize
    embed/                 embedder.h, local_onnx, api_adapter
    pipeline/              probe, decode, moments, jobs
    query/                 search, filters
    util/                  status, slice, arena, thread_pool, logging
  bindings/python/         pybind11 module, packaging
  bindings/node/           later
  server/                  later (v2)
  tests/                   unit, model, crash, fuzz, recall, e2e
  bench/                   kv bench, vector bench, e2e bench
  prototype/               Python prototype (M0)
  docs/                    PRD.md TRD.md DESIGN.md DECISIONS.md FORMATS.md LIMITATIONS.md
  CMakeLists.txt
```

## 10. Testing and quality strategy

- Unit tests for every module (GoogleTest or Catch2).
- Model based tests: random sequences of put, delete, get, scan, flush, compact, reopen, compared against `std::map`. Run with many seeds in CI.
- Crash tests: a fault injecting filesystem wrapper that can drop unsynced writes and fail at a chosen operation, plus a loop that kills the process with SIGKILL at random moments, reopens, and verifies against the model. Required to pass 10,000 iterations before the engine is called done.
- Corruption tests: flip random bytes in WAL, SSTables and manifest; the engine must return an error or recover, never crash or return wrong data.
- Fuzzing of the SSTable and WAL readers (libFuzzer).
- Sanitizers in CI: ASan, UBSan for every PR, TSan on the concurrency tests.
- Vector tests: recall@10 versus brute force at several sizes and efSearch values; fail the build if recall drops below the target.
- Retrieval quality: a small labelled golden set (videos plus text queries plus expected timestamps). Report hit rate at k=5 per release. This measures the model and pipeline, not just the database.
- Benchmarks reproducible from one command, results written to `bench/results/`, with environment info recorded.
- CI matrix: Linux, macOS, Windows; x86-64 and arm64 where available.

## 11. Scalability design seams (built in v1, used later)

- Tenant and keyspace prefix on every key, so one database can later hold many tenants or be sharded by tenant.
- Storage behind interfaces (`Env`/filesystem, `Embedder`, `Index`) so object storage or a different index can be swapped in.
- No global mutable state; every database instance is self contained and can run many per process.
- Deterministic keys and idempotent commits so ingest can be distributed across workers.
- Versioned formats with checksums everywhere, so rolling upgrades and replication are possible.
- v2 server mode: a stateless gateway (HTTP or gRPC, auth, quotas), a router that places tenants on nodes with consistent hashing, workers that each own the databases of their tenants, WAL shipping to a replica for failover, object storage for backups. Scale out by adding workers and moving tenants. Large single tenants beyond one machine would need sharded vector indexes, which is v3 and out of scope until real demand exists.

## 12. Build plan, milestone by milestone

Each milestone lists the work, then the acceptance tests. Do not skip ahead.

### M0: Python prototype (about 2 weeks)

- Script: ffmpeg frame sampling, CLIP embeddings (open\_clip), numpy cosine search, a golden set of 5 to 10 of your own videos with expected timestamps.
- Accept: golden set hit rate at k=5 is reported. Decide go or no-go on the idea from real numbers. Tune the similarity threshold and sampling rate here.

### M1: C++ foundations and repo

- CMake project, CI (build plus tests plus sanitizers), `Status`, `Slice`, crc32c, varint and fixed-width encoding helpers, logging, a tiny test harness.
- Accept: CI green on three OSes, encoding round-trip tests pass.

### M2: Minimal durable KV

- WAL (with crc, block fragmenting), `std::map` memtable, put/get/delete, recovery on open, `WriteBatch`.
- Accept: model test against `std::map` with reopen; kill loop of 1,000 iterations passes; truncating the WAL tail at any byte recovers a consistent prefix.

### M3: SSTables and flush

- Skip list memtable, SSTable writer and reader (blocks, index, footer, checksums), manifest and CURRENT, flush on full, read path across memtable and files, iterators, block cache.
- Accept: model test with flush and reopen; corruption tests pass; fuzzers run 10 minutes without crash; first benchmark recorded.

### M4: Compaction and bloom filters

- Bloom filters, k-way merge iterator, leveled compaction on a background thread, write throttling, snapshots, tombstone dropping rules, orphan cleanup on open.
- Accept: model test with random compactions; kill loop of 10,000 iterations including during compaction; space amplification bounded in a delete heavy test; `readrandom` and `fillrandom` within 2x of LevelDB.
- Release gate 1: ship this as a standalone embedded KV library (`purpledb-kv`) with docs, benchmarks and a Limitations page. Write the first blog post here.

### M5: Vector segments and brute force search

- Vector segment format, int8 quantization, brute force SIMD search, vector id to key mapping, delete bitmap.
- Accept: exact results match a numpy reference; crash tests cover vector writes; throughput benchmark recorded.

### M6: HNSW

- HNSW build and search, snapshot persistence, replay of tail inserts, filtered search with over-fetch, background rebuild after deletes.
- Accept: recall@10 of at least 0.95 at 1M vectors versus brute force; p95 query under 50 ms on 4 cores (TARGET, record actual); cold open under 5 s at 10M (memory mapped).

### M7: Embedders and ingest pipeline

- `Embedder` interface, Local ONNX adapter, API adapter (one provider embedding API and caption mode), ffmpeg subprocess decode, moment selection, batching, resumable jobs.
- Accept: golden set hit rate within 5 points of the M0 prototype; kill -9 during ingest resumes without duplicates; API adapter passes retry and rate limit tests against a mock server.

### M8: Query layer and C ABI

- Search (text and by example), filters, delete video, stats; the C ABI header; ABI version test.
- Accept: end to end tests through the C ABI only; concurrent readers during ingest stay correct under TSan.

### M9: Python client and v1.0

- pybind11 module, wheels for three OSes, typed stubs, README with a 10 line example and honest benchmarks, `LIMITATIONS.md`, examples for FastAPI and Flask and Django.
- Accept: clean `pip install` on a fresh machine for each OS, examples run, docs build. Tag v1.0.

### After v1.0

- Node client, HTTP server mode with auth and quotas, compression, disk resident ANN, audio search, replication and tenant placement (section 11).

## 13. Risks

- Search quality is bounded by the embedding model (weak at counting, text in frames, fine actions, and temporal questions). Mitigation: golden set tracking, swappable embedders, caption mode, honest docs.
- Scope: C++ plus LSM plus HNSW plus pipeline is large. Mitigation: release gate after M4, M0 go/no-go, no features outside this document.
- Crash safety bugs are subtle. Mitigation: fault injection and kill loops are mandatory, not optional.
- Existing alternatives (Immich, Frigate, vector stores with ffmpeg glue) are good enough for many users. Mitigation: validate with real users after M0 and M4, and focus on the local video moment niche.
- Memory blowup at 10M moments. Mitigation: int8 vectors, memory mapping, bounded caches, measured at M6.
- Cross platform build pain (Windows especially). Mitigation: CI on all three from M1.

## 14. Open decisions for the owner

- License (Apache-2.0 suggested for adoption).
- Which local model to ship first (CLIP ViT-B/32 is small and fast, SigLIP is generally stronger) and which API provider to support first.
- Default similarity threshold and sampling rate (set from M0 results).
- Whether purpledb-kv ships as its own package at release gate 1.
