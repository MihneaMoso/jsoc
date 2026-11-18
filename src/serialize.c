#include "serialize.h"

#include "dom_internal.h"

#include <stdint.h>
#include <string.h>

/* Minified output is never longer than the input: every byte written comes
 * from a lexeme that is copied verbatim (strings are emitted between their
 * original quotes, numbers keep their spelling, literals are fixed) and the
 * only bytes added are punctuation that already existed in the input.  So
 * `doc->length + 1` is a hard bound and a single arena allocation suffices.
 *
 * The writer below is a bare cursor: no per-fragment capacity check, no
 * copying between buffers, no function call per byte.  `put_mem` still checks
 * the bound so a hand-built document cannot run off the end. */
typedef struct {
  char *p;
  char *limit;
} out_cursor;

static inline JSOC_ALWAYS_INLINE char *put_mem(out_cursor *o, const char *s,
                                               size_t n) {
  if (JSOC_UNLIKELY((size_t)(o->limit - o->p) < n))
    return NULL;
  char *p = o->p;
  memcpy(p, s, n);
  o->p = p + n;
  return p;
}

static inline JSOC_ALWAYS_INLINE char *put_ch(out_cursor *o, char c) {
  if (JSOC_UNLIKELY(o->p >= o->limit))
    return NULL;
  *o->p++ = c;
  return o->p;
}

static char *write_value(out_cursor *o, const jsoc_document *doc,
                         const jsoc_value *v);

static char *write_object(out_cursor *o, const jsoc_document *doc,
                          const jsoc_value *v) {
  if (!put_ch(o, '{'))
    return NULL;
  uint32_t m = v->first;
  for (uint32_t i = 0; i < v->count; i++) {
    const jsoc_member *member = &doc->members[m];
    if (i != 0 && !put_ch(o, ','))
      return NULL;
    if (!put_ch(o, '"'))
      return NULL;
    if (!put_mem(o, doc->input + member->key_offset, member->key_length))
      return NULL;
    if (!put_mem(o, "\":", 2))
      return NULL;
    if (!write_value(o, doc, &doc->values[member->value]))
      return NULL;
    m = member->next;
  }
  return put_ch(o, '}');
}

static JSOC_NOINLINE char *write_number_array(out_cursor *o,
                                              const jsoc_document *doc,
                                              const jsoc_value *v) {
  /* Long arrays of numbers are the commonest shape in data-heavy documents
   * (canada, numbers, mesh).  Unrolling one level keeps the sibling walk in
   * a tight loop. */
  if (!put_ch(o, '['))
    return NULL;
  uint32_t child = v->first;
  for (uint32_t i = 0; i < v->count; i++) {
    const jsoc_value *c = &doc->values[child];
    if (JSOC_LIKELY(c->type == JSOC_VALUE_NUMBER)) {
      if (i != 0 && !put_ch(o, ','))
        return NULL;
      if (!put_mem(o, doc->input + c->offset, c->length))
        return NULL;
    } else {
      if (!write_value(o, doc, c))
        return NULL;
    }
    child = c->next;
  }
  return put_ch(o, ']');
}

static char *write_array(out_cursor *o, const jsoc_document *doc,
                         const jsoc_value *v) {
  if (v->count >= 8 && doc->values[v->first].type == JSOC_VALUE_NUMBER)
    return write_number_array(o, doc, v);
  if (!put_ch(o, '['))
    return NULL;
  uint32_t child = v->first;
  for (uint32_t i = 0; i < v->count; i++) {
    if (i != 0 && !put_ch(o, ','))
      return NULL;
    if (!write_value(o, doc, &doc->values[child]))
      return NULL;
    child = doc->values[child].next;
  }
  return put_ch(o, ']');
}

static char *write_value(out_cursor *o, const jsoc_document *doc,
                         const jsoc_value *v) {
  switch (v->type) {
  case JSOC_VALUE_NULL:
    return put_mem(o, "null", 4);
  case JSOC_VALUE_FALSE:
    return put_mem(o, "false", 5);
  case JSOC_VALUE_TRUE:
    return put_mem(o, "true", 4);
  case JSOC_VALUE_NUMBER:
    return put_mem(o, doc->input + v->offset, v->length);
  case JSOC_VALUE_STRING:
    /* The raw slice is already a valid JSON string body: all escapes were
     * validated by the tokenizer, so it is written back verbatim. */
    if (!put_ch(o, '"'))
      return NULL;
    if (!put_mem(o, doc->input + v->offset, v->length))
      return NULL;
    return put_ch(o, '"');
  case JSOC_VALUE_ARRAY:
    return write_array(o, doc, v);
  case JSOC_VALUE_OBJECT:
    return write_object(o, doc, v);
  }
  return NULL;
}

JSOC_API bool jsoc_value_minify(const jsoc_document *doc, const jsoc_value *v,
                                jsoc_arena *arena, jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!doc || !arena || !out || !v || !doc->input)
    return false;

  char *data = jsoc_arena_alloc(arena, doc->length + 1, 1);
  if (!data)
    return false;

  out_cursor o = {.p = data, .limit = data + doc->length};
  if (JSOC_UNLIKELY(!write_value(&o, doc, v)))
    return false;

  const size_t n = (size_t)(o.p - data);
  *o.p = '\0';
  *out = (jsoc_string){data, n};
  return true;
}

JSOC_API bool jsoc_document_minify(const jsoc_document *doc, jsoc_arena *arena,
                                   jsoc_string *out) {
  if (!doc || !doc->root) {
    if (out)
      *out = (jsoc_string){NULL, 0};
    return false;
  }
  return jsoc_value_minify(doc, doc->root, arena, out);
}

JSOC_API bool jsoc_minify(const char *input, size_t length, jsoc_arena *arena,
                          jsoc_string *out, jsoc_error *err) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!input || !arena || !out) {
    if (err)
      *err = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return false;
  }

  jsoc_document doc = jsoc_parse(input, length, arena);
  if (err)
    *err = doc.error;
  if (doc.error.code != JSOC_OK)
    return false;

  return jsoc_document_minify(&doc, arena, out);
}
