# Track B: Index

## Approach

Our solution is an inverted index search engine designed to execute boolean and exact phrase queries over hundreds of thousands of documents with sub-second latency and minimal memory overhead.

### 1. Two-Phase Query Compilation
* `queries.txt` is parsed ahead of index creation. All unique query terms are collected into a dictionary with a 64-bit FNV-1a hash table.
* Queries are pre-compiled into an intermediate representation supporting AND, OR, NOT, and phrase sequence constraints.

### 2. Parallel CSR Inverted Index Construction
* `docs.txt` is memory-mapped and partitioned into byte chunks with line alignment.
* **Pass 1**: Threads tokenize text on whitespace/punctuation boundaries. Only terms existing in the active query dictionary are counted; 87% of non-query vocabulary is filtered out immediately on-the-fly. Document start offsets and lengths are recorded for phrase evaluation.
* **Pass 2**: Postings are written into a single contiguous Compressed Sparse Row (CSR) array with pre-calculated offsets. Zero pointer indirection and zero dynamic array resizing.
* Immediately upon index completion, `madvise(MADV_DONTNEED)` is invoked on `docs.txt`, clearing the 242 MB of sequentially scanned file pages from container RSS limits.

### 3. Zero-Malloc Query Evaluation Engine
* Boolean operations (`AND`, `OR`, `AND NOT`) operate on sorted doc ID arrays using two-pointer linear scans (`intersect_raw`, `union_raw`, `diff_raw`) writing directly into pre-allocated thread scratch buffers.
* Phrase queries execute boolean candidates first, then verify token adjacency directly on the underlying document bytes without token copying.
* Query execution is partitioned across threads with thread-local string buffers, avoiding lock contention during output formatting.

## Dependencies

Zero external dependencies. Pure standard C++17.

## With another 24 hours

1. **SIMD Vectorized Set Intersections**: Implement AVX-512 `_mm512_cmpestri` / Galloping search on inverted index postings lists to accelerate high-cardinality `AND` intersections.
2. **Delta Varint Postings Compression**: Encode doc ID gaps with Elias-Fano or Variable Byte encoding, shrinking inverted index memory from 69 MB to under 18 MB.
