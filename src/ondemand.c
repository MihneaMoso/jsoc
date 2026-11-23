#define _GNU_SOURCE 1

#include "ondemand.h"

#include "dom_internal.h"
#include "number.h"
#include "tokenizer_internal.h"

#include <limits.h>
#include <string.h>

/* Stage-1 emits one index per structural token: '{', '}', '[', ']', ',', ':',
 * the two quotes of a string, and the first byte of every scalar.  All byte
 * offsets are absolute.  A value is described entirely by the index position
 * it starts at; containers are skipped by tracking nesting depth. */

enum { OD_SCALAR, OD_STRING, OD_OBJECT, OD_ARRAY };

static inline unsigned char od_at(const jsoc_ondemand *od, size_t p) {
  return (unsigned char)od->input[od->indexes[p]];
}

/* Index position just past the value that starts at `p`. */
static inline size_t od_skip(const jsoc_ondemand *od, size_t p) {
  const unsigned char c = od_at(od, p);
  if (c == '"')
    return p + 2; /* opening quote + closing quote */
  if (c == '{' || c == '[') {
    unsigned depth = 1;
    size_t q = p + 1;
    while (depth) {
      const unsigned char x = od_at(od, q++);
      if (x == '{' || x == '[')
        depth++;
      else if (x == '}' || x == ']')
        depth--;
    }
    return q;
  }
  return p + 1; /* number or literal */
}

/* Length of a scalar lexeme starting at index position `p`.  Stage 1 already
 * validated the token, so scanning to the next non-lexeme byte is enough. */
static inline uint32_t od_scalar_len(const jsoc_ondemand *od, size_t p) {
  const char *s = od->input + od->indexes[p];
  size_t i = 0;
  if (*s == '-' || (*s >= '0' && *s <= '9')) {
    for (;;) {
      const unsigned char c = (unsigned char)s[i];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
          c == 'e' || c == 'E')
        i++;
      else
        break;
    }
  } else {
    while (s[i] >= 'a' && s[i] <= 'z') i++;
  }
  return (uint32_t)i;
}

/* Classify the value at `p`.  For strings, `*body` / `*len` describe the raw
 * bytes between the quotes; for scalars they describe the number/literal
 * lexeme.  Containers leave them untouched. */
static inline int od_value(const jsoc_ondemand *od, size_t p, const char **body,
                           uint32_t *len) {
  const unsigned char c = od_at(od, p);
  if (c == '"') {
    const uint32_t off = od->indexes[p] + 1;
    *body = od->input + off;
    *len = od->indexes[p + 1] - off;
    return OD_STRING;
  }
  if (c == '{') return OD_OBJECT;
  if (c == '[') return OD_ARRAY;
  *body = od->input + od->indexes[p];
  *len = od_scalar_len(od, p);
  return OD_SCALAR;
}

/* Validate a number lexeme that is not a plain positive integer. */
static inline bool od_validate_number(const jsoc_ondemand *od, size_t p,
                                      uint32_t len, jsoc_arena *arena) {
  size_t end = 0, erroff = 0;
  bool oor = false;
  return jsoc_validate_number_checked(od->input, od->length, od->indexes[p],
                                      arena, &end, &erroff, &oor) == JSOC_OK &&
         !oor && end == (size_t)od->indexes[p] + len;
}

static inline bool od_store_scalar(const jsoc_ondemand *od, size_t p,
                            jsoc_decode_kind kind, void *out,
                            jsoc_arena *arena) {
  const char *body;
  uint32_t len = 0;
  const int k = od_value(od, p, &body, &len);

  switch (kind) {
  case JSOC_DECODE_BOOL: {
    if (k != OD_SCALAR) return false;
    bool b;
    if (len == 4 && memcmp(body, "true", 4) == 0)
      b = true;
    else if (len == 5 && memcmp(body, "false", 5) == 0)
      b = false;
    else
      return false;
    memcpy(out, &b, sizeof b);
    return true;
  }
  case JSOC_DECODE_UINT64: {
    if (k != OD_SCALAR || len == 0) return false;
    bool neg = body[0] == '-';
    size_t i = neg ? 1 : 0;
    if (i >= len) return false;
    uint64_t mag = 0;
    bool plain = !neg;
    for (; i < len; i++) {
      const unsigned d = (unsigned)(unsigned char)body[i] - (unsigned)'0';
      if (d > 9) {
        plain = false;
        break;
      }
      if (mag > (UINT64_MAX - d) / 10) {
        plain = false;
        break;
      }
      mag = mag * 10 + d;
    }
    if (!plain || (neg && mag != 0)) {
      if (!od_validate_number(od, p, len, arena)) return false;
      double d;
      if (!jsoc_lexeme_to_double(arena, body, len, &d)) return false;
      if (d < 0.0 || d != (double)(uint64_t)d) return false;
      mag = (uint64_t)d;
    }
    memcpy(out, &mag, sizeof mag);
    return true;
  }
  case JSOC_DECODE_INT64: {
    if (k != OD_SCALAR || len == 0) return false;
    bool neg = body[0] == '-';
    size_t i = neg ? 1 : 0;
    if (i >= len) return false;
    uint64_t mag = 0;
    bool plain = true;
    for (; i < len; i++) {
      const unsigned d = (unsigned)(unsigned char)body[i] - (unsigned)'0';
      if (d > 9) {
        plain = false;
        break;
      }
      if (mag > (UINT64_MAX - d) / 10) {
        plain = false;
        break;
      }
      mag = mag * 10 + d;
    }
    if (!plain) {
      if (!od_validate_number(od, p, len, arena)) return false;
      double d;
      if (!jsoc_lexeme_to_double(arena, body, len, &d)) return false;
      if (d < (double)INT64_MIN || d > (double)INT64_MAX ||
          d != (double)(int64_t)d)
        return false;
      int64_t q = (int64_t)d;
      memcpy(out, &q, sizeof q);
      return true;
    }
    if (neg) {
      if (mag > (uint64_t)INT64_MAX + 1u) return false;
      const int64_t q = (mag == (uint64_t)INT64_MAX + 1u)
                            ? INT64_MIN
                            : -(int64_t)mag;
      memcpy(out, &q, sizeof q);
    } else {
      if (mag > (uint64_t)INT64_MAX) return false;
      const int64_t q = (int64_t)mag;
      memcpy(out, &q, sizeof q);
    }
    return true;
  }
  case JSOC_DECODE_DOUBLE: {
    if (k != OD_SCALAR) return false;
    if (!od_validate_number(od, p, len, arena)) return false;
    double d;
    if (!jsoc_lexeme_to_double(arena, body, len, &d)) return false;
    memcpy(out, &d, sizeof d);
    return true;
  }
  case JSOC_DECODE_STRING_REF:
  case JSOC_DECODE_STRING_SLICE: {
    if (k != OD_STRING) return false;
    if (kind == JSOC_DECODE_STRING_SLICE) {
      jsoc_string s = {(char *)body, len};
      memcpy(out, &s, sizeof s);
      return true;
    }
    char *buf = jsoc_arena_alloc(arena, (size_t)len + 1, 1);
    if (!buf) return false;
    size_t out_len = 0;
    if (!jsoc_decode_escaped_string(body, len, buf, &out_len)) return false;
    jsoc_string s = {buf, out_len};
    memcpy(out, &s, sizeof s);
    return true;
  }
  default:
    return false;
  }
}

static size_t od_field_count(const jsoc_decode_field *fields) {
  size_t n = 0;
  if (!fields) return 0;
  while (n < JSOC_DECODE_MAX_FIELDS && fields[n].name != NULL) n++;
  return n;
}

/* Decode the object value starting at index position `p`.  On success *end
 * receives the index position just past its closing brace. */
static jsoc_decode_result od_decode_object(const jsoc_ondemand *od, size_t p,
                                           const jsoc_decode_field *fields,
                                           size_t field_count, char *base,
                                           jsoc_arena *arena, size_t *end);

static jsoc_decode_result od_decode_array(const jsoc_ondemand *od, size_t p,
                                          const jsoc_decode_field *f,
                                          char *items, size_t capacity,
                                          size_t *count_out, jsoc_arena *arena,
                                          size_t *end) {
  if (od_at(od, p) != '[')
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, f->name};

  *count_out = 0;
  size_t q = p + 1;
  if (od_at(od, q) == ']') {
    *end = q + 1;
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }

  for (size_t k = 0; k < capacity; k++) {
    jsoc_decode_result r = {JSOC_DECODE_OK, 0, NULL};
    if (f->item_kind == JSOC_DECODE_STRUCT) {
      size_t item_end = 0;
      r = od_decode_object(od, q, f->items, od_field_count(f->items),
                           items + k * f->item_size, arena, &item_end);
      if (r.code != JSOC_DECODE_OK) return r;
      q = item_end;
    } else {
      if (!od_store_scalar(od, q, f->item_kind, items + k * f->item_size, arena))
        return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, f->name};
      q = od_skip(od, q);
    }
    *count_out = k + 1;

    if (od_at(od, q) == ']') {
      *end = q + 1;
      return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
    }
    /* next element begins at q; loop continues */
  }
  return (jsoc_decode_result){JSOC_DECODE_OVERFLOW, 0, f->name};
}

static jsoc_decode_result od_decode_object(const jsoc_ondemand *od, size_t p,
                                           const jsoc_decode_field *fields,
                                           size_t field_count, char *base,
                                           jsoc_arena *arena, size_t *end) {
  if (od_at(od, p) != '{')
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};

  /* First-character buckets make field matching O(1) for typical structs
   * without a hash pass over every key byte. */
  size_t nf = 0;
  uint8_t flens[JSOC_DECODE_MAX_FIELDS];
  int8_t fbucket[256];
  int8_t fnext[JSOC_DECODE_MAX_FIELDS];
  for (size_t i = 0; i < field_count; i++) {
    if (fields[i].name == NULL) break;
    flens[nf] = (uint8_t)strlen(fields[i].name);
    fnext[nf] = 0;
    nf++;
  }
  for (int i = 0; i < 256; i++) fbucket[i] = -1;
  for (size_t i = 0; i < nf; i++) {
    const int8_t b = (int8_t)(unsigned char)fields[i].name[0];
    fnext[i] = fbucket[b];
    fbucket[b] = (int8_t)i;
  }

  bool seen[JSOC_DECODE_MAX_FIELDS] = {false};
  size_t q = p + 1;
  if (od_at(od, q) == '}') {
    *end = q + 1;
    /* required fields are checked below */
  } else {
    for (;;) {
      const uint32_t ko = od->indexes[q] + 1;
      const char *key = od->input + ko;
      const uint32_t klen = od->indexes[q + 1] - ko;
      const size_t v = q + 2; /* closing quote; separators omitted */

      const jsoc_decode_field *f = NULL;
      size_t fi = 0;
      if (klen != 0 && !memchr(key, '\\', klen)) {
        for (int8_t i = fbucket[(unsigned char)key[0]]; i >= 0; i = fnext[i]) {
          if (flens[i] == klen && memcmp(fields[i].name, key, klen) == 0) {
            f = &fields[i];
            fi = (size_t)i;
            break;
          }
        }
      }

      jsoc_decode_result r = {JSOC_DECODE_OK, 0, NULL};
      size_t next;
      if (f && f->kind == JSOC_DECODE_STRUCT) {
        next = 0;
        r = od_decode_object(od, v, f->nested, od_field_count(f->nested),
                             base + f->offset, arena, &next);
      } else if (f && f->kind == JSOC_DECODE_ARRAY) {
        if (od_at(od, v) != '[') {
          r.code = JSOC_DECODE_TYPE_MISMATCH;
          r.field_name = f->name;
          next = 0;
        } else {
          /* ARRAY writes through a caller-supplied pointer at `offset`, and
           * capacity/count live at `count_offset` (same as jsoc_decode_object). */
          char *items = *(char **)(base + f->offset);
          if (!items) {
            r.code = JSOC_DECODE_OVERFLOW;
            r.field_name = f->name;
            next = 0;
          } else {
            size_t *count_out = (size_t *)(base + f->count_offset);
            next = 0;
            r = od_decode_array(od, v, f, items, *count_out, count_out,
                                arena, &next);
          }
        }
      } else {
        if (f) {
          r.code = od_store_scalar(od, v, f->kind, base + f->offset, arena)
                       ? JSOC_DECODE_OK
                       : JSOC_DECODE_TYPE_MISMATCH;
          if (r.code != JSOC_DECODE_OK) r.field_name = f->name;
        }
        next = od_skip(od, v);
      }
      if (r.code != JSOC_DECODE_OK) return r;
      if (f && !seen[fi]) seen[fi] = true; /* first duplicate wins */

      q = next;
      if (od_at(od, q) == '}') {
        *end = q + 1;
        break;
      }
      /* next key begins at q; loop continues */
    }
  }

  for (size_t i = 0; i < nf; i++) {
    if (fields[i].required && !seen[i])
      return (jsoc_decode_result){JSOC_DECODE_MISSING_FIELD, 0, fields[i].name};
  }
  return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
}

JSOC_API jsoc_string jsoc_ondemand_string(const jsoc_ondemand *od,
                                          size_t value_pos,
                                          jsoc_arena *arena) {
  if (!od || !arena || value_pos + 1 >= od->count ||
      od_at(od, value_pos) != '"')
    return (jsoc_string){NULL, 0};
  const uint32_t off = od->indexes[value_pos] + 1;
  const uint32_t len = od->indexes[value_pos + 1] - off;
  char *buf = jsoc_arena_alloc(arena, (size_t)len + 1, 1);
  if (!buf) return (jsoc_string){NULL, 0};
  const char *raw = od->input + off;
  if (JSOC_LIKELY(memchr(raw, '\\', len) == NULL)) {
    memcpy(buf, raw, len);
    buf[len] = '\0';
    return (jsoc_string){buf, len};
  }
  size_t out_len = 0;
  if (!jsoc_decode_escaped_string(raw, len, buf, &out_len))
    return (jsoc_string){NULL, 0};
  return (jsoc_string){buf, out_len};
}

JSOC_API bool jsoc_ondemand_scan(jsoc_ondemand *od, const char *input,
                                 size_t length, jsoc_arena *arena,
                                 jsoc_error *err) {
  if (!od || !input || length == 0 || !arena) {
    if (err) *err = (jsoc_error){JSOC_ERR_SYNTAX, 0, 1, 1};
    return false;
  }
  if (length > (size_t)UINT32_MAX) {
    if (err) *err = (jsoc_error){JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1};
    return false;
  }
  uint32_t *indexes = NULL;
  size_t count = 0;
  if (jsoc_dom_stage1_compact(input, length, arena, &indexes, &count)) {
    od->input = input;
    od->length = (uint32_t)length;
    od->indexes = indexes;
    od->count = count;
    return true;
  }
  /* Replay the ordinary parser to recover the exact error semantics. */
  jsoc_document doc = jsoc_parse(input, length, arena);
  if (err) *err = doc.error;
  return false;
}

JSOC_API jsoc_decode_result
jsoc_decode_ondemand(const jsoc_decode_field *fields, size_t count,
                     const jsoc_ondemand *od, void *out, jsoc_arena *arena) {
  if (!od || !fields || !out || !arena || od->count == 0 || count > JSOC_DECODE_MAX_FIELDS)
    return (jsoc_decode_result){JSOC_DECODE_CUSTOM, 0, NULL};
  size_t end = 0;
  jsoc_decode_result r = od_decode_object(od, 0, fields, count, (char *)out,
                                          arena, &end);
  if (r.code == JSOC_DECODE_OK && end != od->count)
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
  return r;
}
