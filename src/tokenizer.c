#include "jsoc.h"
#include "token.h"
#include "arena.h"
#include "error.h"
#include "simd.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* Hard limit on container nesting.  The parser is iterative, so this only
 * bounds the bracket stack; it exists to turn pathological input into a
 * clean error instead of unbounded memory use downstream. */
#define JSOC_MAX_DEPTH 1024

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
} tokenizer_t;

/* ------------------------------------------------------------------------- */
/* Errors                                                                     */
/* ------------------------------------------------------------------------- */

/* Line/column are only needed when reporting a failure, so they are computed
 * lazily.  Eager per-byte tracking costs a branch and two increments on every
 * byte of the input, which is a significant fraction of total tokenizer time. */
static JSOC_NOINLINE jsoc_error make_error_at(const tokenizer_t *t, jsoc_error_code code, size_t offset) {
    jsoc_error err;
    err.code = code;
    err.offset = offset;
    err.line = 1;
    err.column = 1;

    size_t limit = offset < t->length ? offset : t->length;
    for (size_t i = 0; i < limit; i++) {
        if (t->input[i] == '\n') {
            err.line++;
            err.column = 1;
        } else {
            err.column++;
        }
    }
    return err;
}

static inline jsoc_error make_error(const tokenizer_t *t, jsoc_error_code code) {
    return make_error_at(t, code, t->pos);
}

static const jsoc_error JSOC_ERR_NONE = { JSOC_OK, 0, 0, 0 };

/* Tokens store 32-bit offsets/lengths, so inputs beyond 4 GiB are rejected. */
static inline bool tokens_fit_u32(size_t length) {
    return length <= (size_t)UINT32_MAX;
}

/* ------------------------------------------------------------------------- */
/* Token storage (arena-backed: no malloc, no leak, dies with the arena)      */
/* ------------------------------------------------------------------------- */

static JSOC_NOINLINE jsoc_error tokens_grow(tokenizer_t *t) {
    size_t new_capacity = t->capacity ? t->capacity * 2 : 256;
    jsoc_token *items = jsoc_arena_alloc(t->arena, new_capacity * sizeof(jsoc_token), _Alignof(jsoc_token));
    if (JSOC_UNLIKELY(!items)) return make_error(t, JSOC_ERR_NOMEM);

    if (t->count != 0) {
        memcpy(items, t->tokens, t->count * sizeof(jsoc_token));
    }
    t->tokens = items;
    t->capacity = new_capacity;
    return JSOC_ERR_NONE;
}

static inline jsoc_error push_token(tokenizer_t *t, jsoc_token_type type, size_t offset, size_t length) {
    if (JSOC_UNLIKELY(t->count == t->capacity)) {
        jsoc_error err = tokens_grow(t);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) return err;
    }
    t->tokens[t->count++] = jsoc_token_make(type, (uint32_t)offset, (uint32_t)length);
    return JSOC_ERR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Character classes                                                          */
/* ------------------------------------------------------------------------- */

static inline bool is_digit(unsigned char c) {
    return (unsigned)(c - '0') < 10u;
}

static inline bool is_hex_digit(unsigned char c) {
    return (unsigned)(c - '0') < 10u ||
           (unsigned)(c - 'a') < 6u ||
           (unsigned)(c - 'A') < 6u;
}

static inline bool starts_value(unsigned char c) {
    return c == '{' || c == '[' || c == '"' || c == 't' || c == 'f' || c == 'n' ||
           c == '-' || is_digit(c);
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
            if (JSOC_UNLIKELY(err.code != JSOC_OK)) return err;
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
            case '"': case '\\': case '/': case 'b':
            case 'f': case 'n': case 'r': case 't':
                i += 2;
                break;
            case 'u':
                if (JSOC_UNLIKELY(i + 6 > len)) {
                    return make_error_at(t, JSOC_ERR_INVALID_UNICODE, i);
                }
                for (size_t k = 2; k < 6; k++) {
                    if (JSOC_UNLIKELY(!is_hex_digit((unsigned char)in[i + k]))) {
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

static jsoc_error parse_number(tokenizer_t *t) {
    const char *in = t->input;
    const size_t len = t->length;
    const size_t start = t->pos;
    size_t i = start;

    if (in[i] == '-') i++;

    if (JSOC_UNLIKELY(i >= len || !is_digit((unsigned char)in[i]))) {
        return make_error_at(t, JSOC_ERR_INVALID_NUMBER, i);
    }

    if (in[i] == '0') {
        i++;
        /* JSON forbids leading zeroes (01, -012). */
        if (JSOC_UNLIKELY(i < len && is_digit((unsigned char)in[i]))) {
            return make_error_at(t, JSOC_ERR_INVALID_NUMBER, i);
        }
    } else {
        while (i < len && is_digit((unsigned char)in[i])) i++;
    }

    if (i < len && in[i] == '.') {
        i++;
        if (JSOC_UNLIKELY(i >= len || !is_digit((unsigned char)in[i]))) {
            return make_error_at(t, JSOC_ERR_INVALID_NUMBER, i);
        }
        while (i < len && is_digit((unsigned char)in[i])) i++;
    }

    if (i < len && (in[i] == 'e' || in[i] == 'E')) {
        i++;
        if (i < len && (in[i] == '+' || in[i] == '-')) i++;
        if (JSOC_UNLIKELY(i >= len || !is_digit((unsigned char)in[i]))) {
            return make_error_at(t, JSOC_ERR_INVALID_NUMBER, i);
        }
        while (i < len && is_digit((unsigned char)in[i])) i++;
    }

    t->pos = i;
    return push_token(t, JSOC_TOK_NUMBER, start, i - start);
}

static jsoc_error parse_literal(tokenizer_t *t, const char *lit, size_t len, jsoc_token_type type) {
    if (JSOC_UNLIKELY(t->pos + len > t->length)) {
        return make_error(t, JSOC_ERR_EXPECTED_VALUE);
    }
    if (JSOC_UNLIKELY(memcmp(t->input + t->pos, lit, len) != 0)) {
        return make_error(t, JSOC_ERR_EXPECTED_VALUE);
    }
    jsoc_error err = push_token(t, type, t->pos, len);
    if (JSOC_UNLIKELY(err.code != JSOC_OK)) return err;
    t->pos += len;
    return JSOC_ERR_NONE;
}

/* ------------------------------------------------------------------------- */
/* Tokenizer                                                                  */
/* ------------------------------------------------------------------------- */

JSOC_API jsoc_result jsoc_tokenize(const char *input, size_t length, jsoc_arena *arena) {
    jsoc_result result = { .error = { JSOC_OK, 0, 0, 0 }, .tokens = NULL, .count = 0 };

    if (JSOC_UNLIKELY(!input || length == 0)) {
        result.error = (jsoc_error){ JSOC_ERR_SYNTAX, 0, 1, 1 };
        return result;
    }
    if (JSOC_UNLIKELY(!arena)) {
        result.error = (jsoc_error){ JSOC_ERR_NOMEM, 0, 1, 1 };
        return result;
    }
    if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
        result.error = (jsoc_error){ JSOC_ERR_INPUT_TOO_LARGE, 0, 1, 1 };
        return result;
    }

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
    };

    /* Heuristic first allocation; geometric growth handles dense inputs
     * (e.g. "[1,1,1,...]" can approach one token per byte).  The measured
     * average for real JSON is ~3.4 bytes/token, so /3 avoids a grow+copy
     * on typical documents. */
    size_t initial = length / 3 + 32;
    t.tokens = jsoc_arena_alloc(arena, initial * sizeof(jsoc_token), _Alignof(jsoc_token));
    if (JSOC_UNLIKELY(!t.tokens)) {
        result.error = (jsoc_error){ JSOC_ERR_NOMEM, 0, 1, 1 };
        return result;
    }
    t.capacity = initial;

    jsoc_error err = JSOC_ERR_NONE;

    /* State: expecting a value. */
sv_value:
    t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
    if (JSOC_UNLIKELY(t.pos >= t.length)) goto at_eof;

    switch ((unsigned char)t.input[t.pos]) {
    case '{':
        if (JSOC_UNLIKELY(t.depth >= JSOC_MAX_DEPTH)) { err = make_error(&t, JSOC_ERR_DEPTH_EXCEEDED); goto fail; }
        err = push_token(&t, JSOC_TOK_OBJ_BEGIN, t.pos, 1);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.stack[t.depth++] = '}';
        t.pos++;
        t.after_comma = false;
        goto sv_key;

    case '[':
        if (JSOC_UNLIKELY(t.depth >= JSOC_MAX_DEPTH)) { err = make_error(&t, JSOC_ERR_DEPTH_EXCEEDED); goto fail; }
        err = push_token(&t, JSOC_TOK_ARR_BEGIN, t.pos, 1);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
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
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
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
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.after_comma = false;
        goto sv_after;

    case 't':
        err = parse_literal(&t, "true", 4, JSOC_TOK_TRUE);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.after_comma = false;
        goto sv_after;

    case 'f':
        err = parse_literal(&t, "false", 5, JSOC_TOK_FALSE);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.after_comma = false;
        goto sv_after;

    case 'n':
        err = parse_literal(&t, "null", 4, JSOC_TOK_NULL);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.after_comma = false;
        goto sv_after;

    case ',': case ':':
        /* A comma/colon where a value belongs. */
        err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
        goto fail;

    default:
        if (is_digit((unsigned char)t.input[t.pos]) || t.input[t.pos] == '-') {
            err = parse_number(&t);
            if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
            t.after_comma = false;
            goto sv_after;
        }
        err = make_error(&t, JSOC_ERR_EXPECTED_VALUE);
        goto fail;
    }

/* State: inside an object, expecting a key or the closing brace. */
sv_key:
    t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
    if (JSOC_UNLIKELY(t.pos >= t.length)) goto at_eof;

    if (t.input[t.pos] == '}') {
        if (JSOC_UNLIKELY(t.after_comma)) {
            err = make_error(&t, JSOC_ERR_EXPECTED_KEY);
            goto fail;
        }
        err = push_token(&t, JSOC_TOK_OBJ_END, t.pos, 1);
        if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
        t.depth--;
        t.pos++;
        goto sv_after;
    }
    if (JSOC_UNLIKELY(t.input[t.pos] != '"')) {
        err = make_error_at(&t, JSOC_ERR_EXPECTED_KEY, t.pos);
        goto fail;
    }
    err = parse_string(&t);
    if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
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
    if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
    t.pos++;
    goto sv_value;

/* State: after a complete value, expecting ',' or the matching close. */
sv_after:
    t.pos = jsoc_skip_ws(t.input, t.length, t.pos);
    if (JSOC_UNLIKELY(t.pos >= t.length)) goto at_eof;

    if (JSOC_UNLIKELY(t.depth == 0)) {
        /* A second top-level value is only accepted when separated by
         * whitespace (ndjson-style streams), never back-to-back. */
        bool separated = t.pos > 0 && jsoc_is_ws((unsigned char)t.input[t.pos - 1]);
        if (separated && starts_value((unsigned char)t.input[t.pos])) goto sv_value;
        err = make_error(&t, JSOC_ERR_TRAILING_GARBAGE);
        goto fail;
    }

    {
        unsigned char c = (unsigned char)t.input[t.pos];
        if (c == ',') {
            err = push_token(&t, JSOC_TOK_COMMA, t.pos, 1);
            if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
            t.pos++;
            t.after_comma = true;
            if (t.stack[t.depth - 1] == '}') goto sv_key;
            goto sv_value;
        }
        if (t.stack[t.depth - 1] == '}' && c == '}') {
            err = push_token(&t, JSOC_TOK_OBJ_END, t.pos, 1);
            if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
            t.depth--;
            t.pos++;
            goto sv_after;
        }
        if (t.stack[t.depth - 1] == ']' && c == ']') {
            err = push_token(&t, JSOC_TOK_ARR_END, t.pos, 1);
            if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;
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
    if (JSOC_UNLIKELY(err.code != JSOC_OK)) goto fail;

    result.tokens = t.tokens;
    result.count = t.count;
    return result;

fail:
    result.error = err;
    result.tokens = t.tokens;
    result.count = t.count;
    return result;
}

JSOC_API jsoc_result jsoc_tokenize_file(const char *path, jsoc_arena *arena) {
    if (JSOC_UNLIKELY(!path || !arena)) {
        return (jsoc_result){ .error = { JSOC_ERR_NOMEM, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }

    FILE *f = fopen(path, "rb");
    if (JSOC_UNLIKELY(!f)) {
        return (jsoc_result){ .error = { JSOC_ERR_NOMEM, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }
    if (JSOC_UNLIKELY(fseek(f, 0, SEEK_END) != 0)) {
        fclose(f);
        return (jsoc_result){ .error = { JSOC_ERR_SYNTAX, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }
    long end = ftell(f);
    if (JSOC_UNLIKELY(end < 0)) {
        fclose(f);
        return (jsoc_result){ .error = { JSOC_ERR_SYNTAX, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }
    rewind(f);

    size_t length = (size_t)end;
    if (JSOC_UNLIKELY(!tokens_fit_u32(length))) {
        fclose(f);
        return (jsoc_result){ .error = { JSOC_ERR_INPUT_TOO_LARGE, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }

    /* Zero-copy tokens point into this buffer, so it must live exactly as
     * long as the arena (and therefore the token array).  Never free it
     * here. */
    char *buf = jsoc_arena_alloc(arena, length + 1, 1);
    if (JSOC_UNLIKELY(!buf)) {
        fclose(f);
        return (jsoc_result){ .error = { JSOC_ERR_NOMEM, 0, 0, 0 }, .tokens = NULL, .count = 0 };
    }
    size_t got = fread(buf, 1, length, f);
    fclose(f);
    buf[got] = '\0';

    return jsoc_tokenize(buf, got, arena);
}

JSOC_API const char *jsoc_decode_error_message(jsoc_decode_error code) {
    switch (code) {
    case JSOC_DECODE_OK: return "Success";
    case JSOC_DECODE_TYPE_MISMATCH: return "Type mismatch";
    case JSOC_DECODE_MISSING_FIELD: return "Missing required field";
    case JSOC_DECODE_EXTRA_FIELD: return "Unexpected field";
    case JSOC_DECODE_OVERFLOW: return "Value overflow";
    case JSOC_DECODE_CUSTOM: return "Custom decode error";
    default: return "Unknown decode error";
    }
}
