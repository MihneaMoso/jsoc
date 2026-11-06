#pragma once

#include "arena.h"
#include "export.h"

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
