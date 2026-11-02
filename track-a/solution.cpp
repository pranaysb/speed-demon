
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
#include <atomic>
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
    uint64_t h = 0xcbf29ce484222325ULL ^ len;
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v; memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ULL;
        i += 8;
    }
    while (i < len) {
        h = (h ^ (uint8_t)p[i]) * 0x100000001b3ULL;
        i++;
    }
    return h;
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

struct L1CacheEntry {
    uint64_t hash;
    uint16_t ep_id;
    uint16_t len;
};

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

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif
    auto t_start = std::chrono::high_resolution_clock::now();
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
    if (fd < 0) { perror("open"); return 1; }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    unsigned int num_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);

    for (unsigned int i = 0; i < num_threads; i++) {
        size_t s = (i * file_size) / num_threads;
        if (i == 0) {
            chunk_starts[0] = data;
        } else {
            const uint8_t *p = data + s;
            while (p < data + file_size && *p != '\n') p++;
            if (p < data + file_size) p++;
            chunk_starts[i] = p;
            chunk_ends[i - 1] = p;
        }
    }
    chunk_ends[num_threads - 1] = data + file_size;

    // Phase 1: Parallel count per (hour, ep)
    // Structure: 72 hours * 2048 endpoints = 147,456 slots (589 KB per thread)
    struct ThreadCountTable {
        uint32_t counts[72][MAX_ENDPOINTS];
        uint32_t errors[72][MAX_ENDPOINTS];
    };
    std::vector<ThreadCountTable *> thread_tables(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_tables[t] = (ThreadCountTable *)calloc(1, sizeof(ThreadCountTable));
    }

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_tables, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &tbl = *thread_tables[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;
            L1CacheEntry l1_cache[4096] = {};

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
                if (p[3] == ' ') p += 4;
                else if (p[4] == ' ') p += 5;
                else if (p[5] == ' ') p += 6;
                else if (p[6] == ' ') p += 7;
                else if (p[7] == ' ') p += 8;
                else { while (p < end && *p != ' ') p++; if (p < end) p++; }

                const char *ep_start = (const char *)p;
                const char *sp = (const char *)memchr(ep_start, ' ', end - (const uint8_t *)ep_start);
                if (!sp) break;
                size_t ep_len = sp - ep_start;
                p = (const uint8_t *)sp + 1;

                uint64_t h = fast_hash(ep_start, ep_len);
                size_t l1_idx = h & 4095;
                uint16_t ep_id;
                if (__builtin_expect(l1_cache[l1_idx].hash == h && l1_cache[l1_idx].len == ep_len, 1)) {
                    ep_id = l1_cache[l1_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    l1_cache[l1_idx] = {h, ep_id, (uint16_t)ep_len};
                }

                bool is_err = (p[0] == '5');
                tbl.counts[hour_idx][ep_id]++;
                if (is_err) tbl.errors[hour_idx][ep_id]++;

                p += 4; // skip status and space
                while (p < end && *p != '\n') p++;
                if (p < end && *p == '\n') p++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Sum counts and build global CSR write offsets per (hour, ep, thread)
    // Total requests across all hours
    size_t total_requests = 0;
    static uint32_t global_counts[72][MAX_ENDPOINTS] = {};
    static uint32_t global_errors[72][MAX_ENDPOINTS] = {};
    static uint32_t thread_start_pos[12][72][MAX_ENDPOINTS] = {};

    size_t running_total = 0;
    for (int h = 0; h < 72; h++) {
        for (size_t ep = 0; ep < g_num_endpoints; ep++) {
            uint32_t total_ep = 0;
            uint32_t total_err = 0;
            for (unsigned int t = 0; t < num_threads; t++) {
                thread_start_pos[t][h][ep] = running_total + total_ep;
                total_ep += thread_tables[t]->counts[h][ep];
                total_err += thread_tables[t]->errors[h][ep];
            }
            global_counts[h][ep] = total_ep;
            global_errors[h][ep] = total_err;
            running_total += total_ep;
        }
    }
    total_requests = running_total;

    // Flat latencies array: 3,100,000 * 4 bytes = 12.4 MB!
    size_t lat_bytes = total_requests * sizeof(uint32_t);
    uint32_t *all_latencies = (uint32_t *)mmap(NULL, lat_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    // Phase 2: Direct write into all_latencies reusing thread_tables as cur_pos (0 extra memory!)
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_tables, &chunk_starts, &chunk_ends, all_latencies]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;
            L1CacheEntry l1_cache[4096] = {};

            // Reuse thread_tables[t]->counts directly as cur_pos!
            uint32_t (*cur_pos)[MAX_ENDPOINTS] = thread_tables[t]->counts;
            for (int h = 0; h < 72; h++) {
                for (size_t ep = 0; ep < g_num_endpoints; ep++) {
                    cur_pos[h][ep] = thread_start_pos[t][h][ep];
                }
            }

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
                if (p[3] == ' ') p += 4;
                else if (p[4] == ' ') p += 5;
                else if (p[5] == ' ') p += 6;
                else if (p[6] == ' ') p += 7;
                else if (p[7] == ' ') p += 8;
                else { while (p < end && *p != ' ') p++; if (p < end) p++; }

                const char *ep_start = (const char *)p;
                const char *sp = (const char *)memchr(ep_start, ' ', end - (const uint8_t *)ep_start);
                if (!sp) break;
                size_t ep_len = sp - ep_start;
                p = (const uint8_t *)sp + 1;

                uint64_t h = fast_hash(ep_start, ep_len);
                size_t l1_idx = h & 4095;
                uint16_t ep_id;
                if (__builtin_expect(l1_cache[l1_idx].hash == h && l1_cache[l1_idx].len == ep_len, 1)) {
                    ep_id = l1_cache[l1_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    l1_cache[l1_idx] = {h, ep_id, (uint16_t)ep_len};
                }

                p += 4; // skip status and space

                uint32_t lat = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p++ - '0');
                }
                if (p < end && *p == '\r') p++;
                if (p < end && *p == '\n') p++;

                all_latencies[cur_pos[hour_idx][ep_id]++] = lat;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    for (unsigned int t = 0; t < num_threads; t++) free(thread_tables[t]);

#if defined(__linux__)
    posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED);
#endif
    munmap((void *)data, file_size);
    close(fd);

    // Sorted endpoints order
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);
    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 1024 * 1024);

    std::vector<std::string> hour_outputs(72);
    std::vector<std::atomic<bool>> hour_ready(72);
    for (int h = 0; h < 72; h++) hour_ready[h].store(false);

    std::atomic<int> next_hour{0};
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([&]() {
            int h;
            char line_buf[512];
            while ((h = next_hour.fetch_add(1, std::memory_order_relaxed)) < 72) {
                std::string &out = hour_outputs[h];
                const char *h_str = g_hour_strings[h];

                for (uint16_t ep_id : sorted_endpoint_ids) {
                    uint32_t n = global_counts[h][ep_id];
                    if (n == 0) continue;

                    uint32_t start_pos = thread_start_pos[0][h][ep_id];
                    uint32_t *lats = all_latencies + start_pos;

                    if (n == 2) {
                        if (lats[0] > lats[1]) std::swap(lats[0], lats[1]);
                    } else if (n > 2) {
                        size_t rank99 = (99 * (size_t)n + 99) / 100;
                        size_t rank95 = (95 * (size_t)n + 99) / 100;
                        size_t rank50 = (50 * (size_t)n + 99) / 100;
                        std::nth_element(lats, lats + rank99 - 1, lats + n);
                        std::nth_element(lats, lats + rank95 - 1, lats + rank99 - 1);
                        std::nth_element(lats, lats + rank50 - 1, lats + rank95 - 1);
                    }

                    size_t rank50 = (50 * (size_t)n + 99) / 100;
                    size_t rank95 = (95 * (size_t)n + 99) / 100;
                    size_t rank99 = (99 * (size_t)n + 99) / 100;
                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    uint32_t errs = global_errors[h][ep_id];
                    uint64_t ppm = (uint64_t)errs * 1000000ULL / n;

                    char *cur = line_buf;
                    memcpy(cur, h_str, 20); cur += 20;
                    *cur++ = ' ';

                    const std::string &ep_str = g_endpoint_strings[ep_id];
                    memcpy(cur, ep_str.data(), ep_str.size()); cur += ep_str.size();
                    *cur++ = ' ';

                    cur = fast_u32(cur, n); *cur++ = ' ';
                    cur = fast_u32(cur, p50); *cur++ = ' ';
                    cur = fast_u32(cur, p95); *cur++ = ' ';
                    cur = fast_u32(cur, p99); *cur++ = ' ';
                    cur = fast_u32(cur, errs); *cur++ = ' ';
                    cur = fast_u64(cur, ppm); *cur++ = '\n';

                    out.append(line_buf, cur - line_buf);
                }
                hour_ready[h].store(true, std::memory_order_release);
            }
        });
    }

    for (int h = 0; h < 72; h++) {
        while (!hour_ready[h].load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (!hour_outputs[h].empty()) {
            fwrite(hour_outputs[h].data(), 1, hour_outputs[h].size(), fout);
            hour_outputs[h].clear();
            hour_outputs[h].shrink_to_fit();
        }
    }
    fclose(fout);
    free(out_buf);

    for (auto &w : workers) w.join();
    munmap(all_latencies, lat_bytes);

    auto t_done = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t_done - t_start).count();
    printf("Direct Flat CSR Track A completed in: %.2f ms (Total items: %zu)\n", ms, total_requests);

    return 0;
}
