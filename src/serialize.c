#include "serialize.h"

#include "dom_internal.h"
#include "writer.h"

#include <stdint.h>
#include <string.h>

/* Re-emit a value's raw lexeme, which values decode back to the exact bytes
 * that were validated (or, for strings, the bytes between the quotes, again
 * verbatim).  `jsoc_writer_value_raw` is used for all scalars so the
 * tokenizer's validated spelling is never re-escaped. */
static bool write_value_text(jsoc_writer *w, const jsoc_document *doc,
                             const jsoc_value *v);

static bool write_object_text(jsoc_writer *w, const jsoc_document *doc,
                              const jsoc_value *v) {
  if (!jsoc_writer_object_begin(w))
    return false;
  uint32_t m = v->first;
  for (uint32_t i = 0; i < v->count; i++) {
    const jsoc_member *member = &doc->members[m];
    const char *key = doc->input + member->key_offset;
    if (!jsoc_writer_object_raw_key(w, key, member->key_length))
      return false;
    if (!write_value_text(w, doc, &doc->values[member->value]))
      return false;
    m = member->next;
  }
  return jsoc_writer_object_end(w);
}

static bool write_value_text(jsoc_writer *w, const jsoc_document *doc,
                             const jsoc_value *v) {
  switch (v->type) {
  case JSOC_VALUE_NULL:
    return jsoc_writer_null(w);
  case JSOC_VALUE_FALSE:
    return jsoc_writer_bool(w, false);
  case JSOC_VALUE_TRUE:
    return jsoc_writer_bool(w, true);
  case JSOC_VALUE_NUMBER:
    return jsoc_writer_value_raw(w, doc->input + v->offset, v->length);
  case JSOC_VALUE_STRING:
    /* The raw slice is already a valid JSON string body: all escapes were
     * validated by the tokenizer, so it is written back verbatim. */
    if (!jsoc_writer_value_raw(w, "\"", 1))
      return false;
    if (!jsoc_writer_raw(w, doc->input + v->offset, v->length))
      return false;
    return jsoc_writer_raw(w, "\"", 1);
  case JSOC_VALUE_ARRAY: {
    if (!jsoc_writer_array_begin(w))
      return false;
    uint32_t child = v->first;
    for (uint32_t i = 0; i < v->count; i++) {
      if (!write_value_text(w, doc, &doc->values[child]))
        return false;
      child = doc->values[child].next;
    }
    return jsoc_writer_array_end(w);
  }
  case JSOC_VALUE_OBJECT:
    return write_object_text(w, doc, v);
  }
  return false;
}

JSOC_API bool jsoc_document_minify(const jsoc_document *doc, jsoc_arena *arena,
                                   jsoc_string *out) {
  if (!doc || !doc->root) {
    return false;
  }
  return jsoc_value_minify(doc, doc->root, arena, out);
}

JSOC_API bool jsoc_value_minify(const jsoc_document *doc, const jsoc_value *v,
                                jsoc_arena *arena, jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!doc || !arena || !out || !v) {
    return false;
  }

  /* A minified document is at most two bytes per input byte; preallocate the
   * input length so growth is the exception, not the rule. */
  jsoc_writer w;
  jsoc_writer_init(&w, arena, doc->length + 16);
  if (!w.buffer)
    return false;

  if (!write_value_text(&w, doc, v) || w.failed)
    return false;

  const size_t n = jsoc_writer_size(&w);
  char *data = jsoc_arena_alloc(arena, n + 1, 1);
  if (!data)
    return false;
  memcpy(data, jsoc_writer_data(&w), n);
  data[n] = '\0';
  *out = (jsoc_string){data, n};
  return true;
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
