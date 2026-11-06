#include "path.h"

#include "dom_internal.h"

#include <stdint.h>
#include <string.h>

/* Stateful decoder that walks a raw (escaped) key one *decoded* byte at a
 * time, so keys of any length can be compared without a buffer. */
typedef struct {
  const char *raw;
  size_t pos;
  size_t end;
  char pending[4];
  size_t pending_len;
  size_t pending_pos;
  uint32_t cp;
  bool bad;
} key_walker;

static void key_walker_init(key_walker *w, const char *raw, size_t len) {
  w->raw = raw;
  w->pos = 0;
  w->end = len;
  w->pending_len = 0;
  w->pending_pos = 0;
  w->bad = false;
}

/* Decode four hex digits after a `\u`.  Returns false (and marks the walker
 * bad) on anything else. */
static bool key_hex4(key_walker *w) {
  if (w->pos + 4 > w->end) {
    w->bad = true;
    return false;
  }
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    const unsigned char h = (unsigned char)w->raw[w->pos++];
    v <<= 4;
    if (h >= '0' && h <= '9') {
      v |= (uint32_t)(h - '0');
    } else if (h >= 'a' && h <= 'f') {
      v |= (uint32_t)(h - 'a' + 10);
    } else if (h >= 'A' && h <= 'F') {
      v |= (uint32_t)(h - 'A' + 10);
    } else {
      w->bad = true;
      return false;
    }
  }
  w->cp = v;
  return true;
}

/* Next decoded byte, or -1 at the end (or on malformed data). */
static int key_walker_next(key_walker *w) {
  if (w->pending_pos < w->pending_len)
    return (unsigned char)w->pending[w->pending_pos++];
  if (w->pos >= w->end)
    return -1;
  char c = w->raw[w->pos++];
  if (c != '\\')
    return (unsigned char)c;
  if (w->pos >= w->end) {
    w->bad = true;
    return -1;
  }
  const char n = w->raw[w->pos++];
  switch (n) {
  case '"':
    return '"';
  case '\\':
    return '\\';
  case '/':
    return '/';
  case 'b':
    return '\b';
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  default:
    break;
  }
  if (n != 'u') {
    w->bad = true;
    return -1;
  }

  uint32_t cp = 0;
  if (!key_hex4(w))
    return -1;
  cp = w->cp;
  if (cp >= 0xD800 && cp <= 0xDBFF && w->pos + 6 <= w->end &&
      w->raw[w->pos] == '\\' && w->raw[w->pos + 1] == 'u') {
    w->pos += 2;
    if (!key_hex4(w))
      return -1;
    const uint32_t lo = w->cp;
    if (lo >= 0xDC00 && lo <= 0xDFFF) {
      cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
    } else {
      cp = 0xFFFD; /* unpaired surrogate: replacement character */
    }
  }

  if (cp < 0x80)
    return (int)cp;
  if (cp < 0x800) {
    w->pending[0] = (char)(0xC0 | (cp >> 6));
    w->pending[1] = (char)(0x80 | (cp & 0x3F));
    w->pending_len = 2;
  } else if (cp < 0x10000) {
    w->pending[0] = (char)(0xE0 | (cp >> 12));
    w->pending[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    w->pending[2] = (char)(0x80 | (cp & 0x3F));
    w->pending_len = 3;
  } else {
    w->pending[0] = (char)(0xF0 | (cp >> 18));
    w->pending[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    w->pending[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    w->pending[3] = (char)(0x80 | (cp & 0x3F));
    w->pending_len = 4;
  }
  w->pending_pos = 1;
  return (unsigned char)w->pending[0];
}

/* Does member `m`'s raw (still escaped) key match the pointer's reference
 * token?  Both sides are decoded on the fly - JSON escapes on the key, RFC
 * 6901 escapes on the token - so no intermediate buffer is needed.  Duplicate
 * keys keep their first-match order. */
static bool key_matches_token(const char *raw, size_t raw_len,
                              const char *token, size_t token_len) {
  key_walker walk;
  key_walker_init(&walk, raw, raw_len);

  size_t o = 0;
  for (;;) {
    /* The token is already plain text apart from the two escapes. */
    int want = -1;
    if (o < token_len) {
      if (token[o] == '~') {
        if (o + 1 >= token_len)
          return false;
        if (token[o + 1] == '1')
          want = '/';
        else if (token[o + 1] == '0')
          want = '~';
        else
          return false;
        o += 2;
      } else {
        want = (unsigned char)token[o++];
      }
    }

    const int got = key_walker_next(&walk);
    if (want < 0 && got < 0)
      return !walk.bad;
    if (walk.bad)
      return false;
    if (got != want)
      return false;
  }
}

/* Object lookup by decoded key, matching jsoc_object_get's semantics. */
static const jsoc_value *pointer_object_get(const jsoc_document *doc,
                                            const jsoc_value *v,
                                            const char *token,
                                            size_t token_len) {
  uint32_t m = v->first;
  while (m != JSOC_NO_INDEX) {
    const jsoc_member *member = &doc->members[m];
    if (key_matches_token(doc->input + member->key_offset, member->key_length,
                          token, token_len)) {
      return &doc->values[member->value];
    }
    m = member->next;
  }
  return NULL;
}

/* Decode an array index token: "0", "1", ... only.  "-" (the end-of-array
 * token) and any non-canonical form are rejected, since they cannot name an
 * existing element. */
static bool pointer_array_index(const char *token, size_t token_len,
                                size_t *out) {
  if (token_len == 0 || token_len > 10)
    return false;
  size_t value = 0;
  for (size_t i = 0; i < token_len; i++) {
    const unsigned char c = (unsigned char)token[i];
    if (c < '0' || c > '9')
      return false;
    /* Leading zeros are not a valid index spelling ("01"). */
    if (i == 1 && token[0] == '0')
      return false;
    value = value * 10u + (size_t)(c - '0');
  }
  *out = value;
  return true;
}

JSOC_API const jsoc_value *jsoc_pointer_get_cstr(const jsoc_document *doc,
                                                 const jsoc_value *root,
                                                 const char *pointer) {
  if (!pointer)
    return NULL;
  return jsoc_pointer_get(doc, root, pointer, strlen(pointer));
}

JSOC_API const jsoc_value *jsoc_pointer_get(const jsoc_document *doc,
                                            const jsoc_value *root,
                                            const char *pointer,
                                            size_t length) {
  if (!doc || !root || !pointer)
    return NULL;

  const jsoc_value *cur = root;

  size_t i = 0;
  while (i < length) {
    if (pointer[i] != '/')
      return NULL; /* malformed: token without '/' */
    i++;

    const size_t start = i;
    while (i < length && pointer[i] != '/')
      i++;

    if (cur == NULL)
      return NULL;

    switch (cur->type) {
    case JSOC_VALUE_OBJECT:
      cur = pointer_object_get(doc, cur, pointer + start, i - start);
      break;
    case JSOC_VALUE_ARRAY: {
      size_t index;
      if (!pointer_array_index(pointer + start, i - start, &index))
        return NULL;
      cur = jsoc_array_at(doc, cur, index);
      break;
    }
    default:
      return NULL;
    }
  }

  return cur;
}

JSOC_API void jsoc_pointer_builder_init(jsoc_pointer_builder *b,
                                        jsoc_arena *arena) {
  if (!b)
    return;
  b->arena = arena;
  b->length = 0;
  b->data = arena ? jsoc_arena_alloc(arena, 1, 1) : NULL;
  if (b->data)
    b->data[0] = '\0';
}

JSOC_API bool jsoc_pointer_push_token(jsoc_pointer_builder *b,
                                      const char *token, size_t token_len) {
  if (!b || !b->arena || !token)
    return false;
  /* Worst case is one '/'-introducer plus "~0"/"~1" for every byte. */
  const size_t need = b->length + 1 + token_len * 2 + 1;
  char *p = jsoc_arena_alloc(b->arena, need, 1);
  if (!p)
    return false;
  if (b->data)
    memcpy(p, b->data, b->length);
  p[b->length++] = '/';
  for (size_t i = 0; i < token_len; i++) {
    const char c = token[i];
    if (c == '~') {
      p[b->length++] = '~';
      p[b->length++] = '0';
    } else if (c == '/') {
      p[b->length++] = '~';
      p[b->length++] = '1';
    } else {
      p[b->length++] = c;
    }
  }
  p[b->length] = '\0';
  b->data = p;
  return true;
}
