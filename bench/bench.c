#define _POSIX_C_SOURCE 199309L
#include "arena.h"
#include "jsoc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile uintptr_t bench_sink;

static double now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);

  size_t ulen = (size_t)len;
  char *buf = malloc(ulen + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t got = fread(buf, 1, ulen, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

typedef enum {
  MODE_TOKENIZE = 1u << 0,
  MODE_PARSE = 1u << 1,
  MODE_MANY = 1u << 2,
  MODE_UTF8 = 1u << 3,
  MODE_ALL = MODE_TOKENIZE | MODE_PARSE | MODE_MANY | MODE_UTF8,
} mode_mask;

static size_t many_docs;
static uintptr_t many_sink;

static bool count_document(const jsoc_document *doc, void *user) {
  (void)user;
  many_docs++;
  if (doc->root)
    many_sink += (uintptr_t)doc->root;
  return true;
}

typedef struct {
  const char *name;
  const char *input;
  size_t len;
  int iters;
} bench_job;

static void run_tokenize(const bench_job *job) {
  jsoc_arena *a = jsoc_arena_create(job->len + 65536);
  if (!a)
    return;

  for (int i = 0; i < 3; i++) {
    jsoc_arena_reset(a);
    jsoc_result r = jsoc_tokenize(job->input, job->len, a);
    if (r.error.code != JSOC_OK) {
      fprintf(stderr, "  tokenize warmup error: %s\n",
              jsoc_error_message(r.error.code));
      jsoc_arena_destroy(a);
      return;
    }
  }

  double best = 0.0;
  size_t best_count = 0;
  for (int i = 0; i < job->iters; i++) {
    jsoc_arena_reset(a);
    const double start = now();
    jsoc_result r = jsoc_tokenize(job->input, job->len, a);
    const double end = now();
    if (r.error.code != JSOC_OK) {
      fprintf(stderr, "  tokenize iter error: %s\n",
              jsoc_error_message(r.error.code));
      jsoc_arena_destroy(a);
      return;
    }
    const double elapsed = end - start;
    if (best == 0.0 || elapsed < best) {
      best = elapsed;
      best_count = r.count;
    }
    bench_sink += (uintptr_t)r.tokens;
  }

  const double mbs = ((double)job->len / 1e6) / best;
  printf("  %-10s %8.3f ms  %8.2f GB/s  %10.2f Mtoken/s  %zu tokens\n",
         "tokenize", best * 1000.0, mbs / 1000.0,
         (double)best_count / best / 1e6, best_count);
  jsoc_arena_destroy(a);
}

static void run_parse(const bench_job *job) {
  jsoc_arena *a = jsoc_arena_create(job->len + 65536);
  if (!a)
    return;

  size_t values = 0;
  for (int i = 0; i < 3; i++) {
    jsoc_arena_reset(a);
    jsoc_document doc = jsoc_parse(job->input, job->len, a);
    if (doc.error.code != JSOC_OK) {
      fprintf(stderr, "  parse warmup error: %s\n",
              jsoc_error_message(doc.error.code));
      jsoc_arena_destroy(a);
      return;
    }
    values = doc.value_count;
  }

  double best = 0.0;
  for (int i = 0; i < job->iters; i++) {
    jsoc_arena_reset(a);
    const double start = now();
    jsoc_document doc = jsoc_parse(job->input, job->len, a);
    const double end = now();
    if (doc.error.code != JSOC_OK) {
      fprintf(stderr, "  parse iter error: %s\n",
              jsoc_error_message(doc.error.code));
      jsoc_arena_destroy(a);
      return;
    }
    const double elapsed = end - start;
    if (best == 0.0 || elapsed < best)
      best = elapsed;
    bench_sink += (uintptr_t)doc.root;
  }

  const double mbs = ((double)job->len / 1e6) / best;
  printf("  %-10s %8.3f ms  %8.2f GB/s  %10.2f Mvalue/s %zu values\n", "parse",
         best * 1000.0, mbs / 1000.0, (double)values / best / 1e6, values);
  jsoc_arena_destroy(a);
}

static void run_many(const bench_job *job) {
  jsoc_arena *a = jsoc_arena_create(job->len / 2 + 65536);
  if (!a)
    return;

  size_t docs = 0;
  for (int i = 0; i < 3; i++) {
    jsoc_arena_reset(a);
    many_docs = 0;
    jsoc_error err =
        jsoc_parse_many(job->input, job->len, a, count_document, NULL);
    if (err.code != JSOC_OK) {
      fprintf(stderr, "  parse_many warmup error: %s\n",
              jsoc_error_message(err.code));
      jsoc_arena_destroy(a);
      return;
    }
    docs = many_docs;
  }

  double best = 0.0;
  for (int i = 0; i < job->iters; i++) {
    jsoc_arena_reset(a);
    many_docs = 0;
    const double start = now();
    jsoc_error err =
        jsoc_parse_many(job->input, job->len, a, count_document, NULL);
    const double end = now();
    if (err.code != JSOC_OK) {
      fprintf(stderr, "  parse_many iter error: %s\n",
              jsoc_error_message(err.code));
      jsoc_arena_destroy(a);
      return;
    }
    const double elapsed = end - start;
    if (best == 0.0 || elapsed < best)
      best = elapsed;
  }

  const double mbs = ((double)job->len / 1e6) / best;
  printf("  %-10s %8.3f ms  %8.2f GB/s  %10.2f Kdoc/s   %zu docs\n",
         "parse_many", best * 1000.0, mbs / 1000.0, (double)docs / best / 1e3,
         docs);
  bench_sink += many_sink;
  jsoc_arena_destroy(a);
}

static void run_utf8(const bench_job *job) {
  size_t off = 0;
  for (int i = 0; i < 3; i++) {
    if (!jsoc_validate_utf8(job->input, job->len, &off)) {
      fprintf(stderr, "  utf8 warmup error at %zu\n", off);
      return;
    }
  }

  double best = 0.0;
  for (int i = 0; i < job->iters; i++) {
    const double start = now();
    const bool ok = jsoc_validate_utf8(job->input, job->len, &off);
    const double end = now();
    if (!ok) {
      fprintf(stderr, "  utf8 iter error at %zu\n", off);
      return;
    }
    const double elapsed = end - start;
    if (best == 0.0 || elapsed < best)
      best = elapsed;
  }

  const double mbs = ((double)job->len / 1e6) / best;
  printf("  %-10s %8.3f ms  %8.2f GB/s\n", "utf8", best * 1000.0, mbs / 1000.0);
}

static bool has_suffix(const char *s, const char *suffix) {
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);
  return n >= m && memcmp(s + n - m, suffix, m) == 0;
}

static mode_mask parse_modes(const char *s) {
  if (!s || strcmp(s, "all") == 0)
    return MODE_ALL;
  mode_mask m = 0;
  if (strstr(s, "tokenize"))
    m |= MODE_TOKENIZE;
  if (strstr(s, "parse_many") || strstr(s, "many"))
    m |= MODE_MANY;
  if (strstr(s, "parse"))
    m |= MODE_PARSE;
  if (strstr(s, "utf8"))
    m |= MODE_UTF8;
  return m ? m : MODE_ALL;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: %s <json_file> [more_files...]\n", argv[0]);
    fprintf(stderr, "       BENCH_ITERS=N   iterations (default 20)\n");
    fprintf(stderr,
            "       BENCH_MODE=M    tokenize|parse|parse_many|utf8|all\n");
    return 1;
  }

  int iters = 20;
  const char *env = getenv("BENCH_ITERS");
  if (env && atoi(env) > 0)
    iters = atoi(env);
  const mode_mask modes = parse_modes(getenv("BENCH_MODE"));

  printf("jsoc benchmark (%d iterations, best of run)\n\n", iters);

  for (int i = 1; i < argc; i++) {
    size_t len = 0;
    char *input = read_file(argv[i], &len);
    if (!input) {
      fprintf(stderr, "Failed to read %s\n", argv[i]);
      continue;
    }
    printf("%s (%zu bytes)\n", argv[i], len);
    bench_job job = {
        .name = argv[i], .input = input, .len = len, .iters = iters};
    const bool ndjson =
        has_suffix(argv[i], ".ndjson") || has_suffix(argv[i], ".jsonl");
    if (modes & MODE_TOKENIZE)
      run_tokenize(&job);
    /* A concatenated-document file is not a single JSON document. */
    if ((modes & MODE_PARSE) && !ndjson)
      run_parse(&job);
    if ((modes & MODE_MANY) || ((modes & MODE_PARSE) && ndjson)) {
      run_many(&job);
    }
    if (modes & MODE_UTF8)
      run_utf8(&job);
    free(input);
  }

  if (bench_sink == 0x1)
    fprintf(stderr, " ");
  return 0;
}
