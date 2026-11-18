/*
 * Descriptor-driven serialization (include/encode.h).
 *
 * The encoder is the mirror of the decoder, so most checks are written as
 * encode -> parse -> decode round trips: whatever the encoder emits must be
 * valid JSON that decodes back to the same struct.  The remaining tests pin
 * the zero-copy STRING_RAW path, the capacity contract, and the hostile-count
 * guard.
 */
#include "test.h"
#include "decode.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

struct point {
  int64_t x;
  int64_t y;
};

struct item {
  int64_t id;
  jsoc_string name;   /* decoded, escaped on output */
  jsoc_string raw;    /* raw slice from a validated document */
  struct point pos;   /* nested struct */
  double score;
  bool active;
};

struct doc {
  jsoc_string title;
  struct item *items;
  size_t item_count;
  int64_t *nums;
  size_t num_count;
};

static const jsoc_encode_field point_enc[] = {
    JSOC_ENCODE_FIELD_A(point, x, JSOC_ENCODE_INT64),
    JSOC_ENCODE_FIELD_A(point, y, JSOC_ENCODE_INT64),
    {.name = NULL},
};

static const jsoc_encode_field item_enc[] = {
    JSOC_ENCODE_FIELD_A(item, id, JSOC_ENCODE_INT64),
    JSOC_ENCODE_FIELD_A(item, name, JSOC_ENCODE_STRING),
    JSOC_ENCODE_FIELD_A(item, raw, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(item, pos, JSOC_ENCODE_STRUCT, .nested = point_enc),
    JSOC_ENCODE_FIELD_A(item, score, JSOC_ENCODE_DOUBLE),
    JSOC_ENCODE_FIELD_A(item, active, JSOC_ENCODE_BOOL),
    {.name = NULL},
};

static const jsoc_encode_field doc_enc[] = {
    JSOC_ENCODE_FIELD_A(doc, title, JSOC_ENCODE_STRING),
    JSOC_ENCODE_FIELD_A(doc, items, JSOC_ENCODE_ARRAY,
                        .count_offset = offsetof(struct doc, item_count),
                        .item_kind = JSOC_ENCODE_STRUCT,
                        .item_size = sizeof(struct item),
                        .items = item_enc,
                        .max_items = 8),
    JSOC_ENCODE_FIELD_A(doc, nums, JSOC_ENCODE_ARRAY,
                        .count_offset = offsetof(struct doc, num_count),
                        .item_kind = JSOC_ENCODE_INT64,
                        .item_size = sizeof(int64_t),
                        .max_items = 8),
    {.name = NULL},
};

static const size_t doc_field_count = 3;

static const jsoc_decode_field point_dec[] = {
    JSOC_DECODE_FIELD_A(point, x, JSOC_DECODE_INT64),
    JSOC_DECODE_FIELD_A(point, y, JSOC_DECODE_INT64),
    {.name = NULL},
};

static const jsoc_decode_field item_dec[] = {
    JSOC_DECODE_FIELD_A(item, id, JSOC_DECODE_INT64),
    JSOC_DECODE_FIELD_A(item, name, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(item, raw, JSOC_DECODE_STRING_SLICE),
    JSOC_DECODE_FIELD_A(item, pos, JSOC_DECODE_STRUCT, .nested = point_dec),
    JSOC_DECODE_FIELD_A(item, score, JSOC_DECODE_DOUBLE),
    JSOC_DECODE_FIELD_A(item, active, JSOC_DECODE_BOOL),
    {.name = NULL},
};

static const jsoc_decode_field doc_dec[] = {
    JSOC_DECODE_FIELD_A(doc, title, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(doc, items, JSOC_DECODE_ARRAY,
                        .count_offset = offsetof(struct doc, item_count),
                        .item_kind = JSOC_DECODE_STRUCT,
                        .item_size = sizeof(struct item),
                        .items = item_dec),
    JSOC_DECODE_FIELD_A(doc, nums, JSOC_DECODE_ARRAY,
                        .count_offset = offsetof(struct doc, num_count),
                        .item_kind = JSOC_DECODE_INT64,
                        .item_size = sizeof(int64_t)),
    {.name = NULL},
};

static bool test_round_trip(void) {
  jsoc_arena *a = jsoc_arena_create(65536);
  REQUIRE(a != NULL);

  /* A source document supplies the zero-copy raw slice for `raw`. */
  const char *source = "{\"r\":\"\\u0041\\n\"}";
  jsoc_document sd = jsoc_parse(source, strlen(source), a);
  REQUIRE(sd.error.code == JSOC_OK);
  const jsoc_value *rv = jsoc_object_get_cstr(&sd, sd.root, "r");
  REQUIRE(rv != NULL);
  jsoc_string raw = jsoc_value_raw(&sd, rv);
  REQUIRE(raw.length == 8);

  struct item items[2] = {
      {.id = -7,
       .name = {("a\"b\\c\n\x01"), 7},
       .raw = raw,
       .pos = {-3, 12},
       .score = 0.5,
       .active = true},
      {.id = 9007199254740993LL,
       .name = {"plain", 5},
       .raw = {NULL, 0},
       .pos = {0, 0},
       .score = 1e300,
       .active = false},
  };
  int64_t nums[3] = {-1, 0, 9223372036854775807LL};
  struct doc in = {
      .title = {"T\"x", 3},
      .items = items,
      .item_count = 2,
      .nums = nums,
      .num_count = 3,
  };

  jsoc_string out;
  REQUIRE(jsoc_encode_object(doc_enc, doc_field_count, &in, a, &out));
  REQUIRE(out.data != NULL && out.length > 0);

  jsoc_document d = jsoc_parse(out.data, out.length, a);
  REQUIRE(d.error.code == JSOC_OK);

  struct item decoded[8] = {0};
  struct doc got = {.items = decoded, .item_count = 8, .nums = nums,
                    .num_count = 3};
  jsoc_decode_result r =
      jsoc_decode_object(doc_dec, doc_field_count, &d, d.root, &got);
  REQUIRE(r.code == JSOC_DECODE_OK);
  REQUIRE(got.item_count == 2 && got.num_count == 3);
  REQUIRE(got.title.length == 3 && memcmp(got.title.data, "T\"x", 3) == 0);
  REQUIRE(decoded[0].id == -7);
  REQUIRE(decoded[0].name.length == 7 &&
          memcmp(decoded[0].name.data, "a\"b\\c\n\x01", 7) == 0);
  REQUIRE(decoded[0].pos.x == -3 && decoded[0].pos.y == 12);
  REQUIRE(decoded[0].score == 0.5 && decoded[0].active == true);
  REQUIRE(decoded[1].id == 9007199254740993LL);
  REQUIRE(decoded[1].score == 1e300 && decoded[1].active == false);
  REQUIRE(nums[0] == -1 && nums[1] == 0 && nums[2] == 9223372036854775807LL);

  /* The raw slice survives verbatim: same bytes, no double escaping. */
  REQUIRE(decoded[0].raw.length == 8);
  REQUIRE(memcmp(decoded[0].raw.data, "\\u0041\\n", 8) == 0);
  /* ...and still decodes to the same logical string. */
  {
    const jsoc_value *v = jsoc_object_get_cstr(&d, d.root, "items");
    REQUIRE(v != NULL);
    const jsoc_value *first = jsoc_array_at(&d, v, 0);
    const jsoc_value *rawv = jsoc_object_get_cstr(&d, first, "raw");
    REQUIRE(rawv != NULL);
    jsoc_string logical = jsoc_value_get_string(&d, rawv);
    REQUIRE(logical.length == 2 && memcmp(logical.data, "A\n", 2) == 0);
  }

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_escaping(void) {
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);

  /* Every escaped form the writer supports, including a C0 byte. */
  const char control[] = {'"', '\\', '\b', '\f', '\n', '\r', '\t', 1, 'x'};
  struct doc d = {.title = {control, sizeof control}};

  jsoc_string out;
  REQUIRE(jsoc_encode_object(doc_enc, doc_field_count, &d, a, &out));
  /* No raw control byte may appear in the output. */
  for (size_t i = 0; i < out.length; i++) {
    REQUIRE((unsigned char)out.data[i] >= 0x20);
  }

  jsoc_document parsed = jsoc_parse(out.data, out.length, a);
  REQUIRE(parsed.error.code == JSOC_OK);
  const jsoc_value *t = jsoc_object_get_cstr(&parsed, parsed.root, "title");
  REQUIRE(t != NULL);
  jsoc_string back = jsoc_value_get_string(&parsed, t);
  REQUIRE(back.length == sizeof control);
  REQUIRE(memcmp(back.data, control, sizeof control) == 0);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_capacity_contract(void) {
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);

  struct item one = {.id = 42, .score = 2.5, .active = true};
  struct doc d = {.items = &one, .item_count = 1};

  const size_t bound = jsoc_encode_size(doc_enc, doc_field_count, &d);
  REQUIRE(bound > 2);

  jsoc_string out;
  REQUIRE(jsoc_encode_object_hinted(doc_enc, doc_field_count, &d, bound, a,
                                    &out));
  REQUIRE(out.length > 0 && out.length <= bound);

  /* The exact bound is enough; one byte less must fail cleanly rather than
   * write a truncated document. */
  jsoc_string tight = {NULL, 0};
  REQUIRE(!jsoc_encode_object_hinted(doc_enc, doc_field_count, &d,
                                     out.length - 1, a, &tight));
  REQUIRE(tight.data == NULL && tight.length == 0);

  /* `jsoc_encode_object` allocates from the bound and succeeds. */
  jsoc_string auto_out;
  REQUIRE(jsoc_encode_object(doc_enc, doc_field_count, &d, a, &auto_out));
  REQUIRE(auto_out.length == out.length);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_scalar_values(void) {
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);

  struct pair {
    int64_t i;
    uint64_t u;
    double d;
    const char *s;
    jsoc_string raw;
    bool b;
  };
  static const jsoc_encode_field pair_enc[] = {
      JSOC_ENCODE_FIELD_A(pair, i, JSOC_ENCODE_INT64),
      JSOC_ENCODE_FIELD_A(pair, u, JSOC_ENCODE_UINT64),
      JSOC_ENCODE_FIELD_A(pair, d, JSOC_ENCODE_DOUBLE),
      JSOC_ENCODE_FIELD_A(pair, s, JSOC_ENCODE_CSTR),
      JSOC_ENCODE_FIELD_A(pair, raw, JSOC_ENCODE_RAW),
      JSOC_ENCODE_FIELD_A(pair, b, JSOC_ENCODE_BOOL),
      {.name = NULL},
  };
  struct pair p = {.i = INT64_MIN,
                   .u = UINT64_MAX,
                   .d = -0.0,
                   .s = "q\"z",
                   .raw = {"[1,2]", 5},
                   .b = false};

  jsoc_string out;
  REQUIRE(jsoc_encode_object(pair_enc, 6, &p, a, &out));
  jsoc_document d = jsoc_parse(out.data, out.length, a);
  REQUIRE(d.error.code == JSOC_OK);

  int64_t i = 0;
  uint64_t u = 0;
  double dv = 0;
  bool b = true;
  REQUIRE(jsoc_value_get_int64(&d, jsoc_object_get_cstr(&d, d.root, "i"), &i));
  REQUIRE(i == INT64_MIN);
  REQUIRE(jsoc_value_get_uint64(&d, jsoc_object_get_cstr(&d, d.root, "u"),
                                &u));
  REQUIRE(u == UINT64_MAX);
  REQUIRE(jsoc_value_get_double(&d, jsoc_object_get_cstr(&d, d.root, "d"),
                                &dv));
  REQUIRE(1.0 / dv < 0.0); /* -0.0 preserved */
  REQUIRE(jsoc_value_get_bool(jsoc_object_get_cstr(&d, d.root, "b"), &b));
  REQUIRE(b == false);

  const jsoc_value *s = jsoc_object_get_cstr(&d, d.root, "s");
  REQUIRE(s != NULL);
  jsoc_string ss = jsoc_value_get_string(&d, s);
  REQUIRE(ss.length == 3 && memcmp(ss.data, "q\"z", 3) == 0);

  const jsoc_value *raw = jsoc_object_get_cstr(&d, d.root, "raw");
  REQUIRE(raw != NULL && jsoc_value_is_array(raw));
  REQUIRE(jsoc_array_size(&d, raw) == 2);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_double_round_trip(void) {
  jsoc_arena *a = jsoc_arena_create(4096);
  REQUIRE(a != NULL);

  struct holder {
    double d;
  };
  static const jsoc_encode_field holder_enc[] = {
      JSOC_ENCODE_FIELD_A(holder, d, JSOC_ENCODE_DOUBLE),
      {.name = NULL},
  };

  static const double values[] = {
      0.0, 1.0, -1.0, 0.1, 1.0 / 3.0, 1e-300, 1e300, 4.9406564584124654e-324,
      1.7976931348623157e308, 2.2250738585072014e-308, 123456789.125,
  };
  for (size_t k = 0; k < sizeof values / sizeof values[0]; k++) {
    struct holder h = {.d = values[k]};
    jsoc_string out;
    REQUIRE(jsoc_encode_object(holder_enc, 1, &h, a, &out));
    jsoc_document d = jsoc_parse(out.data, out.length, a);
    REQUIRE(d.error.code == JSOC_OK);
    double back = 0;
    const jsoc_value *dv = jsoc_object_get_cstr(&d, d.root, "d");
    REQUIRE(dv != NULL && jsoc_value_get_double(&d, dv, &back));
    REQUIRE(memcmp(&back, &values[k], sizeof back) == 0);
    jsoc_arena_reset(a);
  }

  /* Non-finite values become null, keeping the text valid. */
  struct holder inf = {.d = INFINITY};
  jsoc_string out;
  REQUIRE(jsoc_encode_object(holder_enc, 1, &inf, a, &out));
  REQUIRE(out.length == 10 && memcmp(out.data, "{\"d\":null}", 10) == 0);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_hostile_count(void) {
  jsoc_arena *a = jsoc_arena_create(65536);
  REQUIRE(a != NULL);

  struct item two[2] = {{.id = 1}, {.id = 2}};
  struct doc d = {.items = two, .item_count = SIZE_MAX};

  jsoc_string out;
  /* A count past the declared maximum fails instead of iterating a buffer
   * that may be shorter than the count claims. */
  REQUIRE(!jsoc_encode_object(doc_enc, doc_field_count, &d, a, &out));
  REQUIRE(out.data == NULL && out.length == 0);

  /* A NULL element pointer writes an empty array, not a crash. */
  struct doc empty = {.items = NULL, .item_count = 5, .nums = NULL,
                      .num_count = 5};
  jsoc_string e;
  REQUIRE(jsoc_encode_object(doc_enc, doc_field_count, &empty, a, &e));
  jsoc_document ed = jsoc_parse(e.data, e.length, a);
  REQUIRE(ed.error.code == JSOC_OK);
  const jsoc_value *ea = jsoc_object_get_cstr(&ed, ed.root, "items");
  REQUIRE(ea != NULL && jsoc_array_size(&ed, ea) == 0);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

int main(void) {
  printf("=== jsoc encode tests ===\n\n");

  TEST(1, round_trip);
  TEST(2, escaping);
  TEST(3, capacity_contract);
  TEST(4, scalar_values);
  TEST(5, double_round_trip);
  TEST(6, hostile_count);

  return jsoc_test_finish("encode");
}
