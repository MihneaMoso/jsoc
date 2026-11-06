#pragma once

#include "arena.h"
#include "dom.h"
#include "error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialization.
 *
 * Both functions build their text with a `jsoc_writer`, so the result lives in
 * the supplied arena and needs no separate free.  `jsoc_minify` validates the
 * input first: malformed JSON yields the tokenizer's error and no text.
 */

/* Re-emit a parsed document as JSON text with whitespace between tokens
 * removed.  Object member order is preserved. */
JSOC_API bool jsoc_minify(const char *input, size_t length, jsoc_arena *arena,
                          jsoc_string *out, jsoc_error *err) JSOC_NODISCARD;

/* Same, but re-serializing an existing document (e.g. to normalize it). */
JSOC_API bool jsoc_document_minify(const jsoc_document *doc, jsoc_arena *arena,
                                   jsoc_string *out) JSOC_NODISCARD;

#ifdef __cplusplus
}
#endif
