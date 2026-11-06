#define _POSIX_C_SOURCE 200809L
#include "cursor.h"
#include "decode.h"
#include "dom_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(num, name)                                                        \
  do {                                                                         \
    tests_run++;                                                               \
    printf("TEST %d: %s ... ", num, #name);                                    \
    fflush(stdout);                                                            \
    if (test_##name()) {                                                       \
      tests_passed++;                                                          \
      printf("PASS\n");                                                        \
    } else {                                                                   \
      tests_failed++;                                                          \
      printf("FAIL\n");                                                        \
    }                                                                          \
  } while (0)

#define REQUIRE(cond)                                                          \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "  check failed at line %d: %s\n", __LINE__, #cond);     \
      goto fail;                                                               \
    }                                                                          \
  } while (0)

static jsoc_arena *arena_or_null(void) { return jsoc_arena_create(4096); }

/* ------------------------------------------------------------------------- */
/* Structure                                                                  */
/* ------------------------------------------------------------------------- */

static bool test_root_scalars(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const struct {
    const char *text;
    jsoc_value_type type;
    bool is_integer;
    int64_t i;
    double d;
  } cases[] = {
      {"null", JSOC_VALUE_NULL, false, 0, 0},
      {"true", JSOC_VALUE_TRUE, false, 0, 0},
      {"false", JSOC_VALUE_FALSE, false, 0, 0},
      {"42", JSOC_VALUE_NUMBER, true, 42, 42.0},
      {"-7", JSOC_VALUE_NUMBER, true, -7, -7.0},
      {"1.5", JSOC_VALUE_NUMBER, false, 0, 1.5},
      {"-2.25e2", JSOC_VALUE_NUMBER, false, 0, -225.0},
      {"\"hi\"", JSOC_VALUE_STRING, false, 0, 0},
  };

  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    jsoc_document doc = jsoc_parse(cases[k].text, strlen(cases[k].text), a);
    REQUIRE(doc.error.code == JSOC_OK);
    REQUIRE(doc.root != NULL);
    REQUIRE(jsoc_value_type_of(doc.root) == cases[k].type);

    if (cases[k].type == JSOC_VALUE_NUMBER) {
      REQUIRE(jsoc_value_is_integer(&doc, doc.root) == cases[k].is_integer);
      if (cases[k].is_integer) {
        int64_t v = 0;
        REQUIRE(jsoc_value_get_int64(&doc, doc.root, &v));
        REQUIRE(v == cases[k].i);
      }
      double d = 0;
      REQUIRE(jsoc_value_get_double(&doc, doc.root, &d));
      REQUIRE(d == cases[k].d);
    }
    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_empty_containers(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  jsoc_document arr = jsoc_parse("[]", 2, a);
  REQUIRE(arr.error.code == JSOC_OK);
  REQUIRE(jsoc_value_is_array(arr.root));
  REQUIRE(jsoc_array_size(&arr, arr.root) == 0);
  REQUIRE(jsoc_array_at(&arr, arr.root, 0) == NULL);

  jsoc_arena_reset(a);

  jsoc_document obj = jsoc_parse("{}", 2, a);
  REQUIRE(obj.error.code == JSOC_OK);
  REQUIRE(jsoc_value_is_object(obj.root));
  REQUIRE(jsoc_object_size(&obj, obj.root) == 0);
  REQUIRE(jsoc_object_get_cstr(&obj, obj.root, "x") == NULL);

  jsoc_object_iter it;
  jsoc_object_iter_init(&obj, obj.root, &it);
  jsoc_string key;
  const jsoc_value *val;
  REQUIRE(!jsoc_object_iter_next(&it, &key, &val));

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_nested_structure(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *json = "{\"name\":\"jsoc\",\"ok\":true,\"count\":3,\"nil\":null,"
                     "\"items\":[1,2,3],\"nested\":{\"x\":[{\"y\":10},{}]}}";
  jsoc_document doc = jsoc_parse(json, strlen(json), a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(jsoc_value_is_object(doc.root));
  REQUIRE(jsoc_object_size(&doc, doc.root) == 6);

  const jsoc_value *name = jsoc_object_get_cstr(&doc, doc.root, "name");
  REQUIRE(name && jsoc_value_is_string(name));
  jsoc_string s = jsoc_value_get_string(&doc, name);
  REQUIRE(s.data && s.length == 4 && memcmp(s.data, "jsoc", 4) == 0);

  const jsoc_value *ok = jsoc_object_get_cstr(&doc, doc.root, "ok");
  bool b = false;
  REQUIRE(ok && jsoc_value_get_bool(ok, &b) && b);

  const jsoc_value *count = jsoc_object_get_cstr(&doc, doc.root, "count");
  int64_t n = 0;
  REQUIRE(count && jsoc_value_get_int64(&doc, count, &n) && n == 3);

  REQUIRE(jsoc_value_is_null(jsoc_object_get_cstr(&doc, doc.root, "nil")));

  const jsoc_value *items = jsoc_object_get_cstr(&doc, doc.root, "items");
  REQUIRE(items && jsoc_value_is_array(items));
  REQUIRE(jsoc_array_size(&doc, items) == 3);
  for (size_t i = 0; i < 3; i++) {
    int64_t v = 0;
    REQUIRE(jsoc_value_get_int64(&doc, jsoc_array_at(&doc, items, i), &v));
    REQUIRE(v == (int64_t)(i + 1));
  }

  const jsoc_value *nested = jsoc_object_get_cstr(&doc, doc.root, "nested");
  const jsoc_value *x = jsoc_object_get_cstr(&doc, nested, "x");
  REQUIRE(x && jsoc_array_size(&doc, x) == 2);
  const jsoc_value *first = jsoc_array_at(&doc, x, 0);
  const jsoc_value *y = jsoc_object_get_cstr(&doc, first, "y");
  int64_t yv = 0;
  REQUIRE(y && jsoc_value_get_int64(&doc, y, &yv) && yv == 10);
  REQUIRE(jsoc_object_size(&doc, jsoc_array_at(&doc, x, 1)) == 0);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

static bool test_string_decoding(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *json = "[\"plain\",\"a\\nb\\t\\\"c\\\\d\\/e\",\"\\u00e9\","
                     "\"\\ud83d\\ude00\",\"\\u0041\\u0000B\",\"\xE2\x82\xAC\"]";
  jsoc_document doc = jsoc_parse(json, strlen(json), a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(jsoc_array_size(&doc, doc.root) == 6);

  jsoc_string s0 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 0));
  REQUIRE(s0.length == 5 && memcmp(s0.data, "plain", 5) == 0);

  jsoc_string s1 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 1));
  REQUIRE(s1.length == 10);
  REQUIRE(memcmp(s1.data, "a\nb\t\"c\\d/e", 10) == 0);

  jsoc_string s2 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 2));
  REQUIRE(s2.length == 2 && (unsigned char)s2.data[0] == 0xC3 &&
          (unsigned char)s2.data[1] == 0xA9);

  jsoc_string s3 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 3));
  REQUIRE(s3.length == 4);
  REQUIRE(
      (unsigned char)s3.data[0] == 0xF0 && (unsigned char)s3.data[1] == 0x9F &&
      (unsigned char)s3.data[2] == 0x98 && (unsigned char)s3.data[3] == 0x80);

  jsoc_string s4 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 4));
  REQUIRE(s4.length == 3);
  REQUIRE(s4.data[0] == 'A' && s4.data[1] == '\0' && s4.data[2] == 'B');
  REQUIRE(s4.data[3] == '\0'); /* NUL-terminated */

  jsoc_string s5 =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 5));
  REQUIRE(s5.length == 3 && memcmp(s5.data, "\xE2\x82\xAC", 3) == 0);

  /* Cached: repeated calls must return the same buffer. */
  jsoc_string again =
      jsoc_value_get_string(&doc, jsoc_array_at(&doc, doc.root, 1));
  REQUIRE(again.data == s1.data && again.length == s1.length);

  /* Raw slices remain undecoded. */
  jsoc_string raw = jsoc_value_raw(&doc, jsoc_array_at(&doc, doc.root, 1));
  REQUIRE(raw.length == 15 &&
          memcmp(raw.data, "a\\nb\\t\\\"c\\\\d\\/e", 15) == 0);

  /* Non-strings yield NULL. */
  jsoc_string none = jsoc_value_get_string(&doc, doc.root);
  REQUIRE(none.data == NULL);
  REQUIRE(jsoc_value_get_string(&doc, NULL).data == NULL);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_object_keys(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *json = "{\"plain\":1,\"a\\u0062\":2,\"x\\ny\":3,\"\\u20ac\":4}";
  jsoc_document doc = jsoc_parse(json, strlen(json), a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(jsoc_object_size(&doc, doc.root) == 4);

  const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "plain");
  REQUIRE(v != NULL);
  v = jsoc_object_get_cstr(&doc, doc.root, "ab");
  REQUIRE(v != NULL);
  int64_t n = 0;
  REQUIRE(jsoc_value_get_int64(&doc, v, &n) && n == 2);
  v = jsoc_object_get_cstr(&doc, doc.root, "x\ny");
  REQUIRE(v != NULL);
  jsoc_value_get_int64(&doc, v, &n);
  REQUIRE(n == 3);
  v = jsoc_object_get_cstr(&doc, doc.root, "\xE2\x82\xAC");
  REQUIRE(v != NULL);
  jsoc_value_get_int64(&doc, v, &n);
  REQUIRE(n == 4);

  /* Escaped keys are not matched by their raw spelling. */
  REQUIRE(jsoc_object_get_cstr(&doc, doc.root, "a\\u0062") == NULL);

  jsoc_string k = jsoc_object_get_key(&doc, doc.root, 1);
  REQUIRE(k.length == 2 && memcmp(k.data, "ab", 2) == 0);
  jsoc_string raw = jsoc_object_key_at(&doc, doc.root, 1);
  REQUIRE(raw.length == 7 && memcmp(raw.data, "a\\u0062", 7) == 0);

  jsoc_object_iter it;
  jsoc_object_iter_init(&doc, doc.root, &it);
  jsoc_string key;
  const jsoc_value *val;
  size_t seen = 0;
  while (jsoc_object_iter_next(&it, &key, &val)) {
    REQUIRE(key.data != NULL);
    REQUIRE(val != NULL);
    seen++;
  }
  REQUIRE(seen == 4);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Numbers                                                                    */
/* ------------------------------------------------------------------------- */

static bool test_integer_boundaries(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  struct {
    const char *text;
    bool i64_ok;
    int64_t i64;
    bool u64_ok;
    uint64_t u64;
    bool integer;
  } cases[] = {
      {"9223372036854775807", true, INT64_MAX, true, INT64_MAX, true},
      {"9223372036854775808", false, 0, true, (uint64_t)INT64_MAX + 1u, true},
      {"-9223372036854775808", true, INT64_MIN, false, 0, true},
      {"18446744073709551615", false, 0, true, UINT64_MAX, true},
      {"0", true, 0, true, 0, true},
      {"-0", true, 0, true, 0, true},
      {"1.0", false, 0, false, 0, false},
      {"1e2", false, 0, false, 0, false},
      {"-0.0", false, 0, false, 0, false},
  };

  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    jsoc_document doc = jsoc_parse(cases[k].text, strlen(cases[k].text), a);
    REQUIRE(doc.error.code == JSOC_OK);
    REQUIRE(jsoc_value_is_number(doc.root));
    REQUIRE(jsoc_value_is_integer(&doc, doc.root) == cases[k].integer);

    int64_t si = 123;
    bool got = jsoc_value_get_int64(&doc, doc.root, &si);
    REQUIRE(got == cases[k].i64_ok);
    if (got)
      REQUIRE(si == cases[k].i64);

    uint64_t ui = 123;
    got = jsoc_value_get_uint64(&doc, doc.root, &ui);
    REQUIRE(got == cases[k].u64_ok);
    if (got)
      REQUIRE(ui == cases[k].u64);

    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_double_boundaries(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  struct {
    const char *text;
    bool ok;
    double value;
  } cases[] = {
      {"0", true, 0.0},
      {"-0", true, -0.0},
      {"1e-400", true, 0.0},
      {"123456789012345678901234567890.0", true, 1.2345678901234568e29},
      {"0.1", true, 0.1},
      {"-3.25", true, -3.25},
  };

  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    jsoc_document doc = jsoc_parse(cases[k].text, strlen(cases[k].text), a);
    REQUIRE(doc.error.code == JSOC_OK);
    double d = 42.0;
    bool got = jsoc_value_get_double(&doc, doc.root, &d);
    REQUIRE(got == cases[k].ok);
    if (got) {
      if (cases[k].value == 0.0) {
        REQUIRE(d == 0.0);
      } else {
        const double scale = fabs(cases[k].value);
        REQUIRE(fabs(d - cases[k].value) <= scale * 1e-15);
      }
    }
    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* UTF-8                                                                      */
/* ------------------------------------------------------------------------- */

static bool test_utf8_validation(void) {
  jsoc_arena *a = NULL;
  size_t off = 999;
  REQUIRE(jsoc_validate_utf8("", 0, &off));
  REQUIRE(jsoc_validate_utf8("plain ascii", 11, &off));
  REQUIRE(jsoc_validate_utf8("\xC2\xA2\xE2\x82\xAC\xF0\x9F\x98\x80", 9, &off));

  REQUIRE(!jsoc_validate_utf8("\x80", 1, &off) && off == 0);
  REQUIRE(!jsoc_validate_utf8("\xC0\xAF", 2, &off) && off == 0);
  REQUIRE(!jsoc_validate_utf8("\xE0\x80\x80", 3, &off) && off == 1);
  REQUIRE(!jsoc_validate_utf8("\xED\xA0\x80", 3, &off) && off == 1);
  REQUIRE(!jsoc_validate_utf8("\xF4\x90\x80\x80", 4, &off) && off == 1);
  REQUIRE(!jsoc_validate_utf8("\xF5\x80\x80\x80", 4, &off) && off == 0);
  REQUIRE(!jsoc_validate_utf8("\xE2\x82", 2, &off) && off == 0);
  REQUIRE(!jsoc_validate_utf8("ab\xFF"
                              "cd",
                              5, &off) &&
          off == 2);
  REQUIRE(!jsoc_validate_utf8("\xE2\x28\xA1", 3, &off) && off == 1);

  /* Chunk boundaries: exercise the AVX2 path in -march=native builds and
   * the scalar tail otherwise. */
  char buf[256];
  memset(buf, 'a', sizeof buf);
  REQUIRE(jsoc_validate_utf8(buf, sizeof buf, &off));
  buf[95] = (char)0xC3;
  buf[96] = (char)0xA9;
  REQUIRE(jsoc_validate_utf8(buf, sizeof buf, &off));
  buf[100] = (char)0xFF;
  REQUIRE(!jsoc_validate_utf8(buf, sizeof buf, &off) && off == 100);

  a = arena_or_null();
  REQUIRE(a != NULL);
  const char bad[] = "{\"x\":\"ab\xFF\"}";
  jsoc_document doc = jsoc_parse(bad, sizeof bad - 1, a);
  REQUIRE(doc.error.code == JSOC_ERR_INVALID_UTF8);
  REQUIRE(doc.error.offset == 8);
  REQUIRE(doc.root == NULL);
  jsoc_arena_destroy(a);

  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Errors / streaming / files                                                 */
/* ------------------------------------------------------------------------- */

static bool test_parse_errors(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  struct {
    const char *text;
    jsoc_error_code code;
  } cases[] = {
      {"", JSOC_ERR_SYNTAX},
      {"{", JSOC_ERR_UNTERMINATED_OBJECT},
      {"[1,", JSOC_ERR_UNTERMINATED_ARRAY},
      {"{\"a\" 1}", JSOC_ERR_EXPECTED_COLON},
      {"{\"a\":1,}", JSOC_ERR_EXPECTED_KEY},
      {"[1 2]", JSOC_ERR_EXPECTED_COMMA_OR_END},
      {"\"abc", JSOC_ERR_UNTERMINATED_STRING},
      {"\"a\\q\"", JSOC_ERR_INVALID_ESCAPE},
      {"{\"a\":1} {\"b\":2}", JSOC_ERR_TRAILING_GARBAGE},
      {"[1]x", JSOC_ERR_TRAILING_GARBAGE},
      {"18446744073709551616", JSOC_ERR_NUMBER_OVERFLOW},
      {"-9223372036854775809", JSOC_ERR_NUMBER_OVERFLOW},
      {"1e309", JSOC_ERR_NUMBER_OVERFLOW},
      {"[0,1e309]", JSOC_ERR_NUMBER_OVERFLOW},
  };

  for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
    jsoc_document doc = jsoc_parse(cases[k].text, strlen(cases[k].text), a);
    if (doc.error.code != cases[k].code) {
      fprintf(stderr, "  %s: got %d expected %d\n", cases[k].text,
              (int)doc.error.code, (int)cases[k].code);
      goto fail;
    }
    REQUIRE(doc.root == NULL);
    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

typedef struct {
  size_t docs;
  int64_t sum;
  size_t stop_after;
} many_state;

static bool many_callback(const jsoc_document *doc, void *user) {
  many_state *st = user;
  st->docs++;
  int64_t n = 0;
  if (jsoc_value_get_int64(doc, doc->root, &n))
    st->sum += n;
  else {
    const jsoc_value *v = jsoc_object_get_cstr(doc, doc->root, "v");
    if (v && jsoc_value_get_int64(doc, v, &n))
      st->sum += n;
  }
  return st->stop_after == 0 || st->docs < st->stop_after;
}

static bool test_parse_many(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *ndjson = "1\n2\n{\"v\":3}\n[4,5]\n";
  many_state st = {0, 0, 0};
  jsoc_error err =
      jsoc_parse_many(ndjson, strlen(ndjson), a, many_callback, &st);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(st.docs == 4);
  REQUIRE(st.sum == 1 + 2 + 3);

  /* Callback stop. */
  st = (many_state){0, 0, 2};
  err = jsoc_parse_many(ndjson, strlen(ndjson), a, many_callback, &st);
  REQUIRE(err.code == JSOC_ERR_USER_STOP);
  REQUIRE(st.docs == 2);

  /* NULL callback validates the whole stream. */
  err = jsoc_parse_many(ndjson, strlen(ndjson), a, NULL, NULL);
  REQUIRE(err.code == JSOC_OK);

  /* Invalid later document reports the error. */
  const char *bad = "1\n2\n{\"a\":}\n";
  err = jsoc_parse_many(bad, strlen(bad), a, NULL, NULL);
  REQUIRE(err.code == JSOC_ERR_EXPECTED_VALUE);

  /* Single-document entry point rejects the stream. */
  jsoc_document doc = jsoc_parse(ndjson, strlen(ndjson), a);
  REQUIRE(doc.error.code == JSOC_ERR_TRAILING_GARBAGE);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_overflow_scope(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  /* A number overflow belongs to the document that contains it.  A trailing
   * concatenated value is not part of the document `jsoc_parse` exposes. */
  const char *trailing = "1 1e999";
  jsoc_document doc = jsoc_parse(trailing, strlen(trailing), a);
  REQUIRE(doc.error.code == JSOC_ERR_TRAILING_GARBAGE);
  REQUIRE(doc.error.offset == 2);

  const char *nested = "[1] 1e999";
  doc = jsoc_parse(nested, strlen(nested), a);
  REQUIRE(doc.error.code == JSOC_ERR_TRAILING_GARBAGE);
  REQUIRE(doc.error.offset == 4);

  /* The first document's overflow is reported. */
  const char *first = "1e999 1";
  doc = jsoc_parse(first, strlen(first), a);
  REQUIRE(doc.error.code == JSOC_ERR_NUMBER_OVERFLOW);
  REQUIRE(doc.error.offset == 0);
  REQUIRE(doc.error.line == 1 && doc.error.column == 1);

  /* Streaming validates every document, but still invokes the callback for
   * the valid prefix before reporting the offending one. */
  many_state st = {0, 0, 0};
  jsoc_error err =
      jsoc_parse_many(trailing, strlen(trailing), a, many_callback, &st);
  REQUIRE(err.code == JSOC_ERR_NUMBER_OVERFLOW);
  REQUIRE(err.offset == 2);
  REQUIRE(st.docs == 1);
  REQUIRE(st.sum == 1);

  /* Integer and double boundaries reached through the checked tokenizer. */
  struct {
    const char *text;
    jsoc_error_code code;
  } cases[] = {
      {"9223372036854775807", JSOC_OK},
      {"9223372036854775808", JSOC_OK},
      {"18446744073709551615", JSOC_OK},
      {"18446744073709551616", JSOC_ERR_NUMBER_OVERFLOW},
      {"-9223372036854775808", JSOC_OK},
      {"-9223372036854775809", JSOC_ERR_NUMBER_OVERFLOW},
      {"1.7976931348623157e308", JSOC_OK},
      {"1.7976931348623159e308", JSOC_ERR_NUMBER_OVERFLOW},
      {"0.0001e312", JSOC_OK},
      {"0.0001e313", JSOC_ERR_NUMBER_OVERFLOW},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena_reset(a);
    doc = jsoc_parse(cases[i].text, strlen(cases[i].text), a);
    REQUIRE(doc.error.code == cases[i].code);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_files(void) {
  jsoc_arena *a = NULL;
  const char *path = "/tmp/jsoc_dom_test.json";
  const char *json = "{\"file\":true,\"n\":[1,2,3]}";
  FILE *f = fopen(path, "wb");
  REQUIRE(f != NULL);
  REQUIRE(fwrite(json, 1, strlen(json), f) == strlen(json));
  fclose(f);

  a = arena_or_null();
  REQUIRE(a != NULL);

  jsoc_document doc = jsoc_parse_file(path, a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(jsoc_value_is_object(doc.root));
  const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "file");
  bool b = false;
  REQUIRE(v && jsoc_value_get_bool(v, &b) && b);

  jsoc_arena_reset(a);

  const char *ndpath = "/tmp/jsoc_dom_test.ndjson";
  f = fopen(ndpath, "wb");
  REQUIRE(f != NULL);
  REQUIRE(fwrite("1\n2\n3\n", 1, 6, f) == 6);
  fclose(f);

  many_state st = {0, 0, 0};
  jsoc_error err = jsoc_parse_many_file(ndpath, a, many_callback, &st);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(st.docs == 3 && st.sum == 6);

  jsoc_arena_destroy(a);
  remove(path);
  remove(ndpath);
  return true;
fail:
  if (a)
    jsoc_arena_destroy(a);
  remove(path);
  remove("/tmp/jsoc_dom_test.ndjson");
  return false;
}

/* ------------------------------------------------------------------------- */
/* Depth / lifetime                                                           */
/* ------------------------------------------------------------------------- */

static bool test_deep_nesting(void) {
  char *json = NULL;
  char *deep = NULL;
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  enum { DEPTH = 900 };
  json = malloc((size_t)DEPTH * 2 + 1);
  REQUIRE(json != NULL);
  for (size_t i = 0; i < DEPTH; i++)
    json[i] = '[';
  for (size_t i = 0; i < DEPTH; i++)
    json[DEPTH + i] = ']';
  json[DEPTH * 2] = '\0';

  jsoc_document doc = jsoc_parse(json, DEPTH * 2, a);
  REQUIRE(doc.error.code == JSOC_OK);
  const jsoc_value *v = doc.root;
  for (size_t i = 0; i + 1 < DEPTH; i++) {
    REQUIRE(jsoc_value_is_array(v));
    REQUIRE(jsoc_array_size(&doc, v) == 1);
    v = jsoc_array_at(&doc, v, 0);
  }
  REQUIRE(jsoc_value_is_array(v));
  REQUIRE(jsoc_array_size(&doc, v) == 0);

  /* Above the tokenizer's hard limit. */
  deep = malloc(2048 * 2 + 1);
  REQUIRE(deep != NULL);
  for (size_t i = 0; i < 2048; i++)
    deep[i] = '[';
  for (size_t i = 0; i < 2048; i++)
    deep[2048 + i] = ']';
  jsoc_document too_deep = jsoc_parse(deep, 4096, a);
  REQUIRE(too_deep.error.code == JSOC_ERR_DEPTH_EXCEEDED);

  free(deep);
  free(json);
  jsoc_arena_destroy(a);
  return true;
fail:
  free(json);
  jsoc_arena_destroy(a);
  return false;
}

static bool test_arena_reset(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  for (int round = 0; round < 100; round++) {
    char buf[128];
    snprintf(buf, sizeof buf, "{\"round\":%d,\"v\":[1,2,3,4,5]}", round);
    jsoc_document doc = jsoc_parse(buf, strlen(buf), a);
    REQUIRE(doc.error.code == JSOC_OK);
    const jsoc_value *r = jsoc_object_get_cstr(&doc, doc.root, "round");
    int64_t n = -1;
    REQUIRE(r && jsoc_value_get_int64(&doc, r, &n) && n == round);
    const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "v");
    REQUIRE(v && jsoc_array_size(&doc, v) == 5);
    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_misc_accessors(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  jsoc_document doc = jsoc_parse("[true,false,null,\"s\",7,{},[]]", 29, a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(jsoc_array_size(&doc, doc.root) == 7);

  bool b = true;
  REQUIRE(jsoc_value_get_bool(jsoc_array_at(&doc, doc.root, 0), &b) && b);
  b = true;
  REQUIRE(jsoc_value_get_bool(jsoc_array_at(&doc, doc.root, 1), &b) && !b);
  REQUIRE(!jsoc_value_get_bool(jsoc_array_at(&doc, doc.root, 2), &b));

  REQUIRE(jsoc_value_is_bool(jsoc_array_at(&doc, doc.root, 0)));
  REQUIRE(jsoc_value_is_null(jsoc_array_at(&doc, doc.root, 2)));
  REQUIRE(jsoc_value_is_string(jsoc_array_at(&doc, doc.root, 3)));
  REQUIRE(jsoc_value_is_number(jsoc_array_at(&doc, doc.root, 4)));
  REQUIRE(jsoc_value_is_object(jsoc_array_at(&doc, doc.root, 5)));
  REQUIRE(jsoc_value_is_array(jsoc_array_at(&doc, doc.root, 6)));

  jsoc_string raw = jsoc_value_raw(&doc, jsoc_array_at(&doc, doc.root, 4));
  REQUIRE(raw.length == 1 && memcmp(raw.data, "7", 1) == 0);
  REQUIRE(jsoc_value_raw(&doc, jsoc_array_at(&doc, doc.root, 5)).data == NULL);
  REQUIRE(jsoc_array_at(&doc, doc.root, 7) == NULL);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_differential_with_tokenizer(void) {
  jsoc_arena *a = arena_or_null();
  jsoc_arena *ta = NULL;
  REQUIRE(a != NULL);

  /* For every input, parse success must agree with tokenize success and,
   * on success, the value tree must account for every token. */
  const char *inputs[] = {
      "0",
      "-0.5e+10",
      "\"\"",
      "\"\\u0041\"",
      "[]",
      "{}",
      "[[[[1]]]]",
      "{\"a\":{\"b\":{\"c\":[true,false,null]}}}",
      "  [ 1 , 2 , 3 ]  ",
      "\"\\ud834\\udd1e\"",
  };
  for (size_t k = 0; k < sizeof inputs / sizeof inputs[0]; k++) {
    size_t len = strlen(inputs[k]);
    ta = jsoc_arena_create(1024);
    REQUIRE(ta != NULL);
    jsoc_result r = jsoc_tokenize(inputs[k], len, ta);
    REQUIRE(r.error.code == JSOC_OK);
    jsoc_document doc = jsoc_parse(inputs[k], len, a);
    REQUIRE(doc.error.code == JSOC_OK);
    /* Every token except EOF maps to exactly one value or member. */
    size_t scalars = 0;
    for (size_t i = 0; i < r.count; i++) {
      switch (r.tokens[i].type) {
      case JSOC_TOK_STRING:
        if (i + 1 < r.count && r.tokens[i + 1].type == JSOC_TOK_COLON) {
          /* key */
        } else {
          scalars++;
        }
        break;
      case JSOC_TOK_NUMBER:
      case JSOC_TOK_TRUE:
      case JSOC_TOK_FALSE:
      case JSOC_TOK_NULL:
        scalars++;
        break;
      case JSOC_TOK_OBJ_BEGIN:
      case JSOC_TOK_ARR_BEGIN:
        scalars++;
        break;
      default:
        break;
      }
    }
    REQUIRE(doc.value_count == scalars);
    jsoc_arena_destroy(ta);
    ta = NULL;
    jsoc_arena_reset(a);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(ta);
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* JSON pointer                                                               */
/* ------------------------------------------------------------------------- */

static bool test_json_pointer(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *text = "{"
                     "\"a\": [10, {\"b\": \"x\"}],"
                     "\"a/b\": \"slash-key\","
                     "\"c~d\": \"tilde-key\","
                     "\"\\u00e9\": \"unicode-key\","
                     "\"e\": [[1, 2], [3, 4]]"
                     "}";
  jsoc_document doc = jsoc_parse(text, strlen(text), a);
  REQUIRE(doc.error.code == JSOC_OK);
  REQUIRE(doc.root != NULL);

  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "") == doc.root);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a") ==
          jsoc_object_get_cstr(&doc, doc.root, "a"));
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/0") != NULL);
  REQUIRE(jsoc_value_get_int64(&doc,
                               jsoc_pointer_get_cstr(&doc, doc.root, "/a/0"),
                               &(int64_t){0}) ||
          true);

  int64_t i = -1;
  const jsoc_value *v = jsoc_pointer_get_cstr(&doc, doc.root, "/a/0");
  REQUIRE(v != NULL);
  REQUIRE(jsoc_value_get_int64(&doc, v, &i) && i == 10);

  /* Escaped tokens address keys that decode to the same logical text. */
  v = jsoc_pointer_get_cstr(&doc, doc.root, "/a~1b");
  REQUIRE(v != NULL);
  REQUIRE(jsoc_value_is_string(v));
  {
    jsoc_string s = jsoc_value_get_string(&doc, v);
    REQUIRE(s.length == 9 && memcmp(s.data, "slash-key", 9) == 0);
  }

  v = jsoc_pointer_get_cstr(&doc, doc.root, "/c~0d");
  REQUIRE(v != NULL);
  {
    jsoc_string s = jsoc_value_get_string(&doc, v);
    REQUIRE(s.length == 9 && memcmp(s.data, "tilde-key", 9) == 0);
  }

  /* The pointer text itself is not the key text: a raw "/" in the key is
   * spelled "~1" in the pointer, so "/a/b" addresses nothing. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/b") == NULL);
  /* A bad escape is rejected. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/c~2d") == NULL);

  v = jsoc_pointer_get_cstr(&doc, doc.root, "/\xc3\xa9");
  REQUIRE(v != NULL);

  /* Nested arrays. */
  v = jsoc_pointer_get_cstr(&doc, doc.root, "/e/1/0");
  REQUIRE(v != NULL);
  REQUIRE(jsoc_value_get_int64(&doc, v, &i) && i == 3);

  /* Out-of-range index, index on an object, and a descent into a scalar. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/9") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/00") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/-") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/a/0/0") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "a") == NULL);
  REQUIRE(jsoc_pointer_get(&doc, doc.root, "/a/-1", 5) == NULL);

  /* The builder round-trips: tokens escape, "/" becomes "~1". */
  jsoc_pointer_builder b;
  jsoc_pointer_builder_init(&b, a);
  REQUIRE(jsoc_pointer_push_token(&b, "a", 1));
  REQUIRE(jsoc_pointer_push_token(&b, "1", 1));
  REQUIRE(b.length == 4 && memcmp(b.data, "/a/1", 4) == 0);
  REQUIRE(jsoc_pointer_get(&doc, doc.root, b.data, b.length) ==
          jsoc_pointer_get_cstr(&doc, doc.root, "/a/1"));

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Writer / minify                                                           */
/* ------------------------------------------------------------------------- */

static bool test_writer(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  jsoc_writer w;
  jsoc_writer_init(&w, a, 64);
  int64_t i = 0;

  REQUIRE(jsoc_writer_object_begin(&w));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "name"));
  REQUIRE(jsoc_writer_string_cstr(&w, "a\"b\\c\nd\x01"));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "n"));
  REQUIRE(jsoc_writer_int64(&w, -5));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "u"));
  REQUIRE(jsoc_writer_uint64(&w, (uint64_t)UINT64_MAX));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "f"));
  REQUIRE(jsoc_writer_double(&w, 0.5));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "ok"));
  REQUIRE(jsoc_writer_bool(&w, true));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "nothing"));
  REQUIRE(jsoc_writer_null(&w));
  REQUIRE(jsoc_writer_object_key_cstr(&w, "list"));
  REQUIRE(jsoc_writer_array_begin(&w));
  REQUIRE(jsoc_writer_double(&w, 1.0));
  REQUIRE(jsoc_writer_double(&w, -0.0));
  REQUIRE(jsoc_writer_double(&w, 1e300));
  REQUIRE(jsoc_writer_array_end(&w));
  REQUIRE(jsoc_writer_object_end(&w));
  REQUIRE(!w.failed);

  /* Re-parse what we wrote: it must be valid JSON with the same values.
   * Copy first: the text lives in the arena, and parsing would reset it. */
  jsoc_string text = {jsoc_writer_data(&w), jsoc_writer_size(&w)};
  REQUIRE(text.data != NULL);
  char *copy = malloc(text.length + 1);
  REQUIRE(copy != NULL);
  memcpy(copy, text.data, text.length);
  copy[text.length] = '\0';
  jsoc_arena_reset(a);
  jsoc_document doc = jsoc_parse(copy, text.length, a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *name = jsoc_object_get_cstr(&doc, doc.root, "name");
  REQUIRE(name != NULL);
  {
    jsoc_string s = jsoc_value_get_string(&doc, name);
    REQUIRE(s.length == 8 && memcmp(s.data, "a\"b\\c\nd\x01", 8) == 0);
  }
  i = 0;
  {
    const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "n");
    REQUIRE(v != NULL && jsoc_value_get_int64(&doc, v, &i) && i == -5);
  }
  {
    uint64_t u = 0;
    const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "u");
    REQUIRE(v != NULL && jsoc_value_get_uint64(&doc, v, &u) && u == UINT64_MAX);
  }
  {
    const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "f");
    double d = 0;
    REQUIRE(v != NULL && jsoc_value_get_double(&doc, v, &d) && d == 0.5);
  }
  {
    const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "list");
    REQUIRE(v != NULL && jsoc_array_size(&doc, v) == 3);
    double d = 0;
    REQUIRE(jsoc_value_get_double(&doc, jsoc_array_at(&doc, v, 1), &d));
    REQUIRE(1.0 / d < 0.0);
    REQUIRE(jsoc_value_get_double(&doc, jsoc_array_at(&doc, v, 2), &d) &&
            d == 1e300);
  }

  jsoc_arena_destroy(a);
  free(copy);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_minify(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  /* Whitespace, escapes and literal spacing all normalize away.  The key
   * `"a\tb"` keeps its escape, since re-escaping would change the bytes. */
  const char *pretty = "{ \"a\\tb\" : [ 1 ,\n 2 ] , \"c\" : \"\\u0041\" }";
  jsoc_string out;
  jsoc_error err;
  REQUIRE(jsoc_minify(pretty, strlen(pretty), a, &out, &err));
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(out.data != NULL);
  /* Escaped bytes stay escaped: the raw slice is written back verbatim. */
  static const char want[] = {'{', '"',  'a', '\\', 't', 'b', '"', ':', '[',
                              '1', ',',  '2', ']',  ',', '"', 'c', '"', ':',
                              '"', '\\', 'u', '0',  '0', '4', '1', '"', '}'};
  REQUIRE(out.length == sizeof want &&
          memcmp(out.data, want, sizeof want) == 0);

  /* Malformed input reports the tokenizer error and no text. */
  REQUIRE(!jsoc_minify("{", 1, a, &out, &err));
  REQUIRE(err.code != JSOC_OK);
  REQUIRE(out.data == NULL && out.length == 0);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Cursor                                                                    */
/* ------------------------------------------------------------------------- */

static bool test_cursor(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *text = "{\"a\":[{\"b\":1},{\"b\":2}],\"c\":[3,4]}";
  jsoc_document doc = jsoc_parse(text, strlen(text), a);
  REQUIRE(doc.error.code == JSOC_OK);

  int64_t i = 0;
  jsoc_cursor cur;
  jsoc_cursor_init(&cur, &doc);
  REQUIRE(jsoc_cursor_value(&cur) == doc.root);
  REQUIRE(jsoc_cursor_key(&cur).data == NULL);

  /* into "a", then the first array element */
  REQUIRE(jsoc_cursor_down(&cur));
  REQUIRE(jsoc_cursor_key(&cur).length == 1);
  REQUIRE(memcmp(jsoc_cursor_key(&cur).data, "a", 1) == 0);
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_ARRAY);
  REQUIRE(jsoc_cursor_down(&cur));
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_OBJECT);

  /* "b" = 1 */
  REQUIRE(jsoc_cursor_down(&cur));
  REQUIRE(jsoc_cursor_key(&cur).length == 1);
  REQUIRE(memcmp(jsoc_cursor_key(&cur).data, "b", 1) == 0);
  REQUIRE(jsoc_value_get_int64(&doc, jsoc_cursor_value(&cur), &i) && i == 1);

  /* scalar: no further descent */
  REQUIRE(!jsoc_cursor_down(&cur));

  /* up to the array element, then onto its sibling element */
  REQUIRE(jsoc_cursor_up(&cur));
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_OBJECT);
  REQUIRE(jsoc_cursor_key(&cur).data == NULL); /* array element: no key */
  REQUIRE(jsoc_cursor_next(&cur));
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_OBJECT);
  REQUIRE(jsoc_cursor_down(&cur));
  REQUIRE(jsoc_value_get_int64(&doc, jsoc_cursor_value(&cur), &i) && i == 2);
  REQUIRE(!jsoc_cursor_down(&cur)); /* scalar */
  REQUIRE(!jsoc_cursor_next(&cur)); /* last member of element 1 */

  /* back out of the array and onto the object's "c" member */
  REQUIRE(jsoc_cursor_up(&cur));
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_OBJECT);
  REQUIRE(!jsoc_cursor_next(&cur)); /* last element of the array */
  REQUIRE(jsoc_cursor_up(&cur));
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_ARRAY);
  REQUIRE(jsoc_cursor_key(&cur).length == 1);
  REQUIRE(memcmp(jsoc_cursor_key(&cur).data, "a", 1) == 0);
  REQUIRE(jsoc_cursor_next(&cur)); /* the root's next member */
  REQUIRE(jsoc_cursor_key(&cur).length == 1);
  REQUIRE(memcmp(jsoc_cursor_key(&cur).data, "c", 1) == 0);
  REQUIRE(jsoc_cursor_value(&cur)->type == JSOC_VALUE_ARRAY);
  REQUIRE(!jsoc_cursor_next(&cur)); /* last member */

  /* into [3, 4], then all the way back to the root */
  REQUIRE(jsoc_cursor_down(&cur));
  REQUIRE(jsoc_value_get_int64(&doc, jsoc_cursor_value(&cur), &i) && i == 3);
  REQUIRE(jsoc_cursor_next(&cur));
  REQUIRE(jsoc_value_get_int64(&doc, jsoc_cursor_value(&cur), &i) && i == 4);
  REQUIRE(jsoc_cursor_up(&cur));
  REQUIRE(jsoc_cursor_key(&cur).length == 1);
  REQUIRE(memcmp(jsoc_cursor_key(&cur).data, "c", 1) == 0);
  REQUIRE(jsoc_cursor_up(&cur));
  REQUIRE(jsoc_cursor_value(&cur) == doc.root);
  REQUIRE(jsoc_cursor_key(&cur).data == NULL);

  /* `up` from the root fails. */
  REQUIRE(!jsoc_cursor_up(&cur));

  /* `enter` walks a whole document in depth-first order. */
  jsoc_cursor walk;
  jsoc_cursor_init(&walk, &doc);
  size_t visited = 0;
  const jsoc_value *v = jsoc_cursor_value(&walk);
  REQUIRE(v != NULL);
  size_t guard = 0;
  for (;;) {
    if (++guard > 64)
      break;
    v = jsoc_cursor_value(&walk);
    if (v)
      visited += jsoc_cursor_key(&walk).length;
    if (!jsoc_cursor_enter(&walk)) {
      if (!jsoc_cursor_next(&walk)) {
        while (!jsoc_cursor_next(&walk)) {
          if (!jsoc_cursor_up(&walk))
            goto done;
        }
      }
    }
  }
done:
  REQUIRE(visited == 1 /* a */ + 1 /* b */ + 1 /* b */ + 1 /* c */);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Reflected decode                                                          */
/* ------------------------------------------------------------------------- */

struct point {
  int64_t x;
  int64_t y;
};
struct shape {
  struct point *at;
  size_t at_count;
};

static const jsoc_decode_field point_fields[] = {
    JSOC_DECODE_FIELD_A(point, x, JSOC_DECODE_INT64, .required = true),
    JSOC_DECODE_FIELD_A(point, y, JSOC_DECODE_INT64, .required = true),
    {.name = NULL},
};

static const jsoc_decode_field shape_fields[] = {
    JSOC_DECODE_FIELD_A(shape, at, JSOC_DECODE_ARRAY,
                        .count_offset = offsetof(struct shape, at_count),
                        .item_kind = JSOC_DECODE_STRUCT,
                        .item_size = sizeof(struct point),
                        .items = point_fields),
    {.name = NULL},
};

static bool test_decode(void) {
  jsoc_arena *a = arena_or_null();
  REQUIRE(a != NULL);

  const char *text = "{\"x\":-3,\"y\":12,\"z\":1}";
  jsoc_document doc = jsoc_parse(text, strlen(text), a);
  REQUIRE(doc.error.code == JSOC_OK);

  struct point p = {0, 0};
  size_t n = sizeof(point_fields) / sizeof(point_fields[0]) - 1;
  jsoc_decode_result r =
      jsoc_decode_object(point_fields, n, &doc, doc.root, &p);
  REQUIRE(r.code == JSOC_DECODE_OK);
  REQUIRE(p.x == -3 && p.y == 12);

  /* A missing required member is reported. */
  {
    const char *part_text = "{\"x\":5}";
    jsoc_document part = jsoc_parse(part_text, strlen(part_text), a);
    REQUIRE(part.error.code == JSOC_OK);
    jsoc_decode_result m =
        jsoc_decode_object(point_fields, n, &part, part.root, &p);
    REQUIRE(m.code == JSOC_DECODE_MISSING_FIELD);
    REQUIRE(m.field_name != NULL && strcmp(m.field_name, "y") == 0);
  }

  /* Type mismatch. */
  {
    const char *bad_text = "{\"x\":\"s\",\"y\":1}";
    jsoc_document bad = jsoc_parse(bad_text, strlen(bad_text), a);
    REQUIRE(bad.error.code == JSOC_OK);
    jsoc_decode_result t =
        jsoc_decode_object(point_fields, 2, &bad, bad.root, &p);
    REQUIRE(t.code == JSOC_DECODE_TYPE_MISMATCH);
    REQUIRE(t.field_name != NULL && strcmp(t.field_name, "x") == 0);
  }

  /* Arrays of structs. */
  {
    struct point items[3];
    /* `at_count` is the buffer capacity on entry. */
    struct shape s = {items, 3};
    const char *arr = "{\"at\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]}";
    jsoc_document d = jsoc_parse(arr, strlen(arr), a);
    REQUIRE(d.error.code == JSOC_OK);
    jsoc_decode_result ar = jsoc_decode_object(shape_fields, 1, &d, d.root, &s);
    REQUIRE(ar.code == JSOC_DECODE_OK);
    REQUIRE(s.at_count == 2);
    REQUIRE(items[0].x == 1 && items[0].y == 2);
    REQUIRE(items[1].x == 3 && items[1].y == 4);
  }

  /* An array longer than the buffer overflows. */
  {
    struct point items[1];
    struct shape s = {items, 1};
    const char *arr = "{\"at\":[{\"x\":1,\"y\":2},{\"x\":3,\"y\":4}]}";
    jsoc_document d = jsoc_parse(arr, strlen(arr), a);
    REQUIRE(d.error.code == JSOC_OK);
    REQUIRE(jsoc_decode_object(shape_fields, 1, &d, d.root, &s).code ==
            JSOC_DECODE_OVERFLOW);
    REQUIRE(s.at_count == 1);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc DOM test suite ===\n\n");

  TEST(1, root_scalars);
  TEST(2, empty_containers);
  TEST(3, nested_structure);
  TEST(4, string_decoding);
  TEST(5, object_keys);
  TEST(6, integer_boundaries);
  TEST(7, double_boundaries);
  TEST(8, utf8_validation);
  TEST(9, parse_errors);
  TEST(10, parse_many);
  TEST(11, files);
  TEST(12, deep_nesting);
  TEST(13, arena_reset);
  TEST(14, misc_accessors);
  TEST(15, differential_with_tokenizer);
  TEST(16, overflow_scope);
  TEST(17, json_pointer);
  TEST(18, writer);
  TEST(19, minify);
  TEST(20, cursor);
  TEST(21, decode);

  printf("\n=== Results: %d/%d passed (%d failed) ===\n", tests_passed,
         tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}
