#pragma once

#include "token.h"
#include "dom.h"
#include "decode_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JSOC_API const char *jsoc_decode_error_message(jsoc_decode_error code) JSOC_CONST JSOC_NODISCARD;
#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reflected decoding.
 *
 * A struct type is described by a table of `jsoc_decode_field` entries, one per
 * JSON member.  A decode walks the document, matches keys against the table,
 * and stores values at `offsetof` positions:
 *
 *     struct person { int64_t id; char *name; size_t name_len; };
 *
 *     static const jsoc_decode_field person_fields[] = {
 *         JSOC_DECODE_FIELD_A(person, id,   JSOC_INT64,  .required = true),
 *         JSOC_DECODE_FIELD_A(person, name, JSOC_STRING_REF),
 *         { .name = NULL },
 *     };
 *
 *     struct person p;
 *     jsoc_decode_result r = jsoc_decode_object(person_fields, 2, &doc, root,
 * &p);
 *
 * Scalars are stored with the number/boolean getters, so the same range and
 * integer/literal rules apply: `"id": "42"` does not decode, and `1e999`
 * overflows.
 *
 * Array members write into a caller-supplied buffer described by the field's
 * `items`/`item_kind` and by the `count_offset` destination, which holds the
 * buffer's capacity on entry and the element count on return.
 */

typedef struct jsoc_decode_field jsoc_decode_field;

struct jsoc_decode_field {
  const char *name;

  jsoc_decode_kind kind;
  bool required;

  /* offsetof of the destination member. */
  size_t offset;

  /* STRUCT: the nested object's descriptor (terminated by a NULL name). */
  const jsoc_decode_field *nested;

  /* ARRAY: destination pointer, capacity/count, element kind and stride. */
  size_t count_offset;
  jsoc_decode_kind item_kind;
  size_t item_size;
  const jsoc_decode_field *items; /* ARRAY of STRUCT */
};

/* Convenience for table entries.  Named after the destination member, which
 * must be spelled exactly like its JSON key:
 *
 *     static const jsoc_decode_field person_fields[] = {
 *         JSOC_DECODE_FIELD_A(person, id,   JSOC_INT64,  .required = true),
 *         JSOC_DECODE_FIELD_A(person, name, JSOC_STRING_REF),
 *         { .name = NULL }   // required terminator
 *     };
 */
/* NB: the kind parameter must not be spelled like the designator. */
#define JSOC_DECODE_FIELD_A(obj, member, knd, ...)                             \
  {.name = #member, .kind = (knd), .offset = offsetof(struct obj, member), ##__VA_ARGS__}

/* Decode a JSON object into a struct described by `fields` (which must end
 * with an entry whose `name` is NULL). */
JSOC_API jsoc_decode_result jsoc_decode_object(const jsoc_decode_field *fields,
                                               size_t field_count,
                                               const jsoc_document *doc,
                                               const jsoc_value *object,
                                               void *out);

/* Decode any value into `out` according to `kind`.  Containers return a type
 * mismatch: use `jsoc_decode_object` for objects and the array helpers for
 * arrays. */
JSOC_API jsoc_decode_result jsoc_decode_scalar(jsoc_decode_kind kind,
                                               const jsoc_document *doc,
                                               const jsoc_value *v, void *out);

/* Decode an array of scalars/structs. */
JSOC_API jsoc_decode_result jsoc_decode_array(const jsoc_decode_field *field,
                                              size_t field_count,
                                              const jsoc_document *doc,
                                              const jsoc_value *array,
                                              void *out);

JSOC_API const char *jsoc_decode_error_message(jsoc_decode_error code) JSOC_CONST JSOC_NODISCARD;
#ifdef __cplusplus
}
#endif
