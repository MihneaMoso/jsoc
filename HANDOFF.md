# jsoc — handoff

Continuation notes for the C23 JSON library.

Last verified: **2026-11-18**, HEAD `151b671` ("fix benchmarks"), clean
`release-native` build. Treat older numbers in this file as historical; re-run
the harness after any change.

## TL;DR state

- **Tests**: all 10 test binaries pass in `debug`, `release`,
  `release-native`, `PORTABLE=1 release`, and `asan` (ASan+UBSan).
- **No compiler warnings** under the project warning set
  (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wvla -Wdouble-promotion`).
- **Corpora**: `~/simdjson-data/jsonchecker` (1886 files),
  `~/simdjson-data/JSONTestSuite/test_parsing` (318 files), and
  `~/simdjson-data/jsonexamples` all report
  `unexpected_pass=0 unexpected_fail=0 broken_dom=0`
  (`files=2235 pass_ok=250 fail_ok=455 impl_defined=1527 skipped=3`).
- **DOM parsing is ahead of simdjson 5.1.1** on the benchmark corpus
  (`taskset -c 2`, same process, best of run):
  - 19 JSON documents: **19/19 wins**, geometric mean **1.27x**.
  - 22 JSON documents (adds the three compact API responses): **22/22 wins**,
    geometric mean **1.25x**.
  - Adding `amazon_cellphones.ndjson` to the same run exposes the separate
    streaming gap: 19 wins / 1 loss, geometric mean 1.215x.
- **Serialization / deserialization is only partially at parity**, and the
  overlapping parts are currently **slower** than simdjson except for
  number-heavy DOM serialization (see §Serialization & deserialization):
  - DOM serialize (pre-parsed, 19 files): **4 wins / 15 losses**, geomean
    **0.52x**.
  - Parse + serialize: **5 wins / 14 losses**, geomean **0.69x**.
  - Typed deserialize (twitter): **0.26x**.
  - Typed serialize (twitter): **0.04x**.
  - NDJSON stream: **0.69x**.
  - Raw (non-validating) minify: jsoc has no equivalent; the validating
    `jsoc_minify` is ~0.09x of simdjson's raw `minify`.
  The implementation plan to close these gaps, with performance targets, is
  §Serialization & deserialization → Implementation plan.

The tokenizer is a fused AVX2 stage-1 + switch-grammar design; the DOM is a
compact 16-byte value tree in the arena; strings stay zero-copy until decoded;
numbers keep their lexeme and convert on demand.

## Build / test / bench / corpus

Everything external is persistent under the home directory; `/tmp` is cleared
on reboot.

| component | path |
|---|---|
| simdjson checkout + static library | `~/simdjson/` (`build/libsimdjson.a`) |
| test corpora | `~/simdjson-data/` (`jsonchecker`, `jsonexamples`, `JSONTestSuite`) |
| benchmark corpus | `~/bench_corpus/` (real-world JSON + NDJSON) |

```bash
cd /home/moso/jsoc

# Build one variant, then run the phony runners.
make clean && make release-native
make test                 # 10 binaries; runs, does not rebuild
make corpus               # 2235 files; runs, does not rebuild
make bench                # runs ~/bench_corpus/{twitter,citm_catalog}.json
make compare-serialization
make fuzz                 # bounded deterministic fuzz run
make check                # ASan+UBSan variant + tests

# Other variants (always clean between them):
make clean && make debug
make clean && make release
make clean && make PORTABLE=1 release
make clean && make asan
```

### Important: `.cflags` and the dirty-rebuild trap

`build/.cflags` is written **only by the variant targets** (`debug`, `release`,
`release-native`, `asan`, ...). Consumers (`test`, `bench`, `corpus`, `fuzz`)
are `.PHONY` runners and never change the stamp. This fixed a long-standing
trap where `make release-native && make test` rebuilt the library without
`-O3 -march=native`, silently producing a ~0.3x benchmark. If a benchmark
suddenly looks 3–4x slower, check `cat build/.cflags` first, then
`make clean`.

### simdjson comparison

```bash
# DOM parse comparison
SIMDJSON_DIR=~/simdjson BENCH_ITERS=200 bench/compare.sh <files...>

# Serialization / deserialization comparison
bench/compare_serialization.sh <files...>
BENCH_MODE=dom_serialize,roundtrip,raw_minify,stream,typed_de,typed_ser \
  bench/compare_serialization.sh ~/bench_corpus/twitter.json
```

`bench/compare_simdjson.cpp` and `bench/compare_serialization.cpp` link both
libraries into one process and use the same protocol: warmups, contiguous timed
phases, alternating phase order per file, best-of-run. They are deliberately
not part of `make all`.

The serialization harness's typed modes use **C++26 static reflection** (GCC 16
+ `-freflection`) to drive simdjson's `doc.get<T>()` and
`simdjson::to_json_string(T)`. The build script detects support and disables
those two modes if the compiler lacks it.

## Serialization & deserialization

### What jsoc has today

| area | public API | notes |
|---|---|---|
| JSON text writer | `include/writer.h` | arena or fixed buffer; automatic comma/colon framing; escaping; scalar writers |
| DOM → JSON | `jsoc_minify`, `jsoc_document_minify`, `jsoc_value_minify` (`include/serialize.h`) | validates first when given input; writes raw slices verbatim |
| DOM parse | `jsoc_parse`, `jsoc_parse_file` (`include/dom.h`) | full validation, zero-copy strings, lexeme-preserving numbers |
| Streaming | `jsoc_parse_many`, `jsoc_parse_many_file` | NDJSON / concatenated documents; callback per top-level value |
| Typed decode | `include/decode.h` | descriptor tables → structs, nested structs, fixed-capacity arrays |
| Cursor | `include/cursor.h` | forward-only walk over an already-materialized DOM |
| Pointer | `include/path.h` | RFC 6901 lookup and builder |
| UTF-8 | `jsoc_validate_utf8` | full validation |

### Parity matrix vs simdjson 5.1.1

| capability | simdjson 5.1.1 | jsoc today | status |
|---|---|---|---|
| DOM parse | `dom::parser::parse` | `jsoc_parse` | **parity**, faster |
| NDJSON / concatenated docs | `parse_many` / `iterate_many` (batched) | `jsoc_parse_many` | **API parity**, 0.69x |
| On-Demand lazy front-end | `ondemand::parser`, `document`/`value`/`object`/`array` | none (cursor needs a full DOM) | **missing** |
| Typed/reflected deserialize | `doc.get<T>()`, `simdjson::deserialize`, STL containers, maps, optional, smart pointers | `decode.h` tables (structs, nested structs, fixed arrays) | **partial**; no dynamic containers, no lazy path |
| DOM → JSON | `to_string`, `minify`, `prettify`, `operator<<` | `jsoc_document_minify` | **minify only**; 0.52x pre-parsed |
| Pretty print | `prettify(element, indent_step)` | none | **missing** |
| FracturedJSON | `fractured_json`, `fractured_json_string`, `to_fractured_json_string` | none | **missing** |
| Raw minify (no validation) | `simdjson::minify(buf, len, dst, dst_len)` | none; `jsoc_minify` validates | **missing**, different contract |
| Low-level builder | `builder::string_builder` (`append`, `append_key_value`, `escape_and_append*`, `append_raw`, `view`, `move_to`, `validate_unicode`) | `writer.h` with automatic framing | **API parity** (different style) |
| Typed/reflected serialize | `to_json_string(T)`, `builder::append`, `to_json`, annotations/enums/containers/optional/size bound | none; caller writes every value | **missing** |
| Number formatting | Dragonbox + Jeaiii, shortest round-trip | `snprintf` + `strtod` verification loop for doubles, `snprintf` for integers | **performance gap** |
| JSON Pointer | `at_pointer` | `jsoc_pointer_get` | **parity** |
| JSONPath | `at_path` (subset) | none | **missing** (navigation, not core serde) |
| Compile-time JSON | C++26 `compile_time_json` | N/A in C23 | **N/A** |
| C++ stream operators | `operator<<` | C API only | **N/A** (optional C++ wrapper) |
| UTF-8 validation | full | full | **parity** |

Deliberate behavioural differences that remain valid:

1. **Numbers keep their original lexeme** in the DOM. Re-serialization writes
   the validated bytes, so `1.0` stays `1.0` and `1e2` stays `1e2`. This is why
   jsoc already wins number-heavy DOM serialization (canada 2.2x, numbers
   2.3x). Do not give this up to match simdjson's decoded-number output.
2. **Escaped strings are written verbatim** by `jsoc_document_minify`
   (`"\u0041"` stays `"\u0041"`); simdjson decodes and re-escapes
   (`"\u0041"` becomes `"A"`). Both are valid JSON. On
   `twitterescaped.json`, jsoc's output is 562,408 B vs simdjson's 466,906 B.
3. **NaN / Infinity are rejected**, as before.

### Benchmark harness

`bench/compare_serialization.cpp` (built by
`bench/compare_serialization.sh`) has six modes:

| mode | jsoc | simdjson |
|---|---|---|
| `dom_serialize` | parse once, repeatedly `jsoc_document_minify` | parse once, repeatedly `to_string(doc)` |
| `roundtrip` | `jsoc_minify` (parse+walk) | `parse` + `to_string` |
| `raw_minify` | `jsoc_minify` (validating) | `simdjson::minify` (raw, no validation) |
| `typed_de` | `jsoc_parse` + `jsoc_decode_object` | `ondemand::parser::iterate` + `doc.get<T>()` |
| `typed_ser` | hand-written `jsoc_writer` calls | `simdjson::to_json_string(T)` (reflection) |
| `stream` | `jsoc_parse_many` | `dom::parser::parse_many` |

The typed model follows simdjson's `benchmark/static_reflect/twitter_benchmark`
shape: `TwitterData { vector<Status> statuses }`,
`Status { created_at, id, text, User user, retweet_count, favorite_count }`,
`User { id, name, screen_name, location, description, verified,
followers_count, friends_count, statuses_count }`. jsoc uses the analogous
fixed-capacity structs and `decode.h` tables.

Protocol: `taskset -c 2`, same process, two settle parses per phase switch,
contiguous phases, alternating phase order per file, best-of-run. Rates use
input bytes for parse/serialize-the-document and output bytes for typed
serialization (both libraries produced exactly 81,927 B there).

### Measured results (release-native, Ryzen 5 5600)

**DOM serialize, pre-parsed, 19 real documents**

```
file                          jsoc GB/s  simdjson GB/s  speedup
apache_builds                     0.93        2.01      0.463x
canada                            1.00        0.45      2.203x
citm_catalog                      1.45        4.52      0.321x
github_events                     1.25        3.57      0.350x
google_maps_api_response          0.84        2.64      0.317x
gsoc-2018                         3.64        6.26      0.581x
instruments                       0.88        2.93      0.299x
marine_ik                         0.65        0.67      0.962x
mesh                              0.74        0.62      1.203x
mesh.pretty                       1.61        1.35      1.193x
numbers                           1.22        0.52      2.329x
random                            0.55        1.70      0.324x
repeat                            1.18        3.45      0.342x
semanticscholar-corpus            1.08        2.60      0.413x
tree-pretty                       0.86        2.93      0.294x
twitterescaped                    0.96        2.96      0.325x
twitter                           1.09        3.33      0.326x
twitter_timeline                  0.73        2.30      0.317x
update-center                     0.74        2.02      0.366x
aggregate                         1.07        1.39
geomean speedup 0.515x; jsoc wins 4, simdjson wins 15
```

**Parse + serialize, 19 files**: geomean **0.692x**, 5 wins / 14 losses,
aggregate 0.78 vs 0.86 GB/s. The same wins (canada 1.71x, numbers 2.24x, mesh
1.32x, mesh.pretty 1.30x, marine_ik 1.06x).

**Raw minify**: `jsoc_minify` (validate + DOM + serialize) vs
`simdjson::minify` (no validation, no DOM): geomean **0.094x**, range 0.062x
(canada) to 0.209x (gsoc). This compares different contracts; the missing
feature is a raw minifier, not a slower implementation of `minify`.

**Typed deserialize (twitter.json)**: jsoc 1.24 GB/s vs simdjson 4.78 GB/s =
**0.260x**. `jsoc_parse` alone on the same document is ~3.87 GB/s, so the DOM
build plus descriptor decode together cost ~3x more than on-demand's fused
parse-into-struct path.

**Typed serialize (twitter.json)**: jsoc 0.44 GB/s vs simdjson 11.7 GB/s =
**0.039x**; both emit 81,927 B. This is the largest single gap.

**NDJSON stream**:

```
file                          jsoc GB/s  simdjson GB/s  speedup
amazon_cellphones (793)           2.59        4.11      0.631x
amazon_cellphones x32 (25376)     2.40        3.44      0.698x
twitter x16 (16)                  1.85        2.44      0.756x
geomean 0.693x
```

Also measured: `jsoc_parse_many` on a **single** twitter document is 2.19 GB/s,
while `jsoc_parse` on the same bytes is 3.87 GB/s. Streaming is leaving ~40% on
the table even for one document.

### Diagnosis

1. **DOM serializer overhead.** `jsoc_document_minify` performs several
   out-of-line writer calls per value (`writer_separator`, `jsoc_writer_raw`,
   `writer_char`) with a capacity check and `memcpy` each time. A typical
   string costs 3 calls, a key 4, so 14k values take ~0.6 ms for 467 KB.
   simdjson's formatter writes through a local buffer pointer with inlined
   stores. The fix is a direct-pointer writer, not a different algorithm.
2. **Double allocation.** `jsoc_value_minify` allocates the writer buffer
   (`doc->length + 16`), then allocates a second buffer and `memcpy`s the
   result. Return the writer buffer directly.
3. **Scalar number formatting.** Integers go through `snprintf`; doubles use
   `snprintf` in a precision loop with `strtod` verification. simdjson uses
   Jeaiii + Dragonbox. This dominates typed serialization and any output with
   many numbers.
4. **Per-byte string escaping.** `jsoc_writer_string` scans one byte at a
   time. simdjson escapes 32 bytes at a time.
5. **Typed decode materializes the DOM first.** `jsoc_parse` + descriptor
   decode is ~3x the cost of on-demand's fused parse-into-struct. No amount of
   descriptor tuning closes that; an on-demand front-end is required.
6. **`jsoc_parse_many` tokenizes the whole stream up front** into a separate
   token arena, then builds documents. It therefore pays an extra full pass,
   O(stream) token memory, and a slower stream tokenizer mode even for one
   document. simdjson batches documents and builds each as it goes.

### Implementation plan

Priorities are ordered by (impact on the parity matrix) / effort. Each phase
lands with its own tests and benchmark gate; do not land a phase that regresses
the existing DOM-parse wins.

#### P0 — Streaming fast path (`jsoc_parse_many`)

**Goal:** streaming deserialization at or above simdjson, without changing the
public API or error semantics.

- Replace the whole-stream token array with a batched pipeline:
  1. Scan a chunk (start with 1–4 MB) with the existing stage-1 masks and a
     depth/string state; record top-level value spans. Newlines at depth 0 are
     a fast boundary for NDJSON but are not required (concatenated documents
     must still work).
  2. Parse each span with the same fast `jsoc_parse`/DOM-builder path used for
     single documents.
  3. Keep tokens/DOM for the current batch in an arena region that is rewound
     between batches. Add `jsoc_arena_mark` / `jsoc_arena_release` (or use a
     saved offset) so per-document lifetime is preserved without freeing
     chunks.
- Keep the documented behaviour: deliver every complete document before an
  error, never a truncated one, exact error code/offset, line/column lazy.
- Add an internal "single document fast path": if the whole input is one value,
  delegate to the same builder as `jsoc_parse` (no extra token arena).
- Tests: `test_stream` (port the remaining simdjson
  `document_stream_tests.cpp` cases), `fuzz_ndjson`, corpus, guard pages.
- Gate: amazon ≥1.25x, large NDJSON ≥1.2x, single-document `parse_many`
  ≥0.95x of `jsoc_parse`; memory O(batch), not O(stream).

#### P1 — Raw minify (`jsoc_minify_raw`)

**Goal:** feature + performance parity with `simdjson::minify`.

- API (`include/minify.h`):
  ```c
  JSOC_API bool jsoc_minify_raw(const char *input, size_t length,
                                char *out, size_t out_capacity,
                                size_t *out_length);
  JSOC_API bool jsoc_minify_raw_arena(const char *input, size_t length,
                                      jsoc_arena *arena, jsoc_string *out);
  ```
- Contract mirrors simdjson: **does not validate JSON**; tracks quotes and
  backslashes only, returns an unterminated-string error for an unclosed
  string. Keep the validating `jsoc_minify` unchanged.
- Implementation: 32-byte AVX2 loop with three masks (`"`/`\`, whitespace,
  control not needed), prefix-xor for in-string, copy non-whitespace runs
  outside strings and verbatim runs inside strings; handle escaped quotes;
  16/8-byte tail. No token array, no DOM, no UTF-8 pass.
- Tests: port `tests/minify_tests.cpp`; `fuzz_minify`; assert byte-identical
  output with `simdjson::minify` on the corpus and with whitespace-stripped
  `jsoc_minify` on valid documents.
- Gate: ≥1.1x simdjson raw minify on every corpus file; zero overreads.

#### P2 — Fast serializer core

**Goal:** remove the writer overhead and the scalar-formatting gap.

- `jsoc_writer` gets a local cursor contract:
  - `jsoc_writer_init_bounded(w, buffer, capacity)` (caller-owned) and an
    internal unchecked mode used only after a bound check.
  - Inline `put_char`, `put_mem`, `put_quoted_raw`, `put_escaped`.
  - Frame state becomes a small `needs_comma` bit checked in the same branch
    as the store; no per-fragment function calls.
- **Size bound:** add `jsoc_value_size_bound(doc, v)` (string `6*len+2`,
  numbers `len`, literals, quotes/commas/braces) and allocate the output once.
  `jsoc_value_minify` writes directly into that buffer; remove the second
  allocation and `memcpy`.
- **SIMD escaping:** 32-byte AVX2 chunks for `"`/`\`/C0; copy clean runs with
  wide unaligned stores; 16/8-byte tail; scalar fallback for `PORTABLE=1`.
  Keep the exact current escaping rules (C0 as `\b\f\n\r\t` or `\u00xx`,
  UTF-8 passthrough).
- **Fast integers:** Jeaiii-style 8-byte stores or a 2-digit LUT + reciprocal
  division; branchless sign. No `snprintf` in the hot path.
- **Fast doubles:** port Ryu, Schubfach, or Dragonbox to C and emit the
  shortest round-trip form; no `strtod` verification loop. Keep a `snprintf`
  fallback only for unusual rounding modes/platforms, behind a compile-time
  switch. This is required for typed serialize and for number-heavy output.
- Tests: existing writer/minify suites, bit-exact double round-trip over
  random bit patterns and the `test_numbers` corpus, locale independence,
  `fuzz_minify`.
- Gate: DOM serialize geomean ≥1.3x; string-heavy docs ≥1.2x; canada/numbers
  stay ≥2x; typed serialize with decoded strings ≥1.2x.

#### P3 — Descriptor-driven serialization (`include/encode.h`)

**Goal:** the C analogue of simdjson's reflected `to_json_string(T)`.

```c
typedef enum {
  JSOC_ENCODE_BOOL, JSOC_ENCODE_INT64, JSOC_ENCODE_UINT64,
  JSOC_ENCODE_DOUBLE, JSOC_ENCODE_STRING, JSOC_ENCODE_STRING_RAW,
  JSOC_ENCODE_STRING_CSTR, JSOC_ENCODE_STRUCT, JSOC_ENCODE_ARRAY,
  JSOC_ENCODE_RAW, JSOC_ENCODE_NULL
} jsoc_encode_kind;

typedef struct jsoc_encode_field {
  const char *name;
  jsoc_encode_kind kind;
  size_t offset;
  const struct jsoc_encode_field *nested; /* STRUCT */
  size_t count_offset;                    /* ARRAY: count */
  size_t item_size;                       /* ARRAY: stride */
  const struct jsoc_encode_field *items;  /* ARRAY of STRUCT */
  size_t max_items;                       /* ARRAY: safety bound */
} jsoc_encode_field;

JSOC_API bool jsoc_encode_object(const jsoc_encode_field *fields,
                                 size_t field_count, const void *object,
                                 jsoc_arena *arena, jsoc_string *out);
```

- `JSOC_ENCODE_FIELD_A(obj, member, kind, ...)` mirrors
  `JSOC_DECODE_FIELD_A`, so a struct can have one decode table and one encode
  table with the same member names.
- Kinds:
  - `JSOC_ENCODE_STRING_RAW` writes `"` + slice + `"` verbatim. Use it with
    `JSOC_DECODE_STRING_SLICE` for a **zero-copy round trip**: the tokenizer
    already validated the escapes, so no re-scan is needed and serialization
    becomes a `memcpy`. This is the intended way to beat simdjson on
    document round-trips.
  - `JSOC_ENCODE_STRING` escapes decoded bytes (SIMD path from P2).
  - `JSOC_ENCODE_RAW` writes an already-valid JSON fragment (number lexeme,
    pre-quoted string).
- Two-pass or bound-first design: `jsoc_encode_size_bound(...)` returns an
  upper bound (strings 6n+2, ints 24, doubles 32, punctuation), the caller
  allocates once, and the encoder uses the unchecked writer. Provide a checked
  fallback for hostile counts.
- Tests: `encode(decode(x)) == x` for random structs; nested structs/arrays;
  null/optional; unicode and escapes; `test_dom` writer tests; new
  `test_encode`; `fuzz_encode`.
- Gate: typed serialize ≥1.2x simdjson on twitter-sized structs with decoded
  strings; ≥2x with `STRING_RAW` slices; no allocation per field.

#### P4 — On-Demand lazy deserialization (`include/ondemand.h`)

**Goal:** the structural parity feature, and the only way to reach simdjson's
typed-deserialize speed.

API sketch:

```c
typedef struct jsoc_ondemand jsoc_ondemand;
typedef struct { const jsoc_ondemand *doc; uint32_t index; } jsoc_om_value;
typedef struct { uint32_t first; } jsoc_om_object;
typedef struct { uint32_t first; } jsoc_om_array;

JSOC_API jsoc_ondemand *jsoc_ondemand_create(jsoc_arena *arena,
                                             const char *input, size_t length);
JSOC_API jsoc_error jsoc_ondemand_root(jsoc_ondemand *d, jsoc_om_value *out);
JSOC_API bool jsoc_ondemand_get_object(jsoc_ondemand *d, jsoc_om_value v,
                                       jsoc_om_object *out);
JSOC_API bool jsoc_ondemand_object_field(jsoc_ondemand *d, jsoc_om_object o,
                                         const char *key, size_t key_len,
                                         jsoc_om_value *out);
JSOC_API bool jsoc_ondemand_get_array(jsoc_ondemand *d, jsoc_om_value v,
                                      jsoc_om_array *out);
JSOC_API bool jsoc_ondemand_array_at(jsoc_ondemand *d, jsoc_om_array a,
                                     size_t index, jsoc_om_value *out);
JSOC_API bool jsoc_ondemand_skip(jsoc_ondemand *d, jsoc_om_value v);
JSOC_API bool jsoc_ondemand_get_int64/uint64/double/bool/string(...);
JSOC_API jsoc_error jsoc_ondemand_validate_all(jsoc_ondemand *d);
```

- Implementation:
  1. Skip/validate a value with an explicit stack. Strings use 32-byte quote
     masks (reuse `simd.h`), escapes use the same state machine as the
     tokenizer, numbers use `number.h`, literals use 8-byte compares.
  2. Objects/arrays expose first/next positions; keys are raw slices with an
     optional decoded cache in the arena.
  3. Typed getters use the same range/integer rules as the DOM.
  4. `jsoc_ondemand_validate_all` walks the unread remainder so callers can
     opt into full-document validation with `jsoc_parse` semantics.
- Error semantics: exact code and offset; lazy line/column; no overread past
  `length`. Keep the DOM parser as the oracle and fuzz the two against each
  other.
- Tests: port the applicable `tests/ondemand/*` cases (wrong type, errors,
  numbers, strings, object/array iteration, document stream); differential
  on-demand vs DOM; guard pages; ASan small inputs.
- Gate: typed deserialize ≥1.2x simdjson ondemand on twitter; DOM parse
  unchanged; no per-value heap allocation.

#### P5 — Pretty print and FracturedJSON

- Refactor the writer/DOM serializer over a formatter (mini vs pretty) and add
  ```c
  JSOC_API bool jsoc_prettify(const char *input, size_t length,
                              jsoc_arena *arena, int indent_step,
                              jsoc_string *out, jsoc_error *err);
  JSOC_API bool jsoc_document_prettify(const jsoc_document *doc,
                                       jsoc_arena *arena, int indent_step,
                                       jsoc_string *out);
  ```
- Port `tests/dom/prettify_tests.cpp`.
- FracturedJSON is a separate layout engine (inline / compact multiline /
  table / expanded) behind `jsoc_fractured_options`; port
  `tests/fractured_json_tests.cpp` and `dom/fractured_json.h`. Lower priority
  than P0–P4 because it is formatting, not data interchange.
- Gate: prettify ≥1.0x simdjson (its pretty formatter is scalar); FracturedJSON
  byte-compatible with the reference for the ported tests.

#### P6 — JSONPath and C++ ergonomics (optional)

- `jsoc_pointer_get_path` / a documented JSONPath subset matching simdjson's
  `at_path`; port `tests/dom/json_path_tests.cpp` and the on-demand pointer
  tests.
- A header-only C++ wrapper (`include/jsoc.hpp`) providing `operator<<`,
  `to_json_string` overloads returning `std::string_view`, and `std::string`
  conveniences. This does not affect the C ABI and is not required for parity
  of the C library.

### Definition of done (feature parity)

- Every row in the parity matrix is either implemented or explicitly marked
  N/A with a reason.
- Ported tests from `tests/builder/`, `tests/dom/*serialization*`,
  `tests/dom/prettify_tests.cpp`, `tests/fractured_json_tests.cpp`, and the
  applicable `tests/ondemand/*` suites pass.
- All five build variants pass, including `PORTABLE=1` and ASan.
- The 2235-file corpus stays clean, and every serialized output re-parses.
- The serialization harness reports:

| benchmark | today | target |
|---|---:|---:|
| DOM parse (19 files) | 1.27x | keep ≥1.2x |
| DOM serialize, pre-parsed | 0.52x | ≥1.3x |
| Parse + serialize | 0.69x | ≥1.2x |
| Raw minify | n/a (0.09x validating) | ≥1.1x vs simdjson raw |
| Typed deserialize (twitter) | 0.26x | ≥1.2x |
| Typed serialize (twitter) | 0.04x | ≥1.2x decoded, ≥2x raw |
| NDJSON stream | 0.69x | ≥1.2x |

## Layout

```
include/
  jsoc.h        tokenize/tokenize_file + decode error enum
  dom.h         DOM, parse, parse_many, UTF-8 validation
  path.h        JSON Pointer (RFC 6901)
  writer.h      arena/fixed-backed JSON text writer
  serialize.h   minify
  cursor.h      forward-only cursor over a document
  decode.h      descriptor-table decoding into structs
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
  decode.c      descriptor-table decoder
  simd.h        block classification, digit runs, escape chaining, prefix-xor
  utf8.h        AVX2 UTF-8 validator + scalar oracle
  number.h      number parsing/range helpers shared by both backends
  dom_internal.h private 16-byte jsoc_value / jsoc_member layout
  tokenizer_internal.h hidden backend entry points (used by the tests)
test/           one binary per topic (tokenizer, dom, numbers, errors, stream,
                pointer, minify, padding, unicode, static) + corpus runner
fuzz/           standalone fuzz targets
bench/
  bench.c                    native throughput benchmark
  compare.sh                 build+run the DOM comparison
  compare_simdjson.cpp       DOM comparison harness
  compare_serialization.sh   build+run the serde comparison
  compare_serialization.cpp  serde comparison harness
scripts/        fetch-testdata.sh
.github/        CI matrix
```

Planned additions from the plan: `include/minify.h`, `include/encode.h`,
`include/ondemand.h`, `include/jsoc.hpp`, and the matching `src/` files.

## Data model / invariants

### Tokens

```c
typedef struct {
    uint32_t offset;      // byte offset into the caller's input
    uint32_t length;      // raw byte length (escapes are not decoded)
    jsoc_token_type type; // enum with a uint8_t underlying type
} jsoc_token;             // 12 bytes
```

- **Zero-copy.** A string token spans the bytes *between* the quotes; escapes
  and `\uXXXX` are validated but not decoded.
- Tokens come from the `jsoc_arena`. `jsoc_tokenize_file` reads the file into
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
(`fused_process_start` + `fused_validate_grammar`). It is **0.18–0.34 ms slower**
per corpus file and has been removed: the emission loop was never blocked by
the grammar state, so the extra pass is pure overhead.

## Modules on top of the DOM

### `path.h` — JSON Pointer

`jsoc_pointer_get(_cstr)` walks an RFC 6901 pointer. Tokens are unescaped
(`~1`→`/`, `~0`→`~`) and compared against the key's *logical* spelling: the
comparison decodes `\uXXXX` and surrogate pairs on the fly, so
`"/é"` finds a member whose raw slice is `"\u00e9"`. Array tokens must be
canonical decimals (`"01"`, `"-"` and friends are rejected).
`jsoc_pointer_builder` appends escaped tokens into an arena buffer for the
reverse direction.

### `writer.h` — text writer

An arena- or fixed-buffer-backed writer with automatic framing:
`array_begin/end`, `object_begin/end`, `object_key*`, and scalar writers.
`writer_separator` tracks `nonempty`/`after_key` per frame so an object's
`key : value` element gets exactly one comma. `jsoc_writer_value_raw` writes
already-valid JSON text (a number lexeme, an already-quoted string) as a
value. Doubles currently use the shortest `%g` that reads back bit-identically;
non-finite values become `null`. Escaping handles `"` `\` and every C0 byte;
everything else passes through, so the writer does not validate UTF-8.
**This module is the main serialization bottleneck; see P2.**

### `serialize.h` — minify

`jsoc_minify` parses, walks the DOM, and writes raw slices verbatim: the
tokenizer already validated every escape, so re-escaping is unnecessary (and
would change the bytes). Preallocates `doc->length + 16` from the arena, then
currently copies once more into the final arena string. P2 removes that copy
and adds a raw, non-validating minifier.

### `cursor.h` — cursor

Depth-first, forward-only navigation (`down` / `next` / `up` / `enter`) with
one stack entry per level (values, child index, and key). `down` on an object
enters its first member; on an array, its first element. Keys of array elements
are `{NULL, 0}`. The struct is ~28 KB at `JSOC_CURSOR_MAX_DEPTH = 1024`; keep
it in automatic storage of a non-recursive function. This is not a substitute
for On-Demand: it navigates a fully materialized DOM.

### `decode.h` — descriptor-table decoding

Tables of `jsoc_decode_field` (terminated by `{ .name = NULL }`) map JSON
members onto struct fields via `offsetof`. Supported kinds: scalar getters
(which reuse the DOM getters, so ranges and integer/literal rules apply),
`JSOC_DECODE_STRING_REF` (decoded copy), `JSOC_DECODE_STRING_SLICE` (raw
slice), `JSOC_DECODE_STRUCT`, and `JSOC_DECODE_ARRAY` (destination pointer +
capacity/read-back count + element stride). Errors report the field name.
Duplicate JSON members keep the first match, matching the DOM's lookups.
Unknown members are ignored. `JSOC_DECODE_MAX_FIELDS` is 64 per struct.

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

When adding serialization/deserialization features, extend this discipline:
- serialize(DOM) must re-parse to an equal DOM (ignoring whitespace);
- encode(decode(x)) must equal x for generated structs;
- on-demand traversal must agree with DOM accessor results and errors;
- outputs must be accepted by simdjson as well, not just jsoc.

## Performance notes

Measured on a Ryzen 5 5600, `taskset -c 2`, best-of-run, release-native.

### DOM parse vs simdjson

- 19 real JSON documents: **19/19 wins**, geometric mean **1.27x**, aggregate
  throughput jsoc 2.92 GB/s vs simdjson 2.26 GB/s.
- 22 documents (adds the three compact API responses): **22/22 wins**,
  geomean **1.25x**.
- Narrowest win is `repeat.json` (~1.01–1.05x), dominated by run-to-run noise
  on an 11 KB document; number-heavy documents (`canada`, `numbers`) are
  ~1.7–2.0x.

### Tokenizer / stage 1

- `jsoc_classify64` alone: ~18.2 GB/s.
- Stage 1 + escape/string/start masks: ~12.3–13.3 GB/s.
- Isolated cost of the pure token store (count-only experiment): 2–9 %, i.e.
  the store is *not* what separates jsoc from simdjson stage 1.
- UTF-8 validation (`jsoc_validate_utf8`, lookup4 over 32-byte chunks):
  37–71 GB/s, 0.017–0.047 ms on the four fixtures.

### Serialization / deserialization

See the tables in §Serialization & deserialization. Current state: number-heavy
DOM serialization wins (raw lexemes), everything else loses; the writer core,
number formatting, streaming, and the missing on-demand front-end are the
causes.

### Rejected experiments (do not redo without a new measurement)

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
  and ~30 % larger `.text`, so the out-of-line block stays;
- 8-byte SWAR short-integer path, 16-byte-only number scan, conditional-widen
  16/32 B number scan, staged 16-byte number fast path, alphabet-LUT forward
  number scan: all measured worse;
- ungated / document-gated 2-byte UTF-8 fast paths, per-half non-ASCII feed,
  sentinel `indexes[count]`, hybrid 16/32 B index writer, iterative stage-2
  walk: all slower;
- forcing inlining of container parsers, `flatten`, removing the write delay,
  LTO/PGO/jump-table dispatch, a stack-token fast path for tiny inputs: no win.

### Where the remaining parse gap is

The DOM consumes a compact token stream: `jsoc_parse` and `jsoc_parse_many` use
`jsoc_tokenize_compact_checked`, which validates `,` and `:` through the same
grammar but does not store them. The public `jsoc_tokenize` stream is
unchanged. Separator stores were never the main cost (count-only experiment:
2–9 %), so the residual gap on string-heavy documents is the materialization of
12-byte tokens and the tokenize/DOM split itself. simdjson builds its DOM
directly from stage 1/2 masks. The next structural parse win is a mask-driven
DOM builder that skips the token array for the common cases; keep the
token-walking builder as the reference and error oracle. This is orthogonal to
the P0–P4 serde plan and should not be attempted in the same change.

## Next work

1. Execute the serde plan in order P0 → P1 → P2 → P3 → P4; P5/P6 are optional.
2. Mask-driven DOM construction (parse) if the twitter/citm gap must be closed
   further, with the token-walking builder kept as the oracle.
3. Fuse UTF-8 validation into stage 1 (`jsoc_validate_utf8` currently re-reads
   every byte).
4. Expose `number.h` helpers publicly if callers need
   `jsoc_value_get_double`'s fast path without a document.
5. Keep this file and the benchmark tables honest: re-run both comparison
   harnesses after any change.

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
- **Always `make clean` between variants.** `asan` leaves sanitized objects in
  `build/`; linking a non-clean release against them produced the historical
  spurious 0.3x benchmark. `build/.cflags` now prevents the other direction
  (phony runners rebuilding without optimization), but a clean build is still
  the only reliable comparison.
- `jsoc_value` is a union: scalars use `offset`/`length`, containers
  `first`/`count`. Never write both.
- The C++ comparison harnesses include the public headers; keep them C++-clean
  (`extern "C"` guards, no compound literals, no C-only designated initializers
  in inline functions).
- **Do not compare `jsoc_minify` (validating) with `simdjson::minify` (raw).**
  They are different operations. Use `jsoc_minify` vs parse+`to_string`, and
  add/benchmark `jsoc_minify_raw` separately.
- **Preserve the raw-lexeme advantage**: DOM serialization must not parse
  numbers or re-escape validated string slices. That is why canada/numbers win
  today.
