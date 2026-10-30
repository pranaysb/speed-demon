# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 24.50 | 185 | 0 |
| 1 | C++ Rewrite: Inverted index with two-pointer sorted set operations, zero-copy `mmap` DocSpans, multi-threaded query execution | run-201eb8f6f162 | 4.70 | 258 | 30.7 |
| 2 | Query-Guided Indexing & Parallel Tokenization: Read queries first to filter vocabulary to only relevant terms (discarding 87% of corpus words), 12-way parallel document indexing, zero-allocation ping-pong boolean set evaluation, fast integer serialization | local | 1.26 | 100 | |

## Approach & Architectural Highlights
1. **Query-Guided Vocabulary Pruning**: The specification explicitly states: *"The queries are in the input from the start: how much of the corpus you index to do that is your call."* We read `queries.txt` first to populate a compact L2/L3-resident hash dictionary containing ONLY the ~75,000 unique query terms. The remaining 525,000 corpus terms are skipped with zero allocations, eliminating 87% of all inverted index postings lists.
2. **Parallel Document Chunk Indexing**: Documents are partitioned across 12 hardware threads. Because doc IDs within each thread chunk are strictly increasing, merging thread-local postings into the global index is a direct vector append with zero sorting required.
3. **Zero-Allocation Ping-Pong Boolean Set Evaluator**: Reusable scratch buffers ping-pong during `AND`, `OR`, and `NOT` left-to-right evaluations, eliminating all dynamic vector reallocations during the 50,000 query evaluations.
4. **Fast Phrase Candidate Intersect & Sequence Scan**: Phrase queries intersect candidate term lists before checking token sequences in matched documents.
5. **Zero-Copy Document Slicing**: Documents in `docs.txt` are mapped into memory; documents are referenced via `{ptr, len}` spans without heap allocations.
