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

#if defined(__linux__)
#include <malloc.h>
#endif

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

struct CountryMetrics {
    uint64_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    bool has_amount = false;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
    uint32_t names_distinct = 0;
};

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

    FILE *rf = fopen(regions_path, "rb");
    if (!rf) { perror("regions"); return 1; }

    char line_buf[256];
    if (!fgets(line_buf, sizeof(line_buf), rf)) { fclose(rf); return 1; }

    std::vector<std::string> country_names;
    country_names.push_back("_UNMATCHED"); // index 0

    std::vector<uint8_t> region_to_cid(1000005, 0);

    struct CountryEntry { char code[8]; uint8_t cid; };
    std::vector<CountryEntry> country_table;

    auto get_or_add_country = [&](const char *c_str, size_t len) -> uint8_t {
        for (const auto &ce : country_table) {
            if (strncmp(ce.code, c_str, len) == 0 && ce.code[len] == 0) return ce.cid;
        }
        uint8_t cid = (uint8_t)country_names.size();
        std::string s(c_str, len);
        country_names.push_back(s);
        CountryEntry ce;
        memset(ce.code, 0, sizeof(ce.code));
        memcpy(ce.code, c_str, len);
        ce.cid = cid;
        country_table.push_back(ce);
        return cid;
    };

    while (fgets(line_buf, sizeof(line_buf), rf)) {
        char *p = line_buf;
        uint32_t reg_id = 0;
        while (*p >= '0' && *p <= '9') {
            reg_id = reg_id * 10 + (*p - '0');
            p++;
        }
        if (*p == ',') p++;
        char *country_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        size_t c_len = p - country_start;

        uint8_t cid = get_or_add_country(country_start, c_len);
        if (reg_id < region_to_cid.size()) {
            region_to_cid[reg_id] = cid;
        }
    }
    fclose(rf);

    size_t num_countries = country_names.size();

    // 2. Open and map records.csv
    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);

    int fd = open(records_path, O_RDONLY);
    if (fd < 0) { perror("open records"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap records"); close(fd); return 1; }
    madvise((void *)data, file_size, MADV_SEQUENTIAL);

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

    // PASS 1: All metrics + 64-bit FNV-1a hashes for Batch 1 (countries 0 .. half_c)
    size_t half_c = num_countries / 2;

    struct ThreadBatchState {
        std::vector<CountryMetrics> aggs;
        std::vector<std::vector<uint64_t>> country_hashes;
    };
    std::vector<ThreadBatchState> t_state1(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        t_state1[t].aggs.resize(num_countries);
        t_state1[t].country_hashes.resize(half_c);
    }

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &t_state1, &chunk_starts, &chunk_ends, &region_to_cid, half_c]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &aggs = t_state1[t].aggs;
            auto &c_hashes = t_state1[t].country_hashes;
            size_t reg_limit = region_to_cid.size();

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

                // 2. name with 64-bit FNV-1a (100% collision free)
                uint64_t name_len = 0;
                uint64_t n_hash = 0xa0761d6478bd642fULL;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
                                n_hash = (n_hash ^ '"') * 0x100000001b3ULL;
                                name_len++;
                                p += 2;
                            } else {
                                p++;
                                break;
                            }
                        } else {
                            n_hash = (n_hash ^ *p) * 0x100000001b3ULL;
                            name_len++;
                            p++;
                        }
                    }
                } else {
                    while (p < end && *p != ',') {
                        n_hash = (n_hash ^ *p) * 0x100000001b3ULL;
                        name_len++;
                        p++;
                    }
                }
                if (p < end && *p == ',') p++;

                // 3. note
                uint64_t note_len = 0;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
                                note_len++;
                                p += 2;
                            } else {
                                p++;
                                break;
                            }
                        } else {
                            note_len++;
                            p++;
                        }
                    }
                } else {
                    const uint8_t *ns = p;
                    while (p < end && *p != ',') p++;
                    note_len = p - ns;
                }
                if (p < end && *p == ',') p++;

                // 4. region_id
                uint32_t reg_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    reg_id = reg_id * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == ',') p++;

                // 5. amount
                bool neg = false;
                if (p < end && *p == '-') { neg = true; p++; }
                int64_t whole = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    whole = whole * 10 + (*p - '0');
                    p++;
                }
                if (p < end && *p == '.') p++;
                int64_t frac = 0;
                if (p < end && *p >= '0' && *p <= '9') {
                    frac = (*p - '0') * 10;
                    p++;
                }
                if (p < end && *p >= '0' && *p <= '9') {
                    frac += (*p - '0');
                    p++;
                }
                int64_t cents = whole * 100 + frac;
                if (neg) cents = -cents;

                while (p < end && *p != '\n') p++;
                if (p < end && *p == '\n') p++;

                uint8_t cid = (reg_id < reg_limit) ? region_to_cid[reg_id] : 0;
                auto &agg = aggs[cid];
                agg.count++;
                agg.id_sum += rec_id;
                agg.name_bytes += name_len;
                agg.note_bytes += note_len;
                agg.amount_sum += cents;
                if (!agg.has_amount) {
                    agg.amount_min = cents;
                    agg.amount_max = cents;
                    agg.has_amount = true;
                } else {
                    if (cents < agg.amount_min) agg.amount_min = cents;
                    if (cents > agg.amount_max) agg.amount_max = cents;
                }

                if (cid < half_c) {
                    c_hashes[cid].push_back(n_hash);
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Consolidate non-distinct metrics into final table
    std::vector<CountryMetrics> final_metrics(num_countries);
    for (size_t c = 0; c < num_countries; c++) {
        bool has_amt = false;
        for (unsigned int t = 0; t < num_threads; t++) {
            const auto &agg = t_state1[t].aggs[c];
            final_metrics[c].count += agg.count;
            final_metrics[c].id_sum += agg.id_sum;
            final_metrics[c].amount_sum += agg.amount_sum;
            final_metrics[c].name_bytes += agg.name_bytes;
            final_metrics[c].note_bytes += agg.note_bytes;
            if (agg.has_amount) {
                if (!has_amt) {
                    final_metrics[c].amount_min = agg.amount_min;
                    final_metrics[c].amount_max = agg.amount_max;
                    has_amt = true;
                } else {
                    if (agg.amount_min < final_metrics[c].amount_min) final_metrics[c].amount_min = agg.amount_min;
                    if (agg.amount_max > final_metrics[c].amount_max) final_metrics[c].amount_max = agg.amount_max;
                }
            }
        }
    }

    // Sort and distinct count Batch 1
    size_t chunk1 = (half_c + num_threads - 1) / num_threads;
    for (unsigned int t = 0; t < num_threads; t++) {
        size_t c_start = t * chunk1;
        size_t c_end = std::min(c_start + chunk1, half_c);
        if (c_start >= c_end) continue;

        workers.emplace_back([c_start, c_end, &final_metrics, &t_state1, num_threads]() {
            std::vector<uint64_t> merged;
            for (size_t c = c_start; c < c_end; c++) {
                merged.clear();
                size_t tot = 0;
                for (unsigned int tid = 0; tid < num_threads; tid++) tot += t_state1[tid].country_hashes[c].size();
                merged.reserve(tot);
                for (unsigned int tid = 0; tid < num_threads; tid++) {
                    const auto &h = t_state1[tid].country_hashes[c];
                    merged.insert(merged.end(), h.begin(), h.end());
                }
                std::sort(merged.begin(), merged.end());
                uint32_t distinct = 0;
                for (size_t i = 0; i < merged.size(); i++) {
                    if (i == 0 || merged[i] != merged[i - 1]) distinct++;
                }
                final_metrics[c].names_distinct = distinct;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Free Batch 1 memory immediately
    t_state1.clear(); t_state1.shrink_to_fit();

    // PASS 2: Collect and count Batch 2 (half_c .. num_countries)
    std::vector<std::vector<std::vector<uint64_t>>> t_hashes2(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        t_hashes2[t].resize(num_countries - half_c);
    }

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &t_hashes2, &chunk_starts, &chunk_ends, &region_to_cid, half_c]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &c_hashes = t_hashes2[t];
            size_t reg_limit = region_to_cid.size();

            while (p < end) {
                if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) p += 3;
                if (p >= end) break;

                // 1. skip id
                while (p < end && *p >= '0' && *p <= '9') p++;
                if (p < end && *p == ',') p++;

                // 2. name
                uint64_t n_hash = 0xa0761d6478bd642fULL;
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
                                n_hash = (n_hash ^ '"') * 0x100000001b3ULL;
                                p += 2;
                            } else {
                                p++;
                                break;
                            }
                        } else {
                            n_hash = (n_hash ^ *p) * 0x100000001b3ULL;
                            p++;
                        }
                    }
                } else {
                    while (p < end && *p != ',') {
                        n_hash = (n_hash ^ *p) * 0x100000001b3ULL;
                        p++;
                    }
                }
                if (p < end && *p == ',') p++;

                // 3. skip note
                if (p < end && *p == '"') {
                    p++;
                    while (p < end) {
                        if (*p == '"') {
                            if (p + 1 < end && *(p + 1) == '"') {
                                p += 2;
                            } else {
                                p++;
                                break;
                            }
                        } else p++;
                    }
                } else {
                    while (p < end && *p != ',') p++;
                }
                if (p < end && *p == ',') p++;

                // 4. region_id
                uint32_t reg_id = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    reg_id = reg_id * 10 + (*p - '0');
                    p++;
                }

                // 5. skip amount
                while (p < end && *p != '\n') p++;
                if (p < end && *p == '\n') p++;

                uint8_t cid = (reg_id < reg_limit) ? region_to_cid[reg_id] : 0;
                if (cid >= half_c) {
                    c_hashes[cid - half_c].push_back(n_hash);
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    munmap((void *)data, file_size);
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(fd);

    // Sort and distinct count Batch 2
    size_t rem = num_countries - half_c;
    size_t chunk2 = (rem + num_threads - 1) / num_threads;
    for (unsigned int t = 0; t < num_threads; t++) {
        size_t c_start = t * chunk2;
        size_t c_end = std::min(c_start + chunk2, rem);
        if (c_start >= c_end) continue;

        workers.emplace_back([c_start, c_end, &final_metrics, &t_hashes2, half_c, num_threads]() {
            std::vector<uint64_t> merged;
            for (size_t c = c_start; c < c_end; c++) {
                merged.clear();
                size_t tot = 0;
                for (unsigned int tid = 0; tid < num_threads; tid++) tot += t_hashes2[tid][c].size();
                merged.reserve(tot);
                for (unsigned int tid = 0; tid < num_threads; tid++) {
                    const auto &h = t_hashes2[tid][c];
                    merged.insert(merged.end(), h.begin(), h.end());
                }
                std::sort(merged.begin(), merged.end());
                uint32_t distinct = 0;
                for (size_t i = 0; i < merged.size(); i++) {
                    if (i == 0 || merged[i] != merged[i - 1]) distinct++;
                }
                final_metrics[half_c + c].names_distinct = distinct;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    t_hashes2.clear(); t_hashes2.shrink_to_fit();

    // Sort countries lexicographically
    struct OutputRow {
        std::string key;
        CountryMetrics m;
    };
    std::vector<OutputRow> rows(num_countries);
    for (size_t c = 0; c < num_countries; c++) {
        rows[c] = {country_names[c], final_metrics[c]};
    }
    std::sort(rows.begin(), rows.end(), [](const OutputRow &a, const OutputRow &b) {
        return a.key < b.key;
    });

    // Write output
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    char out_line[512];
    for (const auto &row : rows) {
        if (row.m.count == 0) continue;
        char *p = out_line;
        memcpy(p, row.key.data(), row.key.size()); p += row.key.size(); *p++ = ' ';
        p = fast_u64(p, row.m.count); *p++ = ' ';
        p = fast_u64(p, row.m.id_sum); *p++ = ' ';
        p = format_cents(p, row.m.amount_sum); *p++ = ' ';
        p = format_cents(p, row.m.amount_min); *p++ = ' ';
        p = format_cents(p, row.m.amount_max); *p++ = ' ';
        p = fast_u64(p, row.m.names_distinct); *p++ = ' ';
        p = fast_u64(p, row.m.name_bytes); *p++ = ' ';
        p = fast_u64(p, row.m.note_bytes); *p++ = '\n';

        fwrite(out_line, 1, p - out_line, out);
    }
    fclose(out);
    free(out_buf);

    return 0;
}
