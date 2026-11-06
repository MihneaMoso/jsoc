#include "decode.h"

#include "dom_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Number of usable entries in a NULL-terminated descriptor table. */
static size_t count_fields(const jsoc_decode_field *fields) {
  if (!fields)
    return 0;
  size_t n = 0;
  while (n < JSOC_DECODE_MAX_FIELDS + 1 && fields[n].name != NULL)
    n++;
  return n;
}

/* A missing member is an error only when the field is required. */
static jsoc_decode_result field_missing(const jsoc_decode_field *f) {
  if (!f->required) {
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, f->name};
  }
  return (jsoc_decode_result){JSOC_DECODE_MISSING_FIELD, 0, f->name};
}

jsoc_decode_result jsoc_decode_scalar(jsoc_decode_kind kind,
                                      const jsoc_document *doc,
                                      const jsoc_value *v, void *out) {
  switch (kind) {
  case JSOC_DECODE_BOOL:
    return (jsoc_decode_result){jsoc_value_get_bool(v, (bool *)out)
                                    ? JSOC_DECODE_OK
                                    : JSOC_DECODE_TYPE_MISMATCH,
                                0, NULL};
  case JSOC_DECODE_INT64:
    return (jsoc_decode_result){jsoc_value_get_int64(doc, v, (int64_t *)out)
                                    ? JSOC_DECODE_OK
                                    : JSOC_DECODE_TYPE_MISMATCH,
                                0, NULL};
  case JSOC_DECODE_UINT64:
    return (jsoc_decode_result){jsoc_value_get_uint64(doc, v, (uint64_t *)out)
                                    ? JSOC_DECODE_OK
                                    : JSOC_DECODE_TYPE_MISMATCH,
                                0, NULL};
  case JSOC_DECODE_DOUBLE:
    return (jsoc_decode_result){(v->type == JSOC_VALUE_NUMBER ||
                                 jsoc_value_get_double(doc, v, (double *)out))
                                    ? JSOC_DECODE_OK
                                    : JSOC_DECODE_TYPE_MISMATCH,
                                0, NULL};
  case JSOC_DECODE_STRING_REF:
  case JSOC_DECODE_STRING_SLICE: {
    if (v->type != JSOC_VALUE_STRING) {
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
    }
    jsoc_string s = (kind == JSOC_DECODE_STRING_REF)
                        ? jsoc_value_get_string(doc, v)
                        : jsoc_value_raw(doc, v);
    if (s.data == NULL) {
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
    }
    memcpy(out, &s, sizeof s);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  case JSOC_DECODE_STRUCT:
  case JSOC_DECODE_ARRAY:
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
  }
  return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
}

/* Store one field in `out` object space. */
static jsoc_decode_result field_store(jsoc_decode_kind kind,
                                      const jsoc_document *doc,
                                      const jsoc_value *v, void *out,
                                      const jsoc_decode_field *f, char *base) {
  jsoc_decode_result r = jsoc_decode_scalar(kind, doc, v, base + f->offset);
  if (r.code != JSOC_DECODE_OK)
    r.field_name = f->name;
  (void)out;
  return r;
}

JSOC_API jsoc_decode_result jsoc_decode_object(const jsoc_decode_field *fields,
                                               size_t field_count,
                                               const jsoc_document *doc,
                                               const jsoc_value *object,
                                               void *out) {
  if (!doc || !object || object->type != JSOC_VALUE_OBJECT || !fields || !out) {
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, 0, NULL};
  }

  /* Match in table order; every field is written exactly once. */
  if (field_count > JSOC_DECODE_MAX_FIELDS) {
    return (jsoc_decode_result){JSOC_DECODE_CUSTOM, 0, NULL};
  }
  bool seen[JSOC_DECODE_MAX_FIELDS] = {false};

  uint32_t m = object->first;
  while (m != JSOC_NO_INDEX) {
    const jsoc_member *member = &doc->members[m];
    const jsoc_value *v = &doc->values[member->value];
    const jsoc_string raw_key = {doc->input + member->key_offset,
                                 member->key_length};

    for (size_t i = 0; i < field_count; i++) {
      const jsoc_decode_field *f = &fields[i];
      if (f->name == NULL)
        break;
      /* Escapes in the key are decoded, so the comparison here is on
       * the raw slice: a key that contains an escape cannot match. */
      if (memchr(raw_key.data, '\\', raw_key.length))
        continue;
      if (strlen(f->name) != raw_key.length ||
          memcmp(f->name, raw_key.data, raw_key.length) != 0) {
        continue;
      }
      if (i < field_count && seen[i]) {
        /* Duplicate JSON members: keep the first, like the DOM. */
        m = member->next;
        goto next_member;
      }
      jsoc_decode_result r = {JSOC_DECODE_OK, 0, f->name};

      switch (f->kind) {
      case JSOC_DECODE_STRUCT: {
        r = jsoc_decode_object(f->nested, count_fields(f->nested), doc, v,
                               (char *)out + f->offset);
        if (r.code != JSOC_DECODE_OK && r.field_name == NULL) {
          r.field_name = f->name;
        }
        break;
      }
      case JSOC_DECODE_ARRAY: {
        if (v->type != JSOC_VALUE_ARRAY) {
          r.code = JSOC_DECODE_TYPE_MISMATCH;
          break;
        }
        size_t *count = (size_t *)((char *)out + f->count_offset);
        const size_t capacity = *count;
        *count = 0;
        char *base = *(char **)((char *)out + f->offset);
        if (!base) {
          r.code = JSOC_DECODE_OVERFLOW;
          break;
        }
        uint32_t child = v->first;
        for (size_t k = 0; k < v->count && k < capacity; k++) {
          if (f->item_kind == JSOC_DECODE_STRUCT) {
            r = jsoc_decode_object(f->items, count_fields(f->items), doc,
                                   &doc->values[child], base);
          } else {
            r = jsoc_decode_scalar(f->item_kind, doc, &doc->values[child],
                                   base);
          }
          if (r.code != JSOC_DECODE_OK) {
            r.field_name = f->name;
            break;
          }
          base += f->item_size;
          child = doc->values[child].next;
          *count = k + 1;
        }
        if (r.code == JSOC_DECODE_OK && v->count > capacity) {
          r.code = JSOC_DECODE_OVERFLOW;
        }
        break;
      }
      default:
        r = field_store(f->kind, doc, v, out, f, (char *)out);
        break;
      }
      if (r.code != JSOC_DECODE_OK)
        return r;
      seen[i] = true;
      break;
    }

  next_member:
    m = member->next;
  }

  for (size_t i = 0; i < field_count; i++) {
    if (fields[i].name != NULL && !seen[i]) {
      const jsoc_decode_result r = field_missing(&fields[i]);
      if (r.code != JSOC_DECODE_OK)
        return r;
    }
  }
  return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
}

JSOC_API bool jsoc_decode_value_is_array(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_ARRAY;
}

JSOC_API const jsoc_value *jsoc_decode_array_at(const jsoc_document *doc,
                                                const jsoc_value *array,
                                                size_t index) {
  if (!array || array->type != JSOC_VALUE_ARRAY || index >= array->count) {
    return NULL;
  }
  uint32_t child = array->first;
  while (index != 0) {
    child = doc->values[child].next;
    index--;
  }
  return &doc->values[child];
}
