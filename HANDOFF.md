# jsoc — rewrite handoff

Pragmatic continuation notes for the C23 JSON tokenizer/decoder rewrite.

## TL;DR state (2026-11-02)

- **All 28 tests pass** in `debug`, `release`, `release-native`, `asan` (ASan+UBSan), and
  `PORTABLE=1 release`.
- **No compiler warnings** under the project warning set
  (`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wvla -Wdouble-promotion`).
- Throughput on the 4.5 MB compact / 7.4 MB pretty fixtures:
  - **compact: ~4.7 ms, ~0.96 GB/s, 1,320,002 tokens**
  - **pretty:  ~6.1 ms, ~1.22 GB/s**
- This is ~33 % faster than the state at the start of this pass, but still **not
  simdjson-class (2–5 GB/s)**. The state machine, not the byte scanners, is now the
  bottleneck. The next big win is a two-stage (bulk structural scan) tokenizer; see
  “Next performance work”.

## Build / test / bench

```bash
cd /home/moso/jsoc

make clean
make debug                 # -O0 -g3; builds tests + bench
./build/test_tokenizer     # 28/28

make release-native        # -O3 -march=native; default hot build
./build/test_tokenizer
BENCH_ITERS=200 taskset -c 2 ./build/bench \
    /tmp/opencode/bench_compact.json \
    /tmp/opencode/bench_pretty.json

make asan                  # ASan + UBSan
./build/test_tokenizer

make PORTABLE=1 release    # forces -DJSOC_NO_SIMD (scalar scanners)
make check                 # builds asan and runs the test binary

make test                  # rebuilds release, then runs tests
make bench                 # rebuilds release, then benches BENCH_ARGS
```

Useful make switches:

- `PORTABLE=1` → `-DJSOC_NO_SIMD`.
- `VECTORIZE=1` → re-enable the compiler’s auto-vectorizer (see perf notes; it is
  **off by default on purpose**).
- `BENCH_ARGS="file1 file2"`, `BENCH_ITERS=N`.
- The build directory is `build/`; `build/.cflags` is a stamp that forces a rebuild
  when compiler flags change, so switching between debug/asan/release is safe
  without `make clean`.

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
  token.c     token printing only (malloc-based token array was removed)
  simd.h      pure scanning primitives (scalar fast path + out-of-line SIMD)
  tokenizer.c state machine; the only file that needs changing for perf work
test/test_tokenizer.c  28 tests
bench/bench.c          multi-file throughput benchmark
legacy/                old implementation (reference only, not built)
```

`src/tokenizer_simd.c` was deleted: it was broken (gated AVX2 intrinsics on
`__SSE2__`, compared against a repeating whitespace-pattern table, and called
`__builtin_ctz(~0)` in an infinite-loop path).

## Data model / invariants

### Tokens

```c
typedef struct {
    uint32_t offset;   // byte offset into the caller's input
    uint32_t length;   // raw byte length (escapes are not decoded)
    jsoc_token_type type; // enum with uint8_t underlying type
} jsoc_token;          // 12 bytes
```

- **Zero-copy.** A string token spans the bytes *between* the quotes; escapes and
  `\uXXXX` are validated but not decoded.
- Tokens are allocated from the `jsoc_arena`; there is no malloc/free in the token
  API and no leak.
- Tokens are only valid while the input buffer and the arena are alive.
  `jsoc_tokenize_file` reads the file into the **arena**, so its tokens stay valid
  until the arena is reset/destroyed. Do not reintroduce `free(file_buffer)`.

### Arena

Chunked bump allocator; the caller’s `jsoc_arena *` handle is stable.

- The handle always describes the current chunk, so allocation is a pointer bump.
- On overflow, a larger chunk is allocated and the old chunk’s bookkeeping is moved
  into a retired node linked via `next`. **Old bytes are never copied**, so earlier
  pointers remain valid.
- `jsoc_arena_reset` frees retired chunks and rewinds the current chunk.
- `jsoc_arena_destroy` frees everything.

### Errors

`jsoc_error_code` includes `JSOC_ERR_INVALID_CONTROL` and
`JSOC_ERR_INPUT_TOO_LARGE`. `jsoc_tokenize` returns:

- `JSOC_ERR_NOMEM` for `!arena` or allocation failure;
- `JSOC_ERR_INPUT_TOO_LARGE` when `length > UINT32_MAX` (offsets are `uint32_t`);
- `JSOC_ERR_SYNTAX` for empty input.

Line/column are computed **lazily** in `make_error_at()` by rescanning up to the
error offset. This intentionally keeps a per-byte branch out of the hot path. Do
not reintroduce eager line/column tracking.

## Tokenizer state machine (`src/tokenizer.c`)

Explicit labels, no recursion:

- `sv_value` — expects a value (object/array start, string, number, literal, or a
  matching close for an empty container).
- `sv_key` — inside an object, expects a string key or `}`.
- `sv_colon` — after a key, expects `:`.
- `sv_after` — after a value, expects `,`, the matching close, or end of input.
- `at_eof` — checks for unterminated containers and emits `JSOC_TOK_EOF`.
- `fail` — returns the error plus the partial token prefix.

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

Token capacity starts at `length / 3 + 32` tokens. The measured average is
~3.4 bytes/token; using `/4` caused a grow+memcpy on the compact fixture. If you
change token density assumptions, re-measure.

## SIMD layer design (`src/simd.h`)

Two pure functions:

- `jsoc_skip_ws(p, len, i)` — first non-whitespace index.
- `jsoc_find_string_special(p, len, i)` — first `"`, `\`, or control byte (< 0x20).

Design that was measured to be fastest:

1. Inlined fast path is **scalar** and probes at most `JSOC_SCALAR_PROBE` (16) bytes.
   Typical JSON strings/whitespace runs are shorter than that.
2. Only longer runs call the **out-of-line** `jsoc_skip_ws_long` /
   `jsoc_find_string_special_long` vector helpers (AVX2 when `__AVX2__`, else SSE2,
   else scalar).
3. `JSOC_NO_SIMD` forces the scalar path.

Two non-obvious lessons, both measured:

- **Do not inline the vector helpers.** Inlining AVX2 code at every call site
  bloats the hot loop and made the common short-run case 5–10 % slower even though
  the vector instructions were never executed. Keeping them `JSOC_NOINLINE` fixed it.
- **Disable compiler auto-vectorization** (`-fno-tree-vectorize`, the default).
  With `-march=native`, GCC auto-vectorized the short byte loops and added
  peel/remainder branches per token; throughput dropped from ~0.96 GB/s to
  ~0.78 GB/s compact. `VECTORIZE=1` reproduces the slow build for experiments.

Measured (same machine, 100–200 iterations, pinned to one core):

| build | compact | pretty |
|---|---|---|
| `release-native` (AVX2 scanners, `-fno-tree-vectorize`) | ~0.96 GB/s | ~1.22 GB/s |
| `release` (SSE2 scanners) | ~0.94 GB/s | ~1.20 GB/s |
| `PORTABLE=1 release` (scalar) | ~0.95 GB/s | ~1.20–1.23 GB/s |
| `release-native VECTORIZE=1` (auto-vectorized) | ~0.78 GB/s | ~1.07 GB/s |

At this point the scanners are not the bottleneck. Keep the SIMD code for
long-string/long-whitespace inputs and move on to the structural stage.

## Bugs fixed in this pass (do not regress)

- AVX2 intrinsics were guarded only by `__SSE2__` → debug build failed with
  “target specific option mismatch”.
- Whitespace mask compared input to a repeating `' ', '\t', '\n', '\r'` pattern
  instead of four splats.
- `combined == 0` → `__builtin_ctz(~0)` = 0 could loop forever. The current code
  uses `__builtin_ctzll(~(uint64_t)mask)` only when the mask is known not all-ones.
- Arena growth returned `aligned - size` instead of `aligned`, and reallocated +
  memcpy’d live data. Now chunked, no copies, stable pointers.
- `jsoc_tokenize_file` freed the file buffer while zero-copy tokens still pointed
  into it. The file bytes are now arena-owned.
- Jsoc token array API was malloc-based and redundant; removed. Only the arena
  token storage remains.
- `JSOC_NODISCARD` was attached to `void` functions; removed.
- Test 24 generated `[0123456789…]`, which is invalid JSON (leading zero). It now
  generates a valid long number, and `test_strict_numbers` asserts the strictness.

## Next performance work

### 1. Two-stage / bulk structural scan (highest impact)

Current cost is ~3.5 ns per token (1.32 M tokens in 4.7 ms), dominated by the
per-token state dispatch and token stores. To reach 2–5 GB/s, follow the simdjson
split:

**Stage 1 (vectorized, over 64-byte blocks):**
- classify bytes into structural (`{ } [ ] : ,`), quote, backslash, whitespace,
  and other;
- maintain an “inside string” mask derived from quote positions and backslash
  escapes;
- produce a compact list of indexes for structural characters and string starts.
- AVX2 approach: compare against `{ } [ ] : , " \\`, use `movemask`, then
  `_pext_u64` (BMI2) to extract indexes. `__builtin_popcountll` gives the output
  count. A 64-byte block is typically one or two loads.

**Stage 2 (scalar, over the index list):**
- walk structural indexes; use the original state machine (`sv_value` etc.) but
  never scan for punctuation again;
- numbers/strings/whitespace are delimited by the known structural positions;
- keep zero-copy token emission unchanged so the public API and tests stay the
  same.

Suggested internal shape:

```c
typedef struct {
    uint32_t *struct_idx;  // arena-allocated, one per structural char
    uint32_t  struct_count;
} jsoc_stage1;
```

Do not change the public token format during this step. Add an internal fast path
for inputs above some threshold (e.g. 4 KiB) and keep the current state machine for
small inputs until the new path is proven by the full test suite.

### 2. Number fast path

Numbers are currently parsed one byte at a time. Options, in order of effort:

- Add a tight integer fast path that accumulates digits in a `uint64_t` and only
  falls back to the general scanner for `.`, `e`, or `E`.
- Use an 8-byte SWAR digit check:
  `x = load64; if ((x & 0xF0F0...) == 0x3030... && all_low_nibbles_<=9) …`.
  Stop at the first non-digit with `__builtin_ctzll` on the mismatch mask.
- Keep the strict leading-zero and `-`/`+` rules; tests pin them.

### 3. String / escape work

- String tokens are raw slices today, which is correct for a tokenizer.
- When the decoder lands, unescape lazily into the arena only if the slice
  actually contains `\` or `\u`. Most strings do not.
- Consider validating UTF-8 in stage 1 (the enum `JSOC_ERR_INVALID_UTF8` is already
  reserved but unused).

### 4. Token representation (only if measurements justify it)

12 bytes/token is 15.8 MB for 1.32 M tokens. An 8-byte packed form (e.g. type in
the top bits of `offset` or `length`) would halve stores but limits input/token
size and changes the public struct. Do not do this before stage 1; measure first.

### 5. Reflection-based decoder (next feature milestone)

Once stage 1 + tokenizer are stable, add `jsoc_decode(T, const char *json, …)`:

- a decode context holding the arena, token array, input pointer, and current
  token index;
- generated/reflected per-type descriptors (`field name`, `offsetof`, kind,
  optional pointer/size for arrays, validation range);
- object decoding matches keys against a descriptor table; array decoding loops;
- scalars parse from raw token slices with overflow checks;
- keep errors in `jsoc_decode_result` (`token_index`, `field_name`) or extend the
  existing decode enum.

Do not start the decoder until the tokenizer API is frozen; the decoder will
depend on token offsets and the zero-copy lifetime rules.

## Gotchas / rules of thumb

- **Never free memory that tokens point into.** Arena owns it; for caller-provided
  input, the caller owns it.
- **`jsoc_arena_reset` invalidates every token and every allocation** from that
  arena. The bench resets and re-tokenizes each iteration on purpose.
- **Keep line/column lazy.** Eager tracking costs a branch per byte.
- **Keep the vector helpers out-of-line** and keep `-fno-tree-vectorize` unless a
  new measurement says otherwise.
- **Run all build variants** after touching headers: debug catches
  intrinsics/attribute problems that `release-native` masks.
- **Do not “optimize” by removing validation**; the tests pin strict JSON errors.
- `legacy/` is reference only. Do not add it to the build.

## Verification checklist before handing off again

```bash
make clean
make debug              && ./build/test_tokenizer          # 28/28
make asan               && ./build/test_tokenizer          # 28/28, no ASan/UBSan
make PORTABLE=1 release && ./build/test_tokenizer          # 28/28
make release-native     && ./build/test_tokenizer          # 28/28
BENCH_ITERS=200 taskset -c 2 ./build/bench \
    /tmp/opencode/bench_compact.json /tmp/opencode/bench_pretty.json
```

Expected: all tests pass, no warnings, compact ≥ ~0.95 GB/s, pretty ≥ ~1.2 GB/s.
