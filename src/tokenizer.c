#define _GNU_SOURCE 1

#include "arena.h"
#include "error.h"
#include "jsoc.h"
#include "number.h"
#include "simd.h"
#include "token.h"
#include "tokenizer_internal.h"
#include "utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Hard limit on container nesting.  The parser is iterative, so this only
 * bounds the bracket stack; it exists to turn pathological input into a
 * clean error instead of unbounded memory use downstream. */

typedef struct {
  const char *input;
  size_t length;
  size_t pos;

  jsoc_arena *arena;
  jsoc_token *tokens;
  size_t count;
  size_t capacity;

  uint16_t depth;
  char stack[JSOC_MAX_DEPTH]; /* expected closing byte: '}' or ']' */
  bool after_comma;

  /* Checked mode: record (do not fail on) the first number lexeme that is
   * syntactically valid but unrepresentable.  The DOM decides later whether
   * that number belongs to the document it exposes. */
  bool check_numbers;

  /* Offset just past the most recent number/literal token (SIZE_MAX when the
   * last value was a string or a container).  A byte glued to a scalar can
   * never start a new document, in any mode. */
  size_t last_scalar_end;

  /* Stream mode: a second top-level value is accepted without whitespace
   * after the first one (ndjson streams).  `jsoc_parse` uses the strict
   * form, `jsoc_parse_many` the relaxed one. */
  bool stream;

  /* Compact mode: ',' and ':' are validated but not stored.  The DOM never
   * needs a separator - the container it belongs to implies it - and they are
   * 37-46% of the tokens in the corpora that matter.  Only the DOM path uses
   * this; the public token stream is unchanged. */
  bool compact;
  size_t overflow_token;
} tokenizer_t;

/* ------------------------------------------------------------------------- */
/* Errors                                                                     */
/* ------------------------------------------------------------------------- */

/* Line/column are only needed when reporting a failure, so they are computed
 * lazily.  Eager per-byte tracking costs a branch and two increments on every
 * byte of the input, which is a significant fraction of total tokenizer time.
 */
JSOC_INTERNAL jsoc_error jsoc_error_at_offset(const char *input, size_t length,
                                              jsoc_error_code code,
                                              size_t offset) {
  jsoc_error err;
  err.code = code;
  err.offset = offset;
  err.line = 1;
  err.column = 1;

  size_t limit = offset < length ? offset : length;
  for (size_t i = 0; i < limit; i++) {
    if (input[i] == '\n') {
      err.line++;
      err.column = 1;
    } else {
      err.column++;
    }
  }
  return err;
}

static inline jsoc_error make_error_at(const tokenizer_t *t,
                                       jsoc_error_code code, size_t offset) {
  return jsoc_error_at_offset(t->input, t->length, code, offset);
}

static inline jsoc_error make_error(const tokenizer_t *t,
                                    jsoc_error_code code) {
  return make_error_at(t, code, t->pos);
}

static const jsoc_error JSOC_ERR_NONE = {JSOC_OK, 0, 0, 0};

/* Tokens store 32-bit offsets/lengths, so inputs beyond 4 GiB are rejected. */
static inline bool tokens_fit_u32(size_t length) {
  return length <= (size_t)UINT32_MAX;
}

/* ------------------------------------------------------------------------- */
/* Token storage (arena-backed: no malloc, no leak, dies with the arena)      */
/* ------------------------------------------------------------------------- */

static JSOC_NOINLINE jsoc_error tokens_grow(tokenizer_t *t) {
  size_t new_capacity = t->capacity ? t->capacity * 2 : 256;
  jsoc_token *items = jsoc_arena_alloc(
      t->arena, new_capacity * sizeof(jsoc_token), _Alignof(jsoc_token));
  if (JSOC_UNLIKELY(!items))
    return make_error(t, JSOC_ERR_NOMEM);

  if (t->count != 0) {
    memcpy(items, t->tokens, t->count * sizeof(jsoc_token));
  }
  t->tokens = items;
  t->capacity = new_capacity;
  return JSOC_ERR_NONE;
}

static inline jsoc_error push_token(tokenizer_t *t, jsoc_token_type type,
                                    size_t offset, size_t length) {
  /* Compact mode: the separator is still validated by the grammar, it just is
   * not part of the stream the DOM consumes. */
  if (JSOC_UNLIKELY(t->compact &&
                    (type == JSOC_TOK_COMMA || type == JSOC_TOK_COLON))) {
    return JSOC_ERR_NONE;
  }
  if (JSOC_UNLIKELY(t->count == t->capacity)) {
    jsoc_error err = tokens_grow(t);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      return err;
  }
  t->tokens[t->count++] =
      jsoc_token_make(type, (uint32_t)offset, (uint32_t)length);
  return JSOC_ERR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Character classes                                                          */
/* ------------------------------------------------------------------------- */

static inline bool starts_value(unsigned char c) {
  return c == '{' || c == '[' || c == '"' || c == 't' || c == 'f' || c == 'n' ||
         c == '-' || jsoc_is_digit(c);
}

/* ------------------------------------------------------------------------- */
/* Strings                                                                    */
/* ------------------------------------------------------------------------- */

/* `t->pos` must point at the opening quote.  On success the token covers the
 * raw bytes between the quotes (zero-copy: escapes stay as written) and
 * `t->pos` is advanced past the closing quote. */
static jsoc_error parse_string(tokenizer_t *t) {
  const char *in = t->input;
  const size_t len = t->length;
  const size_t start = t->pos;
  size_t i = start + 1;

  for (;;) {
    i = jsoc_find_string_special(in, len, i);
    if (JSOC_UNLIKELY(i >= len)) {
      return make_error_at(t, JSOC_ERR_UNTERMINATED_STRING, len);
    }

    unsigned char c = (unsigned char)in[i];

    if (c == '"') {
      jsoc_error err = push_token(t, JSOC_TOK_STRING, start + 1, i - start - 1);
      if (JSOC_UNLIKELY(err.code != JSOC_OK))
        return err;
      t->pos = i + 1;
      return JSOC_ERR_NONE;
    }

    if (c == '\\') {
      /* A backslash at the very end cannot start an escape. */
      if (JSOC_UNLIKELY(i + 1 >= len)) {
        return make_error_at(t, JSOC_ERR_UNTERMINATED_STRING, len);
      }
      unsigned char esc = (unsigned char)in[i + 1];
      switch (esc) {
      case '"':
      case '\\':
      case '/':
      case 'b':
      case 'f':
      case 'n':
      case 'r':
      case 't':
        i += 2;
        break;
      case 'u':
        if (JSOC_UNLIKELY(i + 6 > len)) {
          return make_error_at(t, JSOC_ERR_INVALID_UNICODE, i);
        }
        for (size_t k = 2; k < 6; k++) {
          if (JSOC_UNLIKELY(!jsoc_is_hex_digit((unsigned char)in[i + k]))) {
            return make_error_at(t, JSOC_ERR_INVALID_UNICODE, i);
          }
        }
        i += 6;
        break;
      default:
        return make_error_at(t, JSOC_ERR_INVALID_ESCAPE, i);
      }
      continue;
    }

    /* Only raw control characters reach this point. */
    return make_error_at(t, JSOC_ERR_INVALID_CONTROL, i);
  }
}

/* ------------------------------------------------------------------------- */
/* Numbers                                                                    */
/* ------------------------------------------------------------------------- */

static jsoc_error_code validate_number_basic(const char *in, size_t len,
                                             size_t start, size_t *end,
                                             size_t *err_off);
static jsoc_error parse_number(tokenizer_t *t) {
  const size_t start = t->pos;
  size_t end = 0;
  size_t err_off = 0;
  jsoc_error_code ec;
  if (t->check_numbers) {
    bool out_of_range = false;
    ec = jsoc_validate_number_checked(t->input, t->length, start, t->arena, &end,
                                 &err_off, &out_of_range);
    if (JSOC_UNLIKELY(ec == JSOC_OK && out_of_range &&
                      t->overflow_token == SIZE_MAX)) {
      t->overflow_token = t->count;
    }
  } else {
    ec = validate_number_basic(t->input, t->length, start, &end, &err_off);
  }
  if (JSOC_UNLIKELY(ec != JSOC_OK)) {
    return make_error_at(t, ec, err_off);
  }

  t->pos = end;
  t->last_scalar_end = end;
  return push_token(t, JSOC_TOK_NUMBER, start, end - start);
}

static jsoc_error parse_literal(tokenizer_t *t, const char *lit, size_t len,
                                jsoc_token_type type) {
  if (JSOC_UNLIKELY(t->pos + len > t->length)) {
    return make_error(t, JSOC_ERR_EXPECTED_VALUE);
  }
  if (JSOC_UNLIKELY(memcmp(t->input + t->pos, lit, len) != 0)) {
    return make_error(t, JSOC_ERR_EXPECTED_VALUE);
  }
  jsoc_error err = push_token(t, type, t->pos, len);
  if (JSOC_UNLIKELY(err.code != JSOC_OK))
    return err;
  t->last_scalar_end = t->pos + len;
  t->pos += len;
  return JSOC_ERR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Tokenizer                                                                  */
/* ------------------------------------------------------------------------- */

static jsoc_result tokenize_scalar(const char *input, size_t length,
                                   jsoc_arena *arena, size_t *overflow_token,
                                   bool stream, bool compact) {
  jsoc_result result = {
      .error = {JSOC_OK, 0, 0, 0}, .tokens = NULL, .count = 0};

  tokenizer_t t = {
      .input = input,
      .length = length,
      .pos = 0,
      .arena = arena,
      .tokens = NULL,
      .count = 0,
      .capacity = 0,
      .depth = 0,
      .after_comma = false,
      .check_numbers = overflow_token != NULL,
      .last_scalar_end = SIZE_MAX,
      .stream = stream,
      .compact = compact,
      .overflow_token = SIZE_MAX,
  };
  if (overflow_token)
    *overflow_token = SIZE_MAX;

  /* Heuristic first allocation; geometric growth handles dense inputs
   * (e.g. "[1,1,1,...]" can approach one token per byte).  The measured
   * average for real JSON is ~3.4 bytes/token, so /3 avoids a grow+copy
   * on typical documents. */
  size_t initial = length / 3 + 32;
  t.tokens = jsoc_arena_alloc(arena, initial * sizeof(jsoc_token),
                              _Alignof(jsoc_token));
  if (JSOC_UNLIKELY(!t.tokens)) {
    result.error = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return result;
  }
  t.capacity = initial;

  jsoc_error err = JSOC_ERR_NONE;

  /* State: expecting a value. */
sv_value:
  t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
  if (JSOC_UNLIKELY(t.pos >= t.length))
    goto at_eof;

  switch ((unsigned char)t.input[t.pos]) {
  case '{':
    if (JSOC_UNLIKELY(t.depth >= JSOC_MAX_DEPTH)) {
      err = make_error(&t, JSOC_ERR_DEPTH_EXCEEDED);
      goto fail;
    }
    err = push_token(&t, JSOC_TOK_OBJ_BEGIN, t.pos, 1);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.stack[t.depth++] = '}';
    t.pos++;
    t.after_comma = false;
    goto sv_key;

  case '[':
    if (JSOC_UNLIKELY(t.depth >= JSOC_MAX_DEPTH)) {
      err = make_error(&t, JSOC_ERR_DEPTH_EXCEEDED);
      goto fail;
    }
    err = push_token(&t, JSOC_TOK_ARR_BEGIN, t.pos, 1);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.stack[t.depth++] = ']';
    t.pos++;
    t.after_comma = false;
    goto sv_value;

  case ']':
    if (JSOC_UNLIKELY(t.depth == 0 || t.stack[t.depth - 1] != ']')) {
      err = make_error(&t, JSOC_ERR_SYNTAX);
      goto fail;
    }
    if (JSOC_UNLIKELY(t.after_comma)) {
      err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
      goto fail;
    }
    err = push_token(&t, JSOC_TOK_ARR_END, t.pos, 1);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.depth--;
    t.pos++;
    goto sv_after;

  case '}':
    if (JSOC_UNLIKELY(t.depth == 0 || t.stack[t.depth - 1] != '}')) {
      err = make_error(&t, JSOC_ERR_SYNTAX);
      goto fail;
    }
    /* A '}' where a value is required, e.g. {"a":}. */
    err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
    goto fail;

  case '"':
    err = parse_string(&t);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.after_comma = false;
    goto sv_after;

  case 't':
    err = parse_literal(&t, "true", 4, JSOC_TOK_TRUE);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.after_comma = false;
    goto sv_after;

  case 'f':
    err = parse_literal(&t, "false", 5, JSOC_TOK_FALSE);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.after_comma = false;
    goto sv_after;

  case 'n':
    err = parse_literal(&t, "null", 4, JSOC_TOK_NULL);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.after_comma = false;
    goto sv_after;

  case ',':
  case ':':
    /* A comma/colon where a value belongs. */
    err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
    goto fail;

  default:
    if (jsoc_is_digit((unsigned char)t.input[t.pos]) || t.input[t.pos] == '-') {
      err = parse_number(&t);
      if (JSOC_UNLIKELY(err.code != JSOC_OK))
        goto fail;
      t.after_comma = false;
      goto sv_after;
    }
    err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
    goto fail;
  }

/* State: inside an object, expecting a key or the closing brace. */
sv_key:
  t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
  if (JSOC_UNLIKELY(t.pos >= t.length))
    goto at_eof;

  if (t.input[t.pos] == '}') {
    if (JSOC_UNLIKELY(t.after_comma)) {
      err = make_error(&t, JSOC_ERR_EXPECTED_KEY);
      goto fail;
    }
    err = push_token(&t, JSOC_TOK_OBJ_END, t.pos, 1);
    if (JSOC_UNLIKELY(err.code != JSOC_OK))
      goto fail;
    t.depth--;
    t.pos++;
    goto sv_after;
  }
  if (JSOC_UNLIKELY(t.input[t.pos] != '"')) {
    err = make_error_at(&t, JSOC_ERR_EXPECTED_KEY, t.pos);
    goto fail;
  }
  err = parse_string(&t);
  if (JSOC_UNLIKELY(err.code != JSOC_OK))
    goto fail;
  t.after_comma = false;
  goto sv_colon;

/* State: after an object key, expecting ':'. */
sv_colon:
  t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
  if (JSOC_UNLIKELY(t.pos >= t.length)) {
    err = make_error_at(&t, JSOC_ERR_UNTERMINATED_OBJECT, t.length);
    goto fail;
  }
  if (JSOC_UNLIKELY(t.input[t.pos] != ':')) {
    err = make_error(&t, JSOC_ERR_EXPECTED_COLON);
    goto fail;
  }
  err = push_token(&t, JSOC_TOK_COLON, t.pos, 1);
  if (JSOC_UNLIKELY(err.code != JSOC_OK))
    goto fail;
  t.pos++;
  goto sv_value;

/* State: after a complete value, expecting ',' or the matching close. */
sv_after:
  t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
  if (JSOC_UNLIKELY(t.pos >= t.length))
    goto at_eof;

  if (JSOC_UNLIKELY(t.depth == 0)) {
    /* A second top-level value is accepted when separated by whitespace
     * (ndjson-style), or in stream mode without whitespace too - but never
     * glued to the previous scalar: `1"x"` and `true1` are junk, matching
     * the fused backend, where `follows` suppresses such a start. */
    bool separated = t.pos > 0 && jsoc_is_ws((unsigned char)t.input[t.pos - 1]);
    bool glued = t.pos == t.last_scalar_end;
    if ((separated || t.stream) && !glued &&
        starts_value((unsigned char)t.input[t.pos])) {
      goto sv_value;
    }
    err = make_error(&t, JSOC_ERR_TRAILING_GARBAGE);
    goto fail;
  }

  {
    unsigned char c = (unsigned char)t.input[t.pos];
    if (c == ',') {
      err = push_token(&t, JSOC_TOK_COMMA, t.pos, 1);
      if (JSOC_UNLIKELY(err.code != JSOC_OK))
        goto fail;
      t.pos++;
      t.after_comma = true;
      if (t.stack[t.depth - 1] == '}')
        goto sv_key;
      goto sv_value;
    }
    if (t.stack[t.depth - 1] == '}' && c == '}') {
      err = push_token(&t, JSOC_TOK_OBJ_END, t.pos, 1);
      if (JSOC_UNLIKELY(err.code != JSOC_OK))
        goto fail;
      t.depth--;
      t.pos++;
      goto sv_after;
    }
    if (t.stack[t.depth - 1] == ']' && c == ']') {
      err = push_token(&t, JSOC_TOK_ARR_END, t.pos, 1);
      if (JSOC_UNLIKELY(err.code != JSOC_OK))
        goto fail;
      t.depth--;
      t.pos++;
      goto sv_after;
    }
    if (c == '}' || c == ']') {
      err = make_error(&t, JSOC_ERR_SYNTAX);
      goto fail;
    }
    err = make_error(&t, JSOC_ERR_EXPECTED_COMMA_OR_END);
    goto fail;
  }

at_eof:
  if (t.depth > 0) {
    jsoc_error_code code = t.stack[t.depth - 1] == '}'
                               ? JSOC_ERR_UNTERMINATED_OBJECT
                               : JSOC_ERR_UNTERMINATED_ARRAY;
    err = make_error_at(&t, code, t.length);
    goto fail;
  }
  if (JSOC_UNLIKELY(t.count == 0)) {
    err = make_error_at(&t, JSOC_ERR_EXPECTED_VALUE, t.length);
    goto fail;
  }

  err = push_token(&t, JSOC_TOK_EOF, t.pos, 0);
  if (JSOC_UNLIKELY(err.code != JSOC_OK))
    goto fail;

  result.tokens = t.tokens;
  result.count = t.count;
  if (overflow_token)
    *overflow_token = t.overflow_token;
  return result;

fail:
  result.error = err;
  result.tokens = t.tokens;
  result.count = t.count;
  if (overflow_token)
    *overflow_token = t.overflow_token;
  return result;
}

JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_impl(const char *input,
                                                    size_t length,
                                                    jsoc_arena *arena) {
  return tokenize_scalar(input, length, arena, NULL, false, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_scalar_stream_impl(const char *input, size_t length,
                                 jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, true, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_scalar_checked_impl(const char *input, size_t length,
                                  jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, false, false);
}

JSOC_INTERNAL jsoc_result jsoc_tokenize_scalar_compact_impl(const char *input,
                                                            size_t length,
                                                            jsoc_arena *arena,
                                                            size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, false, true);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_scalar_stream_compact_impl(const char *input, size_t length,
                                         jsoc_arena *arena,
                                         size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, true, true);
}

/* ------------------------------------------------------------------------- */
/* Fast syntax-only number scanner used by the public tokenizer.
 *
 * A second, checked variant below also records the lexeme's shape for the
 * DOM's representability check.  Keeping this one small lets the fused scanner
 * inline it; the checked variant is only called by the DOM. */
static inline JSOC_ALWAYS_INLINE jsoc_error_code validate_number_basic(
    const char *in, size_t len, size_t start, size_t *end, size_t *err_off) {
  size_t i = start;

  if (in[i] == '-')
    i++;

  if (JSOC_UNLIKELY(i >= len || !jsoc_is_digit((unsigned char)in[i]))) {
    *err_off = i;
    return JSOC_ERR_INVALID_NUMBER;
  }

  if (in[i] == '0') {
    i++;
    /* JSON forbids leading zeroes (01, -012). */
    if (JSOC_UNLIKELY(i < len && jsoc_is_digit((unsigned char)in[i]))) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
  } else {
    i += jsoc_digit_run(in, len, i);
  }

  if (i < len && in[i] == '.') {
    i++;
    const size_t run = jsoc_digit_run(in, len, i);
    if (JSOC_UNLIKELY(run == 0)) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
    i += run;
  }

  if (i < len && (in[i] == 'e' || in[i] == 'E')) {
    i++;
    if (i < len && (in[i] == '+' || in[i] == '-'))
      i++;
    const size_t run = jsoc_digit_run(in, len, i);
    if (JSOC_UNLIKELY(run == 0)) {
      *err_off = i;
      return JSOC_ERR_INVALID_NUMBER;
    }
    i += run;
  }

  *end = i;
  return JSOC_OK;
}

/* ------------------------------------------------------------------------- */
/* SIMD tokenizer: stage 1 classification fused with token emission           */
/* ------------------------------------------------------------------------- */

#if JSOC_SIMD_AVX2

enum {
  JSOC_GS_DOC = 0,      /* start of a document: exactly one value */
  JSOC_GS_VAL_OR_CLOSE, /* after '[': a value or ']' */
  JSOC_GS_VAL,          /* after ':' or ',' in an array: a value only */
  JSOC_GS_KEY_OR_CLOSE, /* after '{': a string key or '}' */
  JSOC_GS_KEY,          /* after ',' in an object: a string key only */
  JSOC_GS_COLON,        /* after a key: ':' */
  JSOC_GS_AFTER,        /* after a value: ',' or the matching close */
};

enum {
  JSOC_PEND_NONE = 0,
  JSOC_PEND_STRING,
};

typedef struct {
  const char *input;
  size_t length;
  jsoc_arena *arena;
  jsoc_token *tokens;
  size_t count;
  size_t capacity;

  uint16_t depth;
  unsigned char stack[JSOC_MAX_DEPTH];
  uint8_t state;

  uint8_t pending;
  size_t pend_open;      /* opening quote offset of a pending string */
  size_t esc_skip_until; /* first byte not consumed by a prior escape */
  bool need_scalar;      /* string validation failed: rescan for errors */
  bool failed;
  jsoc_error err;
  jsoc_stage1_state s1;

  /* Checked mode, mirroring tokenizer_t. */
  bool check_numbers;
  /* Stream mode: adjacent top-level documents are accepted. */
  bool stream;
  /* Compact mode: ',' and ':' are validated but not stored (DOM only). */
  bool compact;
  /* Validate UTF-8 in the same pass as the tokenizer (DOM fast path). */
  bool check_utf8;
  jsoc_utf8_state utf8;
  size_t overflow_token;
} fused_t;

/* Maps a byte to the token it starts, or to 0 for a byte that cannot start a
 * token.  This also rejects the 0x0C/0x1A false positives produced by the
 * nibble-table operator mask: they map to 0 and are handled as invalid scalar
 * starts.
 *
 * The scalar kinds are split so that "is this a literal" is a single test
 * instead of three byte comparisons on the number-heavy path: a number start
 * is JSOC_KIND_NUMBER, and the three literals take the next three values in
 * literal order (true, false, null), so the table also names the literal. */
enum {
  JSOC_KIND_NUMBER = JSOC_TOK_NULL + 1, /* '-' or a digit */
  JSOC_KIND_TRUE = JSOC_KIND_NUMBER + 1,
  JSOC_KIND_FALSE,
  JSOC_KIND_NULL_KIND,
};

static const unsigned char jsoc_token_kind[256] = {
    ['{'] = JSOC_TOK_OBJ_BEGIN, ['}'] = JSOC_TOK_OBJ_END,
    ['['] = JSOC_TOK_ARR_BEGIN, [']'] = JSOC_TOK_ARR_END,
    [','] = JSOC_TOK_COMMA,     [':'] = JSOC_TOK_COLON,
    ['"'] = JSOC_TOK_STRING,    ['t'] = JSOC_KIND_TRUE,
    ['f'] = JSOC_KIND_FALSE,    ['n'] = JSOC_KIND_NULL_KIND,
    ['-'] = JSOC_KIND_NUMBER,   ['0'] = JSOC_KIND_NUMBER,
    ['1'] = JSOC_KIND_NUMBER,   ['2'] = JSOC_KIND_NUMBER,
    ['3'] = JSOC_KIND_NUMBER,   ['4'] = JSOC_KIND_NUMBER,
    ['5'] = JSOC_KIND_NUMBER,   ['6'] = JSOC_KIND_NUMBER,
    ['7'] = JSOC_KIND_NUMBER,   ['8'] = JSOC_KIND_NUMBER,
    ['9'] = JSOC_KIND_NUMBER,
};

/* Failure is recorded inline: the hot loop must not contain calls, otherwise
 * the compiler has to spill the tokenizer state around them on every token.
 * Line and column are filled in once, in the cold failure path. */
static inline JSOC_ALWAYS_INLINE void
fused_fail(fused_t *f, jsoc_error_code code, size_t offset) {
  if (!f->failed) {
    f->failed = true;
    f->err.code = code;
    f->err.offset = offset;
  }
}

static JSOC_NOINLINE bool fused_grow(fused_t *f, size_t need) {
  size_t new_capacity = f->capacity ? f->capacity * 2 : 256;
  if (new_capacity < need)
    new_capacity = need;
  if (JSOC_UNLIKELY(new_capacity > SIZE_MAX / sizeof(jsoc_token))) {
    f->failed = true;
    f->err.code = JSOC_ERR_NOMEM;
    f->err.offset = 0;
    return false;
  }
  jsoc_token *items = jsoc_arena_alloc(
      f->arena, new_capacity * sizeof(jsoc_token), _Alignof(jsoc_token));
  if (JSOC_UNLIKELY(!items)) {
    f->failed = true;
    f->err.code = JSOC_ERR_NOMEM;
    f->err.offset = 0;
    return false;
  }
  if (f->count != 0) {
    memcpy(items, f->tokens, f->count * sizeof(jsoc_token));
  }
  f->tokens = items;
  f->capacity = new_capacity;
  return true;
}

/* Token emission is unchecked: callers reserve room for a whole 64-byte block
 * (at most 65 tokens) before scanning it, which keeps the store loop free of
 * branches and out-of-line calls. */
static inline JSOC_ALWAYS_INLINE void
fused_emit(fused_t *f, jsoc_token_type type, size_t offset, size_t length) {
  f->tokens[f->count++] =
      jsoc_token_make(type, (uint32_t)offset, (uint32_t)length);
}

/* Compact mode: separators are validated by the grammar but never stored,
 * because the container they belong to implies them.  Keeping this check at
 * the three separator sites (instead of in fused_emit) leaves the much more
 * frequent value emits branch-free. */
static inline JSOC_ALWAYS_INLINE void
fused_emit_separator(fused_t *f, jsoc_token_type type, size_t offset) {
  if (!f->compact)
    fused_emit(f, type, offset, 1);
}

/* Handling for the state that follows a complete value.  This is the most
 * frequent structural transition, so it is kept separate and branch-light. */
static inline JSOC_ALWAYS_INLINE int
grammar_after(fused_t *f, jsoc_token_type t, size_t s) {
  if (t == JSOC_TOK_COMMA) {
    if (JSOC_UNLIKELY(f->depth == 0)) {
      fused_fail(f, JSOC_ERR_TRAILING_GARBAGE, s);
      return 1;
    }
    f->state = (f->stack[f->depth - 1] == '}') ? JSOC_GS_KEY : JSOC_GS_VAL;
    return 0;
  }

  if (t == JSOC_TOK_OBJ_END || t == JSOC_TOK_ARR_END) {
    if (JSOC_UNLIKELY(f->depth == 0)) {
      fused_fail(f, JSOC_ERR_TRAILING_GARBAGE, s);
      return 1;
    }
    const unsigned char want = (t == JSOC_TOK_OBJ_END) ? '}' : ']';
    if (JSOC_UNLIKELY(f->stack[f->depth - 1] != want)) {
      fused_fail(f, JSOC_ERR_SYNTAX, s);
      return 1;
    }
    f->depth--;
    return 0;
  }

  /* A value, ':' or opening bracket after a value: legal at the top level
   * only when whitespace separated (ndjson-style streams), unless the caller
   * is reading a stream, where adjacent documents are legal. */
  if (f->depth == 0) {
    const bool separated = s > 0 && jsoc_is_ws((unsigned char)f->input[s - 1]);
    if (t != JSOC_TOK_COLON && (separated || f->stream)) {
      f->state = JSOC_GS_DOC;
      return 2;
    }
    fused_fail(f, JSOC_ERR_TRAILING_GARBAGE, s);
    return 1;
  }
  fused_fail(f, JSOC_ERR_EXPECTED_COMMA_OR_END, s);
  return 1;
}

/* Grammar step.  Returns 0 on success, 1 on failure (f->err is set), and 2
 * when a top-level value follows whitespace and must be re-dispatched as a
 * new document (ndjson-style streams).
 *
 * The fast cases are ordered by frequency and produce constant next states,
 * so the compiler never has to load a transition from a table.  Token types
 * 7..11 (STRING..NULL) are exactly the "value" range. */
static inline JSOC_ALWAYS_INLINE int grammar_step(fused_t *f, jsoc_token_type t,
                                                  size_t s) {
  const uint8_t st = f->state;

  if (JSOC_UNLIKELY(st == JSOC_GS_AFTER)) {
    return grammar_after(f, t, s);
  }

  if (JSOC_LIKELY(t >= JSOC_TOK_STRING)) {
    if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
      return 1;
    }
    if (JSOC_UNLIKELY(st >= JSOC_GS_KEY_OR_CLOSE)) {
      /* In a key slot only a string is a key; numbers and literals are
       * values and therefore invalid here. */
      if (JSOC_UNLIKELY(t != JSOC_TOK_STRING)) {
        fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
        return 1;
      }
      f->state = JSOC_GS_COLON;
    } else {
      f->state = JSOC_GS_AFTER;
    }
    return 0;
  }

  switch (t) {
  case JSOC_TOK_OBJ_BEGIN:
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY_OR_CLOSE || st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
      return 1;
    }
    if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
      return 1;
    }
    if (JSOC_UNLIKELY(f->depth >= JSOC_MAX_DEPTH)) {
      fused_fail(f, JSOC_ERR_DEPTH_EXCEEDED, s);
      return 1;
    }
    f->stack[f->depth++] = '}';
    f->state = JSOC_GS_KEY_OR_CLOSE;
    return 0;

  case JSOC_TOK_ARR_BEGIN:
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY_OR_CLOSE || st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
      return 1;
    }
    if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
      return 1;
    }
    if (JSOC_UNLIKELY(f->depth >= JSOC_MAX_DEPTH)) {
      fused_fail(f, JSOC_ERR_DEPTH_EXCEEDED, s);
      return 1;
    }
    f->stack[f->depth++] = ']';
    f->state = JSOC_GS_VAL_OR_CLOSE;
    return 0;

  case JSOC_TOK_OBJ_END:
    if (st == JSOC_GS_KEY_OR_CLOSE) {
      f->depth--;
      f->state = JSOC_GS_AFTER;
      return 0;
    }
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
    } else if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
    } else if (st == JSOC_GS_VAL && f->depth > 0 &&
               f->stack[f->depth - 1] == '}') {
      fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    } else {
      fused_fail(f, JSOC_ERR_SYNTAX, s);
    }
    return 1;

  case JSOC_TOK_ARR_END:
    if (st == JSOC_GS_VAL_OR_CLOSE) {
      f->depth--;
      f->state = JSOC_GS_AFTER;
      return 0;
    }
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY_OR_CLOSE || st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
    } else if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
    } else if (st == JSOC_GS_VAL && f->depth > 0 &&
               f->stack[f->depth - 1] == ']') {
      fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    } else {
      fused_fail(f, JSOC_ERR_SYNTAX, s);
    }
    return 1;

  case JSOC_TOK_COMMA:
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY_OR_CLOSE || st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
    } else if (JSOC_UNLIKELY(st == JSOC_GS_COLON)) {
      fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
    } else {
      fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    }
    return 1;

  case JSOC_TOK_COLON:
    if (JSOC_LIKELY(st == JSOC_GS_COLON)) {
      f->state = JSOC_GS_VAL;
      return 0;
    }
    if (JSOC_UNLIKELY(st == JSOC_GS_KEY_OR_CLOSE || st == JSOC_GS_KEY)) {
      fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
    } else {
      fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    }
    return 1;

  default:
    fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    return 1;
  }
}

/* Validate the escape and control bytes inside strings in this block directly
 * from the stage-1 masks, so clean string bytes are never rescanned.  Any
 * invalid byte switches to the reference scanner, which reports the exact
 * error code, offset and partial token stream. */
static JSOC_NOINLINE bool
fused_check_specials(fused_t *f, size_t base, uint64_t backslash,
                     uint64_t control, uint64_t in_string) {
  if (JSOC_UNLIKELY((control & in_string) != 0)) {
    f->need_scalar = true;
    f->failed = true;
    return false;
  }

  uint64_t bs = backslash & in_string;
  while (bs != 0) {
    const unsigned b = (unsigned)__builtin_ctzll(bs);
    bs &= bs - 1;
    const size_t g = base + b;
    /* A backslash consumed by an escape that started earlier in this or a
     * previous block is not itself an escape introducer. */
    if (g < f->esc_skip_until)
      continue;
    const size_t e = g + 1;
    if (JSOC_UNLIKELY(e >= f->length)) {
      f->need_scalar = true;
      f->failed = true;
      return false;
    }
    const unsigned char c = (unsigned char)f->input[e];
    if (c == 'u') {
      if (JSOC_UNLIKELY(g + 6 > f->length)) {
        f->need_scalar = true;
        f->failed = true;
        return false;
      }
      for (size_t k = 2; k < 6; k++) {
        if (JSOC_UNLIKELY(!jsoc_is_hex_digit((unsigned char)f->input[g + k]))) {
          f->need_scalar = true;
          f->failed = true;
          return false;
        }
      }
      f->esc_skip_until = g + 6;
    } else if (c == '"' || c == '\\' || c == '/' || c == 'b' || c == 'f' ||
               c == 'n' || c == 'r' || c == 't') {
      f->esc_skip_until = g + 2;
    } else {
      f->need_scalar = true;
      f->failed = true;
      return false;
    }
  }
  return true;
}

/* A scalar ended before `p`, but `p` is not the next token start.  Mirror the
 * portable path's taxonomy: junk at the top level is trailing garbage, junk
 * inside a container means a separator is missing. */
static inline JSOC_ALWAYS_INLINE bool fused_scalar_junk(fused_t *f, size_t p) {
  fused_fail(f,
             f->depth == 0 ? JSOC_ERR_TRAILING_GARBAGE
                           : JSOC_ERR_EXPECTED_COMMA_OR_END,
             p);
  return false;
}

/* After a complete value the next structural byte is very often ','.  Consume
 * it in the same iteration: this saves a trip through the outer bit loop and
 * a grammar call for every element of a non-empty container.
 * Returns 1 if a comma was consumed, 0 if the next start is not a comma, and
 * -1 on a grammar error. */
static inline JSOC_ALWAYS_INLINE int
fused_try_comma(fused_t *f, uint64_t *rest_starts, size_t base) {
  if (*rest_starts == 0)
    return 0;
  const unsigned b = (unsigned)__builtin_ctzll(*rest_starts);
  if (f->input[base + b] != ',')
    return 0;
  *rest_starts &= *rest_starts - 1;
  if (JSOC_UNLIKELY(grammar_after(f, JSOC_TOK_COMMA, base + b) != 0))
    return -1;
  fused_emit_separator(f, JSOC_TOK_COMMA, base + b);
  return 1;
}

/* Consume a ':' that directly follows a key string in the same block.
 * Returns true if it did. */
static inline JSOC_ALWAYS_INLINE bool
fused_try_colon(fused_t *f, uint64_t *rest_starts, size_t base) {
  if (*rest_starts == 0)
    return false;
  const unsigned b = (unsigned)__builtin_ctzll(*rest_starts);
  if (f->input[base + b] != ':')
    return false;
  *rest_starts &= *rest_starts - 1;
  f->state = JSOC_GS_VAL;
  fused_emit_separator(f, JSOC_TOK_COLON, base + b);
  return true;
}

static inline JSOC_ALWAYS_INLINE bool fused_take_start(uint64_t *rest_starts,
                                                       size_t base, size_t *s) {
  if (*rest_starts == 0)
    return false;
  *s = base + (unsigned)__builtin_ctzll(*rest_starts);
  *rest_starts &= *rest_starts - 1;
  return true;
}

enum {
  FUSED_DONE = 0,
  FUSED_CONTINUE = 1,
  FUSED_FAILED = -1,
};

/* A byte that may legally terminate a scalar.  Anything else glued to a
 * number or literal is junk (e.g. `123abc`), which the portable path reports
 * at the first byte after the scalar. */
static inline JSOC_ALWAYS_INLINE bool fused_is_scalar_end(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
         c == '}' || c == ']';
}

/* A scalar (number or literal) ending at `end` was just emitted.  The byte at
 * `end` must be a delimiter; the next structural start (already known from the
 * block mask) is then handed back to the caller. */
static inline JSOC_ALWAYS_INLINE int fused_scalar_done(fused_t *f, size_t end,
                                                       uint64_t *rest_starts,
                                                       size_t base, size_t *s) {
  if (JSOC_LIKELY(end < f->length)) {
    if (JSOC_UNLIKELY(!fused_is_scalar_end((unsigned char)f->input[end]))) {
      fused_scalar_junk(f, end);
      return FUSED_FAILED;
    }
  }

  if (*rest_starts != 0) {
    const unsigned b = (unsigned)__builtin_ctzll(*rest_starts);
    *rest_starts &= *rest_starts - 1;
    /* A comma is the overwhelmingly common separator after a scalar.
     * Emit it here, then continue with the following start. */
    if (f->input[base + b] == ',') {
      if (JSOC_UNLIKELY(grammar_after(f, JSOC_TOK_COMMA, base + b) != 0))
        return FUSED_FAILED;
      fused_emit_separator(f, JSOC_TOK_COMMA, base + b);
      if (!fused_take_start(rest_starts, base, s))
        return FUSED_DONE;
    } else {
      *s = base + b;
    }
    return FUSED_CONTINUE;
  }

  return FUSED_DONE;
}

static inline JSOC_ALWAYS_INLINE bool fused_process_start(fused_t *f, size_t s,
                                                          uint64_t *rest_starts,
                                                          uint64_t quote,
                                                          size_t base) {
  if (f->pending == JSOC_PEND_STRING) {
    const size_t content_start = f->pend_open + 1;
    const size_t content_len = s - content_start;
    fused_emit(f, JSOC_TOK_STRING, content_start, content_len);
    f->pending = JSOC_PEND_NONE;
    if (f->state == JSOC_GS_COLON) {
      if (!fused_try_colon(f, rest_starts, base))
        return true;
    } else if (f->state == JSOC_GS_AFTER) {
      const int r = fused_try_comma(f, rest_starts, base);
      if (JSOC_UNLIKELY(r < 0))
        return false;
      if (r == 0)
        return true;
    } else {
      return true;
    }
    if (!fused_take_start(rest_starts, base, &s))
      return true;
    goto again;
  }

  unsigned char c;
  unsigned char k;
  int g;

again:
  c = (unsigned char)f->input[s];
  k = jsoc_token_kind[c];

  if (k == JSOC_TOK_STRING) {
    g = grammar_step(f, JSOC_TOK_STRING, s);
    if (g == 2)
      goto again;
    if (g != 0)
      return false;

    /* Strings are very short, so the closing quote is usually still in
     * this block.  In that case consume it here (it is necessarily the
     * next start: string contents are masked out of `starts`) instead of
     * paying another loop iteration for it. */
    if (*rest_starts != 0) {
      const unsigned b2 = (unsigned)__builtin_ctzll(*rest_starts);
      if (((quote >> b2) & 1u) != 0) {
        *rest_starts &= *rest_starts - 1;
        const size_t close = base + b2;
        const size_t content_start = s + 1;
        fused_emit(f, JSOC_TOK_STRING, content_start, close - content_start);
        if (f->state == JSOC_GS_COLON) {
          if (!fused_try_colon(f, rest_starts, base))
            return true;
        } else if (f->state == JSOC_GS_AFTER) {
          const int r = fused_try_comma(f, rest_starts, base);
          if (JSOC_UNLIKELY(r < 0))
            return false;
          if (r == 0)
            return true;
        } else {
          return true;
        }
        if (!fused_take_start(rest_starts, base, &s))
          return true;
        goto again;
      }
    }

    f->pending = JSOC_PEND_STRING;
    f->pend_open = s;
    return true;
  }

  if (k >= JSOC_KIND_NUMBER) {
    g = grammar_step(f, JSOC_TOK_NUMBER, s);
    if (g == 2)
      goto again;
    if (g != 0)
      return false;

    if (k != JSOC_KIND_NUMBER) {
      const size_t litlen = (k == JSOC_KIND_FALSE) ? 5 : 4;
      const char *lit = (k == JSOC_KIND_TRUE)
                            ? "true"
                            : ((k == JSOC_KIND_FALSE) ? "false" : "null");
      /* Compare inline: a memcmp() call per literal costs more than the
       * four/five byte loads and would force spills around it. */
      bool ok = s + litlen <= f->length;
      if (ok) {
        const char *q = f->input + s;
        ok = q[1] == lit[1] && q[2] == lit[2] && q[3] == lit[3];
        if (litlen == 5)
          ok = ok && q[4] == 'e';
      }
      if (JSOC_UNLIKELY(!ok)) {
        fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
        return false;
      }
      fused_emit(f,
                 (c == 't')   ? JSOC_TOK_TRUE
                 : (c == 'f') ? JSOC_TOK_FALSE
                              : JSOC_TOK_NULL,
                 s, litlen);
      {
        const int st = fused_scalar_done(f, s + litlen, rest_starts, base, &s);
        if (JSOC_UNLIKELY(st == FUSED_FAILED))
          return false;
        if (st == FUSED_CONTINUE)
          goto again;
        return true;
      }
    }

    size_t end = 0;
    size_t err_off = 0;
    jsoc_error_code ec;
    if (f->check_numbers) {
      bool out_of_range = false;
      ec = jsoc_validate_number_checked(f->input, f->length, s, f->arena, &end,
                                   &err_off, &out_of_range);
      if (JSOC_UNLIKELY(ec == JSOC_OK && out_of_range &&
                        f->overflow_token == SIZE_MAX)) {
        f->overflow_token = f->count;
      }
    } else {
      ec = validate_number_basic(f->input, f->length, s, &end, &err_off);
    }
    if (JSOC_UNLIKELY(ec != JSOC_OK)) {
      fused_fail(f, ec, err_off);
      return false;
    }
    fused_emit(f, JSOC_TOK_NUMBER, s, end - s);
    {
      const int st = fused_scalar_done(f, end, rest_starts, base, &s);
      if (JSOC_UNLIKELY(st == FUSED_FAILED))
        return false;
      if (st == FUSED_CONTINUE)
        goto again;
      return true;
    }
  }

  if (k != 0) {
    g = grammar_step(f, (jsoc_token_type)k, s);
    if (g == 2)
      goto again;
    if (g != 0)
      return false;
    if (f->compact && (k == JSOC_TOK_COMMA || k == JSOC_TOK_COLON)) {
      /* Validated, deliberately not stored (see fused_emit_separator). */
    } else {
      fused_emit(f, (jsoc_token_type)k, s, 1);
    }
    if (f->state == JSOC_GS_AFTER) {
      const int r = fused_try_comma(f, rest_starts, base);
      if (JSOC_UNLIKELY(r < 0))
        return false;
      if (r == 0)
        return true;
    }
    if (!fused_take_start(rest_starts, base, &s))
      return true;
    goto again;
  }

  /* Reaching here means the byte starts a number/literal, a structural
   * token, or nothing at all.  The first two cases were handled above, so
   * this is a byte that cannot start a JSON value.  Classify it by the
   * grammar slot to keep the exact error taxonomy of the portable path:
   * EXPECTED_VALUE in a value slot, EXPECTED_KEY in a key slot, and
   * EXPECTED_COMMA_OR_END/TRAILING_GARBAGE after a value. */
  switch (f->state) {
  case JSOC_GS_DOC:
  case JSOC_GS_VAL:
  case JSOC_GS_VAL_OR_CLOSE:
    fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    break;
  case JSOC_GS_KEY:
  case JSOC_GS_KEY_OR_CLOSE:
    fused_fail(f, JSOC_ERR_EXPECTED_KEY, s);
    break;
  case JSOC_GS_COLON:
    fused_fail(f, JSOC_ERR_EXPECTED_COLON, s);
    break;
  case JSOC_GS_AFTER:
    if (f->depth == 0) {
      fused_fail(f, JSOC_ERR_TRAILING_GARBAGE, s);
    } else {
      fused_fail(f, JSOC_ERR_EXPECTED_COMMA_OR_END, s);
    }
    break;
  default:
    fused_fail(f, JSOC_ERR_EXPECTED_VALUE, s);
    break;
  }
  return false;
}

static inline void fused_process_block(fused_t *f, const uint8_t *block,
                                       size_t base, uint64_t valid_mask) {
  const jsoc_block_masks m = jsoc_classify64(block);

  if (JSOC_UNLIKELY(f->check_utf8) &&
      JSOC_LIKELY((m.quote | f->s1.prev_in_string) != 0)) {
    /* On the success path every non-ASCII byte is inside a string, so blocks
     * with no string activity need no UTF-8 work.  Invalid UTF-8 elsewhere
     * makes the tokenizer fail, and that path re-validates the whole input. */
    if (JSOC_LIKELY(m.nonascii == 0)) {
      /* Both 32-byte chunks are ASCII.  This terminates any pending
       * multibyte sequence (which is an error) and resets the carry state.
       * Zero is a valid stand-in for the previous ASCII bytes in the next
       * block's checks. */
      f->utf8.error = _mm256_or_si256(f->utf8.error, f->utf8.prev_incomplete);
      f->utf8.prev_incomplete = _mm256_setzero_si256();
      f->utf8.prev_input = _mm256_setzero_si256();
    } else {
      jsoc_utf8_check_chunk(
          &f->utf8,
          _mm256_loadu_si256((const __m256i *)(const void *)block));
      jsoc_utf8_check_chunk(
          &f->utf8,
          _mm256_loadu_si256((const __m256i *)(const void *)(block + 32)));
    }
  }

  uint64_t quote;
  uint64_t in_string;
  if (JSOC_LIKELY(m.quote == 0 && f->s1.prev_in_string == 0)) {
    /* No string activity in this block (or carried into it).  Skipping
     * the prefix XOR removes a pclmul from every string-free block. */
    (void)jsoc_escape_next(m.backslash, &f->s1.next_is_escaped);
    quote = 0;
    in_string = 0;
  } else {
    const uint64_t escaped =
        jsoc_escape_next(m.backslash, &f->s1.next_is_escaped);
    quote = m.quote & ~escaped;
    in_string = jsoc_prefix_xor(quote) ^ f->s1.prev_in_string;
    f->s1.prev_in_string = (uint64_t)((int64_t)in_string >> 63);

    /* Backslashes and raw control bytes are only special inside a string;
     * elsewhere the grammar rejects them as invalid value starts. */
    if (JSOC_UNLIKELY(((m.backslash | m.control) & in_string & valid_mask) !=
                      0)) {
      if (!fused_check_specials(f, base, m.backslash & valid_mask,
                                m.control & valid_mask, in_string)) {
        return;
      }
    }
  }

  const uint64_t string_tail = in_string ^ quote;
  const uint64_t scalar = ~(m.op | m.whitespace);
  const uint64_t nonquote_scalar = scalar & ~quote;
  const uint64_t follows = (nonquote_scalar << 1) | f->s1.prev_scalar;
  f->s1.prev_scalar = nonquote_scalar >> 63;

  uint64_t starts =
      ((m.op | (scalar & ~follows)) & ~string_tail) | (quote & ~in_string);
  starts &= valid_mask;

  if (JSOC_UNLIKELY(starts == 0))
    return;

  /* A block can emit at most one token per start plus a pending scalar
   * carried over from the previous block, so 65 slots is a hard bound. */
  if (JSOC_UNLIKELY(f->capacity - f->count < 65)) {
    if (!fused_grow(f, f->count + 65))
      return;
  }

  while (starts != 0) {
    const unsigned bit = (unsigned)__builtin_ctzll(starts);
    starts &= starts - 1;
    if (JSOC_UNLIKELY(
            !fused_process_start(f, base + bit, &starts, quote, base))) {
      return;
    }
  }
}

/* ------------------------------------------------------------------------- */
/* DOM fast path: structural index stream                                     */
/* ------------------------------------------------------------------------- */

/* Stage 1 for the split DOM path.  This is the same block classification and
 * UTF-8/escape validation as the fused tokenizer, but instead of running the
 * grammar and materialising 12-byte tokens it appends one 4-byte index per
 * token start.  Grammar, scalar validation and DOM construction happen in
 * stage 2.  Any irregularity makes stage 1 return false so the caller can
 * replay the existing tokenizer and preserve its exact errors. */

typedef struct {
  jsoc_stage1_state s1;
  jsoc_utf8_state utf8;
  size_t esc_skip_until;
} stage1_t;

/* Length of a JSON escape sequence in bytes, or 0 when the byte after a
 * backslash is not a legal escape. */
static const unsigned char jsoc_escape_len[256] = {
    ['"'] = 2,  ['\\'] = 2, ['/'] = 2,  ['b'] = 2,  ['f'] = 2,
    ['n'] = 2,  ['r'] = 2,  ['t'] = 2,  ['u'] = 6,
};

/* Nibble classifiers for the escape fast path.  A byte belongs to a class iff
 * `class_lut[low_nibble] & (1 << high_nibble)` is non-zero.  High nibbles
 * above 7 map to zero, so non-ASCII bytes are never accepted. */
static const unsigned char jsoc_escape_nibble[16] = {
    [2] = 0xC4, [4] = 0x80, [5] = 0x80, [6] = 0x40,
    [12] = 0x20, [14] = 0x40, [15] = 0x04,
};
static const unsigned char jsoc_hex_nibble[16] = {
    [0] = 0x08, [1] = 0x58, [2] = 0x58, [3] = 0x58, [4] = 0x58,
    [5] = 0x58, [6] = 0x58, [7] = 0x08, [8] = 0x08, [9] = 0x08,
};
static const unsigned char jsoc_high_bit[16] = {
    1, 2, 4, 8, 16, 32, 64, 128, 0, 0, 0, 0, 0, 0, 0, 0,
};

static JSOC_NOINLINE bool stage1_check_specials(const char *input,
                                                size_t length,
                                                const uint8_t *block,
                                                size_t base,
                                                uint64_t backslash,
                                                uint64_t control,
                                                uint64_t in_string) {
  if (JSOC_UNLIKELY((control & in_string) != 0))
    return false;

  /* `backslash` already excludes chars escaped by an odd backslash run, so
   * every set bit introduces an escape. */
  uint64_t bs = backslash & in_string;
  if (bs == 0)
    return true;

  const size_t rem = (length - base < 64) ? length - base : 64;
  if (rem < 64)
    bs &= ((uint64_t)1 << rem) - 1;

  /* Sparse escapes are cheaper with the serial loop: the vector classifier
   * costs about as much as three or four scalar escapes, and most documents
   * have at most one or two per block.  Dense escape blocks (synthetic
   * `\uXXXX` corpora) go through the vector path below. */
  if (JSOC_LIKELY(__builtin_popcountll(bs) <= 2) || rem < 6) {
    while (bs != 0) {
      const unsigned b = (unsigned)__builtin_ctzll(bs);
      bs &= bs - 1;
      const size_t g = base + b;
      const size_t e = g + 1;
      if (JSOC_UNLIKELY(e >= length))
        return false;
      const unsigned char c = (unsigned char)input[e];
      const unsigned len = jsoc_escape_len[c];
      if (JSOC_UNLIKELY(len == 0))
        return false;
      if (len == 6) {
        if (JSOC_UNLIKELY(g + 6 > length))
          return false;
        for (size_t k = 2; k < 6; k++) {
          if (JSOC_UNLIKELY(!jsoc_is_hex_digit((unsigned char)input[g + k])))
            return false;
        }
      }
    }
    return true;
  }

  /* Classify all 64 bytes once.  The escape path is hot only on synthetically
   * escaped documents; once it is entered, doing every per-byte test with
   * three shuffles is much cheaper than a serial bit loop, especially for
   * `\uXXXX`, which has four hex digits to check per escape. */
  const __m256i nib = _mm256_set1_epi8(0x0F);
  const __m256i zero = _mm256_setzero_si256();
  const __m256i uchr = _mm256_set1_epi8('u');
  const __m256i esc_lut = _mm256_broadcastsi128_si256(
      _mm_loadu_si128((const __m128i *)(const void *)jsoc_escape_nibble));
  const __m256i hex_lut = _mm256_broadcastsi128_si256(
      _mm_loadu_si128((const __m128i *)(const void *)jsoc_hex_nibble));
  const __m256i high_bit = _mm256_broadcastsi128_si256(
      _mm_loadu_si128((const __m128i *)(const void *)jsoc_high_bit));

  uint64_t u_at = 0;
  uint64_t hex = 0;
  uint64_t esc = 0;
  for (int half = 0; half < 2; half++) {
    const __m256i v = _mm256_loadu_si256(
        (const __m256i *)(const void *)(block + (size_t)half * 32));
    const __m256i lo = _mm256_and_si256(v, nib);
    const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), nib);
    const __m256i bits = _mm256_shuffle_epi8(high_bit, hi);
    const uint32_t bad_esc = (uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_and_si256(_mm256_shuffle_epi8(esc_lut, lo),
                                           bits),
                          zero));
    const uint32_t bad_hex = (uint32_t)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_and_si256(_mm256_shuffle_epi8(hex_lut, lo),
                                           bits),
                          zero));
    const uint32_t is_u =
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, uchr));
    esc |= (uint64_t)(~bad_esc) << (half * 32);
    hex |= (uint64_t)(~bad_hex) << (half * 32);
    u_at |= (uint64_t)is_u << (half * 32);
  }

  /* `safe2`: the next byte is inside this block.  `safe6`: the four bytes
   * after `\u` are inside this block too. */
  const uint64_t safe2 = ((uint64_t)1 << (rem - 1)) - 1;
  const uint64_t safe6 =
      (rem >= 6) ? (((uint64_t)1 << (rem - 5)) - 1) : 0;

  /* Plain escapes: the byte after the backslash must be a legal escape. */
  const uint64_t us = bs & (u_at >> 1);
  const uint64_t other = (bs & ~us) & safe2;
  if (JSOC_UNLIKELY((other & ~(esc >> 1)) != 0))
    return false;

  /* `\u` escapes: all four following bytes must be hex digits.  Shifting the
   * mask makes this a single AND per escape-free block. */
  const uint64_t hex4 = hex & (hex >> 1) & (hex >> 2) & (hex >> 3);
  if (JSOC_UNLIKELY(((us & safe6) & ~(hex4 >> 2)) != 0))
    return false;

  /* Escapes too close to the block edge (and any position where the following
   * bytes are not visible) go through the authoritative scalar check. */
  uint64_t rest = (bs & ~safe2) | (us & ~safe6 & safe2);
  while (rest != 0) {
    const unsigned b = (unsigned)__builtin_ctzll(rest);
    rest &= rest - 1;
    const size_t g = base + b;
    const size_t e = g + 1;
    if (JSOC_UNLIKELY(e >= length))
      return false;
    const unsigned char c = (unsigned char)input[e];
    const unsigned len = jsoc_escape_len[c];
    if (JSOC_UNLIKELY(len == 0))
      return false;
    if (len == 6) {
      if (JSOC_UNLIKELY(g + 6 > length))
        return false;
      for (size_t k = 2; k < 6; k++) {
        if (JSOC_UNLIKELY(!jsoc_is_hex_digit((unsigned char)input[g + k])))
          return false;
      }
    }
  }
  return true;
}

static inline JSOC_ALWAYS_INLINE bool
stage1_compute_block(stage1_t *st, const char *input, size_t length,
                     const uint8_t *block, size_t base, uint64_t valid_mask,
                     const bool branchless, uint64_t *out_starts) {
  const __m256i lo = _mm256_loadu_si256((const __m256i *)(const void *)block);
  const __m256i hi =
      _mm256_loadu_si256((const __m256i *)(const void *)(block + 32));
  const jsoc_block_masks m = jsoc_classify64_vec(lo, hi);

#ifndef JSOC_EXP_S1_NO_UTF8
  if (JSOC_LIKELY((m.quote | st->s1.prev_in_string) != 0)) {
    if (JSOC_LIKELY(m.nonascii == 0)) {
      st->utf8.error =
          _mm256_or_si256(st->utf8.error, st->utf8.prev_incomplete);
      st->utf8.prev_incomplete = _mm256_setzero_si256();
      st->utf8.prev_input = _mm256_setzero_si256();
    } else {
      jsoc_utf8_check_chunk(&st->utf8, lo);
      jsoc_utf8_check_chunk(&st->utf8, hi);
    }
  }
#endif

  uint64_t quote;
  uint64_t in_string;
  if (!branchless &&
      JSOC_LIKELY(m.quote == 0 && st->s1.prev_in_string == 0)) {
    /* No string activity in this block (or carried into it): skip the
     * prefix XOR.  Wordy inputs take the branch-free path instead. */
    (void)jsoc_escape_next(m.backslash, &st->s1.next_is_escaped);
    quote = 0;
    in_string = 0;
  } else {
    const uint64_t escaped =
        jsoc_escape_next(m.backslash, &st->s1.next_is_escaped);
    quote = m.quote & ~escaped;
    in_string = jsoc_prefix_xor(quote) ^ st->s1.prev_in_string;
    st->s1.prev_in_string = (uint64_t)((int64_t)in_string >> 63);

    if (JSOC_UNLIKELY(((m.backslash | m.control) & in_string & valid_mask) !=
                      0)) {
#ifndef JSOC_EXP_S1_NO_SPECIALS
      if (!stage1_check_specials(input, length, block, base,
                                 m.backslash & ~escaped & valid_mask,
                                 m.control & valid_mask, in_string)) {
        return false;
      }
#endif
    }
  }

  const uint64_t string_tail = in_string ^ quote;
  const uint64_t scalar = ~(m.op | m.whitespace);
  const uint64_t nonquote_scalar = scalar & ~quote;
  const uint64_t follows = (nonquote_scalar << 1) | st->s1.prev_scalar;
  st->s1.prev_scalar = nonquote_scalar >> 63;

  uint64_t starts =
      ((m.op | (scalar & ~follows)) & ~string_tail) | (quote & ~in_string);
  *out_starts = starts & valid_mask;
  return true;
}

/* Positions of the set bits of every byte value, padded to eight slots.
 * One 8-byte load plus vpmovzxbd turns a mask byte into the eight offsets
 * to store, which is far cheaper than a serial ctz/blsr chain per index. */
static const unsigned char stage1_pos_table[256][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0},
    {2, 0, 0, 0, 0, 0, 0, 0}, {0, 2, 0, 0, 0, 0, 0, 0}, {1, 2, 0, 0, 0, 0, 0, 0}, {0, 1, 2, 0, 0, 0, 0, 0},
    {3, 0, 0, 0, 0, 0, 0, 0}, {0, 3, 0, 0, 0, 0, 0, 0}, {1, 3, 0, 0, 0, 0, 0, 0}, {0, 1, 3, 0, 0, 0, 0, 0},
    {2, 3, 0, 0, 0, 0, 0, 0}, {0, 2, 3, 0, 0, 0, 0, 0}, {1, 2, 3, 0, 0, 0, 0, 0}, {0, 1, 2, 3, 0, 0, 0, 0},
    {4, 0, 0, 0, 0, 0, 0, 0}, {0, 4, 0, 0, 0, 0, 0, 0}, {1, 4, 0, 0, 0, 0, 0, 0}, {0, 1, 4, 0, 0, 0, 0, 0},
    {2, 4, 0, 0, 0, 0, 0, 0}, {0, 2, 4, 0, 0, 0, 0, 0}, {1, 2, 4, 0, 0, 0, 0, 0}, {0, 1, 2, 4, 0, 0, 0, 0},
    {3, 4, 0, 0, 0, 0, 0, 0}, {0, 3, 4, 0, 0, 0, 0, 0}, {1, 3, 4, 0, 0, 0, 0, 0}, {0, 1, 3, 4, 0, 0, 0, 0},
    {2, 3, 4, 0, 0, 0, 0, 0}, {0, 2, 3, 4, 0, 0, 0, 0}, {1, 2, 3, 4, 0, 0, 0, 0}, {0, 1, 2, 3, 4, 0, 0, 0},
    {5, 0, 0, 0, 0, 0, 0, 0}, {0, 5, 0, 0, 0, 0, 0, 0}, {1, 5, 0, 0, 0, 0, 0, 0}, {0, 1, 5, 0, 0, 0, 0, 0},
    {2, 5, 0, 0, 0, 0, 0, 0}, {0, 2, 5, 0, 0, 0, 0, 0}, {1, 2, 5, 0, 0, 0, 0, 0}, {0, 1, 2, 5, 0, 0, 0, 0},
    {3, 5, 0, 0, 0, 0, 0, 0}, {0, 3, 5, 0, 0, 0, 0, 0}, {1, 3, 5, 0, 0, 0, 0, 0}, {0, 1, 3, 5, 0, 0, 0, 0},
    {2, 3, 5, 0, 0, 0, 0, 0}, {0, 2, 3, 5, 0, 0, 0, 0}, {1, 2, 3, 5, 0, 0, 0, 0}, {0, 1, 2, 3, 5, 0, 0, 0},
    {4, 5, 0, 0, 0, 0, 0, 0}, {0, 4, 5, 0, 0, 0, 0, 0}, {1, 4, 5, 0, 0, 0, 0, 0}, {0, 1, 4, 5, 0, 0, 0, 0},
    {2, 4, 5, 0, 0, 0, 0, 0}, {0, 2, 4, 5, 0, 0, 0, 0}, {1, 2, 4, 5, 0, 0, 0, 0}, {0, 1, 2, 4, 5, 0, 0, 0},
    {3, 4, 5, 0, 0, 0, 0, 0}, {0, 3, 4, 5, 0, 0, 0, 0}, {1, 3, 4, 5, 0, 0, 0, 0}, {0, 1, 3, 4, 5, 0, 0, 0},
    {2, 3, 4, 5, 0, 0, 0, 0}, {0, 2, 3, 4, 5, 0, 0, 0}, {1, 2, 3, 4, 5, 0, 0, 0}, {0, 1, 2, 3, 4, 5, 0, 0},
    {6, 0, 0, 0, 0, 0, 0, 0}, {0, 6, 0, 0, 0, 0, 0, 0}, {1, 6, 0, 0, 0, 0, 0, 0}, {0, 1, 6, 0, 0, 0, 0, 0},
    {2, 6, 0, 0, 0, 0, 0, 0}, {0, 2, 6, 0, 0, 0, 0, 0}, {1, 2, 6, 0, 0, 0, 0, 0}, {0, 1, 2, 6, 0, 0, 0, 0},
    {3, 6, 0, 0, 0, 0, 0, 0}, {0, 3, 6, 0, 0, 0, 0, 0}, {1, 3, 6, 0, 0, 0, 0, 0}, {0, 1, 3, 6, 0, 0, 0, 0},
    {2, 3, 6, 0, 0, 0, 0, 0}, {0, 2, 3, 6, 0, 0, 0, 0}, {1, 2, 3, 6, 0, 0, 0, 0}, {0, 1, 2, 3, 6, 0, 0, 0},
    {4, 6, 0, 0, 0, 0, 0, 0}, {0, 4, 6, 0, 0, 0, 0, 0}, {1, 4, 6, 0, 0, 0, 0, 0}, {0, 1, 4, 6, 0, 0, 0, 0},
    {2, 4, 6, 0, 0, 0, 0, 0}, {0, 2, 4, 6, 0, 0, 0, 0}, {1, 2, 4, 6, 0, 0, 0, 0}, {0, 1, 2, 4, 6, 0, 0, 0},
    {3, 4, 6, 0, 0, 0, 0, 0}, {0, 3, 4, 6, 0, 0, 0, 0}, {1, 3, 4, 6, 0, 0, 0, 0}, {0, 1, 3, 4, 6, 0, 0, 0},
    {2, 3, 4, 6, 0, 0, 0, 0}, {0, 2, 3, 4, 6, 0, 0, 0}, {1, 2, 3, 4, 6, 0, 0, 0}, {0, 1, 2, 3, 4, 6, 0, 0},
    {5, 6, 0, 0, 0, 0, 0, 0}, {0, 5, 6, 0, 0, 0, 0, 0}, {1, 5, 6, 0, 0, 0, 0, 0}, {0, 1, 5, 6, 0, 0, 0, 0},
    {2, 5, 6, 0, 0, 0, 0, 0}, {0, 2, 5, 6, 0, 0, 0, 0}, {1, 2, 5, 6, 0, 0, 0, 0}, {0, 1, 2, 5, 6, 0, 0, 0},
    {3, 5, 6, 0, 0, 0, 0, 0}, {0, 3, 5, 6, 0, 0, 0, 0}, {1, 3, 5, 6, 0, 0, 0, 0}, {0, 1, 3, 5, 6, 0, 0, 0},
    {2, 3, 5, 6, 0, 0, 0, 0}, {0, 2, 3, 5, 6, 0, 0, 0}, {1, 2, 3, 5, 6, 0, 0, 0}, {0, 1, 2, 3, 5, 6, 0, 0},
    {4, 5, 6, 0, 0, 0, 0, 0}, {0, 4, 5, 6, 0, 0, 0, 0}, {1, 4, 5, 6, 0, 0, 0, 0}, {0, 1, 4, 5, 6, 0, 0, 0},
    {2, 4, 5, 6, 0, 0, 0, 0}, {0, 2, 4, 5, 6, 0, 0, 0}, {1, 2, 4, 5, 6, 0, 0, 0}, {0, 1, 2, 4, 5, 6, 0, 0},
    {3, 4, 5, 6, 0, 0, 0, 0}, {0, 3, 4, 5, 6, 0, 0, 0}, {1, 3, 4, 5, 6, 0, 0, 0}, {0, 1, 3, 4, 5, 6, 0, 0},
    {2, 3, 4, 5, 6, 0, 0, 0}, {0, 2, 3, 4, 5, 6, 0, 0}, {1, 2, 3, 4, 5, 6, 0, 0}, {0, 1, 2, 3, 4, 5, 6, 0},
    {7, 0, 0, 0, 0, 0, 0, 0}, {0, 7, 0, 0, 0, 0, 0, 0}, {1, 7, 0, 0, 0, 0, 0, 0}, {0, 1, 7, 0, 0, 0, 0, 0},
    {2, 7, 0, 0, 0, 0, 0, 0}, {0, 2, 7, 0, 0, 0, 0, 0}, {1, 2, 7, 0, 0, 0, 0, 0}, {0, 1, 2, 7, 0, 0, 0, 0},
    {3, 7, 0, 0, 0, 0, 0, 0}, {0, 3, 7, 0, 0, 0, 0, 0}, {1, 3, 7, 0, 0, 0, 0, 0}, {0, 1, 3, 7, 0, 0, 0, 0},
    {2, 3, 7, 0, 0, 0, 0, 0}, {0, 2, 3, 7, 0, 0, 0, 0}, {1, 2, 3, 7, 0, 0, 0, 0}, {0, 1, 2, 3, 7, 0, 0, 0},
    {4, 7, 0, 0, 0, 0, 0, 0}, {0, 4, 7, 0, 0, 0, 0, 0}, {1, 4, 7, 0, 0, 0, 0, 0}, {0, 1, 4, 7, 0, 0, 0, 0},
    {2, 4, 7, 0, 0, 0, 0, 0}, {0, 2, 4, 7, 0, 0, 0, 0}, {1, 2, 4, 7, 0, 0, 0, 0}, {0, 1, 2, 4, 7, 0, 0, 0},
    {3, 4, 7, 0, 0, 0, 0, 0}, {0, 3, 4, 7, 0, 0, 0, 0}, {1, 3, 4, 7, 0, 0, 0, 0}, {0, 1, 3, 4, 7, 0, 0, 0},
    {2, 3, 4, 7, 0, 0, 0, 0}, {0, 2, 3, 4, 7, 0, 0, 0}, {1, 2, 3, 4, 7, 0, 0, 0}, {0, 1, 2, 3, 4, 7, 0, 0},
    {5, 7, 0, 0, 0, 0, 0, 0}, {0, 5, 7, 0, 0, 0, 0, 0}, {1, 5, 7, 0, 0, 0, 0, 0}, {0, 1, 5, 7, 0, 0, 0, 0},
    {2, 5, 7, 0, 0, 0, 0, 0}, {0, 2, 5, 7, 0, 0, 0, 0}, {1, 2, 5, 7, 0, 0, 0, 0}, {0, 1, 2, 5, 7, 0, 0, 0},
    {3, 5, 7, 0, 0, 0, 0, 0}, {0, 3, 5, 7, 0, 0, 0, 0}, {1, 3, 5, 7, 0, 0, 0, 0}, {0, 1, 3, 5, 7, 0, 0, 0},
    {2, 3, 5, 7, 0, 0, 0, 0}, {0, 2, 3, 5, 7, 0, 0, 0}, {1, 2, 3, 5, 7, 0, 0, 0}, {0, 1, 2, 3, 5, 7, 0, 0},
    {4, 5, 7, 0, 0, 0, 0, 0}, {0, 4, 5, 7, 0, 0, 0, 0}, {1, 4, 5, 7, 0, 0, 0, 0}, {0, 1, 4, 5, 7, 0, 0, 0},
    {2, 4, 5, 7, 0, 0, 0, 0}, {0, 2, 4, 5, 7, 0, 0, 0}, {1, 2, 4, 5, 7, 0, 0, 0}, {0, 1, 2, 4, 5, 7, 0, 0},
    {3, 4, 5, 7, 0, 0, 0, 0}, {0, 3, 4, 5, 7, 0, 0, 0}, {1, 3, 4, 5, 7, 0, 0, 0}, {0, 1, 3, 4, 5, 7, 0, 0},
    {2, 3, 4, 5, 7, 0, 0, 0}, {0, 2, 3, 4, 5, 7, 0, 0}, {1, 2, 3, 4, 5, 7, 0, 0}, {0, 1, 2, 3, 4, 5, 7, 0},
    {6, 7, 0, 0, 0, 0, 0, 0}, {0, 6, 7, 0, 0, 0, 0, 0}, {1, 6, 7, 0, 0, 0, 0, 0}, {0, 1, 6, 7, 0, 0, 0, 0},
    {2, 6, 7, 0, 0, 0, 0, 0}, {0, 2, 6, 7, 0, 0, 0, 0}, {1, 2, 6, 7, 0, 0, 0, 0}, {0, 1, 2, 6, 7, 0, 0, 0},
    {3, 6, 7, 0, 0, 0, 0, 0}, {0, 3, 6, 7, 0, 0, 0, 0}, {1, 3, 6, 7, 0, 0, 0, 0}, {0, 1, 3, 6, 7, 0, 0, 0},
    {2, 3, 6, 7, 0, 0, 0, 0}, {0, 2, 3, 6, 7, 0, 0, 0}, {1, 2, 3, 6, 7, 0, 0, 0}, {0, 1, 2, 3, 6, 7, 0, 0},
    {4, 6, 7, 0, 0, 0, 0, 0}, {0, 4, 6, 7, 0, 0, 0, 0}, {1, 4, 6, 7, 0, 0, 0, 0}, {0, 1, 4, 6, 7, 0, 0, 0},
    {2, 4, 6, 7, 0, 0, 0, 0}, {0, 2, 4, 6, 7, 0, 0, 0}, {1, 2, 4, 6, 7, 0, 0, 0}, {0, 1, 2, 4, 6, 7, 0, 0},
    {3, 4, 6, 7, 0, 0, 0, 0}, {0, 3, 4, 6, 7, 0, 0, 0}, {1, 3, 4, 6, 7, 0, 0, 0}, {0, 1, 3, 4, 6, 7, 0, 0},
    {2, 3, 4, 6, 7, 0, 0, 0}, {0, 2, 3, 4, 6, 7, 0, 0}, {1, 2, 3, 4, 6, 7, 0, 0}, {0, 1, 2, 3, 4, 6, 7, 0},
    {5, 6, 7, 0, 0, 0, 0, 0}, {0, 5, 6, 7, 0, 0, 0, 0}, {1, 5, 6, 7, 0, 0, 0, 0}, {0, 1, 5, 6, 7, 0, 0, 0},
    {2, 5, 6, 7, 0, 0, 0, 0}, {0, 2, 5, 6, 7, 0, 0, 0}, {1, 2, 5, 6, 7, 0, 0, 0}, {0, 1, 2, 5, 6, 7, 0, 0},
    {3, 5, 6, 7, 0, 0, 0, 0}, {0, 3, 5, 6, 7, 0, 0, 0}, {1, 3, 5, 6, 7, 0, 0, 0}, {0, 1, 3, 5, 6, 7, 0, 0},
    {2, 3, 5, 6, 7, 0, 0, 0}, {0, 2, 3, 5, 6, 7, 0, 0}, {1, 2, 3, 5, 6, 7, 0, 0}, {0, 1, 2, 3, 5, 6, 7, 0},
    {4, 5, 6, 7, 0, 0, 0, 0}, {0, 4, 5, 6, 7, 0, 0, 0}, {1, 4, 5, 6, 7, 0, 0, 0}, {0, 1, 4, 5, 6, 7, 0, 0},
    {2, 4, 5, 6, 7, 0, 0, 0}, {0, 2, 4, 5, 6, 7, 0, 0}, {1, 2, 4, 5, 6, 7, 0, 0}, {0, 1, 2, 4, 5, 6, 7, 0},
    {3, 4, 5, 6, 7, 0, 0, 0}, {0, 3, 4, 5, 6, 7, 0, 0}, {1, 3, 4, 5, 6, 7, 0, 0}, {0, 1, 3, 4, 5, 6, 7, 0},
    {2, 3, 4, 5, 6, 7, 0, 0}, {0, 2, 3, 4, 5, 6, 7, 0}, {1, 2, 3, 4, 5, 6, 7, 0}, {0, 1, 2, 3, 4, 5, 6, 7},
};

/* Emit every set bit of `starts` as `base + bit`, in increasing order.  Kept
 * separate from the scan so the caller can write the previous block's indexes
 * while the current block is being classified (the same pipelining simdjson
 * uses).
 *
 * The obvious loop (tzcnt, clear the bit, store) is bound by the serial
 * clear-lowest-bit chain: measured at ~1.5 cycles per index and the single
 * largest cost in stage 1.  Instead, split the mask into its eight bytes and
 * convert each byte into up to eight offsets with one table load.  Each group
 * stores eight indexes and advances by the byte's population count, so the
 * surplus slots are simply overwritten by the next group (and by the next
 * block, since the following write starts exactly at the new count).  The
 * index buffer is allocated with 64 spare entries, which covers the slack. */
static inline JSOC_ALWAYS_INLINE void
stage1_write_indexes(uint32_t *out, size_t *count, uint64_t starts,
                     size_t base) {
#ifdef JSOC_EXP_S1_NO_WRITE
  (void)out; (void)base;
  if (starts != 0) *count += (size_t)__builtin_popcountll(starts);
  return;
#endif
  if (JSOC_UNLIKELY(starts == 0))
    return;
  size_t n = *count;
  __m256i bv = _mm256_set1_epi32((int)(uint32_t)base);
  const __m256i step = _mm256_set1_epi32(8);
  for (unsigned i = 0; i < 8; i++, bv = _mm256_add_epi32(bv, step)) {
    const unsigned byte = (unsigned)(starts >> (i * 8)) & 0xFFu;
    if (byte == 0)
      continue;
    const __m128i raw =
        _mm_loadl_epi64((const __m128i *)(const void *)stage1_pos_table[byte]);
    const __m256i v = _mm256_cvtepu8_epi32(raw);
    _mm256_storeu_si256((__m256i *)(void *)(out + n), _mm256_add_epi32(v, bv));
    n += (size_t)__builtin_popcount(byte);
  }
  *count = n;
}

static inline JSOC_ALWAYS_INLINE bool
stage1_scan(const char *input, size_t length, uint32_t *indexes,
            const bool branchless, size_t *out_count) {
  stage1_t st;
  memset(&st, 0, sizeof st);
  jsoc_utf8_state_init(&st.utf8);

  const uint8_t *p = (const uint8_t *)input;
  size_t base = 0;
  size_t count = 0;

  /* Delay each block's index write by one iteration so it overlaps the next
   * block's classification. */
  uint64_t held_starts = 0;
  size_t held_base = 0;
  bool have_held = false;

  while (base + 64 <= length) {
    uint64_t starts;
    if (JSOC_UNLIKELY(!stage1_compute_block(&st, input, length, p + base, base,
                                            ~(uint64_t)0, branchless,
                                            &starts))) {
      return false;
    }
    if (have_held)
      stage1_write_indexes(indexes, &count, held_starts, held_base);
    held_starts = starts;
    held_base = base;
    have_held = true;
    base += 64;
  }

  if (base < length) {
    alignas(64) uint8_t block[64];
    const size_t rem = length - base;
    memset(block, ' ', sizeof block);
    memcpy(block, p + base, rem);
    const uint64_t valid = ((uint64_t)1 << rem) - 1;
    uint64_t starts;
    if (JSOC_UNLIKELY(!stage1_compute_block(&st, input, length, block, base,
                                            valid, branchless, &starts))) {
      return false;
    }
    if (have_held)
      stage1_write_indexes(indexes, &count, held_starts, held_base);
    held_starts = starts;
    held_base = base;
    have_held = true;
  }
  if (have_held)
    stage1_write_indexes(indexes, &count, held_starts, held_base);

  st.utf8.error = _mm256_or_si256(st.utf8.error, st.utf8.prev_incomplete);
  if (JSOC_UNLIKELY(!_mm256_testz_si256(st.utf8.error, st.utf8.error)))
    return false;

  *out_count = count;
  return true;
}

JSOC_INTERNAL bool jsoc_dom_stage1(const char *input, size_t length,
                                   jsoc_arena *arena, bool branchless,
                                   uint32_t **out_indexes, size_t *out_count) {
  uint32_t *indexes = jsoc_arena_alloc(
      arena, (length + 64) * sizeof(uint32_t), _Alignof(uint32_t));
  if (JSOC_UNLIKELY(!indexes))
    return false;

  /* `branchless` is a literal at each call so the string-scan branch is
   * compiled out on the wordy path. */
  if (branchless) {
    if (!stage1_scan(input, length, indexes, true, out_count))
      return false;
  } else {
    if (!stage1_scan(input, length, indexes, false, out_count))
      return false;
  }
  *out_indexes = indexes;
  return true;
}

static inline JSOC_ALWAYS_INLINE jsoc_result
tokenize_simd_core(const char *input, size_t length, jsoc_arena *arena,
                   size_t *overflow_token, bool check_numbers, bool stream,
                   bool compact, bool check_utf8) {
  jsoc_result result = {
      .error = {JSOC_OK, 0, 0, 0}, .tokens = NULL, .count = 0};

  if (overflow_token)
    *overflow_token = SIZE_MAX;

  fused_t f;
  memset(&f, 0, sizeof f);
  f.input = input;
  f.length = length;
  f.arena = arena;
  f.state = JSOC_GS_DOC;
  f.check_numbers = check_numbers;
  f.stream = stream;
  f.compact = compact;
  f.check_utf8 = check_utf8;
  f.overflow_token = SIZE_MAX;

  /* Same density heuristic as the portable path; geometric growth handles
   * adversarially dense inputs without a second pass. */
  const size_t initial = length / 2 + 64;
  f.tokens = jsoc_arena_alloc(arena, initial * sizeof(jsoc_token),
                              _Alignof(jsoc_token));
  if (JSOC_UNLIKELY(!f.tokens)) {
    result.error = (jsoc_error){JSOC_ERR_NOMEM, 0, 1, 1};
    return result;
  }
  f.capacity = initial;

  const uint8_t *p = (const uint8_t *)input;
  size_t base = 0;

  while (base + 64 <= length) {
    fused_process_block(&f, p + base, base, ~(uint64_t)0);
    if (JSOC_UNLIKELY(f.failed))
      break;
    base += 64;
  }

  if (!f.failed && base < length) {
    alignas(64) uint8_t block[64];
    const size_t rem = length - base; /* 1..63: full blocks are done */
    memset(block, ' ', sizeof block);
    memcpy(block, p + base, rem);
    const uint64_t valid = ((uint64_t)1 << rem) - 1;
    fused_process_block(&f, block, base, valid);
    /* A failure is handled below: if the fast scan saw a string special
     * it must hand over to the reference scanner, which reports the
     * first error in input order. */
  }

  if (JSOC_UNLIKELY(f.need_scalar || f.failed)) {
    /* The fused scan may have stopped early, so consult the full validator
     * to preserve the existing precedence: UTF-8 errors are reported before
     * syntax errors anywhere in the input. */
    if (check_utf8) {
      size_t off = 0;
      if (!jsoc_utf8_valid(input, length, &off)) {
        result.error =
            jsoc_error_at_offset(input, length, JSOC_ERR_INVALID_UTF8, off);
        result.tokens = f.tokens;
        result.count = f.count;
        if (overflow_token)
          *overflow_token = f.overflow_token;
        return result;
      }
    }
    if (f.need_scalar) {
      /* Invalid escape/control byte: the reference scanner reports the
       * exact error and the same partial token stream. */
      return tokenize_scalar(input, length, arena, overflow_token, stream,
                             compact);
    }
    goto fail;
  }

  if (check_utf8) {
    f.utf8.error = _mm256_or_si256(f.utf8.error, f.utf8.prev_incomplete);
    if (JSOC_UNLIKELY(!_mm256_testz_si256(f.utf8.error, f.utf8.error))) {
      /* Cold: find the exact offending byte with the scalar oracle. */
      size_t off = 0;
      (void)jsoc_utf8_validate_scalar((const uint8_t *)input, length, &off);
      result.error =
          jsoc_error_at_offset(input, length, JSOC_ERR_INVALID_UTF8, off);
      result.tokens = f.tokens;
      result.count = f.count;
      if (overflow_token)
        *overflow_token = f.overflow_token;
      return result;
    }
  }

  if (f.pending == JSOC_PEND_STRING) {
    fused_fail(&f, JSOC_ERR_UNTERMINATED_STRING, length);
    goto fail;
  }

  /* Reserve room for the final scalar (if any) and the EOF token. */
  if (JSOC_UNLIKELY(f.capacity - f.count < 2)) {
    if (!fused_grow(&f, f.count + 2))
      goto fail;
  }

  if (JSOC_UNLIKELY(f.depth > 0)) {
    const jsoc_error_code code = (f.stack[f.depth - 1] == '}')
                                     ? JSOC_ERR_UNTERMINATED_OBJECT
                                     : JSOC_ERR_UNTERMINATED_ARRAY;
    fused_fail(&f, code, length);
    goto fail;
  }

  if (JSOC_UNLIKELY(f.count == 0)) {
    fused_fail(&f, JSOC_ERR_EXPECTED_VALUE, length);
    goto fail;
  }

  fused_emit(&f, JSOC_TOK_EOF, length, 0);

  result.tokens = f.tokens;
  result.count = f.count;
  if (overflow_token)
    *overflow_token = f.overflow_token;
  return result;

fail:
  result.error = jsoc_error_at_offset(input, length, f.err.code, f.err.offset);
  result.tokens = f.tokens;
  result.count = f.count;
  if (overflow_token)
    *overflow_token = f.overflow_token;
  return result;
}

JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_impl(const char *input,
                                                  size_t length,
                                                  jsoc_arena *arena) {
  return tokenize_simd_core(input, length, arena, NULL, false, false, false,
                            false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_stream_impl(const char *input, size_t length,
                               jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_simd_core(input, length, arena, overflow_token,
                            overflow_token != NULL, true, false, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_checked_impl(const char *input, size_t length,
                                jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_simd_core(input, length, arena, overflow_token, true, false,
                            false, false);
}

JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_compact_impl(const char *input,
                                                          size_t length,
                                                          jsoc_arena *arena,
                                                          size_t *overflow_token) {
  return tokenize_simd_core(input, length, arena, overflow_token, true, false,
                            true, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_stream_compact_impl(const char *input, size_t length,
                                       jsoc_arena *arena,
                                       size_t *overflow_token) {
  return tokenize_simd_core(input, length, arena, overflow_token,
                            overflow_token != NULL, true, true, false);
}

#endif /* JSOC_SIMD_AVX2 */

#if !JSOC_SIMD_AVX2
/* No SIMD in this build: every backend entry point is the portable scanner.
 * The SIMD-named ones exist so callers do not need #ifdef. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_impl(const char *input,
                                                  size_t length,
                                                  jsoc_arena *arena) {
  return tokenize_scalar(input, length, arena, NULL, false, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_stream_impl(const char *input, size_t length,
                               jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, true, false);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_checked_impl(const char *input, size_t length,
                                jsoc_arena *arena, size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, false, false);
}

JSOC_INTERNAL jsoc_result jsoc_tokenize_simd_compact_impl(const char *input,
                                                          size_t length,
                                                          jsoc_arena *arena,
                                                          size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, false, true);
}

JSOC_INTERNAL jsoc_result
jsoc_tokenize_simd_stream_compact_impl(const char *input, size_t length,
                                       jsoc_arena *arena,
                                       size_t *overflow_token) {
  return tokenize_scalar(input, length, arena, overflow_token, true, true);
}

JSOC_INTERNAL bool jsoc_dom_stage1(const char *input, size_t length,
                                   jsoc_arena *arena, bool branchless,
                                   uint32_t **out_indexes, size_t *out_count) {
  (void)input;
  (void)length;
  (void)arena;
  (void)branchless;
  (void)out_indexes;
  (void)out_count;
  return false;
}
#endif

JSOC_INTERNAL bool jsoc_tokenizer_simd_available(void) {
#if JSOC_SIMD_AVX2
  return true;
#else
  return false;
#endif
}

static jsoc_result tokenize_dispatch(const char *input, size_t length,
                                     jsoc_arena *arena,
                                     size_t *overflow_token) {
  if (overflow_token)
    *overflow_token = SIZE_MAX;
  if (JSOC_UNLIKELY(!input || length == 0)) {
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1},
                         .tokens = NULL,
                         .count = 0};
  }

#if JSOC_SIMD_AVX2
  /* Stage 1 has a fixed 64-byte setup cost; tiny documents are cheaper in
   * the portable state machine. */
  if (length >= 64) {
    return overflow_token ? jsoc_tokenize_simd_checked_impl(
                                input, length, arena, overflow_token)
                          : jsoc_tokenize_simd_impl(input, length, arena);
  }
#endif
  return overflow_token ? jsoc_tokenize_scalar_checked_impl(
                              input, length, arena, overflow_token)
                        : jsoc_tokenize_scalar_impl(input, length, arena);
}

JSOC_API jsoc_result jsoc_tokenize(const char *input, size_t length,
                                   jsoc_arena *arena) {
  return tokenize_dispatch(input, length, arena, NULL);
}

/* Stream variant of the internal dispatcher: identical token stream to
 * `jsoc_tokenize_checked`, except that adjacent top-level documents are
 * accepted (ndjson). */
JSOC_INTERNAL jsoc_result jsoc_tokenize_stream_checked(const char *input,
                                                       size_t length,
                                                       jsoc_arena *arena,
                                                       size_t *overflow_token) {
  if (overflow_token)
    *overflow_token = SIZE_MAX;
  if (JSOC_UNLIKELY(!input || length == 0)) {
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1},
                         .tokens = NULL,
                         .count = 0};
  }

#if JSOC_SIMD_AVX2
  /* Same 64-byte cut-over as the strict path. */
  if (length >= 64) {
    return jsoc_tokenize_simd_stream_impl(input, length, arena, overflow_token);
  }
#endif
  return jsoc_tokenize_scalar_stream_impl(input, length, arena, overflow_token);
}

/* Compact stream dispatcher: what `jsoc_parse_many` consumes.  Validation is
 * that of `jsoc_tokenize_stream_checked`; ',' and ':' are not stored. */
JSOC_INTERNAL jsoc_result
jsoc_tokenize_stream_compact_checked(const char *input, size_t length,
                                     jsoc_arena *arena,
                                     size_t *overflow_token) {
  if (overflow_token)
    *overflow_token = SIZE_MAX;
  if (JSOC_UNLIKELY(!input || length == 0)) {
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1},
                         .tokens = NULL,
                         .count = 0};
  }

#if JSOC_SIMD_AVX2
  if (length >= 64) {
    return jsoc_tokenize_simd_stream_compact_impl(input, length, arena,
                                                  overflow_token);
  }
#endif
  return jsoc_tokenize_scalar_stream_compact_impl(input, length, arena,
                                                  overflow_token);
}

/* Compact variant of the checked dispatcher: ',' and ':' are validated but
 * not stored, which is what the DOM consumes.  Errors are identical to
 * `jsoc_tokenize_checked` - only the stream differs. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_compact_checked(const char *input,
                                                        size_t length,
                                                        jsoc_arena *arena,
                                                        size_t *overflow_token) {
  if (overflow_token)
    *overflow_token = SIZE_MAX;
  if (JSOC_UNLIKELY(!input || length == 0)) {
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1},
                         .tokens = NULL,
                         .count = 0};
  }

#if JSOC_SIMD_AVX2
  /* Same 64-byte cut-over as the other paths. */
  if (length >= 64) {
    return jsoc_tokenize_simd_compact_impl(input, length, arena, overflow_token);
  }
#endif
  return jsoc_tokenize_scalar_compact_impl(input, length, arena, overflow_token);
}

/* DOM fast path: compact checked tokenizer that also validates UTF-8 inside
 * the same stage-1 pass.  On inputs the SIMD path does not handle, this falls
 * back to the ordinary UTF-8 check plus the portable compact tokenizer, so the
 * public entry points and their token layout are unchanged. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_compact_utf8(const char *input,
                                                     size_t length,
                                                     jsoc_arena *arena,
                                                     size_t *overflow_token) {
  if (overflow_token)
    *overflow_token = SIZE_MAX;
  if (JSOC_UNLIKELY(!input || length == 0)) {
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 1, 1}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1},
                         .tokens = NULL,
                         .count = 0};
  }

#if JSOC_SIMD_AVX2
  if (length >= 64) {
    return tokenize_simd_core(input, length, arena, overflow_token, true, false,
                              true, true);
  }
#endif

  size_t utf8_offset = 0;
  if (JSOC_UNLIKELY(!jsoc_utf8_valid(input, length, &utf8_offset))) {
    return (jsoc_result){.error = jsoc_error_at_offset(
                                       input, length, JSOC_ERR_INVALID_UTF8,
                                       utf8_offset),
                         .tokens = NULL,
                         .count = 0};
  }
  return jsoc_tokenize_compact_checked(input, length, arena, overflow_token);
}

/* Internal entry point used by the DOM: identical token stream, plus the token
 * index of the first syntactically valid but unrepresentable number (or
 * SIZE_MAX).  Range failures are reported out of band so a caller exposing
 * only the first document can ignore numbers in trailing values. */
JSOC_INTERNAL jsoc_result jsoc_tokenize_checked(const char *input,
                                                size_t length,
                                                jsoc_arena *arena,
                                                size_t *overflow_token) {
  return tokenize_dispatch(input, length, arena, overflow_token);
}

JSOC_API jsoc_result jsoc_tokenize_file(const char *path, jsoc_arena *arena) {
  if (JSOC_UNLIKELY(!path || !arena)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 0, 0}, .tokens = NULL, .count = 0};
  }

  FILE *f = fopen(path, "rb");
  if (JSOC_UNLIKELY(!f)) {
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 0, 0}, .tokens = NULL, .count = 0};
  }
  if (JSOC_UNLIKELY(fseek(f, 0, SEEK_END) != 0)) {
    fclose(f);
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 0, 0}, .tokens = NULL, .count = 0};
  }
  long end = ftell(f);
  if (JSOC_UNLIKELY(end < 0)) {
    fclose(f);
    return (jsoc_result){
        .error = {JSOC_ERR_SYNTAX, 0, 0, 0}, .tokens = NULL, .count = 0};
  }
  rewind(f);

  size_t length = (size_t)end;
  if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
    fclose(f);
    return (jsoc_result){.error = {JSOC_ERR_INPUT_TOO_LARGE, 0, 0, 0},
                         .tokens = NULL,
                         .count = 0};
  }

  /* Zero-copy tokens point into this buffer, so it must live exactly as
   * long as the arena (and therefore the token array).  Never free it
   * here. */
  char *buf = jsoc_arena_alloc(arena, length + 1, 1);
  if (JSOC_UNLIKELY(!buf)) {
    fclose(f);
    return (jsoc_result){
        .error = {JSOC_ERR_NOMEM, 0, 0, 0}, .tokens = NULL, .count = 0};
  }
  size_t got = fread(buf, 1, length, f);
  fclose(f);
  buf[got] = '\0';

  return jsoc_tokenize(buf, got, arena);
}

JSOC_API const char *jsoc_decode_error_message(jsoc_decode_error code) {
  switch (code) {
  case JSOC_DECODE_OK:
    return "Success";
  case JSOC_DECODE_TYPE_MISMATCH:
    return "Type mismatch";
  case JSOC_DECODE_MISSING_FIELD:
    return "Missing required field";
  case JSOC_DECODE_EXTRA_FIELD:
    return "Unexpected field";
  case JSOC_DECODE_OVERFLOW:
    return "Value overflow";
  case JSOC_DECODE_CUSTOM:
    return "Custom decode error";
  default:
    return "Unknown decode error";
  }
}
