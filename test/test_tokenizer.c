#define _POSIX_C_SOURCE 200809L
#include "jsoc.h"
#include "token.h"
#include "arena.h"
#include "error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(num, name) \
    do { \
        tests_run++; \
        printf("TEST %d: %s ... ", num, #name); \
        fflush(stdout); \
        if (test_##name()) { \
            tests_passed++; \
            printf("PASS\n"); \
        } else { \
            tests_failed++; \
            printf("FAIL\n"); \
        } \
    } while (0)

static bool test_empty_object(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("{}", 2, a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 3) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_empty_array(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("[]", 2, a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 3) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_ARR_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_ARR_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_simple_object(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "{\"a\":1}";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 6) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_STRING) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].offset != 2) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].length != 1) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_COLON) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].type != JSOC_TOK_NUMBER) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].offset != 5) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].length != 1) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[4].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[5].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_nested_objects(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "{\"a\":{\"b\":2}}";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 10) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_STRING) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_COLON) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[4].type != JSOC_TOK_STRING) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[5].type != JSOC_TOK_COLON) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[6].type != JSOC_TOK_NUMBER) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[7].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[8].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[9].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_array_of_objects(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "[{},{}]";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 8) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_ARR_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].type != JSOC_TOK_COMMA) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[4].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[5].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[6].type != JSOC_TOK_ARR_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[7].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_strings_with_escapes(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "\"hello\\nworld\\t\\\"\\\\\"";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 2) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_unicode_escape(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "\"\\u0041\\u00e9\"";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 2) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_numbers(void) {
    const char *nums[] = {
        "0", "-0", "42", "-42", "3.14", "-3.14",
        "1e10", "1E10", "1e+10", "1e-10",
        "3.14e-10", "-1.5E+5"
    };
    for (size_t i = 0; i < sizeof(nums)/sizeof(nums[0]); i++) {
        jsoc_arena *a = jsoc_arena_create(1024);
        jsoc_result r = jsoc_tokenize(nums[i], strlen(nums[i]), a);
        if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
        if (r.count != 2) { jsoc_arena_destroy(a); return false; }
        if (r.tokens[0].type != JSOC_TOK_NUMBER) { jsoc_arena_destroy(a); return false; }
        if (r.tokens[1].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
        jsoc_arena_destroy(a);
    }
    return true;
}

static bool test_literals(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("true false null", 15, a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 4) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_TRUE) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_FALSE) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_NULL) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[3].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_whitespace(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    const char *json = "  \t\n\r  {  \t\n\r  }  \t\n\r";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 3) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[0].type != JSOC_TOK_OBJ_BEGIN) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[1].type != JSOC_TOK_OBJ_END) { jsoc_arena_destroy(a); return false; }
    if (r.tokens[2].type != JSOC_TOK_EOF) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_unterminated_string(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("\"hello", 6, a);
    if (r.error.code != JSOC_ERR_UNTERMINATED_STRING) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_unterminated_array(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("[1,2", 4, a);
    if (r.error.code != JSOC_ERR_UNTERMINATED_ARRAY) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_unterminated_object(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("{\"a\":1", 6, a);
    if (r.error.code != JSOC_ERR_UNTERMINATED_OBJECT) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_invalid_escape(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("\"\\x\"", 4, a);
    if (r.error.code != JSOC_ERR_INVALID_ESCAPE) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_invalid_unicode(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("\"\\uGGGG\"", 8, a);
    if (r.error.code != JSOC_ERR_INVALID_UNICODE) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_trailing_garbage(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("{} extra", 8, a);
    if (r.error.code != JSOC_ERR_TRAILING_GARBAGE) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_mismatched_brackets(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("[}", 2, a);
    if (r.error.code != JSOC_ERR_SYNTAX) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_expected_colon(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("{\"a\" 1}", 7, a);
    if (r.error.code != JSOC_ERR_EXPECTED_COLON) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_error_expected_comma(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    jsoc_result r = jsoc_tokenize("[1 2]", 5, a);
    if (r.error.code != JSOC_ERR_EXPECTED_COMMA_OR_END) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_example_json(void) {
    jsoc_arena *a = jsoc_arena_create(4096);
    const char *json = "[{\"foo\":\"bar\",\"bin\":\"baz\"},{},{},[],{\"test\":\"hello\"}]";
    jsoc_result r = jsoc_tokenize(json, strlen(json), a);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 27) { jsoc_arena_destroy(a); return false; }
    int counts[12] = {0};
    for (size_t i = 0; i < r.count; i++) counts[r.tokens[i].type]++;
    if (counts[JSOC_TOK_OBJ_BEGIN] != 4) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_OBJ_END] != 4) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_ARR_BEGIN] != 2) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_ARR_END] != 2) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_COMMA] != 5) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_STRING] != 6) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_COLON] != 3) { jsoc_arena_destroy(a); return false; }
    if (counts[JSOC_TOK_EOF] != 1) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_arena_basic(void) {
    jsoc_arena *a = jsoc_arena_create(1024);
    void *p1 = jsoc_arena_alloc(a, 100, 16);
    if (!p1) { jsoc_arena_destroy(a); return false; }
    if (((uintptr_t)p1 % 16) != 0) { jsoc_arena_destroy(a); return false; }
    void *p2 = jsoc_arena_alloc(a, 200, 32);
    if (!p2) { jsoc_arena_destroy(a); return false; }
    if (((uintptr_t)p2 % 32) != 0) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_reset(a);
    void *p3 = jsoc_arena_alloc(a, 50, 8);
    if (!p3) { jsoc_arena_destroy(a); return false; }
    if (p3 != a->base) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_arena_growth(void) {
    jsoc_arena *a = jsoc_arena_create(64);
    for (int i = 0; i < 1000; i++) {
        void *p = jsoc_arena_alloc(a, 100, 16);
        if (!p) { jsoc_arena_destroy(a); return false; }
    }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_token_print(void) {
    jsoc_token tok = { .offset = 4, .length = 3, .type = JSOC_TOK_STRING };
    char buf[64];
    jsoc_token_print(&tok, "abc\"hello\"def", buf, sizeof(buf));
    if (strcmp(buf, "STRING(hel)") != 0) return false;
    return true;
}

static bool test_large_input(void) {
    size_t n = 100000;
    char *json = malloc(n + 3);
    json[0] = '[';
    for (size_t i = 0; i < n/2; i++) json[1+i] = (char)('1' + (i % 9));
    json[1+n/2] = ']';
    json[2+n/2] = '\0';
    size_t json_len = 1 + n/2 + 1;  // '[' + n/2 digits + ']'

    jsoc_arena *a = jsoc_arena_create(4096);
    jsoc_result r = jsoc_tokenize(json, json_len, a);
    free(json);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != 4) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_deep_nesting(void) {
    size_t depth = 1000;
    char *json = malloc(depth * 2 + 1);
    for (size_t i = 0; i < depth; i++) json[i] = '[';
    for (size_t i = 0; i < depth; i++) json[depth + i] = ']';
    json[depth * 2] = '\0';

    jsoc_arena *a = jsoc_arena_create(4096);
    jsoc_result r = jsoc_tokenize(json, depth * 2, a);
    free(json);
    if (r.error.code != JSOC_OK) { jsoc_arena_destroy(a); return false; }
    if (r.count != depth * 2 + 1) { jsoc_arena_destroy(a); return false; }
    jsoc_arena_destroy(a);
    return true;
}

static bool test_strict_numbers(void) {
    const char *bad[] = { "[01]", "[1.]", "[1e]", "[-]", "[+1]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        jsoc_arena *a = jsoc_arena_create(1024);
        jsoc_result r = jsoc_tokenize(bad[i], strlen(bad[i]), a);
        bool ok = r.error.code != JSOC_OK;
        jsoc_arena_destroy(a);
        if (!ok) return false;
    }
    return true;
}

static bool test_missing_value(void) {
    const char *bad[] = { "{\"a\":}", "{\"a\":,}", "[1,]", "{\"a\":1,}" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        jsoc_arena *a = jsoc_arena_create(1024);
        jsoc_result r = jsoc_tokenize(bad[i], strlen(bad[i]), a);
        bool ok = r.error.code != JSOC_OK;
        jsoc_arena_destroy(a);
        if (!ok) return false;
    }
    return true;
}

static bool test_tokenize_file(void) {
    const char *path = "/tmp/jsoc_test_tokenize_file.json";
    const char *json = "{\"file\":true}";
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(json);
    bool wrote = fwrite(json, 1, len, f) == len;
    fclose(f);
    if (!wrote) { remove(path); return false; }

    jsoc_arena *a = jsoc_arena_create(256);
    jsoc_result r = jsoc_tokenize_file(path, a);
    bool ok = r.error.code == JSOC_OK && r.count == 6;
    if (ok) {
        ok = r.tokens[0].type == JSOC_TOK_OBJ_BEGIN &&
             r.tokens[1].type == JSOC_TOK_STRING &&
             r.tokens[1].offset == 2 && r.tokens[1].length == 4 &&
             r.tokens[2].type == JSOC_TOK_COLON &&
             r.tokens[3].type == JSOC_TOK_TRUE &&
             r.tokens[4].type == JSOC_TOK_OBJ_END &&
             r.tokens[5].type == JSOC_TOK_EOF;
    }
    jsoc_arena_destroy(a);
    remove(path);
    return ok;
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

    printf("\n=== Results: %d/%d passed (%d failed) ===\n", tests_passed, tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}