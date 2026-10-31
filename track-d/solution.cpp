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
#include <thread>

#if defined(__linux__)
#include <malloc.h>
#endif

struct Edge {
    uint32_t target;
    uint32_t weight;
};

struct HeapItem {
    uint64_t dist;
    uint32_t u;
};

class QuadHeap {
public:
    std::vector<HeapItem> data;
    size_t sz = 0;

    QuadHeap() { data.resize(16384); }
    inline bool empty() const { return sz == 0; }
    inline void clear() { sz = 0; }

    inline void push(uint64_t dist, uint32_t u) {
        if (__builtin_expect(sz >= data.size(), 0)) data.resize(data.size() * 2);
        size_t idx = sz++;
        data[idx] = {dist, u};
        while (idx > 0) {
            size_t p = (idx - 1) >> 2;
            if (dist < data[p].dist) {
                data[idx] = data[p];
                idx = p;
            } else break;
        }
        data[idx] = {dist, u};
    }

    inline HeapItem pop() {
        HeapItem top = data[0];
        HeapItem last = data[--sz];
        if (sz > 0) {
            size_t idx = 0;
            while (true) {
                size_t c = (idx << 2) + 1;
                if (c >= sz) break;
                size_t mc = c;
                uint64_t md = data[c].dist;
                if (c + 1 < sz && data[c + 1].dist < md) { md = data[c + 1].dist; mc = c + 1; }
                if (c + 2 < sz && data[c + 2].dist < md) { md = data[c + 2].dist; mc = c + 2; }
                if (c + 3 < sz && data[c + 3].dist < md) { md = data[c + 3].dist; mc = c + 3; }
                if (md < last.dist) {
                    data[idx] = data[mc];
                    idx = mc;
                } else break;
            }
            data[idx] = last;
        }
        return top;
    }
};

static inline const uint8_t *fast_parse_u32(const uint8_t *p, uint32_t *val) {
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *val = v;
    return p;
}

static inline char* fast_i64(char *p, int64_t v) {
    if (v < 0) {
        *p++ = '-';
        v = -v;
    }
    if (v == 0) {
        *p++ = '0';
        return p;
    }
    char buf[24];
    int i = 0;
    while (v > 0) {
        buf[i++] = (v % 10) + '0';
        v /= 10;
    }
    while (i > 0) *p++ = buf[--i];
    return p;
}

struct CompactDistMap {
    struct Entry {
        uint32_t node;
        uint64_t dist;
    };
    std::vector<Entry> table;
    uint32_t mask;
    uint32_t count = 0;
    static constexpr uint32_t EMPTY = 0xffffffff;

    CompactDistMap(size_t cap = 65536) {
        table.resize(cap, {EMPTY, UINT64_MAX});
        mask = (uint32_t)(cap - 1);
    }

    inline void clear(const std::vector<uint32_t> &touched) {
        for (uint32_t u : touched) {
            uint32_t slot = (u * 0x9e3779b9u) & mask;
            while (table[slot].node != u) {
                slot = (slot + 1) & mask;
            }
            table[slot].node = EMPTY;
            table[slot].dist = UINT64_MAX;
        }
        count = 0;
    }

    inline uint64_t get(uint32_t u) const {
        uint32_t slot = (u * 0x9e3779b9u) & mask;
        while (true) {
            uint32_t n = table[slot].node;
            if (n == u) return table[slot].dist;
            if (n == EMPTY) return UINT64_MAX;
            slot = (slot + 1) & mask;
        }
    }

    inline bool update_if_less(uint32_t u, uint64_t d, std::vector<uint32_t> &touched) {
        if (__builtin_expect(count * 2 >= table.size(), 0)) {
            size_t new_cap = table.size() * 2;
            std::vector<Entry> new_table(new_cap, {EMPTY, UINT64_MAX});
            uint32_t new_mask = (uint32_t)(new_cap - 1);
            for (uint32_t x : touched) {
                uint64_t old_d = get(x);
                uint32_t slot = (x * 0x9e3779b9u) & new_mask;
                while (new_table[slot].node != EMPTY) slot = (slot + 1) & new_mask;
                new_table[slot] = {x, old_d};
            }
            table = std::move(new_table);
            mask = new_mask;
        }

        uint32_t slot = (u * 0x9e3779b9u) & mask;
        while (true) {
            uint32_t n = table[slot].node;
            if (n == u) {
                if (d < table[slot].dist) {
                    table[slot].dist = d;
                    return true;
                }
                return false;
            }
            if (n == EMPTY) {
                table[slot] = {u, d};
                touched.push_back(u);
                count++;
                return true;
            }
            slot = (slot + 1) & mask;
        }
    }
};

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

    char graph_path[1024];
    snprintf(graph_path, sizeof(graph_path), "%s/graph.txt", data_dir);

    int g_fd = open(graph_path, O_RDONLY);
    if (g_fd < 0) { perror("open graph"); return 1; }

    struct stat st;
    fstat(g_fd, &st);
    size_t g_size = st.st_size;
    const uint8_t *g_data = (const uint8_t *)mmap(NULL, g_size, PROT_READ, MAP_SHARED, g_fd, 0);

    const uint8_t *p = g_data;
    uint32_t num_nodes = 0, num_edges = 0;
    p = fast_parse_u32(p, &num_nodes); p++;
    p = fast_parse_u32(p, &num_edges);
    while (p < g_data + g_size && *p != '\n') p++;
    if (p < g_data + g_size) p++;

    const uint8_t *edges_start = p;
    const uint8_t *edges_end = g_data + g_size;

    std::vector<uint32_t> offsets(num_nodes + 1, 0);
    const uint8_t *ep = edges_start;
    while (ep < edges_end) {
        uint32_t u;
        ep = fast_parse_u32(ep, &u);
        offsets[u + 1]++;
        while (ep < edges_end && *ep != '\n') ep++;
        if (ep < edges_end) ep++;
    }
    for (uint32_t i = 0; i < num_nodes; i++) offsets[i + 1] += offsets[i];

    std::vector<uint32_t> cur = offsets;
    std::vector<Edge> edges(num_edges);

    ep = edges_start;
    while (ep < edges_end) {
        uint32_t u, v, w;
        ep = fast_parse_u32(ep, &u); ep++;
        ep = fast_parse_u32(ep, &v); ep++;
        ep = fast_parse_u32(ep, &w);
        if (ep < edges_end && *ep == '\r') ep++;
        if (ep < edges_end && *ep == '\n') ep++;
        edges[cur[u]++] = {v, w};
    }
    munmap((void *)g_data, g_size);
#if defined(__linux__)
    posix_fadvise(g_fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(g_fd);
    cur.clear(); cur.shrink_to_fit();

    // Read queries
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);
    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }

    fstat(q_fd, &st);
    size_t q_size = st.st_size;
    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_SHARED, q_fd, 0);

    struct Query { uint32_t s; uint32_t t; };
    std::vector<Query> queries;
    queries.reserve(2048);
    p = q_data; const uint8_t *q_end = q_data + q_size;
    while (p < q_end) {
        while (p < q_end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
        if (p >= q_end) break;
        Query q_item;
        p = fast_parse_u32(p, &q_item.s); while (p < q_end && *p == ' ') p++;
        p = fast_parse_u32(p, &q_item.t); while (p < q_end && *p != '\n') p++;
        if (p < q_end && *p == '\n') p++;
        queries.push_back(q_item);
    }
    munmap((void *)q_data, q_size);
#if defined(__linux__)
    posix_fadvise(q_fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(q_fd);

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<std::thread> workers;
    size_t chunk_size = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * chunk_size;
        size_t end_q = std::min(start_q + chunk_size, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([&, start_q, end_q]() {
            CompactDistMap dist_map(65536);
            std::vector<uint32_t> touched;
            touched.reserve(16384);
            QuadHeap pq;

            for (size_t qid = start_q; qid < end_q; qid++) {
                uint32_t s = queries[qid].s;
                uint32_t t = queries[qid].t;
                if (s >= num_nodes || t >= num_nodes) { answers[qid] = -1; continue; }
                if (s == t) { answers[qid] = 0; continue; }

                if (offsets[s + 1] == offsets[s]) {
                    answers[qid] = -1;
                    continue;
                }

                dist_map.update_if_less(s, 0, touched);
                pq.push(0, s);

                uint64_t ans = UINT64_MAX;

                while (!pq.empty()) {
                    HeapItem top = pq.pop();
                    uint64_t d = top.dist;
                    uint32_t u = top.u;

                    if (d > dist_map.get(u)) continue;
                    if (u == t) {
                        ans = d;
                        break;
                    }

                    uint32_t es = offsets[u], ee = offsets[u + 1];
                    for (uint32_t e = es; e < ee; e++) {
                        uint32_t v = edges[e].target;
                        uint64_t nd = d + edges[e].weight;

                        if (dist_map.update_if_less(v, nd, touched)) {
                            pq.push(nd, v);
                        }
                    }
                }

                answers[qid] = (ans == UINT64_MAX) ? -1 : (int64_t)ans;
                dist_map.clear(touched);
                touched.clear();
                pq.clear();
            }
        });
    }
    for (auto &w : workers) w.join();

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/distances.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen out"); return 1; }
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    char row[32];
    for (size_t i = 0; i < num_queries; i++) {
        char *rp = fast_i64(row, answers[i]);
        *rp++ = '\n';
        fwrite(row, 1, rp - row, out);
    }
    fclose(out);
    free(out_buf);

    return 0;
}
