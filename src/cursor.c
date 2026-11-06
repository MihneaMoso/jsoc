#include "cursor.h"

#include "dom_internal.h"

#include <string.h>

static jsoc_string null_string(void) { return (jsoc_string){NULL, 0}; }

/* The raw key of member `m`. */
static jsoc_string member_key(const jsoc_document *doc, uint32_t m) {
  const jsoc_member *member = &doc->members[m];
  return (jsoc_string){doc->input + member->key_offset, member->key_length};
}

JSOC_API void jsoc_cursor_init(jsoc_cursor *c, const jsoc_document *doc) {
  c->doc = doc;
  c->depth = 0;
  c->value = doc ? doc->root : NULL;
  c->key = null_string();
}

JSOC_API void jsoc_cursor_set(jsoc_cursor *c, const jsoc_value *value,
                              const jsoc_string *key) {
  c->value = value;
  c->key = key ? *key : null_string();
  c->depth = 0;
}

JSOC_API const jsoc_value *jsoc_cursor_value(const jsoc_cursor *c) {
  return c->value;
}

JSOC_API jsoc_string jsoc_cursor_key(const jsoc_cursor *c) { return c->key; }

JSOC_API bool jsoc_cursor_down(jsoc_cursor *c) {
  if (!c->doc || !c->value)
    return false;
  if (c->value->count == 0)
    return false;
  if (c->depth >= JSOC_CURSOR_MAX_DEPTH)
    return false;

  c->values[c->depth] = c->value;
  c->keys[c->depth] = c->key;
  c->depth++;

  if (c->value->type == JSOC_VALUE_OBJECT) {
    const uint32_t m = c->value->first;
    c->child[c->depth - 1] = m;
    c->value = &c->doc->values[c->doc->members[m].value];
    c->key = member_key(c->doc, m);
  } else if (c->value->type == JSOC_VALUE_ARRAY) {
    const uint32_t i = c->value->first;
    c->child[c->depth - 1] = i;
    c->value = &c->doc->values[i];
    c->key = null_string();
  } else {
    c->depth--;
    return false;
  }
  return true;
}

JSOC_API bool jsoc_cursor_next(jsoc_cursor *c) {
  if (!c->doc || c->depth == 0)
    return false;
  const jsoc_value *parent = c->values[c->depth - 1];
  const uint32_t cur = c->child[c->depth - 1];

  if (parent->type == JSOC_VALUE_OBJECT) {
    const uint32_t m = c->doc->members[cur].next;
    if (m == JSOC_NO_INDEX)
      return false;
    c->child[c->depth - 1] = m;
    c->value = &c->doc->values[c->doc->members[m].value];
    c->key = member_key(c->doc, m);
    return true;
  }

  if (parent->type == JSOC_VALUE_ARRAY) {
    const uint32_t i = c->doc->values[cur].next;
    if (i == JSOC_NO_INDEX)
      return false;
    c->child[c->depth - 1] = i;
    c->value = &c->doc->values[i];
    c->key = null_string();
    return true;
  }

  return false;
}

JSOC_API bool jsoc_cursor_up(jsoc_cursor *c) {
  if (!c->doc || c->depth == 0)
    return false;
  c->depth--;
  c->value = c->values[c->depth];
  c->key = c->keys[c->depth];
  return true;
}

JSOC_API bool jsoc_cursor_enter(jsoc_cursor *c) {
  return jsoc_cursor_down(c) || jsoc_cursor_next(c);
}
