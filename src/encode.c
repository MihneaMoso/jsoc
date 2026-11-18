#include "encode.h"

#include "arena_internal.h"
#include "simd.h"
#include "text.h"

#include <stdint.h>
#include <string.h>

/*
 * Two-pass descriptor encoder.
 *
 * Pass 1 walks the descriptor and the object to prove a capacity.  Pass 2
 * writes into that capacity with a bare pointer.  Keeping the two passes
 * separate is what makes the write loop free of growth checks.
 *
 * `jsoc_encode_object` usually skips pass 1 entirely: it writes into the
 * unallocated tail of the arena's current chunk and commits only the bytes
 * used, falling back to the exact bound when the document does not fit.
 *
 * The write loop inlines the scalar kinds.  A field is then one store for the
 * pre-quoted key ("name": with the leading comma folded in) plus one store
 * for the value, instead of a call and three appends per member.  The cursor
 * is threaded through the recursive helpers as a value so it never round
 * trips through memory.
 */

/* ------------------------------------------------------------------------- */
/* Pass 1: capacity                                                          */
/* ------------------------------------------------------------------------- */

static size_t field_count(const jsoc_encode_field *fields, size_t n) {
  if (!fields)
    return 0;
  if (n > JSOC_ENCODE_MAX_FIELDS)
    n = JSOC_ENCODE_MAX_FIELDS;
  size_t i = 0;
  while (i < n && fields[i].name)
    i++;
  return i;
}

static inline size_t add_bound(size_t a, size_t b) {
  return (a > SIZE_MAX - b) ? SIZE_MAX : a + b;
}

static size_t bound_object(const jsoc_encode_field *fields, size_t n,
                           const void *object);
static size_t bound_value(const jsoc_encode_field *f, const void *container);
static size_t array_bound(const jsoc_encode_field *f, const void *container);

/* Quoted key plus colon, with the member separator.  A hand-written
 * descriptor may leave `key` empty; the writer then builds it from `name`. */
static inline size_t key_bound(const jsoc_encode_field *f) {
  return f->key_length ? f->key_length : f->name_length + 4;
}

static size_t array_bound(const jsoc_encode_field *f, const void *container) {
  const char *base =
      *(const char *const *)((const char *)container + f->offset);
  size_t count = *(const size_t *)((const char *)container + f->count_offset);
  if (f->max_items && count > f->max_items)
    return SIZE_MAX; /* corrupt/hostile count: fail, never over-read */
  if (!base)
    count = 0;

  size_t total = 2; /* brackets */
  if (count)
    total = add_bound(total, count - 1);
  if (total == SIZE_MAX)
    return SIZE_MAX;

  if (f->item_kind == JSOC_ENCODE_STRUCT) {
    const size_t n = field_count(f->items, JSOC_ENCODE_MAX_FIELDS);
    for (size_t i = 0; i < count; i++) {
      total =
          add_bound(total, bound_object(f->items, n, base + i * f->item_size));
      if (JSOC_UNLIKELY(total == SIZE_MAX))
        return SIZE_MAX;
    }
  } else {
    jsoc_encode_field elem = *f;
    elem.kind = f->item_kind;
    elem.offset = 0;
    for (size_t i = 0; i < count; i++) {
      total = add_bound(total, bound_value(&elem, base + i * f->item_size));
      if (JSOC_UNLIKELY(total == SIZE_MAX))
        return SIZE_MAX;
    }
  }
  return total;
}

static size_t bound_value(const jsoc_encode_field *f, const void *container) {
  const char *base = (const char *)container + f->offset;
  switch (f->kind) {
  case JSOC_ENCODE_NULL:
    return 4;
  case JSOC_ENCODE_BOOL:
    return 5;
  case JSOC_ENCODE_INT64:
    return 21;
  case JSOC_ENCODE_UINT64:
    return 20;
  case JSOC_ENCODE_DOUBLE:
    return 32;
  case JSOC_ENCODE_STRING:
  case JSOC_ENCODE_STRING_RAW: {
    const jsoc_string *s = (const jsoc_string *)base;
    const size_t n = (s && s->data) ? s->length : 0;
    if (JSOC_UNLIKELY(n > (SIZE_MAX - 2) / 6))
      return SIZE_MAX;
    return n * 6 + 2; /* worst case: every byte becomes \u00XX */
  }
  case JSOC_ENCODE_CSTR: {
    const char *s = *(const char *const *)base;
    const size_t n = s ? strlen(s) : 0;
    if (JSOC_UNLIKELY(n > (SIZE_MAX - 2) / 6))
      return SIZE_MAX;
    return n * 6 + 2;
  }
  case JSOC_ENCODE_RAW: {
    const jsoc_string *s = (const jsoc_string *)base;
    return (s && s->data) ? s->length : 0;
  }
  case JSOC_ENCODE_STRUCT:
    return bound_object(f->nested,
                        field_count(f->nested, JSOC_ENCODE_MAX_FIELDS), base);
  case JSOC_ENCODE_ARRAY:
    return array_bound(f, container);
  }
  return SIZE_MAX;
}

/* The hot loop of the size pass.  Scalar bounds are inlined so a field costs
 * one indexed load and one addition; only nested containers are calls. */
static size_t bound_object(const jsoc_encode_field *fields, size_t n,
                           const void *object) {
  size_t total = 1; /* '{' */
  for (size_t i = 0; i < n; i++) {
    const jsoc_encode_field *f = &fields[i];
    const char *base = (const char *)object + f->offset;
    size_t v;
    switch (f->kind) {
    case JSOC_ENCODE_NULL:
      v = 4;
      break;
    case JSOC_ENCODE_BOOL:
      v = 5;
      break;
    case JSOC_ENCODE_INT64:
      v = 21;
      break;
    case JSOC_ENCODE_UINT64:
      v = 20;
      break;
    case JSOC_ENCODE_DOUBLE:
      v = 32;
      break;
    case JSOC_ENCODE_STRING:
    case JSOC_ENCODE_STRING_RAW: {
      const jsoc_string *str = (const jsoc_string *)base;
      const size_t len = (str && str->data) ? str->length : 0;
      if (JSOC_UNLIKELY(len > (SIZE_MAX - 2) / 6))
        return SIZE_MAX;
      v = len * 6 + 2;
      break;
    }
    case JSOC_ENCODE_CSTR: {
      const char *str = *(const char *const *)base;
      const size_t len = str ? strlen(str) : 0;
      if (JSOC_UNLIKELY(len > (SIZE_MAX - 2) / 6))
        return SIZE_MAX;
      v = len * 6 + 2;
      break;
    }
    case JSOC_ENCODE_RAW: {
      const jsoc_string *str = (const jsoc_string *)base;
      v = (str && str->data) ? str->length : 0;
      break;
    }
    default: /* STRUCT, ARRAY */
      v = (f->kind == JSOC_ENCODE_STRUCT)
              ? bound_object(f->nested,
                             field_count(f->nested, JSOC_ENCODE_MAX_FIELDS),
                             base)
              : array_bound(f, object);
      if (JSOC_UNLIKELY(v == SIZE_MAX))
        return SIZE_MAX;
      break;
    }
    total = add_bound(total, key_bound(f));
    total = add_bound(total, v);
    if (JSOC_UNLIKELY(total == SIZE_MAX))
      return SIZE_MAX;
  }
  return add_bound(total, 1); /* '}' */
}

JSOC_API size_t jsoc_encode_size(const jsoc_encode_field *fields,
                                 size_t field_count_hint, const void *object) {
  const size_t n = field_count(fields, field_count_hint);
  if (!n || !object)
    return 0;
  const size_t bound = bound_object(fields, n, object);
  return (bound == SIZE_MAX) ? 0 : bound;
}

/* ------------------------------------------------------------------------- */
/* Pass 2: writing                                                           */
/* ------------------------------------------------------------------------- */

/* Every helper takes the cursor by value and returns the advanced pointer,
 * or NULL when the capacity is exhausted. */

static char *put_object(char *p, char *end,
                        const jsoc_encode_field *fields, size_t n,
                        const void *object);
static char *put_value(char *p, char *end, const jsoc_encode_field *f,
                       const void *container);

static JSOC_NOINLINE char *put_double(char *p, char *end, double value) {
  char tmp[32];
  const size_t n = jsoc_double_text(value, tmp);
  if (JSOC_UNLIKELY(n == 0 || (size_t)(end - p) < n))
    return NULL;
  memcpy(p, tmp, n);
  return p + n;
}

static JSOC_NOINLINE char *put_escaped(char *p, char *end, const char *data,
                                       size_t n) {
  static const char hex[] = "0123456789abcdef";
  if (JSOC_UNLIKELY(end - p < 1))
    return NULL;
  *p++ = '"';
  size_t i = 0;
  while (i < n) {
    const size_t special = jsoc_find_string_special(data, n, i);
    const size_t run = special - i;
    if (JSOC_UNLIKELY((size_t)(end - p) < run + 6))
      return NULL;
    p = jsoc_copy_out(p, data + i, run);
    if (special >= n)
      break;
    const unsigned char c = (unsigned char)data[special];
    char seq[6];
    size_t len;
    switch (c) {
    case '"':
      seq[0] = '\\';
      seq[1] = '"';
      len = 2;
      break;
    case '\\':
      seq[0] = '\\';
      seq[1] = '\\';
      len = 2;
      break;
    case '\b':
      seq[0] = '\\';
      seq[1] = 'b';
      len = 2;
      break;
    case '\f':
      seq[0] = '\\';
      seq[1] = 'f';
      len = 2;
      break;
    case '\n':
      seq[0] = '\\';
      seq[1] = 'n';
      len = 2;
      break;
    case '\r':
      seq[0] = '\\';
      seq[1] = 'r';
      len = 2;
      break;
    case '\t':
      seq[0] = '\\';
      seq[1] = 't';
      len = 2;
      break;
    default:
      seq[0] = '\\';
      seq[1] = 'u';
      seq[2] = '0';
      seq[3] = '0';
      seq[4] = hex[c >> 4];
      seq[5] = hex[c & 0xF];
      len = 6;
      break;
    }
    memcpy(p, seq, len);
    p += len;
    i = special + 1;
  }
  if (JSOC_UNLIKELY(end - p < 1))
    return NULL;
  *p++ = '"';
  return p;
}

static char *put_raw_string(char *p, char *end, const jsoc_string *s) {
  const char *data = (s && s->data) ? s->data : "";
  const size_t n = (s && s->data) ? s->length : 0;
  if (JSOC_UNLIKELY((size_t)(end - p) < n + 2))
    return NULL;
  *p++ = '"';
  p = jsoc_copy_out(p, data, n);
  *p++ = '"';
  return p;
}

/* Emit exactly the bytes of the key, `,"name":` minus the leading comma for
 * the first member.  The descriptor's key is a zero-padded 32-byte array, so
 * the common case is one 32-byte vector store rather than a libc memcpy call
 * with a runtime length. */
static inline char *put_key(char *p, char *end, const jsoc_encode_field *f,
                            bool first) {
  const size_t key_length = f->key_length;
  if (JSOC_UNLIKELY(key_length == 0)) {
    /* Hand-written descriptor: build the key from the member name. */
    const size_t need = f->name_length + (first ? 3 : 4);
    if (JSOC_UNLIKELY((size_t)(end - p) < need))
      return NULL;
    if (!first)
      *p++ = ',';
    *p++ = '"';
    memcpy(p, f->name, f->name_length);
    p += f->name_length;
    *p++ = '"';
    *p++ = ':';
    return p;
  }
  if (JSOC_UNLIKELY(first)) {
    const size_t n = key_length - 1;
    if (JSOC_UNLIKELY((size_t)(end - p) < n))
      return NULL;
    memcpy(p, f->key + 1, n);
    return p + n;
  }
  if (JSOC_LIKELY(key_length <= 16)) {
    if (JSOC_UNLIKELY((size_t)(end - p) < 16))
      return NULL;
    memcpy(p, f->key, 16);
    return p + key_length;
  }
  if (JSOC_UNLIKELY((size_t)(end - p) < sizeof f->key)) {
    if (JSOC_UNLIKELY((size_t)(end - p) < key_length))
      return NULL;
    memcpy(p, f->key, key_length);
    return p + key_length;
  }
  memcpy(p, f->key, sizeof f->key);
  return p + key_length;
}

static char *put_object(char *p, char *end,
                        const jsoc_encode_field *fields, size_t n,
                        const void *object) {
  if (JSOC_UNLIKELY(p >= end))
    return NULL;
  *p++ = '{';

  for (size_t i = 0; i < n; i++) {
    const jsoc_encode_field *f = &fields[i];
    p = put_key(p, end, f, i == 0);
    if (JSOC_UNLIKELY(!p))
      return NULL;

    const char *src = (const char *)object + f->offset;
    switch (f->kind) {
    case JSOC_ENCODE_NULL:
      if (JSOC_UNLIKELY((size_t)(end - p) < 4))
        return NULL;
      memcpy(p, "null", 4);
      p += 4;
      break;
    case JSOC_ENCODE_BOOL:
      if (JSOC_UNLIKELY((size_t)(end - p) < 5))
        return NULL;
      if (*(const bool *)src) {
        memcpy(p, "true", 4);
        p += 4;
      } else {
        memcpy(p, "false", 5);
        p += 5;
      }
      break;
    case JSOC_ENCODE_UINT64:
      if (JSOC_UNLIKELY((size_t)(end - p) < 21))
        return NULL;
      p = jsoc_u64_write(p, *(const uint64_t *)src);
      break;
    case JSOC_ENCODE_INT64: {
      const int64_t v = *(const int64_t *)src;
      const bool negative = v < 0;
      if (JSOC_UNLIKELY((size_t)(end - p) < 22))
        return NULL;
      if (negative)
        *p++ = '-';
      p = jsoc_u64_write(p, negative ? (uint64_t)0 - (uint64_t)v : (uint64_t)v);
      break;
    }
    case JSOC_ENCODE_DOUBLE:
      p = put_double(p, end, *(const double *)src);
      if (JSOC_UNLIKELY(!p))
        return NULL;
      break;
    case JSOC_ENCODE_STRING_RAW: {
      const jsoc_string *s = (const jsoc_string *)src;
      const size_t len = (s && s->data) ? s->length : 0;
      if (JSOC_UNLIKELY((size_t)(end - p) < len + 2))
        return NULL;
      *p++ = '"';
      p = jsoc_copy_out(p, s->data, len);
      *p++ = '"';
      break;
    }
    case JSOC_ENCODE_RAW: {
      const jsoc_string *s = (const jsoc_string *)src;
      if (s && s->data && s->length) {
        if (JSOC_UNLIKELY((size_t)(end - p) < s->length))
          return NULL;
        p = jsoc_copy_out(p, s->data, s->length);
      }
      break;
    }
    case JSOC_ENCODE_STRING:
    case JSOC_ENCODE_CSTR: {
      const char *data;
      size_t len;
      if (f->kind == JSOC_ENCODE_STRING) {
        const jsoc_string *s = (const jsoc_string *)src;
        data = (s && s->data) ? s->data : "";
        len = (s && s->data) ? s->length : 0;
      } else {
        data = *(const char *const *)src;
        if (!data)
          data = "";
        len = strlen(data);
      }
      p = put_escaped(p, end, data, len);
      if (JSOC_UNLIKELY(!p))
        return NULL;
      break;
    }
    case JSOC_ENCODE_STRUCT:
    case JSOC_ENCODE_ARRAY:
      p = put_value(p, end, f, object);
      if (JSOC_UNLIKELY(!p))
        return NULL;
      break;
    }
  }

  if (JSOC_UNLIKELY(p >= end))
    return NULL;
  *p++ = '}';
  return p;
}

static char *put_array(char *p, char *end, const jsoc_encode_field *f,
                       const void *container) {
  const char *items_base =
      *(const char *const *)((const char *)container + f->offset);
  size_t count = *(const size_t *)((const char *)container + f->count_offset);
  if (f->max_items && count > f->max_items)
    return NULL; /* corrupt/hostile count: fail, never over-read */
  if (!items_base)
    count = 0;

  if (JSOC_UNLIKELY(p >= end))
    return NULL;
  *p++ = '[';

  if (f->item_kind == JSOC_ENCODE_STRUCT) {
    const size_t n = field_count(f->items, JSOC_ENCODE_MAX_FIELDS);
    for (size_t i = 0; i < count; i++) {
      if (i != 0) {
        if (JSOC_UNLIKELY(p >= end))
          return NULL;
        *p++ = ',';
      }
      p = put_object(p, end, f->items, n, items_base + i * f->item_size);
      if (JSOC_UNLIKELY(!p))
        return NULL;
    }
  } else {
    jsoc_encode_field elem = *f;
    elem.kind = f->item_kind;
    elem.offset = 0;
    for (size_t i = 0; i < count; i++) {
      if (i != 0) {
        if (JSOC_UNLIKELY(p >= end))
          return NULL;
        *p++ = ',';
      }
      p = put_value(p, end, &elem, items_base + i * f->item_size);
      if (JSOC_UNLIKELY(!p))
        return NULL;
    }
  }

  if (JSOC_UNLIKELY(p >= end))
    return NULL;
  *p++ = ']';
  return p;
}

static char *put_value(char *p, char *end, const jsoc_encode_field *f,
                       const void *container) {
  const char *base = (const char *)container + f->offset;
  switch (f->kind) {
  case JSOC_ENCODE_NULL:
    if (JSOC_UNLIKELY((size_t)(end - p) < 4))
      return NULL;
    memcpy(p, "null", 4);
    return p + 4;
  case JSOC_ENCODE_BOOL:
    if (JSOC_UNLIKELY((size_t)(end - p) < 5))
      return NULL;
    if (*(const bool *)base) {
      memcpy(p, "true", 4);
      return p + 4;
    }
    memcpy(p, "false", 5);
    return p + 5;
  case JSOC_ENCODE_INT64: {
    const int64_t v = *(const int64_t *)base;
    const bool negative = v < 0;
    if (JSOC_UNLIKELY((size_t)(end - p) < 22))
      return NULL;
    if (negative)
      *p++ = '-';
    return jsoc_u64_write(p, negative ? (uint64_t)0 - (uint64_t)v
                                      : (uint64_t)v);
  }
  case JSOC_ENCODE_UINT64:
    if (JSOC_UNLIKELY((size_t)(end - p) < 21))
      return NULL;
    return jsoc_u64_write(p, *(const uint64_t *)base);
  case JSOC_ENCODE_DOUBLE:
    return put_double(p, end, *(const double *)base);
  case JSOC_ENCODE_STRING: {
    const jsoc_string *s = (const jsoc_string *)base;
    return put_escaped(p, end, (s && s->data) ? s->data : "",
                       (s && s->data) ? s->length : 0);
  }
  case JSOC_ENCODE_STRING_RAW:
    return put_raw_string(p, end, (const jsoc_string *)base);
  case JSOC_ENCODE_CSTR: {
    const char *s = *(const char *const *)base;
    return put_escaped(p, end, s ? s : "", s ? strlen(s) : 0);
  }
  case JSOC_ENCODE_RAW: {
    const jsoc_string *s = (const jsoc_string *)base;
    if (!s || !s->data || !s->length)
      return p;
    if (JSOC_UNLIKELY((size_t)(end - p) < s->length))
      return NULL;
    return jsoc_copy_out(p, s->data, s->length);
  }
  case JSOC_ENCODE_STRUCT:
    return put_object(p, end, f->nested,
                      field_count(f->nested, JSOC_ENCODE_MAX_FIELDS), base);
  case JSOC_ENCODE_ARRAY:
    return put_array(p, end, f, container);
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* Entry points                                                              */
/* ------------------------------------------------------------------------- */

JSOC_API bool jsoc_encode_object_hinted(const jsoc_encode_field *fields,
                                        size_t field_count_hint,
                                        const void *object, size_t hint,
                                        jsoc_arena *arena, jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!fields || !arena || !out || !object)
    return false;
  const size_t n = field_count(fields, field_count_hint);
  if (!n)
    return false;
  if (hint == 0)
    hint = 1;

  char *data = jsoc_arena_alloc(arena, hint + 1, 1);
  if (!data)
    return false;
  char *const p = put_object(data, data + hint, fields, n, object);
  if (JSOC_UNLIKELY(!p))
    return false;
  const size_t len = (size_t)(p - data);
  *p = '\0';
  *out = (jsoc_string){data, len};
  return true;
}

JSOC_API bool jsoc_encode_object(const jsoc_encode_field *fields,
                                 size_t field_count_hint, const void *object,
                                 jsoc_arena *arena, jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!fields || !arena || !out || !object)
    return false;
  const size_t n = field_count(fields, field_count_hint);
  if (!n)
    return false;

  /* Fast path: the document usually fits in the unallocated tail of the
   * arena's current chunk, so write it there and commit just the used bytes.
   * That skips the whole size pass, which costs as much as the write itself.
   * If it does not fit, nothing was committed and the exact bound is used. */
  size_t avail = 0;
  char *const tail = jsoc_arena_tail(arena, &avail);
  if (tail && avail > 1) {
    char *const p = put_object(tail, tail + (avail - 1), fields, n, object);
    if (p) {
      const size_t len = (size_t)(p - tail);
      char *const data = jsoc_arena_commit(arena, len);
      if (JSOC_UNLIKELY(!data))
        return false;
      *p = '\0';
      *out = (jsoc_string){data, len};
      return true;
    }
  }

  const size_t bound = bound_object(fields, n, object);
  if (!bound || bound == SIZE_MAX)
    return false;
  return jsoc_encode_object_hinted(fields, field_count_hint, object, bound,
                                   arena, out);
}

JSOC_API bool jsoc_encode_value(const jsoc_encode_field *field,
                                const void *container, jsoc_arena *arena,
                                jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!field || !container || !arena || !out)
    return false;
  const size_t bound = bound_value(field, container);
  if (!bound || bound == SIZE_MAX)
    return false;
  char *data = jsoc_arena_alloc(arena, bound + 1, 1);
  if (!data)
    return false;
  char *const p = put_value(data, data + bound, field, container);
  if (JSOC_UNLIKELY(!p))
    return false;
  const size_t len = (size_t)(p - data);
  *p = '\0';
  *out = (jsoc_string){data, len};
  return true;
}
