#pragma once
#include "error.h"
#include "export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum nesting depth accepted by the tokenizer (arrays plus objects).
 * Public because callers size frames from it. */
#define JSOC_MAX_DEPTH 1024

typedef enum jsoc_token_type : uint8_t {
  JSOC_TOK_EOF = 0,
  JSOC_TOK_OBJ_BEGIN,
  JSOC_TOK_OBJ_END,
  JSOC_TOK_ARR_BEGIN,
  JSOC_TOK_ARR_END,
  JSOC_TOK_COMMA,
  JSOC_TOK_COLON,
  JSOC_TOK_STRING,
  JSOC_TOK_NUMBER,
  JSOC_TOK_TRUE,
  JSOC_TOK_FALSE,
  JSOC_TOK_NULL,
} jsoc_token_type;

typedef struct {
  uint32_t offset;
  uint32_t length;
  jsoc_token_type type;
} jsoc_token;

JSOC_API void jsoc_token_print(const jsoc_token *tok, const char *input,
                               char *buf, size_t bufsz);

static inline const char *jsoc_token_type_name(jsoc_token_type t) {
  static const char *names[] = {"EOF",     "OBJ_BEGIN", "OBJ_END", "ARR_BEGIN",
                                "ARR_END", "COMMA",     "COLON",   "STRING",
                                "NUMBER",  "TRUE",      "FALSE",   "NULL"};
  return (t <= JSOC_TOK_NULL) ? names[t] : "UNKNOWN";
}

static inline jsoc_token jsoc_token_make(jsoc_token_type type, uint32_t offset,
                                         uint32_t length) {
  jsoc_token t;
  t.offset = offset;
  t.length = length;
  t.type = type;
  return t;
}
#ifdef __cplusplus
}
#endif
