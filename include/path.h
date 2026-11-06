#pragma once

#include "dom.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JSON Pointer (RFC 6901).
 *
 * A pointer is a sequence of reference tokens, each introduced by '/':
 *
 *     ""           -> the document itself
 *     "/foo"       -> the member "foo"
 *     "/foo/0"     -> the first element of the array at "/foo"
 *     "/a~1b"      -> the member "a/b"  (~1 escapes '/', ~0 escapes '~')
 *
 * Object keys are matched against their *logical* (escape-decoded) spelling,
 * so `jsoc_object_key_at`'s raw slice is not what is compared.  Array indices
 * must be decimal digits without leading '+'/'-' or redundant zeros.
 */

/* Look up a pointer.  Returns NULL when any step fails or the pointer is
 * malformed.  `pointer` need not be NUL-terminated. */
JSOC_API const jsoc_value *jsoc_pointer_get(const jsoc_document *doc,
                                            const jsoc_value *root,
                                            const char *pointer, size_t length);

/* NUL-terminated convenience wrapper. */
JSOC_API const jsoc_value *jsoc_pointer_get_cstr(const jsoc_document *doc,
                                                 const jsoc_value *root,
                                                 const char *pointer);

/* Set the pointer to the document root.  Useful for building absolute
 * pointers: `jsoc_pointer_set_root(&p); jsoc_pointer_push_token(&p, "foo", 3);`
 * ... then `jsoc_pointer_get(doc, doc->root, p.data, p.length)`. */
typedef struct {
  char *data; /* arena-owned, NUL-terminated buffer */
  size_t length;
  jsoc_arena *arena;
} jsoc_pointer_builder;

JSOC_API void jsoc_pointer_builder_init(jsoc_pointer_builder *b,
                                        jsoc_arena *arena);

/* Escape `token` and append it as one reference token.  Returns false only
 * when the arena allocation fails. */
JSOC_API bool jsoc_pointer_push_token(jsoc_pointer_builder *b,
                                      const char *token,
                                      size_t token_len) JSOC_NODISCARD;

#ifdef __cplusplus
}
#endif
