/**
 * Portable SIMD primitives for the jsoc tokenizer.
 *
 * Two operations cover almost all of the hot scanning work:
 *   - skipping JSON whitespace (space, tab, LF, CR)
 *   - finding the next character that terminates or interrupts a string
 *     (the closing quote, a backslash, or an unescaped control byte)
 *
 * Every implementation here is *pure*: it takes (ptr, len, index) and
 * returns an index. No tokenizer state is mutated, which keeps line/column
 * bookkeeping in exactly one place and makes the scalar fallback trivial
 * to validate against the SIMD paths.
 *
 * Design notes:
 *   - Real JSON has very short strings and short whitespace runs, so the
 *     inlined fast path is scalar. It probes at most JSOC_SCALAR_PROBE
 *     bytes; only longer runs call the out-of-line vector helpers.
 *   - The vector helpers are deliberately not inlined.  Inlining them at
 *     every token-boundary call site bloats the hot loop and measurably
 *     slows down the common (short-run) case, even though the vector
 *     instructions are never executed.
 *
 * Define JSOC_NO_SIMD to force the portable byte loop.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#if !defined(JSOC_NO_SIMD) && (defined(__GNUC__) || defined(__clang__))
#  if defined(__AVX2__)
#    include <immintrin.h>
#    define JSOC_SIMD_AVX2 1
#  elif defined(__SSE2__)
#    include <emmintrin.h>
#    define JSOC_SIMD_SSE2 1
#  endif
#endif

#ifndef JSOC_SIMD_AVX2
#  define JSOC_SIMD_AVX2 0
#endif
#ifndef JSOC_SIMD_SSE2
#  define JSOC_SIMD_SSE2 0
#endif

/* Bytes handled by the inlined scalar probe before vectors kick in. */
#define JSOC_SCALAR_PROBE 16

static inline int jsoc_is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ------------------------------------------------------------------------- */
/* Out-of-line vector helpers (long runs only)                               */
/* ------------------------------------------------------------------------- */

static JSOC_NOINLINE size_t jsoc_skip_ws_long(const char *p, size_t len, size_t i) {
#if JSOC_SIMD_AVX2
    const __m256i sp = _mm256_set1_epi8(' ');
    const __m256i tb = _mm256_set1_epi8('\t');
    const __m256i lf = _mm256_set1_epi8('\n');
    const __m256i cr = _mm256_set1_epi8('\r');
    while (i + 32 <= len) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
        __m256i m = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, sp), _mm256_cmpeq_epi8(v, tb)),
            _mm256_or_si256(_mm256_cmpeq_epi8(v, lf), _mm256_cmpeq_epi8(v, cr)));
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
        if (mask != 0xFFFFFFFFu) {
            /* ~mask != 0 because mask is not all ones. */
            return i + (size_t)__builtin_ctzll(~(uint64_t)mask);
        }
        i += 32;
    }
#elif JSOC_SIMD_SSE2
    const __m128i sp = _mm_set1_epi8(' ');
    const __m128i tb = _mm_set1_epi8('\t');
    const __m128i lf = _mm_set1_epi8('\n');
    const __m128i cr = _mm_set1_epi8('\r');
    while (i + 16 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
        __m128i m = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, sp), _mm_cmpeq_epi8(v, tb)),
            _mm_or_si128(_mm_cmpeq_epi8(v, lf), _mm_cmpeq_epi8(v, cr)));
        uint32_t mask = (uint32_t)(uint16_t)_mm_movemask_epi8(m);
        if (mask != 0xFFFFu) {
            return i + (size_t)__builtin_ctzll(~(uint64_t)mask);
        }
        i += 16;
    }
#endif
    while (i < len && jsoc_is_ws((unsigned char)p[i])) i++;
    return i;
}

static JSOC_NOINLINE size_t jsoc_find_string_special_long(const char *p, size_t len, size_t i) {
#if JSOC_SIMD_AVX2
    const __m256i dq = _mm256_set1_epi8('"');
    const __m256i bs = _mm256_set1_epi8('\\');
    const __m256i ctl = _mm256_set1_epi8(0x1F);
    while (i + 32 <= len) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
        /* Unsigned "v <= 0x1F": max(v, 0x1F) == 0x1F.  A signed compare
         * would misclassify bytes >= 0x80 (valid UTF-8 data). */
        __m256i is_ctl = _mm256_cmpeq_epi8(_mm256_max_epu8(v, ctl), ctl);
        __m256i m = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, dq), _mm256_cmpeq_epi8(v, bs)),
            is_ctl);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(m);
        if (mask != 0) return i + (size_t)__builtin_ctz(mask);
        i += 32;
    }
#elif JSOC_SIMD_SSE2
    const __m128i dq = _mm_set1_epi8('"');
    const __m128i bs = _mm_set1_epi8('\\');
    const __m128i ctl = _mm_set1_epi8(0x1F);
    while (i + 16 <= len) {
        __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
        __m128i is_ctl = _mm_cmpeq_epi8(_mm_max_epu8(v, ctl), ctl);
        __m128i m = _mm_or_si128(
            _mm_or_si128(_mm_cmpeq_epi8(v, dq), _mm_cmpeq_epi8(v, bs)),
            is_ctl);
        uint32_t mask = (uint32_t)(uint16_t)_mm_movemask_epi8(m);
        if (mask != 0) return i + (size_t)__builtin_ctz(mask);
        i += 16;
    }
#endif
    while (i < len) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\' || c < 0x20) return i;
        i++;
    }
    return len;
}

/* ------------------------------------------------------------------------- */
/* Inlined hot paths                                                         */
/* ------------------------------------------------------------------------- */

/* Index of the first byte at or after `i` that is not JSON whitespace,
 * or `len` if the remainder is all whitespace. `i` must be <= len. */
static inline size_t jsoc_skip_ws(const char *p, size_t len, size_t i) {
    if (i >= len) return len;

    /* Overwhelmingly common: no whitespace at all. */
    if (!jsoc_is_ws((unsigned char)p[i])) return i;

    size_t limit = (len - i < JSOC_SCALAR_PROBE) ? len : i + JSOC_SCALAR_PROBE;
    while (i < limit && jsoc_is_ws((unsigned char)p[i])) i++;
    if (i < limit) return i;   /* hit a non-whitespace byte inside probe */
    if (i >= len) return len;  /* the whole remainder was whitespace */
    return jsoc_skip_ws_long(p, len, i);
}

/* Index of the first byte at or after `i` that is `"`, `\` or a raw control
 * character (< 0x20), or `len` if none is present. */
static inline size_t jsoc_find_string_special(const char *p, size_t len, size_t i) {
    if (i >= len) return len;

    size_t limit = (len - i < JSOC_SCALAR_PROBE) ? len : i + JSOC_SCALAR_PROBE;
    while (i < limit) {
        unsigned char c = (unsigned char)p[i];
        if (c == '"' || c == '\\' || c < 0x20) return i;
        i++;
    }
    if (i >= len) return len;
    return jsoc_find_string_special_long(p, len, i);
}
