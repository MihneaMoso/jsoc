#define _GNU_SOURCE 1
#include "number.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

JSOC_API bool jsoc_number_to_int64(const char *p, size_t n, int64_t *out) {
  bool negative;
  uint64_t mag;
  if (!jsoc_number_integer_magnitude(p, n, &negative, &mag))
    return false;
  if (negative) {
    if (mag > ((uint64_t)INT64_MAX + 1u))
      return false;
    *out = (mag == ((uint64_t)INT64_MAX + 1u)) ? INT64_MIN : -(int64_t)mag;
  } else {
    if (mag > (uint64_t)INT64_MAX)
      return false;
    *out = (int64_t)mag;
  }
  return true;
}

JSOC_API bool jsoc_number_to_uint64(const char *p, size_t n, uint64_t *out) {
  bool negative;
  uint64_t mag;
  if (!jsoc_number_integer_magnitude(p, n, &negative, &mag))
    return false;
  if (negative)
    return false;
  *out = mag;
  return true;
}

JSOC_API bool jsoc_number_to_double(const char *p, size_t n, double *out) {
  jsoc_arena *a = jsoc_arena_create(n + 64);
  if (!a)
    return false;
  bool ok = jsoc_lexeme_to_double(a, p, n, out);
  jsoc_arena_destroy(a);
  return ok;
}
