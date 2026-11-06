#pragma once

#include "dom.h"
#include "jsoc.h" /* jsoc_decode_result, jsoc_decode_error */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef enum {
  JSOC_DECODE_BOOL,
  JSOC_DECODE_INT64,
  JSOC_DECODE_UINT64,
  JSOC_DECODE_DOUBLE,
  JSOC_DECODE_STRING_REF,   /* copies a decoded, NUL-terminated string */
  JSOC_DECODE_STRING_SLICE, /* stores the raw slice (no copy) */
  JSOC_DECODE_STRUCT,
  JSOC_DECODE_ARRAY,
} jsoc_decode_kind;

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

/* Maximum fields per struct (one bool per field is tracked while decoding). */
#define JSOC_DECODE_MAX_FIELDS 64

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

/* False when the value is an object, true when it is an array. */
JSOC_API bool jsoc_decode_value_is_array(const jsoc_value *v) JSOC_CONST;

/* Array helper: returns the element at `index`, or NULL. */
JSOC_API const jsoc_value *jsoc_decode_array_at(const jsoc_document *doc,
                                                const jsoc_value *array,
                                                size_t index);

#ifdef __cplusplus
}
#endif
