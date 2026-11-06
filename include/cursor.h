#pragma once

#include "dom.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Forward-only DOM cursor.
 *
 * The DOM is a fully materialized tree, so a cursor only saves the caller from
 * tracking sibling indexes.  Navigation is depth-first and forward-only, the
 * way simdjson's On-Demand cursor is:
 *
 *     {
 *       "a": [ { "b": 1 } ]
 *     }
 *
 *   - init on the object;
 *   - `down`   -> the first member (`"a"` -> array);
 *   - `down`   -> the array's first element (object);
 *   - `down`   -> `"b"`;
 *   - `next`   -> skips to the next sibling (none here);
 *   - `up`     -> back onto the parent (the array), and so on.
 *
 * A cursor is invalidated by anything that touches the document (for example
 * by `jsoc_parse_many` resetting the arena), so it is a stack-local helper.
 * It holds one stack entry per nesting level, so it is a few dozen kilobytes:
 * place it in automatic storage of a function that does not recurse.
 */

#define JSOC_CURSOR_MAX_DEPTH 1024

typedef struct {
  const jsoc_document *doc;

  /* `child` holds the member id (objects) or value index (arrays) the frame
   * currently sits on; `values` holds the containers being walked. */
  const jsoc_value *values[JSOC_CURSOR_MAX_DEPTH];
  uint32_t child[JSOC_CURSOR_MAX_DEPTH];
  jsoc_string keys[JSOC_CURSOR_MAX_DEPTH];
  uint32_t depth;

  /* Where the cursor is right now. */
  const jsoc_value *value;
  jsoc_string key;
} jsoc_cursor;

JSOC_API void jsoc_cursor_init(jsoc_cursor *c, const jsoc_document *doc);

/* Attach the cursor to a value; `key` is the raw key it is reachable by. */
JSOC_API void jsoc_cursor_set(jsoc_cursor *c, const jsoc_value *value,
                              const jsoc_string *key);

/* The current value (the document root before the first `down`). */
JSOC_API const jsoc_value *jsoc_cursor_value(const jsoc_cursor *c) JSOC_PURE;

/* The raw key of the current member, or {NULL, 0} at the root / in an array. */
JSOC_API jsoc_string jsoc_cursor_key(const jsoc_cursor *c) JSOC_PURE;

/* Enter the first member/element of the current object/array.  False when the
 * current value is not a container or is empty. */
JSOC_API bool jsoc_cursor_down(jsoc_cursor *c);

/* Skip to the next member/element of the current container.  False at the end
 * (the cursor stays on the last element). */
JSOC_API bool jsoc_cursor_next(jsoc_cursor *c);

/* Leave the current container, returning to its parent.  False at the root. */
JSOC_API bool jsoc_cursor_up(jsoc_cursor *c);

/* Convenience: descend if possible, otherwise move to the next sibling.
 * Equivalent to `jsoc_cursor_down(c) || jsoc_cursor_next(c)`, and the usual
 * way to walk a document. */
JSOC_API bool jsoc_cursor_enter(jsoc_cursor *c);

#ifdef __cplusplus
}
#endif
