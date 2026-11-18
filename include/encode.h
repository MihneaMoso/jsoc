#pragma once

#include "arena.h"
#include "dom.h" /* jsoc_string */
#include "export.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Descriptor-driven serialization.
 *
 * The mirror image of `decode.h`: a struct is described by a table of fields
 * and the encoder writes a JSON object from it.  A struct can therefore use
 * one table for decoding and one for encoding, with the same member names.
 *
 *     static const jsoc_encode_field person_fields[] = {
 *         JSOC_ENCODE_FIELD_A(person, id,   JSOC_ENCODE_INT64),
 *         JSOC_ENCODE_FIELD_A(person, name, JSOC_ENCODE_STRING),
 *         { .name = NULL },   // required terminator
 *     };
 *
 *     jsoc_string out;
 *     if (!jsoc_encode_object(person_fields, 2, &p, arena, &out)) ...
 *
 * Strings come in two flavours.  `JSOC_ENCODE_STRING` escapes a decoded
 * byte slice.  `JSOC_ENCODE_STRING_RAW` writes a slice copied from a
 * validated document (e.g. a `JSOC_DECODE_STRING_SLICE` member) verbatim
 * between quotes: the tokenizer already proved every escape and control byte,
 * so a zero-copy decode followed by this kind is a straight `memcpy` round
 * trip.
 *
 * The encoder makes one size pass and one write pass, into a single arena
 * allocation.  Pass `hint` to `jsoc_encode_object_hinted` when the caller
 * already knows a safe upper bound; that skips the size pass.  `hint` must be
 * at least as large as the encoded text, otherwise the function fails without
 * writing a partial result.
 */

typedef enum {
  JSOC_ENCODE_NULL,
  JSOC_ENCODE_BOOL,
  JSOC_ENCODE_INT64,
  JSOC_ENCODE_UINT64,
  JSOC_ENCODE_DOUBLE,
  JSOC_ENCODE_STRING,     /* jsoc_string: escape the contents */
  JSOC_ENCODE_STRING_RAW, /* jsoc_string: contents are a valid JSON body */
  JSOC_ENCODE_CSTR,       /* const char *: escape */
  JSOC_ENCODE_RAW,        /* jsoc_string: a complete JSON value */
  JSOC_ENCODE_STRUCT,
  JSOC_ENCODE_ARRAY,
} jsoc_encode_kind;

typedef struct jsoc_encode_field jsoc_encode_field;

struct jsoc_encode_field {
  /* Hot fields: the write loop touches these in descriptor order. */
  char key[32]; /* `,"name":`, zero padded (see JSOC_ENCODE_FIELD_A) */
  size_t key_length;
  size_t offset;
  jsoc_encode_kind kind;

  const char *name;
  size_t name_length;

  /* STRUCT: nested descriptor (terminated by a NULL name). */
  const jsoc_encode_field *nested;

  /* ARRAY: element count, element kind, stride and (for STRUCT) descriptor. */
  size_t count_offset;
  jsoc_encode_kind item_kind;
  size_t item_size;
  const jsoc_encode_field *items;
  size_t max_items; /* safety bound for a hostile/corrupt count */
};

/* The key is stored inline and zero padded so the writer can copy a fixed
 * 32 bytes (two vector stores) without reading past the descriptor: member
 * names therefore have a compile-time length limit.  The leading comma is
 * dropped for the first member of an object. */
#define JSOC_ENCODE_FIELD_A(obj, member, knd, ...)                            \
  {.key = ",\"" #member "\":", .key_length = sizeof(",\"" #member "\":") - 1, \
   .offset = offsetof(struct obj, member), .kind = (knd), .name = #member,    \
   .name_length = sizeof(#member) - 1, ##__VA_ARGS__}

#define JSOC_ENCODE_MAX_FIELDS 4096

/* Encode `object` as a JSON object.  `field_count` may be the number of used
 * entries; a NULL-named entry also terminates the table. */
JSOC_API bool jsoc_encode_object(const jsoc_encode_field *fields,
                                 size_t field_count, const void *object,
                                 jsoc_arena *arena,
                                 jsoc_string *out) JSOC_NODISCARD;

/* Same, but the caller guarantees `hint` bytes are enough and the size pass
 * is skipped. */
JSOC_API bool jsoc_encode_object_hinted(const jsoc_encode_field *fields,
                                        size_t field_count,
                                        const void *object, size_t hint,
                                        jsoc_arena *arena,
                                        jsoc_string *out) JSOC_NODISCARD;

/* Encode just one value described by `field`, read from `container`. */
JSOC_API bool jsoc_encode_value(const jsoc_encode_field *field,
                                const void *container, jsoc_arena *arena,
                                jsoc_string *out) JSOC_NODISCARD;

/* Exact number of bytes the encoder will write. */
JSOC_API size_t jsoc_encode_size(const jsoc_encode_field *fields,
                                 size_t field_count,
                                 const void *object) JSOC_PURE;

#ifdef __cplusplus
}
#endif
