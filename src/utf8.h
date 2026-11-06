#pragma once

#include "simd.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * UTF-8 validation.
 *
 * The vector path is the "lookup4" algorithm used by simdjson (Apache-2.0),
 * adapted to plain AVX2 intrinsics.  It validates 32 bytes at a time and
 * carries the previous chunk so multibyte sequences may span chunks.  The
 * scalar path is the oracle: it is also used to find the exact byte offset of
 * the first error, which is cold.
 */

static bool jsoc_utf8_validate_scalar(const uint8_t *p, size_t n, size_t *error_offset) {
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }

        size_t need;
        uint8_t lo = 0x80;
        uint8_t hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            need = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            need = 2;
            if (c == 0xE0) lo = 0xA0;
            if (c == 0xED) hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            need = 3;
            if (c == 0xF0) lo = 0x90;
            if (c == 0xF4) hi = 0x8F;
        } else {
            if (error_offset) *error_offset = i;
            return false;
        }

        if (i + need >= n) {
            /* Not enough bytes left for this sequence. */
            if (error_offset) *error_offset = i;
            return false;
        }
        for (size_t k = 1; k <= need; k++) {
            const uint8_t b = p[i + k];
            if (b < lo || b > hi) {
                if (error_offset) *error_offset = i + k;
                return false;
            }
            lo = 0x80;
            hi = 0xBF;
        }
        i += need + 1;
    }
    return true;
}

#if JSOC_SIMD_AVX2

enum {
    UTF8_TOO_SHORT = 1u << 0,
    UTF8_TOO_LONG = 1u << 1,
    UTF8_OVERLONG_3 = 1u << 2,
    UTF8_TOO_LARGE = 1u << 3,
    UTF8_SURROGATE = 1u << 4,
    UTF8_OVERLONG_2 = 1u << 5,
    UTF8_TOO_LARGE_1000 = 1u << 6,
    UTF8_OVERLONG_4 = 1u << 6,
    UTF8_TWO_CONTS = 1u << 7,
    UTF8_CARRY = UTF8_TOO_SHORT | UTF8_TOO_LONG | UTF8_TWO_CONTS,
};

static inline __m256i jsoc_utf8_lookup16(__m256i table, __m256i nibbles) {
    return _mm256_shuffle_epi8(table, nibbles);
}

static inline __m256i jsoc_utf8_high_nibble(__m256i v) {
    return _mm256_and_si256(_mm256_srli_epi16(v, 4), _mm256_set1_epi8(0x0F));
}

static inline __m256i jsoc_utf8_prev(__m256i input, __m256i prev, int n) {
    return _mm256_alignr_epi8(input, _mm256_permute2x128_si256(prev, input, 0x21),
                              (int)(16 - n));
}

static inline __m256i jsoc_utf8_repeat16(uint8_t v0, uint8_t v1, uint8_t v2, uint8_t v3,
                                         uint8_t v4, uint8_t v5, uint8_t v6, uint8_t v7,
                                         uint8_t v8, uint8_t v9, uint8_t v10, uint8_t v11,
                                         uint8_t v12, uint8_t v13, uint8_t v14, uint8_t v15) {
    return _mm256_setr_epi8(
        (char)v0, (char)v1, (char)v2, (char)v3, (char)v4, (char)v5, (char)v6, (char)v7,
        (char)v8, (char)v9, (char)v10, (char)v11, (char)v12, (char)v13, (char)v14, (char)v15,
        (char)v0, (char)v1, (char)v2, (char)v3, (char)v4, (char)v5, (char)v6, (char)v7,
        (char)v8, (char)v9, (char)v10, (char)v11, (char)v12, (char)v13, (char)v14, (char)v15);
}

static inline __m256i jsoc_utf8_special_cases(__m256i input, __m256i prev1) {
    const __m256i byte_1_high = jsoc_utf8_lookup16(
        jsoc_utf8_repeat16(
            UTF8_TOO_LONG, UTF8_TOO_LONG, UTF8_TOO_LONG, UTF8_TOO_LONG,
            UTF8_TOO_LONG, UTF8_TOO_LONG, UTF8_TOO_LONG, UTF8_TOO_LONG,
            UTF8_TWO_CONTS, UTF8_TWO_CONTS, UTF8_TWO_CONTS, UTF8_TWO_CONTS,
            UTF8_TOO_SHORT | UTF8_OVERLONG_2,
            UTF8_TOO_SHORT,
            UTF8_TOO_SHORT | UTF8_OVERLONG_3 | UTF8_SURROGATE,
            UTF8_TOO_SHORT | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000 | UTF8_OVERLONG_4),
        jsoc_utf8_high_nibble(prev1));

    const __m256i byte_1_low = jsoc_utf8_lookup16(
        jsoc_utf8_repeat16(
            (uint8_t)(UTF8_CARRY | UTF8_OVERLONG_3 | UTF8_OVERLONG_2 | UTF8_OVERLONG_4),
            (uint8_t)(UTF8_CARRY | UTF8_OVERLONG_2),
            UTF8_CARRY, UTF8_CARRY,
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000 | UTF8_SURROGATE),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000),
            (uint8_t)(UTF8_CARRY | UTF8_TOO_LARGE | UTF8_TOO_LARGE_1000)),
        _mm256_and_si256(prev1, _mm256_set1_epi8(0x0F)));

    const __m256i byte_2_high = jsoc_utf8_lookup16(
        jsoc_utf8_repeat16(
            UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT,
            UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT,
            (uint8_t)(UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS |
                      UTF8_OVERLONG_3 | UTF8_TOO_LARGE_1000 | UTF8_OVERLONG_4),
            (uint8_t)(UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS |
                      UTF8_OVERLONG_3 | UTF8_TOO_LARGE),
            (uint8_t)(UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS |
                      UTF8_SURROGATE | UTF8_TOO_LARGE),
            (uint8_t)(UTF8_TOO_LONG | UTF8_OVERLONG_2 | UTF8_TWO_CONTS |
                      UTF8_SURROGATE | UTF8_TOO_LARGE),
            UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT, UTF8_TOO_SHORT),
        jsoc_utf8_high_nibble(input));

    return _mm256_and_si256(_mm256_and_si256(byte_1_high, byte_1_low), byte_2_high);
}

static inline __m256i jsoc_utf8_check_lengths(__m256i input, __m256i prev_input,
                                              __m256i sc) {
    const __m256i prev2 = jsoc_utf8_prev(input, prev_input, 2);
    const __m256i prev3 = jsoc_utf8_prev(input, prev_input, 3);
    const __m256i must23 =
        _mm256_or_si256(_mm256_subs_epu8(prev2, _mm256_set1_epi8((char)0x60)),
                        _mm256_subs_epu8(prev3, _mm256_set1_epi8((char)0x70)));
    const __m256i must23_80 = _mm256_and_si256(must23, _mm256_set1_epi8((char)0x80));
    return _mm256_xor_si256(must23_80, sc);
}

static inline __m256i jsoc_utf8_incomplete(__m256i input) {
    static const uint8_t max_array[32] = {
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 255, 255, 255,
        255, 255, 255, 255, 255, 0xEF, 0xDF, 0xBF,
    };
    const __m256i maxv = _mm256_loadu_si256((const __m256i *)(const void *)max_array);
    /* simdjson's gt_bits is an *unsigned* saturating subtraction.  A signed
     * compare would classify ASCII bytes as "greater" than the 0xBF maximum
     * and report a bogus incomplete sequence at the end of every block. */
    return _mm256_subs_epu8(input, maxv);
}

typedef struct {
    __m256i error;
    __m256i prev_input;
    __m256i prev_incomplete;
} jsoc_utf8_state;

static inline void jsoc_utf8_state_init(jsoc_utf8_state *s) {
    s->error = _mm256_setzero_si256();
    s->prev_input = _mm256_setzero_si256();
    s->prev_incomplete = _mm256_setzero_si256();
}

static inline void jsoc_utf8_check_chunk(jsoc_utf8_state *s, __m256i input) {
    if (JSOC_LIKELY(_mm256_movemask_epi8(input) == 0)) {
        /* An all-ASCII chunk terminates any pending sequence (that is an
         * error) and resets the carry state for the next chunk.  simdjson can
         * skip the reset because it only takes this shortcut for a whole
         * 64-byte block; we process 32-byte chunks, so the stale block must
         * not leak into the next chunk. */
        s->error = _mm256_or_si256(s->error, s->prev_incomplete);
        s->prev_incomplete = _mm256_setzero_si256();
        s->prev_input = input;
        return;
    }
    const __m256i prev1 = jsoc_utf8_prev(input, s->prev_input, 1);
    const __m256i sc = jsoc_utf8_special_cases(input, prev1);
    s->error = _mm256_or_si256(s->error, jsoc_utf8_check_lengths(input, s->prev_input, sc));
    s->prev_incomplete = jsoc_utf8_incomplete(input);
    s->prev_input = input;
}

static inline bool jsoc_utf8_valid_avx2(const char *input, size_t length) {
    const uint8_t *p = (const uint8_t *)input;
    jsoc_utf8_state s;
    jsoc_utf8_state_init(&s);

    size_t i = 0;
    for (; i + 32 <= length; i += 32) {
        jsoc_utf8_check_chunk(&s, _mm256_loadu_si256((const __m256i *)(const void *)(p + i)));
    }
    if (i < length) {
        uint8_t tail[32];
        memset(tail, ' ', sizeof tail);
        memcpy(tail, p + i, length - i);
        jsoc_utf8_check_chunk(&s, _mm256_loadu_si256((const __m256i *)(const void *)tail));
    }
    s.error = _mm256_or_si256(s.error, s.prev_incomplete);
    return _mm256_testz_si256(s.error, s.error) != 0;
}

#endif /* JSOC_SIMD_AVX2 */

static inline bool jsoc_utf8_valid(const char *input, size_t length, size_t *error_offset) {
#if JSOC_SIMD_AVX2
    if (jsoc_utf8_valid_avx2(input, length)) return true;
    return jsoc_utf8_validate_scalar((const uint8_t *)input, length, error_offset);
#else
    return jsoc_utf8_validate_scalar((const uint8_t *)input, length, error_offset);
#endif
}
