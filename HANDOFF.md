# jsoc — rewrite handoff

Pragmatic continuation notes for the C23 JSON tokenizer/decoder rewrite.

## TL;DR state (2026-11-04)

- **All 29 tests pass** in `debug`, `release`, `release-native`,
  `PORTABLE=1 release`, and `asan` (ASan+UBSan).
- **No compiler warnings** under the project warning set
  (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wvla -Wdouble-promotion`).
- The tokenizer is now a **fused AVX2 stage-1 + switch-grammar** design. It
  classifies 64-byte blocks into structural/string masks and walks only the
  structural starts, instead of rescanning bytes per token.
- Both backends are kept and cross-checked:
  - `jsoc_tokenize_scalar_impl` — portable reference state machine (also used
    for inputs shorter than one SIMD block);
  - `jsoc_tokenize_simd_impl` — fused vectorized path, AVX2 builds only.
- Throughput vs simdjson **DOM** on the same machine (`taskset -c 2`):

| fixture | size | jsoc tokenize | simdjson DOM | delta |
|---|---:|---:|---:|---:|
| compact | 4.52 MB | **3.24 ms / 1.39 GB/s** | 3.33 ms / 1.36 GB/s | ~3 % faster |
| pretty | 7.43 MB | **3.66 ms / 2.03 GB/s** | 3.63 ms / 2.05 GB/s | ~1 % slower |
| v_ints | 6.89 MB | **5.80 ms / 1.19 GB/s** | 7.36 ms / 0.94 GB/s | ~21 % faster |
| v_emptystr | 3.00 MB | **4.2 ms / 0.72 GB/s** | 4.91 ms / 0.61 GB/s | ~15 % faster |
| v_nostr | 3.78 MB | **3.11 ms / 1.22 GB/s** | 3.30 ms / 1.14 GB/s | ~6 % faster |

jsoc only tokenizes; simdjson DOM also materializes a full document. Treat the
table as a sanity check, not an apples-to-apples parse benchmark.

## Build / test / bench

```bash
cd /home/moso/jsoc

make clean
make debug                 # -O0 -g3; builds tests + bench
./build/test_tokenizer     # 29/29 (includes the differential fuzz test)

make release-native        # -O3 -march=native; default hot build
./build/test_tokenizer
BENCH_ITERS=300 taskset -c 2 ./build/bench \
    /tmp/opencode/bench_compact.json \
    /tmp/opencode/bench_pretty.json \
    /tmp/opencode/v_ints.json \
    /tmp/opencode/v_emptystr.json \
    /tmp/opencode/v_nostr.json

make asan                  # ASan + UBSan
./build/test_tokenizer

make PORTABLE=1 release    # forces the scalar backend
make check                 # builds asan and runs the test binary

make test                  # rebuilds release, then runs tests
make bench                 # rebuilds release, then benches BENCH_ARGS
```

Useful make switches:

- `PORTABLE=1` → `-DJSOC_NO_SIMD`; `jsoc_tokenize_simd_impl` becomes a scalar
  stub, so the differential test degenerates to a self-check.
- `VECTORIZE=1` → re-enable the compiler’s auto-vectorizer (off by default; see
  perf notes).
- `BENCH_ARGS="file1 file2"`, `BENCH_ITERS=N`.
- The build directory is `build/`; `build/.cflags` is a stamp that forces a
  rebuild when compiler flags change, so switching between debug/asan/release
  is safe without `make clean`.

If the benchmark fixtures are missing:

```bash
python3 - <<'EOF'
import json, random
random.seed(7)
data = [{"id": i, "name": f"item_{i}", "value": random.random()*1000,
         "tags": ["tag_"+str(j) for j in range(3)], "active": i % 2 == 0,
         "nested": {"a": None, "b": True, "score": i*1.5}} for i in range(30000)]
s = json.dumps(data, separators=(',', ':'))
open("/tmp/opencode/bench_compact.json", "w").write(s)
open("/tmp/opencode/bench_pretty.json", "w").write(json.dumps(data, indent=2))
print(len(s), "bytes compact")
EOF
```

## Layout

```
include/
  export.h    visibility / attribute macros, JSOC_C23 detection
  error.h     jsoc_error_code, jsoc_error, jsoc_error_message/format
  arena.h     jsoc_arena handle + alloc helpers
  token.h     jsoc_token (12 bytes), token type names, token_print
  jsoc.h      jsoc_tokenize / jsoc_tokenize_file / jsoc_result / decode stubs
src/
  arena.c     chunked bump allocator
  error.c     error strings + formatting
  token.c     token printing only
  simd.h      pure block scanners: classify64, string/whitespace scans,
              escape chaining, prefix-xor, digit runs
  tokenizer.c fused SIMD tokenizer, portable reference state machine
  tokenizer_internal.h  hidden backend entry points (used by the tests)
test/test_tokenizer.c  29 tests, incl. deterministic differential fuzzing
bench/bench.c          multi-file throughput benchmark
legacy/                old implementation (reference only, not built)
```

`src/tokenizer_simd.c` was deleted; it was an early broken SIMD attempt. Do not
revive it; all vector code lives in `src/simd.h` and is consumed by
`src/tokenizer.c`.

## Data model / invariants

### Tokens

```c
typedef struct {
    uint32_t offset;   // byte offset into the caller's input
    uint32_t length;   // raw byte length (escapes are not decoded)
    jsoc_token_type type; // enum with uint8_t underlying type
} jsoc_token;          // 12 bytes
```

- **Zero-copy.** A string token spans the bytes *between* the quotes; escapes
  and `\uXXXX` are validated but not decoded.
- Tokens are allocated from the `jsoc_arena`; there is no malloc/free in the
  token API and no leak.
- Tokens are only valid while the input buffer and the arena are alive.
  `jsoc_tokenize_file` reads the file into the **arena**, so its tokens stay
  valid until the arena is reset/destroyed. Do not reintroduce
  `free(file_buffer)`.
- The token layout is part of the public contract. Do not change it while
  optimizing the hot loop.

### Arena

Chunked bump allocator; the caller’s `jsoc_arena *` handle is stable.

- The handle always describes the current chunk, so allocation is a pointer
  bump.
- On overflow, a larger chunk is allocated and the old chunk’s bookkeeping is
  moved into a retired node linked via `next`. **Old bytes are never copied**,
  so earlier pointers remain valid.
- `jsoc_arena_reset` frees retired chunks and rewinds the current chunk.
- `jsoc_arena_destroy` frees everything.

### Errors

`jsoc_error_code` includes `JSOC_ERR_INVALID_CONTROL` and
`JSOC_ERR_INPUT_TOO_LARGE`. `jsoc_tokenize` returns:

- `JSOC_ERR_NOMEM` for `!arena` or allocation failure;
- `JSOC_ERR_INPUT_TOO_LARGE` when `length > UINT32_MAX` (offsets are
  `uint32_t`);
- `JSOC_ERR_SYNTAX` for empty input.

Line/column are computed **lazily** in `make_error_at()` by rescanning up to the
error offset. This intentionally keeps a per-byte branch out of the hot path.
Do not reintroduce eager line/column tracking.

## Tokenizer architecture (`src/tokenizer.c`)

### Dispatch

```c
if (length >= 64) return jsoc_tokenize_simd_impl(...);
return jsoc_tokenize_scalar_impl(...);
```

The cut-over is inside `jsoc_tokenize` under `#if JSOC_SIMD_AVX2`. The fused
backend has a fixed 64-byte setup cost, so tiny documents use the portable
state machine. `jsoc_tokenize_simd_impl` also falls back through the same
scalar reference whenever vector support is unavailable.

### Portable reference (`jsoc_tokenize_scalar_impl`)

Explicit labels, no recursion: `sv_value`, `sv_key`, `sv_colon`, `sv_after`,
`at_eof`, `fail`. This path is the fixed point for error taxonomy and partial
token streams. It is also the reference used by the differential tests.

Key semantics pinned by tests:

- `[}` → `JSOC_ERR_SYNTAX`
- `{"a" 1}` → `JSOC_ERR_EXPECTED_COLON`
- `[1 2]` → `JSOC_ERR_EXPECTED_COMMA_OR_END`
- `{} extra` → `JSOC_ERR_TRAILING_GARBAGE`
- `"hello` → `JSOC_ERR_UNTERMINATED_STRING`
- trailing commas rejected (`[1,]`, `{"a":1,}`)
- leading zeros rejected (`[01]`, `[-012]`)
- invalids rejected: `[1.]`, `[1e]`, `[-]`, `[+1]`
- unescaped control bytes in strings → `JSOC_ERR_INVALID_CONTROL`
- max nesting is `JSOC_MAX_DEPTH` (1024)
- multiple top-level values are accepted only when separated by whitespace
  (ndjson-style), never back-to-back.
- junk glued to a number/literal (`123abc`, `truex`, `nullx`) is an error at the
  first byte after the scalar; the fused path checks the delimiter byte
  directly *before* the next structural start.

### Fused backend (`jsoc_tokenize_simd_impl`)

Per 64-byte block:

1. `jsoc_classify64()` returns masks for whitespace, operators
   (`{}[]:,`), quotes, backslashes, and control bytes.
2. `jsoc_escape_next()` chains escape state across block boundaries; masking it
   out of the quote mask gives real quotes.
3. `jsoc_prefix_xor()` produces the in-string mask; the carry is kept in
   `f->s1.prev_in_string`.
4. `string_tail = in_string ^ quote`; `scalar = ~(op | whitespace)`; a
   non-quote scalar is a start only when the previous byte was not a scalar
   (`follows`). The final starts mask is
   `((op | (scalar & ~follows)) & ~string_tail) | (quote & ~in_string)`.
5. Only set bits of `starts` are visited. Each visit dispatches through a
   `jsoc_token_kind` 256-byte lookup table that maps structural bytes to
   token types and scalar-start bytes (`t f n - 0-9`) to the sentinel
   `JSOC_KIND_SCALAR = 0xFF`. Invalid bytes map to 0.

The last partial block is copied into a 64-byte padded buffer and processed
with a `valid` low-bit mask, so no code path reads outside the caller’s input.

Hot-loop choices that were measured, not guessed:

- **Switch-based grammar, not a transition table.** `grammar_step` /
  `grammar_after` are ordered by frequency and produce constant next states;
  this removed a table load + indirection per token.
- **Scalar before structural in the dispatch.** The scalar/literal handler is
  placed before the structural handler; the table sentinel also removes the
  old invalid-start comparison chain.
- **Comma and colon fusion.** After a value the next structural byte is usually
  `,`; after a key it is `:`. Both are consumed in the same iteration (including
  inside `fused_scalar_done`) to avoid a trip through the outer bit loop.
- **Immediate scalar emission + `goto again` chaining.** A completed value
  continues directly with the next start in the same block.
- **Delimiter check instead of `jsoc_skip_ws`.** `fused_is_scalar_end()` accepts
  only space/tab/LF/CR/`,`/`}`/`]` as a scalar terminator. This is cheaper than
  scanning whitespace and preserves the portable error offsets.
- **Unchecked token stores.** Before processing a block, reserve 65 slots
  (`fused_grow` if needed). A block can emit at most one token per start plus a
  carried-over pending scalar; `fused_emit()` therefore has no bounds branch.
- **SIMD digit runs.** `jsoc_digit_run()` (AVX2/SSE2) finds the end of a digit
  run for numbers; `validate_number` still enforces strict JSON number grammar.
- **String fast path.** If the closing quote is already the next start in the
  same block, emit the token immediately; otherwise set
  `JSOC_PEND_STRING` with the opening offset and finish in the next block.
- **Keep `fused_process_block` inlined.** `noinline`, 2x/3x/4x unrolling, extra
  alignment flags, and cold-path helper extraction were all measured slower or
  neutral. Do not re-add them without a fresh benchmark.

### Deferred string validation (critical)

The hot loop never validates string contents; it only tracks whether any block
contained a backslash or an in-string control byte (`any_special`). When
`any_special` is set, `fused_strings_valid()` runs after tokenization:

- it repeats stage 1, pairs unescaped opening/closing quotes, and
  over-attributes “this string may contain a special”;
- for each pair that may contain a special it calls the shared
  `validate_string_content()` with the raw content range;
- a final unterminated string is validated up to `length` as well, because the
  reference scanner reports invalid escapes/control bytes *before* the missing
  closing quote.

If validation fails, **or if the fast scan already failed**, the whole input is
re-tokenized with `jsoc_tokenize_scalar_impl`. This is the parity contract: the
public token stream and error code/offset are always exactly those of the
portable backend. Two consequences:

- Do **not** `goto fail` from the block loop while `any_special` could be set;
  the fast error may be later than a deferred string error. The current loop
  breaks on failure, then re-runs the scalar path when `any_special`.
- When a string opens in one block and a backslash/control byte lands in a later
  block, the closing-quote branch must validate if *any* special is present in
  the closing block (`pending_special || special != 0`). Otherwise a special in
  the same block as the closer is missed. Validation is bounded by
  `[open + 1, close)`, so over-attribution is harmless.

These three cases were all found by the differential fuzzer and fixed; keep the
tests that cover them.

## Correctness / differential testing

`test/test_tokenizer.c` test 29 (`differential_fuzz`) drives both backends
directly through `src/tokenizer_internal.h` (the test rule adds `-Isrc`) and
requires exact agreement on:

- error code and offset,
- token count,
- every token’s `type`, `offset`, and `length`.

It covers:

- generated structured documents (objects/arrays/numbers/literals/strings),
- random whitespace insertion,
- single-byte mutations,
- raw random bytes,
- long strings with valid/invalid escapes and raw control bytes at every offset
  relative to the 64-byte block, including truncation and arbitrary leading
  padding.

The seed is fixed, so failures are reproducible. On mismatch the test prints
both results and an escaped dump of the input.

During development this fuzzer also exposed a pitfall: building it without
`-march=native` made the SIMD backend a scalar stub, so “0 mismatches” proved
nothing. Always compile differential tests with the vector ISA enabled, or
check `jsoc_tokenizer_simd_available()`.

## Performance notes

Measured on a Ryzen 5 5600, `taskset -c 2`, best-of-run, 300 iterations.
Single-file runs are more stable than one multi-file run; `v_ints` needs the
iterations to settle.

Stage-1 microbenchmarks (classify + mask algebra only):

- `jsoc_classify64` alone: ~18.2 GB/s (0.248 ms compact, 0.408 ms pretty).
- stage 1 + escape/string/start masks: ~12.3–13.3 GB/s
  (0.368 ms compact, 0.569 ms pretty).

Isolated cost experiments on the real fixture set (temporary variants):

- token stores: ≈0.4–0.7 ms;
- grammar: ≈0.4–1.0 ms;
- everything else is stage 1 + per-start dispatch.

Rejected experiments (do not redo without new measurements):

- materialized structural index array (two-stage with an array): slower;
- grammar transition table: ~0.7–1.3 ms slower than the switch;
- quote-chain `again_string` fast path: helped `v_emptystr`, regressed all other
  fixtures;
- block-loop unrolling (2x/3x/4x): slower;
- `noinline` block processing, cold-path helper extraction, function/loop
  alignment flags: slower or neutral;
- auto-vectorization (`VECTORIZE=1`): slower.

Remaining gap: the pretty fixture is ~1 % behind simdjson DOM. Everything else
is ahead.

## Next work

### 1. Close the pretty gap (optional, small)

Pretty JSON is whitespace-heavy. Stage 1 already skips it; the remaining cost is
per-start dispatch and the grammar chain. Ideas worth measuring:

- fold whitespace-carried state into the starts mask to avoid re-deriving
  “previous byte was whitespace” for ndjson separation;
- specialize the common `key : value ,` quartet in the structural handler;
- prefetch the next block’s `classify64` results one iteration ahead.

Do not change the public token stream or weaken validation to gain speed.

### 2. Decoder (next feature milestone)

Once the tokenizer API is frozen, add `jsoc_decode(T, const char *json, …)`:

- a decode context holding the arena, token array, input pointer, and current
  token index;
- generated/reflected per-type descriptors (`field name`, `offsetof`, kind,
  optional pointer/size for arrays, validation range);
- object decoding matches keys against a descriptor table; array decoding loops;
- scalars parse from raw token slices with overflow checks;
- keep errors in `jsoc_decode_result` (`token_index`, `field_name`) or extend
  the existing decode enum.

The decoder depends on token offsets and the zero-copy lifetime rules, so freeze
those first.

### 3. Packed tokens (only if measurements justify it)

12 bytes/token is 15.8 MB for 1.32 M tokens. An 8-byte packed form (type in the
top bits of `offset`/`length`) halves stores but limits input/token size and
changes the public struct. Do not do this without a measured win and a plan for
the public API.

### 4. Threading

Out of scope for now. `jsoc_tokenize` must stay single-threaded; a parallel
block tokenizer would change error ordering and the partial-token contract.

## Gotchas / rules of thumb

- **Never free memory that tokens point into.** The arena owns it; for
  caller-provided input, the caller owns it.
- **`jsoc_arena_reset` invalidates every token and every allocation** from that
  arena. The bench resets and re-tokenizes each iteration on purpose.
- **Keep line/column lazy.** Eager tracking costs a branch per byte.
- **Keep the vector helpers out-of-line** and keep `-fno-tree-vectorize` unless
  a new measurement says otherwise.
- **Run all build variants** after touching headers: debug catches
  intrinsics/attribute problems that `release-native` masks.
- **Do not “optimize” by removing validation.** The tests pin strict JSON
  errors; the deferred validator must stay exact.
- **Keep the scalar backend.** It is the oracle for fuzzing and the short-input
  path.
- `legacy/` is reference only. Do not add it to the build.

## Verification checklist before handing off again

```bash
make clean
make debug              && ./build/test_tokenizer          # 29/29
make release            && ./build/test_tokenizer          # 29/29
make release-native     && ./build/test_tokenizer          # 29/29
make PORTABLE=1 release && ./build/test_tokenizer          # 29/29
make asan               && ./build/test_tokenizer          # 29/29, no ASan/UBSan
BENCH_ITERS=300 taskset -c 2 ./build/bench \
    /tmp/opencode/bench_compact.json \
    /tmp/opencode/bench_pretty.json \
    /tmp/opencode/v_ints.json \
    /tmp/opencode/v_emptystr.json \
    /tmp/opencode/v_nostr.json
```

Expected: all tests pass, no warnings, compact ≈1.39 GB/s, pretty ≈2.03 GB/s,
v_ints ≈1.19 GB/s, v_emptystr ≈0.72 GB/s, v_nostr ≈1.22 GB/s.
