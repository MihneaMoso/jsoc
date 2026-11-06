/*
 * Side-by-side DOM benchmark: jsoc vs simdjson.
 *
 * Both parsers are driven from the same process, over the same buffer, with
 * the same protocol as bench/bench.c: three warmup parses, then N timed
 * iterations, reporting the best wall-clock time.
 *
 * This file is deliberately not part of the default build; see bench/compare.sh.
 */

#include "simdjson.h"

extern "C" {
#include "jsoc.h"
#include "arena.h"
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double now() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static bool read_file(const char *path, std::vector<char> &out, size_t *length) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    long n = std::ftell(f);
    if (n < 0) { std::fclose(f); return false; }
    std::rewind(f);
    out.assign((size_t)n + simdjson::SIMDJSON_PADDING + 1, '\0');
    size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    // simdjson reads up to SIMDJSON_PADDING bytes past the end; keep it zeroed.
    std::memset(out.data() + got, 0, simdjson::SIMDJSON_PADDING);
    *length = got;
    return true;
}

static bool has_suffix(const char *s, const char *suffix) {
    size_t n = std::strlen(s), m = std::strlen(suffix);
    return n >= m && std::memcmp(s + n - m, suffix, m) == 0;
}

static double bench_jsoc_single(const char *input, size_t length, int iters,
                                size_t *out_values) {
    jsoc_arena *a = jsoc_arena_create(length + 65536);
    if (!a) return -1.0;

    size_t values = 0;
    for (int i = 0; i < 3; i++) {
        jsoc_arena_reset(a);
        jsoc_document doc = jsoc_parse(input, length, a);
        if (doc.error.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc warmup error: %s\n",
                         jsoc_error_message(doc.error.code));
            jsoc_arena_destroy(a);
            return -1.0;
        }
        values = doc.value_count;
    }

    double best = 0.0;
    for (int i = 0; i < iters; i++) {
        jsoc_arena_reset(a);
        const double start = now();
        jsoc_document doc = jsoc_parse(input, length, a);
        const double end = now();
        if (doc.error.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc iter error: %s\n",
                         jsoc_error_message(doc.error.code));
            jsoc_arena_destroy(a);
            return -1.0;
        }
        if (best == 0.0 || end - start < best) best = end - start;
    }
    jsoc_arena_destroy(a);
    *out_values = values;
    return best;
}

static bool count_doc(const jsoc_document *doc, void *user) {
    size_t *n = (size_t *)user;
    (void)doc;
    (*n)++;
    return true;
}

static double bench_jsoc_many(const char *input, size_t length, int iters,
                              size_t *out_docs) {
    jsoc_arena *a = jsoc_arena_create(length / 2 + 65536);
    if (!a) return -1.0;

    size_t docs = 0;
    for (int i = 0; i < 3; i++) {
        jsoc_arena_reset(a);
        docs = 0;
        jsoc_error err = jsoc_parse_many(input, length, a, count_doc, &docs);
        if (err.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc parse_many warmup error: %s\n",
                         jsoc_error_message(err.code));
            jsoc_arena_destroy(a);
            return -1.0;
        }
    }

    double best = 0.0;
    for (int i = 0; i < iters; i++) {
        jsoc_arena_reset(a);
        size_t count = 0;
        const double start = now();
        jsoc_error err = jsoc_parse_many(input, length, a, count_doc, &count);
        const double end = now();
        if (err.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc parse_many iter error: %s\n",
                         jsoc_error_message(err.code));
            jsoc_arena_destroy(a);
            return -1.0;
        }
        if (best == 0.0 || end - start < best) best = end - start;
    }
    jsoc_arena_destroy(a);
    *out_docs = docs;
    return best;
}

static double bench_simdjson_single(const char *input, size_t length, int iters) {
    simdjson::dom::parser p;
    for (int i = 0; i < 3; i++) {
        auto doc = p.parse(input, length);
        if (doc.error()) {
            std::fprintf(stderr, "simdjson warmup error: %s\n",
                         simdjson::error_message(doc.error()));
            return -1.0;
        }
    }
    double best = 0.0;
    for (int i = 0; i < iters; i++) {
        const double start = now();
        auto doc = p.parse(input, length);
        const double end = now();
        if (doc.error()) {
            std::fprintf(stderr, "simdjson iter error: %s\n",
                         simdjson::error_message(doc.error()));
            return -1.0;
        }
        if (best == 0.0 || end - start < best) best = end - start;
    }
    return best;
}

static double bench_simdjson_many(const char *input, size_t length, int iters) {
    simdjson::dom::parser p;
    for (int i = 0; i < 3; i++) {
        auto stream = p.parse_many(input, length, simdjson::dom::DEFAULT_BATCH_SIZE);
        size_t n = 0;
        for (auto doc : stream) {
            if (doc.error()) {
                std::fprintf(stderr, "simdjson parse_many warmup error: %s\n",
                             simdjson::error_message(doc.error()));
                return -1.0;
            }
            n++;
        }
        (void)n;
    }
    double best = 0.0;
    for (int i = 0; i < iters; i++) {
        const double start = now();
        auto stream = p.parse_many(input, length, simdjson::dom::DEFAULT_BATCH_SIZE);
        size_t n = 0;
        for (auto doc : stream) {
            if (doc.error()) {
                std::fprintf(stderr, "simdjson parse_many iter error: %s\n",
                             simdjson::error_message(doc.error()));
                return -1.0;
            }
            n++;
        }
        const double end = now();
        (void)n;
        if (best == 0.0 || end - start < best) best = end - start;
    }
    return best;
}

static void report(const char *name, double ms, size_t length, size_t units,
                   const char *unit) {
    const double gbps = ms > 0.0 ? ((double)length / 1e9) / (ms / 1000.0) : 0.0;
    std::printf("  %-16s %8.3f ms  %7.2f GB/s  %10.2f M%s/s\n",
                name, ms, gbps, (double)units / (ms / 1000.0) / 1e6, unit);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <json_file> [more_files...]\n", argv[0]);
        std::fprintf(stderr, "       BENCH_ITERS=N   iterations (default 200)\n");
        return 1;
    }

    int iters = 200;
    if (const char *env = std::getenv("BENCH_ITERS")) {
        int n = std::atoi(env);
        if (n > 0) iters = n;
    }

    std::printf("jsoc vs simdjson DOM (%d iterations, best of run, same process)\n\n",
                iters);

    for (int i = 1; i < argc; i++) {
        std::vector<char> buf;
        size_t length = 0;
        if (!read_file(argv[i], buf, &length)) {
            std::fprintf(stderr, "Failed to read %s\n", argv[i]);
            continue;
        }
        const char *input = buf.data();
        const bool many = has_suffix(argv[i], ".ndjson") ||
                          has_suffix(argv[i], ".jsonl");

        std::printf("%s (%zu bytes)\n", argv[i], length);

        size_t units = 0;
        double jsoc = many ? bench_jsoc_many(input, length, iters, &units)
                           : bench_jsoc_single(input, length, iters, &units);
        double sj = many ? bench_simdjson_many(input, length, iters)
                         : bench_simdjson_single(input, length, iters);
        if (jsoc < 0.0 || sj < 0.0) continue;

        const char *unit = many ? "doc" : "value";
        report("jsoc parse", jsoc * 1000.0, length, units, unit);
        report("simdjson DOM", sj * 1000.0, length, units, unit);
        std::printf("  %-16s %8.3fx\n", "jsoc/simdjson", jsoc / sj);
        std::printf("\n");
    }
    return 0;
}
