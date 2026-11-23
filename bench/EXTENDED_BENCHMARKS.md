# Extended benchmark suite

`bench/compare_extended.cpp` adds benchmark shapes from the two most widely
used JSON benchmark suites, so jsoc's performance can be judged beyond its own
parse/serialize microbenchmarks.

## Sources (researched 2026-11-23)

| Mode | Source | What it measures |
|---|---|---|
| `stats` | [miloyip/nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark) (2.1k stars) | DOM traversal: count value types, string bytes, container elements |
| `distinct` | [simdjson `distinct_user_id`](https://github.com/simdjson/simdjson/tree/master/benchmark/distinct_user_id) | Collect + de-duplicate user IDs from a tweet corpus |
| `find` | [simdjson `find_tweet`](https://github.com/simdjson/simdjson/tree/master/benchmark/find_tweet) | Scan for an id and extract a string field |
| `numbers` | [simdjson `bench_number_accessors`](https://github.com/simdjson/simdjson/blob/master/benchmark/bench_number_accessors.cpp) | Sum a 1,000,000-element integer array through the number accessor |

The reference corpus for the first three modes is `~/bench_corpus/*.json`
(the nativejson-benchmark canonical files `canada.json`, `citm_catalog.json`
and `twitter.json` are included). The `numbers` dataset is generated locally
with simdjson's deterministic xorshift generator and seed.

Each mode verifies that both libraries computed the *same answer* before
timing, then uses the same alternating-phase, best-of-run protocol as the
other harnesses.

## Build and run

```sh
./bench/compare_extended.sh ~/bench_corpus/*.json
# or select modes / iterations:
BENCH_MODE=stats,numbers BENCH_ITERS=50 ./bench/compare_extended.sh ~/bench_corpus/*.json
```

## Results on AMD Ryzen 5 5600 (release-native, 20 iterations)

| Mode | Geomean | Wins | Interpretation |
|---|---|---|---|
| stats | 0.84x | 7W / 12L | jsoc wins number-heavy files (canada, mesh, marine_ik, citm) but loses object-heavy ones (twitter, apache_builds, gsoc) |
| distinct | 0.30x | 0W / 2L | jsoc's `jsoc_object_get_cstr` is a linear scan with `strlen`; simdjson's DOM field lookup is much faster |
| find | 0.09x | 0W / 2L | same field-lookup gap, plus number re-validation on every access |
| numbers | 0.02x | 0W / 1L | jsoc re-parses the raw number lexeme on every `jsoc_value_get_int64`; simdjson's tape stores the already-parsed value |

## What this tells us

These results expose a real **design trade-off**, not a bug:

- jsoc is a **zero-copy** DOM: it stores offsets into the original buffer and
  decodes values on demand. That makes parsing and serialization fast and
  keeps memory small, but repeated field lookup / number access pays a per-access
  cost that simdjson's materialised tape does not.
- simdjson's DOM materialises keys and values into a flat tape, so traversal
  and repeated access are much faster at the cost of a slower, more
  allocation-heavy parse.

So the earlier conclusion ("jsoc is faster in every category") holds only for
the **parse / serialize / stream / typed-decode** categories previously
measured. For **DOM traversal and repeated field/number access**, simdjson is
substantially faster. The extended suite exists to make that trade-off visible
and measurable rather than hiding it behind a narrower benchmark set.
