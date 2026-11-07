# Test suite: what is ported from simdjson, and what is not

jsoc's suite is organised by *topic*, not by file.  Where a simdjson test file
maps onto a jsoc suite, this table says so; where it does not (C++-only APIs,
C++20 `#embed`, static reflection), it is listed and the reason is recorded.

## jsoc suites

| binary | what it covers | simdjson files it ports |
|---|---|---|
| `build/test_tokenizer` | token stream parity, errors, partial streams, files, arena, differential fuzzing of **all three tokenizer modes** | `tests/dom/basictests.cpp`, `errortests.cpp` (parts), `simd8_tests.cpp` (in spirit) |
| `build/test_dom` | DOM structure, accessors, strings, numbers, arena reset, differential DOM checks, overflow scoping | `dom/basictests.cpp`, `dom/document_tests.cpp`, `dom/big_integer_tests.cpp` (behaviour) |
| `build/test_numbers` | powers of two/ten, subnormals, no double rounding, round-to-even, many digits, round-trip, integer boundaries, big lexemes, NaN/Inf, locale independence, random round-trips | `dom/integer_tests.cpp`, `dom/big_integer_tests.cpp`, `dom/nan_inf_tests.cpp`, `dom/locale_independence_tests.cpp`, `dom/random_string_number_tests.cpp`, `ondemand/ondemand_number_tests.cpp`, `ondemand_float_tests.cpp`, `ondemand_number_in_string_tests.cpp` |
| `build/test_errors` | truncation at every point, structural errors, wrong-type accessors, depth limit, number overrun scope, NOMEM/missing files | `dom/errortests.cpp`, `ondemand/ondemand_error_tests.cpp`, `ondemand/ondemand_wrong_type_error_tests.cpp`, `ondemand/ondemand_object_error_tests.cpp`, `ondemand/ondemand_array_error_tests.cpp` |
| `build/test_stream` | `jsoc_parse_many`: separators, adjacent documents, BOM, isolation, callback stop, errors mid-stream, root scalars, adversarial documents | `dom/document_stream_tests.cpp`, `ondemand/ondemand_document_stream_tests.cpp`, `dom/parse_many_test.cpp` |
| `build/test_pointer` | RFC 6901 vectors, escapes, index canonicalisation, the documented failures | `dom/pointercheck.cpp`, `dom/json_path_tests.cpp` (pointer subset), `ondemand/ondemand_json_pointer_tests.cpp` |
| `build/test_minify` | reference escaper (exact text), integer boundaries, corpus round-trip | `tests/minify_tests.cpp`, `dom/prettify_tests.cpp` (minify half), `dom/testjson2json.sh` |
| `build/test_padding` | exact-size buffers (ASan-visible), numbers/atoms/strings/containers at the end, deterministic random documents | `dom/unpadded_tests.cpp`, `dom/padding_contents_tests.cpp`, `ondemand/ondemand_padded_input.cpp`, `tests/padded_string_tests.cpp` (behaviour) |
| `build/test_unicode` | random UTF-8 generator, invalid sequences, the scalar/vector oracle agreement | `tests/unicode_tests.cpp`, `dom/stringparsingcheck.cpp` (behaviour) |
| `build/test_static` | struct sizes/offsets, trivial copyability, error message table | `tests/staticchecks.cpp`, `dom/trivially_copyable_test.cpp` |
| `build/fuzz` | 9 targets over generated inputs, standalone and libFuzzer-compatible | `fuzz/fuzz_parser.cpp`, `fuzz_atpath.cpp`, `fuzz_atpointer.cpp`, `fuzz_minify.cpp`, `fuzz_minifyimpl.cpp`, `fuzz_ndjson.cpp`, `fuzz_utf8.cpp`, `fuzz_element.cpp`, `fuzz_dump.cpp`, `fuzz_ondemand.cpp` |

## Corpora

`build/corpus` walks naming-convention directories and scores them (`scripts/
fetch-testdata.sh` fetches them):

| corpus | files | what it pins |
|---|---:|---|
| `jsonchecker/` | 1886 | the classic accept/reject suite, `adversarial/`, `minefield/` |
| `JSONTestSuite/test_parsing` | 318 | the grammar's implementation-defined corners |
| `jsonexamples/` | many | real-world documents: all must parse |

## Not ported (and why)

| simdjson file | reason |
|---|---|
| `tests/ondemand/*` (most) | the On-Demand API does not exist here; the behaviours that do apply (errors, numbers, strings, streaming) are covered by the suites above |
| `tests/builder/*` | static reflection + the string builder; jsoc's `decode.h` is a descriptor table instead |
| `tests/compile_time/*` | C++20 `#embed` compile-time JSON |
| `tests/compilation_failure_tests/*` | C++ compile-failure testing |
| `tests/dom/array_reverse_tests.cpp` | no `reverse()` in jsoc's API |
| `tests/fractured_json_tests.cpp` | `fractured_json()` formatting; jsoc has minify, not prettify/fractured output |
| `tests/dom/readme_examples*` | documentation snippets |
| `tests/dom/singleheadertest.cpp` | no single-header amalgamation |
| `tests/allocation_failure_tests.cpp` | needs fault injection; covered instead by NOMEM/argument tests |
| `tests/dom/document_stream_fuzz_tests.cpp` | its behaviour is in `build/fuzz`'s `ndjson` target |
| `tests/dom/jsoncheck.cpp`, `numberparsingcheck.cpp`, `stringparsingcheck.cpp`, `minefieldcheck.cpp`, `allparserscheckfile.cpp` | standalone corpus checkers; `build/corpus` + `test_unicode`/`test_numbers` cover the same ground |
| `tests/checkimplementation.cpp` | no force-implementation switch |

## Behavioural differences (deliberate)

1. **NaN / Infinity are rejected.** simdjson accepts them only when built with
   `SIMDJSON_ENABLE_NAN_INF`; JSON has no spelling for them, so jsoc never
   accepts them (see `tests/dom/nan_inf_tests.cpp` for the opt-in behaviour).
2. **Minify does not re-escape strings.** simdjson re-encodes every escape;
   jsoc writes the validated raw slice verbatim, so `"\u0041"` stays
   `"\u0041"` instead of becoming `"A"`. Both round-trip.
3. **Stream mode accepts adjacent documents.** `jsoc_parse_many` reads `1 2`
   *and* `{"a":1}{"b":2}`; `jsoc_tokenize` alone still rejects the latter, which
   is why the differential fuzzer tests strict, checked, *and* stream modes
   against each other.
4. **`jsoc_parse_many` delivers the documents before an error, but not a
   truncated one.** simdjson's document stream yields the truncated document as
   an error document; jsoc stops before it. A prefix that is itself invalid
   (an unterminated document followed by the bad bytes) leaves zero documents.
5. **No prettify.** `fractured_json()` / `to_string()` pretty printing has no
   jsoc equivalent; minify does.
