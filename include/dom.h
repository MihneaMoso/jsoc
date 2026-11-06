#pragma once

#include "arena.h"
#include "error.h"
#include "export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DOM API.
 *
 * `jsoc_parse` validates and tokenizes the input, then materializes a compact
 * tree in the supplied arena.  All strings remain zero-copy slices of the
 * caller's input until explicitly decoded; numbers keep their original lexeme
 * and are converted on demand.  Nothing needs to be freed separately.
 */

typedef enum jsoc_value_type : uint8_t {
  JSOC_VALUE_NULL = 0,
  JSOC_VALUE_FALSE,
  JSOC_VALUE_TRUE,
  JSOC_VALUE_NUMBER,
  JSOC_VALUE_STRING,
  JSOC_VALUE_ARRAY,
  JSOC_VALUE_OBJECT,
} jsoc_value_type;

typedef struct jsoc_value jsoc_value;
typedef struct jsoc_member jsoc_member;

/* A non-owning byte slice.  `data` is not NUL-terminated unless it came from
 * `jsoc_value_get_string` / `jsoc_object_get_string`, which decode and
 * NUL-terminate into the document arena. */
typedef struct {
  const char *data;
  size_t length;
} jsoc_string;

typedef struct {
  jsoc_error error;

  /* Root value, or NULL when `error.code != JSOC_OK`. */
  const jsoc_value *root;

  /* The input backing every raw slice.  Must outlive the document. */
  const char *input;
  size_t length;

  /* Arena owning the tree (and any decoded strings). */
  jsoc_arena *arena;

  /* Private implementation fields. */
  jsoc_value *values;
  size_t value_count;
  jsoc_member *members;
  size_t member_count;
  void *string_cache;
} jsoc_document;

/* Parse one JSON document.  Trailing non-whitespace is an error. */
JSOC_API jsoc_document jsoc_parse(const char *input, size_t length,
                                  jsoc_arena *arena) JSOC_NODISCARD;

/* Read `path` into the arena, then parse it. */
JSOC_API jsoc_document jsoc_parse_file(const char *path,
                                       jsoc_arena *arena) JSOC_NODISCARD;

/*
 * Streaming (NDJSON / concatenated documents).  `callback` is invoked once per
 * top-level value, in order.  The arena is reset before each document, so a
 * document and its decoded strings are only valid for the duration of its
 * callback.  Return false from the callback to stop; the function then returns
 * JSOC_ERR_USER_STOP.  A NULL callback validates the stream only.
 */
typedef bool (*jsoc_document_callback)(const jsoc_document *doc, void *user);
JSOC_API jsoc_error jsoc_parse_many(const char *input, size_t length,
                                    jsoc_arena *arena,
                                    jsoc_document_callback callback,
                                    void *user) JSOC_NODISCARD;
JSOC_API jsoc_error jsoc_parse_many_file(const char *path, jsoc_arena *arena,
                                         jsoc_document_callback callback,
                                         void *user) JSOC_NODISCARD;

/* --- Value inspection ---------------------------------------------------- */

JSOC_API jsoc_value_type jsoc_value_type_of(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_null(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_bool(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_number(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_string(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_array(const jsoc_value *v) JSOC_CONST;
JSOC_API bool jsoc_value_is_object(const jsoc_value *v) JSOC_CONST;

/* Raw scalar lexeme (number) or raw string contents (between the quotes,
 * escapes not decoded).  Arrays/objects return {NULL, 0}. */
JSOC_API jsoc_string jsoc_value_raw(const jsoc_document *doc,
                                    const jsoc_value *v) JSOC_PURE;

/* Quote-free, escape-decoded, NUL-terminated string; cached per value.
 * Returns {NULL, 0} for non-strings. */
JSOC_API jsoc_string jsoc_value_get_string(const jsoc_document *doc,
                                           const jsoc_value *v);

/* Boolean value.  Returns false for non-booleans (and writes false). */
JSOC_API bool jsoc_value_get_bool(const jsoc_value *v, bool *out);

/* Numbers.  Integer getters require an integer lexeme (no fraction/exponent)
 * that fits the requested type.  The double getter accepts every valid JSON
 * number; values outside the finite double range fail. */
JSOC_API bool jsoc_value_get_int64(const jsoc_document *doc,
                                   const jsoc_value *v, int64_t *out);
JSOC_API bool jsoc_value_get_uint64(const jsoc_document *doc,
                                    const jsoc_value *v, uint64_t *out);
JSOC_API bool jsoc_value_get_double(const jsoc_document *doc,
                                    const jsoc_value *v, double *out);
JSOC_API bool jsoc_value_is_integer(const jsoc_document *doc,
                                    const jsoc_value *v) JSOC_PURE;

/* --- Arrays -------------------------------------------------------------- */

JSOC_API size_t jsoc_array_size(const jsoc_document *doc, const jsoc_value *v);
JSOC_API const jsoc_value *jsoc_array_at(const jsoc_document *doc,
                                         const jsoc_value *v, size_t index);

/* --- Objects ------------------------------------------------------------- */

JSOC_API size_t jsoc_object_size(const jsoc_document *doc, const jsoc_value *v);

/* Key comparison is escape-aware: a member whose key contains escapes is
 * decoded before comparison, so callers always pass the logical key. */
JSOC_API const jsoc_value *jsoc_object_get(const jsoc_document *doc,
                                           const jsoc_value *v, const char *key,
                                           size_t key_len);
JSOC_API const jsoc_value *jsoc_object_get_cstr(const jsoc_document *doc,
                                                const jsoc_value *v,
                                                const char *key);

/* Raw key slice (escapes not decoded). */
JSOC_API jsoc_string jsoc_object_key_at(const jsoc_document *doc,
                                        const jsoc_value *v, size_t index);

/* Decoded, NUL-terminated key; cached like jsoc_value_get_string. */
JSOC_API jsoc_string jsoc_object_get_key(const jsoc_document *doc,
                                         const jsoc_value *v, size_t index);

typedef struct {
  const jsoc_document *doc;
  const jsoc_value *object;
  uint32_t member;
} jsoc_object_iter;

JSOC_API void jsoc_object_iter_init(const jsoc_document *doc,
                                    const jsoc_value *v, jsoc_object_iter *it);
JSOC_API bool jsoc_object_iter_next(jsoc_object_iter *it, jsoc_string *key,
                                    const jsoc_value **value);

/* --- UTF-8 --------------------------------------------------------------- */

/* Validate UTF-8, setting *error_offset (when non-NULL) to the first bad byte.
 * Returns true when the whole buffer is valid. */
JSOC_API bool jsoc_validate_utf8(const char *input, size_t length,
                                 size_t *error_offset);

#ifdef __cplusplus
}
#endif
