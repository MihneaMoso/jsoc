/*
 * Numbers.
 *
 * Ports the behaviour of simdjson's tests for number handling:
 *   dom/integer_tests.cpp              -> integer and unsigned round-trips
 *   dom/big_integer_tests.cpp          -> lexemes that exceed uint64
 *   dom/nan_inf_tests.cpp              -> NaN/Infinity (jsoc always rejects)
 *   ondemand/ondemand_number_tests.cpp -> powers of two/ten, extremes
 *   ondemand/ondemand_float_tests.cpp  -> subnormals, no double rounding,
 *                                        round-to-even, many digits, round trip
 *   dom/locale_independence_tests.cpp  -> the decimal separator stays '.'
 *   dom/random_string_number_tests.cpp -> deterministic random round-trips
 *
 * jsoc stores the original lexeme and converts on demand, so the shared
 * expectation is "the value read back equals the value the lexeme spells".
 */
#define _GNU_SOURCE /* setlocale, memmem */
#include "test.h"
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool test_powers_of_two(void) {
  /* Every power of two that is a finite double, from the smallest subnormal
   * (2^-1074) to 2^1023, must round-trip exactly. */
  for (int i = -1074; i <= 1023; i++) {
    char json[64];
    /* Printed with max_digits10 precision, the way simdjson does. */
    snprintf(json, sizeof json, "%.*e", 16, ldexp(1.0, i));
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(json, strlen(json), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == ldexp(1.0, i));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_powers_of_ten(void) {
  /* A long sweep of exponents exercises the decimal conversion path. */
  double power = 1e-300;
  for (int i = 0; i < 400; i++) {
    char json[64];
    snprintf(json, sizeof json, "%.*e", 22, power);
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(json, strlen(json), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == power);
    jsoc_arena_destroy(a);
    power *= 10.0;
  }
  return true;
fail:
  return false;
}

static bool test_subnormals(void) {
  /* The smallest subnormal and a few steps around it. */
  const char *cases[][2] = {
      {"5e-324", NULL},
      {"1e-323", NULL},
      {"2.5e-324", NULL},                /* rounds to the smallest subnormal */
      {"2.4703282292062327e-324", NULL}, /* the next one would round up */
      {"4.9406564584124654e-324", NULL}, /* 2 x smallest */
      {"1e-310", NULL},
      {"2.2250738585072011e-308", NULL}, /* smallest normal */
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i][0], strlen(cases[i][0]), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    const double expected = strtod(cases[i][0], NULL);
    REQUIRE(got == expected || (isnan(got) && isnan(expected)));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_no_double_rounding(void) {
  /* Values that a naive two-step conversion (string -> long double ->
   * double) would mis-convert must come back exact. */
  const char *cases[] = {
      "1.7976931348623157e308",    "2.2250738585072014e-308",
      "0.5000000000000001665",     "0.99999999999999988898",
      "1.00000000000000011102",    "3.518437208883201171875e-013",
      "9.109383264802274e-31",     "4.905518220961699e-321",
      "8.3568685333116959565e-65", "1.7976931348623157e308",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == strtod(cases[i], NULL));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_round_to_even(void) {
  /* Ties must resolve to the even last digit, not up. */
  const char *cases[] = {
      "2.5", "3.5", "-2.5", "0.125", "0.135", "0.375", "1.0000000000000002"};
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == strtod(cases[i], NULL));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_many_digits(void) {
  /* 100+ significant digits: everything after the double is discarded, but
   * the rounding must still be exact. */
  const char *cases[] = {
      "1.00000000000000000000000000000000000000000000000000000000000000000"
      "000000000000000000000",
      "3.14159265358979323846264338327950288419716939937510582097494",
      "2.71828182845904523536028747135266249775724709369995957496696",
      "1.7976931348623157"
      "308"
      "0000000000000000000000000000001",
      "1234567890123456789012345678901234567890.12345678901234567890",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == strtod(cases[i], NULL));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_round_trip(void) {
  /* Every double printed with the shortest round-tripping precision must
   * read back identically. */
  jsoc_rng r;
  jsoc_rng_seed(&r, 42);
  for (int i = 0; i < 2000; i++) {
    uint64_t bits = jsoc_rng_next(&r);
    double value;
    memcpy(&value, &bits, sizeof value);
    if (isnan(value) || isinf(value))
      continue;

    char json[64];
    snprintf(json, sizeof json, "%.17g", value);
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(json, strlen(json), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == value);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_small_integers(void) {
  /* Every value in a wide range, positive and negative. */
  for (int64_t i = -2048; i <= 2048; i += 7) {
    char json[32];
    snprintf(json, sizeof json, "%lld", (long long)i);
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(json, strlen(json), a);
    REQUIRE(doc.error.code == JSOC_OK);
    int64_t got = 0;
    REQUIRE(jsoc_value_get_int64(&doc, doc.root, &got));
    REQUIRE(got == i);
    REQUIRE(jsoc_value_is_integer(&doc, doc.root));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_integer_boundaries(void) {
  /* The exact int64/uint64 edges, and one either side of them.  A lexeme
   * that spells an integer beyond uint64 makes `jsoc_parse` fail with
   * JSOC_ERR_NUMBER_OVERFLOW, but the double getter still works, so the
   * "fits" columns only apply where the parse succeeded. */
  const struct {
    const char *lexeme;
    jsoc_error_code parse_error;
    bool fits_i64;
    bool fits_u64;
    int64_t i64;
    uint64_t u64;
  } cases[] = {
      {"0", JSOC_OK, true, true, 0, 0},
      {"-1", JSOC_OK, true, false, -1, 0},
      {"9223372036854775807", JSOC_OK, true, true, INT64_MAX,
       9223372036854775807u},
      {"9223372036854775808", JSOC_OK, false, true, 0, 9223372036854775808u},
      {"-9223372036854775808", JSOC_OK, true, false, INT64_MIN, 0},
      {"18446744073709551615", JSOC_OK, false, true, 0, UINT64_MAX},
      {"10000000000000000000", JSOC_OK, false, true, 0, 10000000000000000000u},
      {"-0", JSOC_OK, true, true, 0, 0},
      {"-9223372036854775809", JSOC_ERR_NUMBER_OVERFLOW, false, false, 0, 0},
      {"18446744073709551616", JSOC_ERR_NUMBER_OVERFLOW, false, false, 0, 0},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i].lexeme, strlen(cases[i].lexeme), a);
    REQUIRE(doc.error.code == cases[i].parse_error);
    if (doc.error.code == JSOC_OK) {
      int64_t i64 = 0;
      uint64_t u64 = 0;
      REQUIRE(jsoc_value_get_int64(&doc, doc.root, &i64) == cases[i].fits_i64);
      if (cases[i].fits_i64)
        REQUIRE(i64 == cases[i].i64);
      REQUIRE(jsoc_value_get_uint64(&doc, doc.root, &u64) == cases[i].fits_u64);
      if (cases[i].fits_u64)
        REQUIRE(u64 == cases[i].u64);
    } else {
      /* The value is still a real double. */
      jsoc_arena_reset(a);
      jsoc_document d2 =
          jsoc_parse(cases[i].lexeme, strlen(cases[i].lexeme), a);
      REQUIRE(d2.error.code == JSOC_ERR_NUMBER_OVERFLOW);
    }
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_big_integer_rejected(void) {
  /* A lexeme that spells a valid integer beyond uint64 is a *range* error,
   * not a syntax error: jsoc reports it out of band so a first-document
   * caller does not fail on trailing values (see dom.c's overflow scope). */
  const char *cases[] = {
      "123456789012345678901",
      "18446744073709551616",
      "18446744073709551617",
      "99999999999999999999999999999999999999999999999999999999999999999",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(doc.error.code == JSOC_ERR_NUMBER_OVERFLOW);
    /* The double getter still works: the value is a real double. */
    jsoc_arena_reset(a);
    jsoc_document d2 = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(d2.error.code == JSOC_ERR_NUMBER_OVERFLOW);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_nan_inf_rejected(void) {
  /* JSON has no spelling for NaN or Infinity.  (simdjson optionally accepts
   * NaN/Infinity when built with SIMDJSON_ENABLE_NAN_INF; jsoc does not,
   * matching the JSON specification.) */
  const char *cases[] = {
      "NaN",        "nan",         "NAN",          "-NaN",     "+NaN",
      "Infinity",   "infinity",    "Infinity",     "INFINITY", "-Infinity",
      "inf",        "-inf",        "Inf",          "-Inf",     "[NaN]",
      "[Infinity]", "[1, inf, 2]", "{\"a\": NaN}", "nulll",    "nul",
      "nAn",        "iNfinity",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i], strlen(cases[i]), a);
    REQUIRE(doc.error.code != JSOC_OK);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_trailing_junk(void) {
  /* simdjson's reject_trailing_junk / reject_similar_prefix /
   * reject_truncated_atoms: junk glued to a literal is an error at the first
   * byte after it. */
  const struct {
    const char *json;
    jsoc_error_code code;
  } cases[] = {
      /* Junk glued to a number or literal is trailing garbage, reported at
       * the first byte after the scalar. */
      {"1abc", JSOC_ERR_TRAILING_GARBAGE},
      {"truex", JSOC_ERR_TRAILING_GARBAGE},
      {"nullnull", JSOC_ERR_TRAILING_GARBAGE},
      {"12x", JSOC_ERR_TRAILING_GARBAGE},
      {"0x10", JSOC_ERR_TRAILING_GARBAGE},
      {"[1]x", JSOC_ERR_TRAILING_GARBAGE},
      {"1,", JSOC_ERR_TRAILING_GARBAGE},
      {"1.2.3", JSOC_ERR_TRAILING_GARBAGE},
      /* Truncated atoms and bytes that cannot start a value. */
      {"nul", JSOC_ERR_EXPECTED_VALUE},
      {"tru", JSOC_ERR_EXPECTED_VALUE},
      {"fals", JSOC_ERR_EXPECTED_VALUE},
      {".5", JSOC_ERR_EXPECTED_VALUE},
      {"+1", JSOC_ERR_EXPECTED_VALUE},
      /* Malformed numbers. */
      {"01", JSOC_ERR_INVALID_NUMBER},
      {"-", JSOC_ERR_INVALID_NUMBER},
      {"1e", JSOC_ERR_INVALID_NUMBER},
      {"1e+", JSOC_ERR_INVALID_NUMBER},
      {"1.", JSOC_ERR_INVALID_NUMBER},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i].json, strlen(cases[i].json), a);
    REQUIRE(doc.error.code == cases[i].code);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_number_in_string(void) {
  /* ondemand/ondemand_number_in_string_tests.cpp: the string and number
   * getters must not be interchangeable. */
  jsoc_arena *a = jsoc_arena_create(1024);
  REQUIRE(a != NULL);
  jsoc_document doc =
      jsoc_parse("{\"s\":\"42\",\"n\":42,\"f\":4.2,\"i\":-7}",
                 strlen("{\"s\":\"42\",\"n\":42,\"f\":4.2,\"i\":-7}"), a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *s = jsoc_object_get_cstr(&doc, doc.root, "s");
  const jsoc_value *n = jsoc_object_get_cstr(&doc, doc.root, "n");
  const jsoc_value *f = jsoc_object_get_cstr(&doc, doc.root, "f");
  const jsoc_value *iv = jsoc_object_get_cstr(&doc, doc.root, "i");
  REQUIRE(s && n && f && iv);

  /* A string is not an integer, even when it looks like one. */
  int64_t i64 = 0;
  REQUIRE(!jsoc_value_get_int64(&doc, s, &i64));
  REQUIRE(!jsoc_value_is_integer(&doc, s));
  jsoc_string raw = jsoc_value_raw(&doc, s);
  REQUIRE(raw.length == 2 && memcmp(raw.data, "42", 2) == 0);

  /* A number has no decoded string form, but its raw lexeme is exact. */
  REQUIRE(jsoc_value_get_string(&doc, n).data == NULL);
  REQUIRE(jsoc_value_raw(&doc, n).length == 2);
  REQUIRE(jsoc_value_get_int64(&doc, n, &i64) && i64 == 42);
  REQUIRE(!jsoc_value_is_integer(&doc, f));
  REQUIRE(jsoc_value_get_int64(&doc, iv, &i64) && i64 == -7);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_locale_independence(void) {
  /* Parsing must accept '.' regardless of LC_NUMERIC, and rejects ',' as a
   * decimal separator.  Skipped when no comma-decimal locale is installed. */
  const char *saved = setlocale(LC_NUMERIC, NULL);
  const char *candidates[] = {"de_DE.UTF-8", "de_DE.utf8",
                              "de-DEU",      "German_Germany.1252",
                              "fr_FR.UTF-8", "French_France.1252"};
  size_t i = 0;
  bool active = false;
  for (; i < sizeof candidates / sizeof candidates[0]; i++) {
    if (setlocale(LC_NUMERIC, candidates[i]) == NULL)
      continue;
    struct lconv *info = localeconv();
    if (info && info->decimal_point && strcmp(info->decimal_point, ",") == 0) {
      active = true;
      break;
    }
  }
  if (active) {
    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    {
      jsoc_document doc = jsoc_parse("1.5", 3, a);
      REQUIRE(doc.error.code == JSOC_OK);
      double got = 0;
      REQUIRE(jsoc_value_get_double(&doc, doc.root, &got) && got == 1.5);
    }
    {
      jsoc_document doc = jsoc_parse("1,5", 3, a);
      REQUIRE(doc.error.code != JSOC_OK);
    }
    /* Serialization must emit '.' too. */
    {
      jsoc_string out;
      REQUIRE(jsoc_minify("[1.5,2.25]", 12, a, &out, &(jsoc_error){0}));
      REQUIRE(out.length == 11);
      REQUIRE(memchr(out.data, ',', out.length) != NULL); /* separator */
      REQUIRE(memmem(out.data, out.length, "1.5", 3) != NULL);
    }
    jsoc_arena_destroy(a);
  } else {
    printf("[SKIP] no comma-decimal locale available ");
    REQUIRE(saved == NULL || setlocale(LC_NUMERIC, saved) != NULL);
  }
  REQUIRE(saved == NULL || setlocale(LC_NUMERIC, saved) != NULL);
  return true;
fail:
  if (saved)
    setlocale(LC_NUMERIC, saved);
  return false;
}

static bool test_random_round_trip(void) {
  /* dom/random_string_number_tests.cpp + the deterministic generator from
   * unpadded_tests: build documents from random strings and numbers, parse
   * them, and compare against printf's own conversion. */
  jsoc_rng r;
  jsoc_rng_seed(&r, 12345);
  char json[256];

  for (int i = 0; i < 3000; i++) {
    const uint32_t pick = jsoc_rng_below(&r, 6);
    if (pick == 0) {
      snprintf(json, sizeof json, "%d",
               (int)(jsoc_rng_next(&r) % 100000) - 50000);
    } else if (pick == 1) {
      snprintf(json, sizeof json, "%.6f",
               (double)(int32_t)jsoc_rng_next(&r) / 1000.0);
    } else if (pick == 2) {
      snprintf(json, sizeof json, "%llu",
               (unsigned long long)(jsoc_rng_next(&r) >> 1));
    } else if (pick == 3) {
      const double d = (double)(int64_t)jsoc_rng_next(&r) / 3.0e9;
      snprintf(json, sizeof json, "%.17g", d);
    } else if (pick == 4) {
      const long e = (long)jsoc_rng_below(&r, 40) - 20;
      snprintf(json, sizeof json, "1e%+ld", e);
    } else {
      snprintf(json, sizeof json, "-0.0");
    }

    jsoc_arena *a = jsoc_arena_create(1024);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(json, strlen(json), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, doc.root, &got));
    REQUIRE(got == strtod(json, NULL));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

int main(void) {
  printf("=== jsoc number tests ===\n\n");

  TEST(1, powers_of_two);
  TEST(2, powers_of_ten);
  TEST(3, subnormals);
  TEST(4, no_double_rounding);
  TEST(5, round_to_even);
  TEST(6, many_digits);
  TEST(7, round_trip);
  TEST(8, small_integers);
  TEST(9, integer_boundaries);
  TEST(10, big_integer_rejected);
  TEST(11, nan_inf_rejected);
  TEST(12, trailing_junk);
  TEST(13, number_in_string);
  TEST(14, locale_independence);
  TEST(15, random_round_trip);

  return jsoc_test_finish("numbers");
}
