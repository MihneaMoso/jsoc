#pragma once

#include "arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JSON text writer.
 *
 * Buffers grow from a `jsoc_arena`, so nothing is freed separately and the
 * finished text stays valid until the arena is reset or destroyed.  Growth is
 * an append: the old buffer is left in the arena (an arena never reuses or
 * moves bytes), so prefer a good initial size - see `jsoc_minify`, which
 * preallocates from the input length.
 *
 * The writer emits structural punctuation, commas and colons itself.  Scalars
 * are escaped for a `string` value: `"` and `\` become `\"`/`\\`, control bytes
 * become `\b`/`\f`/`\n`/`\r`/`\t` or `\u00XX`, and everything else (including
 * validated UTF-8) passes through unchanged.  Raw JSON fragments never need
 * escaping and can be written with `jsoc_writer_raw`.
 */

#define JSOC_WRITER_MAX_DEPTH 64

typedef struct {
  char *buffer;
  size_t length;
  size_t capacity;
  jsoc_arena *arena;
  bool failed;

  /* Structural framing state. */
  uint32_t depth;
  bool in_object[JSOC_WRITER_MAX_DEPTH];
  bool nonempty[JSOC_WRITER_MAX_DEPTH];
  /* Set while the value half of an object member is still due: the element
   * separator has already been written with its key. */
  bool after_key[JSOC_WRITER_MAX_DEPTH];
} jsoc_writer;

JSOC_API void jsoc_writer_init(jsoc_writer *w, jsoc_arena *arena, size_t hint);
JSOC_API void jsoc_writer_init_fixed(jsoc_writer *w, char *buffer,
                                     size_t capacity);

/* The finished text.  Empty (and `failed`) writers return {NULL, 0}. */
JSOC_API const char *jsoc_writer_data(const jsoc_writer *w) JSOC_PURE;
JSOC_API size_t jsoc_writer_size(const jsoc_writer *w) JSOC_PURE;

JSOC_API bool jsoc_writer_raw(jsoc_writer *w, const char *data,
                              size_t len) JSOC_NODISCARD;

/* Write raw JSON text as a *value*: the separator is applied first, and `data`
 * is written verbatim.  Use this for slices that are already valid JSON (a
 * number lexeme, or an already-quoted string), never for framing. */
JSOC_API bool jsoc_writer_value_raw(jsoc_writer *w, const char *data,
                                    size_t len) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_cstr(jsoc_writer *w, const char *data) JSOC_NODISCARD;

/* --- Writing JSON values -------------------------------------------------- */

JSOC_API bool jsoc_writer_string(jsoc_writer *w, const char *data,
                                 size_t len) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_string_cstr(jsoc_writer *w,
                                      const char *data) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_int64(jsoc_writer *w, int64_t value) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_uint64(jsoc_writer *w, uint64_t value) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_double(jsoc_writer *w, double value) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_bool(jsoc_writer *w, bool value) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_null(jsoc_writer *w) JSOC_NODISCARD;

/* --- Framing -------------------------------------------------------------- */

JSOC_API bool jsoc_writer_array_begin(jsoc_writer *w) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_array_end(jsoc_writer *w) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_object_begin(jsoc_writer *w) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_object_end(jsoc_writer *w) JSOC_NODISCARD;

/* Object key: separator, quoted and escaped key, then ':'. */
JSOC_API bool jsoc_writer_object_key(jsoc_writer *w, const char *data,
                                     size_t len) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_object_key_cstr(jsoc_writer *w,
                                          const char *data) JSOC_NODISCARD;

/* Same, but `data` is written verbatim (no escaping).  Use this for slices
 * that are already a JSON string body, e.g. a document's raw key. */
JSOC_API bool jsoc_writer_object_raw_key(jsoc_writer *w, const char *data,
                                         size_t len) JSOC_NODISCARD;
JSOC_API bool jsoc_writer_object_raw_key_cstr(jsoc_writer *w,
                                              const char *data) JSOC_NODISCARD;

#ifdef __cplusplus
}
#endif
