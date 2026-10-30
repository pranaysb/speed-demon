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
#include <string_view>
#include <thread>

// Pre-computed hour ISO strings: 72 hours from 2026-10-30T18:00:00Z to 2026-11-02T17:00:00Z
static char g_hour_strings[72][24];

static void init_hour_strings() {
    int hour_idx = 0;
    // 2026-10-30: hours 18..23
    for (int h = 18; h <= 23; h++) {
        snprintf(g_hour_strings[hour_idx++], sizeof(g_hour_strings[0]), "2026-10-30T%02d:00:00Z", h);
    }
    // 2026-10-31: hours 0..23
    for (int h = 0; h <= 23; h++) {
        snprintf(g_hour_strings[hour_idx++], sizeof(g_hour_strings[0]), "2026-10-31T%02d:00:00Z", h);
    }
    // 2026-11-01: hours 0..23
    for (int h = 0; h <= 23; h++) {
        snprintf(g_hour_strings[hour_idx++], sizeof(g_hour_strings[0]), "2026-11-01T%02d:00:00Z", h);
    }
    // 2026-11-02: hours 0..17
    for (int h = 0; h <= 17; h++) {
        snprintf(g_hour_strings[hour_idx++], sizeof(g_hour_strings[0]), "2026-11-02T%02d:00:00Z", h);
    }
}

// Fast decode of hour_idx from timestamp prefix: YYYY-MM-DDTHH...
// 27-byte timestamp: 2026-10-30T18:00:00.000000Z
static inline int parse_hour_idx(const uint8_t *ts) {
    int day_offset = 0;
    if (ts[5] == '1' && ts[6] == '0') {
        // Month 10
        if (ts[8] == '3') {
            if (ts[9] == '0') day_offset = 0;
            else day_offset = 1; // 31
        }
    } else {
        // Month 11
        if (ts[9] == '1') day_offset = 2; // 01
        else day_offset = 3; // 02
    }
    int hh = (ts[11] - '0') * 10 + (ts[12] - '0');
    return day_offset * 24 + hh - 18;
}

// Global interned endpoints catalogue (up to 2048 distinct endpoints)
static constexpr size_t MAX_ENDPOINTS = 2048;
static std::string g_endpoint_strings[MAX_ENDPOINTS];
static size_t g_num_endpoints = 0;

// Flat hash map for endpoint interning
struct EndpointEntry {
    uint64_t hash;
    const char *str;
    uint16_t len;
    uint16_t id;
};

static constexpr size_t EP_MAP_CAP = 8192;
static constexpr size_t EP_MAP_MASK = EP_MAP_CAP - 1;
static EndpointEntry g_ep_map[EP_MAP_CAP];

static inline uint64_t hash_endpoint(const char *p, size_t len) {
    uint64_t h = len ^ 0xa0761d6478bd642fULL;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)p[i]) * 0x100000001b3ULL;
    }
    return h;
}

static uint16_t get_or_insert_endpoint(const char *p, size_t len) {
    uint64_t h = hash_endpoint(p, len);
    size_t idx = h & EP_MAP_MASK;
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

// Group accumulators
struct GroupData {
    std::vector<uint32_t> latencies;
    uint32_t count = 0;
    uint32_t errors = 0;
};

// 72 hours * MAX_ENDPOINTS
static GroupData g_groups[72][MAX_ENDPOINTS];

int main(int argc, char **argv) {
    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        }
    }

    init_hour_strings();

    char input_path[1024];
    snprintf(input_path, sizeof(input_path), "%s/requests.log", data_dir);

    int fd = open(input_path, O_RDONLY);
    if (fd < 0) {
        perror("open input");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 1;
    }
    madvise((void *)data, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    const uint8_t *p = data;
    const uint8_t *end = data + file_size;

    while (p < end) {
        // Line format: ts SP method SP endpoint SP status SP latency LF
        // ts: 27 bytes
        int hour_idx = parse_hour_idx(p);
        p += 28; // skip ts and space

        // Skip method
        while (*p != ' ') p++;
        p++; // skip space

        // Endpoint
        const char *ep_start = (const char *)p;
        while (*p != ' ') p++;
        size_t ep_len = (const char *)p - ep_start;
        p++; // skip space

        uint16_t ep_id = get_or_insert_endpoint(ep_start, ep_len);

        // Status: 3 digits
        int status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');
        bool is_error = (status >= 500 && status <= 599);
        p += 4; // 3 digits + space

        // Latency: integer
        uint32_t lat = 0;
        while (*p >= '0' && *p <= '9') {
            lat = lat * 10 + (*p - '0');
            p++;
        }
        if (*p == '\r') p++;
        if (*p == '\n') p++;

        // Accumulate into group
        GroupData &g = g_groups[hour_idx][ep_id];
        g.count++;
        if (is_error) g.errors++;
        g.latencies.push_back(lat);
    }

    // Sort endpoints lexicographically to output in exact specification order
    // Order: hour ascending, then endpoint ascending as unsigned bytes
    std::vector<uint16_t> sorted_endpoint_ids(g_num_endpoints);
    for (size_t i = 0; i < g_num_endpoints; i++) {
        sorted_endpoint_ids[i] = (uint16_t)i;
    }
    std::sort(sorted_endpoint_ids.begin(), sorted_endpoint_ids.end(), [](uint16_t a, uint16_t b) {
        return g_endpoint_strings[a] < g_endpoint_strings[b];
    });

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/summary.txt", out_dir);

    // Create dir if needed
    mkdir(out_dir, 0777);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen out");
        return 1;
    }

    // Set large 1MB stream write buffer
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (int h = 0; h < 72; h++) {
        const char *h_str = g_hour_strings[h];
        for (uint16_t ep_id : sorted_endpoint_ids) {
            GroupData &g = g_groups[h][ep_id];
            if (g.count == 0) continue;

            std::sort(g.latencies.begin(), g.latencies.end());

            uint32_t n = g.count;
            // 1-indexed rank: (p * n + 99) / 100
            uint32_t rank50 = (50 * n + 99) / 100;
            uint32_t rank95 = (95 * n + 99) / 100;
            uint32_t rank99 = (99 * n + 99) / 100;

            uint32_t p50 = g.latencies[rank50 - 1];
            uint32_t p95 = g.latencies[rank95 - 1];
            uint32_t p99 = g.latencies[rank99 - 1];

            uint64_t ppm = (uint64_t)g.errors * 1000000ULL / n;

            fprintf(out, "%s %s %u %u %u %u %u %llu\n",
                    h_str,
                    g_endpoint_strings[ep_id].c_str(),
                    n,
                    p50, p95, p99,
                    g.errors,
                    (unsigned long long)ppm);
        }
    }

    fclose(out);
    free(out_buf);
    munmap((void *)data, file_size);
    close(fd);

    return 0;
}
