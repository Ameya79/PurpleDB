# PurpleDB Release Gate 1: Embedded KV Engine Limitations

This document details the known operational constraints, concurrency boundaries, and resource limitations of the PurpleDB embedded key-value storage engine as of Milestone M4 (Release Gate 1).

---

## 1. Concurrency Boundaries

### 1.1 Single-Process Access
- **Constraint**: Like SQLite and LevelDB, PurpleDB is an **embedded** database engine designed to be opened within a single operating system process.
- **Limitation**: Multiple operating system processes must NOT attempt to open the same database directory concurrently. Concurrent writes across processes will corrupt the Write-Ahead Log (`.log`) and Manifest files.
- **Protection**: A future milestone (M7) will introduce OS-level advisory file locks (`LOCK`) to prevent concurrent process instances from opening the same database folder simultaneously.

### 1.2 Thread Concurrency Model
- **Readers**: Multiple reader threads can execute point lookups (`Get`) and range scans (`NewIterator`) concurrently.
- **Writers**: Mutation operations (`Put`, `Delete`, `Write`) acquire an internal mutex (`mutex_`). Single-writer concurrency is enforced to serialize WAL appends and MemTable updates.
- **Background Worker**: A single dedicated background thread handles asynchronous MemTable flushing and leveled compaction without blocking foreground reads.

---

## 2. Resource & Memory Bounds

### 2.1 File Descriptor Usage
- **Limitation**: Each opened SSTable (`.sst`) currently retains an open file descriptor (`RandomAccessFile`).
- **Mitigation**: Levels 1 to 6 chunk data into 2 MiB files. For large datasets ($\ge 100\text{ GB}$), an operating system with low file descriptor limits (e.g. `ulimit -n 1024`) could experience `EMFILE` ("Too many open files").
- **Future Roadmap**: A Table Cache (pooling open `Table` instances with an LRU file descriptor eviction policy) will be implemented as dataset volume scales.

### 2.2 Memory Bounds
- **MemTable Footprint**: Governed by `Options::write_buffer_size` (default: 4 MiB). Peak transient memory usage during high-throughput ingestion can reach $2\times$ `write_buffer_size` when an active MemTable and immutable MemTable coexist during flush.
- **Block Cache Footprint**: Bounded by `Options::block_cache` capacity (default: 8 MiB).
- **Index & Filter Blocks**: Loaded on-demand and retained by active table instances. For 1 million 100-byte keys with 10 bits/key Bloom filters, total filter memory is $\approx 1.25\text{ MB}$.

---

## 3. Storage & Compaction Bounds

### 3.1 Write Amplification vs Compaction Latency
- Leveled compaction targets a $10\times$ capacity growth per level (L1: 64 MB, L2: 640 MB, L3: 6.4 GB).
- Write amplification in leveled compaction ranges from $10\times$ to $25\times$. For large write-heavy workloads, compaction I/O will compete with disk bandwidth unless write throttling (soft slowdown at 8 L0 files, hard stall at 12 L0 files) regulates incoming throughput.

### 3.2 Tombstone Garbage Collection
- Deletion tombstones cannot be reclaimed immediately; they must migrate down through the leveled hierarchy until no older versions exist at deeper levels and no active user snapshots cover the tombstone's sequence number.
- In workloads with continuous unreleased snapshots (`GetSnapshot()` without `ReleaseSnapshot()`), garbage collection of overwritten versions and tombstones is held back, increasing storage footprint.

---

## 4. Key and Value Size Limits
- **Maximum Key Size**: Recommended $\le 4\text{ KiB}$ (enforced by Varint length prefixing). Keys larger than block size will degrade prefix compression and block search performance.
- **Maximum Value Size**: Suitable for arbitrary binary blobs up to 10 MiB in the KV engine. However, high-dimensional vector embeddings ($\ge 2\text{ KiB}$) will bypass SSTables starting in Milestone M5 via append-only vector segment files (WiscKey KV separation pattern).

