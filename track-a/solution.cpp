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

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
static inline uint64_t fast_crc_word(uint64_t crc, uint64_t v) { return _mm_crc32_u64(crc, v); }
static inline uint64_t fast_crc_byte(uint64_t crc, uint8_t b) { return _mm_crc32_u8((uint32_t)crc, b); }
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_acle.h>
static inline uint64_t fast_crc_word(uint64_t crc, uint64_t v) { return __crc32d((uint32_t)crc, v); }
static inline uint64_t fast_crc_byte(uint64_t crc, uint8_t b) { return __crc32b((uint32_t)crc, b); }
#else
static inline uint64_t fast_crc_word(uint64_t crc, uint64_t v) {
    for (int i = 0; i < 8; i++) crc = (crc ^ ((v >> (i * 8)) & 0xFF)) * 0x100000001b3ULL;
    return crc;
}
static inline uint64_t fast_crc_byte(uint64_t crc, uint8_t b) { return (crc ^ b) * 0x100000001b3ULL; }
#endif

static inline int find_space_8(uint64_t v) {
    uint64_t x = v ^ 0x2020202020202020ULL;
    uint64_t z = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    if (z == 0) return -1;
    return __builtin_ctzll(z) >> 3;
}

// Base timestamp: 2026-10-30T18:00:00Z -> hour 0
static inline int parse_hour_idx(const uint8_t *p) {
    int day_offset = 0;
    if (p[5] == '1' && p[6] == '0') {
        if (p[8] == '3') {
            if (p[9] == '0') day_offset = 0;
            else day_offset = 1;
        }
    } else {
        if (p[9] == '1') day_offset = 2;
        else day_offset = 3;
    }
    int hh = (p[11] - '0') * 10 + (p[12] - '0');
    return day_offset * 24 + hh - 18;
}

static char g_hour_strings[72][32];
static void init_hour_strings() {
    int idx = 0;
    for (int h = 18; h <= 23; h++) snprintf(g_hour_strings[idx++], sizeof(g_hour_strings[0]), "2026-10-30T%02d:00:00Z", h);
    for (int h = 0; h <= 23; h++)  snprintf(g_hour_strings[idx++], sizeof(g_hour_strings[0]), "2026-10-31T%02d:00:00Z", h);
    for (int h = 0; h <= 23; h++)  snprintf(g_hour_strings[idx++], sizeof(g_hour_strings[0]), "2026-11-01T%02d:00:00Z", h);
    for (int h = 0; h <= 17; h++)  snprintf(g_hour_strings[idx++], sizeof(g_hour_strings[0]), "2026-11-02T%02d:00:00Z", h);
}

static constexpr size_t MAX_ENDPOINTS = 2048;
static constexpr size_t TOTAL_GROUPS = 72 * MAX_ENDPOINTS; // 147,456 slots

struct EpEntry {
    uint64_t hash = 0;
    const char *ptr = nullptr;
    uint16_t len = 0;
    uint16_t id = 0xFFFF;
};

static constexpr size_t EP_MAP_CAP = 16384;
static constexpr size_t EP_MAP_MASK = EP_MAP_CAP - 1;
static EpEntry g_ep_map[EP_MAP_CAP];
static size_t g_num_endpoints = 0;
static std::mutex g_ep_mutex;
static std::string g_endpoint_strings[MAX_ENDPOINTS];

static uint16_t get_or_insert_endpoint(const char *p, size_t len, uint64_t crc) {
    size_t idx = crc & EP_MAP_MASK;
    while (g_ep_map[idx].ptr != nullptr) {
        if (g_ep_map[idx].hash == crc && g_ep_map[idx].len == len && memcmp(g_ep_map[idx].ptr, p, len) == 0) {
            return g_ep_map[idx].id;
        }
        idx = (idx + 1) & EP_MAP_MASK;
    }

    std::lock_guard<std::mutex> lock(g_ep_mutex);
    idx = crc & EP_MAP_MASK;
    while (g_ep_map[idx].ptr != nullptr) {
        if (g_ep_map[idx].hash == crc && g_ep_map[idx].len == len && memcmp(g_ep_map[idx].ptr, p, len) == 0) {
            return g_ep_map[idx].id;
        }
        idx = (idx + 1) & EP_MAP_MASK;
    }
    uint16_t id = (uint16_t)g_num_endpoints++;
    g_endpoint_strings[id] = std::string(p, len);
    g_ep_map[idx].hash = crc;
    g_ep_map[idx].ptr = g_endpoint_strings[id].c_str();
    g_ep_map[idx].len = (uint16_t)len;
    g_ep_map[idx].id = id;
    return id;
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

    // Per-thread count/write buffers: 12 * 147,456 * 2 bytes = ONLY 3.45 MB total!
    size_t thread_buf_bytes = TOTAL_GROUPS * sizeof(uint16_t);
    std::vector<uint16_t *> thread_ptrs(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_ptrs[t] = (uint16_t *)mmap(NULL, thread_buf_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }

    // Global errors array: 147,456 * 4 bytes = 589 KB
    std::atomic<uint32_t> *global_errors = (std::atomic<uint32_t> *)mmap(NULL, TOTAL_GROUPS * sizeof(std::atomic<uint32_t>), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    // ==========================================
    // PASS 1: Count items per group & discover endpoints
    // ==========================================
    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &thread_ptrs, global_errors]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            uint16_t *counts = thread_ptrs[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;

            struct L1Cache { uint64_t crc; uint16_t ep_id; uint16_t len; };
            L1Cache l1[8192] = {};

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
                uint64_t crc = 0;
                while (true) {
                    uint64_t v; memcpy(&v, p, 8);
                    int sp = find_space_8(v);
                    if (sp == -1) {
                        crc = fast_crc_word(crc, v);
                        p += 8;
                    } else {
                        for (int i = 0; i < sp; i++) crc = fast_crc_byte(crc, p[i]);
                        p += sp + 1;
                        break;
                    }
                }
                size_t ep_len = (const char *)p - 1 - ep_start;

                size_t c_idx = crc & 8191;
                uint16_t ep_id;
                if (__builtin_expect(l1[c_idx].crc == crc && l1[c_idx].len == ep_len, 1)) {
                    ep_id = l1[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint(ep_start, ep_len, crc);
                    l1[c_idx] = {crc, ep_id, (uint16_t)ep_len};
                }

                uint32_t gid = hour_idx * MAX_ENDPOINTS + ep_id;
                counts[gid]++;
                if (p[0] >= '5') {
                    global_errors[gid].fetch_add(1, std::memory_order_relaxed);
                }
                p += 4; // status + space

                // Skip latency digits to newline
                while (p < end && *p != '\n') p++;
                if (p < end) p++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Sort endpoints alphabetically
    uint16_t n_endpoints = (uint16_t)g_num_endpoints;
    std::vector<uint16_t> sorted_to_orig(n_endpoints);
    for (uint16_t i = 0; i < n_endpoints; i++) sorted_to_orig[i] = i;
    std::sort(sorted_to_orig.begin(), sorted_to_orig.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    uint32_t *g_group_start = (uint32_t *)mmap(NULL, TOTAL_GROUPS * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint32_t *g_group_count = (uint32_t *)mmap(NULL, TOTAL_GROUPS * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    uint32_t running_total = 0;
    for (int h = 0; h < 72; h++) {
        for (uint16_t s = 0; s < n_endpoints; s++) {
            uint16_t orig_ep = sorted_to_orig[s];
            uint32_t orig_gid = h * MAX_ENDPOINTS + orig_ep;

            g_group_start[orig_gid] = running_total;

            uint16_t thread_rel_offset = 0;
            for (unsigned int t = 0; t < num_threads; t++) {
                uint16_t cnt = thread_ptrs[t][orig_gid];
                thread_ptrs[t][orig_gid] = thread_rel_offset;
                thread_rel_offset += cnt;
            }
            g_group_count[orig_gid] = thread_rel_offset;
            running_total += thread_rel_offset;
        }
    }

    size_t lats_bytes = (size_t)running_total * sizeof(uint32_t);
    uint32_t *all_lats = (uint32_t *)mmap(NULL, lats_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    // ==========================================
    // PASS 2: Stream latencies directly into final group slices!
    // ==========================================
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &thread_ptrs, g_group_start, all_lats]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            uint16_t *write_pos = thread_ptrs[t];

            uint64_t last_hour_prefix = 0;
            int last_hour_idx = -1;

            struct L1Cache { uint64_t crc; uint16_t ep_id; uint16_t len; };
            L1Cache l1[8192] = {};

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
                uint64_t crc = 0;
                while (true) {
                    uint64_t v; memcpy(&v, p, 8);
                    int sp = find_space_8(v);
                    if (sp == -1) {
                        crc = fast_crc_word(crc, v);
                        p += 8;
                    } else {
                        for (int i = 0; i < sp; i++) crc = fast_crc_byte(crc, p[i]);
                        p += sp + 1;
                        break;
                    }
                }
                size_t ep_len = (const char *)p - 1 - ep_start;

                size_t c_idx = crc & 8191;
                uint16_t ep_id;
                if (__builtin_expect(l1[c_idx].crc == crc && l1[c_idx].len == ep_len, 1)) {
                    ep_id = l1[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint(ep_start, ep_len, crc);
                    l1[c_idx] = {crc, ep_id, (uint16_t)ep_len};
                }

                p += 4; // status + space

                uint32_t lat = *p++ - '0';
                while (*p >= '0') {
                    lat = lat * 10 + (*p++ - '0');
                }
                if (*p == '\r') p++;
                p++;

                uint32_t gid = hour_idx * MAX_ENDPOINTS + ep_id;
                uint32_t pos = g_group_start[gid] + write_pos[gid]++;
                all_lats[pos] = lat;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

#if defined(__linux__)
    posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED);
#endif
    munmap((void *)data, file_size);
    close(fd);

    for (unsigned int t = 0; t < num_threads; t++) {
        munmap(thread_ptrs[t], thread_buf_bytes);
    }

    // ==========================================
    // PHASE 3: Parallel percentile calculation & formatted output
    // ==========================================
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);
    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 1024 * 1024);

    std::vector<std::string> hour_outputs(72);
    std::vector<std::atomic<bool>> hour_ready(72);
    for (int h = 0; h < 72; h++) hour_ready[h].store(false, std::memory_order_relaxed);

    std::atomic<int> next_hour{0};

    // Use full hardware concurrency for Phase 3!
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([&, n_endpoints]() {
            char line_buf[256];
            int h;
            while ((h = next_hour.fetch_add(1)) < 72) {
                std::string &out = hour_outputs[h];
                out.reserve(n_endpoints * 64);
                const char *h_str = g_hour_strings[h];
                memcpy(line_buf, h_str, 20);
                line_buf[20] = ' ';

                for (uint16_t s = 0; s < n_endpoints; s++) {
                    uint16_t orig_ep = sorted_to_orig[s];
                    uint32_t orig_gid = h * MAX_ENDPOINTS + orig_ep;
                    uint32_t n = g_group_count[orig_gid];
                    if (n == 0) continue;

                    uint32_t start_pos = g_group_start[orig_gid];
                    uint32_t errs = global_errors[orig_gid].load(std::memory_order_relaxed);
                    uint64_t ppm = (uint64_t)errs * 1000000ULL / n;

                    uint32_t *lats = &all_lats[start_pos];
                    size_t rank50 = (50 * (size_t)n + 99) / 100;
                    size_t rank95 = (95 * (size_t)n + 99) / 100;
                    size_t rank99 = (99 * (size_t)n + 99) / 100;

                    if (n == 2) {
                        if (lats[0] > lats[1]) std::swap(lats[0], lats[1]);
                    } else if (n > 2) {
                        std::nth_element(lats, lats + rank99 - 1, lats + n);
                        std::nth_element(lats, lats + rank95 - 1, lats + rank99 - 1);
                        std::nth_element(lats, lats + rank50 - 1, lats + rank95 - 1);
                    }

                    uint32_t p50 = lats[rank50 - 1];
                    uint32_t p95 = lats[rank95 - 1];
                    uint32_t p99 = lats[rank99 - 1];

                    char *cur = line_buf + 21;
                    const std::string &ep_str = g_endpoint_strings[orig_ep];
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

    munmap(all_lats, lats_bytes);
    munmap(g_group_start, TOTAL_GROUPS * sizeof(uint32_t));
    munmap(g_group_count, TOTAL_GROUPS * sizeof(uint32_t));
    munmap(global_errors, TOTAL_GROUPS * sizeof(std::atomic<uint32_t>));

    return 0;
}
