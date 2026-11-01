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
#include <atomic>
#include <chrono>

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
    inline uint64_t top_dist() const { return data[0].dist; }

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

struct DistMap {
    static constexpr size_t CAP = 65536;
    static constexpr size_t MASK = CAP - 1;
    static constexpr uint32_t EMPTY = 0xFFFFFFFF;

    struct Entry {
        uint32_t node;
        uint64_t dist;
    };
    Entry *table;
    std::vector<uint32_t> touched;

    DistMap() {
        table = (Entry *)malloc(CAP * sizeof(Entry));
        for (size_t i = 0; i < CAP; i++) table[i].node = EMPTY;
        touched.reserve(16384);
    }
    ~DistMap() { free(table); }

    inline void clear() {
        for (uint32_t slot : touched) table[slot].node = EMPTY;
        touched.clear();
    }

    inline uint64_t get(uint32_t u) const {
        size_t idx = (u * 2654435761ULL) & MASK;
        while (table[idx].node != EMPTY) {
            if (table[idx].node == u) return table[idx].dist;
            idx = (idx + 1) & MASK;
        }
        return UINT64_MAX;
    }

    inline bool update(uint32_t u, uint64_t d) {
        size_t idx = (u * 2654435761ULL) & MASK;
        while (table[idx].node != EMPTY) {
            if (table[idx].node == u) {
                if (d < table[idx].dist) {
                    table[idx].dist = d;
                    return true;
                }
                return false;
            }
            idx = (idx + 1) & MASK;
        }
        table[idx] = {u, d};
        touched.push_back((uint32_t)idx);
        return true;
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
    if (v < 0) { *p++ = '-'; v = -v; }
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
    size_t total_bytes = edges_end - edges_start;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);

    for (unsigned int t = 0; t < num_threads; t++) {
        const uint8_t *cs = edges_start + (total_bytes * t) / num_threads;
        if (t > 0) {
            while (cs < edges_end && *(cs - 1) != '\n') cs++;
        }
        chunk_starts[t] = cs;
        if (t > 0) chunk_ends[t - 1] = cs;
    }
    chunk_ends[num_threads - 1] = edges_end;

    std::vector<std::atomic<uint32_t>> f_offsets_atomic(num_nodes + 1);
    std::vector<std::atomic<uint32_t>> b_offsets_atomic(num_nodes + 1);
    for (uint32_t i = 0; i <= num_nodes; i++) {
        f_offsets_atomic[i].store(0, std::memory_order_relaxed);
        b_offsets_atomic[i].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> workers;
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &f_offsets_atomic, &b_offsets_atomic]() {
            const uint8_t *cur = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            while (cur < cend) {
                uint32_t u, v;
                cur = fast_parse_u32(cur, &u); cur++;
                cur = fast_parse_u32(cur, &v);
                f_offsets_atomic[u + 1].fetch_add(1, std::memory_order_relaxed);
                b_offsets_atomic[v + 1].fetch_add(1, std::memory_order_relaxed);
                while (cur < cend && *cur != '\n') cur++;
                if (cur < cend) cur++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    std::vector<uint32_t> f_offsets(num_nodes + 1, 0);
    std::vector<uint32_t> b_offsets(num_nodes + 1, 0);
    uint32_t f_tot = 0, b_tot = 0;
    for (uint32_t i = 0; i <= num_nodes; i++) {
        f_tot += f_offsets_atomic[i].load(std::memory_order_relaxed);
        b_tot += b_offsets_atomic[i].load(std::memory_order_relaxed);
        f_offsets[i] = f_tot;
        b_offsets[i] = b_tot;
    }
    std::vector<std::atomic<uint32_t>>().swap(f_offsets_atomic);
    std::vector<std::atomic<uint32_t>>().swap(b_offsets_atomic);

    std::vector<std::atomic<uint32_t>> f_cur(num_nodes);
    std::vector<std::atomic<uint32_t>> b_cur(num_nodes);
    for (uint32_t i = 0; i < num_nodes; i++) {
        f_cur[i].store(f_offsets[i], std::memory_order_relaxed);
        b_cur[i].store(b_offsets[i], std::memory_order_relaxed);
    }

    std::vector<Edge> f_edges(num_edges);
    std::vector<Edge> b_edges(num_edges);

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &f_cur, &b_cur, &f_edges, &b_edges]() {
            const uint8_t *cur = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            while (cur < cend) {
                uint32_t u, v, w;
                cur = fast_parse_u32(cur, &u); cur++;
                cur = fast_parse_u32(cur, &v); cur++;
                cur = fast_parse_u32(cur, &w);
                if (cur < cend && *cur == '\r') cur++;
                if (cur < cend && *cur == '\n') cur++;
                uint32_t pos_f = f_cur[u].fetch_add(1, std::memory_order_relaxed);
                f_edges[pos_f] = {v, w};
                uint32_t pos_b = b_cur[v].fetch_add(1, std::memory_order_relaxed);
                b_edges[pos_b] = {u, w};
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    std::vector<std::atomic<uint32_t>>().swap(f_cur);
    std::vector<std::atomic<uint32_t>>().swap(b_cur);

    // Unmap graph.txt immediately
    madvise((void *)g_data, g_size, MADV_DONTNEED);
    munmap((void *)g_data, g_size);
    close(g_fd);

#if defined(__linux__)
    malloc_trim(0);
#endif

    // Read queries
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);
    int q_fd = open(queries_path, O_RDONLY);
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
    madvise((void *)q_data, q_size, MADV_DONTNEED);
    munmap((void *)q_data, q_size);
    close(q_fd);

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);

    // Dynamic work-stealing queue using full hardware concurrency
    std::atomic<size_t> next_qid{0};
    unsigned int query_threads = num_threads;

    for (unsigned int tid = 0; tid < query_threads; tid++) {
        workers.emplace_back([&]() {
            DistMap map_f, map_b;
            QuadHeap pq_f, pq_b;

            size_t batch_start;
            while ((batch_start = next_qid.fetch_add(16, std::memory_order_relaxed)) < num_queries) {
                size_t batch_end = std::min(batch_start + 16, num_queries);

                for (size_t qid = batch_start; qid < batch_end; qid++) {
                    uint32_t s = queries[qid].s;
                    uint32_t t = queries[qid].t;
                    if (s >= num_nodes || t >= num_nodes) { answers[qid] = -1; continue; }
                    if (s == t) { answers[qid] = 0; continue; }

                    if (f_offsets[s + 1] == f_offsets[s] || b_offsets[t + 1] == b_offsets[t]) {
                        answers[qid] = -1;
                        continue;
                    }

                    map_f.update(s, 0); pq_f.push(0, s);
                    map_b.update(t, 0); pq_b.push(0, t);

                    uint64_t best_dist = UINT64_MAX;

                    while (!pq_f.empty() && !pq_b.empty()) {
                        if (pq_f.top_dist() + pq_b.top_dist() >= best_dist) break;

                        if (pq_f.sz <= pq_b.sz) {
                            HeapItem item = pq_f.pop();
                            uint64_t d = item.dist;
                            uint32_t u = item.u;
                            if (d > map_f.get(u)) continue;

                            uint32_t es = f_offsets[u], ee = f_offsets[u + 1];
                            for (uint32_t e = es; e < ee; e++) {
                                uint32_t v = f_edges[e].target;
                                uint64_t nd = d + f_edges[e].weight;
                                if (nd >= best_dist) continue;

                                if (map_f.update(v, nd)) {
                                    pq_f.push(nd, v);
                                    uint64_t bd = map_b.get(v);
                                    if (bd != UINT64_MAX) {
                                        uint64_t cand = nd + bd;
                                        if (cand < best_dist) best_dist = cand;
                                    }
                                }
                            }
                        } else {
                            HeapItem item = pq_b.pop();
                            uint64_t d = item.dist;
                            uint32_t u = item.u;
                            if (d > map_b.get(u)) continue;

                            uint32_t es = b_offsets[u], ee = b_offsets[u + 1];
                            for (uint32_t e = es; e < ee; e++) {
                                uint32_t v = b_edges[e].target;
                                uint64_t nd = d + b_edges[e].weight;
                                if (nd >= best_dist) continue;

                                if (map_b.update(v, nd)) {
                                    pq_b.push(nd, v);
                                    uint64_t fd = map_f.get(v);
                                    if (fd != UINT64_MAX) {
                                        uint64_t cand = nd + fd;
                                        if (cand < best_dist) best_dist = cand;
                                    }
                                }
                            }
                        }
                    }

                    answers[qid] = (best_dist == UINT64_MAX) ? -1 : (int64_t)best_dist;
                    map_f.clear();
                    map_b.clear();
                    pq_f.clear();
                    pq_b.clear();
                }
            }
        });
    }
    for (auto &w : workers) w.join();

    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/distances.txt", out_dir);

    FILE *fout = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(fout, out_buf, _IOFBF, 1024 * 1024);

    char line_buf[32];
    for (size_t qid = 0; qid < num_queries; qid++) {
        char *cur = fast_i64(line_buf, answers[qid]);
        *cur++ = '\n';
        fwrite(line_buf, 1, cur - line_buf, fout);
    }
    fclose(fout);
    free(out_buf);

    return 0;
}
