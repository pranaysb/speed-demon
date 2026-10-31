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

struct DocSpan {
    const uint8_t *ptr;
    uint32_t len;
};

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

static inline void intersect_ptrs(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
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

static inline void union_ptrs(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
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
    while (i < na) out.push_back(a[i++]);
    while (j < nb) out.push_back(b[j++]);
}

static inline void diff_ptrs(const uint32_t *a, size_t na, const uint32_t *b, size_t nb, std::vector<uint32_t> &out) {
    out.clear();
    size_t i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            i++; j++;
        } else if (a[i] < b[j]) {
            out.push_back(a[i]);
            i++;
        } else {
            j++;
        }
    }
    while (i < na) out.push_back(a[i++]);
}

// Fast phrase checker in doc text: true if exact consecutive token sequence matches
static inline bool check_phrase(const uint8_t *tp, size_t len, const std::vector<std::string_view> &terms) {
    const uint8_t *tend = tp + len;
    size_t k = terms.size();
    size_t match_idx = 0;
    char tok_buf[64];

    // Scan tokens sequentially
    while (tp < tend) {
        while (tp < tend) {
            uint8_t c = *tp;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) break;
            tp++;
        }
        if (tp >= tend) break;

        size_t tlen = 0;
        while (tp < tend && tlen < sizeof(tok_buf) - 1) {
            uint8_t c = *tp;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                tok_buf[tlen++] = (char)c; tp++;
            } else if (c >= 'A' && c <= 'Z') {
                tok_buf[tlen++] = (char)(c + ('a' - 'A')); tp++;
            } else break;
        }
        while (tp < tend) {
            uint8_t c = *tp;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) tp++;
            else break;
        }

        std::string_view cur_tok(tok_buf, tlen);
        if (cur_tok == terms[match_idx]) {
            match_idx++;
            if (match_idx == k) return true;
        } else {
            // If match failed, reset match_idx (check if current token matches term 0)
            if (cur_tok == terms[0]) {
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

    auto t0 = std::chrono::high_resolution_clock::now();

    // 1. Read queries
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);

    int q_fd = open(queries_path, O_RDONLY);
    if (q_fd < 0) { perror("open queries"); return 1; }

    struct stat st;
    fstat(q_fd, &st);
    size_t q_size = st.st_size;
    const uint8_t *q_data = (const uint8_t *)mmap(NULL, q_size, PROT_READ, MAP_SHARED, q_fd, 0);

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

    // 2. Read docs.txt
    char docs_path[1024];
    snprintf(docs_path, sizeof(docs_path), "%s/docs.txt", data_dir);

    int d_fd = open(docs_path, O_RDONLY);
    if (d_fd < 0) { perror("open docs"); return 1; }

    fstat(d_fd, &st);
    size_t d_size = st.st_size;
    const uint8_t *d_data = (const uint8_t *)mmap(NULL, d_size, PROT_READ, MAP_SHARED, d_fd, 0);

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
    size_t doc_chunk = (num_docs + num_threads - 1) / num_threads;

    // Pass 1: Count postings per term across threads (ZERO vector allocations!)
    std::vector<std::vector<uint32_t>> thread_counts(num_threads);
    std::vector<std::thread> workers;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        thread_counts[tid].resize(num_terms, 0);
        size_t start_doc = tid * doc_chunk;
        size_t end_doc = std::min(start_doc + doc_chunk, num_docs);

        workers.emplace_back([tid, start_doc, end_doc, &docs, &query_dict, &thread_counts]() {
            auto &counts = thread_counts[tid];
            char term_buf[64];
            std::vector<int32_t> doc_terms;
            doc_terms.reserve(256);

            for (size_t d = start_doc; d < end_doc; d++) {
                const uint8_t *tp = docs[d].ptr;
                const uint8_t *tend = tp + docs[d].len;
                doc_terms.clear();

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
                    if (tid_val >= 0) doc_terms.push_back(tid_val);
                }

                if (doc_terms.size() > 1) {
                    std::sort(doc_terms.begin(), doc_terms.end());
                    doc_terms.erase(std::unique(doc_terms.begin(), doc_terms.end()), doc_terms.end());
                }
                for (int32_t t_id : doc_terms) counts[t_id]++;
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

    // Allocate ONE flat array of 88 MB for all postings!
    uint32_t *all_postings = (uint32_t *)malloc(total_postings * sizeof(uint32_t));

    // Pass 2: Write doc_ids directly into contiguous slices!
    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_doc = tid * doc_chunk;
        size_t end_doc = std::min(start_doc + doc_chunk, num_docs);

        workers.emplace_back([tid, start_doc, end_doc, &docs, &query_dict, &thread_write_ptrs, all_postings]() {
            auto &wr = thread_write_ptrs[tid];
            char term_buf[64];
            std::vector<int32_t> doc_terms;
            doc_terms.reserve(256);

            for (size_t d = start_doc; d < end_doc; d++) {
                const uint8_t *tp = docs[d].ptr;
                const uint8_t *tend = tp + docs[d].len;
                doc_terms.clear();

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
                    if (tid_val >= 0) doc_terms.push_back(tid_val);
                }

                if (doc_terms.size() > 1) {
                    std::sort(doc_terms.begin(), doc_terms.end());
                    doc_terms.erase(std::unique(doc_terms.begin(), doc_terms.end()), doc_terms.end());
                }
                for (int32_t t_id : doc_terms) {
                    uint32_t pos = wr[t_id]++;
                    all_postings[pos] = (uint32_t)d;
                }
            }
        });
    }
    for (auto &w : workers) w.join();
    workers.clear();
    thread_write_ptrs.clear();

    auto t_idx = std::chrono::high_resolution_clock::now();
    printf("CSR Index built: %u postings in %.2f ms\n", total_postings, std::chrono::duration<double, std::milli>(t_idx - t0).count());

    // 3. Answer queries
    size_t num_queries = queries.size();
    std::vector<std::string> results(num_queries);
    size_t query_chunk = (num_queries + num_threads - 1) / num_threads;

    for (unsigned int tid = 0; tid < num_threads; tid++) {
        size_t start_q = tid * query_chunk;
        size_t end_q = std::min(start_q + query_chunk, num_queries);
        if (start_q >= end_q) continue;

        workers.emplace_back([start_q, end_q, &queries, &query_dict, &term_offsets, all_postings, &docs, &results]() {
            std::vector<uint32_t> buf_a, buf_b, cand_a, cand_b;
            buf_a.reserve(16384); buf_b.reserve(16384);
            cand_a.reserve(16384); cand_b.reserve(16384);
            char line_buf[512];

            for (size_t qidx = start_q; qidx < end_q; qidx++) {
                const std::string &q = queries[qidx];
                if (q.rfind("BOOL ", 0) == 0) {
                    const char *cur = q.c_str() + 5;
                    const char *first_word = cur;
                    while (*cur && *cur != ' ') cur++;
                    int32_t tid_val = query_dict.find(first_word, cur - first_word);

                    const uint32_t *p_data = nullptr;
                    size_t p_len = 0;
                    if (tid_val >= 0) {
                        uint32_t st_pos = term_offsets[tid_val];
                        p_len = term_offsets[tid_val + 1] - st_pos;
                        p_data = &all_postings[st_pos];
                    }

                    std::vector<uint32_t> *res_ptr = &buf_a;
                    std::vector<uint32_t> *alt_ptr = &buf_b;
                    res_ptr->assign(p_data, p_data + p_len);

                    while (*cur == ' ') {
                        cur++;
                        const char *op_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        std::string_view op(op_start, cur - op_start);
                        if (*cur == ' ') cur++;

                        const char *t_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        int32_t next_tid = query_dict.find(t_start, cur - t_start);

                        const uint32_t *next_data = nullptr;
                        size_t next_len = 0;
                        if (next_tid >= 0) {
                            uint32_t next_st = term_offsets[next_tid];
                            next_len = term_offsets[next_tid + 1] - next_st;
                            next_data = &all_postings[next_st];
                        }

                        if (op == "AND") {
                            intersect_ptrs(res_ptr->data(), res_ptr->size(), next_data, next_len, *alt_ptr);
                        } else if (op == "OR") {
                            union_ptrs(res_ptr->data(), res_ptr->size(), next_data, next_len, *alt_ptr);
                        } else if (op == "NOT") {
                            diff_ptrs(res_ptr->data(), res_ptr->size(), next_data, next_len, *alt_ptr);
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
                    std::vector<std::string_view> phrase_terms;
                    const char *tp = inner;
                    const char *tend = inner + inner_len;
                    while (tp < tend) {
                        const char *ts = tp;
                        while (tp < tend && *tp != ' ') tp++;
                        phrase_terms.emplace_back(ts, tp - ts);
                        if (tp < tend && *tp == ' ') tp++;
                    }

                    // Sort phrase terms by posting length for fast candidate intersection
                    struct PhraseTermInfo {
                        const uint32_t *data;
                        size_t len;
                    };
                    PhraseTermInfo pinfo[16];
                    size_t num_pt = std::min(phrase_terms.size(), (size_t)16);
                    bool has_empty = false;
                    for (size_t i = 0; i < num_pt; i++) {
                        int32_t ti = query_dict.find(phrase_terms[i].data(), phrase_terms[i].size());
                        if (ti < 0) { has_empty = true; break; }
                        uint32_t st = term_offsets[ti];
                        pinfo[i].len = term_offsets[ti + 1] - st;
                        pinfo[i].data = &all_postings[st];
                        if (pinfo[i].len == 0) { has_empty = true; break; }
                    }

                    std::vector<uint32_t> *res_ptr = &cand_a;
                    std::vector<uint32_t> *alt_ptr = &cand_b;
                    res_ptr->clear();

                    if (!has_empty) {
                        std::sort(pinfo, pinfo + num_pt, [](const PhraseTermInfo &a, const PhraseTermInfo &b) {
                            return a.len < b.len;
                        });
                        res_ptr->assign(pinfo[0].data, pinfo[0].data + pinfo[0].len);
                        for (size_t i = 1; i < num_pt && !res_ptr->empty(); i++) {
                            intersect_ptrs(res_ptr->data(), res_ptr->size(), pinfo[i].data, pinfo[i].len, *alt_ptr);
                            std::swap(res_ptr, alt_ptr);
                        }
                    }

                    std::vector<uint32_t> matches;
                    for (uint32_t cand_id : *res_ptr) {
                        const uint8_t *dp = docs[cand_id].ptr;
                        size_t dlen = docs[cand_id].len;
                        if (check_phrase(dp, dlen, phrase_terms)) {
                            matches.push_back(cand_id);
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
    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (size_t i = 0; i < num_queries; i++) {
        fwrite(results[i].data(), 1, results[i].size(), out);
        fputc('\n', out);
    }
    fclose(out);
    free(out_buf);
    free(all_postings);
    munmap((void *)d_data, d_size);
    close(d_fd);

    auto t_fin = std::chrono::high_resolution_clock::now();
    printf("Total Track B time: %.2f ms\n", std::chrono::duration<double, std::milli>(t_fin - t0).count());
    return 0;
}
