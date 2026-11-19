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
#include <string.h>

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

#if defined(__PCLMUL__)
#  define JSOC_SIMD_PCLMUL 1
#else
#  define JSOC_SIMD_PCLMUL 0
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

/* Copy `n` bytes from `s` to `d`; returns `d + n`.
 *
 * This is the serializer's memcpy.  A document is mostly many medium-length
 * strings, so the libc call and its size dispatch are a large share of the
 * work.  The 32/16/8-byte paths are overlap-correct - the second store of a
 * pair ends exactly at d + n - so they never touch a byte outside the
 * caller's buffer even though they copy unaligned. */
static inline JSOC_ALWAYS_INLINE char *jsoc_copy_out(char *d, const char *s,
                                                     size_t n) {
#if JSOC_SIMD_AVX2
  if (n >= 32) {
    size_t i = 0;
    do {
      _mm256_storeu_si256(
          (__m256i *)(void *)(d + i),
          _mm256_loadu_si256((const __m256i *)(const void *)(s + i)));
      i += 32;
    } while (i + 32 <= n);
    _mm256_storeu_si256(
        (__m256i *)(void *)(d + n - 32),
        _mm256_loadu_si256((const __m256i *)(const void *)(s + n - 32)));
    return d + n;
  }
  if (n >= 16) {
    _mm_storeu_si128((__m128i *)(void *)d,
                     _mm_loadu_si128((const __m128i *)(const void *)s));
    _mm_storeu_si128((__m128i *)(void *)(d + n - 16),
                     _mm_loadu_si128((const __m128i *)(const void *)(s + n - 16)));
    return d + n;
  }
#endif
  if (n >= 8) {
    uint64_t a, b;
    memcpy(&a, s, 8);
    memcpy(&b, s + n - 8, 8);
    memcpy(d, &a, 8);
    memcpy(d + n - 8, &b, 8);
  } else if (n >= 4) {
    uint32_t a, b;
    memcpy(&a, s, 4);
    memcpy(&b, s + n - 4, 4);
    memcpy(d, &a, 4);
    memcpy(d + n - 4, &b, 4);
  } else if (n >= 2) {
    uint16_t a, b;
    memcpy(&a, s, 2);
    memcpy(&b, s + n - 2, 2);
    memcpy(d, &a, 2);
    memcpy(d + n - 2, &b, 2);
  } else if (n != 0) {
    *d = *s;
  }
  return d + n;
}

/* Number of consecutive ASCII digits starting at p[i], capped at len - i. */
static inline JSOC_ALWAYS_INLINE size_t jsoc_digit_run(const char *p, size_t len, size_t i) {
    const size_t start = i;
#if JSOC_SIMD_AVX2 || JSOC_SIMD_SSE2
    while (i + 16 <= len) {
        const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
        const __m128i x = _mm_sub_epi8(v, _mm_set1_epi8('0'));
        const __m128i ok = _mm_cmpeq_epi8(_mm_subs_epu8(x, _mm_set1_epi8(9)),
                                          _mm_setzero_si128());
        const unsigned mask = (unsigned)(uint16_t)_mm_movemask_epi8(ok);
        if (mask != 0xFFFFu) {
            return (i - start) + (size_t)__builtin_ctz(~mask & 0xFFFFu);
        }
        i += 16;
    }
#endif
    while (i < len) {
        const unsigned d = (unsigned)(unsigned char)p[i] - (unsigned)'0';
        if (d > 9) break;
        i++;
    }
    return i - start;
}

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

/* ------------------------------------------------------------------------- */
/* Stage 1: 64-byte structural classification                                */
/* ------------------------------------------------------------------------- */

/* State carried between 64-byte blocks by the structural scanner. */
typedef struct {
    uint64_t next_is_escaped;
    uint64_t prev_in_string;
    uint64_t prev_scalar;
} jsoc_stage1_state;

/* Per-block masks.  All are bit-per-byte, bit 0 = first byte of the block. */
typedef struct {
    uint64_t whitespace;
    uint64_t op;       /* { } [ ] : , (may contain 0x0C/0x1A false positives) */
    uint64_t quote;    /* raw '"' bytes, including escaped ones */
    uint64_t backslash;
    uint64_t control;  /* bytes <= 0x1F */
    uint64_t nonascii; /* non-zero when any byte has the high bit set */
} jsoc_block_masks;

#if JSOC_SIMD_AVX2

static inline JSOC_ALWAYS_INLINE uint64_t jsoc_mask64(uint32_t lo, uint32_t hi) {
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/* Classify an already-loaded 64-byte block (two 32-byte halves). */
static inline JSOC_ALWAYS_INLINE jsoc_block_masks jsoc_classify64_vec(__m256i lo,
                                                                     __m256i hi) {
    /* Byte values < 0x80 map exactly through a 16-entry low-nibble table.
     * The table stores the byte itself for whitespace, so the compare below
     * is an exact byte match (0x30 etc. do not collide with ' '). */
    const __m256i ws_table = _mm256_setr_epi8(
        ' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100,
        ' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100);

    /* Turn '[' into '{' and ']' into '}' with | 0x20.  Their low nibbles are
     * unique among the six operators, so one shuffle + compare finds all of
     * them.  Bytes >= 0x80 make vpshufb produce zero, which cannot match. */
    const __m256i op_table = _mm256_setr_epi8(
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0);

    const __m256i lower = _mm256_set1_epi8(0x20);
    const __m256i quote_ch = _mm256_set1_epi8('"');
    const __m256i slash_ch = _mm256_set1_epi8('\\');
    const __m256i ctl_ch = _mm256_set1_epi8(0x1F);

    const __m256i wsl = _mm256_cmpeq_epi8(lo, _mm256_shuffle_epi8(ws_table, lo));
    const __m256i wsh = _mm256_cmpeq_epi8(hi, _mm256_shuffle_epi8(ws_table, hi));

    const __m256i curl_lo = _mm256_or_si256(lo, lower);
    const __m256i curl_hi = _mm256_or_si256(hi, lower);
    const __m256i opl = _mm256_cmpeq_epi8(curl_lo, _mm256_shuffle_epi8(op_table, lo));
    const __m256i oph = _mm256_cmpeq_epi8(curl_hi, _mm256_shuffle_epi8(op_table, hi));

    const __m256i ql = _mm256_cmpeq_epi8(lo, quote_ch);
    const __m256i qh = _mm256_cmpeq_epi8(hi, quote_ch);
    const __m256i bl = _mm256_cmpeq_epi8(lo, slash_ch);
    const __m256i bh = _mm256_cmpeq_epi8(hi, slash_ch);
    const __m256i cl = _mm256_cmpeq_epi8(_mm256_max_epu8(lo, ctl_ch), ctl_ch);
    const __m256i ch = _mm256_cmpeq_epi8(_mm256_max_epu8(hi, ctl_ch), ctl_ch);

    jsoc_block_masks m;
    m.whitespace = jsoc_mask64((uint32_t)_mm256_movemask_epi8(wsl),
                               (uint32_t)_mm256_movemask_epi8(wsh));
    m.op         = jsoc_mask64((uint32_t)_mm256_movemask_epi8(opl),
                               (uint32_t)_mm256_movemask_epi8(oph));
    m.quote      = jsoc_mask64((uint32_t)_mm256_movemask_epi8(ql),
                               (uint32_t)_mm256_movemask_epi8(qh));
    m.backslash  = jsoc_mask64((uint32_t)_mm256_movemask_epi8(bl),
                               (uint32_t)_mm256_movemask_epi8(bh));
    m.control    = jsoc_mask64((uint32_t)_mm256_movemask_epi8(cl),
                               (uint32_t)_mm256_movemask_epi8(ch));
    m.nonascii   = (uint64_t)(_mm256_movemask_epi8(
                                    _mm256_or_si256(lo, hi)) != 0);
    return m;
}

/* Classify a 64-byte block.  `p` may be unaligned; it must be readable for
 * 64 bytes (the caller copies the final short tail into a padded buffer). */
static inline JSOC_ALWAYS_INLINE jsoc_block_masks jsoc_classify64(const uint8_t *p) {
    return jsoc_classify64_vec(
        _mm256_loadu_si256((const __m256i *)(const void *)p),
        _mm256_loadu_si256((const __m256i *)(const void *)(p + 32)));
}

/* Cumulative bitwise XOR: flips every bit from each set bit onward.
 * Used to turn real-quote positions into an "inside string" mask. */
static inline JSOC_ALWAYS_INLINE uint64_t jsoc_prefix_xor(uint64_t bits) {
#if JSOC_SIMD_PCLMUL
    const __m128i all_ones = _mm_set1_epi8((char)0xFF);
    const __m128i v = _mm_set_epi64x(0, (long long)bits);
    return (uint64_t)_mm_cvtsi128_si64(_mm_clmulepi64_si128(v, all_ones, 0));
#else
    bits ^= bits << 1;
    bits ^= bits << 2;
    bits ^= bits << 4;
    bits ^= bits << 8;
    bits ^= bits << 16;
    bits ^= bits << 32;
    return bits;
#endif
}

/* Return the mask of characters escaped by a backslash (the character after
 * an odd-length backslash run), carrying a trailing escape across blocks. */
static inline JSOC_ALWAYS_INLINE uint64_t jsoc_escape_next(uint64_t backslash,
                                                            uint64_t *next_is_escaped) {
    const uint64_t odd_bits = 0xAAAAAAAAAAAAAAAAULL;
    if (backslash == 0) {
        uint64_t escaped = *next_is_escaped;
        *next_is_escaped = 0;
        return escaped;
    }
    const uint64_t potential = backslash & ~*next_is_escaped;
    const uint64_t maybe_escaped = potential << 1;
    const uint64_t code = ((maybe_escaped | odd_bits) - potential) ^ odd_bits;
    const uint64_t escaped = code ^ (backslash | *next_is_escaped);
    *next_is_escaped = (code & backslash) >> 63;
    return escaped;
}

/* ------------------------------------------------------------------------- */
/* AVX2 compress: remove bytes where mask bit = 1 (whitespace).
 * Based on simdjson's implementation.
 * ------------------------------------------------------------------------- */

#if JSOC_SIMD_AVX2

/* 256 * 8 bytes = 2kB, easily fits in cache. */
static const uint64_t jsoc_thintable_epi8[256] = {
    0x0706050403020100, 0x0007060504030201, 0x0007060504030200,
    0x0000070605040302, 0x0007060504030100, 0x0000070605040301,
    0x0000070605040300, 0x0000000706050403, 0x0007060504020100,
    0x0000070605040201, 0x0000070605040200, 0x0000000706050402,
    0x0000070605040100, 0x0000000706050401, 0x0000000706050400,
    0x0000000007060504, 0x0007060503020100, 0x0000070605030201,
    0x0000070605030200, 0x0000000706050302, 0x0000070605030100,
    0x0000000706050301, 0x0000000706050300, 0x0000000007060503,
    0x0000070605020100, 0x0000000706050201, 0x0000000706050200,
    0x0000000007060502, 0x0000000706050100, 0x0000000007060501,
    0x0000000007060500, 0x0000000000070605, 0x0007060403020100,
    0x0000070604030201, 0x0000070604030200, 0x0000000706040302,
    0x0000070604030100, 0x0000000706040301, 0x0000000706040300,
    0x0000000007060403, 0x0000070604020100, 0x0000000706040201,
    0x0000000706040200, 0x0000000007060402, 0x0000000706040100,
    0x0000000007060401, 0x0000000007060400, 0x0000000000070604,
    0x0000070603020100, 0x0000000706030201, 0x0000000706030200,
    0x0000000007060302, 0x0000000706030100, 0x0000000007060301,
    0x0000000007060300, 0x0000000000070603, 0x00000706020100,
    0x00000007060201, 0x00000007060200, 0x00000000070602,
    0x00000007060100, 0x00000000070601, 0x00000000070600,
    0x0000000000000706, 0x0007050403020100, 0x0000070504030201,
    0x0000070504030200, 0x0000000705040302, 0x0000070504030100,
    0x0000000705040301, 0x0000000705040300, 0x0000000007050403,
    0x0000070504020100, 0x0000000705040201, 0x0000000705040200,
    0x0000000007050402, 0x0000000705040100, 0x0000000007050401,
    0x0000000007050400, 0x0000000000070504, 0x0000070503020100,
    0x0000000705030201, 0x0000000705030200, 0x0000000007050302,
    0x0000000705030100, 0x0000000007050301, 0x0000000007050300,
    0x0000000000070503, 0x0000000705020100, 0x0000000007050201,
    0x00000007050200, 0x00000000070502, 0x00000007050100,
    0x0000000000070501, 0x0000000000070500, 0x0000000000000705,
    0x0000070403020100, 0x0000000704030201, 0x0000000704030200,
    0x0000000007040302, 0x0000000704030100, 0x0000000007040301,
    0x00000007040300, 0x0000000000070403, 0x0000000704020100,
    0x0000000007040201, 0x0000000007040200, 0x0000000000070402,
    0x0000000007040100, 0x0000000000070401, 0x0000000000070400,
    0x0000000000000704, 0x0000000703020100, 0x0000000007030201,
    0x0000000007030200, 0x0000000000070302, 0x0000000007030100,
    0x0000000000070301, 0x0000000000070300, 0x0000000000000703,
    0x00000007020100, 0x00000000070201, 0x00000000070200,
    0x00000000000702, 0x0000000000070100, 0x0000000000000701,
    0x0000000000000700, 0x0000000000000007, 0x0006050403020100,
    0x0000060504030201, 0x0000060504030200, 0x0000000605040302,
    0x0000060504030100, 0x0000000605040301, 0x0000000605040300,
    0x0000000006050403, 0x0000060504020100, 0x0000000605040201,
    0x0000000605040200, 0x0000000006050402, 0x0000000605040100,
    0x0000000006050401, 0x0000000006050400, 0x0000000000060504,
    0x0000060503020100, 0x0000000605030201, 0x0000000605030200,
    0x0000000006050302, 0x0000000605030100, 0x0000000006050301,
    0x0000000006050300, 0x0000000000060503, 0x0000000605020100,
    0x0000000006050201, 0x0000000006050200, 0x0000000000060502,
    0x0000000006050100, 0x0000000000060501, 0x0000000000060500,
    0x0000000000000605, 0x0000060403020100, 0x0000000604030201,
    0x0000000604030200, 0x0000000006040302, 0x0000000604030100,
    0x0000000006040301, 0x0000000006040300, 0x0000000000060403,
    0x0000000604020100, 0x0000000006040201, 0x0000000006040200,
    0x0000000000060402, 0x0000000006040100, 0x0000000000060401,
    0x0000000000060400, 0x0000000000000604, 0x0000000603020100,
    0x0000000006030201, 0x0000000006030200, 0x0000000000060302,
    0x0000000006030100, 0x0000000000060301, 0x0000000000060300,
    0x0000000000000603, 0x0000000006020100, 0x0000000000060201,
    0x0000000000060200, 0x0000000000000602, 0x0000000000060100,
    0x0000000000000601, 0x0000000000000600, 0x0000000000000006,
    0x0000050403020100, 0x0000000504030201, 0x0000000504030200,
    0x0000000005040302, 0x0000000504030100, 0x0000000005040301,
    0x0000000005040300, 0x0000000000050403, 0x0000000504020100,
    0x0000000005040201, 0x0000000005040200, 0x0000000000050402,
    0x0000000005040100, 0x0000000000050401, 0x0000000000050400,
    0x0000000000000504, 0x0000000503020100, 0x0000000005030201,
    0x0000000005030200, 0x0000000000050302, 0x0000000005030100,
    0x0000000000050301, 0x0000000000050300, 0x0000000000000503,
    0x0000000005020100, 0x0000000000050201, 0x0000000000050200,
    0x0000000000000502, 0x0000000000050100, 0x0000000000000501,
    0x0000000000000500, 0x0000000000000005, 0x0000000403020100,
    0x0000000004030201, 0x0000000004030200, 0x0000000000040302,
    0x0000000004030100, 0x0000000000040301, 0x0000000000040300,
    0x0000000000000403, 0x0000000004020100, 0x0000000000040201,
    0x0000000000040200, 0x0000000000000402, 0x0000000000040100,
    0x0000000000000401, 0x0000000000000400, 0x0000000000000004,
    0x0000000003020100, 0x0000000000030201, 0x0000000000030200,
    0x0000000000000302, 0x0000000000030100, 0x0000000000000301,
    0x0000000000000300, 0x0000000000000003, 0x0000000000020100,
    0x0000000000000201, 0x0000000000000200, 0x0000000000000002,
    0x0000000000000100, 0x0000000000000001, 0x0000000000000000,
    0x0000000000000000,
};

/* Popcount * 2 for 8-bit values (0..255). */
static const unsigned char jsoc_bits_set_table256_mul2[256] = {
    0,  2,  2,  4,  2,  4,  4,  6,  2,  4,  4,  6,  4,  6,  6,  8,
    2,  4,  4,  6,  4,  6,  6,  8,  4,  6,  6,  8,  6,  8,  8,  10,
    2,  4,  4,  6,  4,  6,  6,  8,  4,  6,  6,  8,  6,  8,  8,  10,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    2,  4,  4,  6,  4,  6,  6,  8,  4,  6,  6,  8,  6,  8,  8,  10,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    6,  8,  8,  10, 8,  10, 10, 12, 8,  10, 10, 12, 10, 12, 12, 14,
    2,  4,  4,  6,  4,  6,  6,  8,  4,  6,  6,  8,  6,  8,  8,  10,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    6,  8,  8,  10, 8,  10, 10, 12, 8,  10, 10, 12, 10, 12, 12, 14,
    4,  6,  6,  8,  6,  8,  8,  10, 6,  8,  8,  10, 8,  10, 10, 12,
    6,  8,  8,  10, 8,  10, 10, 12, 8,  10, 10, 12, 10, 12, 12, 14,
    6,  8,  8,  10, 8,  10, 10, 12, 8,  10, 10, 12, 10, 12, 12, 14,
    8,  10, 10, 12, 10, 12, 12, 14, 10, 12, 12, 14, 12, 14, 14, 16};

/* Combine table for the second shuffle.
 * 17 entries (0..16 ones) * 16 bytes = 272 bytes. */
static const uint8_t jsoc_pshufb_combine_table[272] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
    0x0f, 0xff, 0xff, 0xff, 0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x02, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x00, 0x01, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0x00, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
    0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

/* Compress 32 bytes: remove bytes where mask bit = 1.
 * `src` must point to 32 readable bytes. `dst` must have 32 writable bytes.
 * Returns the number of bytes written (0..32). */
static inline size_t jsoc_compress32_avx2(const uint8_t *src, uint32_t mask,
                                          uint8_t *dst) {
  __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)src);

  uint8_t m1 = (uint8_t)mask;
  uint8_t m2 = (uint8_t)(mask >> 8);
  uint8_t m3 = (uint8_t)(mask >> 16);
  uint8_t m4 = (uint8_t)(mask >> 24);

  __m256i shufmask = _mm256_set_epi64x(
      (long long)jsoc_thintable_epi8[m4], (long long)jsoc_thintable_epi8[m3],
      (long long)jsoc_thintable_epi8[m2], (long long)jsoc_thintable_epi8[m1]);

  shufmask = _mm256_add_epi8(
      shufmask,
      _mm256_set_epi32(0x18181818, 0x18181818,
                       0x10101010, 0x10101010,
                       0x08080808, 0x08080808,
                       0, 0));

  __m256i pruned = _mm256_shuffle_epi8(v, shufmask);

  int pop1 = jsoc_bits_set_table256_mul2[m1];
  int pop3 = jsoc_bits_set_table256_mul2[m3];

  __m256i v256 = _mm256_castsi128_si256(
      _mm_loadu_si128((const __m128i *)(jsoc_pshufb_combine_table + pop1 * 8)));
  __m256i compactmask = _mm256_insertf128_si256(
      v256,
      _mm_loadu_si128((const __m128i *)(jsoc_pshufb_combine_table + pop3 * 8)),
      1);

  __m256i almost = _mm256_shuffle_epi8(pruned, compactmask);

  __m128i lo = _mm256_castsi256_si128(almost);
  __m128i hi = _mm256_extractf128_si256(almost, 1);

  _mm_storeu_si128((__m128i *)dst, lo);
  _mm_storeu_si128((__m128i *)(dst + 16 - __builtin_popcount(mask & 0xFFFF)), hi);

  return (size_t)(32 - __builtin_popcount(mask));
}

/* Compress 64 bytes by processing two 32-byte halves. */
static inline size_t jsoc_compress64_avx2(const uint8_t *src,
                                          uint32_t mask_lo, uint32_t mask_hi,
                                          uint8_t *dst) {
  size_t n1 = jsoc_compress32_avx2(src, mask_lo, dst);
  size_t n2 = jsoc_compress32_avx2(src + 32, mask_hi, dst + n1);
  return n1 + n2;
}

#endif /* JSOC_SIMD_AVX2 */


#endif /* JSOC_SIMD_AVX2 */
