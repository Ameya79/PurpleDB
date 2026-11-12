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
