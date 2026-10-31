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

#pragma pack(push, 2)
struct TempItem {
    uint32_t lat;
    uint16_t ep_code; // bit 15: is_err, bits 0..14: ep_id
    uint8_t hour_idx;
    uint8_t pad;
};
#pragma pack(pop)

#pragma pack(push, 2)
struct FinalItem {
    uint32_t lat;
    uint16_t ep_code;
};
#pragma pack(pop)

struct L1CacheEntry {
    uint64_t hash;
    const char *str;
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

    std::vector<TempItem *> thread_bufs(num_threads);
    std::vector<FinalItem *> thread_partitioned(num_threads);
    std::vector<size_t> thread_buf_bytes(num_threads);
    std::vector<size_t> thread_final_bytes(num_threads);
    std::vector<size_t> thread_recs(num_threads);
    std::vector<std::vector<uint32_t>> thread_hour_offsets(num_threads, std::vector<uint32_t>(73, 0));
    std::vector<std::vector<uint32_t>> thread_hour_counts(num_threads, std::vector<uint32_t>(72, 0));

    for (unsigned int t = 0; t < num_threads; t++) {
        size_t chunk_bytes = chunk_ends[t] - chunk_starts[t];
        size_t max_recs = (chunk_bytes / 65) + 5000;
        thread_buf_bytes[t] = max_recs * sizeof(TempItem);
        thread_bufs[t] = (TempItem *)mmap(NULL, thread_buf_bytes[t], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_bufs, &thread_recs, &thread_hour_counts, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            TempItem *wptr = thread_bufs[t];
            auto &h_counts = thread_hour_counts[t];

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

                uint16_t is_err = (p[0] == '5') ? 0x8000 : 0x0000;
                p += 4;

                uint32_t lat = 0;
                while (*p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p - '0');
                    p++;
                }
                if (*p == '\r') p++;
                if (*p == '\n') p++;

                h_counts[hour_idx]++;
                *wptr++ = {lat, (uint16_t)(ep_id | is_err), (uint8_t)hour_idx, 0};
            }
            size_t n = wptr - thread_bufs[t];
            thread_recs[t] = n;
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    munmap((void *)data, file_size);
    close(fd);

    // SERIALIZED copy: only 1 thread allocates FinalItem while TempItem is alive, then munmaps TempItem!
    for (unsigned int t = 0; t < num_threads; t++) {
        size_t n = thread_recs[t];
        thread_final_bytes[t] = n * sizeof(FinalItem);
        FinalItem *part = (FinalItem *)mmap(NULL, thread_final_bytes[t], PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        thread_partitioned[t] = part;

        auto &h_offsets = thread_hour_offsets[t];
        auto &h_counts = thread_hour_counts[t];
        for (int hr = 0; hr < 72; hr++) {
            h_offsets[hr + 1] = h_offsets[hr] + h_counts[hr];
        }
        uint32_t cur[72];
        memcpy(cur, h_offsets.data(), sizeof(cur));

        const TempItem *src = thread_bufs[t];
        for (size_t i = 0; i < n; i++) {
            uint8_t hr = src[i].hour_idx;
            uint32_t pos = cur[hr]++;
            part[pos] = {src[i].lat, src[i].ep_code};
        }

        // Munmap thread_bufs[t] IMMEDIATELY before next thread allocates!
        munmap(thread_bufs[t], thread_buf_bytes[t]);
        thread_bufs[t] = nullptr;
    }

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
    std::atomic<int> next_hour{0};

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([&next_hour, &hour_outputs, &sorted_endpoint_ids,
                              &thread_partitioned, &thread_hour_offsets, &thread_hour_counts, num_threads]() {
            char line_buf[256];
            uint32_t ep_counts[MAX_ENDPOINTS];
            uint32_t ep_errors[MAX_ENDPOINTS];
            uint32_t ep_offsets[MAX_ENDPOINTS + 1];
            uint32_t ep_cur[MAX_ENDPOINTS];
            std::vector<uint32_t> sorted_latencies;
            sorted_latencies.reserve(65536);

            int h;
            while ((h = next_hour.fetch_add(1)) < 72) {
                uint32_t h_count = 0;
                for (unsigned int thr = 0; thr < num_threads; thr++) {
                    h_count += thread_hour_counts[thr][h];
                }
                if (h_count == 0) continue;

                memset(ep_counts, 0, sizeof(ep_counts));
                memset(ep_errors, 0, sizeof(ep_errors));

                for (unsigned int thr = 0; thr < num_threads; thr++) {
                    uint32_t cnt = thread_hour_counts[thr][h];
                    if (cnt == 0) continue;
                    uint32_t off = thread_hour_offsets[thr][h];
                    const FinalItem *items = &thread_partitioned[thr][off];
                    for (uint32_t i = 0; i < cnt; i++) {
                        uint16_t ep = items[i].ep_code & 0x7FFF;
                        ep_counts[ep]++;
                        if (items[i].ep_code & 0x8000) ep_errors[ep]++;
                    }
                }

                uint32_t tot_h = 0;
                for (size_t ep = 0; ep < MAX_ENDPOINTS; ep++) {
                    ep_offsets[ep] = tot_h;
                    ep_cur[ep] = tot_h;
                    tot_h += ep_counts[ep];
                }
                ep_offsets[MAX_ENDPOINTS] = tot_h;

                sorted_latencies.resize(tot_h);
                for (unsigned int thr = 0; thr < num_threads; thr++) {
                    uint32_t cnt = thread_hour_counts[thr][h];
                    if (cnt == 0) continue;
                    uint32_t off = thread_hour_offsets[thr][h];
                    const FinalItem *items = &thread_partitioned[thr][off];
                    for (uint32_t i = 0; i < cnt; i++) {
                        uint16_t ep = items[i].ep_code & 0x7FFF;
                        uint32_t pos = ep_cur[ep]++;
                        sorted_latencies[pos] = items[i].lat;
                    }
                }

                std::string &out = hour_outputs[h];
                out.reserve(128 * 1024);
                const char *h_str = g_hour_strings[h];

                for (uint16_t ep_id : sorted_endpoint_ids) {
                    uint32_t n = ep_counts[ep_id];
                    if (n == 0) continue;

                    uint32_t start_pos = ep_offsets[ep_id];
                    uint32_t *lats = &sorted_latencies[start_pos];
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

                    uint32_t errs = ep_errors[ep_id];
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

    for (unsigned int t = 0; t < num_threads; t++) {
        munmap(thread_partitioned[t], thread_final_bytes[t]);
    }

    return 0;
}
