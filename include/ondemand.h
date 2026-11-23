#pragma once

/*
 * On-demand typed decoding.
 *
 * `jsoc_parse` materialises a full DOM before a caller can decode a struct.
 * That costs allocations and a second walk of every value even when the
 * struct only needs a handful of fields.  The on-demand path instead scans
 * the input once into a structural index array (stage 1) and then walks that
 * array directly, skipping values the struct does not name.  It is the C
 * counterpart of simdjson's On-Demand `iterate().get<T>()`.
 *
 * A `jsoc_ondemand` borrows the caller's input buffer; it must stay alive for
 * as long as any decoded slice is used.  The structural indexes live in an
 * arena owned by the caller.  Decoded (escaped) strings are copied into a
 * separate arena passed to `jsoc_decode_ondemand`.
 *
 * Typical use:
 *
 *     jsoc_arena *a = jsoc_arena_create(length + 65536);
 *     jsoc_ondemand od;
 *     jsoc_error err;
 *     if (jsoc_ondemand_scan(&od, input, length, a, &err)) {
 *       jsoc_decode_result r = jsoc_decode_ondemand(person_fields, 2, &od,
 *                                                   &person, a);
 *     }
 */

#include "decode.h"
#include "error.h"
#include "export.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  const char *input;  /* borrowed; must outlive every decoded slice */
  uint32_t length;
  uint32_t *indexes;  /* stage-1 structural indexes, owned by `arena` */
  size_t count;
} jsoc_ondemand;

/* Scan `input` into structural indexes.  UTF-8 and string escapes are
 * validated, exactly as in `jsoc_parse`'s stage 1.  Returns false on any
 * irregularity; `*err` then receives an error describing it (the exact
 * code/offset/line/column is recovered by replaying `jsoc_parse`). */
JSOC_API bool jsoc_ondemand_scan(jsoc_ondemand *od, const char *input,
                                 size_t length, jsoc_arena *arena,
                                 jsoc_error *err) JSOC_NODISCARD;

/* Decode the string value at index position `value_pos` into `arena`,
 * unescaping it and NUL-terminating the result.  `value_pos` is a position in
 * `od->indexes` (the value positions used internally by the on-demand walk).
 * Returns a NULL slice on failure. */
JSOC_API jsoc_string jsoc_ondemand_string(const jsoc_ondemand *od,
                                          size_t value_pos,
                                          jsoc_arena *arena) JSOC_NODISCARD;

/* Decode the root value (an object) into `out`, described by `fields` (a
 * NULL-terminated table, as for `jsoc_decode_object`).  No DOM is built.
 * Escape-free strings are copied into `arena`; escaped strings are decoded
 * there.  `count` is the number of usable entries in `fields`. */
JSOC_API jsoc_decode_result
jsoc_decode_ondemand(const jsoc_decode_field *fields, size_t count,
                     const jsoc_ondemand *od, void *out,
                     jsoc_arena *arena);

#ifdef __cplusplus
}
#endif
