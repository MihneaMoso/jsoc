/*
 * JSON Pointer.
 *
 * Ports the RFC 6901 vectors from simdjson's tests/dom/pointercheck.cpp
 * (demo, modern_support, issue1142, issue2154) and the path tests from
 * tests/dom/json_path_tests.cpp, adapted to jsoc's `jsoc_pointer_get`.
 *
 * A key detail ported with them: jsoc's minify writes each raw string slice
 * verbatim (escapes are never re-encoded), which is why the expected values
 * below are the *minified* documents, exactly as simdjson compares them.
 */
#include "test.h"
#include <string.h>

static bool check_pointer(const char *json, const char *pointer,
                          const char *want_minified, bool expect_error) {
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc = jsoc_parse(json, strlen(json), a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *v = jsoc_pointer_get_cstr(&doc, doc.root, pointer);
  if (expect_error) {
    REQUIRE(v == NULL);
  } else {
    REQUIRE(v != NULL);
    jsoc_string out;
    if (!jsoc_value_minify(&doc, v, a, &out)) {
      jsoc_arena_destroy(a);
      return false;
    }
    REQUIRE(out.length == strlen(want_minified) &&
            memcmp(out.data, want_minified, out.length) == 0);
  }
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_rfc6902(void) {
  /* The RFC 6901 examples, straight from pointercheck.cpp, with the exact
   * minified expected values. */
  const char *doc = "{"
                    "\"foo\": [\"bar\", \"baz\"],"
                    "\"\": 0,"
                    "\"a/b\": 1,"
                    "\"c%d\": 2,"
                    "\"e^f\": 3,"
                    "\"g|h\": 4,"
                    "\"i\\\\j\": 5,"
                    "\"k\\\"l\": 6,"
                    "\" \": 7,"
                    "\"m~n\": 8"
                    "}";
  const char *whole =
      "{\"foo\":[\"bar\",\"baz\"],\"\":0,\"a/b\":1,\"c%d\":2,\"e^f\":3,"
      "\"g|h\":4,\"i\\\\j\":5,\"k\\\"l\":6,\" \":7,\"m~n\":8}";

  struct {
    const char *pointer;
    const char *want;
    bool error;
  } cases[] = {
      {"", whole, false},
      {"/foo", "[\"bar\",\"baz\"]", false},
      {"/foo/0", "\"bar\"", false},
      {"/", "0", false},
      {"/a~1b", "1", false},
      {"/c%d", "2", false},
      {"/e^f", "3", false},
      {"/g|h", "4", false},
      {"/i\\j", "5", false},
      {"/k\"l", "6", false},
      {"/ ", "7", false},
      {"/m~0n", "8", false},
      /* The tilde escapes are required; a literal "~" or "~1" mismatch is
       * not a valid pointer spelling. */
      {"/m~n", NULL, true},
      {"/a/b", NULL, true},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    REQUIRE(
        check_pointer(doc, cases[i].pointer, cases[i].want, cases[i].error));
  }
  return true;
fail:
  return false;
}

static bool test_pointercheck_json(void) {
  /* The second document from pointercheck.cpp: keys that need escapes on
   * both sides, including a key that is itself a JSON escape. */
  const char *doc =
      "{"
      "\"/~01abc\": [0, {\"\\\\\\\" 0\": [\"value0\", \"value1\"]}],"
      "\"0\": \"0 ok\","
      "\"01\": \"01 ok\","
      "\"\": \"empty ok\","
      "\"arr\": []"
      "}";
  const char *whole =
      "{\"/~01abc\":[0,{\"\\\\\\\" 0\":[\"value0\",\"value1\"]}],"
      "\"0\":\"0 ok\",\"01\":\"01 ok\",\"\":\"empty ok\",\"arr\":[]}";

  struct {
    const char *pointer;
    const char *want;
    bool error;
  } cases[] = {
      {"", whole, false},
      {"/~1~001abc", "[0,{\"\\\\\\\" 0\":[\"value0\",\"value1\"]}]", false},
      {"/~1~001abc/1", "{\"\\\\\\\" 0\":[\"value0\",\"value1\"]}", false},
      {"/~1~001abc/1/\\\" 0", "[\"value0\",\"value1\"]", false},
      {"/~1~001abc/1/\\\" 0/0", "\"value0\"", false},
      {"/~1~001abc/1/\\\" 0/1", "\"value1\"", false},
      /* Out of range, and an empty index on an array. */
      {"/~1~001abc/1/\\\" 0/2", NULL, true},
      {"/arr/0", NULL, true},
      /* A pointer must start with '/'. */
      {"~1~001abc", NULL, true},
      {"/~01abc", NULL, true}, /* the key is literally "/~01abc" */
      /* A leading zero is not a valid index spelling. */
      {"/~1~001abc/01", NULL, true},
      {"/~1~001abc/", NULL, true},
      /* "0" is a key here, not an index. */
      {"/0", "\"0 ok\"", false},
      {"/01", "\"01 ok\"", false},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    REQUIRE(
        check_pointer(doc, cases[i].pointer, cases[i].want, cases[i].error));
  }
  return true;
fail:
  return false;
}

static bool test_issue2154(void) {
  /* simdjson issue 2154: taking the *value* as a path must not be reported
   * as an invalid pointer, and a dangling/invalid escape is. */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc =
      jsoc_parse("{\"obj\":{\"s\":\"42\",\"n\":42,\"f\":4.2}}", 33, a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *s = jsoc_pointer_get_cstr(&doc, doc.root, "/obj/s");
  REQUIRE(s != NULL);
  REQUIRE(strcmp(jsoc_value_get_string(&doc, s).data, "42") == 0);

  int64_t n = 0;
  const jsoc_value *nv = jsoc_pointer_get_cstr(&doc, doc.root, "/obj/n");
  REQUIRE(nv != NULL && jsoc_value_get_int64(&doc, nv, &n) && n == 42);

  /* A scalar has no members. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/s/42") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/n/42") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/f/4.2") == NULL);
  /* Bad escapes. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/f/4~") == NULL);
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/f/~") == NULL);
  /* "~1" is fine as an escape, it simply names a missing member. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/obj/f/~1") == NULL);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_issue1142(void) {
  /* simdjson issue 1142: an empty pointer returns the value itself. */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc = jsoc_parse("[1,2,{\"1\":\"bla\"}]", 17, a);
  REQUIRE(doc.error.code == JSOC_OK);

  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "") == doc.root);

  const jsoc_value *first = jsoc_pointer_get_cstr(&doc, doc.root, "/0");
  REQUIRE(first != NULL);
  int64_t i = 0;
  REQUIRE(jsoc_value_get_int64(&doc, first, &i) && i == 1);

  /* "/1" is the array's second element (a number), not the object key. */
  REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, "/1") ==
          jsoc_array_at(&doc, doc.root, 1));

  /* The object's key is a number's spelling. */
  const jsoc_value *obj = jsoc_array_at(&doc, doc.root, 2);
  const jsoc_value *bla = jsoc_pointer_get_cstr(&doc, obj, "/1");
  REQUIRE(bla != NULL);
  jsoc_string s = jsoc_value_get_string(&doc, bla);
  REQUIRE(s.length == 3 && memcmp(s.data, "bla", 3) == 0);

  /* A scalar at the root, and an empty array, both accept "". */
  jsoc_document s2 = jsoc_parse("\"just a string\"", 15, a);
  REQUIRE(s2.error.code == JSOC_OK);
  REQUIRE(jsoc_pointer_get_cstr(&s2, s2.root, "") == s2.root);
  jsoc_document s3 = jsoc_parse("[]", 2, a);
  REQUIRE(s3.error.code == JSOC_OK);
  REQUIRE(jsoc_pointer_get_cstr(&s3, s3.root, "") == s3.root);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_demo_and_wildcards(void) {
  /* The cars demo from pointercheck.cpp' `demo`, over our pointer API. */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  const char *cars = "["
                     "{\"make\":\"Toyota\",\"model\":\"Camry\",\"year\":2018,"
                     "\"tire_pressure\":[40.1,39.9,37.7,40.4]},"
                     "{\"make\":\"Kia\",\"model\":\"Soul\",\"year\":2012,"
                     "\"tire_pressure\":[30.1,31.0,28.6,28.7]},"
                     "{\"make\":\"Toyota\",\"model\":\"Tercel\",\"year\":1999,"
                     "\"tire_pressure\":[29.8,30.0,30.2,30.5]}"
                     "]";
  jsoc_document doc = jsoc_parse(cars, strlen(cars), a);
  REQUIRE(doc.error.code == JSOC_OK);

  const double want[3] = {39.9, 31.0, 30.0};
  for (size_t i = 0; i < 3; i++) {
    char pointer[64];
    snprintf(pointer, sizeof pointer, "/%zu/tire_pressure/1", i);
    const jsoc_value *v = jsoc_pointer_get_cstr(&doc, doc.root, pointer);
    REQUIRE(v != NULL);
    double got = 0;
    REQUIRE(jsoc_value_get_double(&doc, v, &got) && got == want[i]);
  }

  /* json_path_tests' wildcard cases are On-Demand JSON Path syntax, which
   * `jsoc_pointer_get` deliberately does not implement (RFC 6901 has no
   * wildcards): every "*" and "0.*" spelling must fail. */
  const char *wildcards[] = {"*",    "/foo*",    "/foo/*", "/foo/0/*",
                             "/*/0", "/foo/*/0", "/foo.0", "/foo[0]"};
  for (size_t i = 0; i < sizeof wildcards / sizeof wildcards[0]; i++) {
    REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, wildcards[i]) == NULL);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_malformed_pointers(void) {
  /* json_path_tests' malformed-path cases, restricted to pointers. */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc = jsoc_parse("{\"foo\":[1,2,3]}", 15, a);
  REQUIRE(doc.error.code == JSOC_OK);

  const char *bad[] = {"foo",      "/foo/",   "/foo/0/", "/foo/-",
                       "/foo/abc", "/foo/1n", "/foo/+1", "/foo/ 1",
                       "/foo/01",  "/~",      "/~2",     "/~x",
                       "//",       "/",       "/\xff//", NULL};
  for (size_t i = 0; bad[i] != NULL; i++) {
    REQUIRE(jsoc_pointer_get_cstr(&doc, doc.root, bad[i]) == NULL);
  }
  (void)a;
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc pointer tests ===\n\n");

  TEST(1, rfc6902);
  TEST(2, pointercheck_json);
  TEST(3, issue2154);
  TEST(4, issue1142);
  TEST(5, demo_and_wildcards);
  TEST(6, malformed_pointers);

  return jsoc_test_finish("pointer");
}
