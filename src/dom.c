#define _GNU_SOURCE 1

#include "dom.h"
#include "dom_internal.h"
#include "jsoc.h"
#include "number.h"
#include "tokenizer_internal.h"
#include "utf8.h"

#include <stdio.h>
#include <string.h>

typedef struct {
  const char *ptr;
  size_t length;
} dom_cached_string;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct {
  jsoc_document *doc;
  const jsoc_token *tokens;
  size_t pos;
  size_t end;
  uint32_t next_value;
  uint32_t next_member;
  size_t overflow_token; /* first unrepresentable number, or SIZE_MAX */
  bool error;
  jsoc_error_code err_code;
  size_t err_offset;
} dom_builder;

/* ------------------------------------------------------------------------- */
/* DOM builder                                                                */
/* ------------------------------------------------------------------------- */

static uint32_t dom_scalar(dom_builder *b, jsoc_value_type type, size_t offset,
                           size_t length) {
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->offset = (uint32_t)offset;
  v->length = (uint32_t)length;
  v->next = JSOC_NO_INDEX;
  v->type = (uint8_t)type;
  v->flags = 0;
  v->reserved = 0;
  return index;
}

static inline JSOC_ALWAYS_INLINE uint32_t dom_build_value(dom_builder *b);
static uint32_t dom_build_array(dom_builder *b);
static uint32_t dom_build_object(dom_builder *b);

static uint32_t dom_build_array(dom_builder *b) {
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->first = JSOC_NO_INDEX;
  v->next = JSOC_NO_INDEX;
  v->count = 0;
  v->type = (uint8_t)JSOC_VALUE_ARRAY;
  v->flags = 0;
  v->reserved = 0;

  b->pos++; /* '[' */
  uint32_t first = JSOC_NO_INDEX;
  uint32_t prev = JSOC_NO_INDEX;
  uint32_t count = 0;
  for (;;) {
    const jsoc_token_type t = b->tokens[b->pos].type;
    uint32_t child;
    if (JSOC_LIKELY(t >= JSOC_TOK_STRING)) {
      /* Token types STRING..NULL map 1:1 onto the value types in
       * reverse order, so no branch is needed for the five scalars. */
      const jsoc_token tok = b->tokens[b->pos++];
      child = dom_scalar(b, (jsoc_value_type)(JSOC_TOK_NULL - t), tok.offset,
                         tok.length);
    } else if (JSOC_LIKELY(t == JSOC_TOK_ARR_BEGIN)) {
      child = dom_build_array(b);
    } else if (JSOC_LIKELY(t == JSOC_TOK_OBJ_BEGIN)) {
      child = dom_build_object(b);
    } else {
      break; /* ']' */
    }
    if (prev == JSOC_NO_INDEX) {
      first = child;
    } else {
      b->doc->values[prev].next = child;
    }
    prev = child;
    count++;
    /* Compact stream: no ',' token.  The tokenizer already rejected a
     * missing separator, so the next element simply starts here. */
  }
  b->pos++; /* ']' */
  v->first = first;
  v->count = count;
  return index;
}

static uint32_t dom_build_object(dom_builder *b) {
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->first = JSOC_NO_INDEX;
  v->next = JSOC_NO_INDEX;
  v->count = 0;
  v->type = (uint8_t)JSOC_VALUE_OBJECT;
  v->flags = 0;
  v->reserved = 0;

  b->pos++; /* '{' */
  uint32_t first = JSOC_NO_INDEX;
  uint32_t prev = JSOC_NO_INDEX;
  uint32_t count = 0;
  for (;;) {
    if (b->tokens[b->pos].type != JSOC_TOK_STRING)
      break; /* '}' */
    const jsoc_token key = b->tokens[b->pos++];
    /* Compact stream: the ':' is implied by the tokenizer's grammar. */
    const uint32_t child = dom_build_value(b);

    const uint32_t m = b->next_member++;
    jsoc_member *member = &b->doc->members[m];
    member->key_offset = key.offset;
    member->key_length = key.length;
    member->value = child;
    member->next = JSOC_NO_INDEX;
    if (prev == JSOC_NO_INDEX) {
      first = m;
    } else {
      b->doc->members[prev].next = m;
    }
    prev = m;
    count++;
    /* Compact stream: no ',' token; the loop re-checks for a key. */
  }
  b->pos++; /* '}' */
  v->first = first;
  v->count = count;
  return index;
}

static inline JSOC_ALWAYS_INLINE uint32_t dom_build_value(dom_builder *b) {
  const jsoc_token tok = b->tokens[b->pos];
  const jsoc_token_type t = tok.type;
  if (JSOC_LIKELY(t >= JSOC_TOK_STRING)) {
    b->pos++;
    return dom_scalar(b, (jsoc_value_type)(JSOC_TOK_NULL - t), tok.offset,
                      tok.length);
  }
  if (JSOC_LIKELY(t == JSOC_TOK_ARR_BEGIN))
    return dom_build_array(b);
  if (JSOC_LIKELY(t == JSOC_TOK_OBJ_BEGIN))
    return dom_build_object(b);
  /* The tokenizer only produces a document for a value; any other token
   * here means the stream is malformed, which cannot happen after a
   * successful tokenize. */
  b->error = true;
  return JSOC_NO_INDEX;
}

/* ------------------------------------------------------------------------- */
/* Index-driven builder (split DOM path)                                      */
/* ------------------------------------------------------------------------- */

#if JSOC_SIMD_AVX2
/* `jsoc_dom_stage1` already validated UTF-8 and string escapes, so this pass
 * only has to walk the structural indexes, check the grammar, validate number
 * ranges and materialise the tree.  Any anomaly aborts and makes `jsoc_parse`
 * replay the ordinary tokenizer, which reports the exact error and partial
 * token stream; that keeps malformed input semantics untouched. */
typedef struct {
  const char *input;
  size_t length;
  const uint32_t *indexes;
  size_t count;
  size_t pos;
  jsoc_document *doc;
  uint32_t next_value;
  uint32_t next_member;
  size_t value_cap;
  size_t member_cap;
  unsigned depth;
  bool fail;
} idx_builder;

static uint32_t idx_parse_value(idx_builder *b);
static uint32_t idx_parse_array(idx_builder *b);
static uint32_t idx_parse_object(idx_builder *b);

static inline JSOC_ALWAYS_INLINE uint32_t
idx_scalar(idx_builder *b, jsoc_value_type type, uint32_t offset,
           uint32_t length) {
  if (JSOC_UNLIKELY(b->next_value >= b->value_cap)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->offset = offset;
  v->length = length;
  v->next = JSOC_NO_INDEX;
  v->type = (uint8_t)type;
  v->flags = 0;
  v->reserved = 0;
  return index;
}

static inline JSOC_ALWAYS_INLINE bool idx_scalar_end(const idx_builder *b,
                                                     size_t end) {
  if (end >= b->length)
    return true;
  const unsigned char c = (unsigned char)b->input[end];
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
         c == '}' || c == ']';
}

/* The next index after an opening quote is necessarily the closing quote:
 * stage 1 emits no starts for string contents, and escaped quotes are
 * excluded from the quote mask. */
static inline JSOC_ALWAYS_INLINE bool
idx_string(idx_builder *b, uint32_t *offset, uint32_t *length) {
  const size_t p = b->pos;
  if (JSOC_UNLIKELY(p + 1 >= b->count))
    return false;
  const uint32_t open = b->indexes[p];
  const uint32_t close = b->indexes[p + 1];
  /* Both call sites already classified `open` as a quote; `close` is the next
   * structural start and stage 1 only marks real quotes there. */
  if (JSOC_UNLIKELY(close <= open))
    return false;
  *offset = open + 1;
  *length = close - open - 1;
  b->pos = p + 2;
  return true;
}

static uint32_t idx_literal(idx_builder *b, unsigned char c) {
  const uint32_t s = b->indexes[b->pos];
  const char *lit = (c == 't') ? "true" : ((c == 'f') ? "false" : "null");
  const size_t litlen = (c == 'f') ? 5 : 4;
  const jsoc_value_type type = (c == 't')   ? JSOC_VALUE_TRUE
                               : (c == 'f') ? JSOC_VALUE_FALSE
                                            : JSOC_VALUE_NULL;
  if (JSOC_UNLIKELY(s + litlen > b->length ||
                    memcmp(b->input + s, lit, litlen) != 0 ||
                    !idx_scalar_end(b, s + litlen))) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  b->pos++;
  return idx_scalar(b, type, s, (uint32_t)litlen);
}

#ifdef JSOC_EXP_S2_FASTNUM
static uint32_t idx_number(idx_builder *b) {
  const uint32_t s = b->indexes[b->pos];
  size_t end = (b->pos + 1 < b->count) ? b->indexes[b->pos + 1] : b->length;
  while (end > s) {
    const unsigned char c = (unsigned char)b->input[end - 1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    end--;
  }
  b->pos++;
  return idx_scalar(b, JSOC_VALUE_NUMBER, s, (uint32_t)(end - s));
}
#else
static uint32_t idx_number(idx_builder *b) {
  const uint32_t s = b->indexes[b->pos];
  size_t end = 0;
  size_t err_off = 0;
  bool out_of_range = false;
  const jsoc_error_code ec =
      jsoc_validate_number_checked(b->input, b->length, s, b->doc->arena, &end, &err_off,
                           &out_of_range);
  /* Out-of-range numbers take the slow path so the existing overflow/trailing
   * document scoping logic stays authoritative. */
  if (JSOC_UNLIKELY(ec != JSOC_OK || out_of_range ||
                    !idx_scalar_end(b, end))) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  b->pos++;
  return idx_scalar(b, JSOC_VALUE_NUMBER, s, (uint32_t)(end - s));
}
#endif

static inline JSOC_ALWAYS_INLINE uint32_t idx_parse_value(idx_builder *b) {
  if (JSOC_UNLIKELY(b->pos >= b->count)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  const unsigned char c = (unsigned char)b->input[b->indexes[b->pos]];
  if (JSOC_LIKELY(c == '"')) {
    uint32_t offset;
    uint32_t length;
    if (JSOC_UNLIKELY(!idx_string(b, &offset, &length))) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    return idx_scalar(b, JSOC_VALUE_STRING, offset, length);
  }
  if (JSOC_LIKELY(c == '{'))
    return idx_parse_object(b);
  if (JSOC_LIKELY(c == '['))
    return idx_parse_array(b);
  if (c == 't' || c == 'f' || c == 'n')
    return idx_literal(b, c);
  return idx_number(b);
}

static uint32_t idx_parse_array(idx_builder *b) {
  if (JSOC_UNLIKELY(b->next_value >= b->value_cap ||
                    ++b->depth > JSOC_MAX_DEPTH)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->first = JSOC_NO_INDEX;
  v->next = JSOC_NO_INDEX;
  v->count = 0;
  v->type = (uint8_t)JSOC_VALUE_ARRAY;
  v->flags = 0;
  v->reserved = 0;

  b->pos++; /* '[' */
  if (JSOC_UNLIKELY(b->pos >= b->count)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  if (b->input[b->indexes[b->pos]] == ']') {
    b->pos++;
    b->depth--;
    return index;
  }

  uint32_t first = JSOC_NO_INDEX;
  uint32_t prev = JSOC_NO_INDEX;
  uint32_t count = 0;
  for (;;) {
    const uint32_t child = idx_parse_value(b);
    if (JSOC_UNLIKELY(child == JSOC_NO_INDEX))
      return JSOC_NO_INDEX;
    if (prev == JSOC_NO_INDEX) {
      first = child;
    } else {
      b->doc->values[prev].next = child;
    }
    prev = child;
    count++;

    if (JSOC_UNLIKELY(b->pos >= b->count)) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    const unsigned char c = (unsigned char)b->input[b->indexes[b->pos]];
    if (c == ',') {
      b->pos++;
      continue;
    }
    if (JSOC_UNLIKELY(c != ']')) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    b->pos++;
    break;
  }
  b->depth--;
  v->first = first;
  v->count = count;
  return index;
}

static uint32_t idx_parse_object(idx_builder *b) {
  if (JSOC_UNLIKELY(b->next_value >= b->value_cap ||
                    ++b->depth > JSOC_MAX_DEPTH)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  const uint32_t index = b->next_value++;
  jsoc_value *v = &b->doc->values[index];
  v->first = JSOC_NO_INDEX;
  v->next = JSOC_NO_INDEX;
  v->count = 0;
  v->type = (uint8_t)JSOC_VALUE_OBJECT;
  v->flags = 0;
  v->reserved = 0;

  b->pos++; /* '{' */
  if (JSOC_UNLIKELY(b->pos >= b->count)) {
    b->fail = true;
    return JSOC_NO_INDEX;
  }
  if (b->input[b->indexes[b->pos]] == '}') {
    b->pos++;
    b->depth--;
    return index;
  }

  uint32_t first = JSOC_NO_INDEX;
  uint32_t prev = JSOC_NO_INDEX;
  uint32_t count = 0;
  for (;;) {
    if (JSOC_UNLIKELY(b->pos >= b->count ||
                      b->input[b->indexes[b->pos]] != '"')) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    uint32_t key_offset;
    uint32_t key_length;
    if (JSOC_UNLIKELY(!idx_string(b, &key_offset, &key_length))) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    if (JSOC_UNLIKELY(b->pos >= b->count ||
                      b->input[b->indexes[b->pos]] != ':')) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    b->pos++;
    const uint32_t child = idx_parse_value(b);
    if (JSOC_UNLIKELY(child == JSOC_NO_INDEX))
      return JSOC_NO_INDEX;

    if (JSOC_UNLIKELY(b->next_member >= b->member_cap)) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    const uint32_t m = b->next_member++;
    jsoc_member *member = &b->doc->members[m];
    member->key_offset = key_offset;
    member->key_length = key_length;
    member->value = child;
    member->next = JSOC_NO_INDEX;
    if (prev == JSOC_NO_INDEX) {
      first = m;
    } else {
      b->doc->members[prev].next = m;
    }
    prev = m;
    count++;

    if (JSOC_UNLIKELY(b->pos >= b->count)) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    const unsigned char c = (unsigned char)b->input[b->indexes[b->pos]];
    if (c == ',') {
      b->pos++;
      continue;
    }
    if (JSOC_UNLIKELY(c != '}')) {
      b->fail = true;
      return JSOC_NO_INDEX;
    }
    b->pos++;
    break;
  }
  b->depth--;
  v->first = first;
  v->count = count;
  return index;
}

/* The split path pays for grammar in stage 2, so it only wins when the input
 * is structurally dense.  Long-string documents (low punctuation/quote
 * density) are better served by the fused tokenizer, which does grammar work
 * the scan is already touching.  Sampling a 4 KiB prefix is enough to tell
 * them apart and costs a few microseconds. */
typedef struct {
  bool worthwhile;
  bool branchless; /* string-dense: skip the per-block empty-string test */
} dom_probe;

static dom_probe dom_probe_prefix(const char *input, size_t length) {
  const size_t limit = length < 4096 ? length : 4096;
  size_t base = 0;
  size_t hits = 0;
  size_t quotes = 0;
  const uint8_t *p = (const uint8_t *)input;
  while (base + 64 <= limit) {
    const jsoc_block_masks m = jsoc_classify64(p + base);
    hits += (size_t)__builtin_popcountll(m.op | m.quote);
    quotes += (size_t)__builtin_popcountll(m.quote);
    base += 64;
  }
  for (; base < limit; base++) {
    const unsigned char c = p[base];
    if (c == '{' || c == '}' || c == '[' || c == ']' || c == ':' ||
        c == ',' || c == '"')
      hits++;
    if (c == '"')
      quotes++;
  }
  dom_probe probe;
  /* At least one structural byte per 16 bytes. */
  probe.worthwhile = hits * 16 >= limit;
  /* At least one quote per 32 bytes: the input is mostly strings, so paying
   * the prefix XOR unconditionally beats a per-block branch. */
  probe.branchless = quotes * 32 >= limit;
  return probe;
}

/* Build one document from the index stream.  Returns false without setting a
 * final error when the input needs the ordinary path. */
static bool dom_build_indexed(jsoc_document *doc, const uint32_t *indexes,
                              size_t count, jsoc_arena *arena) {
#ifdef JSOC_EXP_S2_WALK
  uint64_t sum = 0;
  for (size_t i = 0; i < count; i++)
    sum += (unsigned char)doc->input[indexes[i]];
  if (sum == 0xFFFFFFFFu)
    return false;
  doc->values = NULL;
  doc->members = NULL;
  doc->value_count = 0;
  doc->member_count = 0;
  doc->root = NULL;
  return true;
#else
  const size_t value_cap = count / 2 + 2;
  const size_t member_cap = count / 2 + 2;

  doc->values = jsoc_arena_alloc_t(arena, jsoc_value, value_cap);
  doc->members = jsoc_arena_alloc_t(arena, jsoc_member, member_cap);
  if (JSOC_UNLIKELY(!doc->values || !doc->members))
    return false;
  doc->value_count = value_cap;
  doc->member_count = member_cap;
  doc->string_cache = NULL;

  idx_builder b = {
      .input = doc->input,
      .length = doc->length,
      .indexes = indexes,
      .count = count,
      .pos = 0,
      .doc = doc,
      .next_value = 0,
      .next_member = 0,
      .value_cap = value_cap,
      .member_cap = member_cap,
      .depth = 0,
      .fail = false,
  };
  const uint32_t root = idx_parse_value(&b);
  if (JSOC_UNLIKELY(b.fail || root == JSOC_NO_INDEX || b.pos != count))
    return false;

  doc->value_count = b.next_value;
  doc->member_count = b.next_member;
  doc->root = &doc->values[root];
  return true;
#endif
}
#endif /* JSOC_SIMD_AVX2 */

/* Build one document starting at `start`.  `limit` is the exclusive end of
 * the token stream (the EOF token index).  On success `*out_end` receives the
 * token index one past the document, which lets callers detect concatenated
 * documents without a separate scanning pass. */
static bool dom_build_document(jsoc_document *doc, const jsoc_token *tokens,
                               size_t start, size_t limit,
                               size_t overflow_token, jsoc_arena *arena,
                               size_t *out_end) {
  const size_t remaining = limit - start;
  /* The token stream is compact, so a token is either a value (a scalar or a
   * container begin) or a close: `remaining` is therefore a safe upper bound
   * on the value count.  A member costs at least key + value, so half of that
   * bounds the member count.  Only the used prefix of each array is touched,
   * and no exact counting pass is needed. */
  const size_t value_cap = remaining;
  const size_t member_cap = remaining / 2 + 1;

  doc->values = jsoc_arena_alloc_t(arena, jsoc_value, value_cap);
  doc->members = jsoc_arena_alloc_t(arena, jsoc_member, member_cap);
  if (JSOC_UNLIKELY(!doc->values || !doc->members)) {
    doc->error = (jsoc_error){JSOC_ERR_NOMEM, 0, 0, 0};
    return false;
  }
  /* Capacities while building; replaced by the exact counts on success. */
  doc->value_count = value_cap;
  doc->member_count = member_cap;
  doc->string_cache = NULL;

  dom_builder b = {
      .doc = doc,
      .tokens = tokens,
      .pos = start,
      .end = limit,
      .next_value = 0,
      .next_member = 0,
      .overflow_token = overflow_token,
      .error = false,
      .err_code = JSOC_OK,
      .err_offset = 0,
  };
  const uint32_t root = dom_build_value(&b);
  if (JSOC_UNLIKELY(overflow_token != SIZE_MAX && overflow_token < b.pos)) {
    doc->error =
        jsoc_error_at_offset(doc->input, doc->length, JSOC_ERR_NUMBER_OVERFLOW,
                             tokens[overflow_token].offset);
    return false;
  }
  if (JSOC_UNLIKELY(b.error || root == JSOC_NO_INDEX)) {
    const jsoc_error_code code =
        b.err_code != JSOC_OK ? b.err_code : JSOC_ERR_SYNTAX;
    const size_t offset =
        b.err_code != JSOC_OK
            ? b.err_offset
            : (b.pos < b.end ? b.tokens[b.pos].offset : doc->length);
    doc->error = jsoc_error_at_offset(doc->input, doc->length, code, offset);
    return false;
  }
  doc->value_count = b.next_value;
  doc->member_count = b.next_member;
  doc->root = &doc->values[root];
  if (out_end)
    *out_end = b.pos;
  return true;
}

/* ------------------------------------------------------------------------- */
/* String decoding                                                            */
/* ------------------------------------------------------------------------- */

static inline int hex_value(unsigned char c) {
  if (c >= '0' && c <= '9')
    return (int)(c - '0');
  if (c >= 'a' && c <= 'f')
    return (int)(c - 'a') + 10;
  if (c >= 'A' && c <= 'F')
    return (int)(c - 'A') + 10;
  return -1;
}

static inline char *write_utf8(char *out, uint32_t cp) {
  if (cp < 0x80) {
    *out++ = (char)cp;
  } else if (cp < 0x800) {
    *out++ = (char)(0xC0 | (cp >> 6));
    *out++ = (char)(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    *out++ = (char)(0xE0 | (cp >> 12));
    *out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *out++ = (char)(0x80 | (cp & 0x3F));
  } else {
    *out++ = (char)(0xF0 | (cp >> 18));
    *out++ = (char)(0x80 | ((cp >> 12) & 0x3F));
    *out++ = (char)(0x80 | ((cp >> 6) & 0x3F));
    *out++ = (char)(0x80 | (cp & 0x3F));
  }
  return out;
}

static bool decode_escaped_string(const char *raw, size_t length, char *out,
                                  size_t *out_len) {
  size_t i = 0;
  char *o = out;
  while (i < length) {
    const unsigned char c = (unsigned char)raw[i++];
    if (c != '\\') {
      *o++ = (char)c;
      continue;
    }
    if (JSOC_UNLIKELY(i >= length))
      return false;
    const unsigned char esc = (unsigned char)raw[i++];
    switch (esc) {
    case '"':
      *o++ = '"';
      break;
    case '\\':
      *o++ = '\\';
      break;
    case '/':
      *o++ = '/';
      break;
    case 'b':
      *o++ = '\b';
      break;
    case 'f':
      *o++ = '\f';
      break;
    case 'n':
      *o++ = '\n';
      break;
    case 'r':
      *o++ = '\r';
      break;
    case 't':
      *o++ = '\t';
      break;
    case 'u': {
      if (JSOC_UNLIKELY(i + 4 > length))
        return false;
      uint32_t cp = 0;
      for (int k = 0; k < 4; k++) {
        const int h = hex_value((unsigned char)raw[i + (size_t)k]);
        if (JSOC_UNLIKELY(h < 0))
          return false;
        cp = (cp << 4) | (uint32_t)h;
      }
      i += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= length && raw[i] == '\\' &&
          raw[i + 1] == 'u') {
        uint32_t low = 0;
        bool ok = true;
        for (int k = 0; k < 4; k++) {
          const int h = hex_value((unsigned char)raw[i + 2 + (size_t)k]);
          if (h < 0) {
            ok = false;
            break;
          }
          low = (low << 4) | (uint32_t)h;
        }
        if (ok && low >= 0xDC00 && low <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          i += 6;
        }
      }
      o = write_utf8(o, cp);
      break;
    }
    default:
      return false;
    }
  }
  *o = '\0';
  *out_len = (size_t)(o - out);
  return true;
}

static dom_cached_string *dom_cache(jsoc_document *doc) {
  if (doc->string_cache == NULL) {
    const size_t count = doc->value_count + doc->member_count;
    if (count == 0)
      return NULL;
    dom_cached_string *cache =
        jsoc_arena_alloc_t_zero(doc->arena, dom_cached_string, count);
    if (!cache)
      return NULL;
    doc->string_cache = cache;
  }
  return (dom_cached_string *)doc->string_cache;
}

/* Decode into the cache slot `slot`.  Slots [0, value_count) are values;
 * slots after that are object keys (value_count + member_index). */
static jsoc_string dom_cached_decode(jsoc_document *doc, size_t slot,
                                     size_t offset, size_t length) {
  dom_cached_string *cache = dom_cache(doc);
  if (JSOC_UNLIKELY(cache == NULL))
    return (jsoc_string){NULL, 0};
  dom_cached_string *entry = &cache[slot];
  if (entry->ptr != NULL)
    return (jsoc_string){entry->ptr, entry->length};

  char *buf = jsoc_arena_alloc(doc->arena, length + 1, 1);
  if (JSOC_UNLIKELY(!buf))
    return (jsoc_string){NULL, 0};
  size_t out_len = 0;
  if (JSOC_UNLIKELY(
          !decode_escaped_string(doc->input + offset, length, buf, &out_len))) {
    return (jsoc_string){NULL, 0};
  }
  entry->ptr = buf;
  entry->length = out_len;
  return (jsoc_string){buf, out_len};
}

/* ------------------------------------------------------------------------- */
/* Public parse entry points                                                  */
/* ------------------------------------------------------------------------- */

JSOC_API bool jsoc_validate_utf8(const char *input, size_t length,
                                 size_t *error_offset) {
  if (error_offset)
    *error_offset = 0;
  if (!input)
    return length == 0;
  return jsoc_utf8_valid(input, length, error_offset);
}

/* Token index one past the top-level value at `start`.  Only used by the
 * streaming path, where sizing each document's arrays from the remaining
 * stream would over-allocate badly. */
static size_t dom_skip_value(const jsoc_token *tokens, size_t start,
                             size_t limit) {
  if (start >= limit)
    return start;
  const jsoc_token_type type = tokens[start].type;
  if (type != JSOC_TOK_OBJ_BEGIN && type != JSOC_TOK_ARR_BEGIN) {
    return start + 1;
  }
  size_t i = start;
  unsigned depth = 0;
  do {
    switch (tokens[i].type) {
    case JSOC_TOK_OBJ_BEGIN:
    case JSOC_TOK_ARR_BEGIN:
      depth++;
      break;
    case JSOC_TOK_OBJ_END:
    case JSOC_TOK_ARR_END:
      depth--;
      break;
    default:
      break;
    }
    i++;
  } while (i < limit && depth != 0);
  /* Unbalanced at the end of the window: the document is truncated, not
   * complete.  Returning `limit` here would let the caller mistake the last
   * token for a closer. */
  return depth == 0 ? i : SIZE_MAX;
}

/* True when the value at `start` is complete inside [start, limit): a scalar
 * token, or a container whose closing token is present.  The streaming path
 * walks a truncated token stream after a tokenizer error, so a document that
 * never finished must be recognised rather than built. */
static bool dom_value_complete(const jsoc_token *tokens, size_t start,
                               size_t limit, size_t *out_end) {
  if (start >= limit)
    return false;
  const jsoc_token_type type = tokens[start].type;
  if (type != JSOC_TOK_OBJ_BEGIN && type != JSOC_TOK_ARR_BEGIN) {
    *out_end = start + 1;
    return true;
  }
  const size_t end = dom_skip_value(tokens, start, limit);
  if (end == SIZE_MAX || end > limit)
    return false;
  /* A container ends with its closing token only when the depth closed
   * exactly, i.e. the token before `end` is a closer. */
  if (end == start + 1)
    return false;
  const jsoc_token_type last = tokens[end - 1].type;
  const bool closed =
      (type == JSOC_TOK_OBJ_BEGIN && last == JSOC_TOK_OBJ_END) ||
      (type == JSOC_TOK_ARR_BEGIN && last == JSOC_TOK_ARR_END);
  if (!closed)
    return false;
  *out_end = end;
  return true;
}

JSOC_API jsoc_document jsoc_parse(const char *input, size_t length,
                                  jsoc_arena *arena) {
  jsoc_document doc;
  memset(&doc, 0, sizeof doc);
  doc.error = (jsoc_error)JSOC_ERR_INIT;

  if (JSOC_UNLIKELY(!input || length == 0)) {
    doc.error = (jsoc_error){JSOC_ERR_SYNTAX, 0, 1, 1};
    return doc;
  }
  if (JSOC_UNLIKELY(!arena)) {
    doc.error = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return doc;
  }
  if (JSOC_UNLIKELY(length > (size_t)UINT32_MAX)) {
    doc.error = (jsoc_error){JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1};
    return doc;
  }

#if JSOC_SIMD_AVX2
  /* Split fast path: stage 1 emits structural indexes (and validates UTF-8 /
   * string escapes); stage 2 walks them to build the tree.  Anything the fast
   * path cannot handle replays the ordinary tokenizer so errors stay exact. */
  const dom_probe probe =
      (length >= 64) ? dom_probe_prefix(input, length)
                     : (dom_probe){false, false};
  if (length >= 64 && probe.worthwhile) {
    uint32_t *indexes = NULL;
    size_t index_count = 0;
    if (jsoc_dom_stage1(input, length, arena, probe.branchless, &indexes,
                        &index_count)) {
      doc.input = input;
      doc.length = length;
      doc.arena = arena;
      if (dom_build_indexed(&doc, indexes, index_count, arena))
        return doc;
      memset(&doc, 0, sizeof doc);
      doc.error = (jsoc_error)JSOC_ERR_INIT;
    }
  }
#endif

  size_t overflow_token = SIZE_MAX;
  /* Compact mode: separators are validated by the tokenizer but not stored,
   * so the container loops below need no comma/colon bookkeeping.  UTF-8 is
   * validated inside the same stage-1 pass. */
  jsoc_result r =
      jsoc_tokenize_compact_utf8(input, length, arena, &overflow_token);
  if (JSOC_UNLIKELY(r.error.code != JSOC_OK)) {
    doc.error = r.error;
    return doc;
  }

  doc.input = input;
  doc.length = length;
  doc.arena = arena;

  const size_t limit = r.count - 1; /* EOF */
  size_t end = 0;
  if (JSOC_UNLIKELY(!dom_build_document(&doc, r.tokens, 0, limit,
                                        overflow_token, arena, &end))) {
    doc.root = NULL;
    return doc;
  }
  if (JSOC_UNLIKELY(end < limit)) {
    /* The tokenizer accepts whitespace-separated concatenated documents;
     * `jsoc_parse` exposes exactly one. */
    doc.root = NULL;
    doc.error = jsoc_error_at_offset(input, length, JSOC_ERR_TRAILING_GARBAGE,
                                     r.tokens[end].offset);
  }
  return doc;
}

static jsoc_error stream_run(jsoc_token *tokens, size_t limit,
                             const char *input, size_t length,
                             jsoc_arena *arena, size_t overflow_token,
                             jsoc_document_callback callback, void *user,
                             jsoc_error *fatal);

/* Whitespace per JSON (space, tab, LF, CR).  Mirrors jsoc_is_ws in simd.h. */
static inline bool dom_is_ws(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

JSOC_API jsoc_error jsoc_parse_many(const char *input, size_t length,
                                    jsoc_arena *arena,
                                    jsoc_document_callback callback,
                                    void *user) {
  if (JSOC_UNLIKELY(!input)) {
    return (jsoc_error){JSOC_ERR_SYNTAX, 0, 1, 1};
  }
  /* An empty input is an empty stream, not an error (simdjson agrees). */
  if (JSOC_UNLIKELY(length == 0)) {
    return (jsoc_error)JSOC_ERR_INIT;
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
  }
  if (JSOC_UNLIKELY(length > (size_t)UINT32_MAX)) {
    return (jsoc_error){JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1};
  }

  /* A UTF-8 BOM is accepted once, at the very start of the stream.  It is
   * not whitespace, so every later stage would reject it. */
  if (length >= 3 && (unsigned char)input[0] == 0xEF &&
      (unsigned char)input[1] == 0xBB && (unsigned char)input[2] == 0xBF) {
    input += 3;
    length -= 3;
    if (JSOC_UNLIKELY(length == 0)) {
      return (jsoc_error){JSOC_ERR_SYNTAX, 0, 1, 1};
    }
  }

  size_t utf8_offset = 0;
  if (JSOC_UNLIKELY(!jsoc_utf8_valid(input, length, &utf8_offset))) {
    return jsoc_error_at_offset(input, length, JSOC_ERR_INVALID_UTF8,
                                utf8_offset);
  }

  jsoc_arena *token_arena = jsoc_arena_create(length / 8 + 4096);
  if (JSOC_UNLIKELY(!token_arena)) {
    return (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
  }

  size_t overflow_token = SIZE_MAX;
  /* Stream mode: documents in a stream may be adjacent, which
   * `jsoc_tokenize_checked` would reject as trailing garbage.  Compact so the
   * DOM builder sees the same separator-free stream as `jsoc_parse`. */
  jsoc_result r = jsoc_tokenize_stream_compact_checked(
      input, length, token_arena, &overflow_token);
  if (JSOC_UNLIKELY(r.error.code != JSOC_OK)) {
    /* The whole input is tokenized before the first callback, so replay
     * the well-formed prefix that precedes the offending byte: the caller
     * then sees every complete document followed by the error, which is
     * what simdjson's document stream does.  A prefix that is itself
     * invalid (an unterminated document next to the bad bytes) leaves
     * nothing, so a truncated document is never exposed. */
    jsoc_error err = r.error;

    /* Whitespace-only input is an empty stream, not an error. */
    if (err.code == JSOC_ERR_EXPECTED_VALUE) {
      bool any_value = false;
      for (size_t i = 0; i < length; i++) {
        if (!dom_is_ws((unsigned char)input[i])) {
          any_value = true;
          break;
        }
      }
      if (!any_value) {
        jsoc_arena_destroy(token_arena);
        return (jsoc_error)JSOC_ERR_INIT;
      }
    }

    /* A document that opened before the error is never delivered: the
     * partial stream stops where the tokenizer did, so only complete
     * documents are walked. */
    if (JSOC_LIKELY(r.count > 1)) {
      jsoc_error fatal = (jsoc_error)JSOC_ERR_INIT;
      const jsoc_error delivered =
          stream_run(r.tokens, r.count, input, length, arena, overflow_token,
                     callback, user, &fatal);
      jsoc_arena_destroy(token_arena);
      if (delivered.code != JSOC_OK) {
        return delivered;
      }
      return err;
    }
    jsoc_arena_destroy(token_arena);
    return err;
  }
  if (JSOC_UNLIKELY(r.count == 0)) {
    jsoc_arena_destroy(token_arena);
    return jsoc_error_at_offset(input, length, JSOC_ERR_EXPECTED_VALUE, length);
  }

  jsoc_error end_err = (jsoc_error)JSOC_ERR_INIT;
  const jsoc_error result =
      stream_run(r.tokens, r.count - 1, input, length, arena, overflow_token,
                 callback, user, &end_err);
  /* The tokens live in `token_arena`, which must outlive stream_run but not
   * its return value. */
  jsoc_arena_destroy(token_arena);
  return result;
}

/* Deliver one document at a time from the token stream [0, limit).  On
 * failure *fatal receives the error and no further document is delivered. */
static jsoc_error stream_run(jsoc_token *tokens, size_t limit,
                             const char *input, size_t length,
                             jsoc_arena *arena, size_t overflow_token,
                             jsoc_document_callback callback, void *user,
                             jsoc_error *fatal) {
  size_t start = 0;
  while (start < limit) {
    size_t hint = 0;
    if (!dom_value_complete(tokens, start, limit, &hint)) {
      /* A truncated trailing document is not delivered: the caller gets
       * the error instead. */
      break;
    }

    jsoc_arena_reset(arena);
    jsoc_document doc;
    memset(&doc, 0, sizeof doc);
    doc.error = (jsoc_error)JSOC_ERR_INIT;
    doc.input = input;
    doc.length = length;
    doc.arena = arena;

    size_t end = start;
    if (JSOC_UNLIKELY(!dom_build_document(&doc, tokens, start, hint,
                                          overflow_token, arena, &end))) {
      if (fatal) {
        *fatal = doc.error;
      }
      return doc.error;
    }
    if (JSOC_UNLIKELY(end <= start)) {
      jsoc_error err = jsoc_error_at_offset(input, length, JSOC_ERR_SYNTAX,
                                            tokens[start].offset);
      if (fatal) {
        *fatal = err;
      }
      return err;
    }
    if (callback && !callback(&doc, user)) {
      jsoc_error stop = {JSOC_ERR_USER_STOP, 0, 0, 0};
      if (fatal) {
        *fatal = stop;
      }
      return stop;
    }
    start = end;
  }

  return (jsoc_error)JSOC_ERR_INIT;
}

/* ------------------------------------------------------------------------- */
/* Files                                                                      */
/* ------------------------------------------------------------------------- */

static char *arena_read_file(const char *path, jsoc_arena *arena,
                             size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long end = ftell(f);
  if (end < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  size_t len = (size_t)end;
  char *buf = jsoc_arena_alloc(arena, len + 1, 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  const size_t got = fread(buf, 1, len, f);
  fclose(f);
  if (got != len)
    return NULL;
  buf[len] = '\0';
  *out_len = len;
  return buf;
}

JSOC_API jsoc_document jsoc_parse_file(const char *path, jsoc_arena *arena) {
  jsoc_document doc;
  memset(&doc, 0, sizeof doc);
  doc.error = (jsoc_error)JSOC_ERR_INIT;
  if (!path || !arena) {
    doc.error = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return doc;
  }
  size_t length = 0;
  char *input = arena_read_file(path, arena, &length);
  if (!input) {
    doc.error = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return doc;
  }
  return jsoc_parse(input, length, arena);
}

JSOC_API jsoc_error jsoc_parse_many_file(const char *path, jsoc_arena *arena,
                                         jsoc_document_callback callback,
                                         void *user) {
  if (!path || !arena)
    return (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};

  jsoc_arena *file_arena = jsoc_arena_create(65536);
  if (!file_arena)
    return (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
  size_t length = 0;
  char *input = arena_read_file(path, file_arena, &length);
  if (!input) {
    jsoc_arena_destroy(file_arena);
    return (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
  }
  jsoc_error err = jsoc_parse_many(input, length, arena, callback, user);
  jsoc_arena_destroy(file_arena);
  return err;
}

/* ------------------------------------------------------------------------- */
/* Accessors                                                                  */
/* ------------------------------------------------------------------------- */

JSOC_API jsoc_value_type jsoc_value_type_of(const jsoc_value *v) {
  return v ? (jsoc_value_type)v->type : JSOC_VALUE_NULL;
}

JSOC_API bool jsoc_value_is_null(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_NULL;
}
JSOC_API bool jsoc_value_is_bool(const jsoc_value *v) {
  return v && (v->type == JSOC_VALUE_TRUE || v->type == JSOC_VALUE_FALSE);
}
JSOC_API bool jsoc_value_is_number(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_NUMBER;
}
JSOC_API bool jsoc_value_is_string(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_STRING;
}
JSOC_API bool jsoc_value_is_array(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_ARRAY;
}
JSOC_API bool jsoc_value_is_object(const jsoc_value *v) {
  return v && v->type == JSOC_VALUE_OBJECT;
}

JSOC_API jsoc_string jsoc_value_raw(const jsoc_document *doc,
                                    const jsoc_value *v) {
  if (!doc || !v)
    return (jsoc_string){NULL, 0};
  switch (v->type) {
  case JSOC_VALUE_STRING:
  case JSOC_VALUE_NUMBER:
  case JSOC_VALUE_TRUE:
  case JSOC_VALUE_FALSE:
  case JSOC_VALUE_NULL:
    return (jsoc_string){doc->input + v->offset, v->length};
  default:
    return (jsoc_string){NULL, 0};
  }
}

JSOC_API jsoc_string jsoc_value_get_string(const jsoc_document *doc,
                                           const jsoc_value *v) {
  if (!doc || !v || v->type != JSOC_VALUE_STRING)
    return (jsoc_string){NULL, 0};
  const size_t slot = (size_t)(v - doc->values);
  if (JSOC_UNLIKELY(slot >= doc->value_count))
    return (jsoc_string){NULL, 0};
  return dom_cached_decode((jsoc_document *)doc, slot, v->offset, v->length);
}

JSOC_API bool jsoc_value_get_bool(const jsoc_value *v, bool *out) {
  if (out)
    *out = false;
  if (!v)
    return false;
  if (v->type == JSOC_VALUE_TRUE) {
    if (out)
      *out = true;
    return true;
  }
  if (v->type == JSOC_VALUE_FALSE)
    return true;
  return false;
}

static bool number_lexeme(const jsoc_document *doc, const jsoc_value *v,
                          const char **p, size_t *n) {
  if (!doc || !v || v->type != JSOC_VALUE_NUMBER)
    return false;
  *p = doc->input + v->offset;
  *n = v->length;
  return true;
}

JSOC_API bool jsoc_value_is_integer(const jsoc_document *doc,
                                    const jsoc_value *v) {
  const char *p;
  size_t n;
  if (!number_lexeme(doc, v, &p, &n))
    return false;
  return jsoc_number_is_integer(p, n);
}

JSOC_API bool jsoc_value_get_int64(const jsoc_document *doc,
                                   const jsoc_value *v, int64_t *out) {
  if (out)
    *out = 0;
  const char *p;
  size_t n;
  if (!number_lexeme(doc, v, &p, &n) || !jsoc_number_is_integer(p, n))
    return false;
  bool negative;
  uint64_t mag;
  if (!jsoc_number_integer_magnitude(p, n, &negative, &mag))
    return false;
  if (negative) {
    if (mag > ((uint64_t)INT64_MAX + 1u))
      return false;
    if (out)
      *out = (mag == ((uint64_t)INT64_MAX + 1u)) ? INT64_MIN : -(int64_t)mag;
    return true;
  }
  if (mag > (uint64_t)INT64_MAX)
    return false;
  if (out)
    *out = (int64_t)mag;
  return true;
}

JSOC_API bool jsoc_value_get_uint64(const jsoc_document *doc,
                                    const jsoc_value *v, uint64_t *out) {
  if (out)
    *out = 0;
  const char *p;
  size_t n;
  if (!number_lexeme(doc, v, &p, &n) || !jsoc_number_is_integer(p, n))
    return false;
  bool negative;
  uint64_t mag;
  if (!jsoc_number_integer_magnitude(p, n, &negative, &mag))
    return false;
  if (negative && mag != 0)
    return false;
  if (out)
    *out = mag;
  return true;
}

JSOC_API bool jsoc_value_get_double(const jsoc_document *doc,
                                    const jsoc_value *v, double *out) {
  if (out)
    *out = 0.0;
  const char *p;
  size_t n;
  if (!number_lexeme(doc, v, &p, &n))
    return false;
  return jsoc_lexeme_to_double(doc->arena, p, n, out);
}

/* ------------------------------------------------------------------------- */
/* Array/object access                                                        */
/* ------------------------------------------------------------------------- */

JSOC_API size_t jsoc_array_size(const jsoc_document *doc, const jsoc_value *v) {
  (void)doc;
  if (!v || v->type != JSOC_VALUE_ARRAY)
    return 0;
  return v->count;
}

JSOC_API const jsoc_value *jsoc_array_at(const jsoc_document *doc,
                                         const jsoc_value *v, size_t index) {
  if (!doc || !v || v->type != JSOC_VALUE_ARRAY || index >= v->count)
    return NULL;
  uint32_t child = v->first;
  while (index != 0) {
    child = doc->values[child].next;
    index--;
  }
  return &doc->values[child];
}

JSOC_API size_t jsoc_object_size(const jsoc_document *doc,
                                 const jsoc_value *v) {
  (void)doc;
  if (!v || v->type != JSOC_VALUE_OBJECT)
    return 0;
  return v->count;
}

static const jsoc_value *object_find(const jsoc_document *doc,
                                     const jsoc_value *v, const char *key,
                                     size_t key_len) {
  if (!doc || !v || v->type != JSOC_VALUE_OBJECT)
    return NULL;
  uint32_t m = v->first;
  while (m != JSOC_NO_INDEX) {
    const jsoc_member *member = &doc->members[m];
    const char *mk = doc->input + member->key_offset;
    const bool escaped = memchr(mk, '\\', member->key_length) != NULL;
    /* Fast path: escape-free key, compared against the raw slice.  A key
     * that contains a backslash must never match the caller's escaped
     * spelling; the logical key is what is being looked up. */
    if (!escaped && member->key_length == key_len &&
        memcmp(mk, key, key_len) == 0) {
      return &doc->values[member->value];
    }
    /* Slow path: decode only keys that actually contain escapes. */
    if (escaped) {
      jsoc_string decoded =
          dom_cached_decode((jsoc_document *)doc, doc->value_count + (size_t)m,
                            member->key_offset, member->key_length);
      if (decoded.data != NULL && decoded.length == key_len &&
          memcmp(decoded.data, key, key_len) == 0) {
        return &doc->values[member->value];
      }
    }
    m = member->next;
  }
  return NULL;
}

JSOC_API const jsoc_value *jsoc_object_get(const jsoc_document *doc,
                                           const jsoc_value *v, const char *key,
                                           size_t key_len) {
  if (!key)
    return NULL;
  return object_find(doc, v, key, key_len);
}

JSOC_API const jsoc_value *jsoc_object_get_cstr(const jsoc_document *doc,
                                                const jsoc_value *v,
                                                const char *key) {
  if (!key)
    return NULL;
  return jsoc_object_get(doc, v, key, strlen(key));
}

JSOC_API jsoc_string jsoc_object_key_at(const jsoc_document *doc,
                                        const jsoc_value *v, size_t index) {
  if (!doc || !v || v->type != JSOC_VALUE_OBJECT || index >= v->count) {
    return (jsoc_string){NULL, 0};
  }
  uint32_t m = v->first;
  while (index != 0) {
    m = doc->members[m].next;
    index--;
  }
  return (jsoc_string){doc->input + doc->members[m].key_offset,
                       doc->members[m].key_length};
}

JSOC_API jsoc_string jsoc_object_get_key(const jsoc_document *doc,
                                         const jsoc_value *v, size_t index) {
  if (!doc || !v || v->type != JSOC_VALUE_OBJECT || index >= v->count) {
    return (jsoc_string){NULL, 0};
  }
  uint32_t m = v->first;
  while (index != 0) {
    m = doc->members[m].next;
    index--;
  }
  return dom_cached_decode((jsoc_document *)doc, doc->value_count + (size_t)m,
                           doc->members[m].key_offset,
                           doc->members[m].key_length);
}

JSOC_API void jsoc_object_iter_init(const jsoc_document *doc,
                                    const jsoc_value *v, jsoc_object_iter *it) {
  it->doc = doc;
  it->object = v;
  it->member = (v && v->type == JSOC_VALUE_OBJECT) ? v->first : JSOC_NO_INDEX;
}

JSOC_API bool jsoc_object_iter_next(jsoc_object_iter *it, jsoc_string *key,
                                    const jsoc_value **value) {
  if (!it || it->member == JSOC_NO_INDEX || !it->doc || !it->object)
    return false;
  const jsoc_member *m = &it->doc->members[it->member];
  if (key) {
    *key = (jsoc_string){it->doc->input + m->key_offset, m->key_length};
  }
  if (value)
    *value = &it->doc->values[m->value];
  it->member = m->next;
  return true;
}
