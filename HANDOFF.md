# jsoc — handoff

Continuation notes for the C23 JSON library rewrite.

## TL;DR state

- **All tests pass** in `debug`, `release`, `release-native`,
  `PORTABLE=1 release`, and `asan` (ASan+UBSan):
  - `test_tokenizer`: 29/29 (includes the differential fuzzer),
  - `test_dom`: 21/21 (DOM, pointer, writer, minify, cursor, decode).
- **No compiler warnings** under the project warning set
  (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wvla -Wdouble-promotion`).
- **Corpora are clean**: `simdjson-data/jsonchecker` (1886 files) and
  `JSONTestSuite/test_parsing` (318 files) both report
  `unexpected_pass=0 unexpected_fail=0 broken_dom=0`.
- The tokenizer is a **fused AVX2 stage-1 + switch-grammar** design; the DOM is
  a compact 16-byte value tree in the arena; strings stay zero-copy until
  decoded; numbers keep their lexeme and convert on demand.
- Throughput vs simdjson **DOM** on the same machine (`taskset -c 2`, same
  process, best of 200 iterations; see §Performance):

| fixture | size | jsoc parse | simdjson DOM | jsoc/simdjson |
|---|---:|---:|---:|---:|
| twitter | 0.63 MB | 0.245 ms / 2.58 GB/s | 0.174 ms / 3.64 GB/s | 1.41x |
| citm_catalog | 1.73 MB | 0.642 ms / 2.69 GB/s | 0.456 ms / 3.79 GB/s | 1.41x |
| canada | 2.25 MB | 1.813 ms / 1.24 GB/s | 1.788 ms / 1.26 GB/s | 1.01x |
| gsoc-2018 | 3.33 MB | 0.665 ms / 5.00 GB/s | 0.720 ms / 4.62 GB/s | 0.92x |

jsoc is at parity or ahead on gsoc/canada and ~40 % behind on twitter/citm.
The gap is in the tokenizer, not the DOM: the DOM path stores a 12-byte token
per value/container/quote (separators are validated but dropped), while
simdjson builds its DOM straight from the stage 1/2 masks.

## Build / test / bench / corpus

```bash
cd /home/moso/jsoc

make debug              && ./build/test_tokenizer && ./build/test_dom
make release            && ./build/test_tokenizer && ./build/test_dom
make release-native     && ./build/test_tokenizer && ./build/test_dom
make PORTABLE=1 release && ./build/test_tokenizer && ./build/test_dom
make asan               && ./build/test_tokenizer && ./build/test_dom

./build/corpus /tmp/opencode/simdjson-data/jsonchecker \
              /tmp/opencode/JSONTestSuite/test_parsing

BENCH_ITERS=200 BENCH_MODE=tokenize,parse,parse_many,utf8 taskset -c 2 ./build/bench <files...>
```

The benchmark treats `.ndjson` / `.jsonl` as streaming input and routes them to
`parse_many`; every other file is a single-document parse.

### simdjson comparison

```bash
SIMDJSON_DIR=/tmp/opencode/simdjson BENCH_ITERS=200 bench/compare.sh <files...>
```

`bench/compare.sh` builds `bench/compare_simdjson.cpp`, which links both
libraries into one process and uses the same timing protocol as `bench.c`
(three warmup parses, N timed iterations, best-of). It is deliberately not part
of `make all`: it needs a C++ compiler and simdjson.

## Layout

```
include/
  jsoc.h        tokenize/tokenize_file + decode error enum
  dom.h         DOM, parse, parse_many, UTF-8 validation
  path.h        JSON Pointer (RFC 6901)
  writer.h      arena-backed JSON text writer
  serialize.h   minify
  cursor.h      forward-only cursor over a document
  decode.h      reflected decoding into structs
  token.h       jsoc_token (12 bytes) + helpers
  arena.h       arena handle
  error.h       jsoc_error / jsoc_error_code
  export.h      visibility + attribute macros
src/
  arena.c       chunked bump allocator
  error.c       error strings + lazy line/column formatting
  token.c       token names + printing
  tokenizer.c   fused SIMD tokenizer + portable reference state machine
  dom.c         DOM builder, string decode/cache, parse APIs, accessors
  path.c        pointer lookup + pointer builder
  writer.c      text writer (escaping, numbers, framing)
  serialize.c   minify (DOM walk through the writer)
  cursor.c      cursor navigation
  decode.c      reflected decoder
  simd.h        block classification, digit runs, escape chaining, prefix-xor
  utf8.h        AVX2 UTF-8 validator + scalar oracle
  number.h      number parsing/range helpers shared by both backends
  dom_internal.h private 16-byte jsoc_value / jsoc_member layout
  tokenizer_internal.h hidden backend entry points (used by the tests)
test/
  test.h             shared harness (TEST/REQUIRE, file readers, xorshift64)
  test_tokenizer.c   29 tests incl. the differential fuzzer, which compares
                     the SIMD and portable backends in all three modes
                     (strict, checked, stream)
  test_dom.c          21 tests: DOM, pointer, writer, minify, cursor, decode
  test_numbers.c     15 tests (see TESTING.md for the simdjson mapping)
  test_errors.c       6 tests
  test_stream.c       8 tests
  test_pointer.c      6 tests (RFC 6901 vectors)
  test_minify.c       3 tests (reference escaper + corpus round-trip)
  test_padding.c      8 tests (exact-size buffers)
  test_unicode.c      3 tests (random UTF-8 generator)
  test_static.c       4 tests (sizes, offsets, trivial copyability)
  corpus.c           acceptance-corpus runner with DOM integrity checks
fuzz/
  fuzz.c             9 standalone fuzz targets; libFuzzer-compatible
scripts/
  fetch-testdata.sh  clones jsonchecker / jsonexamples / JSONTestSuite
bench/
  bench.c            native throughput benchmark
  compare.sh         build+run the simdjson comparison
  compare_simdjson.cpp  the comparison harness (not built by default)
.github/workflows/
  ci.yml             build matrix + fuzz smoke + corpora + sanitizers
TESTING.md           the simdjson port mapping, and what is not ported
legacy/              the old implementation; reference only, never built
```

Running:

```bash
make test            # every suite
make corpus          # acceptance corpora (make fetch-testdata first)
make fuzz            # bounded deterministic fuzz run
make check           # all of the above under ASan+UBSan
make fetch-testdata  # one-time corpus download
make install         # /usr/local + a pkg-config file
```

`build/.cflags` stamps the effective command line, so switching between
debug/asan/release never mixes objects from different flag sets.

## Data model / invariants

### Tokens

```c
typedef struct {
    uint32_t offset;   // byte offset into the caller's input
    uint32_t length;   // raw byte length (escapes are not decoded)
    jsoc_token_type type; // enum with a uint8_t underlying type
} jsoc_token;          // 12 bytes
```

- **Zero-copy.** A string token spans the bytes *between* the quotes; escapes
  and `\uXXXX` are validated but not decoded.
- Tokens come from the `jsoc_arena`.  `jsoc_tokenize_file` reads the file into
  the arena itself, so tokens stay valid until the arena is reset or
  destroyed. **Never free a buffer that tokens point into.**
- The token layout is public. Do not change it while optimizing.

### DOM

- `jsoc_value` is 16 bytes: scalars use `offset`/`length`, containers use
  `first`/`count` (siblings are linked through `next`).
- Object members live in a parallel 16-byte `jsoc_member` array; keys store the
  raw offset/length and are decoded lazily and cached per value/key slot.
- `jsoc_parse` uses `limit = token_count - 1`, so lengths are never truncated
  by the token stream, and detects trailing documents from the builder's end
  position.
- Decoded strings are allocated from the document arena and cached in
  `doc->string_cache`; they stay valid until the arena is reset.

### Errors

- The tokenizer's partial token stream, error code, and offset always match
  the portable reference exactly: whenever the SIMD path sees anything it
  cannot classify with full confidence (bad escape, unmatched grammar, bad
  structural close) it hands the input over to `tokenize_scalar`, which is the
  oracle.
- Number *range* failures are recorded out of band as `*overflow_token`, so a
  caller exposing only the first document ignores out-of-range numbers in
  trailing values.
- Line/column stay **lazy** (`make_error_at` rescans up to the error offset).

### Arena

Chunked bump allocator; the handle stays stable and describes the current
chunk. On overflow a larger chunk is allocated and the old one is retired via
`next`; bytes are never copied. `jsoc_arena_reset` frees retired chunks and
rewinds. Nothing in the token/DOM APIs calls `free` on arena-owned memory.

## Tokenizer architecture (`src/tokenizer.c`)

`tokenize_dispatch` sends inputs shorter than one 64-byte block to the portable
state machine and the rest to the fused SIMD path. Both backends are compiled
twice, specialized on `check_numbers`, so the unchecked `jsoc_tokenize` has no
branch on the number-range work.

Per 64-byte block:

1. `jsoc_classify64()` → whitespace, operator (`{}[]:,`), quote, backslash,
   control masks.
2. `jsoc_escape_next()` chains escape state across blocks; masking it out of
   the quote mask leaves real quotes.
3. `jsoc_prefix_xor()` → in-string mask, carried in `f->s1.prev_in_string`.
4. `starts = ((op | (scalar & ~follows)) & ~string_tail) | (quote & ~in_string)`,
   then `&= valid_mask`. A string-free block skips the prefix XOR entirely.
5. Only set bits of `starts` are visited. `jsoc_token_kind[c]` maps structural
   bytes to token types, scalar-start bytes to `JSOC_KIND_SCALAR = 0xFF`, and
   anything else to 0.
6. Escapes/controls are validated from the stage-1 masks inside the block that
   produces them (`fused_check_specials`), so clean string bytes are never
   rescanned; any failure defers to the reference scanner.

The last partial block is copied into a 64-byte padded buffer and processed
with a low `valid` mask; nothing ever reads past the input.

Measured hot-loop choices (do not undo without a fresh benchmark): switch-based
grammar (a table is 0.7–1.3 ms slower), scalar-before-structural dispatch,
comma/colon fusion, `goto again` chaining for scalars, `fused_is_scalar_end`
instead of `jsoc_skip_ws`, unchecked token stores (65 slots reserved per
block), `fused_process_block` kept inline (noinline / unrolling / alignment
flags all measured neutral or worse), compile-time specialization on
`check_numbers`, inlined `validate_number_*`.

### Deferred grammar: measured, rejected

A variant that emitted tokens without any grammar check and validated the
result in a separate linear pass was implemented and benchmarked
(`fused_process_start` + `fused_validate_grammar`, snapshot in
`/tmp/opencode/tok_before_defergram.bak` for the pre-refactor tree). It is
**0.18–0.34 ms slower** per corpus file and has been removed: the emission loop
was never blocked by the grammar state, so the extra pass is pure overhead.

## Modules added on top of the DOM

### `path.h` — JSON Pointer

`jsoc_pointer_get(_cstr)` walks a RFC 6901 pointer. Tokens are unescaped
(`~1`→`/`, `~0`→`~`) and compared against the key's *logical* spelling: the
comparison decodes `\uXXXX` and surrogate pairs on the fly, so
`"/é"` finds a member whose raw slice is `"\u00e9"`. Array tokens must be
canonical decimals (`"01"`, `"-"` and friends are rejected). `jsoc_pointer_builder`
appends escaped tokens into an arena buffer for the reverse direction.

### `writer.h` — text writer

An arena-backed writer with automatic framing: `array_begin/end`,
`object_begin/end`, `object_key*`, and scalar writers. `writer_separator`
tracks `nonempty`/`after_key` per frame so an object's `key : value` element
gets exactly one comma. `jsoc_writer_value_raw` writes already-valid JSON text
(a number lexeme, an already-quoted string) as a value. Doubles use the
shortest `%g` that reads back bit-identically; non-finite values become
`null`. Escaping handles `"` `\` and every C0 byte; everything else passes
through, so the writer does not validate UTF-8.

### `serialize.h` — minify

`jsoc_minify` parses, walks the DOM, and writes raw slices verbatim: the
tokenizer already validated every escape, so re-escaping is unnecessary (and
would change the bytes). Preallocates `doc->length + 16` from the arena, so
growth is the exception.

### `cursor.h` — cursor

Depth-first, forward-only navigation (`down` / `next` / `up` / `enter`) with
one stack entry per level (values, child index, and key). `down` on an object
enters its first member; on an array, its first element. Keys of array elements
are `{NULL, 0}`. The struct is ~28 KB at `JSOC_CURSOR_MAX_DEPTH = 1024`; keep
it in automatic storage of a non-recursive function.

### `decode.h` — reflected decoding

Tables of `jsoc_decode_field` (terminated by `{ .name = NULL }`) map JSON
members onto struct fields via `offsetof`. Supported kinds: scalar getters
(which reuse the DOM getters, so ranges and integer/literal rules apply),
`JSOC_DECODE_STRING_REF` (decoded copy), `JSOC_DECODE_STRING_SLICE` (raw
slice), `JSOC_DECODE_STRUCT`, and `JSOC_DECODE_ARRAY` (destination pointer +
capacity/read-back count + element stride). Errors report the field name.
Duplicate JSON members keep the first match, matching the DOM's lookups.

## Correctness / differential testing

`test_tokenizer` test 29 (`differential_fuzz`) drives both backends through
`tokenizer_internal.h` and requires exact agreement on error code and offset,
token count, and every token's type/offset/length. It covers generated
documents, random whitespace, single-byte mutations, raw random bytes, and
long strings with valid/invalid escapes and raw control bytes at every offset
relative to the 64-byte block boundary.

`test_dom` test 15 additionally re-parses generated documents and cross-checks
the DOM against the public accessors. `corpus.c` walks the acceptance corpora
and also verifies that every document it accepts can be traversed and that
`jsoc_object_get` agrees with iteration.

Build the differential test with `-march=native`: without AVX2 the SIMD backend
is a scalar stub and the comparison only checks one path against itself.

## Performance notes

Measured on a Ryzen 5 5600, `taskset -c 2`, best-of-run.

- `jsoc_classify64` alone: ~18.2 GB/s.
- Stage 1 + escape/string/start masks: ~12.3–13.3 GB/s.
- Isolated cost of the pure token store (count-only experiment): 2–9 %, i.e.
  the store is *not* what separates jsoc from simdjson stage 1.
- UTF-8 validation (`jsoc_validate_utf8`, lookup4 over 32-byte chunks):
  37–71 GB/s, 0.017–0.047 ms on the four fixtures.

Rejected experiments (do not redo without a new measurement):

- deferred grammar validation pass: 0.18–0.34 ms slower per fixture;
- grammar transition table: ~0.7–1.3 ms slower than the switch;
- block-loop unrolling (2x/3x/4x) and `noinline` block processing: slower or
  neutral;
- auto-vectorization (`VECTORIZE=1`): slower;
- `loop_width` / function / loop alignment flags: slower or neutral;
- PGO (`-fprofile-generate/-use`): the profile did not apply to the object
  paths and showed no benefit;
- caching one 16-byte digit mask across a number's integer/fraction/exponent
  runs: 8–12 % slower (extra mask setup and spills outweigh the saved loads);
- adding a digit mask to `jsoc_classify64`: 3–16 % slower (stage 1 is latency
  bound and every block pays, while only number-heavy blocks benefit);
- forcing `fused_process_block` inline so compact mode constant-folds: neutral
  and ~30 % larger `.text`, so the out-of-line block stays.

### Where the remaining gap is

The DOM now consumes a **compact token stream**: `jsoc_parse` and
`jsoc_parse_many` use `jsoc_tokenize_compact_checked`, which validates `,` and
`:` through the same grammar but does not store them (37–46 % of tokens on
twitter/citm). The public `jsoc_tokenize` stream is unchanged. Measured effect
(release-native, same-process compare): citm 1.45x → 1.41x, twitter unchanged
at 1.41x, canada 1.01x, gsoc 0.92x. Separator stores were never the main cost
(count-only experiment: 2–9 %), so the remaining twitter/citm gap is the
materialisation of 12-byte tokens and the tokenize/DOM split itself: simdjson
builds its DOM directly from stage 1/2 masks, while jsoc first writes a token
array and then walks it. The next structural win is a mask-driven DOM builder
that skips the token array for the common cases, not more micro-tuning of the
existing emit loop.

## Next work

1. **Mask-driven DOM construction** (the structural change described above) if
   the twitter/citm gap must be closed further. Keep the token-walking builder
   as the reference implementation and the error oracle.
2. **Fuse UTF-8 validation into stage 1.** `jsoc_validate_utf8` is already fast,
   but it currently re-reads every byte of the document.
3. **`jsoc_parse` streaming/sliced mode** for On-Demand-style paging
   (`jsoc_parse_many` already exists and resets the arena per document).
4. Expose `number.h` helpers in the public API if anyone needs
   `jsoc_value_get_double`'s fast path without a document.
5. Keep `HANDOFF.md` and the benchmark numbers in this file honest: re-run the
   comparison harness after any tokenizer change.

## Gotchas / rules of thumb

- **Never free memory that tokens or decoded strings point into.**
  `jsoc_arena_reset` invalidates every token and every allocation.
- **Keep line/column lazy**; eager tracking costs a branch per byte.
- **Do not "optimize" by removing validation.** The tests pin strict JSON
  errors, and the deferred-escape validation must stay exact.
- **Keep the scalar backend.** It is the oracle for fuzzing, the short-input
  path, and every cold error report.
- **Run all build variants** after touching headers: `debug` catches
  intrinsics/attribute problems that `release-native` masks, and `PORTABLE=1`
  exercises the scalar-only path.
- `jsoc_value` is a union: scalars use `offset`/`length`, containers
  `first`/`count`. Never write both.
- The C++ comparison harness includes the public headers; keep them C++-clean
  (`extern "C"` guards, no compound literals, no C-only designated initializers
  in inline functions).
