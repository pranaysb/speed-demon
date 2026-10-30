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

struct Edge {
    uint32_t target;
    uint32_t weight;
};

// Fast 4-ary Min Heap
struct HeapItem {
    uint64_t dist;
    uint32_t u;
};

class QuadHeap {
public:
    std::vector<HeapItem> data;

    QuadHeap() {
        data.reserve(65536);
    }

    inline bool empty() const { return data.empty(); }
    inline size_t size() const { return data.size(); }
    inline void clear() { data.clear(); }

    inline void push(uint64_t dist, uint32_t u) {
        size_t idx = data.size();
        data.push_back({dist, u});
        sift_up(idx);
    }

    inline HeapItem pop() {
        HeapItem top = data[0];
        HeapItem last = data.back();
        data.pop_back();
        if (!data.empty()) {
            data[0] = last;
            sift_down(0);
        }
        return top;
    }

private:
    inline void sift_up(size_t idx) {
        HeapItem item = data[idx];
        while (idx > 0) {
            size_t p = (idx - 1) >> 2;
            if (item.dist < data[p].dist) {
                data[idx] = data[p];
                idx = p;
            } else {
                break;
            }
        }
        data[idx] = item;
    }

    inline void sift_down(size_t idx) {
        size_t n = data.size();
        HeapItem item = data[idx];
        while (true) {
            size_t child = (idx << 2) + 1;
            if (child >= n) break;

            size_t min_child = child;
            uint64_t min_dist = data[child].dist;

            size_t c2 = child + 1;
            if (c2 < n && data[c2].dist < min_dist) {
                min_dist = data[c2].dist;
                min_child = c2;
            }
            size_t c3 = child + 2;
            if (c3 < n && data[c3].dist < min_dist) {
                min_dist = data[c3].dist;
                min_child = c3;
            }
            size_t c4 = child + 3;
            if (c4 < n && data[c4].dist < min_dist) {
                min_dist = data[c4].dist;
                min_child = c4;
            }

            if (min_dist < item.dist) {
                data[idx] = data[min_child];
                idx = min_child;
            } else {
                break;
            }
        }
        data[idx] = item;
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
    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            data_dir = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        }
    }

    char graph_path[1024];
    snprintf(graph_path, sizeof(graph_path), "%s/graph.txt", data_dir);

    int g_fd = open(graph_path, O_RDONLY);
    if (g_fd < 0) { perror("open graph"); return 1; }

    struct stat st;
    fstat(g_fd, &st);
    size_t g_size = st.st_size;
    const uint8_t *g_data = (const uint8_t *)mmap(NULL, g_size, PROT_READ, MAP_PRIVATE, g_fd, 0);
    if (g_data == MAP_FAILED) { perror("mmap graph"); close(g_fd); return 1; }
    madvise((void *)g_data, g_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    const uint8_t *p = g_data;
    uint32_t num_nodes = 0, num_edges = 0;
    p = fast_parse_u32(p, &num_nodes); p++;
    p = fast_parse_u32(p, &num_edges);
    while (p < g_data + g_size && *p != '\n') p++;
    if (p < g_data + g_size) p++;

    const uint8_t *edges_start = p;
    const uint8_t *edges_end = g_data + g_size;

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<const uint8_t *> chunk_starts(num_threads);
    std::vector<const uint8_t *> chunk_ends(num_threads);
    size_t body_len = edges_end - edges_start;

    for (unsigned int i = 0; i < num_threads; i++) {
        size_t s = (i * body_len) / num_threads;
        const uint8_t *cp = edges_start + s;
        if (i > 0) {
            while (cp < edges_end && *(cp - 1) != '\n') cp++;
        }
        chunk_starts[i] = cp;
        if (i > 0) chunk_ends[i - 1] = cp;
    }
    chunk_ends[num_threads - 1] = edges_end;

    // Pass 1: Count node degrees in parallel
    std::vector<std::vector<uint32_t>> thread_degrees(num_threads);
    std::vector<std::thread> workers;

    for (unsigned int t = 0; t < num_threads; t++) {
        thread_degrees[t].resize(num_nodes, 0);
        workers.emplace_back([t, &thread_degrees, &chunk_starts, &chunk_ends]() {
            const uint8_t *cp = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            auto &deg = thread_degrees[t];

            while (cp < cend) {
                uint32_t u, v, w;
                cp = fast_parse_u32(cp, &u); cp++;
                cp = fast_parse_u32(cp, &v); cp++;
                cp = fast_parse_u32(cp, &w);
                if (cp < cend && *cp == '\r') cp++;
                if (cp < cend && *cp == '\n') cp++;
                deg[u]++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Compute CSR offsets
    std::vector<uint32_t> offsets(num_nodes + 1, 0);
    std::vector<std::vector<uint32_t>> thread_offsets(num_threads);
    for (unsigned int t = 0; t < num_threads; t++) thread_offsets[t].resize(num_nodes, 0);

    for (uint32_t u = 0; u < num_nodes; u++) {
        uint32_t d_total = 0;
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_offsets[t][u] = d_total;
            d_total += thread_degrees[t][u];
        }
        offsets[u + 1] = offsets[u] + d_total;
    }

    for (uint32_t u = 0; u < num_nodes; u++) {
        uint32_t base = offsets[u];
        for (unsigned int t = 0; t < num_threads; t++) {
            thread_offsets[t][u] += base;
        }
    }

    // Pass 2: Write edges directly into CSR in parallel (no intermediate raw_edges)
    std::vector<Edge> edges(num_edges);
    for (unsigned int t = 0; t < num_threads; t++) {
        workers.emplace_back([t, &thread_offsets, &edges, &chunk_starts, &chunk_ends]() {
            const uint8_t *cp = chunk_starts[t];
            const uint8_t *cend = chunk_ends[t];
            auto &wr = thread_offsets[t];

            while (cp < cend) {
                uint32_t u, v, w;
                cp = fast_parse_u32(cp, &u); cp++;
                cp = fast_parse_u32(cp, &v); cp++;
                cp = fast_parse_u32(cp, &w);
                if (cp < cend && *cp == '\r') cp++;
                if (cp < cend && *cp == '\n') cp++;
                uint32_t pos = wr[u]++;
                edges[pos] = {v, w};
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    munmap((void *)g_data, g_size);
    close(g_fd);

    // Read queries
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);
    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }

    fstat(q_fd, &st);
    size_t q_size = st.st_size;
    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_PRIVATE, q_fd, 0);

    struct Query { uint32_t s; uint32_t t; };
    std::vector<Query> queries;
    queries.reserve(2048);
    p = q_data; const uint8_t *q_end = q_data + q_size;
    while (p < q_end) {
        while (p < q_end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
        if (p >= q_end) break;
        Query q;
        p = fast_parse_u32(p, &q.s); while (p < q_end && *p == ' ') p++;
        p = fast_parse_u32(p, &q.t); while (p < q_end && *p != '\n') p++;
        if (p < q_end && *p == '\n') p++;
        queries.push_back(q);
    }
    munmap((void *)q_data, q_size);
    close(q_fd);

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);
    size_t chunk_size = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * chunk_size;
        size_t end_q = std::min(start_q + chunk_size, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([&, start_q, end_q]() {
            std::vector<uint64_t> dist(num_nodes, UINT64_MAX);
            std::vector<uint32_t> touched;
            touched.reserve(32768);
            QuadHeap pq;

            for (size_t qid = start_q; qid < end_q; qid++) {
                uint32_t s = queries[qid].s;
                uint32_t t = queries[qid].t;
                if (s >= num_nodes || t >= num_nodes) { answers[qid] = -1; continue; }
                if (s == t) { answers[qid] = 0; continue; }

                dist[s] = 0;
                touched.push_back(s);
                pq.push(0, s);
                int64_t found_dist = -1;

                while (!pq.empty()) {
                    HeapItem top = pq.pop();
                    uint64_t d = top.dist;
                    uint32_t u = top.u;

                    if (d > dist[u]) continue;
                    if (u == t) { found_dist = (int64_t)d; break; }

                    uint32_t edge_start = offsets[u];
                    uint32_t edge_end = offsets[u + 1];
                    for (uint32_t e = edge_start; e < edge_end; e++) {
                        uint32_t v = edges[e].target;
                        uint64_t nd = d + edges[e].weight;
                        if (nd < dist[v]) {
                            if (dist[v] == UINT64_MAX) touched.push_back(v);
                            dist[v] = nd;
                            pq.push(nd, v);
                        }
                    }
                }

                answers[qid] = found_dist;
                for (uint32_t x : touched) dist[x] = UINT64_MAX;
                touched.clear();
                pq.clear();
            }
        });
    }
    for (auto &w : workers) w.join();

    // Write output
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/distances.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen out"); return 1; }
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    char line[32];
    for (size_t i = 0; i < num_queries; i++) {
        char *lp = fast_i64(line, answers[i]);
        *lp++ = '\n';
        fwrite(line, 1, lp - line, out);
    }
    fclose(out);
    free(out_buf);

    return 0;
}
