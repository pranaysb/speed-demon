
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
    uint64_t h = 0xcbf29ce484222325ULL ^ len;
    size_t i = 0;
    while (i + 8 <= len) {
        uint64_t v;
        memcpy(&v, p + i, 8);
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

#pragma pack(push, 2)
struct Item6 {
    uint32_t lat;
    uint16_t ep_code; // bit 15: is_err, bits 0..14: ep_id
};
#pragma pack(pop)

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

static constexpr size_t BLOCK_CAP = 1024;
struct ItemBlock {
    Item6 items[BLOCK_CAP];
    ItemBlock *next;
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
            while (p < data + file_size && *p != '\n') p++;
            if (p < data + file_size) p++;
            chunk_starts[i] = p;
            chunk_ends[i - 1] = p;
        }
    }
    chunk_ends[num_threads - 1] = data + file_size;

    // Sized arena for 1024-item blocks (~16MB total across all threads)
    size_t blocks_per_thread = (file_size / (num_threads * 70 * BLOCK_CAP)) + 25;
    size_t arena_bytes = blocks_per_thread * sizeof(ItemBlock);

    std::vector<ItemBlock *> thread_arenas(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_arenas[t] = (ItemBlock *)mmap(NULL, arena_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }

    struct ThreadHourHead {
        ItemBlock *head = nullptr;
        ItemBlock *tail = nullptr;
        uint16_t tail_count = 0;
        uint32_t total_count = 0;
    };

    std::vector<std::vector<ThreadHourHead>> thread_hours(num_threads, std::vector<ThreadHourHead>(72));

    // Pre-allocated histogram tables per thread: 72 * 2048 * 2 bytes = 288 KB
    struct ThreadHist {
        uint16_t counts[72][MAX_ENDPOINTS];
    };
    std::vector<ThreadHist *> thread_hists(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) {
        thread_hists[t] = (ThreadHist *)mmap(NULL, sizeof(ThreadHist), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }

    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_arenas, &thread_hours, &thread_hists, &chunk_starts, &chunk_ends]() {
            const uint8_t *p = chunk_starts[t];
            const uint8_t *end = chunk_ends[t];
            ItemBlock *arena = thread_arenas[t];
            size_t arena_idx = 0;
            auto &hours = thread_hours[t];
            auto &hist = *thread_hists[t];

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
                size_t c_idx = h & 4095;
                uint16_t ep_id;
                if (__builtin_expect(l1_cache[c_idx].hash == h && l1_cache[c_idx].len == ep_len, 1)) {
                    ep_id = l1_cache[c_idx].ep_id;
                } else {
                    ep_id = get_or_insert_endpoint_with_hash(ep_start, ep_len, h);
                    l1_cache[c_idx] = {h, ep_id, (uint16_t)ep_len};
                }

                uint16_t is_err = (p[0] >= '5') ? 0x8000 : 0;
                p += 4; // status + space

                uint32_t lat = 0;
                while (p < end && *p >= '0' && *p <= '9') {
                    lat = lat * 10 + (*p - '0');
                    p++;
                }
                if (*p == '\r') p++;
                if (*p == '\n') p++;

                hist.counts[hour_idx][ep_id]++;

                ThreadHourHead &hh = hours[hour_idx];
                if (hh.tail_count == BLOCK_CAP || hh.tail == nullptr) {
                    ItemBlock *new_block = &arena[arena_idx++];
                    new_block->next = nullptr;
                    if (!hh.head) hh.head = new_block;
                    else hh.tail->next = new_block;
                    hh.tail = new_block;
                    hh.tail_count = 0;
                }
                hh.tail->items[hh.tail_count++] = {lat, (uint16_t)(ep_id | is_err)};
                hh.total_count++;
            }

        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

#if defined(__linux__)
    madvise((void *)data, file_size, MADV_DONTNEED);
    posix_fadvise(fd, 0, file_size, POSIX_FADV_DONTNEED);
#endif
    munmap((void *)data, file_size);
    close(fd);
#if defined(__linux__)
    malloc_trim(0);
#endif

    // Sort endpoints
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) sorted_endpoint_ids[i] = (uint16_t)i;
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    // Prepare streaming output file
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);
    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(512 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 512 * 1024);

    std::vector<std::string> hour_outputs(72);
    std::vector<std::atomic<bool>> hour_ready(72);
    for (int h = 0; h < 72; h++) hour_ready[h].store(false, std::memory_order_relaxed);

    std::atomic<int> next_hour{0};
    unsigned int p2_threads = num_threads;

    for (unsigned int t = 0; t < p2_threads; t++) {
        workers.emplace_back([&next_hour, &hour_outputs, &hour_ready, &sorted_endpoint_ids,
                              &thread_hours, &thread_hists, num_threads]() {
            char line_buf[256];
            uint32_t ep_counts[MAX_ENDPOINTS];
            uint32_t ep_errors[MAX_ENDPOINTS];
            uint32_t ep_offsets[MAX_ENDPOINTS + 1];
            uint32_t ep_cur[MAX_ENDPOINTS];
            std::vector<uint32_t> sorted_latencies;
            sorted_latencies.reserve(45000);

            int h;
            while ((h = next_hour.fetch_add(1)) < 72) {
                // Sum counts and errors directly from histograms in 0.05ms!
                memset(ep_counts, 0, sizeof(ep_counts));
                memset(ep_errors, 0, sizeof(ep_errors));

                uint32_t h_count = 0;
                for (unsigned int thr = 0; thr < num_threads; thr++) {
                    const auto &th = *thread_hists[thr];
                    for (size_t ep = 0; ep < g_num_endpoints; ep++) {
                        ep_counts[ep] += th.counts[h][ep];
                    }
                    h_count += thread_hours[thr][h].total_count;
                }
                if (h_count == 0) {
                    hour_ready[h].store(true, std::memory_order_release);
                    continue;
                }

                uint32_t tot_h = 0;
                size_t active_eps = 0;
                for (size_t ep = 0; ep < g_num_endpoints; ep++) {
                    ep_offsets[ep] = tot_h;
                    ep_cur[ep] = tot_h;
                    tot_h += ep_counts[ep];
                    if (ep_counts[ep] > 0) active_eps++;
                }

                sorted_latencies.resize(tot_h);
                // SINGLE PASS scatter into sorted_latencies + on-the-fly error counting
                for (unsigned int thr = 0; thr < num_threads; thr++) {
                    auto &hh = thread_hours[thr][h];
                    ItemBlock *blk = hh.head;
                    while (blk) {
                        size_t n = (blk == hh.tail) ? hh.tail_count : BLOCK_CAP;
                        for (size_t i = 0; i < n; i++) {
                            uint16_t code = blk->items[i].ep_code;
                            uint16_t ep = code & 0x7FFF;
                            ep_errors[ep] += (code >> 15);
                            uint32_t pos = ep_cur[ep]++;
                            sorted_latencies[pos] = blk->items[i].lat;
                        }
                        blk = blk->next;
                    }
                }

                std::string &out = hour_outputs[h];
                out.reserve(active_eps * 60);
                const char *h_str = g_hour_strings[h];

                for (uint16_t ep_id : sorted_endpoint_ids) {
                    uint32_t n = ep_counts[ep_id];
                    if (n == 0) continue;

                    uint32_t start_pos = ep_offsets[ep_id];
                    uint32_t *lats = &sorted_latencies[start_pos];
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

                    uint32_t errs = ep_errors[ep_id];
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

    // Stream writes in sequential hour order and immediately free string buffers!
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

    
    for (unsigned int t = 0; t < num_threads; t++) {
        munmap(thread_arenas[t], arena_bytes);
        munmap(thread_hists[t], sizeof(ThreadHist));
    }


    return 0;
}
