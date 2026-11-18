/* Common decode types shared by decode.h and decode_direct.h. */

#ifndef JSOC_DECODE_TYPES_H
#define JSOC_DECODE_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  JSOC_DECODE_OK = 0,
  JSOC_DECODE_TYPE_MISMATCH,
  JSOC_DECODE_MISSING_FIELD,
  JSOC_DECODE_EXTRA_FIELD,
  JSOC_DECODE_OVERFLOW,
  JSOC_DECODE_CUSTOM,
  JSOC_DECODE_UNEXPECTED_EOF = 10,
  JSOC_DECODE_DUPLICATE_KEY = 11,
} jsoc_decode_error;

typedef struct {
  jsoc_decode_error code;
  size_t token_index;
  const char *field_name;
} jsoc_decode_result;

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

#define JSOC_DECODE_MAX_FIELDS 64

#ifdef __cplusplus
}
#endif

#endif /* JSOC_DECODE_TYPES_H */
