#pragma once
#include "export.h"
#include "error.h"
#include "token.h"
#include "arena.h"
#include <stddef.h>

typedef struct {
    jsoc_error error;
    jsoc_token *tokens;
    size_t count;
} jsoc_result;

/*
 * Tokenize `length` bytes at `input`.  Tokens are zero-copy views: offsets
 * index into `input`, so the caller must keep `input` alive for as long as
 * the tokens (or at least the arena) are used.  Tokens themselves are
 * allocated from `arena`; nothing needs to be freed separately.
 *
 * On success `error.code == JSOC_OK`.  On failure `tokens`/`count` may hold
 * the partial prefix, but should not be relied on.
 */
JSOC_API jsoc_result jsoc_tokenize(const char *input, size_t length, jsoc_arena *arena) JSOC_NODISCARD;

/*
 * Read `path` into `arena` and tokenize it.  The file bytes live in the
 * arena, so tokens remain valid until the arena is reset or destroyed.
 */
JSOC_API jsoc_result jsoc_tokenize_file(const char *path, jsoc_arena *arena) JSOC_NODISCARD;

typedef enum {
    JSOC_DECODE_OK = 0,
    JSOC_DECODE_TYPE_MISMATCH,
    JSOC_DECODE_MISSING_FIELD,
    JSOC_DECODE_EXTRA_FIELD,
    JSOC_DECODE_OVERFLOW,
    JSOC_DECODE_CUSTOM,
} jsoc_decode_error;

typedef struct {
    jsoc_decode_error code;
    size_t token_index;
    const char *field_name;
} jsoc_decode_result;

JSOC_API const char *jsoc_decode_error_message(jsoc_decode_error code) JSOC_CONST JSOC_NODISCARD;