#define _GNU_SOURCE 1
#include "decode_direct.h"

#include "dom_internal.h"
#include "token.h"
#include "number.h"

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

/* Get the next token, advancing pos.  Returns false if at end. */
static inline bool next_token(jsoc_decode_ctx *ctx, const jsoc_token **out) {
  if (ctx->pos >= ctx->count)
    return false;
  *out = &ctx->tokens[ctx->pos++];
  return true;
}

/* Peek at the next token without advancing. */
static inline bool peek_token(jsoc_decode_ctx *ctx, const jsoc_token **out) {
  if (ctx->pos >= ctx->count)
    return false;
  *out = &ctx->tokens[ctx->pos];
  return true;
}

/* Decode a scalar value from the current token. */
static jsoc_decode_result decode_scalar_direct(jsoc_decode_ctx *ctx,
                                               jsoc_decode_kind kind, void *out) {
  const jsoc_token *tok;
  if (!next_token(ctx, &tok))
    return (jsoc_decode_result){JSOC_DECODE_UNEXPECTED_EOF, ctx->pos, NULL};

  const char *lexeme = ctx->input + tok->offset;
  size_t lexeme_len = tok->length;

  switch (kind) {
  case JSOC_DECODE_BOOL: {
    if (tok->type != JSOC_TOK_TRUE && tok->type != JSOC_TOK_FALSE)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    bool v = (tok->type == JSOC_TOK_TRUE);
    memcpy(out, &v, sizeof v);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  case JSOC_DECODE_INT64: {
    if (tok->type != JSOC_TOK_NUMBER)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    int64_t v;
    bool ok = jsoc_number_to_int64(lexeme, lexeme_len, &v);
    if (!ok)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    memcpy(out, &v, sizeof v);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  case JSOC_DECODE_UINT64: {
    if (tok->type != JSOC_TOK_NUMBER)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    uint64_t v;
    bool ok = jsoc_number_to_uint64(lexeme, lexeme_len, &v);
    if (!ok)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    memcpy(out, &v, sizeof v);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  case JSOC_DECODE_DOUBLE: {
    if (tok->type != JSOC_TOK_NUMBER)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    double v;
    bool ok = jsoc_number_to_double(lexeme, lexeme_len, &v);
    if (!ok)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    memcpy(out, &v, sizeof v);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  case JSOC_DECODE_STRING_REF:
  case JSOC_DECODE_STRING_SLICE: {
    if (tok->type != JSOC_TOK_STRING)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
    jsoc_string s = {lexeme, lexeme_len};
    memcpy(out, &s, sizeof s);
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
  }
  default:
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos - 1, NULL};
  }
}

JSOC_API void jsoc_decode_ctx_init(jsoc_decode_ctx *ctx,
                                   const jsoc_token *tokens, size_t count,
                                   const char *input) {
  ctx->tokens = tokens;
  ctx->count = count;
  ctx->pos = 0;
  ctx->input = input;
}

JSOC_API jsoc_decode_result jsoc_decode_value(jsoc_decode_ctx *ctx,
                                              jsoc_decode_kind kind, void *out) {
  const jsoc_token *tok;
  if (!peek_token(ctx, &tok))
    return (jsoc_decode_result){JSOC_DECODE_UNEXPECTED_EOF, ctx->pos, NULL};

  if (tok->type == JSOC_TOK_OBJ_BEGIN) {
    if (kind != JSOC_DECODE_STRUCT)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL}; /* caller must handle object */
  }
  if (tok->type == JSOC_TOK_ARR_BEGIN) {
    if (kind != JSOC_DECODE_ARRAY)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};
    return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL}; /* caller must handle array */
  }
  return decode_scalar_direct(ctx, kind, out);
}

static bool skip_value(jsoc_decode_ctx *ctx) {
  const jsoc_token *tok;
  if (!next_token(ctx, &tok))
    return false;

  if (tok->type == JSOC_TOK_OBJ_BEGIN) {
    int depth = 1;
    while (depth > 0 && ctx->pos < ctx->count) {
      const jsoc_token *t;
      if (!next_token(ctx, &t))
        return false;
      if (t->type == JSOC_TOK_OBJ_BEGIN) depth++;
      else if (t->type == JSOC_TOK_OBJ_END) depth--;
    }
    return true;
  }
  if (tok->type == JSOC_TOK_ARR_BEGIN) {
    int depth = 1;
    while (depth > 0 && ctx->pos < ctx->count) {
      const jsoc_token *t;
      if (!next_token(ctx, &t))
        return false;
      if (t->type == JSOC_TOK_ARR_BEGIN) depth++;
      else if (t->type == JSOC_TOK_ARR_END) depth--;
    }
    return true;
  }
  return true;
}

JSOC_API jsoc_decode_result jsoc_decode_object_direct(jsoc_decode_ctx *ctx,
                                                      const jsoc_decode_field *fields,
                                                      size_t field_count,
                                                      void *out) {
  const jsoc_token *tok;
  if (!next_token(ctx, &tok) || tok->type != JSOC_TOK_OBJ_BEGIN)
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};

  size_t n = 0;
  while (n < JSOC_DECODE_MAX_FIELDS + 1 && fields[n].name != NULL)
    n++;

  if (field_count > JSOC_DECODE_MAX_FIELDS) {
    return (jsoc_decode_result){JSOC_DECODE_CUSTOM, 0, NULL};
  }

  bool seen[JSOC_DECODE_MAX_FIELDS] = {false};

  while (ctx->pos < ctx->count) {
    const jsoc_token *peeked;
    if (!peek_token(ctx, &peeked))
      return (jsoc_decode_result){JSOC_DECODE_UNEXPECTED_EOF, ctx->pos, NULL};

    if (peeked->type == JSOC_TOK_OBJ_END) {
      next_token(ctx, &peeked); /* consume } */
      break;
    }

    if (peeked->type != JSOC_TOK_STRING)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};

    /* Get the key */
    next_token(ctx, &peeked);
    const char *key = ctx->input + peeked->offset;
    size_t key_len = peeked->length;

    /* Expect colon */
    if (!next_token(ctx, &peeked) || peeked->type != JSOC_TOK_COLON)
      return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};

    /* Match key against descriptor */
    bool found = false;
    for (size_t i = 0; i < n; i++) {
      if (fields[i].name && strlen(fields[i].name) == key_len &&
          memcmp(fields[i].name, key, key_len) == 0) {
        if (seen[i])
          return (jsoc_decode_result){JSOC_DECODE_DUPLICATE_KEY, ctx->pos, fields[i].name};
        seen[i] = true;
        found = true;

        jsoc_decode_result r = decode_scalar_direct(ctx, fields[i].kind,
                                                    (char *)out + fields[i].offset);
        if (r.code != JSOC_DECODE_OK) {
          r.field_name = fields[i].name;
          return r;
        }
        break;
      }
    }

    if (!found) {
      /* Skip unknown key's value */
      if (!skip_value(ctx))
        return (jsoc_decode_result){JSOC_DECODE_UNEXPECTED_EOF, ctx->pos, NULL};
    }
  }

  /* Check required fields */
  for (size_t i = 0; i < n; i++) {
    if (fields[i].required && !seen[i])
      return field_missing(&fields[i]);
  }

  return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
}

JSOC_API jsoc_decode_result jsoc_decode_array_direct(jsoc_decode_ctx *ctx,
                                                     const jsoc_decode_field *field,
                                                     void *out) {
  const jsoc_token *tok;
  if (!next_token(ctx, &tok) || tok->type != JSOC_TOK_ARR_BEGIN)
    return (jsoc_decode_result){JSOC_DECODE_TYPE_MISMATCH, ctx->pos, NULL};

  size_t *count_ptr = (size_t *)((char *)out + field->count_offset);
  size_t capacity = *count_ptr;
  size_t count = 0;
  char *base = (char *)out + field->offset;

  while (ctx->pos < ctx->count) {
    const jsoc_token *peeked;
    if (!peek_token(ctx, &peeked))
      return (jsoc_decode_result){JSOC_DECODE_UNEXPECTED_EOF, ctx->pos, NULL};

    if (peeked->type == JSOC_TOK_ARR_END) {
      next_token(ctx, &peeked); /* consume ] */
      break;
    }

    if (count >= capacity)
      return (jsoc_decode_result){JSOC_DECODE_CUSTOM, ctx->pos, NULL};

    char *elem_ptr = base + count * field->item_size;
    jsoc_decode_result r = decode_scalar_direct(ctx, field->item_kind, elem_ptr);
    if (r.code != JSOC_DECODE_OK)
      return r;
    count++;
  }

  *count_ptr = count;
  return (jsoc_decode_result){JSOC_DECODE_OK, 0, NULL};
}
