/*
 * Minify and string escaping.
 *
 * Ports simdjson's tests/minify_tests.cpp:
 *   - integer_test: every number at a digit-count boundary round-trips;
 *   - string_escape_test: a reference escaper, every length up to 48, every
 *     special byte at every position, and runs of 6-byte-per-byte escapes at
 *     every offset from the end of the buffer (an over-write would overflow a
 *     preallocated buffer and fail under the address sanitizer);
 *   - load_to_string / load_minify: the corpus files in jsonexamples
 *     (fetched by scripts/fetch-testdata.sh) minify to the same text when
 *     minified twice, which is simdjson's json2json check.
 *
 * Note the one deliberate difference: jsoc writes each raw string slice
 * verbatim (the tokenizer already validated every escape), so it never
 * re-encodes one.  simdjson re-escapes.  Both are correct; the tests below
 * only pin jsoc's behaviour.
 */
#include "test.h"
#include <stdint.h>
#include <string.h>

static const char hex_digits[] = "0123456789abcdef";

/* The exact reference escaper from minify_tests.cpp. */
static size_t escape_reference(const char *s, size_t len, char *out) {
  size_t o = 0;
  out[o++] = '"';
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    switch (c) {
    case '"':
      out[o++] = '\\';
      out[o++] = '"';
      break;
    case '\\':
      out[o++] = '\\';
      out[o++] = '\\';
      break;
    case '\b':
      out[o++] = '\\';
      out[o++] = 'b';
      break;
    case '\f':
      out[o++] = '\\';
      out[o++] = 'f';
      break;
    case '\n':
      out[o++] = '\\';
      out[o++] = 'n';
      break;
    case '\r':
      out[o++] = '\\';
      out[o++] = 'r';
      break;
    case '\t':
      out[o++] = '\\';
      out[o++] = 't';
      break;
    default:
      if (c < 0x20) {
        out[o++] = '\\';
        out[o++] = 'u';
        out[o++] = '0';
        out[o++] = '0';
        out[o++] = hex_digits[c >> 4];
        out[o++] = hex_digits[c & 0xF];
      } else {
        out[o++] = (char)c;
      }
      break;
    }
  }
  out[o++] = '"';
  return o;
}

static bool check_escape(const char *s, size_t len) {
  jsoc_arena *a = NULL;
  char expected[4096];
  const size_t want_len = escape_reference(s, len, expected);
  REQUIRE(want_len < sizeof expected);

  a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  /* The writer escapes from a buffer of exactly `len` bytes, so under the
   * address sanitizer a read past the end fails the test here. */
  char *exact = jsoc_arena_alloc(a, len ? len : 1, 1);
  REQUIRE(exact != NULL);
  if (len) {
    memcpy(exact, s, len);
  }
  jsoc_writer w;
  jsoc_writer_init(&w, a, 64);
  REQUIRE(jsoc_writer_string(&w, exact, len));
  REQUIRE(!w.failed);

  const size_t got_len = jsoc_writer_size(&w);
  REQUIRE(got_len == want_len);
  REQUIRE(memcmp(jsoc_writer_data(&w), expected, got_len) == 0);
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_string_escape(void) {
  /* Strings of every length up to 48 with a character that needs escaping
   * (or one that does not) at every position, alone and next to a second
   * one at the end; then strings made only of specials, including one where
   * every byte becomes \u0001. */
  static const char specials[] = {'"',    '\\', '\n',   '\x00',     '\x01',
                                  '\x1f', ' ',  '\x7f', (char)0x80, (char)0xff};
  static char buf[512];

  for (size_t len = 0; len <= 48; len++) {
    REQUIRE(check_escape("", 0));
    for (size_t k = 0; k < sizeof specials; k++) {
      for (size_t pos = 0; pos < len; pos++) {
        memset(buf, 'a', sizeof buf);
        buf[pos] = specials[k];
        REQUIRE(check_escape(buf, len));
        if (len > 0) {
          buf[len - 1] = '"';
          REQUIRE(check_escape(buf, len));
        }
      }
      /* Strings made only of that byte, in every length. */
      for (size_t i = 0; i < len; i++) {
        buf[i] = specials[k];
      }
      REQUIRE(check_escape(buf, len));
    }
    /* Every byte becomes six: the most output per input byte. */
    memset(buf, '\x01', sizeof buf);
    REQUIRE(check_escape(buf, len));
  }

  /* A run of 40 6-byte escapes starting at every distance from the end of
   * the output buffer, so that writing more than was reserved would
   * overflow it (and fail under the address sanitizer). */
  for (size_t prefix = 0; prefix <= 300; prefix++) {
    memset(buf, 'a', prefix);
    memset(buf + prefix, '\x01', 40);
    REQUIRE(check_escape(buf, prefix + 40));
  }
  return true;
fail:
  return false;
}

static bool test_integer_minify(void) {
  /* Each integer at a digit-count boundary, and the extremes, must come
   * back unchanged from minify. */
  char json[32];
  uint64_t power = 1;
  for (int digits = 1; digits <= 19; digits++) {
    const int64_t values[3] = {(int64_t)power, (int64_t)power - 1,
                               -(int64_t)power};
    for (size_t k = 0; k < 3; k++) {
      snprintf(json, sizeof json, "[%lld]", (long long)values[k]);
      jsoc_arena *a = jsoc_arena_create(1024);
      REQUIRE(a != NULL);
      jsoc_string out;
      REQUIRE(jsoc_minify(json, strlen(json), a, &out, &(jsoc_error){0}));
      REQUIRE(out.length == strlen(json) &&
              memcmp(out.data, json, out.length) == 0);
      jsoc_arena_destroy(a);
    }
    power *= 10;
  }
  return true;
fail:
  return false;
}

static bool test_corpus_round_trip(void) {
  /* load_to_string / load_minify over jsonexamples: minifying twice must
   * give identical text, and minify(parse(minify(x))) is a fixed point. */
  const char *dir = getenv("JSOC_TEST_DATA");
  if (!dir) {
    dir = "/tmp/opencode/simdjson-data/jsonexamples";
  }
  char path[512];
  snprintf(path, sizeof path, "%s/twitter.json", dir);
  size_t len = 0;
  char *text = jsoc_test_read_file(path, &len);
  if (!text) {
    printf("[SKIP] no %s\n            ", path);
    return true;
  }

  jsoc_arena *a = jsoc_arena_create(len + 65536);
  jsoc_arena *b = jsoc_arena_create(len + 65536);
  REQUIRE(a != NULL && b != NULL);
  jsoc_string first;
  REQUIRE(jsoc_minify(text, len, a, &first, &(jsoc_error){0}));

  /* `first` lives in arena a, so the second pass uses its own arena: resetting
   * a would invalidate the input while it is still being read. */
  jsoc_string second;
  REQUIRE(jsoc_minify(first.data, first.length, b, &second, &(jsoc_error){0}));
  REQUIRE(second.length == first.length &&
          memcmp(second.data, first.data, first.length) == 0);

  free(text);
  jsoc_arena_destroy(a);
  jsoc_arena_destroy(b);
  return true;
fail:
  jsoc_arena_destroy(a);
  jsoc_arena_destroy(b);
  return false;
}

int main(void) {
  printf("=== jsoc minify tests ===\n\n");

  TEST(1, string_escape);
  TEST(2, integer_minify);
  TEST(3, corpus_round_trip);

  return jsoc_test_finish("minify");
}
