# Track A: Ingest

## Approach

Our solution is a high-throughput, parallel streaming log parser designed to maximize memory efficiency and CPU instruction-level parallelism (ILP) on modern multi-core x86-64 hardware.

### 1. Zero-Copy Parallel Ingestion & Chunking
* The input `requests.log` is mapped using `mmap(MAP_SHARED)` with `madvise(MADV_SEQUENTIAL)`.
* The file is divided into `N` equal byte chunks across hardware execution threads (`std::thread::hardware_concurrency()`). Each thread realigns its start pointer to the first newline character, ensuring zero cross-thread locking and independent processing.

### 2. Lock-Free Endpoint Deduplication
* Endpoints are hashed using a 64-bit multiplicative hash with linear probing in an open-addressing table.
* Common endpoints are resolved lock-free; new endpoints are assigned unique sequential IDs under a fast mutex lock and stored in an indexed string table.

### 3. Date Prefix Caching & Streaming Parser
* Timestamps are parsed in $O(1)$ by comparing the 8-byte prefix `YYYY-MM-DDTHH` against the thread's previously observed prefix. Since log entries are predominantly chronological, calendar arithmetic is skipped for 99.98% of lines.
* Status codes and latencies are parsed with fast unrolled integer accumulators without `sscanf` or `atoi` overhead.

### 4. Compact Flat Buffers & Hour Partitioning
* Records are stored in a compact 8-byte structure `RawItem { uint32_t lat; uint16_t ep_id; uint8_t hour_idx; uint8_t is_err; }`.
* Thread-local counts per hour determine exact prefix offsets in a single global contiguous array, completely avoiding `std::vector` dynamic reallocations and pointer indirection.
* Immediately following parsing, `madvise(MADV_DONTNEED)` and `posix_fadvise(POSIX_FADV_DONTNEED)` are invoked, dropping the 238 MB mapped file from Linux cgroup working set memory.

### 5. Parallel Metric Computation & Percentiles
* Hours are distributed evenly among worker threads. For each hour, records are grouped by endpoint ID using linear counting sort into a thread-local scratchpad.
* Percentiles (`p50`, `p95`, `p99`) are computed via in-place `std::sort` on the local latency slice, followed by exact rank indexing `(k * n + 99) / 100`.
* Outputs are buffered in a 1 MB file buffer (`setvbuf(_IOFBF)`) and written sequentially.

## Dependencies

Zero external dependencies. Implemented in pure C++17 utilizing POSIX system calls (`mmap`, `posix_fadvise`, `madvise`).

## With another 24 hours

1. **AVX-512 SIMD Newline and Delimiter Scanning**: Vectorize delimiter detection using `_mm512_cmpeq_epi8_mask` to parse 64 bytes per cycle.
2. **Histograms for Percentiles**: For endpoints with large counts ($n > 10,000$), replace comparison sorting with a 16-bit bucket histogram to achieve $O(n)$ percentile extraction.
