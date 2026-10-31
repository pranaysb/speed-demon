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
#include <chrono>

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

struct RecordHash {
    uint64_t hash;
    uint8_t cid;
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

    struct CountryEntry { char code[4]; uint8_t cid; };
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

    // 2. Map records.csv
    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);

    int fd = open(records_path, O_RDONLY);
    if (fd < 0) { perror("open records"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap records");
        close(fd);
        return 1;
    }
    madvise((void *)data, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);

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

    std::vector<std::vector<ThreadCountryAgg>> thread_aggs(num_threads);
    std::vector<std::vector<RecordHash>> thread_hashes(num_threads);

    for (unsigned int t = 0; t < num_threads; t++) {
        thread_aggs[t].resize(num_countries);
        // Estimate ~4M / num_threads records
        thread_hashes[t].reserve(4000000 / num_threads + 32768);
    }

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_aggs, &thread_hashes, &chunk_starts, &chunk_ends, &region_to_cid]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &aggs = thread_aggs[t];
            auto &th_hashes = thread_hashes[t];
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

                // 2. name
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
                n_hash ^= name_len;

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
                    while (p < end && *p != ',') {
                        note_len++;
                        p++;
                    }
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
                int64_t frac = 0;
                if (p < end && *p >= '0' && *p <= '9') {
                    frac = (*p - '0') * 10;
                    p++;
                    if (p < end && *p >= '0' && *p <= '9') {
                        frac += (*p - '0');
                        p++;
                    }
                }
                int64_t cents = sign * (whole * 100 + frac);

                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;

                uint8_t cid = (reg_id < reg_limit) ? region_to_cid[reg_id] : 0;
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
                th_hashes.push_back({n_hash, cid});
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    munmap((void *)data, file_size);
    close(fd);



    // 3. Merge scalar aggregates & compute country offsets
    std::vector<FinalCountryAgg> final_aggs(num_countries);
    for (size_t c = 0; c < num_countries; c++) {
        final_aggs[c].key = country_names[c];
    }

    std::vector<std::vector<uint32_t>> thread_country_counts(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_country_counts[t].resize(num_countries, 0);
        for (size_t c = 0; c < num_countries; c++) {
            const auto &ca = thread_aggs[t][c];
            thread_country_counts[t][c] = (uint32_t)ca.count;
            if (ca.count == 0) continue;
            auto &fa = final_aggs[c];
            fa.count += ca.count;
            fa.id_sum += ca.id_sum;
            fa.amount_sum += ca.amount_sum;
            fa.name_bytes += ca.name_bytes;
            fa.note_bytes += ca.note_bytes;
            if (!ca.has_amount) continue;
            if (fa.count == ca.count) {
                fa.amount_min = ca.amount_min;
                fa.amount_max = ca.amount_max;
            } else {
                if (ca.amount_min < fa.amount_min) fa.amount_min = ca.amount_min;
                if (ca.amount_max > fa.amount_max) fa.amount_max = ca.amount_max;
            }
        }
    }

    // Prefix sum country offsets
    std::vector<uint32_t> country_offsets(num_countries + 1, 0);
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) thread_write_ptrs[t].resize(num_countries, 0);

    for (size_t c = 0; c < num_countries; c++) {
        uint32_t s = 0;
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][c] = s;
            s += thread_country_counts[t][c];
        }
        country_offsets[c + 1] = country_offsets[c] + s;
    }
    for (size_t c = 0; c < num_countries; c++) {
        uint32_t base = country_offsets[c];
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][c] += base;
        }
    }

    size_t total_records = country_offsets[num_countries];
    std::vector<uint64_t> sorted_hashes(total_records);

    // Parallel scatter of hashes into flat contiguous per-country blocks
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_hashes, &thread_write_ptrs, &sorted_hashes]() {
            auto &wr = thread_write_ptrs[t];
            const auto &th = thread_hashes[t];
            for (const auto &rh : th) {
                uint32_t dest = wr[rh.cid]++;
                sorted_hashes[dest] = rh.hash;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    thread_hashes.clear(); thread_hashes.shrink_to_fit();

    // Parallel sort & distinct count for each country
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, num_threads, num_countries, &final_aggs, &country_offsets, &sorted_hashes]() {
            for (size_t c = t; c < num_countries; c += num_threads) {
                uint32_t st = country_offsets[c];
                uint32_t en = country_offsets[c + 1];
                if (st >= en) continue;
                std::sort(&sorted_hashes[st], &sorted_hashes[en]);
                final_aggs[c].names_distinct = std::unique(&sorted_hashes[st], &sorted_hashes[en]) - &sorted_hashes[st];
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();



    // Sort countries by key in byte order
    std::vector<size_t> sorted_cids;
    for (size_t c = 0; c < num_countries; c++) {
        if (final_aggs[c].count > 0) {
            sorted_cids.push_back(c);
        }
    }
    std::sort(sorted_cids.begin(), sorted_cids.end(), [&](size_t a, size_t b) {
        return final_aggs[a].key < final_aggs[b].key;
    });

    // 4. Format output
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen out"); return 1; }

    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    char row_buf[512];
    for (size_t cid : sorted_cids) {
        const FinalCountryAgg &fa = final_aggs[cid];
        char *p = row_buf;
        memcpy(p, fa.key.data(), fa.key.size()); p += fa.key.size(); *p++ = ' ';
        p = fast_u64(p, fa.count); *p++ = ' ';
        p = fast_u64(p, fa.id_sum); *p++ = ' ';
        p = format_cents(p, fa.amount_sum); *p++ = ' ';
        p = format_cents(p, fa.amount_min); *p++ = ' ';
        p = format_cents(p, fa.amount_max); *p++ = ' ';
        p = fast_u64(p, fa.names_distinct); *p++ = ' ';
        p = fast_u64(p, fa.name_bytes); *p++ = ' ';
        p = fast_u64(p, fa.note_bytes); *p++ = '\n';
        fwrite(row_buf, 1, p - row_buf, out);
    }
    fclose(out);
    free(out_buf);

    return 0;
}