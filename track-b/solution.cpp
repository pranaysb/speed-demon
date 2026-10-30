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

// Robust hash table using term indices instead of raw pointers
struct TermSlot {
    uint64_t hash;
    uint32_t term_id; // UINT32_MAX = empty
};

static inline uint64_t hash_term(const char *p, size_t len) {
    uint64_t h = len ^ 0xa0761d6478bd642fULL;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ (uint8_t)p[i]) * 0x100000001b3ULL;
    }
    return h;
}

class TermDict {
public:
    size_t cap;
    size_t mask;
    std::vector<TermSlot> table;
    std::vector<std::string> terms;

    TermDict(size_t initial_cap = 1048576) {
        cap = initial_cap;
        mask = cap - 1;
        table.assign(cap, {0, UINT32_MAX});
        terms.reserve(cap / 2);
    }

    uint32_t get_or_insert(const char *p, size_t len) {
        uint64_t h = hash_term(p, len);
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h &&
                terms[table[idx].term_id] == std::string_view(p, len)) {
                return table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }

        uint32_t id = (uint32_t)terms.size();
        terms.emplace_back(p, len);
        table[idx].hash = h;
        table[idx].term_id = id;

        // Resize if load factor exceeds 70%
        if (terms.size() * 10 >= cap * 7) {
            rehash(cap * 2);
        }

        return id;
    }

    int32_t find(const char *p, size_t len) const {
        uint64_t h = hash_term(p, len);
        size_t idx = h & mask;
        while (table[idx].term_id != UINT32_MAX) {
            if (table[idx].hash == h &&
                terms[table[idx].term_id] == std::string_view(p, len)) {
                return (int32_t)table[idx].term_id;
            }
            idx = (idx + 1) & mask;
        }
        return -1;
    }

private:
    void rehash(size_t new_cap) {
        cap = new_cap;
        mask = cap - 1;
        table.assign(cap, {0, UINT32_MAX});
        for (size_t i = 0; i < terms.size(); i++) {
            const std::string &s = terms[i];
            uint64_t h = hash_term(s.c_str(), s.size());
            size_t idx = h & mask;
            while (table[idx].term_id != UINT32_MAX) {
                idx = (idx + 1) & mask;
            }
            table[idx].hash = h;
            table[idx].term_id = (uint32_t)i;
        }
    }
};

// Two-pointer set operations on sorted vectors
static std::vector<uint32_t> intersect_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    std::vector<uint32_t> res;
    res.reserve(std::min(a.size(), b.size()));
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            res.push_back(a[i]);
            i++;
            j++;
        } else if (a[i] < b[j]) {
            i++;
        } else {
            j++;
        }
    }
    return res;
}

static std::vector<uint32_t> union_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    std::vector<uint32_t> res;
    res.reserve(a.size() + b.size());
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            res.push_back(a[i]);
            i++;
            j++;
        } else if (a[i] < b[j]) {
            res.push_back(a[i]);
            i++;
        } else {
            res.push_back(b[j]);
            j++;
        }
    }
    while (i < a.size()) res.push_back(a[i++]);
    while (j < b.size()) res.push_back(b[j++]);
    return res;
}

static std::vector<uint32_t> difference_sorted(const std::vector<uint32_t> &a, const std::vector<uint32_t> &b) {
    std::vector<uint32_t> res;
    res.reserve(a.size());
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] == b[j]) {
            i++;
            j++;
        } else if (a[i] < b[j]) {
            res.push_back(a[i]);
            i++;
        } else {
            j++;
        }
    }
    while (i < a.size()) res.push_back(a[i++]);
    return res;
}

// Tokenize document text according to Track B rules
static inline void tokenize_doc(const uint8_t *text, size_t len, std::vector<std::string_view> &tokens) {
    tokens.clear();
    const uint8_t *p = text;
    const uint8_t *end = text + len;

    while (p < end) {
        // Find start of token
        while (p < end) {
            uint8_t c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                break;
            }
            p++;
        }
        if (p >= end) break;

        const uint8_t *start = p;
        while (p < end) {
            uint8_t c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                p++;
            } else {
                break;
            }
        }
        tokens.emplace_back((const char *)start, p - start);
    }
}

// Case-insensitive token match
static inline bool token_eq(std::string_view doc_tok, const std::string &query_tok) {
    if (doc_tok.size() != query_tok.size()) return false;
    for (size_t i = 0; i < doc_tok.size(); i++) {
        char c = doc_tok[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        if (c != query_tok[i]) return false;
    }
    return true;
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

    // 1. Memory map docs.txt
    char docs_path[1024];
    snprintf(docs_path, sizeof(docs_path), "%s/docs.txt", data_dir);

    int d_fd = open(docs_path, O_RDONLY);
    if (d_fd < 0) {
        perror("open docs");
        return 1;
    }

    struct stat st;
    fstat(d_fd, &st);
    size_t d_size = st.st_size;

    const uint8_t *d_data = (const uint8_t *)mmap(NULL, d_size, PROT_READ, MAP_PRIVATE, d_fd, 0);
    if (d_data == MAP_FAILED) {
        perror("mmap docs");
        close(d_fd);
        return 1;
    }
    madvise((void *)d_data, d_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    std::vector<DocSpan> docs;
    docs.reserve(300000);

    TermDict dict(1048576);
    std::vector<std::vector<uint32_t>> postings;
    postings.reserve(1048576);

    const uint8_t *p = d_data;
    const uint8_t *end = d_data + d_size;

    char term_buf[64];
    uint32_t doc_id = 0;
    while (p < end) {
        // id \t text \n
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p < end && *p == '\t') p++;

        const uint8_t *text_start = p;
        while (p < end && *p != '\n') p++;
        size_t text_len = p - text_start;
        if (text_len > 0 && text_start[text_len - 1] == '\r') {
            text_len--;
        }
        if (p < end && *p == '\n') p++;

        docs.push_back({text_start, (uint32_t)text_len});

        // Tokenize text
        const uint8_t *tp = text_start;
        const uint8_t *tend = text_start + text_len;

        while (tp < tend) {
            while (tp < tend) {
                uint8_t c = *tp;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                    break;
                }
                tp++;
            }
            if (tp >= tend) break;

            size_t tlen = 0;
            while (tp < tend && tlen < sizeof(term_buf) - 1) {
                uint8_t c = *tp;
                if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                    term_buf[tlen++] = (char)c;
                    tp++;
                } else if (c >= 'A' && c <= 'Z') {
                    term_buf[tlen++] = (char)(c + ('a' - 'A'));
                    tp++;
                } else {
                    break;
                }
            }
            while (tp < tend) {
                uint8_t c = *tp;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
                    tp++;
                } else {
                    break;
                }
            }

            uint32_t tid = dict.get_or_insert(term_buf, tlen);
            if (tid >= postings.size()) {
                postings.resize(tid + 1);
            }
            if (postings[tid].empty() || postings[tid].back() != doc_id) {
                postings[tid].push_back(doc_id);
            }
        }

        doc_id++;
    }

    // 2. Read queries.txt
    char queries_path[1024];
    snprintf(queries_path, sizeof(queries_path), "%s/queries.txt", data_dir);

    FILE *qf = fopen(queries_path, "rb");
    if (!qf) {
        perror("fopen queries");
        return 1;
    }

    std::vector<std::string> queries;
    queries.reserve(60000);
    char qline[4096];
    while (fgets(qline, sizeof(qline), qf)) {
        size_t len = strlen(qline);
        while (len > 0 && (qline[len - 1] == '\r' || qline[len - 1] == '\n')) {
            qline[--len] = '\0';
        }
        if (len > 0) {
            queries.emplace_back(qline, len);
        }
    }
    fclose(qf);

    size_t num_queries = queries.size();
    std::vector<std::string> results(num_queries);

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
            std::vector<std::string_view> doc_tokens;
            doc_tokens.reserve(1024);

            for (size_t qidx = start_q; qidx < end_q; qidx++) {
                const std::string &q = queries[qidx];
                if (q.rfind("BOOL ", 0) == 0) {
                    // BOOL t1 OP t2 OP t3 ...
                    const char *cur = q.c_str() + 5;
                    const char *w_start = cur;
                    while (*cur && *cur != ' ') cur++;
                    int32_t tid = dict.find(w_start, cur - w_start);

                    static const std::vector<uint32_t> empty_vec;
                    std::vector<uint32_t> cur_res = (tid >= 0 && (size_t)tid < postings.size()) ? postings[tid] : empty_vec;

                    while (*cur == ' ') {
                        cur++; // skip space
                        const char *op_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        std::string_view op(op_start, cur - op_start);
                        if (*cur == ' ') cur++;

                        const char *t_start = cur;
                        while (*cur && *cur != ' ') cur++;
                        int32_t next_tid = dict.find(t_start, cur - t_start);
                        const std::vector<uint32_t> &next_post = (next_tid >= 0 && (size_t)next_tid < postings.size()) ? postings[next_tid] : empty_vec;

                        if (op == "AND") {
                            cur_res = intersect_sorted(cur_res, next_post);
                        } else if (op == "OR") {
                            cur_res = union_sorted(cur_res, next_post);
                        } else if (op == "NOT") {
                            cur_res = difference_sorted(cur_res, next_post);
                        }
                    }

                    char buf[512];
                    uint64_t id_sum = 0;
                    for (uint32_t id : cur_res) id_sum += id;
                    int written = snprintf(buf, sizeof(buf), "%zu %llu", cur_res.size(), (unsigned long long)id_sum);
                    size_t limit = std::min(cur_res.size(), (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        written += snprintf(buf + written, sizeof(buf) - written, " %u", cur_res[i]);
                    }
                    results[qidx] = buf;

                } else if (q.rfind("PHRASE \"", 0) == 0) {
                    // PHRASE "t1 t2 ... tk"
                    const char *inner = q.c_str() + 8;
                    size_t inner_len = q.size() - 9; // exclude trailing "
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
                    int32_t t0 = dict.find(phrase_terms[0].c_str(), phrase_terms[0].size());
                    std::vector<uint32_t> candidates = (t0 >= 0 && (size_t)t0 < postings.size()) ? postings[t0] : empty_vec;

                    for (size_t i = 1; i < phrase_terms.size() && !candidates.empty(); i++) {
                        int32_t ti = dict.find(phrase_terms[i].c_str(), phrase_terms[i].size());
                        const std::vector<uint32_t> &post = (ti >= 0 && (size_t)ti < postings.size()) ? postings[ti] : empty_vec;
                        candidates = intersect_sorted(candidates, post);
                    }

                    std::vector<uint32_t> matches;
                    size_t k = phrase_terms.size();

                    for (uint32_t cand_id : candidates) {
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

                    char buf[512];
                    uint64_t id_sum = 0;
                    for (uint32_t id : matches) id_sum += id;
                    int written = snprintf(buf, sizeof(buf), "%zu %llu", matches.size(), (unsigned long long)id_sum);
                    size_t limit = std::min(matches.size(), (size_t)10);
                    for (size_t i = 0; i < limit; i++) {
                        written += snprintf(buf + written, sizeof(buf) - written, " %u", matches[i]);
                    }
                    results[qidx] = buf;
                }
            }
        });
    }

    for (auto &t : workers) {
        t.join();
    }

    // Write output
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/results.txt", out_dir);
    mkdir(out_dir, 0777);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen results");
        return 1;
    }

    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    for (size_t i = 0; i < num_queries; i++) {
        fprintf(out, "%s\n", results[i].c_str());
    }

    fclose(out);
    free(out_buf);
    munmap((void *)d_data, d_size);
    close(d_fd);

    return 0;
}
