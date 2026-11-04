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

/* True when this build contains the AVX2 stage-1 tokenizer. */
JSOC_INTERNAL bool jsoc_tokenizer_simd_available(void);

/* Lazy line/column computation shared by both backends. */
JSOC_INTERNAL jsoc_error jsoc_error_at_offset(const char *input, size_t length,
                                              jsoc_error_code code, size_t offset);
