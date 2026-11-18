#include "serialize.h"

#include "dom_internal.h"
#include "simd.h"

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

/* ------------------------------------------------------------------------- */
/* Raw minify: SIMD whitespace strip using stage1 scanner + compress          */
/* ------------------------------------------------------------------------- */

static inline JSOC_ALWAYS_INLINE size_t jsoc_skip_ws_simd(const char *p,
                                                          size_t len,
                                                          size_t i) {
#if JSOC_SIMD_AVX2
  while (i + 32 <= len) {
    const __m256i v = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
    const __m256i ws = _mm256_set1_epi8(0x20);
    const __m256i ctl = _mm256_set1_epi8(0x1F);
    const __m256i m1 = _mm256_cmpeq_epi8(v, ws);
    const __m256i m2 = _mm256_cmpeq_epi8(_mm256_max_epu8(v, ctl), ctl);
    const uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(m1, m2));
    if (mask != UINT32_MAX)
      return i + (size_t)__builtin_ctz(~mask);
    i += 32;
  }
#elif JSOC_SIMD_SSE2
  while (i + 16 <= len) {
    const __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(p + i));
    const __m128i ws = _mm_set1_epi8(0x20);
    const __m128i ctl = _mm_set1_epi8(0x1F);
    const __m128i m1 = _mm_cmpeq_epi8(v, ws);
    const __m128i m2 = _mm_cmpeq_epi8(_mm_max_epu8(v, ctl), ctl);
    const uint32_t mask = (uint16_t)_mm_movemask_epi8(_mm_or_si128(m1, m2));
    if (mask != 0xFFFFu)
      return i + (size_t)__builtin_ctz(~mask & 0xFFFFu);
    i += 16;
  }
#endif
  while (i < len && jsoc_is_ws((unsigned char)p[i]))
    i++;
  return i;
}

JSOC_API bool jsoc_minify_raw(const char *input, size_t length,
                              jsoc_arena *arena, jsoc_string *out) {
  if (out)
    *out = (jsoc_string){NULL, 0};
  if (!input || !arena || !out)
    return false;

  char *data = jsoc_arena_alloc(arena, length + 1, 1);
  if (!data)
    return false;

#if JSOC_SIMD_AVX2
  /* Fast path: use stage1-style scanning to identify whitespace outside
   * strings, then compress it out with AVX2.  This mirrors simdjson's
   * json_minifier which runs the stage1 scanner and uses vcompress. */
  const char *p = input;
  size_t i = 0;
  size_t out_pos = 0;
  uint64_t prev_in_string = 0;
  uint64_t next_is_escaped = 0;

  while (i + 64 <= length) {
    const __m256i lo = _mm256_loadu_si256((const __m256i *)(const void *)(p + i));
    const __m256i hi = _mm256_loadu_si256((const __m256i *)(const void *)(p + i + 32));
    const jsoc_block_masks m = jsoc_classify64_vec(lo, hi);

    /* Update escape state (same as stage1). */
    uint64_t escaped = jsoc_escape_next(m.backslash, &next_is_escaped);

    /* Quote mask excluding escaped quotes. */
    uint64_t quote = m.quote & ~escaped;

    /* in_string: 1 for bytes inside a string.  Carried across blocks. */
    uint64_t in_string = jsoc_prefix_xor(quote) ^ prev_in_string;
    prev_in_string = (uint64_t)((int64_t)in_string >> 63);

    /* Whitespace outside strings = whitespace & ~in_string */
    uint32_t ws_lo = (uint32_t)(m.whitespace & ~in_string);
    uint32_t ws_hi = (uint32_t)((m.whitespace >> 32) & ~(in_string >> 32));

    /* Compress out the whitespace. */
    size_t n = jsoc_compress64_avx2((const uint8_t *)(p + i), ws_lo, ws_hi,
                                    (uint8_t *)(data + out_pos));
    out_pos += n;
    i += 64;
  }

  /* Handle remainder with scalar fallback that tracks string state. */
  bool in_string_scalar = (prev_in_string != 0);
  bool escape_next = false;
  while (i < length) {
    unsigned char c = (unsigned char)p[i];
    
    if (escape_next) {
      /* This byte is escaped, treat it as ordinary content. */
      escape_next = false;
      data[out_pos++] = c;
      i++;
      continue;
    }
    
    if (in_string_scalar) {
      /* Inside a string: only quote and backslash are special. */
      data[out_pos++] = c;
      if (c == '\\') {
        escape_next = true;
      } else if (c == '"') {
        in_string_scalar = false;
      }
      i++;
    } else {
      /* Outside a string: whitespace is removed, quote starts string. */
      if (jsoc_is_ws(c)) {
        i++;
      } else {
        data[out_pos++] = c;
        if (c == '"') {
          in_string_scalar = true;
        }
        i++;
      }
    }
  }

  data[out_pos] = '\0';
  *out = (jsoc_string){data, out_pos};
  return true;

#else
  /* Portable fallback: simple scalar scan with string tracking. */
  char *p = data;
  size_t i = 0;
  bool in_string = false;
  bool escape_next = false;

  while (i < length) {
    unsigned char c = (unsigned char)input[i];
    
    if (escape_next) {
      escape_next = false;
      *p++ = c;
      i++;
      continue;
    }
    
    if (in_string) {
      *p++ = c;
      if (c == '\\') {
        escape_next = true;
      } else if (c == '"') {
        in_string = false;
      }
      i++;
    } else {
      if (jsoc_is_ws(c)) {
        i++;
      } else {
        *p++ = c;
        if (c == '"') {
          in_string = true;
        }
        i++;
      }
    }
  }

  const size_t n = (size_t)(p - data);
  *p = '\0';
  *out = (jsoc_string){data, n};
  return true;
#endif
}
