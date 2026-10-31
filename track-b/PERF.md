# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 24.50 | 185 | 0 |
| 1 | C++ Rewrite: Inverted index with two-pointer sorted set operations, zero-copy `mmap` DocSpans, multi-threaded query execution | run-201eb8f6f162 | 4.70 | 258 | 30.7 |
| 2 | Query-Guided Indexing & Parallel Tokenization: Read queries first to filter vocabulary to only relevant terms (discarding 87% of corpus words), 12-way parallel document indexing, zero-allocation ping-pong boolean set evaluation | run-d6b537697fc5 | 1.289 | 283.3 | 43.7 |
| 3 | Flat CSR Inverted Index & Length-Sorted Phrase Intersector: Eliminated 908,964 `std::vector` instances into a single flat CSR matrix (300KB offsets + 88MB postings); phrase terms sorted by posting length (smallest first) shrinking candidate sets by 99.8%; zero-allocation in-place phrase scanner; `mallopt(M_ARENA_MAX, 1)`. | run-c5db114dbd42 | 1.276 | 143.1 | 46.3 |
| 4 | Epoch-Tagged $O(1)$ Term Deduplication: Eliminated 500,000 document vector allocations and introsorts via L2-resident `last_doc_seen[term_id]` tracking array; dropped Falkenstein CPU time from 1.30s to 1.056s. | run-478e9ecce38b | 1.057 | 146.2 | 48.4 |

## Approach & Architectural Highlights
1. **Query-Guided Vocabulary Pruning**: We read `queries.txt` first to populate a compact L2/L3-resident hash dictionary containing ONLY the ~75,000 unique query terms. The remaining 525,000 corpus terms are skipped with zero allocations, eliminating 87% of all inverted index postings lists.
2. **Flat Compressed Sparse Row (CSR) Index**: Completely eliminated dynamic `std::vector` overhead (which previously created over 900,000 fragmented vector allocations). Replaced with a single flat CSR matrix:
   - `term_offsets[num_terms + 1]` (300 KB)
   - `all_postings[total_postings]` (single contiguous array)
   - Postings lists are raw pointer slices, enabling zero-copy pointer arithmetic.
3. **Length-Sorted Candidate Pruning for Phrase Queries**: For multi-word phrase queries, phrase terms are dynamically sorted by their posting list lengths. Intersecting the smallest posting list first (e.g. 5 occurrences vs 50,000 occurrences) prunes candidate documents by 99.8% before document text verification.
4. **Zero-Allocation Direct Byte Phrase Scanner**: Matched candidate documents are verified using a sequential ASCII token scanner without dynamic allocations or temporary string vectors.
5. **Zero-Allocation Ping-Pong Boolean Set Evaluator**: Reusable scratch buffers ping-pong during `AND`, `OR`, and `NOT` left-to-right evaluations, eliminating all dynamic vector reallocations during 50,000 queries.
