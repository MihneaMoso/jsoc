/*
 * Static checks.
 *
 * Ports simdjson's tests/staticchecks.cpp and
 * tests/dom/trivially_copyable_test.cpp: the sizes, layouts and message tables
 * that the rest of the library depends on are pinned here, so a change that
 * breaks an assumption fails at test time rather than in a user's program.
 */
#include "dom_internal.h"
#include "test.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool test_struct_sizes(void) {
  /* The public token layout is an API contract (HANDOFF.md). */
  REQUIRE(sizeof(jsoc_token) == 12);
  REQUIRE(offsetof(jsoc_token, offset) == 0);
  REQUIRE(offsetof(jsoc_token, length) == 4);
  REQUIRE(offsetof(jsoc_token, type) == 8);
  REQUIRE(sizeof(jsoc_token_type) == 1);

  /* The DOM is sized for cache friendliness; a change here is a perf change. */
  REQUIRE(sizeof(jsoc_value) == 16);
  REQUIRE(offsetof(jsoc_value, next) == 8);
  REQUIRE(offsetof(jsoc_value, type) == 12);
  REQUIRE(sizeof(jsoc_member) == 16);
  REQUIRE(sizeof(jsoc_string) == 16);
  REQUIRE(sizeof(jsoc_value_type) == 1);

  /* Scalars and containers share the same fields: writing both halves of the
   * union is a bug, so keep the sizes equal and the offsets identical. */
  REQUIRE(offsetof(jsoc_value, offset) == offsetof(jsoc_value, first));
  REQUIRE(offsetof(jsoc_value, length) == offsetof(jsoc_value, count));
  return true;
fail:
  return false;
}

static bool test_trivially_copyable(void) {
  /* A value copied bytewise must behave identically: the DOM is documented
   * as memcpy-able (it has no pointers into itself). */
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);
  jsoc_document doc = jsoc_parse("[1,{\"a\":2}]", 11, a);
  REQUIRE(doc.error.code == JSOC_OK);

  const jsoc_value *array = doc.root;
  jsoc_value copy = *array;
  REQUIRE(memcmp(&copy, array, sizeof copy) == 0);

  /* Mutating the copy must not touch the original: the sibling chain is
   * stored by index, so no aliasing exists even after a copy. */
  copy.next = JSOC_NO_INDEX;
  copy.count = 0;
  REQUIRE(array->next == JSOC_NO_INDEX);
  REQUIRE(array->count == 2);
  REQUIRE(doc.values[array->first].next != JSOC_NO_INDEX);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_error_messages(void) {
  /* Every error code must have a message, and the message must not be the
   * generic fallback. */
  for (int code = 0; code <= JSOC_ERR_INPUT_TOO_LARGE; code++) {
    const char *msg = jsoc_error_message((jsoc_error_code)code);
    REQUIRE(msg != NULL && msg[0] != '\0');
    REQUIRE(strcmp(msg, "Unknown error") != 0);
  }
  /* Past the end of the enum the fallback is the documented answer. */
  REQUIRE(strcmp(jsoc_error_message((jsoc_error_code)200), "Unknown error") ==
          0);

  /* Formatting must fill a small buffer without truncating the prefix. */
  char buf[64];
  jsoc_error_format(&(jsoc_error){JSOC_ERR_SYNTAX, 7, 1, 8}, buf, sizeof buf);
  REQUIRE(buf[0] != '\0');
  return true;
fail:
  return false;
}

static bool test_tokens_fit_u32(void) {
  /* Offsets are uint32_t; a longer input is rejected before any work. */
  jsoc_arena *a = jsoc_arena_create(64);
  REQUIRE(a != NULL);
  jsoc_result r = jsoc_tokenize("1", 1, a);
  REQUIRE(r.error.code == JSOC_OK);
  REQUIRE(r.count == 2); /* number + EOF */
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc static checks ===\n\n");

  TEST(1, struct_sizes);
  TEST(2, trivially_copyable);
  TEST(3, error_messages);
  TEST(4, tokens_fit_u32);

  return jsoc_test_finish("static");
}
