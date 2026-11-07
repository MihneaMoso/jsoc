/*
 * Errors.
 *
 * Ports the behaviour of simdjson's tests/dom/errortests.cpp,
 * tests/ondemand/ondemand_error_tests.cpp,
 * tests/ondemand/ondemand_wrong_type_error_tests.cpp,
 * tests/ondemand/ondemand_object_error_tests.cpp and
 * tests/ondemand/ondemand_array_error_tests.cpp, adapted to jsoc's API (which
 * reports errors as jsoc_error and "wrong type" as a failed getter).
 */
#include "test.h"
#include <stdint.h>
#include <string.h>

static bool test_unterminated(void) {
  /* Each truncation point reports the container it was inside, at the end
   * of input, exactly like the reference scanner. */
  const struct {
    const char *json;
    jsoc_error_code code;
  } cases[] = {
      {"", JSOC_ERR_SYNTAX},
      {"   ", JSOC_ERR_EXPECTED_VALUE},
      {"\"abc", JSOC_ERR_UNTERMINATED_STRING},
      {"{\"a\":\"bc", JSOC_ERR_UNTERMINATED_STRING},
      {"[", JSOC_ERR_UNTERMINATED_ARRAY},
      {"[1,", JSOC_ERR_UNTERMINATED_ARRAY},
      {"[1", JSOC_ERR_UNTERMINATED_ARRAY},
      {"{\"a\":1", JSOC_ERR_UNTERMINATED_OBJECT},
      {"{\"a\":", JSOC_ERR_UNTERMINATED_OBJECT},
      {"{\"a\"", JSOC_ERR_UNTERMINATED_OBJECT},
      {"[{", JSOC_ERR_UNTERMINATED_OBJECT},
      {"{\"a\":[", JSOC_ERR_UNTERMINATED_ARRAY},
      /* A raw control byte inside a string is its own error. */
      {"\"a\x01"
       "b\"",
       JSOC_ERR_INVALID_CONTROL},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(4096);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i].json, strlen(cases[i].json), a);
    REQUIRE(doc.error.code == cases[i].code);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_structure_errors(void) {
  const struct {
    const char *json;
    jsoc_error_code code;
  } cases[] = {
      {"[}", JSOC_ERR_SYNTAX},
      {"{\"a\" 1}", JSOC_ERR_EXPECTED_COLON},
      {"[1 2]", JSOC_ERR_EXPECTED_COMMA_OR_END},
      {"{} extra", JSOC_ERR_TRAILING_GARBAGE},
      {"[1,]", JSOC_ERR_EXPECTED_VALUE},
      {"{\"a\":1,}", JSOC_ERR_EXPECTED_KEY},
      {"{,}", JSOC_ERR_EXPECTED_KEY},
      {"{:}", JSOC_ERR_EXPECTED_KEY},
      {"{\"a\":}", JSOC_ERR_EXPECTED_VALUE},
      {"{\"a\"::1}", JSOC_ERR_EXPECTED_VALUE},
      {"[:]", JSOC_ERR_EXPECTED_VALUE},
      {"[1,,2]", JSOC_ERR_EXPECTED_VALUE},
      {"{}}", JSOC_ERR_TRAILING_GARBAGE},
      {"]", JSOC_ERR_SYNTAX},
      {"}", JSOC_ERR_SYNTAX}, /* lone close: the portable scanner's generic
                                 syntax error */
      {",", JSOC_ERR_EXPECTED_VALUE},
      {":", JSOC_ERR_EXPECTED_VALUE},
      /* Strings where a value is required. */
      {"[\"a\" \"b\"]", JSOC_ERR_EXPECTED_COMMA_OR_END},
      /* Trailing documents are only valid whitespace-separated, which is
       * what `jsoc_parse_many` consumes; `jsoc_parse` rejects them. */
      {"{\"a\":1}{\"b\":2}", JSOC_ERR_TRAILING_GARBAGE},
      {"1 2 3", JSOC_ERR_TRAILING_GARBAGE},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(4096);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i].json, strlen(cases[i].json), a);
    if (cases[i].code == JSOC_OK) {
      REQUIRE(doc.error.code == JSOC_OK);
    } else {
      REQUIRE(doc.error.code == cases[i].code);
    }
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_get_values_on_error(void) {
  /* Accessors must fail cleanly (and not crash) on the wrong type, the way
   * simdjson's get_values_on_error requires an empty result. */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc =
      jsoc_parse("{\"a\":1,\"b\":[1,2],\"c\":{\"d\":3}}",
                 strlen("{\"a\":1,\"b\":[1,2],\"c\":{\"d\":3}}"), a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *v = jsoc_object_get_cstr(&doc, doc.root, "a");
  REQUIRE(v != NULL);

  /* An int is not an array or an object. */
  REQUIRE(jsoc_array_size(&doc, v) == 0);
  REQUIRE(jsoc_array_at(&doc, v, 0) == NULL);
  REQUIRE(jsoc_object_size(&doc, v) == 0);
  REQUIRE(jsoc_object_get_cstr(&doc, v, "x") == NULL);
  REQUIRE(!jsoc_value_is_array(v) && !jsoc_value_is_object(v));

  /* A missing member is NULL, not an error object. */
  REQUIRE(jsoc_object_get_cstr(&doc, doc.root, "nope") == NULL);
  REQUIRE(jsoc_object_get_cstr(&doc, doc.root, "") == NULL);
  REQUIRE(jsoc_object_get_cstr(&doc, v, "a") == NULL);

  /* Wrong scalar kinds. */
  bool b = true;
  REQUIRE(!jsoc_value_get_bool(v, &b) && !b);
  int64_t i = 0;
  REQUIRE(jsoc_value_get_int64(&doc, v, &i) && i == 1);
  REQUIRE(jsoc_value_get_uint64(&doc, v, &(uint64_t){0}));

  /* Beyond the last element. */
  const jsoc_value *arr = jsoc_object_get_cstr(&doc, doc.root, "b");
  REQUIRE(jsoc_array_size(&doc, arr) == 2);
  REQUIRE(jsoc_array_at(&doc, arr, 2) == NULL);
  REQUIRE(jsoc_array_at(&doc, arr, (size_t)-1) == NULL);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_depth_exceeded(void) {
  jsoc_arena *a = NULL;
  /* Nesting is bounded by JSOC_MAX_DEPTH; the error names the limit. */
  char *json = malloc(2 * JSOC_MAX_DEPTH + 2);
  REQUIRE(json != NULL);
  for (size_t i = 0; i < JSOC_MAX_DEPTH; i++)
    json[i] = '[';
  for (size_t i = 0; i < JSOC_MAX_DEPTH; i++)
    json[JSOC_MAX_DEPTH + i] = ']';
  json[2 * JSOC_MAX_DEPTH] = '\0';

  a = jsoc_arena_create(2 * JSOC_MAX_DEPTH + 64);
  REQUIRE(a != NULL);
  jsoc_document doc = jsoc_parse(json, 2 * JSOC_MAX_DEPTH, a);
  REQUIRE(doc.error.code == JSOC_OK);
  jsoc_arena_destroy(a);
  free(json);
  json = NULL;

  /* One level deeper fails with a depth error, not a crash. */
  json = malloc(2 * (JSOC_MAX_DEPTH + 2) + 2);
  REQUIRE(json != NULL);
  for (size_t i = 0; i < JSOC_MAX_DEPTH + 1; i++)
    json[i] = '[';
  for (size_t i = 0; i < JSOC_MAX_DEPTH + 1; i++)
    json[JSOC_MAX_DEPTH + 1 + i] = ']';
  jsoc_arena *b = jsoc_arena_create(2 * (JSOC_MAX_DEPTH + 2) + 64);
  REQUIRE(b != NULL);
  jsoc_document deep = jsoc_parse(json, 2 * (JSOC_MAX_DEPTH + 1), b);
  REQUIRE(deep.error.code == JSOC_ERR_DEPTH_EXCEEDED);
  jsoc_arena_destroy(b);
  free(json);
  return true;
fail:
  free(json);
  jsoc_arena_destroy(a);
  return false;
}

static bool test_number_overrun_scope(void) {
  /* errortests' number_overrun_at_root/in_array/in_object: a number that
   * runs to the end of the input is complete at the top level, but inside a
   * container the missing separator makes the document invalid. */
  const struct {
    const char *json;
    jsoc_error_code code;
  } cases[] = {
      {"1", JSOC_OK},
      {"-1", JSOC_OK},
      {"1234567890123456", JSOC_OK},
      {"1.5", JSOC_OK},
      {"[1", JSOC_ERR_UNTERMINATED_ARRAY},
      {"{\"a\":1", JSOC_ERR_UNTERMINATED_OBJECT},
      {"[1.5", JSOC_ERR_UNTERMINATED_ARRAY},
      {"[1,", JSOC_ERR_UNTERMINATED_ARRAY},
      {"{\"a\":1.0", JSOC_ERR_UNTERMINATED_OBJECT},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(4096);
    REQUIRE(a != NULL);
    jsoc_document doc = jsoc_parse(cases[i].json, strlen(cases[i].json), a);
    REQUIRE(doc.error.code == cases[i].code);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_arena_nomem(void) {
  jsoc_arena *a = NULL;
  /* A bad handle is reported, never dereferenced. */
  jsoc_document bad = jsoc_parse("1", 1, NULL);
  REQUIRE(bad.error.code != JSOC_OK);
  jsoc_document nully = jsoc_parse(NULL, 0, NULL);
  REQUIRE(nully.error.code != JSOC_OK);
  /* And so is a missing file. */
  a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  const char *missing = "/tmp/opencode/definitely-not-here-jsoc.json";
  jsoc_document d = jsoc_parse_file(missing, a);
  REQUIRE(d.error.code != JSOC_OK);
  jsoc_error e = jsoc_parse_many_file(missing, a, NULL, NULL);
  REQUIRE(e.code != JSOC_OK);
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc error tests ===\n\n");

  TEST(1, unterminated);
  TEST(2, structure_errors);
  TEST(3, get_values_on_error);
  TEST(4, depth_exceeded);
  TEST(5, number_overrun_scope);
  TEST(6, arena_nomem);

  return jsoc_test_finish("errors");
}
