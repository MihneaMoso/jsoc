# jsoc

A JSON library in C23: a zero-copy tokenizer, a DOM parser, a streaming
(NDJSON) parser, JSON Pointer navigation, a JSON text writer, minification, an
On-Demand-style cursor, and reflected decoding. Everything is arena-allocated:
no per-value `malloc`/`free`, and no code depends on threads.

## Headers

Include `jsoc.h` for the whole API, or pick a header individually:

| header        | what it provides                                                |
|---------------|-----------------------------------------------------------------|
| `jsoc.h`      | `jsoc_tokenize`, `jsoc_tokenize_file`, `jsoc_result`            |
| `dom.h`       | DOM values, `jsoc_parse` / `jsoc_parse_file`, `jsoc_parse_many`  |
| `path.h`      | JSON Pointer (RFC 6901) lookup and pointer building              |
| `writer.h`    | arena-backed JSON text writer                                    |
| `serialize.h` | `jsoc_minify` (whitespace-stripped re-serialization)             |
| `cursor.h`    | forward-only depth-first cursor over a parsed document           |
| `decode.h`    | reflected decoding into `struct`s via descriptor tables          |
| `arena.h`     | the arena handle and its allocation helpers                      |
| `token.h`     | `jsoc_token` (12 bytes) and its type names                       |
| `error.h`     | `jsoc_error`, `jsoc_error_code`, formatting                      |
| `export.h`    | visibility/attribute macros                                      |

## Build

```bash
make                 # release (-O3)
make release-native  # -O3 -march=native: enables the AVX2 backends
make debug           # -O0 -g3
make asan            # ASan + UBSan
make PORTABLE=1 release   # force the scalar backends (no SIMD)

make test            # test_tokenizer + test_dom
make bench           # throughput benchmark (BENCH_ARGS / BENCH_ITERS)
make corpus          # acceptance corpora (CORPUS_ARGS)
make check           # build asan and run the tests
make install         # /usr/local + a pkg-config file
```

Build variants are independent: `build/.cflags` stamps the effective command
line, so switching targets never mixes objects from different flag sets.

## Validate and inspect

```c
#include "jsoc.h"

jsoc_arena *arena = jsoc_arena_create(0);
const char *doc = "{\"name\":\"ada\",\"tags\":[\"a\",\"b\"],\"age\":36}";

jsoc_result tokens = jsoc_tokenize(doc, strlen(doc), arena);
for (size_t i = 0; i < tokens.count; i++) {
    // tokens.tokens[i].offset indexes into `doc`
}

jsoc_document d = jsoc_parse(doc, strlen(doc), arena);
const jsoc_value *name = jsoc_object_get_cstr(&d, d.root, "name");
jsoc_string s = jsoc_value_get_string(&d, name);  // decoded, NUL-terminated
```

## Compare against simdjson

`bench/compare.sh` builds the normal benchmark harness plus a C++ harness that
times both parsers in the same process over the same files:

```bash
SIMDJSON_DIR=/path/to/simdjson BENCH_ITERS=200 bench/compare.sh file1.json ...
```

simdjson is located from `SIMDJSON_DIR` (a checkout with `singleheader/`), then
`pkg-config --exists simdjson`, then `/usr/include/simdjson.h`.

## Layout

```
include/  public headers (see table above)
src/      arena.c error.c token.c tokenizer.c dom.c path.c writer.c
          serialize.c cursor.c decode.c
          + headers shared internally: simd.h utf8.h number.h dom_internal.h
          tokenizer_internal.h
test/     test_tokenizer.c (29 tests, incl. differential fuzzing)
          test_dom.c (21 tests: DOM, pointer, writer, cursor, decode)
          corpus.c  (acceptance-corpus runner)
bench/    bench.c  + compare.sh / compare_simdjson.cpp (optional, needs simdjson)
legacy/   the old implementation; reference only, not built
```

`HANDOFF.md` documents the invariants, the performance work that has already
been measured, and what is worth trying next.
