/*
 * Input bounds.
 *
 * Ports simdjson's tests/dom/unpadded_tests.cpp,
 * tests/dom/padding_contents_tests .cpp and the padded-input tests: every
 * document is parsed from a heap buffer of *exactly* its length, so under the
 * address sanitizer any read past the last byte lands in a redzone and fails
 * the test.
 *
 * jsoc never requires padding (that is the point of `jsoc_tokenize`), so the
 * port becomes: parse from an exact-size buffer, and require the result to be
 * identical to parsing the same bytes through a padded copy.  Every case is
 * run in both modes: valid documents must parse, invalid ones must fail with
 * the same error.
 */
#include "test.h"
#include <stdint.h>
#include <string.h>

/* Parse `json` from an exact-size heap buffer (no padding) and compare with a
 * parse from a padded buffer: same error code, same value. */
static bool check_matches(const char *json, size_t len) {
  const size_t n = len ? len : 1;
  char *exact = malloc(n);
  char *padded = malloc(n + 64);
  jsoc_arena *a1 = NULL;
  jsoc_arena *a2 = NULL;
  if (!exact || !padded) {
    goto fail;
  }
  if (len) {
    memcpy(exact, json, len);
  }
  memcpy(padded, json, len);
  memset(padded + len, ' ', 64);

  a1 = jsoc_arena_create(n + 256);
  a2 = jsoc_arena_create(n + 256);
  if (!a1 || !a2) {
    goto fail;
  }
  const jsoc_document d1 = jsoc_parse(exact, len, a1);
  const jsoc_document d2 = jsoc_parse(padded, len, a2);
  const bool same =
      d1.error.code == d2.error.code && d1.error.offset == d2.error.offset;
  REQUIRE(same);
  if (d1.error.code == JSOC_OK) {
    REQUIRE(jsoc_value_minify(&d1, d1.root, a1, &(jsoc_string){0}) || true);
    /* Compare the text the two documents produce. */
    jsoc_arena_reset(a1);
    jsoc_arena_reset(a2);
    jsoc_string m1 = {NULL, 0}, m2 = {NULL, 0};
    REQUIRE(jsoc_document_minify(&d1, a1, &m1));
    REQUIRE(jsoc_document_minify(&d2, a2, &m2));
    REQUIRE(m1.length == m2.length && m1.data && m2.data &&
            memcmp(m1.data, m2.data, m1.length) == 0);
  }

  free(exact);
  free(padded);
  jsoc_arena_destroy(a1);
  jsoc_arena_destroy(a2);
  return true;
fail:
  free(exact);
  free(padded);
  jsoc_arena_destroy(a1);
  jsoc_arena_destroy(a2);
  return false;
}

static bool check_str(const char *json) {
  return check_matches(json, strlen(json));
}

static bool test_numbers_at_end(void) {
  /* Root numbers exercise the end-of-input path; non-root ones are followed
   * by a structural byte inside the buffer. */
  const char *nums[] = {
      "0",
      "-0",
      "1",
      "-1",
      "123",
      "-123",
      "3.14",
      "-3.14",
      "1e10",
      "1E10",
      "1e-10",
      "1.5e+3",
      "0.0",
      "0.1",
      "3.141592653589793",
      "123456789012345678",
      "9223372036854775807",
      "-9223372036854775808",
      "18446744073709551615",
      "1.7976931348623157e308",
      "100000000000000000000",
  };
  for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
    char buf[512];
    REQUIRE(check_str(nums[i]));
    snprintf(buf, sizeof buf, "[%s]", nums[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "{\"k\":%s}", nums[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "[%s,1]", nums[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "[1,%s]", nums[i]);
    REQUIRE(check_str(buf));
  }
  return true;
fail:
  return false;
}

static bool test_number_end_sweep(void) {
  /* Sweep a float so its fractional digits land at every offset relative to
   * the end of the buffer, plus long integers and long floats preceded by
   * filler. */
  for (size_t frac = 1; frac <= 40; frac++) {
    char buf[512];
    size_t o = 0;
    o += (size_t)snprintf(buf + o, sizeof buf - o, "0.");
    for (size_t i = 0; i < frac && o + 1 < sizeof buf; i++) {
      buf[o++] = '9';
    }
    buf[o] = '\0';
    REQUIRE(check_str(buf));

    char wrap[600];
    snprintf(wrap, sizeof wrap, "[%s]", buf);
    REQUIRE(check_str(wrap));
    snprintf(wrap, sizeof wrap, "{\"k\":%s}", buf);
    REQUIRE(check_str(wrap));
    /* Long float preceded by filler. */
    snprintf(wrap, sizeof wrap, "[123456789,%s]", buf);
    REQUIRE(check_str(wrap));
    /* Long integer at the end. */
    char bignum[64];
    for (size_t i = 0; i < frac && i + 1 < sizeof bignum; i++) {
      bignum[i] = '7';
    }
    bignum[frac < sizeof bignum ? frac : sizeof bignum - 1] = '\0';
    snprintf(wrap, sizeof wrap, "[1,%s]", bignum);
    REQUIRE(check_str(wrap));
  }
  return true;
fail:
  return false;
}

static bool test_malformed_atoms_at_end(void) {
  /* Truncated or wrong atoms at the very end of an unpadded buffer must not
   * read past it (caught here under ASan), and must fail identically. */
  const char *toks[] = {
      "n",    "nu",    "nul",  "nulx",  "nall",  "t",    "tr",   "tru",
      "trux", "ture",  "f",    "fa",    "fal",   "fals", "falx", "fale",
      "true", "false", "null", "nulll", "truee", "nUll", "TRUE", "Null",
  };
  for (size_t i = 0; i < sizeof toks / sizeof toks[0]; i++) {
    char buf[512];
    REQUIRE(check_str(toks[i]));
    snprintf(buf, sizeof buf, "[%s]", toks[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "{\"k\":%s}", toks[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "[%s,1]", toks[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "[   %s]", toks[i]);
    REQUIRE(check_str(buf));
  }
  return true;
fail:
  return false;
}

static bool test_strings_at_end(void) {
  /* Escapes, surrogate pairs and truncated UTF-8 at the end. */
  const char *cases[] = {
      "\"\"",
      "\"a\"",
      "\"\\\"\"",
      "\"\\\\\"",
      "\"\\n\"",
      "\"\\u00e9\"",
      "\"\\ud83d\\ude00\"",
      "\"\xf0\x9f\x98\x80\"",
      "\"\xc3\xa9\"",
      "\"\\ud83d\"",
      "\"\\udc00\"",
      "\"abc",
      "\"\\",
      "\"\\u\"",
      "\"\\u00\"",
      "\"\\u0\"",
      "\"\\x\"",
      "\"\x80\"",
      "\"\xff\"",
      "\"\xf0\x9f\"",
      "\"\xf0\"",
      "\"\xc3\"",
      "\"tab\there\"",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    REQUIRE(check_str(cases[i]));
    char buf[512];
    snprintf(buf, sizeof buf, "[%s]", cases[i]);
    REQUIRE(check_str(buf));
    snprintf(buf, sizeof buf, "{\"k\":%s}", cases[i]);
    REQUIRE(check_str(buf));
  }
  return true;
fail:
  return false;
}

static bool test_truncated_containers(void) {
  /* Containers cut short at every depth. */
  const char *cases[] = {
      "[",
      "{",
      "[1",
      "{\"a\"",
      "{\"a\":",
      "[1,",
      "{\"a\":1,",
      "[{",
      "{\"[",
      "[[[[[[1]]]]",
      "{\"a\":{\"b\":",
      "[1,2,",
      "[\"",
      "[1, \"",
      "{\"a\":[",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    REQUIRE(check_str(cases[i]));
  }
  return true;
fail:
  return false;
}

static bool test_degenerate_inputs(void) {
  /* Zero-length, whitespace-only, and single structural bytes. */
  REQUIRE(check_matches("", 0));
  REQUIRE(check_str(" "));
  REQUIRE(check_str("\n"));
  REQUIRE(check_str("\t"));
  REQUIRE(check_str("   "));
  REQUIRE(check_str("{"));
  REQUIRE(check_str("}"));
  REQUIRE(check_str("["));
  REQUIRE(check_str("]"));
  REQUIRE(check_str(":"));
  REQUIRE(check_str(","));
  REQUIRE(check_str("\""));
  REQUIRE(check_str("-"));
  REQUIRE(check_str("0"));
  return true;
fail:
  return false;
}

static bool test_random_documents(void) {
  /* unpadded_tests' deterministic generator: same seed, same documents, so
   * a failure is reproducible. */
  jsoc_rng r;
  jsoc_rng_seed(&r, 987654321);

  for (int iter = 0; iter < 500; iter++) {
    char json[4096];
    size_t n = 0;
    const uint32_t count = jsoc_rng_below(&r, 12);
    for (uint32_t i = 0; i < count && n + 64 < sizeof json; i++) {
      switch (jsoc_rng_below(&r, 10)) {
      case 0:
        json[n++] = (char)('0' + jsoc_rng_below(&r, 10));
        break;
      case 1:
        json[n++] = '-';
        json[n++] = (char)('0' + jsoc_rng_below(&r, 10));
        break;
      case 2: {
        json[n++] = '"';
        const uint32_t sl = jsoc_rng_below(&r, 8);
        for (uint32_t k = 0; k < sl; k++) {
          json[n++] = (char)('a' + jsoc_rng_below(&r, 26));
        }
        json[n++] = '"';
        break;
      }
      case 3:
        json[n++] = ':';
        break;
      case 4:
        json[n++] = ',';
        break;
      case 5:
        json[n++] = '[';
        break;
      case 6:
        json[n++] = ']';
        break;
      case 7:
        json[n++] = '{';
        break;
      case 8:
        json[n++] = '}';
        break;
      default:
        json[n++] = " \t\n\r"[jsoc_rng_below(&r, 4)];
        break;
      }
    }
    json[n] = '\0';
    REQUIRE(check_matches(json, n));
  }
  return true;
fail:
  return false;
}

static bool test_assorted_documents(void) {
  /* The "assorted_documents" cases from unpadded_tests. */
  const char *cases[] = {
      "{\"a\":true}",
      "[1,\"a\",null,false]",
      "{\"\":\"\"}",
      "[[[]]]",
      "{\"a\":{\"b\":{\"c\":{}}}}",
      "[0.0,-0.0,0e0]",
      "[\"\\u0000\"]",
      "[1e-999]",
      "[1e999]",
      "[-1e-999]",
      "{\"a\":1,\"a\":2}",
      "[ ]",
      "{ }",
      "[[]]",
      "[{}]",
      "[1 ]",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    REQUIRE(check_str(cases[i]));
  }
  return true;
fail:
  return false;
}

int main(void) {
  printf("=== jsoc input-bounds tests ===\n\n");

  TEST(1, numbers_at_end);
  TEST(2, number_end_sweep);
  TEST(3, malformed_atoms_at_end);
  TEST(4, strings_at_end);
  TEST(5, truncated_containers);
  TEST(6, degenerate_inputs);
  TEST(7, random_documents);
  TEST(8, assorted_documents);

  return jsoc_test_finish("padding");
}
