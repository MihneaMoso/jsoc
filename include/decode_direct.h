/* Direct token-stream decoding (no DOM materialization).
 *
 * This API decodes JSON directly from a token stream into native structs,
 * avoiding the intermediate DOM tree.  It mirrors simdjson's ondemand API
 * but works with jsoc's token stream.
 */

#ifndef JSOC_DECODE_DIRECT_H
#define JSOC_DECODE_DIRECT_H

#include "decode.h"
#include "token.h"
#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decode context for direct token-stream decoding. */
typedef struct {
  const jsoc_token *tokens;
  size_t count;
  size_t pos;
  const char *input;
} jsoc_decode_ctx;

/* Initialize a decode context from a token stream. */
JSOC_API void jsoc_decode_ctx_init(jsoc_decode_ctx *ctx,
                                   const jsoc_token *tokens, size_t count,
                                   const char *input);

/* Decode a single value from the current position.  Advances ctx->pos. */
JSOC_API jsoc_decode_result jsoc_decode_value(jsoc_decode_ctx *ctx,
                                              jsoc_decode_kind kind, void *out);

/* Decode an object from the current position (expects object start). */
JSOC_API jsoc_decode_result jsoc_decode_object_direct(jsoc_decode_ctx *ctx,
                                                      const jsoc_decode_field *fields,
                                                      size_t field_count,
                                                      void *out);

/* Decode an array from the current position (expects array start). */
JSOC_API jsoc_decode_result jsoc_decode_array_direct(jsoc_decode_ctx *ctx,
                                                     const jsoc_decode_field *field,
                                                     void *out);

#ifdef __cplusplus
}
#endif

#endif /* JSOC_DECODE_DIRECT_H */
