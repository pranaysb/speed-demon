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

static inline int parse_hour_idx(const uint8_t *ts) {
    int day_offset = 0;
    if (ts[5] == '1' && ts[6] == '0') {
        if (ts[8] == '3') {
            if (ts[9] == '0') day_offset = 0;
            else day_offset = 1;
        }
    } else {
        if (ts[9] == '1') day_offset = 2;
        else day_offset = 3;
    }
    int hh = (ts[11] - '0') * 10 + (ts[12] - '0');
    return day_offset * 24 + hh - 18;
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
    g_ep_map[idx].str = g_endpoint_strings[id].c_str();
    g_ep_map[idx].len = (uint16_t)len;
    g_ep_map[idx].id = id;
    return id;
}

static inline uint16_t find_endpoint_fast(const char *p, size_t len) {
    uint64_t h = fast_hash(p, len);
    size_t idx = h & EP_MAP_MASK;
    while (true) {
        if (g_ep_map[idx].hash == h && g_ep_map[idx].len == len &&
            memcmp(g_ep_map[idx].str, p, len) == 0) {
            return g_ep_map[idx].id;
        }
        idx = (idx + 1) & EP_MAP_MASK;
    }
}

static inline char* fast_u32(char *p, uint32_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[12]; int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + '0'; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

static inline char* fast_u64(char *p, uint64_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[24]; int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + '0'; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

static constexpr size_t TOTAL_GROUPS = 72 * MAX_ENDPOINTS;

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif

    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    init_hour_strings();

    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/requests.log", data_dir);

    int fd = open(input_path, O_RDONLY);
    if (fd < 0) { perror("open input"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { perror("mmap"); close(fd); return 1; }

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);

    for (unsigned int i = 0; i < num_threads; i++) {
        size_t s = (i * file_size) / num_threads;
        if (i > 0) while (s < file_size && data[s - 1] != '\n') s++;
        chunk_starts[i] = data + s;
        if (i > 0) chunk_ends[i - 1] = chunk_starts[i];
    }
    chunk_ends[num_threads - 1] = data + file_size;

    // Pass 1: Count records & errors per group across threads (ZERO memory allocations!)
    std::vector<std::vector<uint32_t>> thread_counts(num_threads);
    std::vector<std::vector<uint32_t>> thread_errors(num_threads);
    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        thread_counts[t].resize(TOTAL_GROUPS, 0);
        thread_errors[t].resize(TOTAL_GROUPS, 0);
        workers.emplace_back([t, &thread_counts, &thread_errors, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &counts = thread_counts[t];
            auto &errors = thread_errors[t];

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

                p += 28; // ts + space
                while (*p != ' ') p++;
                p++;

                const char *ep_start = (const char *)p;
                while (*p != ' ') p++;
                size_t ep_len = (const char *)p - ep_start;
                p++;

                uint16_t ep_id = get_or_insert_endpoint(ep_start, ep_len);

                bool is_err = (p[0] == '5');
                p += 4;
                while (*p != '\n') p++;
                p++;

                uint32_t g_idx = ((uint32_t)hour_idx << 11) | (uint32_t)ep_id;
                counts[g_idx]++;
                if (is_err) errors[g_idx]++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Compute exact offsets
    std::vector<uint32_t> total_counts(TOTAL_GROUPS, 0);
    std::vector<uint32_t> total_errors(TOTAL_GROUPS, 0);
    std::vector<uint32_t> group_offsets(TOTAL_GROUPS + 1, 0);
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) thread_write_ptrs[t].resize(TOTAL_GROUPS, 0);

    for (size_t g = 0; g < TOTAL_GROUPS; g++) {
        uint32_t c = 0, err = 0;
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][g] = c;
            c += thread_counts[t][g];
            err += thread_errors[t][g];
        }
        total_counts[g] = c;
        total_errors[g] = err;
    }
    thread_counts.clear();
    thread_errors.clear();

    uint32_t total_records = 0;
    for (size_t g = 0; g < TOTAL_GROUPS; g++) {
        group_offsets[g] = total_records;
        total_records += total_counts[g];
    }
    group_offsets[TOTAL_GROUPS] = total_records;

    for (size_t g = 0; g < TOTAL_GROUPS; g++) {
        uint32_t base = group_offsets[g];
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][g] += base;
        }
    }

    // Allocate ONE single contiguous buffer of 12.4 MB!
    uint32_t *all_latencies = (uint32_t *)malloc(total_records * sizeof(uint32_t));

    // Pass 2: Stream latencies directly into exact contiguous slices (lock-free!)
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_write_ptrs, all_latencies, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &wr = thread_write_ptrs[t];

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
                while (*p != ' ') p++;
                p++;

                const char *ep_start = (const char *)p;
                while (*p != ' ') p++;
                size_t ep_len = (const char *)p - ep_start;
                p++;

                uint16_t ep_id = find_endpoint_fast(ep_start, ep_len);
                p += 4;

                uint32_t lat = 0;
                while (*p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p - '0');
                    p++;
                }
                while (*p != '\n') p++;
                p++;

                uint32_t g_idx = ((uint32_t)hour_idx << 11) | (uint32_t)ep_id;
                uint32_t pos = wr[g_idx]++;
                all_latencies[pos] = lat;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    thread_write_ptrs.clear();

    munmap((void *)data, file_size);
    close(fd);

    // Sort endpoints
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    // Pass 3: Process hours in parallel
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);

    std::vector<std::string> hour_outputs(72);
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, num_threads, &hour_outputs, &sorted_endpoint_ids,
                              &group_offsets, &total_counts, &total_errors, all_latencies]() {
            char line_buf[256];
            for (int h = t; h < 72; h += num_threads) {
                std::string &out = hour_outputs[h];
                out.reserve(128 * 1024);
                const char *h_str = g_hour_strings[h];
                for (uint16_t ep_id : sorted_endpoint_ids) {
                    uint32_t g_idx = ((uint32_t)h << 11) | (uint32_t)ep_id;
                    uint32_t n = total_counts[g_idx];
                    if (n == 0) continue;

                    uint32_t start_pos = group_offsets[g_idx];
                    uint32_t *lats = &all_latencies[start_pos];
                    std::sort(lats, lats + n);

                    uint32_t rank50 = (50 * n + 99) / 100;
                    uint32_t rank95 = (95 * n + 99) / 100;
                    uint32_t rank99 = (99 * n + 99) / 100;

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    uint32_t errs = total_errors[g_idx];
                    uint64_t ppm = (uint64_t)errs * 1000000ULL / n;

                    char *cur = line_buf;
                    memcpy(cur, h_str, 20); cur += 20;
                    *cur++ = ' ';
                    const std::string &ep_s = g_endpoint_strings[ep_id];
                    memcpy(cur, ep_s.data(), ep_s.size()); cur += ep_s.size();
                    *cur++ = ' ';
                    cur = fast_u32(cur, n); *cur++ = ' ';
                    cur = fast_u32(cur, p50); *cur++ = ' ';
                    cur = fast_u32(cur, p95); *cur++ = ' ';
                    cur = fast_u32(cur, p99); *cur++ = ' ';
                    cur = fast_u32(cur, errs); *cur++ = ' ';
                    cur = fast_u64(cur, ppm); *cur++ = '\n';
                    out.append(line_buf, cur - line_buf);
                }
            }
        });
    }
    for (auto &w : workers) w.join();

    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 1024 * 1024);
    for (int h = 0; h < 72; h++) {
        if (!hour_outputs[h].empty()) {
            fwrite(hour_outputs[h].data(), 1, hour_outputs[h].size(), fout);
        }
    }
    fclose(fout);
    free(out_buf);
    free(all_latencies);

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    printf("Optimized Two-Pass Track A: %.2f ms\n", ms);
    return 0;
}
