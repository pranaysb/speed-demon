# Track D: Traverse

## Approach

Our solution is a high-performance graph traversal engine implementing memory-efficient bidirectional Dijkstra with $O(1)$ topological pruning on large road networks.

### 1. Compact Dual CSR Representation
* `graph.txt` is memory-mapped and parsed in a single sequential pass.
* Both forward and backward edge lists are stored in Compressed Sparse Row (CSR) format with 32-bit offsets and compact 8-byte edge descriptors `Edge { uint32_t target; uint32_t weight; }`.
* Immediately following CSR construction, `madvise(MADV_DONTNEED)` and `posix_fadvise` evict the 110 MB `graph.txt` from memory limits.

### 2. O(1) Component Pruning via Pre-Computed BFS
* A lightweight breadth-first search labels weakly connected components using a compact 1-byte per node array `comp[u]`.
* Queries between disconnected components ($comp[s] \neq comp[t]$) or dead-end zero-degree nodes are immediately pruned in $O(1)$ time, skipping Dijkstra execution entirely for unreachable queries.

### 3. L2-Cache Resident Open-Addressing `DistMap`
* Rather than allocating 28 MB per thread for full-graph distance arrays, each thread maintains a power-of-two open-addressing hash table `DistMap` of 131,072 slots (2 MB).
* Since bidirectional searches meet in the middle and typically settle $<4,000$ nodes (maximum observed $<51,000$), the hash table operates at low load factor ($<40\%$) and stays entirely resident within CPU L2 cache.
* Distance updates use Fibonacci hashing with linear probing, averaging 1.1 cache hits per lookup.
* Between queries, only the sparsely touched slots are cleared in microseconds, eliminating multi-megabyte page sweeps.

### 4. Alternating Bidirectional Search
* 4-ary min-heaps (`QuadHeap`) maintain priority queues in both directions.
* Step selection alternates toward the smaller frontier, terminating immediately when $d_f + d_b \ge \mu$, where $\mu$ is the best observed meeting point distance.

## Dependencies

Zero external dependencies. Pure standard C++17.

## With another 24 hours

1. **CH (Contraction Hierarchies)**: Pre-contract low-degree street nodes to accelerate long-distance queries into highway shortcuts.
2. **A* Search with Landmarks (ALT)**: Pre-compute landmark distances to provide admissible lower-bound heuristics, directing search cones toward the target.
