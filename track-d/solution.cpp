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

    std::vector<uint32_t> f_offsets(num_nodes + 1, 0);
    std::vector<uint32_t> b_offsets(num_nodes + 1, 0);

    const uint8_t *ep = edges_start;
    while (ep < edges_end) {
        uint32_t u, v;
        ep = fast_parse_u32(ep, &u); ep++;
        ep = fast_parse_u32(ep, &v);
        f_offsets[u + 1]++;
        b_offsets[v + 1]++;
        while (ep < edges_end && *ep != '\n') ep++;
        if (ep < edges_end) ep++;
    }
    for (uint32_t i = 0; i < num_nodes; i++) {
        f_offsets[i + 1] += f_offsets[i];
        b_offsets[i + 1] += b_offsets[i];
    }

    std::vector<uint32_t> f_cur = f_offsets;
    std::vector<uint32_t> b_cur = b_offsets;
    std::vector<Edge> f_edges(num_edges);
    std::vector<Edge> b_edges(num_edges);

    ep = edges_start;
    while (ep < edges_end) {
        uint32_t u, v, w;
        ep = fast_parse_u32(ep, &u); ep++;
        ep = fast_parse_u32(ep, &v); ep++;
        ep = fast_parse_u32(ep, &w);
        if (ep < edges_end && *ep == '\r') ep++;
        if (ep < edges_end && *ep == '\n') ep++;
        f_edges[f_cur[u]++] = {v, w};
        b_edges[b_cur[v]++] = {u, w};
    }
    munmap((void *)g_data, g_size);
#if defined(__linux__)
    posix_fadvise(g_fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
    close(g_fd);
    f_cur.clear(); f_cur.shrink_to_fit();
    b_cur.clear(); b_cur.shrink_to_fit();

    // BFS component labeling in uint8_t
    std::vector<uint8_t> comp(num_nodes, 0);
    std::vector<uint32_t> q(num_nodes);
    uint8_t current_comp = 0;

    for (uint32_t root = 0; root < num_nodes; root++) {
        if (comp[root] != 0) continue;
        current_comp++;
        size_t head = 0, tail = 0;
        comp[root] = current_comp;
        q[tail++] = root;

        while (head < tail) {
            uint32_t u = q[head++];
            for (uint32_t e = f_offsets[u], ee = f_offsets[u + 1]; e < ee; e++) {
                uint32_t v = f_edges[e].target;
                if (comp[v] == 0) { comp[v] = current_comp; q[tail++] = v; }
            }
            for (uint32_t e = b_offsets[u], ee = b_offsets[u + 1]; e < ee; e++) {
                uint32_t v = b_edges[e].target;
                if (comp[v] == 0) { comp[v] = current_comp; q[tail++] = v; }
            }
        }
    }
    q.clear(); q.shrink_to_fit();

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

    unsigned int num_threads = 4;
    std::vector<std::thread> workers;
    size_t chunk_size = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * chunk_size;
        size_t end_q = std::min(start_q + chunk_size, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([&, start_q, end_q]() {
            size_t dist_bytes = (num_nodes + 1) * sizeof(uint64_t);
            uint64_t *dist_f = (uint64_t *)mmap(NULL, dist_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
            uint64_t *dist_b = (uint64_t *)mmap(NULL, dist_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

            std::vector<uint32_t> touched_f; touched_f.reserve(16384);
            std::vector<uint32_t> touched_b; touched_b.reserve(16384);
            QuadHeap pq_f, pq_b;

            for (size_t qid = start_q; qid < end_q; qid++) {
                uint32_t s = queries[qid].s;
                uint32_t t = queries[qid].t;
                if (s >= num_nodes || t >= num_nodes) { answers[qid] = -1; continue; }
                if (s == t) { answers[qid] = 0; continue; }

                if (f_offsets[s + 1] == f_offsets[s] || b_offsets[t + 1] == b_offsets[t] || comp[s] != comp[t]) {
                    answers[qid] = -1;
                    continue;
                }

                dist_f[s] = 1; touched_f.push_back(s); pq_f.push(0, s);
                dist_b[t] = 1; touched_b.push_back(t); pq_b.push(0, t);

                uint64_t best_dist = UINT64_MAX;

                while (!pq_f.empty() && !pq_b.empty()) {
                    if (pq_f.top_dist() + pq_b.top_dist() >= best_dist) break;

                    if (pq_f.top_dist() <= pq_b.top_dist()) {
                        HeapItem item = pq_f.pop();
                        uint64_t d = item.dist;
                        uint32_t u = item.u;
                        if (d + 1 > dist_f[u]) continue;

                        uint32_t es = f_offsets[u], ee = f_offsets[u + 1];
                        for (uint32_t e = es; e < ee; e++) {
                            uint32_t v = f_edges[e].target;
                            uint64_t nd = d + f_edges[e].weight;
                            if (nd >= best_dist) continue;
                            uint64_t cur_v = dist_f[v];
                            if (cur_v == 0 || nd + 1 < cur_v) {
                                if (cur_v == 0) touched_f.push_back(v);
                                dist_f[v] = nd + 1;
                                pq_f.push(nd, v);
                                if (dist_b[v] != 0) {
                                    uint64_t cand = nd + (dist_b[v] - 1);
                                    if (cand < best_dist) best_dist = cand;
                                }
                            }
                        }
                    } else {
                        HeapItem item = pq_b.pop();
                        uint64_t d = item.dist;
                        uint32_t u = item.u;
                        if (d + 1 > dist_b[u]) continue;

                        uint32_t es = b_offsets[u], ee = b_offsets[u + 1];
                        for (uint32_t e = es; e < ee; e++) {
                            uint32_t v = b_edges[e].target;
                            uint64_t nd = d + b_edges[e].weight;
                            if (nd >= best_dist) continue;
                            uint64_t cur_v = dist_b[v];
                            if (cur_v == 0 || nd + 1 < cur_v) {
                                if (cur_v == 0) touched_b.push_back(v);
                                dist_b[v] = nd + 1;
                                pq_b.push(nd, v);
                                if (dist_f[v] != 0) {
                                    uint64_t cand = nd + (dist_f[v] - 1);
                                    if (cand < best_dist) best_dist = cand;
                                }
                            }
                        }
                    }
                }

                answers[qid] = (best_dist == UINT64_MAX) ? -1 : (int64_t)best_dist;
                for (uint32_t x : touched_f) dist_f[x] = 0;
                for (uint32_t x : touched_b) dist_b[x] = 0;
                touched_f.clear();
                touched_b.clear();
                pq_f.clear();
                pq_b.clear();
            }

            munmap(dist_f, dist_bytes);
            munmap(dist_b, dist_bytes);
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
