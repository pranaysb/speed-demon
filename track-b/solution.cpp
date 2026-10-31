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

// FNV-1a 64-bit constants
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

static inline size_t intersect_raw(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
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

static inline size_t intersect_galloping(const uint32_t *small, size_t n_small, const uint32_t *large, size_t n_large, uint32_t *out) {
    size_t k = 0;
    size_t j = 0;
    for (size_t i = 0; i < n_small; i++) {
        uint32_t target = small[i];
        if (j >= n_large) break;
        if (large[j] == target) {
            out[k++] = target;
            j++;
            continue;
        } else if (large[j] > target) {
            continue;
        }
        size_t step = 1;
        size_t prev = j;
        while (prev + step < n_large && large[prev + step] < target) {
            prev += step;
            step <<= 1;
        }
        size_t high = std::min(prev + step, n_large);
        size_t low = prev;
        while (low < high) {
            size_t mid = low + (high - low) / 2;
            if (large[mid] < target) low = mid + 1;
            else high = mid;
        }
        j = low;
        if (j < n_large && large[j] == target) {
            out[k++] = target;
            j++;
        }
    }
    return k;
}

static inline size_t intersect_adaptive(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
    if (na == 0 || nb == 0) return 0;
    if (na * 16 < nb) {
        return intersect_galloping(a, na, b, nb, out);
    }
    if (nb * 16 < na) {
        return intersect_galloping(b, nb, a, na, out);
    }
    return intersect_raw(a, na, b, nb, out);
}

static inline size_t union_raw(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
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

static inline size_t diff_raw(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, uint32_t *out) {
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
    size_t match_idx = 0;

    while (tp < tend) {
        while (tp < tend && !is_alnum_tab[*tp]) tp++;
        if (tp >= tend) break;

        const uint8_t *tok_start = tp;
        while (tp < tend && is_alnum_tab[*tp]) tp++;
        size_t tok_len = tp - tok_start;

        auto match_term = [&](size_t idx) {
            const auto &term = terms[idx];
            if (term.size() != tok_len) return false;
            for (size_t i = 0; i < tok_len; i++) {
                if (to_lower_tab[tok_start[i]] != (uint8_t)term[i]) return false;
            }
            return true;
        };

        if (match_term(match_idx)) {
            match_idx++;
            if (match_idx == k) return true;
        } else {
            if (match_term(0)) {
                match_idx = 1;
                if (match_idx == k) return true;
            } else {
                match_idx = 0;
            }
        }
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

struct DocChunk {
    size_t start_offset;
    size_t end_offset;
    uint32_t start_doc_id;
};

int main(int argc, char **argv) {
#if defined(__linux__)
    mallopt(M_ARENA_MAX, 1);
#endif
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

    // Pre-parse queries into bytecode steps
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

    // 2. Open docs.txt and partition directly across threads
    char docs_path[1024];
    snprintf(docs_path, sizeof(docs_path), "%s/docs.txt", data_dir);

    int d_fd = open(docs_path, O_RDONLY);
    if (d_fd < 0) { perror("open docs"); return 1; }

    fstat(d_fd, &st);
    size_t d_size = st.st_size;
    const uint8_t *d_data = (const uint8_t *)mmap(NULL, d_size, PROT_READ, MAP_SHARED, d_fd, 0);
    madvise((void *)d_data, d_size, MADV_SEQUENTIAL);

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

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

    // Pass 1: Parallel count postings + record doc offsets
    std::vector<std::vector<uint32_t>> thread_counts(num_threads);
    std::vector<std::thread> workers;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        thread_counts[tid].resize(num_terms, 0);

        workers.emplace_back([tid, &chunks, d_data, num_terms, &query_dict, &thread_counts, &doc_offset_arr, &doc_len_arr]() {
            auto &counts = thread_counts[tid];
            std::vector<uint32_t> last_doc_seen(num_terms, UINT32_MAX);

            const uint8_t *p = d_data + chunks[tid].start_offset;
            const uint8_t *p_end = d_data + chunks[tid].end_offset;
            uint32_t cur_doc = chunks[tid].start_doc_id;

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

                    int32_t tid_val = query_dict.find_by_hash(h, tok_len);
                    if (tid_val >= 0 && last_doc_seen[tid_val] != cur_doc) {
                        last_doc_seen[tid_val] = cur_doc;
                        counts[tid_val]++;
                    }
                }
                cur_doc++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Compute CSR offsets
    std::vector<uint32_t> term_offsets(num_terms + 1, 0);
    std::vector<std::vector<uint32_t>> thread_write_ptrs(num_threads);
    for (unsigned int tid = 0; tid < num_threads; tid++) thread_write_ptrs[tid].resize(num_terms, 0);

    for (size_t t = 0; t < num_terms; t++) {
        uint32_t c = 0;
        for (unsigned int tid = 0; tid < num_threads; tid++) {
            thread_write_ptrs[tid][t] = c;
            c += thread_counts[tid][t];
        }
        term_offsets[t + 1] = term_offsets[t] + c;
    }

    uint32_t total_postings = term_offsets[num_terms];
    for (size_t t = 0; t < num_terms; t++) {
        uint32_t base = term_offsets[t];
        for (unsigned int tid = 0; tid < num_threads; tid++) {
            thread_write_ptrs[tid][t] += base;
        }
    }
    thread_counts.clear();

    // Allocate flat postings array
    uint32_t *all_postings = (uint32_t *)malloc(total_postings * sizeof(uint32_t));

    // Pass 2: Write doc_ids into contiguous slices
    for (unsigned int tid = 0; tid < num_threads; tid++) {
        workers.emplace_back([tid, &chunks, d_data, num_terms, &query_dict, &thread_write_ptrs, all_postings]() {
            auto &wr = thread_write_ptrs[tid];
            std::vector<uint32_t> last_doc_seen(num_terms, UINT32_MAX);

            const uint8_t *p = d_data + chunks[tid].start_offset;
            const uint8_t *p_end = d_data + chunks[tid].end_offset;
            uint32_t cur_doc = chunks[tid].start_doc_id;

            while (p < p_end) {
                while (p < p_end && *p >= '0' && *p <= '9') p++;
                if (p < p_end && *p == '\t') p++;
                const uint8_t *text_start = p;
                while (p < p_end && *p != '\n') p++;
                size_t tlen = p - text_start;
                if (tlen > 0 && text_start[tlen - 1] == '\r') tlen--;
                if (p < p_end && *p == '\n') p++;

                const uint8_t *tp = text_start;
                const uint8_t *tend = text_start + tlen;

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

                    int32_t tid_val = query_dict.find_by_hash(h, tok_len);
                    if (tid_val >= 0 && last_doc_seen[tid_val] != cur_doc) {
                        last_doc_seen[tid_val] = cur_doc;
                        uint32_t pos = wr[tid_val]++;
                        all_postings[pos] = cur_doc;
                    }
                }
                cur_doc++;
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    thread_write_ptrs.clear();
    madvise((void *)d_data, d_size, MADV_DONTNEED);

    // 3. Answer queries
    struct ThreadOutBuf {
        std::vector<char> buf;
    };
    std::vector<ThreadOutBuf> thread_out_bufs(num_threads);
    size_t query_chunk = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * query_chunk;
        size_t end_q = std::min(start_q + query_chunk, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([tid, start_q, end_q, &parsed_queries, &term_offsets, all_postings, d_data, &doc_offset_arr, &doc_len_arr, &thread_out_bufs]() {
            auto &out_vec = thread_out_bufs[tid].buf;
            out_vec.resize((end_q - start_q) * 96);
            char *out_ptr = out_vec.data();

            // Reusable buffers: 2 buffers per thread instead of 4
            uint32_t *buf_a = (uint32_t *)malloc(MAX_DOCS * sizeof(uint32_t));
            uint32_t *buf_b = (uint32_t *)malloc(MAX_DOCS * sizeof(uint32_t));
            std::vector<uint32_t> matches;
            matches.reserve(1024);

            for (size_t qidx = start_q; qidx < end_q; qidx++) {
                const auto &pq = parsed_queries[qidx];
                if (!pq.is_phrase) {
                    uint32_t first_tid = pq.steps[0].term_id;
                    const uint32_t *p_data = nullptr;
                    size_t p_len = 0;
                    if (first_tid != UINT32_MAX) {
                        uint32_t st_pos = term_offsets[first_tid];
                        p_len = term_offsets[first_tid + 1] - st_pos;
                        p_data = &all_postings[st_pos];
                    }

                    if (pq.steps.size() == 1) {
                        uint64_t id_sum = 0;
                        for (size_t i = 0; i < p_len; i++) id_sum += p_data[i];
                        out_ptr = fast_u64(out_ptr, p_len); *out_ptr++ = ' ';
                        out_ptr = fast_u64(out_ptr, id_sum);
                        size_t limit = std::min(p_len, (size_t)10);
                        for (size_t i = 0; i < limit; i++) {
                            *out_ptr++ = ' ';
                            out_ptr = fast_u32(out_ptr, p_data[i]);
                        }
                        *out_ptr++ = '\n';
                        continue;
                    }

                    uint32_t *cur_res = buf_a;
                    uint32_t *cur_alt = buf_b;
                    size_t cur_len = p_len;
                    if (p_len > 0) memcpy(cur_res, p_data, p_len * sizeof(uint32_t));

                    for (size_t s = 1; s < pq.steps.size(); s++) {
                        uint8_t op = pq.steps[s].op;
                        uint32_t next_tid = pq.steps[s].term_id;

                        const uint32_t *next_data = nullptr;
                        size_t next_len = 0;
                        if (next_tid != UINT32_MAX) {
                            uint32_t next_st = term_offsets[next_tid];
                            next_len = term_offsets[next_tid + 1] - next_st;
                            next_data = &all_postings[next_st];
                        }

                        if (op == OP_AND) {
                            cur_len = intersect_adaptive(cur_res, cur_len, next_data, next_len, cur_alt);
                        } else if (op == OP_OR) {
                            cur_len = union_raw(cur_res, cur_len, next_data, next_len, cur_alt);
                        } else if (op == OP_NOT) {
                            cur_len = diff_raw(cur_res, cur_len, next_data, next_len, cur_alt);
                        }
                        std::swap(cur_res, cur_alt);
                    }

                    uint64_t id_sum = 0;
                    for (size_t i = 0; i < cur_len; i++) id_sum += cur_res[i];
                    out_ptr = fast_u64(out_ptr, cur_len); *out_ptr++ = ' ';
                    out_ptr = fast_u64(out_ptr, id_sum);
                    size_t limit = std::min(cur_len, (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        *out_ptr++ = ' ';
                        out_ptr = fast_u32(out_ptr, cur_res[i]);
                    }
                    *out_ptr++ = '\n';

                } else {
                    struct PhraseTermInfo {
                        const uint32_t *data;
                        size_t len;
                    };
                    PhraseTermInfo pinfo[16];
                    size_t num_pt = std::min(pq.steps.size(), (size_t)16);
                    bool has_empty = false;
                    for (size_t i = 0; i < num_pt; i++) {
                        uint32_t ti = pq.steps[i].term_id;
                        if (ti == UINT32_MAX) { has_empty = true; break; }
                        uint32_t st = term_offsets[ti];
                        pinfo[i].len = term_offsets[ti + 1] - st;
                        pinfo[i].data = &all_postings[st];
                        if (pinfo[i].len == 0) { has_empty = true; break; }
                    }

                    uint32_t *cand_res = buf_a;
                    uint32_t *cand_alt = buf_b;
                    size_t cand_len = 0;

                    if (!has_empty) {
                        std::sort(pinfo, pinfo + num_pt, [](const PhraseTermInfo &a, const PhraseTermInfo &b) {
                            return a.len < b.len;
                        });
                        cand_len = pinfo[0].len;
                        memcpy(cand_res, pinfo[0].data, cand_len * sizeof(uint32_t));

                        for (size_t i = 1; i < num_pt && cand_len > 0; i++) {
                            cand_len = intersect_adaptive(cand_res, cand_len, pinfo[i].data, pinfo[i].len, cand_alt);
                            std::swap(cand_res, cand_alt);
                        }
                    }

                    matches.clear();
                    for (size_t i = 0; i < cand_len; i++) {
                        uint32_t cand_id = cand_res[i];
                        const uint8_t *dp = d_data + doc_offset_arr[cand_id];
                        size_t dlen = doc_len_arr[cand_id];
                        if (check_phrase(dp, dlen, pq.phrase_raw_terms)) {
                            matches.push_back(cand_id);
                        }
                    }

                    uint64_t id_sum = 0;
                    for (uint32_t id : matches) id_sum += id;
                    out_ptr = fast_u64(out_ptr, matches.size()); *out_ptr++ = ' ';
                    out_ptr = fast_u64(out_ptr, id_sum);
                    size_t limit = std::min(matches.size(), (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        *out_ptr++ = ' ';
                        out_ptr = fast_u32(out_ptr, matches[i]);
                    }
                    *out_ptr++ = '\n';
                }
            }
            out_vec.resize(out_ptr - out_vec.data());
            free(buf_a);
            free(buf_b);
        });
    }
    for (auto &w : workers) w.join();

    // 4. Output results
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/results.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        if (!thread_out_bufs[tid].buf.empty()) {
            fwrite(thread_out_bufs[tid].buf.data(), 1, thread_out_bufs[tid].buf.size(), out);
        }
    }
    fclose(out);
    free(out_buf);
    free(all_postings);
    munmap((void *)d_data, d_size);
    close(d_fd);

    return 0;
}
