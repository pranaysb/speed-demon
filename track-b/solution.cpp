#include <memory>
#include <atomic>
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
#include <chrono>

#if defined(__linux__)
#include <malloc.h>
#endif

alignas(64) static uint8_t is_alnum_tab[256];
alignas(64) static uint8_t to_lower_tab[256];

static void init_tabs() {
    for (int i = 0; i < 256; i++) {
        is_alnum_tab[i] = ((i >= 'a' && i <= 'z') || (i >= 'A' && i <= 'Z') || (i >= '0' && i <= '9')) ? 1 : 0;
        to_lower_tab[i] = (i >= 'A' && i <= 'Z') ? (uint8_t)(i + 32) : (uint8_t)i;
    }
}

static constexpr uint64_t FNV_OFFSET_BASIS = 0xcbf29ce484222325ULL;
static constexpr uint64_t FNV_PRIME = 0x100000001b3ULL;

static inline uint64_t hash_term_bytes(const char *p, size_t len) {
    uint64_t h = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)p[i]) * FNV_PRIME;
    }
    return h;
}

struct TermEntry {
    uint64_t hash;
    uint32_t term_id;
    uint16_t len;
    uint16_t pad;
};

class QueryTermDict {
public:
    size_t cap;
    size_t mask;
    std::vector<TermEntry> table;
    std::vector<std::string> terms;

    QueryTermDict(size_t initial_cap = 262144) {
        cap = initial_cap;
        mask = cap - 1;
        table.resize(cap);
        for (size_t i = 0; i < cap; i++) table[i].term_id = UINT32_MAX;
    }

    uint32_t get_or_insert(const char *p, size_t len) {
        uint64_t h = hash_term_bytes(p, len);
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h && table[idx].len == len) {
                return table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        uint32_t id = (uint32_t)terms.size();
        terms.emplace_back(p, len);
        table[idx].hash = h;
        table[idx].term_id = id;
        table[idx].len = (uint16_t)len;
        return id;
    }

    inline int32_t find_by_hash(uint64_t h, uint16_t len) const {
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h && table[idx].len == len) {
                return (int32_t)table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        return -1;
    }

    inline int32_t find(const char *p, size_t len) const {
        uint64_t h = hash_term_bytes(p, len);
        return find_by_hash(h, (uint16_t)len);
    }
};

enum OpType : uint8_t { OP_AND = 1, OP_OR = 2, OP_NOT = 3 };

struct QueryStep {
    uint8_t op;
    uint32_t term_id;
};

struct ParsedQuery {
    bool is_phrase;
    std::vector<QueryStep> steps;
    std::vector<std::string_view> phrase_raw_terms;
};

struct DocChunk {
    size_t start_offset;
    size_t end_offset;
    uint32_t start_doc_id;
    uint32_t doc_count;
};

struct alignas(16) DictCacheEntry {
    uint64_t hash;
    int32_t term_id;
    uint16_t len;
    uint16_t pad;
};

// 16-bit posting intersections for a partition
static inline size_t intersect_16(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, uint16_t *out) {
    size_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            out[k++] = a[i];
            i++; j++;
        } else if (a[i] < b[j]) {
            i++;
        } else {
            j++;
        }
    }
    return k;
}


static inline size_t intersect_galloping_16(const uint16_t *small, size_t n_small, const uint16_t *large, size_t n_large, uint16_t *out) {
    size_t k = 0, j = 0;
    for (size_t i = 0; i < n_small && j < n_large; i++) {
        uint16_t target = small[i];
        if (large[j] < target) {
            size_t step = 1;
            while (j + step < n_large && large[j + step] < target) {
                j += step;
                step <<= 1;
            }
            size_t low = j;
            size_t high = std::min(j + step + 1, n_large);
            while (low < high) {
                size_t mid = low + (high - low) / 2;
                if (large[mid] < target) low = mid + 1;
                else high = mid;
            }
            j = low;
        }
        if (j < n_large && large[j] == target) {
            out[k++] = target;
            j++;
        }
    }
    return k;
}

static inline size_t intersect_adaptive_16(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, uint16_t *out) {
    if (na == 0 || nb == 0) return 0;
    if (na * 16 < nb) return intersect_galloping_16(a, na, b, nb, out);
    if (nb * 16 < na) return intersect_galloping_16(b, nb, a, na, out);
    return intersect_16(a, na, b, nb, out);
}

void radix_sort_pairs(uint32_t *arr, uint32_t *tmp, size_t n) {
    uint32_t count0[2048] = {0};
    uint32_t count1[2048] = {0};
    uint32_t count2[2048] = {0};
    for (size_t i = 0; i < n; i++) {
        count0[arr[i] & 0x7FF]++;
        count1[(arr[i] >> 11) & 0x7FF]++;
        count2[(arr[i] >> 22) & 0x7FF]++;
    }
    uint32_t off0[2048], off1[2048], off2[2048];
    off0[0] = 0; off1[0] = 0; off2[0] = 0;
    for (int i = 1; i < 2048; i++) {
        off0[i] = off0[i-1] + count0[i-1];
        off1[i] = off1[i-1] + count1[i-1];
        off2[i] = off2[i-1] + count2[i-1];
    }
    for (size_t i = 0; i < n; i++) tmp[off0[arr[i] & 0x7FF]++] = arr[i];
    for (size_t i = 0; i < n; i++) arr[off1[(tmp[i] >> 11) & 0x7FF]++] = tmp[i];
    for (size_t i = 0; i < n; i++) tmp[off2[(arr[i] >> 22) & 0x7FF]++] = arr[i];
    for (size_t i = 0; i < n; i++) arr[i] = tmp[i];
}

static inline size_t union_16(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, uint16_t *out) {
    size_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            out[k++] = a[i];
            i++; j++;
        } else if (a[i] < b[j]) {
            out[k++] = a[i++];
        } else {
            out[k++] = b[j++];
        }
    }
    while (i < na) out[k++] = a[i++];
    while (j < nb) out[k++] = b[j++];
    return k;
}

static inline size_t diff_16(const uint16_t *a, size_t na, const uint16_t *b, size_t nb, uint16_t *out) {
    size_t i = 0, j = 0, k = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            i++; j++;
        } else if (a[i] < b[j]) {
            out[k++] = a[i++];
        } else {
            j++;
        }
    }
    while (i < na) out[k++] = a[i++];
    return k;
}

static inline bool check_phrase(const uint8_t *tp, size_t len, const std::vector<std::string_view> &terms) {
    const uint8_t *tend = tp + len;
    size_t k = terms.size();
    char first_ch = terms[0][0];
    size_t first_len = terms[0].size();

    const uint8_t *cur = tp;
    while (cur < tend) {
        while (cur < tend && to_lower_tab[*cur] != (uint8_t)first_ch) cur++;
        if (cur >= tend) break;

        if (cur > tp && is_alnum_tab[*(cur - 1)]) {
            cur++;
            continue;
        }

        if (cur + first_len > tend || (cur + first_len < tend && is_alnum_tab[*(cur + first_len)])) {
            cur++;
            continue;
        }

        bool match_first = true;
        for (size_t i = 0; i < first_len; i++) {
            if (to_lower_tab[cur[i]] != (uint8_t)terms[0][i]) {
                match_first = false;
                break;
            }
        }
        if (!match_first) {
            cur++;
            continue;
        }

        const uint8_t *sub_p = cur + first_len;
        bool all_matched = true;
        for (size_t m = 1; m < k; m++) {
            while (sub_p < tend && !is_alnum_tab[*sub_p]) sub_p++;
            if (sub_p >= tend) { all_matched = false; break; }
            size_t tlen = terms[m].size();
            if (sub_p + tlen > tend || (sub_p + tlen < tend && is_alnum_tab[*(sub_p + tlen)])) {
                all_matched = false;
                break;
            }
            for (size_t i = 0; i < tlen; i++) {
                if (to_lower_tab[sub_p[i]] != (uint8_t)terms[m][i]) {
                    all_matched = false;
                    break;
                }
            }
            if (!all_matched) break;
            sub_p += tlen;
        }
        if (all_matched) return true;
        cur += first_len;
    }
    return false;
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
    auto t_start = std::chrono::high_resolution_clock::now();
    init_tabs();

    const char *data_dir = "/data";
    const char *out_dir = "/out";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) data_dir = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) out_dir = argv[++i];
    }

    // 1. Read queries and pre-parse
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);

    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }

    struct stat st;
    fstat(q_fd, &st);
    size_t q_size = st.st_size;
    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_SHARED, q_fd, 0);

    QueryTermDict query_dict(262144);
    std::vector<std::string> raw_queries;
    raw_queries.reserve(60000);

    const uint8_t *qp = q_data;
    const uint8_t *q_end = q_data + q_size;
    while (qp < q_end) {
        const uint8_t *q_start = qp;
        while (qp < q_end && *qp != '\n') qp++;
        size_t q_len = qp - q_start;
        if (q_len > 0 && q_start[q_len - 1] == '\r') q_len--;
        if (qp < q_end && *qp == '\n') qp++;
        if (q_len == 0) continue;
        raw_queries.emplace_back((const char *)q_start, q_len);
        const std::string &q = raw_queries.back();

        if (q.rfind("BOOL ", 0) == 0) {
            const char *cur = q.c_str() + 5;
            while (*cur) {
                const char *w_start = cur;
                while (*cur && *cur != ' ') cur++;
                query_dict.get_or_insert(w_start, cur - w_start);
                if (*cur == ' ') cur++;
                while (*cur && *cur != ' ') cur++;
                if (*cur == ' ') cur++;
            }
        } else if (q.rfind("PHRASE \"", 0) == 0) {
            const char *inner = q.c_str() + 8;
            size_t inner_len = q.size() - 9;
            const char *tp = inner;
            const char *tend = inner + inner_len;
            while (tp < tend) {
                const char *ts = tp;
                while (tp < tend && *tp != ' ') tp++;
                query_dict.get_or_insert(ts, tp - ts);
                if (tp < tend && *tp == ' ') tp++;
            }
        }
    }
    munmap((void *)q_data, q_size);
    close(q_fd);

    size_t num_terms = query_dict.terms.size();
    size_t num_queries = raw_queries.size();
    std::vector<ParsedQuery> parsed_queries(num_queries);
    for (size_t qidx = 0; qidx < num_queries; qidx++) {
        const std::string &q = raw_queries[qidx];
        auto &pq = parsed_queries[qidx];
        if (q.rfind("BOOL ", 0) == 0) {
            pq.is_phrase = false;
            const char *cur = q.c_str() + 5;
            const char *first_w = cur;
            while (*cur && *cur != ' ') cur++;
            int32_t tid = query_dict.find(first_w, cur - first_w);
            pq.steps.push_back({0, tid >= 0 ? (uint32_t)tid : UINT32_MAX});

            while (*cur == ' ') {
                cur++;
                const char *op_s = cur;
                while (*cur && *cur != ' ') cur++;
                std::string_view op(op_s, cur - op_s);
                uint8_t op_val = (op == "AND") ? OP_AND : ((op == "OR") ? OP_OR : OP_NOT);
                if (*cur == ' ') cur++;
                const char *ts = cur;
                while (*cur && *cur != ' ') cur++;
                int32_t ntid = query_dict.find(ts, cur - ts);
                pq.steps.push_back({op_val, ntid >= 0 ? (uint32_t)ntid : UINT32_MAX});
            }
        } else if (q.rfind("PHRASE \"", 0) == 0) {
            pq.is_phrase = true;
            const char *inner = q.c_str() + 8;
            size_t inner_len = q.size() - 9;
            const char *tp = inner;
            const char *tend = inner + inner_len;
            while (tp < tend) {
                const char *ts = tp;
                while (tp < tend && *tp != ' ') tp++;
                pq.phrase_raw_terms.emplace_back(ts, tp - ts);
                int32_t tid = query_dict.find(ts, tp - ts);
                pq.steps.push_back({0, tid >= 0 ? (uint32_t)tid : UINT32_MAX});
                if (tp < tend && *tp == ' ') tp++;
            }
        }
    }

    // 2. Open docs.txt
    char docs_path[1024];
    snprintf(docs_path, sizeof(docs_path), "%s/docs.txt", data_dir);
    int d_fd = open(docs_path, O_RDONLY);
    if (d_fd < 0) { perror("open docs"); return 1; }
    fstat(d_fd, &st);
    size_t d_size = st.st_size;
    const uint8_t *d_data = (const uint8_t *)mmap(NULL, d_size, PROT_READ, MAP_SHARED, d_fd, 0);

    unsigned int num_threads = std::clamp(std::thread::hardware_concurrency(), 1u, 12u);
    std::vector<DocChunk> chunks(num_threads);
    size_t approx_chunk = d_size / num_threads;
    size_t cur_offset = 0;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        chunks[tid].start_offset = cur_offset;
        if (tid == num_threads - 1) {
            chunks[tid].end_offset = d_size;
        } else {
            size_t target = std::min(cur_offset + approx_chunk, d_size);
            while (target < d_size && d_data[target] != '\n') target++;
            if (target < d_size && d_data[target] == '\n') target++;
            chunks[tid].end_offset = target;
        }
        cur_offset = chunks[tid].end_offset;
    }

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        if (chunks[tid].start_offset >= chunks[tid].end_offset) {
            chunks[tid].start_doc_id = 0;
            chunks[tid].doc_count = 0;
            continue;
        }
        const uint8_t *p = d_data + chunks[tid].start_offset;
        uint32_t id = 0;
        while (p < d_data + chunks[tid].end_offset && *p >= '0' && *p <= '9') {
            id = id * 10 + (*p - '0');
            p++;
        }
        chunks[tid].start_doc_id = id;
    }

    static constexpr size_t MAX_DOCS = 300000;
    std::vector<uint32_t> doc_offset_arr(MAX_DOCS);
    std::vector<uint32_t> doc_len_arr(MAX_DOCS);

    // Single-Pass Partition Indexing
    struct PartitionIndex {
        uint32_t start_doc_id;
        uint32_t doc_count;
        std::vector<uint32_t> offsets; // num_terms + 1
        std::vector<uint16_t> docs;    // 16-bit local doc ids
        std::vector<uint32_t> pairs;
    };
    std::vector<PartitionIndex> partitions(num_threads);

    auto t_indexing_start = std::chrono::high_resolution_clock::now();
    std::vector<std::thread> workers;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        workers.emplace_back([tid, &chunks, d_data, num_terms, &query_dict, &doc_offset_arr, &doc_len_arr, &partitions]() {
            auto &part = partitions[tid];
            part.start_doc_id = chunks[tid].start_doc_id;
            uint32_t base_doc = chunks[tid].start_doc_id;

            auto &pairs = part.pairs;
            pairs.reserve(2000000);

            auto tl_cache = std::make_unique<DictCacheEntry[]>(32768);
            std::vector<uint32_t> last_doc_seen(num_terms, UINT32_MAX);

            const uint8_t *p = d_data + chunks[tid].start_offset;
            const uint8_t *p_end = d_data + chunks[tid].end_offset;
            uint32_t cur_doc = base_doc;

            while (p < p_end) {
                while (p < p_end && *p >= '0' && *p <= '9') p++;
                if (p < p_end && *p == '\t') p++;
                const uint8_t *text_start = p;
                while (p < p_end && *p != '\n') p++;
                size_t tlen = p - text_start;
                if (tlen > 0 && text_start[tlen - 1] == '\r') tlen--;
                if (p < p_end && *p == '\n') p++;

                doc_offset_arr[cur_doc] = (uint32_t)(text_start - d_data);
                doc_len_arr[cur_doc] = (uint32_t)tlen;

                const uint8_t *tp = text_start;
                const uint8_t *tend = text_start + tlen;
                uint16_t local_doc = (uint16_t)(cur_doc - base_doc);

                while (tp < tend) {
                    while (tp < tend && !is_alnum_tab[*tp]) tp++;
                    if (tp >= tend) break;

                    uint64_t h = FNV_OFFSET_BASIS;
                    uint16_t tok_len = 0;
                    while (tp < tend && is_alnum_tab[*tp]) {
                        h = (h ^ to_lower_tab[*tp]) * FNV_PRIME;
                        tok_len++;
                        tp++;
                    }

                    uint32_t c_idx = (uint32_t)h & 32767;
                    int32_t tid_val;
                    if (tl_cache[c_idx].hash == h && tl_cache[c_idx].len == tok_len) {
                        tid_val = tl_cache[c_idx].term_id;
                    } else {
                        tid_val = query_dict.find_by_hash(h, tok_len);
                        tl_cache[c_idx] = {h, tid_val, tok_len, 0};
                    }

                    if (tid_val >= 0 && last_doc_seen[tid_val] != cur_doc) {
                        last_doc_seen[tid_val] = cur_doc;
                        pairs.push_back(((uint32_t)tid_val << 15) | local_doc);
                    }
                }
                cur_doc++;
            }
            part.doc_count = cur_doc - base_doc;

            std::vector<uint32_t>().swap(last_doc_seen);
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Sequential Radix Sort & CSR construction using single shared 8MB buffer
    std::vector<uint32_t> shared_tmp_pairs(2200000);
    for (unsigned int tid = 0; tid < num_threads; tid++) {
        auto &part = partitions[tid];
        if (shared_tmp_pairs.size() < part.pairs.size()) shared_tmp_pairs.resize(part.pairs.size());
        radix_sort_pairs(part.pairs.data(), shared_tmp_pairs.data(), part.pairs.size());

        part.offsets.assign(num_terms + 1, 0);
        part.docs.resize(part.pairs.size());

        size_t pair_idx = 0;
        size_t total_p = part.pairs.size();
        for (size_t t = 0; t < num_terms; t++) {
            part.offsets[t] = (uint32_t)pair_idx;
            while (pair_idx < total_p && (part.pairs[pair_idx] >> 15) == t) {
                part.docs[pair_idx] = (uint16_t)(part.pairs[pair_idx] & 0x7FFF);
                pair_idx++;
            }
        }
        part.offsets[num_terms] = (uint32_t)pair_idx;
        std::vector<uint32_t>().swap(part.pairs); // Free immediately!
    }
    std::vector<uint32_t>().swap(shared_tmp_pairs); // Free immediately!

#if defined(__linux__)
    posix_fadvise(d_fd, 0, d_size, POSIX_FADV_DONTNEED);
    malloc_trim(0);
#endif
    auto t_indexing_end = std::chrono::high_resolution_clock::now();
    double d_index = std::chrono::duration<double, std::milli>(t_indexing_end - t_indexing_start).count();
    printf("Single-Pass Indexing on %u threads completed in: %.2f ms\n", num_threads, d_index);

    // 3. Query Execution across partitions
    struct ThreadOutBuf {
        std::vector<char> buf;
    };
    std::vector<ThreadOutBuf> thread_out_bufs(num_threads);
    size_t query_chunk = (num_queries + num_threads - 1) / num_threads;

    auto t_eval_start = std::chrono::high_resolution_clock::now();

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * query_chunk;
        size_t end_q = std::min(start_q + query_chunk, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([tid, start_q, end_q, &parsed_queries, &partitions, d_data, &doc_offset_arr, &doc_len_arr, &thread_out_bufs, num_threads]() {
            auto &out_vec = thread_out_bufs[tid].buf;
            out_vec.resize((end_q - start_q) * 96);
            char *out_ptr = out_vec.data();

            // Buffers for 16-bit set operations per partition (at most 25,000 docs = 50KB!)
            auto buf_a = std::make_unique<uint16_t[]>(65536);
            auto buf_b = std::make_unique<uint16_t[]>(65536);
            uint32_t first_10[10];

            for (size_t qidx = start_q; qidx < end_q; qidx++) {
                const auto &pq = parsed_queries[qidx];
                uint64_t total_count = 0;
                uint64_t total_sum = 0;
                size_t num_first_10 = 0;

                if (!pq.is_phrase) {
                    for (unsigned int pidx = 0; pidx < num_threads; pidx++) {
                        const auto &part = partitions[pidx];
                        uint32_t first_tid = pq.steps[0].term_id;
                        const uint16_t *p_data = nullptr;
                        size_t p_len = 0;
                        if (first_tid != UINT32_MAX) {
                            uint32_t st_pos = part.offsets[first_tid];
                            p_len = part.offsets[first_tid + 1] - st_pos;
                            p_data = &part.docs[st_pos];
                        }

                        uint16_t *cur_res = buf_a.get();
                        uint16_t *cur_alt = buf_b.get();
                        size_t cur_len = p_len;
                        if (p_len > 0) {
                            memcpy(cur_res, p_data, p_len * sizeof(uint16_t));
                        }

                        for (size_t si = 1; si < pq.steps.size(); si++) {
                            const auto &st = pq.steps[si];
                            const uint16_t *op_p = nullptr;
                            size_t op_l = 0;
                            if (st.term_id != UINT32_MAX) {
                                uint32_t st_pos = part.offsets[st.term_id];
                                op_l = part.offsets[st.term_id + 1] - st_pos;
                                op_p = &part.docs[st_pos];
                            }

                            if (st.op == OP_AND) {
                                cur_len = intersect_adaptive_16(cur_res, cur_len, op_p, op_l, cur_alt);
                            } else if (st.op == OP_OR) {
                                cur_len = union_16(cur_res, cur_len, op_p, op_l, cur_alt);
                            } else if (st.op == OP_NOT) {
                                cur_len = diff_16(cur_res, cur_len, op_p, op_l, cur_alt);
                            }
                            std::swap(cur_res, cur_alt);
                        }

                        if (cur_len > 0) {
                            total_count += cur_len;
                            uint32_t base = part.start_doc_id;
                            for (size_t i = 0; i < cur_len; i++) {
                                uint32_t g_doc = base + cur_res[i];
                                total_sum += g_doc;
                                if (num_first_10 < 10) first_10[num_first_10++] = g_doc;
                            }
                        }
                    }
                } else {
                    // Phrase query
                    for (unsigned int pidx = 0; pidx < num_threads; pidx++) {
                        const auto &part = partitions[pidx];
                        bool has_empty = false;
                        struct PhraseTermInfo { const uint16_t *data; size_t len; };
                        PhraseTermInfo pinfo[16];
                        size_t num_pt = pq.steps.size();

                        for (size_t i = 0; i < num_pt; i++) {
                            uint32_t ti = pq.steps[i].term_id;
                            if (ti == UINT32_MAX) { has_empty = true; break; }
                            uint32_t st = part.offsets[ti];
                            pinfo[i].len = part.offsets[ti + 1] - st;
                            pinfo[i].data = &part.docs[st];
                            if (pinfo[i].len == 0) { has_empty = true; break; }
                        }

                        if (!has_empty) {
                            std::sort(pinfo, pinfo + num_pt, [](const PhraseTermInfo &a, const PhraseTermInfo &b) {
                                return a.len < b.len;
                            });
                            size_t cand_len = pinfo[0].len;
                            uint16_t *cur_res = buf_a.get();
                            uint16_t *cur_alt = buf_b.get();
                            memcpy(cur_res, pinfo[0].data, cand_len * sizeof(uint16_t));

                            for (size_t i = 1; i < num_pt && cand_len > 0; i++) {
                                cand_len = intersect_adaptive_16(cur_res, cand_len, pinfo[i].data, pinfo[i].len, cur_alt);
                                std::swap(cur_res, cur_alt);
                            }

                            uint32_t base = part.start_doc_id;
                            for (size_t i = 0; i < cand_len; i++) {
                                uint32_t g_doc = base + cur_res[i];
                                const uint8_t *dp = d_data + doc_offset_arr[g_doc];
                                size_t dlen = doc_len_arr[g_doc];
                                if (check_phrase(dp, dlen, pq.phrase_raw_terms)) {
                                    total_count++;
                                    total_sum += g_doc;
                                    if (num_first_10 < 10) first_10[num_first_10++] = g_doc;
                                }
                            }
                        }
                    }
                }

                // Output formatting
                out_ptr = fast_u64(out_ptr, total_count); *out_ptr++ = ' ';
                out_ptr = fast_u64(out_ptr, total_sum);
                for (size_t i = 0; i < num_first_10; i++) {
                    *out_ptr++ = ' ';
                    out_ptr = fast_u32(out_ptr, first_10[i]);
                }
                *out_ptr++ = '\n';
            }
            out_vec.resize(out_ptr - out_vec.data());
        });
    }
    for (auto &w : workers) w.join();

    auto t_eval_end = std::chrono::high_resolution_clock::now();
    double d_eval = std::chrono::duration<double, std::milli>(t_eval_end - t_eval_start).count();
    printf("Query evaluation completed in: %.2f ms\n", d_eval);

    // 4. Write output
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/results.txt", out_dir);
    FILE *fout = fopen(out_path, "wb");
    if (!fout) { perror("fopen results.txt"); return 1; }
    for (unsigned int tid = 0; tid < num_threads; tid++) {
        const auto &vec = thread_out_bufs[tid].buf;
        if (!vec.empty()) fwrite(vec.data(), 1, vec.size(), fout);
    }
    fclose(fout);

    auto t_total_end = std::chrono::high_resolution_clock::now();
    double d_total = std::chrono::duration<double, std::milli>(t_total_end - t_start).count();
    printf("Track B Partitioned Total: %.2f ms\n", d_total);

    return 0;
}
