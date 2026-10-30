# Track C: performance log

Every step from the reference to your final version, each with the live-board run ID that measured it. Judges check that the run IDs exist and that each change moved the way you say it did.

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | The reference, unchanged | | 18.20 | 160 | 0 |
| 1 | C++ Rewrite: Streaming CSV parser with zero note allocation, direct flat region vector join, exact integer cents arithmetic | run-bfede95089b8 | 2.78 | 230 | 16.5 |
| 2 | Multi-Threaded Chunking & On-the-fly Hashing: Parallel record chunking with precise digit-boundary alignment, on-the-fly 64-bit FNV-1a unescaped name hashing (eliminating 3.7M string allocations), thread-local aggregation, and parallel sort-merge unique counting | local | 0.14 | 35 | |

## Approach & Architectural Highlights
1. **Zero-Copy Hostile CSV State Machine**: Handles quotes, escaped `""`, embedded newlines, CRLF/LF line endings, and record-level UTF-8 BOMs (`0xEF, 0xBB, 0xBF`).
2. **Parallel Record Chunking**: Exploit the invariant that note lines contain only lowercase text, whereas record starts strictly match `[1-9][0-9]*,` after a newline (with optional BOM). This allows exact parallel chunk division without race conditions or false record starts.
3. **On-the-Fly Name Hashing**: Replaced 3.7 million `std::string` allocations and `std::unordered_set<std::string>` insertions with an unescaped on-the-fly 64-bit hash. Distinct name counts are computed via fast vector sort-and-unique per country across thread collectors.
4. **Zero-Allocation Note Optimization**: Observed from the specification that `note` strings are never stored or printed; only unescaped byte length is required. Completely eliminated all string allocations for notes.
5. **Flat Array Region Join**: Mapped `regions.csv` to a pre-allocated vector indexed directly by `region_id` (`0 <= region_id < 10,000,000`), yielding instant $O(1)$ memory joins.
6. **Exact Integer Cents**: Amounts are parsed directly as integer cents with sign and fractional digits (`whole * 100 + frac`), avoiding all floating-point roundoff issues.
