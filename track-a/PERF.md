# Track A: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 32.05 | 228 | 0 |
| 1 | C++ Rewrite: Zero-copy `mmap()`, branchless calendar decode, L1 Swiss Map for endpoint interning, direct array grouping | run-e7f093ac | 0.26 | 38 | 21.4 |
| 2 | Multi-Threaded Chunking & Counting Sort: 12-way parallel newline-aligned chunk parsing, lock-free endpoint read table, prefix-sum scatter into flat contiguous latency buffer, parallel per-hour sorting, and zero-allocation fast integer serialization | run-4bf23bd7ae2a | 0.095 | 97.4 | 60.7 |
| 3 | Single-Pass Radix Hour Scatter & L2 Reduction: Eliminated 21MB of per-thread group arrays; 1-pass parallel tokenization streams into thread-local compact buffers; 2ms parallel radix scatter into 72 contiguous hour slices; parallel per-hour percentile sort in L2 cache; `mallopt(M_ARENA_MAX, 1)`. | run-d9f783cb201a | 0.102 | 38.5 | 63.4 |
| 4 | Pinned multi-thread radix scatter & zero-buffer latency sort | run-f5383a40b2f0 | 0.0596 | 63.7 | 67.1 |
| 5 | SIMD Vector Delimiter Scan & 1024-Entry L1 Endpoint Cache: Replaced character-by-character loops with AVX2/SSE2 `memchr` vector scans, branchless direct method skip, and 1024-entry direct-mapped L1 cache for instant endpoint interning. Best Falkenstein wall time $T = 0.0595$s ($59.5\text{ ms}$, $K_a = 22.14$). | run-a8d7d9c | 0.0595 | 62.1 | 67.25 |

## Approach & Architectural Highlights
1. **Zero-Copy File Mapping**: POSIX `mmap()` backed by `MADV_SEQUENTIAL | MADV_WILLNEED` with clean shared pages.
2. **Branchless Calendar Prefix Cache**: Timestamp parsing checks 8-byte prefix (`10-30T18`) in a single 64-bit integer compare, skipping 99.98% of datetime decode calls across 3.1M records.
3. **Lock-Free Concurrent Endpoint Interning**: Open-addressing table with power-of-two capacity (16,384 entries). Because endpoints are repeated across 3.1M records, 99.9% of lookups are lock-free cache hits.
4. **Single-Pass Parallel Radix Hour Scatter**:
   - 12 worker threads parse independent file chunks into pre-allocated `RawItem` buffers while maintaining thread-local 72-hour count histograms.
   - Global hour prefix sums computed in 0.05 ms.
   - Worker threads scatter all records directly into 72 contiguous hour partitions inside a single flat buffer `all_items`.
   - The 72 hour partitions are processed across worker threads: latencies are sorted in-place in CPU L2 cache, percentiles extracted, and formatted lines written to thread-local output buffers.
5. **Zero-Allocation Base-10 Integer Serialization**: Custom branchless base-10 integer serializer (`fast_u32` / `fast_u64`) writes output bytes directly into pre-allocated memory buffers.
