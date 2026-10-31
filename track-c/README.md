# Track C: Join

## Approach

Our solution is a cache-conscious, parallel relational join and aggregation engine designed to parse millions of malformed and RFC 4180 CSV records with zero allocations.

### 1. Zero-Copy Lookup Table & Direct 3-Letter Hash
* `regions.csv` is memory-mapped. Country codes are indexed into a direct 26x26x26 array mapping 3-letter ISO country codes directly to internal country indices (`0..239`).
* `region_id` mapping is stored in a flat 1 MB lookup vector `region_to_cid`, eliminating hash maps and string comparisons entirely.

### 2. Single-Pass Streaming CSV Parser
* `records.csv` is memory-mapped and split into independent thread chunks with line alignment and UTF-8 BOM awareness.
* Each thread parses records in a single streaming pass:
  * Record IDs and Region IDs are parsed via fast unrolled base-10 accumulators.
  * Names are hashed on-the-fly using 64-bit FNV-1a polynomial multiplication (`0x100000001b3ULL`) handling escaped quotes (`""`) and interior commas without memory allocation.
  * Note byte lengths are computed via delimiter scanning.
  * Currency amounts are parsed as signed 64-bit integer cents without floating-point inaccuracies.

### 3. Parallel Hash Aggregation & Distinct Counting
* Country metrics (count, id sum, amount sum/min/max, byte counts) are accumulated in thread-local dense structures.
* Unique name counts per country are computed by collecting 64-bit name hashes into country-partitioned lists, merged across threads and counted via parallel sorting and `std::unique`.
* Final aggregates are formatted with custom integer-to-string routines and sorted alphabetically by country name in byte order.

## Dependencies

Zero external dependencies. Pure standard C++17.

## With another 24 hours

1. **HyperLogLog or Cuckoo Filter for High-Cardinality Groups**: For countries with >50,000 distinct names, explore SIMD bitset filtering before full sorting.
2. **AVX-512 CSV Quote Vectorization**: Implement VPCLMULQDQ / PCMPEQB carries to vectorize quote toggling across 64-byte blocks.
