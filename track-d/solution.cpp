
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

struct Edge32 {
    uint32_t u;
    uint32_t v;
    uint32_t w;
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
    while (*p >= '\x30' && *p <= '\x39') {
        v = v * 10 + (*p - '\x30');
        p++;
    }
    *val = v;
    return p;
}

static inline char* fast_i64(char *p, int64_t v) {
    if (v < 0) { *p++ = '-'; v = -v; }
    if (v == 0) { *p++ = '\x30'; return p; }
    char buf[24]; int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + '\x30'; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif
    auto t_start = std::chrono::high_resolution_clock::now();

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

    unsigned int num_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);

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

    // 1. Parallel line counting
    std::vector<size_t> thread_counts(num_threads, 0);
    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &thread_counts]() {
            const uint8_t *cur = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            size_t c = 0;
            while (cur < cend) {
                if (*cur == '\n') c++;
                cur++;
            }
            thread_counts[t] = c;
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    std::vector<size_t> start_positions(num_threads, 0);
    size_t running_pos = 0;
    for (unsigned int t = 0; t < num_threads; t++) {
        start_positions[t] = running_pos;
        running_pos += thread_counts[t];
    }

    // 2. Parse directly into raw_edges
    size_t raw_edge_bytes = (size_t)num_edges * sizeof(Edge32);
    Edge32 *raw_edges = (Edge32 *)mmap(NULL, raw_edge_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &chunk_starts, &chunk_ends, &start_positions, raw_edges]() {
            const uint8_t *cur = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            Edge32 *out = raw_edges + start_positions[t];
            size_t idx = 0;
            while (cur < cend) {
                uint32_t u, v, w;
                cur = fast_parse_u32(cur, &u); cur++;
                cur = fast_parse_u32(cur, &v); cur++;
                cur = fast_parse_u32(cur, &w);
                if (cur < cend && *cur == '\r') cur++;
                if (cur < cend && *cur == '\n') cur++;
                out[idx++] = {u, v, w};
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

#if defined(__linux__)
    posix_fadvise(g_fd, 0, g_size, POSIX_FADV_DONTNEED);
#endif
    munmap((void *)g_data, g_size);
    close(g_fd);

    // 3. Count out-degrees
    std::vector<uint32_t> f_offsets(num_nodes + 2, 0);
    for (size_t i = 0; i < num_edges; i++) {
        f_offsets[raw_edges[i].u + 1]++;
    }
    for (uint32_t i = 0; i <= num_nodes; i++) {
        f_offsets[i + 1] += f_offsets[i];
    }

    std::vector<uint32_t> cur_f = f_offsets;
    std::vector<Edge> f_edges(num_edges);

    for (size_t i = 0; i < num_edges; i++) {
        uint32_t u = raw_edges[i].u;
        f_edges[cur_f[u]++] = {raw_edges[i].v, raw_edges[i].w};
    }

    // Now build backward CSR directly from f_edges, WITHOUT raw_edges!
    // Free raw_edges right now before allocating b_edges!
    munmap(raw_edges, raw_edge_bytes);

    std::vector<uint32_t> b_offsets(num_nodes + 2, 0);
    for (size_t i = 0; i < num_edges; i++) {
        b_offsets[f_edges[i].target + 1]++;
    }
    for (uint32_t i = 0; i <= num_nodes; i++) {
        b_offsets[i + 1] += b_offsets[i];
    }

    std::vector<uint32_t> cur_b = b_offsets;
    std::vector<Edge> b_edges(num_edges);

    for (uint32_t u = 0; u < num_nodes; u++) {
        uint32_t start_idx = f_offsets[u];
        uint32_t end_idx = f_offsets[u + 1];
        for (uint32_t e = start_idx; e < end_idx; e++) {
            uint32_t v = f_edges[e].target;
            uint32_t w = f_edges[e].weight;
            b_edges[cur_b[v]++] = {u, w};
        }
    }

#if defined(__linux__)
    malloc_trim(0);
#endif

    auto t_graph_done = std::chrono::high_resolution_clock::now();
    double d_graph = std::chrono::duration<double, std::milli>(t_graph_done - t_start).count();
    printf("Two-Stage Graph Ingestion: %.2f ms\n", d_graph);

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
#if defined(__linux__)
    posix_fadvise(q_fd, 0, q_size, POSIX_FADV_DONTNEED);
#endif
    munmap((void *)q_data, q_size);
    close(q_fd);

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);

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
                                    if (bd != UINT64_MAX && nd + bd < best_dist) {
                                        best_dist = nd + bd;
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
                                    uint64_t fd_dist = map_f.get(v);
                                    if (fd_dist != UINT64_MAX && nd + fd_dist < best_dist) {
                                        best_dist = nd + fd_dist;
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

    char line_buf[64];
    for (size_t qid = 0; qid < num_queries; qid++) {
        char *ptr = fast_i64(line_buf, answers[qid]);
        *ptr++ = '\n';
        fwrite(line_buf, 1, ptr - line_buf, fout);
    }
    fclose(fout);
    free(out_buf);

    auto t_all_done = std::chrono::high_resolution_clock::now();
    double d_all = std::chrono::duration<double, std::milli>(t_all_done - t_start).count();
    printf("Total Track D Time: %.2f ms\n", d_all);

    return 0;
}
