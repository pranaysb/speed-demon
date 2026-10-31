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

static uint16_t get_or_insert_endpoint_with_hash(const char *p, size_t len, uint64_t h) {
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

#pragma pack(push, 1)
struct RawItem {
    uint32_t lat;
    uint16_t ep_id;
    uint8_t hour_idx;
    uint8_t is_err;
};
#pragma pack(pop)

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

struct EndpointCacheEntry {
    uint64_t hash;
    uint16_t ep_id;
    uint16_t len;
};

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif
    auto t0 = std::chrono::high_resolution_clock::now();
    init_hour_strings();

    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/requests.log", data_dir);

    int fd = open(input_path, O_RDONLY);
    if (fd < 0) { perror("open input"); return 1; }

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
        if (i > 0) while (s < file_size && data[s - 1] != '\n') s++;
        chunk_starts[i] = data + s;
        if (i > 0) chunk_ends[i - 1] = chunk_starts[i];
    }
    chunk_ends[num_threads - 1] = data + file_size;

    // Tight exact memory limit (275,000 records per thread = 2.2MB per thread)
    size_t max_records_per_thread = (file_size / num_threads / 70) + 30000;
    std::vector<RawItem *> thread_bufs(num_threads);
    std::vector<size_t> thread_recs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_bufs[t] = (RawItem *)malloc(max_records_per_thread * sizeof(RawItem));
    }

    std::vector<std::vector<uint32_t>> thread_hour_counts(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) thread_hour_counts[t].resize(72, 0);

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_bufs, &thread_recs, &thread_hour_counts, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            RawItem *wptr = thread_bufs[t];
            auto &h_counts = thread_hour_counts[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;

            EndpointCacheEntry tl_ep_cache[512] = {};

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

                uint64_t h = fast_hash(ep_start, ep_len);
                size_t c_idx = h & 511;
                uint16_t ep_id;
                if (tl_ep_cache[c_idx].hash == h && tl_ep_cache[c_idx].len == ep_len &&
                    memcmp(g_endpoint_strings[tl_ep_cache[c_idx].ep_id].data(), ep_start, ep_len) == 0) {
                    ep_id = tl_ep_cache[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    tl_ep_cache[c_idx] = {h, ep_id, (uint16_t)ep_len};
                }

                uint8_t is_err = (p[0] == '5') ? 1 : 0;
                p += 4;

                uint32_t lat = 0;
                while (*p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p - '0');
                    p++;
                }
                while (*p != '\n') p++;
                p++;

                h_counts[hour_idx]++;
                *wptr++ = {lat, ep_id, (uint8_t)hour_idx, is_err};
            }
            thread_recs[t] = wptr - thread_bufs[t];
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    madvise((void *)data, file_size, MADV_DONTNEED);
    munmap((void *)data, file_size);
#if defined(__linux__)
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(fd);

    // Compute exact hour offsets
    uint32_t hour_offsets[73] = {0};
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) thread_write_ptrs[t].resize(72, 0);

    for (int h = 0; h < 72; h++) {
        uint32_t c = 0;
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][h] = c;
            c += thread_hour_counts[t][h];
        }
        hour_offsets[h + 1] = hour_offsets[h] + c;
    }
    thread_hour_counts.clear();

    uint32_t total_records = hour_offsets[72];
    for (int h = 0; h < 72; h++) {
        uint32_t base = hour_offsets[h];
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_write_ptrs[t][h] += base;
        }
    }

    // Allocate single contiguous buffer for all items grouped by hour
    RawItem *all_items = (RawItem *)malloc(total_records * sizeof(RawItem));

    // Scatter by hour across threads in parallel
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_bufs, &thread_recs, &thread_write_ptrs, all_items]() {
            const RawItem *src = thread_bufs[t];
            size_t n = thread_recs[t];
            auto &wr = thread_write_ptrs[t];
            for (size_t i = 0; i < n; i++) {
                uint8_t h = src[i].hour_idx;
                uint32_t pos = wr[h]++;
                all_items[pos] = src[i];
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    for (unsigned int t = 0; t < num_threads; t++) free(thread_bufs[t]);
    thread_write_ptrs.clear();

    // Sort endpoints
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);

    std::vector<std::string> hour_outputs(72);

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, num_threads, &hour_outputs, &sorted_endpoint_ids, hour_offsets, all_items]() {
            char line_buf[256];
            uint32_t ep_counts[MAX_ENDPOINTS];
            uint32_t ep_errors[MAX_ENDPOINTS];
            uint32_t ep_offsets[MAX_ENDPOINTS + 1];
            uint32_t ep_cur[MAX_ENDPOINTS];
            std::vector<uint32_t> sorted_latencies;
            sorted_latencies.reserve(65536);

            for (int h = t; h < 72; h += num_threads) {
                uint32_t h_start = hour_offsets[h];
                uint32_t h_count = hour_offsets[h + 1] - h_start;
                if (h_count == 0) continue;

                memset(ep_counts, 0, g_num_endpoints * sizeof(uint32_t));
                memset(ep_errors, 0, g_num_endpoints * sizeof(uint32_t));

                const RawItem *h_items = all_items + h_start;
                for (uint32_t i = 0; i < h_count; i++) {
                    uint16_t ep = h_items[i].ep_id;
                    ep_counts[ep]++;
                    ep_errors[ep] += h_items[i].is_err;
                }

                ep_offsets[0] = 0;
                uint32_t tot_h = 0;
                for (size_t ep = 0; ep < g_num_endpoints; ep++) {
                    ep_offsets[ep] = tot_h;
                    ep_cur[ep] = tot_h;
                    tot_h += ep_counts[ep];
                }
                ep_offsets[g_num_endpoints] = tot_h;

                sorted_latencies.resize(h_count);
                for (uint32_t i = 0; i < h_count; i++) {
                    uint16_t ep = h_items[i].ep_id;
                    sorted_latencies[ep_cur[ep]++] = h_items[i].lat;
                }

                std::string &out_str = hour_outputs[h];
                out_str.reserve(g_num_endpoints * 128);
                const char *h_str = g_hour_strings[h];

                for (uint16_t ep_id : sorted_endpoint_ids) {
                    uint32_t n = ep_counts[ep_id];
                    if (n == 0) continue;

                    uint32_t start_pos = ep_offsets[ep_id];
                    uint32_t *lats = &sorted_latencies[start_pos];
                    std::sort(lats, lats + n);

                    uint32_t rank50 = (50 * n + 99) / 100;
                    uint32_t rank95 = (95 * n + 99) / 100;
                    uint32_t rank99 = (99 * n + 99) / 100;

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    uint32_t errs = ep_errors[ep_id];
                    uint64_t ppm = (uint64_t)errs * 1000000ULL / n;

                    char *cur = line_buf;
                    memcpy(cur, h_str, 20); cur += 20; *cur++ = ' ';
                    const std::string &ep_s = g_endpoint_strings[ep_id];
                    memcpy(cur, ep_s.data(), ep_s.size()); cur += ep_s.size(); *cur++ = ' ';
                    cur = fast_u32(cur, n); *cur++ = ' ';
                    cur = fast_u32(cur, p50); *cur++ = ' ';
                    cur = fast_u32(cur, p95); *cur++ = ' ';
                    cur = fast_u32(cur, p99); *cur++ = ' ';
                    cur = fast_u32(cur, errs); *cur++ = ' ';
                    cur = fast_u64(cur, ppm); *cur++ = '\n';
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
    free(all_items);

    double total_ms = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - t0).count();
    printf("Track A Stream Total Time: %.2f ms\n", total_ms);
    return 0;
}
