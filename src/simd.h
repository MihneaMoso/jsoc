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

#endif /* JSOC_SIMD_AVX2 */
