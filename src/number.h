#pragma once

#include "arena.h"
#include "export.h"
#include "simd.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GLIBC__) || defined(__APPLE__) || defined(__FreeBSD__)
#  define JSOC_HAVE_STRTOD_L 1
#  include <locale.h>
#  if defined(__APPLE__) || defined(__FreeBSD__)
#    include <xlocale.h>
#  endif
#endif

/*
 * Number helpers shared by the tokenizer's checked mode and the DOM.
 *
 * A validated JSON number lexeme is either an integer (it must fit in
 * int64/uint64) or a fractional/exponent form (it must fit in a finite
 * double).  The tokenizer records the lexeme's shape while it scans it, so
 * the common case needs no second pass over the digits.
 */

/* A validated number lexeme has no fraction and no exponent. */
static inline bool jsoc_number_is_integer(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '.' || p[i] == 'e' || p[i] == 'E') return false;
    }
    return true;
}

/* Parse a validated integer lexeme into a magnitude.  Returns false on
 * uint64 overflow. */
static inline bool jsoc_number_integer_magnitude(const char *p, size_t n,
                                                 bool *negative,
                                                 uint64_t *magnitude) {
    size_t i = 0;
    *negative = false;
    if (n != 0 && p[0] == '-') {
        *negative = true;
        i = 1;
    }
    uint64_t mag = 0;
    for (; i < n; i++) {
        const unsigned d = (unsigned)(unsigned char)p[i] - (unsigned)'0';
        if (JSOC_UNLIKELY(d > 9)) return false;
        if (JSOC_UNLIKELY(mag > (UINT64_MAX - d) / 10)) return false;
        mag = mag * 10 + d;
    }
    *magnitude = mag;
    return true;
}

#if JSOC_HAVE_STRTOD_L
static inline locale_t jsoc_c_locale(void) {
    static locale_t loc;
    if (loc == (locale_t)0) {
        loc = newlocale(LC_ALL_MASK, "C", (locale_t)0);
    }
    return loc;
}
#endif

/* Character classes shared by the number scanners. */
static inline bool jsoc_is_digit(unsigned char c) {
    return (unsigned)(c - '0') < 10u;
}

static inline bool jsoc_is_hex_digit(unsigned char c) {
    return (unsigned)(c - '0') < 10u || (unsigned)(c - 'a') < 6u ||
           (unsigned)(c - 'A') < 6u;
}

/* Convert a lexeme to a double using the C locale.  `arena` supplies storage
 * for lexemes longer than the stack buffer. */
static inline bool jsoc_lexeme_to_double(jsoc_arena *arena, const char *p,
                                         size_t n, double *out) {
    char stack[128];
    char *buf;
    if (n < sizeof stack) {
        buf = stack;
    } else {
        buf = jsoc_arena_alloc(arena, n + 1, 1);
        if (!buf) return false;
    }
    memcpy(buf, p, n);
    buf[n] = '\0';

    char *end = NULL;
    double value;
#if JSOC_HAVE_STRTOD_L
    locale_t loc = jsoc_c_locale();
    value = loc ? strtod_l(buf, &end, loc) : strtod(buf, &end);
#else
    value = strtod(buf, &end);
#endif
    if (end != buf + n || !isfinite(value)) return false;
    if (out) *out = value;
    return true;
}

/* Checked variant: identical syntax and error offsets, plus a direct
 * representability verdict.  The lexeme shape stays in locals instead of a
 * struct so the DOM's number check does not add stores per number. */
static inline JSOC_ALWAYS_INLINE jsoc_error_code jsoc_validate_number_checked(
    const char *in, size_t len, size_t start, jsoc_arena *arena, size_t *end,
    size_t *err_off, bool *out_of_range) {
  size_t i = start;
  bool negative = false;
  if (in[i] == '-') {
    negative = true;
    i++;
  }

  if (JSOC_UNLIKELY(i >= len || !jsoc_is_digit((unsigned char)in[i]))) {
    *err_off = i;
    return JSOC_ERR_INVALID_NUMBER;
  }

  const size_t int_start = i;
  size_t int_digits;
  bool int_nonzero;
  if (in[i] == '0') {
    int_nonzero = false;
    int_digits = 1;
    i++;
    /* JSON forbids leading zeroes (01, -012). */
    if (JSOC_UNLIKELY(i < len && jsoc_is_digit((unsigned char)in[i]))) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
  } else {
    int_nonzero = true;
    const size_t run = jsoc_digit_run(in, len, i);
    int_digits = run;
    i += run;
  }

  bool has_fraction = false;
  size_t frac_start = 0;
  size_t frac_digits = 0;
  if (i < len && in[i] == '.') {
    has_fraction = true;
    i++;
    const size_t run = jsoc_digit_run(in, len, i);
    if (JSOC_UNLIKELY(run == 0)) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
    frac_start = i;
    frac_digits = run;
    i += run;
  }

  bool has_exponent = false;
  long explicit_exponent = 0;
  if (i < len && (in[i] == 'e' || in[i] == 'E')) {
    has_exponent = true;
    i++;
    int sign = 1;
    if (i < len && (in[i] == '+' || in[i] == '-')) {
      if (in[i] == '-')
        sign = -1;
      i++;
    }
    const size_t exp_start = i;
    const size_t run = jsoc_digit_run(in, len, i);
    if (JSOC_UNLIKELY(run == 0)) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
    long e = 0;
    for (size_t k = exp_start; k < i + run; k++) {
      if (e < 100000)
        e = e * 10 + (in[k] - '0');
    }
    explicit_exponent = sign * e;
    i += run;
  }

  *end = i;

  /* simdjson-compatible limits: integer lexemes must fit int64/uint64;
   * fractional/exponent forms must fit a finite double. */
  bool bad;
  if (!has_fraction && !has_exponent) {
    /* JSON forbids leading zeroes, so digit count decides most lexemes. */
    if (!negative) {
      bad = int_digits > 20 ||
            (int_digits == 20 &&
             memcmp(in + int_start, "18446744073709551615", 20) > 0);
    } else {
      bad = int_digits > 19 ||
            (int_digits == 19 &&
             memcmp(in + int_start, "9223372036854775808", 19) > 0);
    }
  } else {
    long magnitude;
    if (int_nonzero) {
      magnitude = (long)int_digits - 1 + explicit_exponent;
    } else {
      /* The integer part is all zero: the first nonzero fraction digit
       * fixes the exponent. */
      size_t first_significant = SIZE_MAX;
      for (size_t j = 0; j < frac_digits; j++) {
        if (in[frac_start + j] != '0') {
          first_significant = j;
          break;
        }
      }
      if (first_significant == SIZE_MAX) {
        *out_of_range = false; /* zero */
        return JSOC_OK;
      }
      magnitude = -(long)first_significant - 1 + explicit_exponent;
    }
    if (magnitude >= 309) {
      bad = true;
    } else if (magnitude <= 307) {
      bad = false;
    } else {
      /* 1e308 <= |value| < 1e309: DBL_MAX lives in this band. */
      bad = !jsoc_lexeme_to_double(arena, in + start, i - start, NULL);
    }
  }

  *out_of_range = bad;
  return JSOC_OK;
}
