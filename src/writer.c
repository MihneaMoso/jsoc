#include "writer.h"

#include "simd.h"
#include "text.h"

#include <string.h>

JSOC_API void jsoc_writer_init(jsoc_writer *w, jsoc_arena *arena, size_t hint) {
  memset(w, 0, sizeof *w);
  w->arena = arena;
  size_t cap = hint ? hint : 64;
  if (cap < 64)
    cap = 64;
  w->buffer = jsoc_arena_alloc(arena, cap, 1);
  w->capacity = w->buffer ? cap : 0;
}

JSOC_API void jsoc_writer_init_fixed(jsoc_writer *w, char *buffer,
                                     size_t capacity) {
  memset(w, 0, sizeof *w);
  w->buffer = buffer;
  w->capacity = capacity;
}

JSOC_API const char *jsoc_writer_data(const jsoc_writer *w) {
  return (w->buffer && !w->failed) ? w->buffer : NULL;
}

JSOC_API size_t jsoc_writer_size(const jsoc_writer *w) {
  return w->failed ? 0 : w->length;
}

static bool writer_grow(jsoc_writer *w, size_t needed) {
  if (JSOC_LIKELY(w->length + needed <= w->capacity))
    return true;
  if (!w->arena) {
    w->failed = true;
    return false;
  }
  size_t cap = w->capacity ? w->capacity : 64;
  while (cap < w->length + needed) {
    cap = cap * 2u;
    if (cap == 0) {
      w->failed = true;
      return false;
    }
  }
  char *p = jsoc_arena_alloc(w->arena, cap, 1);
  if (!p) {
    w->failed = true;
    return false;
  }
  if (w->length)
    memcpy(p, w->buffer, w->length);
  w->buffer = p;
  w->capacity = cap;
  return true;
}

JSOC_API bool jsoc_writer_raw(jsoc_writer *w, const char *data, size_t len) {
  if (!w || w->failed || (!data && len))
    return false;
  if (!writer_grow(w, len))
    return false;
  if (len)
    memcpy(w->buffer + w->length, data, len);
  w->length += len;
  return true;
}

JSOC_API bool jsoc_writer_cstr(jsoc_writer *w, const char *data) {
  if (!data)
    return false;
  return jsoc_writer_raw(w, data, strlen(data));
}

static bool writer_char(jsoc_writer *w, char c) {
  return jsoc_writer_raw(w, &c, 1);
}

/* Separator before the next element of the current container.  An object
 * member is `key : value`; the comma belongs to the key, so the value writer
 * sees `after_key` set and must not emit one. */
static bool writer_separator(jsoc_writer *w) {
  if (w->depth == 0)
    return true;
  bool *nonempty = &w->nonempty[w->depth - 1];
  bool *after_key = &w->after_key[w->depth - 1];
  if (!*nonempty || *after_key) {
    *nonempty = true;
    *after_key = false;
    return true;
  }
  return writer_char(w, ',');
}

JSOC_API bool jsoc_writer_value_raw(jsoc_writer *w, const char *data,
                                    size_t len) {
  if (!w || w->failed)
    return false;
  if (!writer_separator(w))
    return false;
  return jsoc_writer_raw(w, data, len);
}

static const char hex_digits[] = "0123456789abcdef";

JSOC_API bool jsoc_writer_string(jsoc_writer *w, const char *data, size_t len) {
  if (!w || w->failed || (!data && len))
    return false;
  if (!writer_separator(w) || !writer_char(w, '"'))
    return false;
  size_t i = 0;
  while (i < len) {
    const size_t special = jsoc_find_string_special(data, len, i);
    if (special > i && !jsoc_writer_raw(w, data + i, special - i))
      return false;
    if (special >= len)
      break;
    const unsigned char c = (unsigned char)data[special];
    char seq[6];
    size_t n;
    switch (c) {
    case '"':
      seq[0] = '\\';
      seq[1] = '"';
      n = 2;
      break;
    case '\\':
      seq[0] = '\\';
      seq[1] = '\\';
      n = 2;
      break;
    case '\b':
      seq[0] = '\\';
      seq[1] = 'b';
      n = 2;
      break;
    case '\f':
      seq[0] = '\\';
      seq[1] = 'f';
      n = 2;
      break;
    case '\n':
      seq[0] = '\\';
      seq[1] = 'n';
      n = 2;
      break;
    case '\r':
      seq[0] = '\\';
      seq[1] = 'r';
      n = 2;
      break;
    case '\t':
      seq[0] = '\\';
      seq[1] = 't';
      n = 2;
      break;
    default:
      seq[0] = '\\';
      seq[1] = 'u';
      seq[2] = '0';
      seq[3] = '0';
      seq[4] = hex_digits[c >> 4];
      seq[5] = hex_digits[c & 0xF];
      n = 6;
      break;
    }
    if (!jsoc_writer_raw(w, seq, n))
      return false;
    i = special + 1;
  }
  return writer_char(w, '"');
}

JSOC_API bool jsoc_writer_string_cstr(jsoc_writer *w, const char *data) {
  if (!data)
    return false;
  return jsoc_writer_string(w, data, strlen(data));
}

JSOC_API bool jsoc_writer_int64(jsoc_writer *w, int64_t value) {
  if (!writer_separator(w))
    return false;
  const bool negative = value < 0;
  const uint64_t magnitude =
      negative ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
  char buf[24];
  char *end = jsoc_u64_write(buf + (negative ? 1 : 0), magnitude);
  if (negative)
    buf[0] = '-';
  return jsoc_writer_raw(w, buf, (size_t)(end - buf));
}

JSOC_API bool jsoc_writer_uint64(jsoc_writer *w, uint64_t value) {
  if (!writer_separator(w))
    return false;
  char buf[24];
  char *end = jsoc_u64_write(buf, value);
  return jsoc_writer_raw(w, buf, (size_t)(end - buf));
}

JSOC_API bool jsoc_writer_double(jsoc_writer *w, double value) {
  if (!writer_separator(w))
    return false;
  /* Non-finite values have no JSON spelling; `null` keeps the text valid.
   * `jsoc_double_text` writes the shortest spelling that reads back
   * bit-identically, and "null" for non-finite input. */
  char buf[32];
  const size_t n = jsoc_double_text(value, buf);
  if (!n)
    return false;
  return jsoc_writer_raw(w, buf, n);
}

JSOC_API bool jsoc_writer_bool(jsoc_writer *w, bool value) {
  if (!writer_separator(w))
    return false;
  return jsoc_writer_raw(w, value ? "true" : "false", value ? 4 : 5);
}

JSOC_API bool jsoc_writer_null(jsoc_writer *w) {
  if (!writer_separator(w))
    return false;
  return jsoc_writer_raw(w, "null", 4);
}

JSOC_API bool jsoc_writer_array_begin(jsoc_writer *w) {
  if (!w || w->failed)
    return false;
  if (!writer_separator(w) || !writer_char(w, '['))
    return false;
  if (w->depth >= JSOC_WRITER_MAX_DEPTH) {
    w->failed = true;
    return false;
  }
  w->in_object[w->depth] = false;
  w->nonempty[w->depth] = false;
  w->after_key[w->depth] = false;
  w->depth++;
  return true;
}

JSOC_API bool jsoc_writer_array_end(jsoc_writer *w) {
  if (!w || w->failed || w->depth == 0 || w->in_object[w->depth - 1]) {
    if (w)
      w->failed = true;
    return false;
  }
  w->depth--;
  return writer_char(w, ']');
}

JSOC_API bool jsoc_writer_object_begin(jsoc_writer *w) {
  if (!w || w->failed)
    return false;
  if (!writer_separator(w) || !writer_char(w, '{'))
    return false;
  if (w->depth >= JSOC_WRITER_MAX_DEPTH) {
    w->failed = true;
    return false;
  }
  w->in_object[w->depth] = true;
  w->nonempty[w->depth] = false;
  w->after_key[w->depth] = false;
  w->depth++;
  return true;
}

JSOC_API bool jsoc_writer_object_end(jsoc_writer *w) {
  if (!w || w->failed || w->depth == 0 || !w->in_object[w->depth - 1]) {
    if (w)
      w->failed = true;
    return false;
  }
  w->depth--;
  return writer_char(w, '}');
}

JSOC_API bool jsoc_writer_object_key(jsoc_writer *w, const char *data,
                                     size_t len) {
  if (!w || w->failed || w->depth == 0 || !w->in_object[w->depth - 1]) {
    if (w)
      w->failed = true;
    return false;
  }
  if (!jsoc_writer_string(w, data, len))
    return false;
  /* The comma for this element, when one was needed, is already out. */
  w->after_key[w->depth - 1] = true;
  return writer_char(w, ':');
}

JSOC_API bool jsoc_writer_object_key_cstr(jsoc_writer *w, const char *data) {
  if (!data)
    return false;
  return jsoc_writer_object_key(w, data, strlen(data));
}

JSOC_API bool jsoc_writer_object_raw_key(jsoc_writer *w, const char *data,
                                         size_t len) {
  if (!w || w->failed || !w->in_object[w->depth - 1]) {
    if (w)
      w->failed = true;
    return false;
  }
  if (!jsoc_writer_value_raw(w, "\"", 1))
    return false;
  if (!jsoc_writer_raw(w, data, len))
    return false;
  if (!jsoc_writer_raw(w, "\"", 1))
    return false;
  w->after_key[w->depth - 1] = true;
  return writer_char(w, ':');
}

JSOC_API bool jsoc_writer_object_raw_key_cstr(jsoc_writer *w,
                                              const char *data) {
  if (!data)
    return false;
  return jsoc_writer_object_raw_key(w, data, strlen(data));
}
