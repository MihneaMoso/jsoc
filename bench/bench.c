#define _POSIX_C_SOURCE 199309L
#include "jsoc.h"
#include "arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    rewind(f);

    size_t ulen = (size_t)len;
    char *buf = malloc(ulen + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, ulen, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static void run_bench(const char *name, const char *input, size_t len, int iters) {
    jsoc_arena *a = jsoc_arena_create(65536);
    if (!a) {
        fprintf(stderr, "arena allocation failed\n");
        return;
    }

    for (int i = 0; i < 3; i++) {
        jsoc_arena_reset(a);
        jsoc_result r = jsoc_tokenize(input, len, a);
        if (r.error.code != JSOC_OK) {
            fprintf(stderr, "  warmup error: %s\n", jsoc_error_message(r.error.code));
            jsoc_arena_destroy(a);
            return;
        }
    }

    double best = 0.0;
    size_t best_count = 0;
    for (int i = 0; i < iters; i++) {
        jsoc_arena_reset(a);
        double start = now();
        jsoc_result r = jsoc_tokenize(input, len, a);
        double end = now();
        if (r.error.code != JSOC_OK) {
            fprintf(stderr, "  iter %d error: %s\n", i, jsoc_error_message(r.error.code));
            jsoc_arena_destroy(a);
            return;
        }
        double elapsed = end - start;
        if (best == 0.0 || elapsed < best) {
            best = elapsed;
            best_count = r.count;
        }
    }

    double mb = (double)len / 1e6;
    double mbs = mb / best;
    printf("%-24s %8.3f ms  %8.1f MB/s  %8.2f GB/s  %zu tokens\n",
           name, best * 1000.0, mbs, mbs / 1000.0, best_count);

    jsoc_arena_destroy(a);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <json_file> [more_files...]\n", argv[0]);
        fprintf(stderr, "       BENCH_ITERS=N to set iteration count (default 20)\n");
        return 1;
    }

    int iters = 20;
    const char *env = getenv("BENCH_ITERS");
    if (env && atoi(env) > 0) iters = atoi(env);

    printf("jsoc tokenizer benchmark (%d iterations, best of run)\n\n", iters);

    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *input = read_file(argv[i], &len);
        if (!input) {
            fprintf(stderr, "Failed to read %s\n", argv[i]);
            continue;
        }
        printf("%s (%zu bytes)\n", argv[i], len);
        run_bench("tokenize", input, len, iters);
        free(input);
    }
    return 0;
}
