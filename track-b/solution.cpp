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

struct DocSpan {
    const uint8_t *ptr;
    uint32_t len;
};

// Fast open-addressing hash table for query terms
struct TermEntry {
    uint64_t hash;
    uint32_t term_id;
    uint16_t len;
    char text[34];
};

static inline uint64_t hash_term(const char *p, size_t len) {
    uint64_t h = len ^ 0xa0761d6478bd642fULL;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)p[i]) * 0x100000001b3ULL;
    }
    return h;
}

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
        uint64_t h = hash_term(p, len);
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h && table[idx].len == len &&
                memcmp(table[idx].text, p, len) == 0) {
                return table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        uint32_t id = (uint32_t)terms.size();
        terms.emplace_back(p, len);
        table[idx].hash = h;
        table[idx].term_id = id;
        table[idx].len = (uint16_t)len;
        memcpy(table[idx].text, p, len);
        table[idx].text[len] = 0;
        return id;
    }

    inline int32_t find(const char *p, size_t len) const {
        uint64_t h = hash_term(p, len);
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h && table[idx].len == len &&
                memcmp(table[idx].text, p, len) == 0) {
                return (int32_t)table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        return -1;
    }
};

// Reusable zero-alloc set operations
static void intersect_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back(a[i]);
            i++; j++;
        } else if (a[i] < b[j]) {
            i++;
        } else {
            j++;
        }
    }
}

static void union_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            out.push_back(a[i]);
            i++; j++;
        } else if (a[i] < b[j]) {
            out.push_back(a[i]);
            i++;
        } else {
            out.push_back(b[j]);
            j++;
        }
    }
    while (i < a.size()) out.push_back(a[i++]);
    while (j < b.size()) out.push_back(b[j++]);
}

static void difference_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            i++; j++;
        } else if (a[i] < b[j]) {
            out.push_back(a[i]);
            i++;
        } else {
            j++;
        }
    }
    while (i < a.size()) out.push_back(a[i++]);
}

static inline void tokenize_doc(const uint8_t *text, size_t len, std::vector<std::string_view> &tokens) {
    tokens.clear();
    const uint8_t *p = text;
    const uint8_t *end = text + len;
    while (p < end) {
        while (p < end) {
            uint8_t c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) break;
            p++;
        }
        if (p >= end) break;
        const uint8_t *start = p;
        while (p < end) {
            uint8_t c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) p++;
            else break;
        }
        tokens.emplace_back((const char *)start, p - start);
    }
}

static inline bool token_eq(std::string_view doc_tok, const std::string &query_tok) {
    if (doc_tok.size() != query_tok.size()) return false;
    for (size_t i = 0; i < doc_tok.size(); i++) {
        char c = doc_tok[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        if (c != query_tok[i]) return false;
    }
    return true;
}

static inline char* fast_u32(char *p, uint32_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[12];
    int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + '0'; v /= 10; }
    while (i > 0) *p++ = buf[--i];
    return p;
}

static inline char* fast_u64(char *p, uint64_t v) {
    if (v == 0) { *p++ = '0'; return p; }
    char buf[24];
    int i = 0;
    while (v > 0) { buf[i++] = (v % 10) + '0'; v /= 10; }
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

    // 1. Read queries.txt FIRST to extract all query terms
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);

    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }

    struct stat st;
    fstat(q_fd, &st);
    size_t q_size = st.st_size;
    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_PRIVATE, q_fd, 0);

    QueryTermDict query_dict(262144);
    std::vector<std::string> queries;
    queries.reserve(60000);

    const uint8_t *qp = q_data;
    const uint8_t *q_end = q_data + q_size;
    while (qp < q_end) {
        const uint8_t *q_start = qp;
        while (qp < q_end && *qp != '\n') qp++;
        size_t q_len = qp - q_start;
        if (q_len > 0 && q_start[q_len - 1] == '\r') q_len--;
        if (qp < q_end && *qp == '\n') qp++;

        if (q_len == 0) continue;
        queries.emplace_back((const char *)q_start, q_len);
        const std::string &q = queries.back();

        if (q.rfind("BOOL ", 0) == 0) {
            const char *cur = q.c_str() + 5;
            while (*cur) {
                const char *w_start = cur;
                while (*cur && *cur != ' ') cur++;
                query_dict.get_or_insert(w_start, cur - w_start);
                if (*cur == ' ') cur++;
                while (*cur && *cur != ' ') cur++; // skip op
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

    // 2. Read and index docs.txt in parallel
    char docs_path[1024];
    snprintf(docs_path, sizeof(docs_path), "%s/docs.txt", data_dir);

    int d_fd = open(docs_path, O_RDONLY);
    if (d_fd < 0) { perror("open docs"); return 1; }

    fstat(d_fd, &st);
    size_t d_size = st.st_size;
    const uint8_t *d_data = (const uint8_t *)mmap(NULL, d_size, PROT_READ, MAP_PRIVATE, d_fd, 0);
    if (d_data == MAP_FAILED) { perror("mmap docs"); close(d_fd); return 1; }
    madvise((void *)d_data, d_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    if (num_threads > 12) num_threads = 12;

    std::vector<DocSpan> docs;
    docs.reserve(300000);

    const uint8_t *p = d_data;
    const uint8_t *d_end = d_data + d_size;
    while (p < d_end) {
        while (p < d_end && *p >= '0' && *p <= '9') p++;
        if (p < d_end && *p == '\t') p++;
        const uint8_t *ts = p;
        while (p < d_end && *p != '\n') p++;
        size_t tlen = p - ts;
        if (tlen > 0 && ts[tlen - 1] == '\r') tlen--;
        if (p < d_end && *p == '\n') p++;
        docs.push_back({ts, (uint32_t)tlen});
    }

    size_t num_docs = docs.size();

    // Divide docs into thread chunks for tokenizing and posting collection
    std::vector<std::vector<std::vector<uint32_t>>> thread_postings(num_threads);
    std::vector<std::thread> workers;
    size_t doc_chunk = (num_docs + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        thread_postings[tid].resize(num_terms);
        size_t start_doc = tid * doc_chunk;
        size_t end_doc = std::min(start_doc + doc_chunk, num_docs);

        workers.emplace_back([tid, start_doc, end_doc, &docs, &query_dict, &thread_postings]() {
            auto &local_posts = thread_postings[tid];
            char term_buf[64];

            for (size_t d = start_doc; d < end_doc; d++) {
                const uint8_t *tp = docs[d].ptr;
                const uint8_t *tend = tp + docs[d].len;
                uint32_t doc_id = (uint32_t)d;

                while (tp < tend) {
                    while (tp < tend) {
                        uint8_t c = *tp;
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) break;
                        tp++;
                    }
                    if (tp >= tend) break;

                    size_t tlen = 0;
                    while (tp < tend && tlen < sizeof(term_buf) - 1) {
                        uint8_t c = *tp;
                        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                            term_buf[tlen++] = (char)c; tp++;
                        } else if (c >= 'A' && c <= 'Z') {
                            term_buf[tlen++] = (char)(c + ('a' - 'A')); tp++;
                        } else break;
                    }
                    while (tp < tend) {
                        uint8_t c = *tp;
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) tp++;
                        else break;
                    }

                    int32_t tid_val = query_dict.find(term_buf, tlen);
                    if (tid_val >= 0) {
                        auto &pl = local_posts[tid_val];
                        if (pl.empty() || pl.back() != doc_id) {
                            pl.push_back(doc_id);
                        }
                    }
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();

    // Merge postings across threads in parallel
    std::vector<std::vector<uint32_t>> postings(num_terms);
    for (unsigned int tid = 0; tid < num_threads; tid++) {
        workers.emplace_back([tid, num_threads, num_terms, &postings, &thread_postings]() {
            for (size_t t = tid; t < num_terms; t += num_threads) {
                size_t total_sz = 0;
                for (size_t thr = 0; thr < thread_postings.size(); thr++) {
                    total_sz += thread_postings[thr][t].size();
                }
                postings[t].reserve(total_sz);
                for (size_t thr = 0; thr < thread_postings.size(); thr++) {
                    postings[t].insert(postings[t].end(), thread_postings[thr][t].begin(), thread_postings[thr][t].end());
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    thread_postings.clear();

    // 3. Answer queries in parallel
    size_t num_queries = queries.size();
    std::vector<std::string> results(num_queries);
    size_t query_chunk = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * query_chunk;
        size_t end_q = std::min(start_q + query_chunk, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([tid, start_q, end_q, &queries, &query_dict, &postings, &docs, &results]() {
            std::vector<uint32_t> buf_a, buf_b, cand_a, cand_b;
            buf_a.reserve(16384); buf_b.reserve(16384);
            cand_a.reserve(16384); cand_b.reserve(16384);
            std::vector<std::string_view> doc_tokens;
            doc_tokens.reserve(1024);
            char line_buf[512];

            for (size_t qidx = start_q; qidx < end_q; qidx++) {
                const std::string &q = queries[qidx];
                if (q.rfind("BOOL ", 0) == 0) {
                    const char *cur = q.c_str() + 5;
                    const char *w_start = cur;
                    while (*cur && *cur != ' ') cur++;
                    int32_t tid_val = query_dict.find(w_start, cur - w_start);

                    static const std::vector<uint32_t> empty_vec;
                    const std::vector<uint32_t> *curr = (tid_val >= 0) ? &postings[tid_val] : &empty_vec;
                    std::vector<uint32_t> *res_ptr = &buf_a;
                    std::vector<uint32_t> *alt_ptr = &buf_b;
                    *res_ptr = *curr;

                    while (*cur == ' ') {
                        cur++; // skip space
                        const char *op_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        std::string_view op(op_start, cur - op_start);
                        if (*cur == ' ') cur++;

                        const char *t_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        int32_t next_tid = query_dict.find(t_start, cur - t_start);
                        const std::vector<uint32_t> &next_post = (next_tid >= 0) ? postings[next_tid] : empty_vec;

                        if (op == "AND") {
                            intersect_sorted(*res_ptr, next_post, *alt_ptr);
                        } else if (op == "OR") {
                            union_sorted(*res_ptr, next_post, *alt_ptr);
                        } else if (op == "NOT") {
                            difference_sorted(*res_ptr, next_post, *alt_ptr);
                        }
                        std::swap(res_ptr, alt_ptr);
                    }

                    uint64_t id_sum = 0;
                    for (uint32_t id : *res_ptr) id_sum += id;
                    char *lp = line_buf;
                    lp = fast_u64(lp, res_ptr->size()); *lp++ = ' ';
                    lp = fast_u64(lp, id_sum);
                    size_t limit = std::min(res_ptr->size(), (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        *lp++ = ' ';
                        lp = fast_u32(lp, (*res_ptr)[i]);
                    }
                    results[qidx].assign(line_buf, lp - line_buf);

                } else if (q.rfind("PHRASE \"", 0) == 0) {
                    const char *inner = q.c_str() + 8;
                    size_t inner_len = q.size() - 9;
                    std::vector<std::string> phrase_terms;
                    const char *tp = inner;
                    const char *tend = inner + inner_len;
                    while (tp < tend) {
                        const char *ts = tp;
                        while (tp < tend && *tp != ' ') tp++;
                        phrase_terms.emplace_back(ts, tp - ts);
                        if (tp < tend && *tp == ' ') tp++;
                    }

                    static const std::vector<uint32_t> empty_vec;
                    int32_t t0 = query_dict.find(phrase_terms[0].c_str(), phrase_terms[0].size());
                    const std::vector<uint32_t> *curr = (t0 >= 0) ? &postings[t0] : &empty_vec;
                    std::vector<uint32_t> *res_ptr = &cand_a;
                    std::vector<uint32_t> *alt_ptr = &cand_b;
                    *res_ptr = *curr;

                    for (size_t i = 1; i < phrase_terms.size() && !res_ptr->empty(); i++) {
                        int32_t ti = query_dict.find(phrase_terms[i].c_str(), phrase_terms[i].size());
                        const std::vector<uint32_t> &post = (ti >= 0) ? postings[ti] : empty_vec;
                        intersect_sorted(*res_ptr, post, *alt_ptr);
                        std::swap(res_ptr, alt_ptr);
                    }

                    std::vector<uint32_t> matches;
                    size_t k = phrase_terms.size();

                    for (uint32_t cand_id : *res_ptr) {
                        const DocSpan &ds = docs[cand_id];
                        tokenize_doc(ds.ptr, ds.len, doc_tokens);
                        if (doc_tokens.size() >= k) {
                            for (size_t i = 0; i <= doc_tokens.size() - k; i++) {
                                bool match = true;
                                for (size_t j = 0; j < k; j++) {
                                    if (!token_eq(doc_tokens[i + j], phrase_terms[j])) {
                                        match = false;
                                        break;
                                    }
                                }
                                if (match) {
                                    matches.push_back(cand_id);
                                    break;
                                }
                            }
                        }
                    }

                    uint64_t id_sum = 0;
                    for (uint32_t id : matches) id_sum += id;
                    char *lp = line_buf;
                    lp = fast_u64(lp, matches.size()); *lp++ = ' ';
                    lp = fast_u64(lp, id_sum);
                    size_t limit = std::min(matches.size(), (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        *lp++ = ' ';
                        lp = fast_u32(lp, matches[i]);
                    }
                    results[qidx].assign(line_buf, lp - line_buf);
                }
            }
        });
    }
    for (auto &w : workers) w.join();

    // 4. Write output
    mkdir(out_dir, 0777);
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/results.txt", out_dir);

    FILE *out = fopen(out_path, "wb");
    if (!out) { perror("fopen results"); return 1; }
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (size_t i = 0; i < num_queries; i++) {
        fwrite(results[i].data(), 1, results[i].size(), out);
        fputc('\n', out);
    }
    fclose(out);
    free(out_buf);
    munmap((void *)d_data, d_size);
    close(d_fd);

    return 0;
}
