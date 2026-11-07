#pragma once

#include "jsoc.h"
#include "export.h"
#include <stdbool.h>

/* Internal entry points.  `jsoc_tokenize` validates its arguments and then
 * dispatches here; the test suite calls both variants directly to prove that
 * the SIMD and portable paths produce byte-identical token streams and
 * errors.  These are deliberately not part of the public API. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_impl(const char *input, size_t length,
                                                    jsoc_arena *arena);
JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_impl(const char *input, size_t length,
                                                  jsoc_arena *arena);

/* Checked variants: same token stream, plus the index of the first number that
 * is syntactically valid but not representable (SIZE_MAX when none).  Range
 * errors are returned out of band so callers can scope them to one document. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_checked(const char *input, size_t length,
                                                jsoc_arena *arena,
                                                size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_checked_impl(const char *input,
                                                            size_t length,
                                                            jsoc_arena *arena,
                                                            size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_checked_impl(const char *input,
                                                          size_t length,
                                                          jsoc_arena *arena,
                                                          size_t *overflow_token);

/* Stream variants: identical token streams, except that a second top-level
 * value is accepted *without* whitespace after the first one, which is what
 * `jsoc_parse_many` needs for ndjson (simdjson's parse_many accepts adjacent
 * documents; `jsoc_tokenize` still rejects them). */
JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_stream_impl(const char *input,
                                                           size_t length,
                                                           jsoc_arena *arena,
                                                           size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_stream_impl(const char *input,
                                                         size_t length,
                                                         jsoc_arena *arena,
                                                         size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_stream_checked(const char *input,
                                                       size_t length,
                                                       jsoc_arena *arena,
                                                       size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_stream_compact_impl(
    const char *input, size_t length, jsoc_arena *arena,
    size_t *overflow_token);
JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_stream_compact_impl(const char *input, size_t length,
                                       jsoc_arena *arena,
                                       size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_stream_compact_checked(
    const char *input, size_t length, jsoc_arena *arena,
    size_t *overflow_token);

/* Compact variant: same validation and the same errors as
 * `jsoc_tokenize_checked`, but ',' and ':' are not stored, because the DOM
 * infers them from the container. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_compact_impl(const char *input,
                                                            size_t length,
                                                            jsoc_arena *arena,
                                                            size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_compact_impl(const char *input,
                                                          size_t length,
                                                          jsoc_arena *arena,
                                                          size_t *overflow_token);
JSOC_INTERNAL jsoc_result jsoc_tokenize_compact_checked(const char *input,
                                                        size_t length,
                                                        jsoc_arena *arena,
                                                        size_t *overflow_token);

/* DOM fast path: same compact token stream and errors as
 * `jsoc_tokenize_compact_checked`, but UTF-8 is validated inside the stage-1
 * scan (AVX2, length >= 64); other inputs take the ordinary UTF-8 check plus
 * the portable compact tokenizer. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_compact_utf8(const char *input,
                                                     size_t length,
                                                     jsoc_arena *arena,
                                                     size_t *overflow_token);

/* True when this build contains the AVX2 stage-1 tokenizer. */
JSOC_INTERNAL bool jsoc_tokenizer_simd_available(void);

/* Lazy line/column computation shared by both backends. */
JSOC_INTERNAL jsoc_error jsoc_error_at_offset(const char *input, size_t length,
                                              jsoc_error_code code, size_t offset);
