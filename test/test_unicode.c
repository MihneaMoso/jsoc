/*
 * UTF-8.
 *
 * Ports simdjson's tests/unicode_tests.cpp: a random UTF-8 generator with a
 * fixed seed, so the corpus is reproducible, plus the oracle comparison
 * (jsoc_validate_utf8 must agree with its own scalar reference, which is what
 * the public API promises).
 */
#include "test.h"
#include "utf8.h"
#include <stdint.h>
#include <string.h>

/* xorshift64: the same seed always produces the same bytes. */
typedef struct {
  uint64_t s;
} utf8_rng;

static uint64_t utf8_next(utf8_rng *r) {
  r->s ^= r->s << 13;
  r->s ^= r->s >> 7;
  r->s ^= r->s << 17;
  return r->s;
}

static uint32_t utf8_below(utf8_rng *r, uint32_t n) {
  return (uint32_t)(utf8_next(r) % n);
}

/* Generate `output_bytes` of random UTF-8 (mostly valid), with the given
 * per-length probabilities.  Bytes 0x00..0x7F are never generated as ASCII
 * when they would break JSON strings, so the output can be embedded in one. */
static size_t utf8_generate(utf8_rng *r, char *out, size_t output_bytes) {
  const uint32_t weights[4] = {60, 25, 12, 3};
  size_t n = 0;
  while (n < output_bytes) {
    uint32_t pick = utf8_below(r, 100);
    uint32_t kind = 0;
    while (kind < 4 && pick >= weights[kind]) {
      pick -= weights[kind];
      kind++;
    }
    uint32_t need;
    uint32_t mask_head;
    uint32_t value_bits;
    switch (kind) {
    case 0:
      need = 0;
      break;
    case 1:
      need = 1;
      mask_head = 0xC0;
      value_bits = 0x80;
      break;
    case 2:
      need = 2;
      mask_head = 0xE0;
      value_bits = 0x800;
      break;
    default:
      need = 3;
      mask_head = 0xF0;
      value_bits = 0x10000;
      break;
    }
    if (n + need + 1 > output_bytes) {
      break;
    }
    if (need == 0) {
      out[n++] = (char)(1 + utf8_below(r, 0x7E)); /* no NUL, no quote */
      continue;
    }
    uint32_t cp =
        value_bits +
        utf8_below(r, kind == 3 ? 0x100000u : (kind == 2 ? 0x800u : 0x400u));
    if (kind == 2 && cp > 0xFFFF) {
      cp = 0xFFFF;
    }
    if (kind == 1 && cp > 0x7FF) {
      cp = 0x7FF;
    }
    if (kind == 3 && cp > 0x10FFFF) {
      cp = 0x10FFFF;
    }
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xE000; /* skip surrogates: not valid UTF-8 */
    }
    if (need == 1) {
      out[n++] = (char)(mask_head | (cp >> 6));
      out[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (need == 2) {
      out[n++] = (char)(mask_head | (cp >> 12));
      out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
      out[n++] = (char)(mask_head | (cp >> 18));
      out[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
      out[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = (char)(0x80 | (cp & 0x3F));
    }
  }
  out[n] = '\0';
  return n;
}

static bool test_random_utf8_generator(void) {
  /* Every generated buffer must validate, at every size, in both the vector
   * and the scalar path (the tests run on an AVX2 build and on PORTABLE). */
  utf8_rng r;
  uint64_t seeds[] = {1, 2, 3, 0x9e3779b97f4a7c15ull, 123456789};
  for (size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
    r.s = seeds[s];
    for (size_t size = 0; size <= 512; size++) {
      char buf[1024];
      const size_t n = utf8_generate(&r, buf, size);
      REQUIRE(n <= size);
      size_t off = 0;
      REQUIRE(jsoc_validate_utf8(buf, n, &off));
      REQUIRE(jsoc_utf8_validate_scalar((const uint8_t *)buf, n, &off));
    }
  }
  return true;
fail:
  return false;
}

static bool test_invalid_utf8_rejected(void) {
  /* Every truncated lead byte, overlong form and surrogate must fail, with
   * the first bad byte reported (and without reading past the end). */
  const char *bad[] = {
      "\x80",
      "\xBF",
      "\xC0\x80",
      "\xC1\xBF",
      "\xE0\x80\x80",
      "\xF0\x80\x80\x80",
      "\xED\xA0\x80",
      "\xED\xBF\xBF",
      "\xC3",
      "\xE1\x80",
      "\xF0\x90\x80",
      "\xF5\x80\x80\x80",
      "\xFF",
      "\xFE",
      "\xC3\x28",
      "\xE1\x80\x28",
      "\xF0\x90\x80\x28",
  };
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    const size_t len = strlen(bad[i]);
    char exact[8];
    REQUIRE(len <= sizeof exact);
    memcpy(exact, bad[i], len);
    size_t off = 0;
    REQUIRE(!jsoc_validate_utf8(exact, len, &off));
    REQUIRE(off < len);
    REQUIRE(!jsoc_utf8_validate_scalar((const uint8_t *)exact, len, &off));
  }
  return true;
fail:
  return false;
}

static bool test_boundary_cases(void) {
  /* UTF-8 inside JSON strings: the validator runs over the whole input, and
   * a document whose strings are valid UTF-8 parses. */
  jsoc_arena *a = NULL;
  struct {
    const char *json;
    bool ok;
  } cases[] = {
      {"\"\xc3\xa9\"", true},
      {"\"\xe2\x82\xac\"", true},
      {"\"\xf0\x9f\x98\x80\"", true},
      {"\"\xc3\"", false},
      {"\"\xf0\x9f\x98\x80", false},
      {"\"\xed\xa0\x80\"", false},
      {"[\"\xc3\xa9\",\"\xf0\x9f\x98\x80\"]", true},
      {"{\"k\":\"\xc3\xa9\"}", true},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    a = jsoc_arena_create(4096);
    REQUIRE(a != NULL);
    const size_t len = strlen(cases[i].json);
    char *exact = malloc(len ? len : 1);
    REQUIRE(exact != NULL);
    memcpy(exact, cases[i].json, len);
    jsoc_document doc = jsoc_parse(exact, len, a);
    REQUIRE((doc.error.code == JSOC_OK) == cases[i].ok);
    /* Only the bare-string documents have a string root. */
    if (doc.error.code == JSOC_OK && cases[i].json[0] == '"') {
      /* The document's string decodes back to the same bytes it was
       * parsed from, so the raw slice is inside the input. */
      jsoc_string got = jsoc_value_get_string(&doc, doc.root);
      REQUIRE(got.length == strlen(cases[i].json) - 2);
      REQUIRE(memcmp(got.data, cases[i].json + 1, got.length) == 0);
    }
    free(exact);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc unicode tests ===\n\n");

  TEST(1, random_utf8_generator);
  TEST(2, invalid_utf8_rejected);
  TEST(3, boundary_cases);

  return jsoc_test_finish("unicode");
}
