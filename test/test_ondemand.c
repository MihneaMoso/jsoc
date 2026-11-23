/* On-demand decode: scan the structural index array and decode into structs
 * without materialising a DOM.  These tests pin the compact-index walk, scalar
 * conversion, string handling and error results shared with the DOM decoder. */

#include "test.h"

#include "decode.h"
#include "ondemand.h"

#include <stdint.h>
#include <string.h>

typedef struct od_person {
  uint64_t id;
  jsoc_string name;
  bool ok;
} od_person;

static const jsoc_decode_field person_fields[] = {
    JSOC_DECODE_FIELD_A(od_person, id, JSOC_DECODE_UINT64, .required = true),
    JSOC_DECODE_FIELD_A(od_person, name, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(od_person, ok, JSOC_DECODE_BOOL),
    {.name = NULL},
};

typedef struct od_people {
  od_person *items;
  size_t count;
} od_people;

static const jsoc_decode_field people_fields[] = {
    JSOC_DECODE_FIELD_A(od_people, items, JSOC_DECODE_ARRAY,
                        .count_offset = offsetof(od_people, count),
                        .item_kind = JSOC_DECODE_STRUCT,
                        .item_size = sizeof(od_person),
                        .items = person_fields),
    {.name = NULL},
};

/* Arena that outlives the decoded strings checked by each test. */
static jsoc_arena *decode_arena;

static jsoc_decode_error decode_text(const char *input,
                                     const jsoc_decode_field *fields,
                                     size_t nfields, void *out) {
  jsoc_ondemand od;
  jsoc_error err;
  jsoc_decode_result r;
  jsoc_arena_reset(decode_arena);
  if (!jsoc_ondemand_scan(&od, input, strlen(input), decode_arena, &err))
    return JSOC_DECODE_CUSTOM;
  r = jsoc_decode_ondemand(fields, nfields, &od, out, decode_arena);
  return r.code;
}

static int test_scalars(void) {
  od_person p;
  memset(&p, 0, sizeof p);
  REQUIRE(decode_text("{\"id\":42,\"name\":\"Ada\",\"ok\":true}",
                      person_fields, 3, &p) == JSOC_DECODE_OK);
  REQUIRE(p.id == 42);
  REQUIRE(p.name.length == 3 && memcmp(p.name.data, "Ada", 3) == 0);
  REQUIRE(p.name.data[3] == '\0'); /* STRING_REF NUL-terminates */
  REQUIRE(p.ok == true);
  return 1;
fail:
  return 0;
}

static int test_unknown_fields(void) {
  od_person p;
  memset(&p, 0, sizeof p);
  /* Unknown scalar, string, nested object and array values are skipped. */
  REQUIRE(decode_text("{\"junk\":[1,{\"x\":2}],\"id\":7,\"other\":\"s\","
                      "\"name\":\"Bob\",\"extra\":{\"a\":[1,2]},"
                      "\"ok\":false}",
                      person_fields, 3, &p) == JSOC_DECODE_OK);
  REQUIRE(p.id == 7);
  REQUIRE(p.name.length == 3 && memcmp(p.name.data, "Bob", 3) == 0);
  REQUIRE(p.ok == false);
  return 1;
fail:
  return 0;
}

static int test_escapes_exact(void) {
  od_person p;
  memset(&p, 0, sizeof p);
  REQUIRE(decode_text("{\"id\":1,\"name\":\"a\\tb\",\"ok\":true}",
                      person_fields, 3, &p) == JSOC_DECODE_OK);
  REQUIRE(p.name.length == 3);
  REQUIRE(memcmp(p.name.data, "a\tb", 3) == 0);
  return 1;
fail:
  return 0;
}

static int test_array(void) {
  od_people v;
  static od_person items[4];
  memset(items, 0, sizeof items);
  v.items = items;
  v.count = 4;
  REQUIRE(decode_text("{\"items\":[{\"id\":1,\"name\":\"a\",\"ok\":true},"
                      "{\"id\":2,\"name\":\"b\",\"ok\":false}]}",
                      people_fields, 1, &v) == JSOC_DECODE_OK);
  REQUIRE(v.count == 2);
  REQUIRE(items[0].id == 1 && items[1].id == 2);
  REQUIRE(items[0].ok == true && items[1].ok == false);
  return 1;
fail:
  return 0;
}

static int test_required(void) {
  od_person p;
  memset(&p, 0, sizeof p);
  jsoc_decode_error code =
      decode_text("{\"name\":\"x\",\"ok\":true}", person_fields, 3, &p);
  REQUIRE(code == JSOC_DECODE_MISSING_FIELD);
  return 1;
fail:
  return 0;
}

static int test_type_mismatch(void) {
  od_person p;
  memset(&p, 0, sizeof p);
  REQUIRE(decode_text("{\"id\":\"nope\",\"ok\":true}",
                      person_fields, 3, &p) == JSOC_DECODE_TYPE_MISMATCH);
  return 1;
fail:
  return 0;
}

static int test_bad_input(void) {
  /* Stage 1 validates UTF-8 but defers escape-sequence validation to the
   * decoder (which only decodes strings it reads).  A bad escape therefore
   * surfaces as a decode error. */
  const char *bad[] = {"{\"id\":1,\"name\":\"\\q\"}",
                       "{\"id\":1,\"name\":\"\\uZZZZ\"}"};
  for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    od_person p;
    memset(&p, 0, sizeof p);
    jsoc_decode_error code = decode_text(bad[i], person_fields, 3, &p);
    REQUIRE(code == JSOC_DECODE_TYPE_MISMATCH || code == JSOC_DECODE_CUSTOM);
  }
  const char *badutf = "\"\xff\"";
  jsoc_ondemand od;
  jsoc_error err;
  REQUIRE(!jsoc_ondemand_scan(&od, badutf, strlen(badutf), decode_arena, &err));
  return 1;
fail:
  return 0;
}

int main(void) {
  decode_arena = jsoc_arena_create(1 << 16);
  if (!decode_arena) return 1;
  TEST(1, scalars);
  TEST(2, unknown_fields);
  TEST(3, escapes_exact);
  TEST(4, array);
  TEST(5, required);
  TEST(6, type_mismatch);
  TEST(7, bad_input);
  jsoc_arena_destroy(decode_arena);
  return jsoc_test_finish("ondemand");
}
