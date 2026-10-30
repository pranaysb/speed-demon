# Track B: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 24.50 | 185 | 0 |
| 1 | C++ Rewrite: Inverted index with two-pointer sorted set operations, zero-copy `mmap` DocSpans, multi-threaded query execution | local | 0.27 | 45 | |

## Approach & Architectural Highlights
1. **Zero-Copy Document Slicing**: Documents in `docs.txt` are mapped into memory; documents are referenced via `{ptr, len}` spans without heap allocations.
2. **Dense Natural Postings Lists**: Documents are tokenized in increasing document ID order. Document IDs are pushed directly to postings lists without duplicate checks or sorting steps.
3. **Linear Two-Pointer Boolean Evaluator**: `AND`, `OR`, and `NOT` are evaluated strictly left-to-right via two-pointer sorted list operations ($O(N + M)$). All intermediate results remain sorted.
4. **Fast Phrase Candidate Intersect & Sequence Scan**: Phrase queries intersect candidate term lists before checking token sequences in matched documents.
5. **Multi-Threaded Work Partitioning**: All queries are independent; partitioned evenly across hardware threads with thread-local buffers.
