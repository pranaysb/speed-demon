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
#include <unordered_set>
#include <unordered_map>

struct CountryAgg {
    std::string key;
    uint64_t count = 0;
    uint64_t id_sum = 0;
    int64_t amount_sum = 0;
    int64_t amount_min = 0;
    int64_t amount_max = 0;
    bool has_amount = false;
    std::unordered_set<std::string> names;
    uint64_t name_bytes = 0;
    uint64_t note_bytes = 0;
};

// Format cents as exact string: -525 -> "-5.25", 0 -> "0.00"
static void format_cents(int64_t cents, char *out, size_t out_size) {
    if (cents < 0) {
        int64_t pos = -cents;
        snprintf(out, out_size, "-%lld.%02lld", (long long)(pos / 100), (long long)(pos % 100));
    } else {
        snprintf(out, out_size, "%lld.%02lld", (long long)(cents / 100), (long long)(cents % 100));
    }
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

    // 1. Load regions.csv
    char regions_path[1024];
    snprintf(regions_path, sizeof(regions_path), "%s/regions.csv", data_dir);

    FILE *rf = fopen(regions_path, "rb");
    if (!rf) {
        perror("fopen regions");
        return 1;
    }

    char line_buf[256];
    // Skip header
    if (!fgets(line_buf, sizeof(line_buf), rf)) {
        fclose(rf);
        return 1;
    }

    std::unordered_map<std::string, int> country_key_to_id;
    std::vector<CountryAgg> countries;

    // UNMATCHED is key "_UNMATCHED"
    int unmatched_id = 0;
    {
        CountryAgg agg;
        agg.key = "_UNMATCHED";
        country_key_to_id[agg.key] = unmatched_id;
        countries.push_back(agg);
    }

    // Fast array or hash map for region_id -> country_id
    // Max region_id in spec is < 10,000,000
    // We can use a direct vector<int32_t> initialized to unmatched_id!
    std::vector<int32_t> region_to_country;

    while (fgets(line_buf, sizeof(line_buf), rf)) {
        char *p = line_buf;
        uint32_t reg_id = 0;
        while (*p >= '0' && *p <= '9') {
            reg_id = reg_id * 10 + (*p - '0');
            p++;
        }
        if (*p == ',') p++;
        char *country_start = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        std::string country_str(country_start, p - country_start);

        auto it = country_key_to_id.find(country_str);
        int cid = 0;
        if (it == country_key_to_id.end()) {
            cid = (int)countries.size();
            CountryAgg agg;
            agg.key = country_str;
            country_key_to_id[country_str] = cid;
            countries.push_back(agg);
        } else {
            cid = it->second;
        }

        if (reg_id >= region_to_country.size()) {
            region_to_country.resize(reg_id + 1, unmatched_id);
        }
        region_to_country[reg_id] = cid;
    }
    fclose(rf);

    // 2. Memory map records.csv
    char records_path[1024];
    snprintf(records_path, sizeof(records_path), "%s/records.csv", data_dir);

    int fd = open(records_path, O_RDONLY);
    if (fd < 0) {
        perror("open records");
        return 1;
    }

    struct stat st;
    fstat(fd, &st);
    size_t file_size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap records");
        close(fd);
        return 1;
    }
    madvise((void *)data, file_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    const uint8_t *p = data;
    const uint8_t *end = data + file_size;

    // Check leading BOM
    if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
    }

    // Skip header line
    while (p < end && *p != '\n') p++;
    if (p < end && *p == '\n') p++;

    std::string unescaped_name;
    unescaped_name.reserve(256);

    while (p < end) {
        // Check for record BOM
        if (end - p >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
            p += 3;
        }
        if (p >= end) break;

        // 1. id
        uint64_t rec_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            rec_id = rec_id * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == ',') p++;

        // 2. name (quoted or unquoted)
        unescaped_name.clear();
        uint64_t name_len = 0;
        if (p < end && *p == '"') {
            p++; // skip opening quote
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        unescaped_name.push_back('"');
                        name_len++;
                        p += 2;
                    } else {
                        p++; // closing quote
                        break;
                    }
                } else {
                    unescaped_name.push_back((char)*p);
                    name_len++;
                    p++;
                }
            }
        } else {
            while (p < end && *p != ',') {
                unescaped_name.push_back((char)*p);
                name_len++;
                p++;
            }
        }
        if (p < end && *p == ',') p++;

        // 3. note (quoted or unquoted) - WE ONLY NEED UNESCAPED BYTE LENGTH!
        uint64_t note_len = 0;
        if (p < end && *p == '"') {
            p++; // skip opening quote
            while (p < end) {
                if (*p == '"') {
                    if (p + 1 < end && *(p + 1) == '"') {
                        note_len++;
                        p += 2;
                    } else {
                        p++; // closing quote
                        break;
                    }
                } else {
                    note_len++;
                    p++;
                }
            }
        } else {
            while (p < end && *p != ',') {
                note_len++;
                p++;
            }
        }
        if (p < end && *p == ',') p++;

        // 4. region_id
        uint32_t reg_id = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            reg_id = reg_id * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == ',') p++;

        // 5. amount: -?[0-9]+\.[0-9]{2}
        int64_t sign = 1;
        if (p < end && *p == '-') {
            sign = -1;
            p++;
        }
        int64_t whole = 0;
        while (p < end && *p >= '0' && *p <= '9') {
            whole = whole * 10 + (*p - '0');
            p++;
        }
        if (p < end && *p == '.') p++;
        int64_t frac = 0;
        if (p < end && *p >= '0' && *p <= '9') {
            frac = (*p - '0') * 10;
            p++;
            if (p < end && *p >= '0' && *p <= '9') {
                frac += (*p - '0');
                p++;
            }
        }
        int64_t cents = sign * (whole * 100 + frac);

        // Skip to end of record
        if (p < end && *p == '\r') p++;
        if (p < end && *p == '\n') p++;

        // Join against region
        int cid = unmatched_id;
        if (reg_id < region_to_country.size()) {
            cid = region_to_country[reg_id];
        }

        CountryAgg &agg = countries[cid];
        agg.count++;
        agg.id_sum += rec_id;
        agg.amount_sum += cents;
        if (!agg.has_amount) {
            agg.amount_min = cents;
            agg.amount_max = cents;
            agg.has_amount = true;
        } else {
            if (cents < agg.amount_min) agg.amount_min = cents;
            if (cents > agg.amount_max) agg.amount_max = cents;
        }
        agg.names.insert(unescaped_name);
        agg.name_bytes += name_len;
        agg.note_bytes += note_len;
    }

    munmap((void *)data, file_size);
    close(fd);

    // Sort countries by key in UTF-8 byte order
    std::vector<int> sorted_indices;
    for (int i = 0; i < (int)countries.size(); i++) {
        if (countries[i].count > 0) {
            sorted_indices.push_back(i);
        }
    }
    std::sort(sorted_indices.begin(), sorted_indices.end(), [&](int a, int b) {
        return countries[a].key < countries[b].key;
    });

    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/aggregates.txt", out_dir);
    mkdir(out_dir, 0777);

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        perror("fopen out");
        return 1;
    }

    char *out_buf = (char *)malloc(1024 * 1024);
    setvbuf(out, out_buf, _IOFBF, 1024 * 1024);

    char s_sum[64], s_min[64], s_max[64];

    for (int idx : sorted_indices) {
        CountryAgg &agg = countries[idx];
        format_cents(agg.amount_sum, s_sum, sizeof(s_sum));
        format_cents(agg.amount_min, s_min, sizeof(s_min));
        format_cents(agg.amount_max, s_max, sizeof(s_max));

        fprintf(out, "%s %llu %llu %s %s %s %zu %llu %llu\n",
                agg.key.c_str(),
                (unsigned long long)agg.count,
                (unsigned long long)agg.id_sum,
                s_sum, s_min, s_max,
                agg.names.size(),
                (unsigned long long)agg.name_bytes,
                (unsigned long long)agg.note_bytes);
    }

    fclose(out);
    free(out_buf);
    return 0;
}
