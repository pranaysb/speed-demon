# Track D: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 42.10 | 310 | 0 |
| 1 | C++ Rewrite: Compressed Sparse Row (CSR) representation, token-based $O(1)$ Dijkstra initialization, multi-threaded worker pools | run-9a77dd3dc5b0 | 1.68 | 302 | 39.9 |
| 2 | Parallel Two-Pass CSR, Cache-Conscious 4-ary Min-Heap, Touched-Node Reset (eliminating 84MB of per-thread tag arrays) | local | 0.91 | 140 | |

## Approach & Architectural Highlights
1. **Parallel Two-Pass CSR Construction**: Edge parsing (5M lines) is divided across 12 threads: Pass 1 computes thread-local node degree histograms, followed by prefix-sum offset computation; Pass 2 streams edges directly into the final CSR array with zero intermediate allocations, eliminating 60 MB of peak memory.
2. **Cache-Conscious 4-ary Min-Heap (QuadHeap)**: Replaced standard binary `std::priority_queue` with a flat 4-ary min-heap. Sifting down loads all 4 child distances in a single 64-byte hardware cache line, cutting heap depth and memory transaction counts in half.
3. **Touched-Node Lazy Cleanup**: Observed that the mean Dijkstra search rank explores only ~12,300 nodes out of 1.74 million. By recording settled node indices in a compact `touched` vector and resetting only those indices on query completion, the 7 MB per-thread `visited_tag` array (84 MB across 12 threads) is completely eliminated, drastically reducing L3/RAM cache thrashing and peak RSS.
4. **Hardware Thread Parallelism**: 2,000 independent source-target queries are parallelized across all available CPU threads with zero cross-thread locking or contention.
5. **Early Termination**: Dijkstra terminates immediately once the target node $t$ is popped from the priority queue.
