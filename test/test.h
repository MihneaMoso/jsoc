/*
 * Shared harness for the jsoc test binaries.
 *
 * Every test file in test/ is a self-contained suite that includes this
 * header, registers its tests in main(), and returns the number of failures.
 * Keeping the harness tiny (no dependencies, no code generation) means a test
 * file can be compiled and run standalone:
 *
 *     cc -std=c2x -O2 -Iinclude -o /tmp/t test/test_numbers.c build/libjsoc.a
 *     /tmp/t
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsoc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Each test file is its own binary, so the counters are per-file. */
static int jsoc_tests_run __attribute__((unused));
static int jsoc_tests_passed __attribute__((unused));
static int jsoc_tests_failed __attribute__((unused));

#define TEST(num, name)                                                         \
    do {                                                                        \
        jsoc_tests_run++;                                                       \
        printf("TEST %d: %s ... ", num, #name);                                 \
        fflush(stdout);                                                         \
        if (test_##name()) {                                                    \
            jsoc_tests_passed++;                                                \
            printf("PASS\n");                                                   \
        } else {                                                                \
            jsoc_tests_failed++;                                                \
            printf("FAIL\n");                                                   \
        }                                                                       \
    } while (0)

#define REQUIRE(cond)                                                           \
    do {                                                                        \
        if (!(cond)) {                                                          \
            fprintf(stderr, "  check failed at line %d: %s\n", __LINE__,        \
                    #cond);                                                     \
            goto fail;                                                          \
        }                                                                       \
    } while (0)

/* Run the registered tests and report. */
static inline int jsoc_test_finish(const char *suite) {
    (void)suite;
    printf("\n=== Results: %d/%d passed (%d failed) ===\n", jsoc_tests_passed,
           jsoc_tests_run, jsoc_tests_failed);
    return jsoc_tests_failed == 0 ? 0 : 1;
}

/* Read a whole file into a malloc'd buffer.  Returns NULL on failure and
 * leaves *out_len at 0.  Used by the corpus-driven tests, which get their data
 * from scripts/fetch-testdata.sh (see HANDOFF.md). */
static inline char *jsoc_test_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Read a file into a heap buffer of *exactly* its length, so that under the
 * address sanitizer any read past the last byte faults.  This is the C
 * equivalent of simdjson's `parse_unpadded` tests: jsoc never requires padding,
 * so the same property is what its callers rely on. */
static inline char *jsoc_test_read_exact(const char *path, size_t *out_len) {
    size_t len = 0;
    char *buf = jsoc_test_read_file(path, &len);
    if (!buf) return NULL;
    char *exact = malloc(len ? len : 1);
    if (!exact) {
        free(buf);
        return NULL;
    }
    if (len) memcpy(exact, buf, len);
    free(buf);
    *out_len = len;
    return exact;
}

/* A deterministic xorshift64 generator: the same seed produces the same inputs
 * forever, so a failure is always reproducible (simdjson's unpadded_tests and
 * its fuzzers use exactly this). */
typedef struct {
    uint64_t s;
} jsoc_rng;

static inline void jsoc_rng_seed(jsoc_rng *r, uint64_t seed) {
    r->s = seed ? seed : 0x9e3779b97f4a7c15ull;
}

static inline uint64_t jsoc_rng_next(jsoc_rng *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return r->s;
}

static inline uint32_t jsoc_rng_below(jsoc_rng *r, uint32_t n) {
    return (uint32_t)(jsoc_rng_next(r) % n);
}

#ifdef __cplusplus
}
#endif
