/*
 * Side-by-side DOM benchmark: jsoc vs simdjson.
 *
 * Both parsers are driven from the same process, over the same buffer, with
 * the same protocol as bench/bench.c: three warmup parses, then N timed
 * iterations, reporting the best wall-clock time.
 *
 * By default each parser runs one contiguous phase per file (all of its timed
 * iterations back to back, best time kept), and the phase order alternates
 * between files so slow drift affects both sides equally.  Set BENCH_BLOCK=N
 * to interleave in blocks of N iterations instead.  Multiple files may be
 * passed; a summary with the geometric-mean speedup and the aggregate
 * throughput closes the report.
 *
 * This file is deliberately not part of the default build; see bench/compare.sh.
 */

#include "simdjson.h"

extern "C" {
#include "jsoc.h"
#include "arena.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
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

static bool count_doc(const jsoc_document *doc, void *user) {
    size_t *n = (size_t *)user;
    (void)doc;
    (*n)++;
    return true;
}

/* One timed jsoc parse.  Returns seconds, or -1 on error (reported here). */
static double time_jsoc(const char *input, size_t length, bool many,
                        jsoc_arena *a, size_t *units, const char *path) {
    jsoc_arena_reset(a);
    const double start = now();
    double end;
    if (many) {
        size_t count = 0;
        jsoc_error err = jsoc_parse_many(input, length, a, count_doc, &count);
        end = now();
        if (err.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc parse_many error on %s: %s\n",
                         path, jsoc_error_message(err.code));
            return -1.0;
        }
        *units = count;
    } else {
        jsoc_document doc = jsoc_parse(input, length, a);
        end = now();
        if (doc.error.code != JSOC_OK) {
            std::fprintf(stderr, "jsoc parse error on %s: %s\n",
                         path, jsoc_error_message(doc.error.code));
            return -1.0;
        }
        *units = doc.value_count;
    }
    return end - start;
}

/* One timed simdjson DOM parse.  Returns seconds, or -1 on error. */
static double time_simdjson(const char *input, size_t length, bool many,
                            simdjson::dom::parser &p, size_t *units,
                            const char *path) {
    if (many) {
        const double start = now();
        auto stream = p.parse_many(input, length, simdjson::dom::DEFAULT_BATCH_SIZE);
        size_t count = 0;
        for (auto doc : stream) {
            if (doc.error()) {
                std::fprintf(stderr, "simdjson parse_many error on %s: %s\n",
                             path, simdjson::error_message(doc.error()));
                return -1.0;
            }
            count++;
        }
        const double end = now();
        *units = count;
        return end - start;
    }
    const double start = now();
    auto doc = p.parse(input, length);
    const double end = now();
    if (doc.error()) {
        std::fprintf(stderr, "simdjson parse error on %s: %s\n",
                     path, simdjson::error_message(doc.error()));
        return -1.0;
    }
    (void)units;
    return end - start;
}

static int choose_iters(size_t length) {
    const char *env = std::getenv("BENCH_ITERS");
    if (env) {
        int n = std::atoi(env);
        if (n > 0) return n;
    }
    // Aim for roughly 200 MB of work per parser; keep runs bounded.
    double n = 200e6 / (double)(length ? length : 1);
    if (n < 20.0) n = 20.0;
    if (n > 500.0) n = 500.0;
    return (int)n;
}

struct Sample {
    const char *path;
    size_t length;
    double jsoc_s;
    double simdjson_s;
    double speedup;      // simdjson_s / jsoc_s, > 1 means jsoc faster
};

int main(int argc, char **argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <json_file> [more_files...]\n", argv[0]);
        std::fprintf(stderr, "       BENCH_ITERS=N   iterations (default auto)\n");
        return 1;
    }

    std::printf("jsoc vs simdjson DOM, same process, best of run\n");
    std::printf("simdjson 5.1.1 (ae0fb53); lower ratio = jsoc faster\n\n");
    std::printf("%-34s %10s %10s %10s %9s\n",
                "file", "jsoc", "simdjson", "jsoc/sj", "speedup");
    std::printf("%-34s %10s %10s %10s %9s\n",
                "", "GB/s", "GB/s", "time", "x");

    std::vector<Sample> samples;
    double total_bytes = 0.0;
    double total_jsoc = 0.0;
    double total_sj = 0.0;
    int failures = 0;

    for (int arg = 1; arg < argc; arg++) {
        std::vector<char> buf;
        size_t length = 0;
        if (!read_file(argv[arg], buf, &length)) {
            std::fprintf(stderr, "Failed to read %s\n", argv[arg]);
            failures++;
            continue;
        }
        if (length == 0) {
            std::fprintf(stderr, "Skipping empty file %s\n", argv[arg]);
            continue;
        }
        const char *input = buf.data();
        const bool many = has_suffix(argv[arg], ".ndjson") ||
                          has_suffix(argv[arg], ".jsonl");
        const int iters = choose_iters(length);

        jsoc_arena *a = jsoc_arena_create(length + 65536);
        if (!a) {
            std::fprintf(stderr, "jsoc arena allocation failed for %s\n", argv[arg]);
            failures++;
            continue;
        }

        simdjson::dom::parser p;

        // Warm both parsers.
        bool ok = true;
        size_t jsoc_units = 0, sj_units = 0;
        for (int i = 0; i < 3 && ok; i++) {
            if (time_jsoc(input, length, many, a, &jsoc_units, argv[arg]) < 0.0)
                ok = false;
            if (ok && time_simdjson(input, length, many, p, &sj_units, argv[arg]) < 0.0)
                ok = false;
        }
        if (!ok) {
            jsoc_arena_destroy(a);
            failures++;
            continue;
        }

        double best_jsoc = 0.0, best_sj = 0.0;
        // Contiguous phases per file reproduce the usual single-parser
        // benchmark protocol (repeated parses, best time) and avoid the
        // cache/TLB artifacts of rapid alternation, which measurably distort
        // a few mid-sized documents.  The phase order alternates per file so
        // slow drift (thermal, background load) cancels across the suite.
        // BENCH_BLOCK=N reintroduces interleaving in blocks of N if wanted.
        int block = iters;
        if (const char *env = std::getenv("BENCH_BLOCK")) {
            int n = std::atoi(env);
            if (n > 0) block = n;
        }
        // After each switch a couple of untimed settle parses re-warm the
        // incoming parser; harmless here, vital when BENCH_BLOCK is small.
        const char *settle_env = std::getenv("BENCH_SETTLE");
        const int settle = settle_env ? std::atoi(settle_env) : 2;
        const bool jsoc_first_file = (arg & 1) != 0;
        // Keep each parser's code and data hot for a block of iterations, and
        // alternate the block order between parsers so slow drift (thermal,
        // background load) hits both sides equally.  Both parsers always run
        // exactly `iters` timed parses.
        for (int done = 0; done < iters; ) {
            int n = std::min(block, iters - done);
            const bool jsoc_first = (((done / block) & 1) == 0) == jsoc_first_file;
            for (int phase = 0; phase < 2 && ok; phase++) {
                const bool run_jsoc = (phase == 0) == jsoc_first;
                for (int k = -settle; k < n && ok; k++) {
                    if (run_jsoc) {
                        double jt = time_jsoc(input, length, many, a,
                                              &jsoc_units, argv[arg]);
                        if (jt < 0.0) { ok = false; break; }
                        if (k >= 0 && (best_jsoc == 0.0 || jt < best_jsoc))
                            best_jsoc = jt;
                    } else {
                        double st = time_simdjson(input, length, many, p,
                                                  &sj_units, argv[arg]);
                        if (st < 0.0) { ok = false; break; }
                        if (k >= 0 && (best_sj == 0.0 || st < best_sj))
                            best_sj = st;
                    }
                }
            }
            done += n;
        }

        if (!ok) {
            jsoc_arena_destroy(a);
            failures++;
            continue;
        }

        const double jsoc_gbps = ((double)length / 1e9) / best_jsoc;
        const double sj_gbps = ((double)length / 1e9) / best_sj;
        const double ratio = best_jsoc / best_sj;
        const double speedup = best_sj / best_jsoc;

        // Show a short, unique tail of the path.
        const char *name = argv[arg];
        const char *slash = std::strrchr(name, '/');
        if (slash) name = slash + 1;

        std::printf("%-34s %10.2f %10.2f %10.3f %8.3fx  (%zu B, %d it)\n",
                    name, jsoc_gbps, sj_gbps, ratio, speedup, length, iters);
        std::fflush(stdout);

        samples.push_back({argv[arg], length, best_jsoc, best_sj, speedup});
        total_bytes += (double)length;
        total_jsoc += best_jsoc;
        total_sj += best_sj;
        jsoc_arena_destroy(a);
    }

    if (samples.empty()) {
        std::fprintf(stderr, "No files benchmarked.\n");
        return 1;
    }

    // Geometric mean of per-file speedups (equal weight per document).
    double log_sum = 0.0;
    double min_speedup = samples[0].speedup, max_speedup = samples[0].speedup;
    for (const Sample &s : samples) {
        log_sum += std::log(s.speedup);
        min_speedup = std::min(min_speedup, s.speedup);
        max_speedup = std::max(max_speedup, s.speedup);
    }
    const double geomean = std::exp(log_sum / (double)samples.size());

    const double agg_jsoc = total_bytes / total_jsoc / 1e9;
    const double agg_sj = total_bytes / total_sj / 1e9;

    std::printf("\n");
    std::printf("summary over %zu files\n", samples.size());
    std::printf("  aggregate throughput   jsoc %6.2f GB/s   simdjson %6.2f GB/s\n",
                agg_jsoc, agg_sj);
    std::printf("  geometric-mean speedup %6.3fx (min %.3fx, max %.3fx)\n",
                geomean, min_speedup, max_speedup);
    std::printf("  jsoc wins %zu, simdjson wins %zu, ties %zu\n",
                (size_t)std::count_if(samples.begin(), samples.end(),
                                      [](const Sample &s) { return s.speedup > 1.005; }),
                (size_t)std::count_if(samples.begin(), samples.end(),
                                      [](const Sample &s) { return s.speedup < 0.995; }),
                (size_t)std::count_if(samples.begin(), samples.end(),
                                      [](const Sample &s) {
                                          return s.speedup >= 0.995 && s.speedup <= 1.005;
                                      }));
    if (failures) std::printf("  %d file(s) failed\n", failures);

    return failures ? 1 : 0;
}
