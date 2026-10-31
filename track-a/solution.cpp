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
#include <mutex>
#include <chrono>

#if defined(__linux__)
#include <malloc.h>
#endif

static char g_hour_strings[72][24];
static void init_hour_strings() {
    int idx = 0;
    for (int h = 18; h <= 23; h++) snprintf(g_hour_strings[idx++], 24, "2026-10-30T%02d:00:00Z", h);
    for (int h = 0; h <= 23; h++)  snprintf(g_hour_strings[idx++], 24, "2026-10-31T%02d:00:00Z", h);
    for (int h = 0; h <= 23; h++)  snprintf(g_hour_strings[idx++], 24, "2026-11-01T%02d:00:00Z", h);
    for (int h = 0; h <= 17; h++)  snprintf(g_hour_strings[idx++], 24, "2026-11-02T%02d:00:00Z", h);
}

static inline int parse_hour_idx(const uint8_t *p) {
    int day = (p[8] - "0"[0]) * 10 + (p[9] - "0"[0]);
    int hour = (p[11] - "0"[0]) * 10 + (p[12] - "0"[0]);
    if (day == 30) return hour - 18;
    if (day == 31) return 6 + hour;
    if (day == 1)  return 30 + hour;
    if (day == 2)  return 54 + hour;
    return 0;
}

static constexpr size_t MAX_ENDPOINTS = 2048;
static std::string g_endpoint_strings[MAX_ENDPOINTS];
static size_t g_num_endpoints = 0;
static std::mutex g_ep_mutex;

struct EndpointEntry {
    uint64_t hash;
    const char *str;
    uint16_t len;
    uint16_t id;
};

static constexpr size_t EP_MAP_CAP = 16384;
static constexpr size_t EP_MAP_MASK = EP_MAP_CAP - 1;
static EndpointEntry g_ep_map[EP_MAP_CAP];

static inline uint64_t fast_hash(const char *p, size_t len) {
    uint64_t h = len;
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x517cc1b727220a95ULL;
        i += 8;
    }
    if (i < len) {
        uint64_t v = 0;
        memcpy(&v, p + i, len - i);
        h = (h ^ v) * 0x517cc1b727220a95ULL;
    }
    return h ^ (h >> 32);
}

static uint16_t get_or_insert_endpoint(const char *p, size_t len) {
    uint64_t h = fast_hash(p, len);
    size_t idx = h & EP_MAP_MASK;
    while (g_ep_map[idx].str != nullptr) {
        if (g_ep_map[idx].hash == h && g_ep_map[idx].len == len &&
            memcmp(g_ep_map[idx].str, p, len) == 0) {
            return g_ep_map[idx].id;
        }
        idx = (idx + 1) & EP_MAP_MASK;
    }

    std::lock_guard<std::mutex> lock(g_ep_mutex);
    idx = h & EP_MAP_MASK;
    while (g_ep_map[idx].str != nullptr) {
        if (g_ep_map[idx].hash == h && g_ep_map[idx].len == len &&
            memcmp(g_ep_map[idx].str, p, len) == 0) {
            return g_ep_map[idx].id;
        }
        idx = (idx + 1) & EP_MAP_MASK;
    }
    uint16_t id = g_num_endpoints++;
    g_endpoint_strings[id] = std::string(p, len);
    g_ep_map[idx].hash = h;
    g_ep_map[idx].len = (uint16_t)len;
    g_ep_map[idx].id = id;
    g_ep_map[idx].str = g_endpoint_strings[id].c_str();
    return id;
}

#pragma pack(push, 1)
struct RawRecord {
    uint32_t lat;
    uint16_t ep_id;
    uint8_t hour_idx;
    uint8_t is_err;
};
#pragma pack(pop)

static inline char* fast_u32(char *p, uint32_t v) {
    if (v == 0) { *p++ = "0"[0]; return p; }
    char buf[12]; int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + "0"[0]; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

static inline char* fast_u64(char *p, uint64_t v) {
    if (v == 0) { *p++ = "0"[0]; return p; }
    char buf[24]; int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + "0"[0]; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif
    auto t_start = std::chrono::high_resolution_clock::now();

    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    init_hour_strings();

    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/requests.log", data_dir);

    int fd = open(input_path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    madvise((void *)data, file_size, MADV_SEQUENTIAL);

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);

    for (unsigned int i = 0; i < num_threads; i++) {
        size_t s = (i * file_size) / num_threads;
        if (i == 0) {
            chunk_starts[0] = data;
        } else {
            const uint8_t *p = data + s;
            while (p < data + file_size && *p != "\n"[0]) p++;
            if (p < data + file_size) p++;
            chunk_starts[i] = p;
            chunk_ends[i - 1] = p;
        }
    }
    chunk_ends[num_threads - 1] = data + file_size;

    // Per-thread record buffers
    size_t est_records_per_thread = (file_size / 70 / num_threads) + 50000;
    std::vector<RawRecord *> thread_records(num_threads);
    std::vector<size_t> thread_record_counts(num_threads, 0);

    for (unsigned int t = 0; t < num_threads; t++) {
        thread_records[t] = (RawRecord *)malloc(est_records_per_thread * sizeof(RawRecord));
    }

    std::vector<std::thread> workers;

    // PASS 1: Single pure streaming parse
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &thread_records, &thread_record_counts]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            RawRecord *recs = thread_records[t];
            size_t count = 0;

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;

            while (p < end) {
                uint64_t cur_hour_prefix;
                memcpy(&cur_hour_prefix, p + 5, 8);
                int hour_idx;
                if (__builtin_expect(cur_hour_prefix == last_hour_prefix, 1)) {
                    hour_idx = last_hour_idx;
                } else {
                    last_hour_prefix = cur_hour_prefix;
                    hour_idx = parse_hour_idx(p);
                    last_hour_idx = hour_idx;
                }

                p += 28;
                while (*p != " "[0]) p++;
                p++;

                const char *ep_start = (const char *)p;
                while (*p != " "[0]) p++;
                size_t ep_len = (const char *)p - ep_start;
                p++;

                uint16_t ep_id = get_or_insert_endpoint(ep_start, ep_len);

                uint8_t is_err = (p[0] == "5"[0]) ? 1 : 0;
                p += 4;

                uint32_t lat = 0;
                while (*p >= "0"[0] && *p <= "9"[0]) {
                    lat = lat * 10 + (*p - "0"[0]);
                    p++;
                }
                while (*p != "\n"[0]) p++;
                p++;

                recs[count++] = {lat, ep_id, (uint8_t)hour_idx, is_err};
            }
            thread_record_counts[t] = count;
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Immediately evict file from memory!
    madvise((void *)data, file_size, MADV_DONTNEED);
    munmap((void *)data, file_size);
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(fd);

    size_t total_recs = 0;
    for (unsigned int t = 0; t < num_threads; t++) total_recs += thread_record_counts[t];

    // Alphabetical endpoint sort
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    // Map internal ep_id -> alpha rank
    std::vector<uint16_t> ep_alpha_rank(g_num_endpoints);
    for (size_t r = 0; r < g_num_endpoints; r++) ep_alpha_rank[sorted_endpoint_ids[r]] = (uint16_t)r;

    // Total buckets = 72 * g_num_endpoints
    size_t num_buckets = 72 * g_num_endpoints;
    std::vector<std::vector<uint32_t>> t_bucket_counts(num_threads);
    std::vector<std::vector<uint32_t>> t_bucket_errs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        t_bucket_counts[t].assign(num_buckets, 0);
        t_bucket_errs[t].assign(num_buckets, 0);
    }

    // Parallel count per bucket
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, num_buckets, &thread_records, &thread_record_counts, &t_bucket_counts, &t_bucket_errs, &ep_alpha_rank]() {
            auto &counts = t_bucket_counts[t];
            auto &errs = t_bucket_errs[t];
            const RawRecord *recs = thread_records[t];
            size_t n = thread_record_counts[t];
            size_t nep = g_num_endpoints;
            for (size_t i = 0; i < n; i++) {
                uint32_t b = recs[i].hour_idx * nep + ep_alpha_rank[recs[i].ep_id];
                counts[b]++;
                errs[b] += recs[i].is_err;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Compute bucket offsets
    std::vector<uint32_t> bucket_offsets(num_buckets + 1, 0);
    std::vector<std::vector<uint32_t>> t_write_ptrs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) t_write_ptrs[t].resize(num_buckets);

    uint32_t running_total = 0;
    for (size_t b = 0; b < num_buckets; b++) {
        bucket_offsets[b] = running_total;
        for (unsigned int t = 0; t < num_threads; t++) {
            t_write_ptrs[t][b] = running_total;
            running_total += t_bucket_counts[t][b];
        }
    }
    bucket_offsets[num_buckets] = running_total;

    // Flat sorted latencies array
    uint32_t *all_lats = (uint32_t *)malloc(total_recs * sizeof(uint32_t));
    std::vector<uint32_t> bucket_errors(num_buckets, 0);

    // Parallel scatter into grouped buckets
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_records, &thread_record_counts, &t_write_ptrs, all_lats, &ep_alpha_rank]() {
            const RawRecord *recs = thread_records[t];
            size_t n = thread_record_counts[t];
            size_t nep = g_num_endpoints;
            auto &wr = t_write_ptrs[t];
            for (size_t i = 0; i < n; i++) {
                uint32_t b = recs[i].hour_idx * nep + ep_alpha_rank[recs[i].ep_id];
                all_lats[wr[b]++] = recs[i].lat;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Free RawRecords immediately
    for (unsigned int t = 0; t < num_threads; t++) free(thread_records[t]);

    for (size_t b = 0; b < num_buckets; b++) {
        uint32_t e = 0;
        for (unsigned int t = 0; t < num_threads; t++) e += t_bucket_errs[t][b];
        bucket_errors[b] = e;
    }
    // Alternatively compute errors during scatter


    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);

    // Output phase: parallel by hour
    std::vector<std::string> hour_outputs(72);
    int hours_per_thread = (72 + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        int h_start = tid * hours_per_thread;
        int h_end = std::min(h_start + hours_per_thread, 72);
        if (h_start >= h_end) continue;

        workers.emplace_back([h_start, h_end, &hour_outputs, &sorted_endpoint_ids, &bucket_offsets, all_lats, &bucket_errors, num_threads]() {
            char line_buf[256];
            size_t nep = g_num_endpoints;

            for (int h = h_start; h < h_end; h++) {
                std::string &out_str = hour_outputs[h];
                out_str.reserve(nep * 64);
                const char *h_str = g_hour_strings[h];

                for (size_t r = 0; r < nep; r++) {
                    uint32_t b = h * nep + r;
                    uint32_t b_start = bucket_offsets[b];
                    uint32_t b_end = bucket_offsets[b + 1];
                    uint32_t n = b_end - b_start;
                    if (n == 0) continue;

                    uint32_t *lats = all_lats + b_start;
                    std::sort(lats, lats + n);

                    uint32_t rank50 = (50 * n + 99) / 100;
                    uint32_t rank95 = (95 * n + 99) / 100;
                    uint32_t rank99 = (99 * n + 99) / 100;

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    // Compute error count for bucket b
                    uint32_t errs = bucket_errors[b];
                    uint64_t ppm = (uint64_t)errs * 1000000ULL / n;

                    uint16_t ep_id = sorted_endpoint_ids[r];
                    const std::string &ep_s = g_endpoint_strings[ep_id];

                    char *cur = line_buf;
                    memcpy(cur, h_str, 20); cur += 20; *cur++ = " "[0];
                    memcpy(cur, ep_s.data(), ep_s.size()); cur += ep_s.size(); *cur++ = " "[0];
                    cur = fast_u32(cur, n); *cur++ = " "[0];
                    cur = fast_u32(cur, p50); *cur++ = " "[0];
                    cur = fast_u32(cur, p95); *cur++ = " "[0];
                    cur = fast_u32(cur, p99); *cur++ = " "[0];
                    cur = fast_u32(cur, errs); *cur++ = " "[0];
                    cur = fast_u64(cur, ppm); *cur++ = "\n"[0];
                    out_str.append(line_buf, cur - line_buf);
                }
            }
        });
    }
    for (auto &w : workers) w.join();

    FILE *out = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);
    for (int h = 0; h < 72; h++) fwrite(hour_outputs[h].data(), 1, hour_outputs[h].size(), out);
    fclose(out);
    free(out_buf);
    free(all_lats);

    double total_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t_start).count();
    printf("Track A Radix Time: %.2f ms\n", total_ms);
    return 0;
}
