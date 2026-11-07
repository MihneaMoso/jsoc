#define _POSIX_C_SOURCE 200809L
#include "arena.h"
#include "error.h"
#include "jsoc.h"
#include "token.h"
#include "tokenizer_internal.h"
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

static bool test_empty_object(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("{}", 2, a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 3) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_empty_array(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("[]", 2, a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 3) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_ARR_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_ARR_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_simple_object(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "{\"a\":1}";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 6) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_STRING) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].offset != 2) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].length != 1) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_COLON) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].type != JSOC_TOK_NUMBER) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].offset != 5) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].length != 1) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[4].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[5].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_nested_objects(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "{\"a\":{\"b\":2}}";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 10) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_STRING) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_COLON) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[4].type != JSOC_TOK_STRING) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[5].type != JSOC_TOK_COLON) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[6].type != JSOC_TOK_NUMBER) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[7].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[8].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[9].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_array_of_objects(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "[{},{}]";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 8) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_ARR_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].type != JSOC_TOK_COMMA) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[4].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[5].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[6].type != JSOC_TOK_ARR_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[7].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_strings_with_escapes(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "\"hello\\nworld\\t\\\"\\\\\"";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 2) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_unicode_escape(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "\"\\u0041\\u00e9\"";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 2) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_numbers(void) {
  const char *nums[] = {"0",     "-0",    "42",       "-42",
                        "3.14",  "-3.14", "1e10",     "1E10",
                        "1e+10", "1e-10", "3.14e-10", "-1.5E+5"};
  for (size_t i = 0; i < sizeof(nums) / sizeof(nums[0]); i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize(nums[i], strlen(nums[i]), a);
    if (r.error.code != JSOC_OK) {
      jsoc_arena_destroy(a);
      return false;
    }
    if (r.count != 2) {
      jsoc_arena_destroy(a);
      return false;
    }
    if (r.tokens[0].type != JSOC_TOK_NUMBER) {
      jsoc_arena_destroy(a);
      return false;
    }
    if (r.tokens[1].type != JSOC_TOK_EOF) {
      jsoc_arena_destroy(a);
      return false;
    }
    jsoc_arena_destroy(a);
  }
  return true;
}

static bool test_literals(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("true false null", 15, a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 4) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_TRUE) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_FALSE) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_NULL) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[3].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_whitespace(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  const char *json = "  \t\n\r  {  \t\n\r  }  \t\n\r";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 3) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[1].type != JSOC_TOK_OBJ_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.tokens[2].type != JSOC_TOK_EOF) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_unterminated_string(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("\"hello", 6, a);
  if (r.error.code != JSOC_ERR_UNTERMINATED_STRING) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_unterminated_array(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("[1,2", 4, a);
  if (r.error.code != JSOC_ERR_UNTERMINATED_ARRAY) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_unterminated_object(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("{\"a\":1", 6, a);
  if (r.error.code != JSOC_ERR_UNTERMINATED_OBJECT) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_invalid_escape(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("\"\\x\"", 4, a);
  if (r.error.code != JSOC_ERR_INVALID_ESCAPE) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_invalid_unicode(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("\"\\uGGGG\"", 8, a);
  if (r.error.code != JSOC_ERR_INVALID_UNICODE) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_trailing_garbage(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("{} extra", 8, a);
  if (r.error.code != JSOC_ERR_TRAILING_GARBAGE) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_mismatched_brackets(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("[}", 2, a);
  if (r.error.code != JSOC_ERR_SYNTAX) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_expected_colon(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("{\"a\" 1}", 7, a);
  if (r.error.code != JSOC_ERR_EXPECTED_COLON) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_error_expected_comma(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  jsoc_result r = jsoc_tokenize("[1 2]", 5, a);
  if (r.error.code != JSOC_ERR_EXPECTED_COMMA_OR_END) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_example_json(void) {
  jsoc_arena *a = jsoc_arena_create(4096);
  const char *json =
      "[{\"foo\":\"bar\",\"bin\":\"baz\"},{},{},[],{\"test\":\"hello\"}]";
  jsoc_result r = jsoc_tokenize(json, strlen(json), a);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 27) {
    jsoc_arena_destroy(a);
    return false;
  }
  int counts[12] = {0};
  for (size_t i = 0; i < r.count; i++)
    counts[r.tokens[i].type]++;
  if (counts[JSOC_TOK_OBJ_BEGIN] != 4) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_OBJ_END] != 4) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_ARR_BEGIN] != 2) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_ARR_END] != 2) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_COMMA] != 5) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_STRING] != 6) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_COLON] != 3) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (counts[JSOC_TOK_EOF] != 1) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_arena_basic(void) {
  jsoc_arena *a = jsoc_arena_create(1024);
  void *p1 = jsoc_arena_alloc(a, 100, 16);
  if (!p1) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (((uintptr_t)p1 % 16) != 0) {
    jsoc_arena_destroy(a);
    return false;
  }
  void *p2 = jsoc_arena_alloc(a, 200, 32);
  if (!p2) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (((uintptr_t)p2 % 32) != 0) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_reset(a);
  void *p3 = jsoc_arena_alloc(a, 50, 8);
  if (!p3) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (p3 != a->base) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_arena_growth(void) {
  jsoc_arena *a = jsoc_arena_create(64);
  for (int i = 0; i < 1000; i++) {
    void *p = jsoc_arena_alloc(a, 100, 16);
    if (!p) {
      jsoc_arena_destroy(a);
      return false;
    }
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_token_print(void) {
  jsoc_token tok = {.offset = 4, .length = 3, .type = JSOC_TOK_STRING};
  char buf[64];
  jsoc_token_print(&tok, "abc\"hello\"def", buf, sizeof(buf));
  if (strcmp(buf, "STRING(hel)") != 0)
    return false;
  return true;
}

static bool test_large_input(void) {
  size_t n = 100000;
  char *json = malloc(n + 3);
  json[0] = '[';
  for (size_t i = 0; i < n / 2; i++)
    json[1 + i] = (char)('1' + (i % 9));
  json[1 + n / 2] = ']';
  json[2 + n / 2] = '\0';
  size_t json_len = 1 + n / 2 + 1; // '[' + n/2 digits + ']'

  jsoc_arena *a = jsoc_arena_create(4096);
  jsoc_result r = jsoc_tokenize(json, json_len, a);
  free(json);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != 4) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_deep_nesting(void) {
  size_t depth = 1000;
  char *json = malloc(depth * 2 + 1);
  for (size_t i = 0; i < depth; i++)
    json[i] = '[';
  for (size_t i = 0; i < depth; i++)
    json[depth + i] = ']';
  json[depth * 2] = '\0';

  jsoc_arena *a = jsoc_arena_create(4096);
  jsoc_result r = jsoc_tokenize(json, depth * 2, a);
  free(json);
  if (r.error.code != JSOC_OK) {
    jsoc_arena_destroy(a);
    return false;
  }
  if (r.count != depth * 2 + 1) {
    jsoc_arena_destroy(a);
    return false;
  }
  jsoc_arena_destroy(a);
  return true;
}

static bool test_strict_numbers(void) {
  const char *bad[] = {"[01]", "[1.]", "[1e]", "[-]", "[+1]"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize(bad[i], strlen(bad[i]), a);
    bool ok = r.error.code != JSOC_OK;
    jsoc_arena_destroy(a);
    if (!ok)
      return false;
  }
  return true;
}

static bool test_missing_value(void) {
  const char *bad[] = {"{\"a\":}", "{\"a\":,}", "[1,]", "{\"a\":1,}"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize(bad[i], strlen(bad[i]), a);
    bool ok = r.error.code != JSOC_OK;
    jsoc_arena_destroy(a);
    if (!ok)
      return false;
  }
  return true;
}

static bool test_tokenize_file(void) {
  const char *path = "/tmp/jsoc_test_tokenize_file.json";
  const char *json = "{\"file\":true}";
  FILE *f = fopen(path, "wb");
  if (!f)
    return false;
  size_t len = strlen(json);
  bool wrote = fwrite(json, 1, len, f) == len;
  fclose(f);
  if (!wrote) {
    remove(path);
    return false;
  }

  jsoc_arena *a = jsoc_arena_create(256);
  jsoc_result r = jsoc_tokenize_file(path, a);
  bool ok = r.error.code == JSOC_OK && r.count == 6;
  if (ok) {
    ok = r.tokens[0].type == JSOC_TOK_OBJ_BEGIN &&
         r.tokens[1].type == JSOC_TOK_STRING && r.tokens[1].offset == 2 &&
         r.tokens[1].length == 4 && r.tokens[2].type == JSOC_TOK_COLON &&
         r.tokens[3].type == JSOC_TOK_TRUE &&
         r.tokens[4].type == JSOC_TOK_OBJ_END &&
         r.tokens[5].type == JSOC_TOK_EOF;
  }
  jsoc_arena_destroy(a);
  remove(path);
  return ok;
}

/* --- Differential fuzz ----------------------------------------------------
 *
 * The fused SIMD tokenizer is an optimisation of the portable reference, so
 * the two must agree on every observable detail: the error code and offset,
 * the token count, and every token's type/offset/length.  These cases mix
 * valid documents, whitespace perturbations, single-byte mutations, and raw
 * garbage so both the happy path and every error classification are hit.
 */

static uint64_t fuzz_state = 0x9e3779b97f4a7c15ULL;

static uint32_t fuzz_rand(void) {
  fuzz_state ^= fuzz_state << 13;
  fuzz_state ^= fuzz_state >> 7;
  fuzz_state ^= fuzz_state << 17;
  return (uint32_t)(fuzz_state >> 16);
}

static const unsigned char fuzz_bytes[] =
    "{}[]:,\"0123456789-+.eE truefalsn\\/bfnrtu\x01\x1f\x7f\x80\xc3\xa9";

/* Compare two results for exact agreement. */
static bool fuzz_same_one(jsoc_result r1, jsoc_result r2, const char *s,
                          size_t n, const char *what) {
  bool same = r1.error.code == r2.error.code &&
              r1.error.offset == r2.error.offset && r1.count == r2.count;
  if (same) {
    for (size_t i = 0; i < r1.count; i++) {
      if (r1.tokens[i].type != r2.tokens[i].type ||
          r1.tokens[i].offset != r2.tokens[i].offset ||
          r1.tokens[i].length != r2.tokens[i].length) {
        same = false;
        break;
      }
    }
  }
  if (!same) {
    printf("%s: simd(code=%d off=%u cnt=%zu) scalar(code=%d off=%u cnt=%zu)\n  "
           "input: ",
           what, (int)r1.error.code, (unsigned)r1.error.offset, r1.count,
           (int)r2.error.code, (unsigned)r2.error.offset, r2.count);
    for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      if (c >= 0x20 && c < 0x7f && c != '\\') {
        putchar(c);
      } else {
        printf("\\x%02x", (unsigned)c);
      }
    }
    putchar('\n');
  }
  return same;
}

/* The compact stream drops ',' and ':' tokens; everything else about it must
 * be identical to the strict stream, including the error and the partial
 * token prefix. */
static bool fuzz_same_compact(jsoc_result compact, jsoc_result strict,
                              const char *s, size_t n) {
  bool same = compact.error.code == strict.error.code &&
              compact.error.offset == strict.error.offset;
  if (same) {
    size_t j = 0;
    for (size_t i = 0; i < strict.count; i++) {
      if (strict.tokens[i].type == JSOC_TOK_COMMA ||
          strict.tokens[i].type == JSOC_TOK_COLON) {
        continue;
      }
      if (j >= compact.count ||
          compact.tokens[j].type != strict.tokens[i].type ||
          compact.tokens[j].offset != strict.tokens[i].offset ||
          compact.tokens[j].length != strict.tokens[i].length) {
        same = false;
        break;
      }
      j++;
    }
    if (same && j != compact.count) {
      same = false;
    }
  }
  if (!same) {
    printf("compact(strict code=%d off=%u cnt=%zu) code=%d off=%u cnt=%zu\n  "
           "input: ",
           (int)strict.error.code, (unsigned)strict.error.offset, strict.count,
           (int)compact.error.code, (unsigned)compact.error.offset,
           compact.count);
    for (size_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      if (c >= 0x20 && c < 0x7f && c != '\\') {
        putchar(c);
      } else {
        printf("\\x%02x", (unsigned)c);
      }
    }
    putchar('\n');
  }
  return same;
}

/* Both backends, every mode (strict, checked, stream, compact) must agree
 * exactly: error code, offset, token count and every token field. */
static bool fuzz_same(const char *s, size_t n) {
  jsoc_arena *a[8];
  for (size_t i = 0; i < 8; i++) {
    a[i] = jsoc_arena_create(4096);
    if (!a[i]) {
      for (size_t j = 0; j < i; j++) {
        jsoc_arena_destroy(a[j]);
      }
      return false;
    }
  }

  size_t overflow;
  const jsoc_result strict_simd = jsoc_tokenize_simd_impl(s, n, a[0]);
  const jsoc_result strict_scalar = jsoc_tokenize_scalar_impl(s, n, a[1]);
  const jsoc_result checked_simd =
      jsoc_tokenize_simd_checked_impl(s, n, a[2], NULL);
  const jsoc_result checked_scalar =
      jsoc_tokenize_scalar_checked_impl(s, n, a[3], NULL);
  const jsoc_result stream_simd =
      jsoc_tokenize_simd_stream_impl(s, n, a[4], NULL);
  const jsoc_result stream_scalar =
      jsoc_tokenize_scalar_stream_impl(s, n, a[5], NULL);
  const jsoc_result compact_simd =
      jsoc_tokenize_simd_compact_impl(s, n, a[6], &overflow);
  const jsoc_result compact_scalar =
      jsoc_tokenize_scalar_compact_impl(s, n, a[7], &overflow);

  const bool ok =
      fuzz_same_one(strict_simd, strict_scalar, s, n, "strict") &&
      fuzz_same_one(checked_simd, checked_scalar, s, n, "checked") &&
      fuzz_same_one(stream_simd, stream_scalar, s, n, "stream") &&
      /* Compact must be identical between backends, and agree with strict. */
      fuzz_same_one(compact_simd, compact_scalar, s, n, "compact") &&
      fuzz_same_compact(compact_simd, strict_simd, s, n);

  for (size_t i = 0; i < 8; i++) {
    jsoc_arena_destroy(a[i]);
  }
  return ok;
}

/* Generate a random (usually valid) JSON document. */
static size_t fuzz_gen(char *buf, size_t cap, int depth) {
  size_t n = 0;
#define PUT(ch)                                                                \
  do {                                                                         \
    if (n < cap)                                                               \
      buf[n++] = (char)(ch);                                                   \
  } while (0)
  switch (fuzz_rand() % (depth > 3 ? 4u : 8u)) {
  case 0: { /* object */
    PUT('{');
    unsigned items = fuzz_rand() % 4u;
    for (unsigned i = 0; i < items; i++) {
      if (i != 0)
        PUT(',');
      unsigned sl = fuzz_rand() % 5u;
      PUT('"');
      for (unsigned j = 0; j < sl; j++) {
        PUT("abcdefghij"[fuzz_rand() % 10u]);
      }
      PUT('"');
      PUT(':');
      n += fuzz_gen(buf + n, cap - n, depth + 1);
    }
    PUT('}');
    break;
  }
  case 1: { /* array */
    PUT('[');
    unsigned items = fuzz_rand() % 5u;
    for (unsigned i = 0; i < items; i++) {
      if (i != 0)
        PUT(',');
      n += fuzz_gen(buf + n, cap - n, depth + 1);
    }
    PUT(']');
    break;
  }
  case 2: { /* number */
    if ((fuzz_rand() & 1u) != 0)
      PUT('-');
    unsigned digits = 1u + fuzz_rand() % 4u;
    for (unsigned i = 0; i < digits; i++) {
      PUT('0' + (char)(fuzz_rand() % 10u));
    }
    if ((fuzz_rand() & 1u) != 0) {
      PUT('.');
      unsigned frac = 1u + fuzz_rand() % 3u;
      for (unsigned i = 0; i < frac; i++) {
        PUT('0' + (char)(fuzz_rand() % 10u));
      }
    }
    if ((fuzz_rand() & 1u) != 0) {
      PUT((fuzz_rand() & 1u) != 0 ? 'e' : 'E');
      if ((fuzz_rand() & 1u) != 0)
        PUT((fuzz_rand() & 1u) != 0 ? '+' : '-');
      unsigned exp = 1u + fuzz_rand() % 2u;
      for (unsigned i = 0; i < exp; i++) {
        PUT('0' + (char)(fuzz_rand() % 10u));
      }
    }
    break;
  }
  case 3: { /* string */
    PUT('"');
    unsigned sl = fuzz_rand() % 8u;
    for (unsigned j = 0; j < sl; j++) {
      unsigned r = fuzz_rand() % 12u;
      if (r == 0) {
        PUT('\\');
        PUT("\"\\/bfnrtu"[fuzz_rand() % 9u]);
      } else if (r == 1) {
        PUT('\\');
        PUT('u');
        PUT('0');
        PUT('0');
        PUT('4');
        PUT('1');
      } else {
        PUT("abcdefghijkl"[r % 12u]);
      }
    }
    PUT('"');
    break;
  }
  case 4:
    PUT('t');
    PUT('r');
    PUT('u');
    PUT('e');
    break;
  case 5:
    PUT('f');
    PUT('a');
    PUT('l');
    PUT('s');
    PUT('e');
    break;
  case 6:
    PUT('n');
    PUT('u');
    PUT('l');
    PUT('l');
    break;
  default:
    PUT('0');
    break;
  }
#undef PUT
  return n;
}

static bool test_differential_fuzz(void) {
  char buf[4096];
  const size_t rounds = 4000;
  for (size_t i = 0; i < rounds; i++) {
    size_t n = fuzz_gen(buf, 1024, 0);

    /* Insert whitespace at random positions. */
    if (fuzz_rand() % 3u == 0) {
      char out[2048];
      size_t on = 0;
      for (size_t k = 0; k < n; k++) {
        if (fuzz_rand() % 5u == 0)
          out[on++] = " \t\n\r"[fuzz_rand() % 4u];
        out[on++] = buf[k];
        if (fuzz_rand() % 7u == 0)
          out[on++] = " \t\n\r"[fuzz_rand() % 4u];
      }
      memcpy(buf, out, on);
      n = on;
    }

    /* Corrupt one byte. */
    if (fuzz_rand() % 4u == 0 && n > 1) {
      buf[fuzz_rand() % n] =
          (char)fuzz_bytes[fuzz_rand() % (sizeof(fuzz_bytes) - 1u)];
    }

    if (!fuzz_same(buf, n)) {
      printf("differential mismatch (structured case %zu)\n", i);
      return false;
    }
  }

  /* Raw garbage: exercises invalid starts, lone punctuation and junk after
   * scalars, none of which the structured generator produces. */
  for (size_t i = 0; i < rounds; i++) {
    size_t n = fuzz_rand() % 300u;
    for (size_t k = 0; k < n; k++) {
      buf[k] = (char)fuzz_bytes[fuzz_rand() % (sizeof(fuzz_bytes) - 1u)];
    }
    if (!fuzz_same(buf, n)) {
      printf("differential mismatch (random case %zu)\n", i);
      return false;
    }
  }

  /* Long strings whose escapes and control bytes land at every possible
   * offset relative to the 64-byte SIMD block: this is where the deferred
   * string validator's quote pairing crosses blocks.  The leading padding
   * shifts the document so both the opening and closing quotes move across
   * block boundaries. */
  for (size_t r = 0; r < 2000; r++) {
    size_t n = 0;
    size_t pad = fuzz_rand() % 200u;
    for (size_t i = 0; i < pad; i++)
      buf[n++] = ' ';
    buf[n++] = '[';
    unsigned items = 1u + fuzz_rand() % 3u;
    for (unsigned it = 0; it < items; it++) {
      if (it != 0)
        buf[n++] = ',';
      buf[n++] = '"';
      unsigned len = fuzz_rand() % 220u;
      for (unsigned k = 0; k < len; k++) {
        unsigned t = fuzz_rand() % 100u;
        if (t < 12) {
          buf[n++] = '\\';
          buf[n++] = "\"\\/bfnrtu "[fuzz_rand() % 10u];
        } else if (t < 15) {
          static const char controls[] = {0x01, 0x1f, 0x7f};
          buf[n++] = controls[fuzz_rand() % 3u];
        } else {
          static const char plain[] = {'a', 'b', 'c', 'X', 'Y', 'Z',
                                       '0', '1', '9', ' ', '\t'};
          buf[n++] = plain[fuzz_rand() % (sizeof plain)];
        }
      }
      if (fuzz_rand() % 12u != 0)
        buf[n++] = '"';
    }
    if (fuzz_rand() % 12u != 0)
      buf[n++] = ']';
    if (fuzz_rand() % 20u == 0)
      n = (size_t)(fuzz_rand() % n);

    if (!fuzz_same(buf, n)) {
      printf("differential mismatch (long string case %zu)\n", r);
      return false;
    }
  }

  return true;
}

int main(void) {
  printf("=== jsoc test suite ===\n\n");

  TEST(1, empty_object);
  TEST(2, empty_array);
  TEST(3, simple_object);
  TEST(4, nested_objects);
  TEST(5, array_of_objects);
  TEST(6, strings_with_escapes);
  TEST(7, unicode_escape);
  TEST(8, numbers);
  TEST(9, literals);
  TEST(10, whitespace);
  TEST(11, error_unterminated_string);
  TEST(12, error_unterminated_array);
  TEST(13, error_unterminated_object);
  TEST(14, error_invalid_escape);
  TEST(15, error_invalid_unicode);
  TEST(16, error_trailing_garbage);
  TEST(17, error_mismatched_brackets);
  TEST(18, error_expected_colon);
  TEST(19, error_expected_comma);
  TEST(20, example_json);
  TEST(21, arena_basic);
  TEST(22, arena_growth);
  TEST(23, token_print);
  TEST(24, large_input);
  TEST(25, deep_nesting);
  TEST(26, strict_numbers);
  TEST(27, tokenize_file);
  TEST(28, missing_value);
  TEST(29, differential_fuzz);

  printf("\n=== Results: %d/%d passed (%d failed) ===\n", tests_passed,
         tests_run, tests_failed);
  return tests_failed == 0 ? 0 : 1;
}