#pragma once

/*
 * Scalar formatting shared by the writer, the encoder and the serializer.
 * Internal: not installable, no stable ABI.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 00, 01, ... 99 */
static const char jsoc_digit_pairs[201] =
    "00010203040506070809"
    "10111213141516171819"
    "20212223242526272829"
    "30313233343536373839"
    "40414243444546474849"
    "50515253545556575859"
    "60616263646566676869"
    "70717273747576777879"
    "80818283848586878889"
    "90919293949596979899";

/* Same, but the second byte of a single-digit entry is NUL.  Writing it and
 * then advancing by one byte overwrites the NUL with the next output, which
 * is how leading zeros are suppressed without a branch. */
static const char jsoc_first_digits[201] = "0\0"
                                           "1\0"
                                           "2\0"
                                           "3\0"
                                           "4\0"
                                           "5\0"
                                           "6\0"
                                           "7\0"
                                           "8\0"
                                           "9\0"
                                           "10111213141516171819202122232425262728293031323334353637383940414243444546474849"
                                           "50515253545556575859606162636465666768697071727374757677787980818283848586878889"
                                           "90919293949596979899";

/* Integer to decimal: James Edward Anhalt III's algorithm, as used by
 * simdjson.  Every digit pair comes from one multiply-and-shift by a magic
 * reciprocal, so there is no division and no per-call table warm-up; digits
 * are written straight into the caller's buffer, two bytes at a time.
 *
 * Callers must reserve 21 bytes: the last write of a short number can spill
 * one byte past the returned end (it is always overwritten). */

/* Multiply-shift reciprocals, rounded up exactly as the algorithm requires. */
#define JSOC_J1 167773u        /* 10 * 2^24 / 1e3 + 1 */
#define JSOC_J2 429497u        /* 10 * 2^32 / 1e5 + 1 */
#define JSOC_J3 281474977u     /* 10 * 2^48 / 1e7 + 1 */
#define JSOC_J4 281474977u     /* 2^48 / 1e6 + 1 */
#define JSOC_J5 1441151881u    /* 10 * 2^57 / 1e9 + 1 */

static inline void jsoc_put_dd(char *p, uint64_t k) {
  memcpy(p, jsoc_digit_pairs + 2 * k, 2);
}
static inline void jsoc_put_fd(char *p, uint64_t k) {
  memcpy(p, jsoc_first_digits + 2 * k, 2);
}

/* n < 10^8: writes 1 to 8 digits. */
static inline char *jsoc_u64_lt8(char *b, uint32_t n) {
  const uint64_t mask24 = (UINT64_C(1) << 24) - 1;
  const uint64_t mask32 = (UINT64_C(1) << 32) - 1;
  if (n < 100) {
    jsoc_put_fd(b, n);
    return n < 10 ? b + 1 : b + 2;
  }
  if (n < 1000000) {
    if (n < 10000) {
      const uint32_t f0 = JSOC_J1 * n;
      jsoc_put_fd(b, f0 >> 24);
      b -= n < 1000;
      const uint32_t f2 = (uint32_t)(f0 & mask24) * 100;
      jsoc_put_dd(b + 2, f2 >> 24);
      return b + 4;
    }
    const uint64_t f0 = (uint64_t)JSOC_J2 * n;
    jsoc_put_fd(b, f0 >> 32);
    b -= n < 100000;
    const uint64_t f2 = (f0 & mask32) * 100;
    jsoc_put_dd(b + 2, f2 >> 32);
    const uint64_t f4 = (f2 & mask32) * 100;
    jsoc_put_dd(b + 4, f4 >> 32);
    return b + 6;
  }
  const uint64_t f0 = (uint64_t)JSOC_J3 * n >> 16;
  jsoc_put_fd(b, f0 >> 32);
  b -= n < 10000000;
  const uint64_t f2 = (f0 & mask32) * 100;
  jsoc_put_dd(b + 2, f2 >> 32);
  const uint64_t f4 = (f2 & mask32) * 100;
  jsoc_put_dd(b + 4, f4 >> 32);
  const uint64_t f6 = (f4 & mask32) * 100;
  jsoc_put_dd(b + 6, f6 >> 32);
  return b + 8;
}

/* z < 10^8: always writes exactly 8 digits. */
static inline char *jsoc_u64_8(char *b, uint32_t z) {
  const uint64_t mask32 = (UINT64_C(1) << 32) - 1;
  const uint64_t f0 = ((uint64_t)JSOC_J4 * z >> 16) + 1;
  jsoc_put_dd(b, f0 >> 32);
  const uint64_t f2 = (f0 & mask32) * 100;
  jsoc_put_dd(b + 2, f2 >> 32);
  const uint64_t f4 = (f2 & mask32) * 100;
  jsoc_put_dd(b + 4, f4 >> 32);
  const uint64_t f6 = (f4 & mask32) * 100;
  jsoc_put_dd(b + 6, f6 >> 32);
  return b + 8;
}

/* 10^8 <= n < 2^32: writes 9 or 10 digits. */
static inline char *jsoc_u64_9_10(char *b, uint64_t n) {
  const uint64_t mask57 = (UINT64_C(1) << 57) - 1;
  const uint64_t f0 = (uint64_t)JSOC_J5 * n;
  jsoc_put_fd(b, f0 >> 57);
  b -= n < 1000000000;
  const uint64_t f2 = (f0 & mask57) * 100;
  jsoc_put_dd(b + 2, f2 >> 57);
  const uint64_t f4 = (f2 & mask57) * 100;
  jsoc_put_dd(b + 4, f4 >> 57);
  const uint64_t f6 = (f4 & mask57) * 100;
  jsoc_put_dd(b + 6, f6 >> 57);
  const uint64_t f8 = (f6 & mask57) * 100;
  jsoc_put_dd(b + 8, f8 >> 57);
  return b + 10;
}

/* Write the decimal digits of `n` at `b`; returns one past the last digit.
 * Requires 21 writable bytes at `b`. */
static inline char *jsoc_u64_write(char *b, uint64_t n) {
  if (n < 100000000ull)
    return jsoc_u64_lt8(b, (uint32_t)n);
  if (n < (UINT64_C(1) << 32))
    return jsoc_u64_9_10(b, n);
  const uint32_t z = (uint32_t)(n % 100000000ull);
  uint64_t u = n / 100000000ull;
  if (u < 100000000ull) {
    b = jsoc_u64_lt8(b, (uint32_t)u);
  } else if (u < (UINT64_C(1) << 32)) {
    b = jsoc_u64_9_10(b, u);
  } else {
    const uint32_t y = (uint32_t)(u % 100000000ull);
    u /= 100000000ull;
    b = jsoc_u64_lt8(b, (uint32_t)u);
    b = jsoc_u64_8(b, y);
  }
  return jsoc_u64_8(b, z);
}

/* Shortest `%g` spelling that reads back bit-identically, in the C locale.
 * `buf` must hold 32 bytes; returns the length, or 0 on failure. */
static inline size_t jsoc_double_text(double value, char *buf) {
  if (value != value || value > 1.7976931348623157e308 ||
      value < -1.7976931348623157e308) {
    memcpy(buf, "null", 4);
    return 4;
  }
  size_t n = 0;
  for (int precision = 1; precision <= 17; precision++) {
    const int written = snprintf(buf, 32, "%.*g", precision, value);
    if (written < 0 || written >= 32)
      return 0;
    n = (size_t)written;
    if (strtod(buf, NULL) == value)
      break;
    if (precision == 17)
      return 0;
  }
  /* Never emit a locale decimal comma. */
  for (size_t i = 0; i < n; i++) {
    if (buf[i] == ',')
      buf[i] = '.';
  }
  return n;
}
