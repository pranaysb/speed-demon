#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#if defined(__linux__)
#include <malloc.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

static inline const uint8_t* find_char_simd(const uint8_t *p, const uint8_t *end, uint8_t target) {
#if defined(__AVX2__)
    __m256i q256 = _mm256_set1_epi8(target);
    while (p + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)p);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, q256));
        if (mask != 0) {
            return p + __builtin_ctz(mask);
        }
        p += 32;
    }
#endif
#if defined(__x86_64__) || defined(_M_X64)
    __m128i q128 = _mm_set1_epi8(target);
    while (p + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)p);
        uint32_t mask = _mm_movemask_epi8(_mm_cmpeq_epi8(v, q128));
        if (mask != 0) {
            return p + __builtin_ctz(mask);
        }
        p += 16;
    }
#elif defined(__ARM_NEON)
    uint8x16_t q = vdupq_n_u8(target);
    while (p + 16 <= end) {
        uint8x16_t v = vld1q_u8(p);
        uint8x16_t cmp = vceqq_u8(v, q);
        uint64_t lo = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
        uint64_t hi = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
        if (lo) return p + (__builtin_ctzll(lo) >> 3);
        if (hi) return p + 8 + (__builtin_ctzll(hi) >> 3);
        p += 16;
    }
#endif
    while (p < end && *p != target) p++;
    return p;
}

static inline char* format_cents(char *buf, int64_t cents) {
    if (cents < 0) {
        *buf++ = '-';
        cents = -cents;
    }
    uint64_t whole = (uint64_t)(cents / 100);
    uint32_t frac = (uint32_t)(cents % 100);

    if (whole == 0) {
        *buf++ = '0';
    } else {
        char tmp[24];
        int ti = 0;
        while (whole > 0) {
            tmp[ti++] = (whole % 10) + '0';
            whole /= 10;
        }
        while (ti > 0) *buf++ = tmp[--ti];
    }
    *buf++ = '.';
    *buf++ = (frac / 10) + '0';
    *buf++ = (frac % 10) + '0';
    return buf;
}

static inline char* fast_u64(char *p, uint64_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[24];
    int i = 0;
    while (v > 0) {
        buf[i++] = (v % 10) + '0';
        v /= 10;
    }
    while (i > 0) *p++ = buf[--i];
    return p;
}

struct ThreadCountryAgg {
    uint64_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    bool has_amount = false;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

struct FinalCountryAgg {
    std::string key;
    uint64_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    uint64_t names_distinct = 0;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

// Global lock-free hash table to assign dense name_id (0..262,143)
static constexpr size_t NAME_MAP_CAP = 524288; // 512K slots
static constexpr size_t NAME_MAP_MASK = NAME_MAP_CAP - 1;

static std::atomic<uint64_t> g_name_map_keys[NAME_MAP_CAP];
static std::atomic<uint32_t> g_name_map_vals[NAME_MAP_CAP];
static std::atomic<uint32_t> g_next_name_id{1}; // 1-based

static inline uint32_t get_or_insert_name_id(uint64_t h) {
    if (__builtin_expect(h == 0, 0)) h = 0x517cc1b727220a95ULL;
    size_t idx = h & NAME_MAP_MASK;
    while (true) {
        uint64_t cur = g_name_map_keys[idx].load(std::memory_order_relaxed);
        if (cur == h) {
            uint32_t v;
            while ((v = g_name_map_vals[idx].load(std::memory_order_acquire)) == 0) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#endif
            }
            return v;
        }
        if (cur == 0) {
            uint64_t expected = 0;
            if (g_name_map_keys[idx].compare_exchange_strong(expected, h, std::memory_order_relaxed)) {
                uint32_t nid = g_next_name_id.fetch_add(1, std::memory_order_relaxed);
                g_name_map_vals[idx].store(nid, std::memory_order_release);
                return nid;
            }
            if (expected == h) {
                uint32_t v;
                while ((v = g_name_map_vals[idx].load(std::memory_order_acquire)) == 0) {
#if defined(__x86_64__) || defined(_M_X64)
                    _mm_pause();
#endif
                }
                return v;
            }
        }
        idx = (idx + 1) & NAME_MAP_MASK;
    }
}

// Country bitset: 131,072 bits = 2048 uint64_t words per country (covers 100,520 distinct names)
static constexpr size_t BITSET_WORDS = 2048;
static std::atomic<uint64_t> g_country_bitsets[256][BITSET_WORDS];

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif

    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        }
    }

    // 1. Load regions.csv
    char regions_path[1024];
    snprintf(regions_path, sizeof(regions_path), "%s/regions.csv", data_dir);
    int rfd = open(regions_path, O_RDONLY);
    if (rfd < 0) { perror("open regions"); return 1; }
    struct stat rst; fstat(rfd, &rst);
    size_t rsize = rst.st_size;
    const uint8_t *rdata = (const uint8_t *)mmap(NULL, rsize, PROT_READ, MAP_SHARED, rfd, 0);
    close(rfd);

    std::vector<std::string> country_names;
    country_names.push_back("_UNMATCHED"); // index 0

    std::vector<uint16_t> region_to_cid(1000005, 0);
    uint16_t code3_to_cid[26 * 26 * 26];
    memset(code3_to_cid, 0, sizeof(code3_to_cid));

    auto get_or_add_code3 = [&](const uint8_t *p, size_t len) -> uint16_t {
        if (len == 3 && p[0] >= 'A' && p[0] <= 'Z' &&
                        p[1] >= 'A' && p[1] <= 'Z' &&
                        p[2] >= 'A' && p[2] <= 'Z') {
            uint32_t idx = (p[0] - 'A') * 676 + (p[1] - 'A') * 26 + (p[2] - 'A');
            if (code3_to_cid[idx] != 0) return code3_to_cid[idx];
            uint16_t cid = (uint16_t)country_names.size();
            country_names.push_back(std::string((const char *)p, len));
            code3_to_cid[idx] = cid;
            return cid;
        }
        for (size_t c = 0; c < country_names.size(); c++) {
            if (country_names[c].size() == len && memcmp(country_names[c].data(), p, len) == 0) return (uint16_t)c;
        }
        uint16_t cid = (uint16_t)country_names.size();
        country_names.push_back(std::string((const char *)p, len));
        return cid;
    };

    const uint8_t *rp = rdata;
    const uint8_t *rend = rdata + rsize;
    while (rp < rend && *rp != '\n') rp++;
    if (rp < rend) rp++;

    while (rp < rend) {
        uint32_t reg_id = 0;
        while (rp < rend && *rp >= '0' && *rp <= '9') {
            reg_id = reg_id * 10 + (*rp - '0');
            rp++;
        }
        if (rp < rend && *rp == ',') rp++;
        const uint8_t *cs = rp;
        while (rp < rend && *rp != '\r' && *rp != '\n') rp++;
        size_t clen = rp - cs;
        if (rp < rend && *rp == '\r') rp++;
        if (rp < rend && *rp == '\n') rp++;
        uint16_t cid = get_or_add_code3(cs, clen);
        if (reg_id < region_to_cid.size()) region_to_cid[reg_id] = cid;
    }
    munmap((void *)rdata, rsize);

    size_t num_countries = country_names.size();

    // 2. Map records.csv
    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);

    int fd = open(records_path, O_RDONLY);
    if (fd < 0) { perror("open records"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);

    for (unsigned int i = 0; i < num_threads; i++) {
        size_t s = (i * file_size) / num_threads;
        if (i == 0) {
            const uint8_t *p = data;
            while (p < data + file_size && *p != '\n') p++;
            if (p < data + file_size) p++;
            chunk_starts[0] = p;
        } else {
            const uint8_t *p = data + s;
            const uint8_t *end = data + file_size;
            while (p < end) {
                if (*p == '\n') {
                    p++;
                    const uint8_t *rec = p;
                    if (end - rec >= 3 && rec[0] == 0xEF && rec[1] == 0xBB && rec[2] == 0xBF) {
                        rec += 3;
                    }
                    if (rec < end && *rec >= '1' && *rec <= '9') {
                        const uint8_t *d = rec;
                        while (d < end && *d >= '0' && *d <= '9') d++;
                        if (d < end && *d == ',') {
                            chunk_starts[i] = p;
                            break;
                        }
                    }
                }
                p++;
            }
            if (p >= end) chunk_starts[i] = end;
            chunk_ends[i - 1] = chunk_starts[i];
        }
    }
    chunk_ends[num_threads - 1] = data + file_size;

    std::vector<std::vector<ThreadCountryAgg>> thread_aggs(num_threads, std::vector<ThreadCountryAgg>(num_countries));
    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_aggs, &chunk_starts, &chunk_ends, &region_to_cid]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &aggs = thread_aggs[t];
            size_t reg_limit = region_to_cid.size();
            struct L1NameEntry { uint64_t hash; uint32_t nid; };
            std::vector<L1NameEntry> l1_names(32768, {0, 0});

            while (p < end) {
                if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
                if (p >= end) break;

                // 1. id
                uint64_t rec_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    rec_id = rec_id * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == ',') p++;

                // 2. name
                uint64_t name_len = 0;
                uint64_t n_hash = 0xa0761d6478bd642fULL;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        const uint8_t *next_q = find_char_simd(p, end, '"');
                        size_t span = next_q - p;
                        name_len += span;
                        size_t rem = span;
                        const uint8_t *sp_p = p;
                        while (rem >= 8) {
                            uint64_t v;
                            memcpy(&v, sp_p, 8);
                            n_hash = (n_hash ^ v) * 0x517cc1b727220a95ULL;
                            sp_p += 8;
                            rem -= 8;
                        }
                        while (rem > 0) {
                            n_hash = (n_hash ^ *sp_p) * 0x100000001b3ULL;
                            sp_p++;
                            rem--;
                        }
                        p = next_q;
                        if (p < end) {
                            if (p + 1 < end && *(p + 1) == '"') {
                                n_hash = (n_hash ^ '"') * 0x100000001b3ULL;
                                name_len++;
                                p += 2;
                            } else {
                                p++; // closing quote
                                break;
                            }
                        }
                    }
                    if (p < end && *p == ',') p++;
                } else {
                    const uint8_t *nc = find_char_simd(p, end, ',');
                    name_len = nc - p;
                    size_t rem = name_len;
                    const uint8_t *sp_p = p;
                    while (rem >= 8) {
                        uint64_t v;
                        memcpy(&v, sp_p, 8);
                        n_hash = (n_hash ^ v) * 0x517cc1b727220a95ULL;
                        sp_p += 8;
                        rem -= 8;
                    }
                    while (rem > 0) {
                        n_hash = (n_hash ^ *sp_p) * 0x100000001b3ULL;
                        sp_p++;
                        rem--;
                    }
                    p = nc;
                    if (p < end && *p == ',') p++;
                }
                n_hash ^= name_len;

                // 3. note (SIMD accelerated)
                uint64_t note_len = 0;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        const uint8_t *next_q = find_char_simd(p, end, '"');
                        note_len += (next_q - p);
                        p = next_q;
                        if (p < end) {
                            if (p + 1 < end && *(p + 1) == '"') {
                                note_len++;
                                p += 2;
                            } else {
                                p++; // closing quote
                                break;
                            }
                        }
                    }
                    if (p < end && *p == ',') p++;
                } else {
                    const uint8_t *nc = find_char_simd(p, end, ',');
                    note_len = nc - p;
                    p = nc;
                    if (p < end && *p == ',') p++;
                }

                // 4. region_id
                uint32_t reg_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    reg_id = reg_id * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == ',') p++;

                // 5. amount (branchless fraction)
                int64_t sign = 1;
                if (p < end && *p == '-') {
                    sign = -1;
                    p++;
                }
                int64_t whole = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    whole = whole * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == '.') p++;
                int64_t cents = sign * (whole * 100 + (p[0] - '0') * 10 + (p[1] - '0'));
                p += 2;

                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;

                uint16_t cid = (reg_id < reg_limit) ? region_to_cid[reg_id] : 0;
                ThreadCountryAgg &ca = aggs[cid];
                ca.count++;
                ca.id_sum += rec_id;
                ca.amount_sum += cents;
                if (!ca.has_amount) {
                    ca.amount_min = cents;
                    ca.amount_max = cents;
                    ca.has_amount = true;
                } else {
                    if (cents < ca.amount_min) ca.amount_min = cents;
                    if (cents > ca.amount_max) ca.amount_max = cents;
                }
                ca.name_bytes += name_len;
                ca.note_bytes += note_len;

                // Lookup dense name ID and set in country bitset
                uint32_t l1_idx = n_hash & 32767;
                uint32_t nid;
                if (l1_names[l1_idx].hash == n_hash && l1_names[l1_idx].nid != 0) {
                    nid = l1_names[l1_idx].nid;
                } else {
                    nid = get_or_insert_name_id(n_hash);
                    l1_names[l1_idx] = {n_hash, nid};
                }
                uint32_t word_idx = nid / 64;
                uint64_t bit_mask = 1ULL << (nid % 64);
                uint64_t old_val = g_country_bitsets[cid][word_idx].load(std::memory_order_relaxed);
                if ((old_val & bit_mask) == 0) {
                    g_country_bitsets[cid][word_idx].fetch_or(bit_mask, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    munmap((void *)data, file_size);
    close(fd);

    // Merge country aggregates across threads
    std::vector<FinalCountryAgg> final_aggs(num_countries);
    for (size_t c = 0; c < num_countries; c++) {
        final_aggs[c].key = country_names[c];
        FinalCountryAgg &fa = final_aggs[c];

        for (unsigned int thr = 0; thr < num_threads; thr++) {
            auto &ca = thread_aggs[thr][c];
            if (ca.count == 0) continue;
            fa.count += ca.count;
            fa.id_sum += ca.id_sum;
            fa.amount_sum += ca.amount_sum;
            if (fa.count == ca.count) {
                fa.amount_min = ca.amount_min;
                fa.amount_max = ca.amount_max;
            } else {
                if (ca.amount_min < fa.amount_min) fa.amount_min = ca.amount_min;
                if (ca.amount_max > fa.amount_max) fa.amount_max = ca.amount_max;
            }
            fa.name_bytes += ca.name_bytes;
            fa.note_bytes += ca.note_bytes;
        }

        if (fa.count > 0) {
            uint64_t distinct = 0;
            for (size_t w = 0; w < BITSET_WORDS; w++) {
                uint64_t val = g_country_bitsets[c][w].load(std::memory_order_relaxed);
                if (val) distinct += __builtin_popcountll(val);
            }
            fa.names_distinct = distinct;
        }
    }

    // Sort final output by country code
    std::sort(final_aggs.begin(), final_aggs.end(), [](const FinalCountryAgg &a, const FinalCountryAgg &b) {
        return a.key < b.key;
    });

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);

    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 1024 * 1024);

    char line_buf[512];
    for (const auto &fa : final_aggs) {
        if (fa.count == 0) continue;
        char *cur = line_buf;
        memcpy(cur, fa.key.data(), fa.key.size()); cur += fa.key.size();
        *cur++ = ' ';
        cur = fast_u64(cur, fa.count); *cur++ = ' ';
        cur = fast_u64(cur, fa.id_sum); *cur++ = ' ';
        cur = format_cents(cur, fa.amount_sum); *cur++ = ' ';
        cur = format_cents(cur, fa.amount_min); *cur++ = ' ';
        cur = format_cents(cur, fa.amount_max); *cur++ = ' ';
        cur = fast_u64(cur, fa.names_distinct); *cur++ = ' ';
        cur = fast_u64(cur, fa.name_bytes); *cur++ = ' ';
        cur = fast_u64(cur, fa.note_bytes); *cur++ = '\n';
        fwrite(line_buf, 1, cur - line_buf, fout);
    }
    fclose(fout);
    free(out_buf);

    return 0;
}
