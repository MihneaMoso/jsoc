#include "writer.h"

#include <stdio.h>
#include <stdlib.h>
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
  size_t run_start = 0;
  for (size_t i = 0; i < len; i++) {
    const unsigned char c = (unsigned char)data[i];
    char esc = 0;
    if (c == '"')
      esc = '"';
    else if (c == '\\')
      esc = '\\';
    else if (c == '\b')
      esc = 'b';
    else if (c == '\f')
      esc = 'f';
    else if (c == '\n')
      esc = 'n';
    else if (c == '\r')
      esc = 'r';
    else if (c == '\t')
      esc = 't';
    else if (c < 0x20)
      esc = 'u';
    if (esc == 0)
      continue;
    if (i != run_start && !jsoc_writer_raw(w, data + run_start, i - run_start))
      return false;
    if (esc == 'u') {
      char seq[6] = {
          '\\', 'u', '0', '0', hex_digits[c >> 4], hex_digits[c & 0xF]};
      if (!jsoc_writer_raw(w, seq, sizeof seq))
        return false;
    } else {
      char seq[2] = {'\\', esc};
      if (!jsoc_writer_raw(w, seq, sizeof seq))
        return false;
    }
    run_start = i + 1;
  }
  if (len != run_start &&
      !jsoc_writer_raw(w, data + run_start, len - run_start)) {
    return false;
  }
  return writer_char(w, '"');
}

JSOC_API bool jsoc_writer_string_cstr(jsoc_writer *w, const char *data) {
  if (!data)
    return false;
  return jsoc_writer_string(w, data, strlen(data));
}

JSOC_API bool jsoc_writer_int64(jsoc_writer *w, int64_t value) {
  char buf[24];
  int n = snprintf(buf, sizeof buf, "%lld", (long long)value);
  if (n < 0 || (size_t)n >= sizeof buf)
    return false;
  if (!writer_separator(w))
    return false;
  return jsoc_writer_raw(w, buf, (size_t)n);
}

JSOC_API bool jsoc_writer_uint64(jsoc_writer *w, uint64_t value) {
  char buf[24];
  int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)value);
  if (n < 0 || (size_t)n >= sizeof buf)
    return false;
  if (!writer_separator(w))
    return false;
  return jsoc_writer_raw(w, buf, (size_t)n);
}

JSOC_API bool jsoc_writer_double(jsoc_writer *w, double value) {
  if (!writer_separator(w))
    return false;
  /* Non-finite values have no JSON spelling; `null` keeps the text valid. */
  if (value != value || value > 1.7976931348623157e308 ||
      value < -1.7976931348623157e308) {
    return jsoc_writer_raw(w, "null", 4);
  }

  /* Shortest representation that reads back bit-identically. */
  char buf[32];
  for (int precision = 1; precision <= 17; precision++) {
    int n = snprintf(buf, sizeof buf, "%.*g", precision, value);
    if (n < 0 || (size_t)n >= sizeof buf)
      return false;
    if (strtod(buf, NULL) == value)
      break;
    if (precision == 17)
      return false;
  }
  /* `%g` may drop the fraction ("1" for 1.0).  JSON does not distinguish
   * integer and floating-point values, so that spelling is fine. */

  /* Locale-independent text: never accept a decimal comma. */
  for (char *p = buf; *p; p++) {
    if (*p == ',')
      *p = '.';
  }
  return jsoc_writer_raw(w, buf, strlen(buf));
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
