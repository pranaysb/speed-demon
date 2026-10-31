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

    uint16_t new_id = (uint16_t)g_num_endpoints++;
    g_endpoint_strings[new_id] = std::string(p, len);
    g_ep_map[idx].hash = h;
    g_ep_map[idx].str = g_endpoint_strings[new_id].c_str();
    g_ep_map[idx].len = (uint16_t)len;
    g_ep_map[idx].id = new_id;
    return new_id;
}

static inline char* fast_u32(char *p, uint32_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[12];
    int i = 0;
    while (v > 0) {
        buf[i++] = (v % 10) + '0';
        v /= 10;
    }
    while (i > 0) *p++ = buf[--i];
    return p;
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

struct L1CacheEntry {
    uint64_t hash;
    const char *str;
    uint16_t ep_id;
    uint16_t len;
};

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif

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

    // Per-thread hour/ep counts
    std::vector<std::vector<uint32_t>> thread_counts(num_threads, std::vector<uint32_t>(72 * MAX_ENDPOINTS, 0));
    std::vector<std::vector<uint32_t>> thread_errors(num_threads, std::vector<uint32_t>(72 * MAX_ENDPOINTS, 0));

    // Pass 1: Count exact items per (hour, ep) and errors
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_counts, &thread_errors, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &counts = thread_counts[t];
            auto &errors = thread_errors[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;
            L1CacheEntry l1_cache[1024] = {};

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
                size_t c_idx = h & 1023;
                uint16_t ep_id;
                if (l1_cache[c_idx].hash == h && l1_cache[c_idx].len == ep_len &&
                    l1_cache[c_idx].str != nullptr &&
                    memcmp(l1_cache[c_idx].str, ep_start, ep_len) == 0) {
                    ep_id = l1_cache[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    l1_cache[c_idx] = {h, g_endpoint_strings[ep_id].c_str(), ep_id, (uint16_t)ep_len};
                }

                uint32_t status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
                p += 4;
                while (p < end && *p != '\n') p++;
                if (p < end) p++;

                size_t cell = (size_t)hour_idx * MAX_ENDPOINTS + ep_id;
                counts[cell]++;
                if (status >= 500) errors[cell]++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    size_t num_eps = g_num_endpoints;
    std::vector<uint32_t> global_counts(72 * MAX_ENDPOINTS, 0);
    std::vector<uint32_t> global_errors(72 * MAX_ENDPOINTS, 0);
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads, std::vector<uint32_t>(72 * MAX_ENDPOINTS, 0));

    uint32_t total_items = 0;
    std::vector<uint32_t> cell_offsets(72 * MAX_ENDPOINTS + 1, 0);

    for (int h = 0; h < 72; h++) {
        for (size_t ep = 0; ep < num_eps; ep++) {
            size_t cell = (size_t)h * MAX_ENDPOINTS + ep;
            uint32_t c = 0;
            uint32_t err = 0;
            for (unsigned int t = 0; t < num_threads; t++) {
                thread_write_ptrs[t][cell] = total_items + c;
                c += thread_counts[t][cell];
                err += thread_errors[t][cell];
            }
            cell_offsets[cell] = total_items;
            global_counts[cell] = c;
            global_errors[cell] = err;
            total_items += c;
        }
    }
    cell_offsets[72 * MAX_ENDPOINTS] = total_items;
    std::vector<std::vector<uint32_t>>().swap(thread_counts);
    std::vector<std::vector<uint32_t>>().swap(thread_errors);

    // Allocate flat latencies array: exactly total_items * 4 bytes (12.4MB on full dataset!)
    size_t lat_bytes = (size_t)total_items * sizeof(uint32_t);
    uint32_t *all_lats = (uint32_t *)mmap(NULL, lat_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    // Pass 2: Write latencies directly into contiguous (hour, ep) cells
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_write_ptrs, all_lats, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            auto &wr = thread_write_ptrs[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;
            L1CacheEntry l1_cache[1024] = {};

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
                size_t c_idx = h & 1023;
                uint16_t ep_id;
                if (l1_cache[c_idx].hash == h && l1_cache[c_idx].len == ep_len &&
                    l1_cache[c_idx].str != nullptr &&
                    memcmp(l1_cache[c_idx].str, ep_start, ep_len) == 0) {
                    ep_id = l1_cache[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    l1_cache[c_idx] = {h, g_endpoint_strings[ep_id].c_str(), ep_id, (uint16_t)ep_len};
                }

                p += 4; // status + space
                uint32_t lat = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p - '0');
                    p++;
                }
                while (p < end && *p != '\n') p++;
                if (p < end) p++;

                size_t cell = (size_t)hour_idx * MAX_ENDPOINTS + ep_id;
                uint32_t pos = wr[cell]++;
                all_lats[pos] = lat;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    std::vector<std::vector<uint32_t>>().swap(thread_write_ptrs);

    // Unmap input file immediately
    munmap((void *)data, file_size);
    close(fd);

#if defined(__linux__)
    malloc_trim(0);
#endif

    // Sort endpoints lexicographically for output ordering
    std::vector<uint16_t> sorted_endpoint_ids(num_eps);
    for (size_t i = 0; i < num_eps; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    std::vector<std::string> hour_outputs(72);
    std::atomic<int> next_hour{0};

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([&, num_eps]() {
            char line_buf[512];
            int h;
            while ((h = next_hour.fetch_add(1, std::memory_order_relaxed)) < 72) {
                std::string &out = hour_outputs[h];
                out.reserve(128 * 1024);
                const char *h_str = g_hour_strings[h];

                for (uint16_t ep_id : sorted_endpoint_ids) {
                    size_t cell = (size_t)h * MAX_ENDPOINTS + ep_id;
                    uint32_t n = global_counts[cell];
                    if (n == 0) continue;

                    uint32_t start_pos = cell_offsets[cell];
                    uint32_t *lats = &all_lats[start_pos];
                    if (n == 2) {
                        if (lats[0] > lats[1]) std::swap(lats[0], lats[1]);
                    } else if (n > 2) {
                        std::sort(lats, lats + n);
                    }

                    uint32_t rank50 = (50 * n + 99) / 100;
                    uint32_t rank95 = (95 * n + 99) / 100;
                    uint32_t rank99 = (99 * n + 99) / 100;

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    uint32_t errs = global_errors[cell];
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

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);

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
    munmap(all_lats, lat_bytes);

    return 0;
}
