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
#include <queue>
#include <thread>

struct Edge {
    uint32_t target;
    uint32_t weight;
};

struct EdgeInput {
    uint32_t u;
    uint32_t v;
    uint32_t w;
};

// Heap item for Dijkstra
struct HeapNode {
    uint64_t dist;
    uint32_t u;

    bool operator>(const HeapNode &other) const {
        return dist > other.dist;
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

    // 1. Read graph.txt
    char graph_path[1024];
    snprintf(graph_path, sizeof(graph_path), "%s/graph.txt", data_dir);

    int g_fd = open(graph_path, O_RDONLY);
    if (g_fd < 0) {
        perror("open graph");
        return 1;
    }

    struct stat st;
    fstat(g_fd, &st);
    size_t g_size = st.st_size;

    const uint8_t *g_data = (const uint8_t *)mmap(NULL, g_size, PROT_READ, MAP_PRIVATE, g_fd, 0);
    if (g_data == MAP_FAILED) {
        perror("mmap graph");
        close(g_fd);
        return 1;
    }
    madvise((void *)g_data, g_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    const uint8_t *p = g_data;
    const uint8_t *end = g_data + g_size;

    uint32_t num_nodes = 0, num_edges = 0;
    p = fast_parse_u32(p, &num_nodes);
    p++; // space
    p = fast_parse_u32(p, &num_edges);
    while (p < end && *p != '\n') p++;
    if (p < end && *p == '\n') p++;

    std::vector<uint32_t> degree(num_nodes, 0);
    std::vector<EdgeInput> raw_edges(num_edges);

    for (uint32_t i = 0; i < num_edges; i++) {
        p = fast_parse_u32(p, &raw_edges[i].u);
        p++;
        p = fast_parse_u32(p, &raw_edges[i].v);
        p++;
        p = fast_parse_u32(p, &raw_edges[i].w);
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        degree[raw_edges[i].u]++;
    }
    munmap((void *)g_data, g_size);
    close(g_fd);

    // Build CSR offsets
    std::vector<uint32_t> offsets(num_nodes + 1, 0);
    for (uint32_t i = 0; i < num_nodes; i++) {
        offsets[i + 1] = offsets[i] + degree[i];
    }

    std::vector<uint32_t> cur_offset = offsets;
    std::vector<Edge> edges(num_edges);

    for (uint32_t i = 0; i < num_edges; i++) {
        uint32_t u = raw_edges[i].u;
        uint32_t idx = cur_offset[u]++;
        edges[idx] = {raw_edges[i].v, raw_edges[i].w};
    }
    // Free raw edges memory
    raw_edges.clear();
    raw_edges.shrink_to_fit();

    // 2. Read queries.txt
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);

    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) {
        perror("open queries");
        return 1;
    }
    fstat(q_fd, &st);
    size_t q_size = st.st_size;

    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_PRIVATE, q_fd, 0);
    madvise((void *)q_data, q_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    struct Query {
        uint32_t s;
        uint32_t t;
    };
    std::vector<Query> queries;
    queries.reserve(2048);

    p = q_data;
    end = q_data + q_size;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
        if (p >= end) break;
        Query q;
        p = fast_parse_u32(p, &q.s);
        while (p < end && *p == ' ') p++;
        p = fast_parse_u32(p, &q.t);
        while (p < end && *p != '\n') p++;
        if (p < end && *p == '\n') p++;
        queries.push_back(q);
    }
    munmap((void *)q_data, q_size);
    close(q_fd);

    size_t num_queries = queries.size();
    std::vector<int64_t> answers(num_queries, -1);

    // Multi-threaded Dijkstra execution
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
            std::vector<uint64_t> dist(num_nodes, UINT64_MAX);
            std::vector<uint32_t> visited_tag(num_nodes, 0);
            uint32_t tag = 1;

            std::priority_queue<HeapNode, std::vector<HeapNode>, std::greater<HeapNode>> pq;

            for (size_t qid = start_q; qid < end_q; qid++) {
                uint32_t s = queries[qid].s;
                uint32_t t = queries[qid].t;

                if (s >= num_nodes || t >= num_nodes) {
                    answers[qid] = -1;
                    continue;
                }

                if (s == t) {
                    answers[qid] = 0;
                    continue;
                }

                // Increment tag to avoid clearing dist array
                tag++;
                if (tag == 0) {
                    std::fill(visited_tag.begin(), visited_tag.end(), 0);
                    tag = 1;
                }

                dist[s] = 0;
                visited_tag[s] = tag;
                pq.push({0, s});

                int64_t found_dist = -1;

                while (!pq.empty()) {
                    HeapNode top = pq.top();
                    pq.pop();

                    uint64_t d = top.dist;
                    uint32_t u = top.u;

                    if (visited_tag[u] == tag && d > dist[u]) {
                        continue;
                    }

                    if (u == t) {
                        found_dist = (int64_t)d;
                        break;
                    }

                    uint32_t edge_start = offsets[u];
                    uint32_t edge_end = offsets[u + 1];

                    for (uint32_t e = edge_start; e < edge_end; e++) {
                        uint32_t v = edges[e].target;
                        uint64_t nd = d + edges[e].weight;

                        if (visited_tag[v] != tag || nd < dist[v]) {
                            dist[v] = nd;
                            visited_tag[v] = tag;
                            pq.push({nd, v});
                        }
                    }
                }

                answers[qid] = found_dist;
                // Clear remaining PQ entries
                while (!pq.empty()) pq.pop();
            }
        });
    }

    for (auto &t : workers) {
        t.join();
    }

    // Write output
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/distances.txt", out_dir);
    mkdir(out_dir, 0777);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen distances");
        return 1;
    }

    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (size_t i = 0; i < num_queries; i++) {
        fprintf(out, "%lld\n", (long long)answers[i]);
    }

    fclose(out);
    free(out_buf);

    return 0;
}
