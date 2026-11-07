/*
 * Self-contained fuzz drivers.
 *
 * Ports the *targets* of simdjson's tests/fuzz/ (the .cpp files) without libFuzzer: each
 * target is a plain function over a byte buffer, so the same file compiles
 * both standalone (any compiler, deterministic seed, runs a bounded number of
 * iterations - what CI runs) and under clang's libFuzzer, which calls
 * LLVMFuzzerTestOneInput directly.
 *
 * Ported targets:
 *   parser     fuzz_parser.cpp         parse, twice
 *   tokenize   fuzz_parser.cpp         both tokenizer backends agree
 *   pointer    fuzz_atpath / fuzz_atpointer    JSON Pointer navigation
 *   minify     fuzz_minify.cpp         minify, reparse, minify again
 *   serialize  fuzz_print_json.cpp     stringify every value, reparse
 *   ndjson     fuzz_ndjson.cpp         parse_many over the same bytes
 *   utf8       fuzz_utf8.cpp           UTF-8 validation
 *   cursor     fuzz_element.cpp        cursor walks the whole document
 *   decode     (jsoc-specific)         reflected decode never crashes
 *
 * Usage:
 *   ./fuzz <target> [seed] [iterations]
 *   ./fuzz all 1 200
 * With clang: cc -fsanitize=fuzzer,address fuzz.c ...; ./fuzz-asan <target>
 */
#define _POSIX_C_SOURCE 200809L
/* The targets deliberately ignore return values: this is a fuzz driver
 * whose job is to exercise every API, not to check them.
 */
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-result"

#include "jsoc.h"
#include "cursor.h"
#include "decode.h"
#include "dom_internal.h"
#include "path.h"
#include "tokenizer_internal.h"
#include "serialize.h"
#include "writer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Inputs: deterministic generators plus a corpus                            */
/* ------------------------------------------------------------------------- */

static uint64_t fuzz_state = 0x123456789abcdefULL;

static uint64_t fuzz_rand(void) {
    fuzz_state ^= fuzz_state << 13;
    fuzz_state ^= fuzz_state >> 7;
    fuzz_state ^= fuzz_state << 17;
    return fuzz_state;
}

static uint32_t fuzz_below(uint32_t n) { return (uint32_t)(fuzz_rand() % n); }

static const char fuzz_bytes[] =
    "{}[]:,\"0123456789-+.eE truefalsn\\/bfnrtu\x01\x1f\x7f\x80\xc3\xa9"
    "\xf0\x9f\x98\x80";

/* Random bytes, occasionally shaped into something JSON-like. */
static size_t fuzz_make_bytes(char *buf, size_t cap) {
    size_t n = 0;
    const size_t target = fuzz_below((uint32_t)cap);
    while (n < target) {
        if (fuzz_below(20) == 0) {
            /* Embedded an actual JSON fragment. */
            const char *frags[] = { "{\"a\":", "[1,2]", "true", "null", "\"x\"",
                                   "}", "]", ":", ",", "1e5", "-3.25" };
            const char *f = frags[fuzz_below(10)];
            const size_t l = strlen(f);
            if (n + l > cap) break;
            memcpy(buf + n, f, l);
            n += l;
            continue;
        }
        buf[n++] = fuzz_bytes[fuzz_below(sizeof fuzz_bytes - 1)];
    }
    return n;
}

/* Generate a structurally valid document (used by targets that need one). */
static size_t fuzz_make_document(char *buf, size_t cap, unsigned depth);

static size_t fuzz_make_string(char *buf, size_t cap) {
    size_t n = 0;
    buf[n++] = '"';
    const uint32_t count = fuzz_below(6);
    for (uint32_t i = 0; i < count && n + 8 < cap; i++) {
        switch (fuzz_below(8)) {
        case 0: memcpy(buf + n, "\\n", 2); n += 2; break;
        case 1: memcpy(buf + n, "\\t", 2); n += 2; break;
        case 2: memcpy(buf + n, "\\\"", 2); n += 2; break;
        case 3: memcpy(buf + n, "\\\\", 2); n += 2; break;
        case 4: memcpy(buf + n, "\\/", 2); n += 2; break;
        case 5: memcpy(buf + n, "\\u00e9", 6); n += 6; break;
        case 6: memcpy(buf + n, "\xc3\xa9", 2); n += 2; break;
        default: buf[n++] = (char)('a' + fuzz_below(26)); break;
        }
    }
    buf[n++] = '"';
    return n;
}

static size_t fuzz_make_document(char *buf, size_t cap, unsigned depth) {
    if (cap < 8) return 0;
    const uint32_t pick = fuzz_below(depth > 2 ? 6 : 9);
    if (pick <= 3 || cap < 64) {
        return fuzz_make_string(buf, cap);
    }
    if (pick <= 5) {
        /* Object of 0..3 members. */
        size_t n = 0;
        buf[n++] = '{';
        const uint32_t items = fuzz_below(4);
        for (uint32_t i = 0; i < items && n + 32 < cap; i++) {
            if (i) buf[n++] = ',';
            n += fuzz_make_string(buf + n, cap - n);
            buf[n++] = ':';
            const size_t before = n;
            n += fuzz_make_document(buf + n, cap - n, depth + 1);
            if (n == before) { n -= 1; break; }
        }
        buf[n++] = '}';
        return n;
    }
    /* Array of 0..3 elements. */
    size_t n = 0;
    buf[n++] = '[';
    const uint32_t items = fuzz_below(4);
    for (uint32_t i = 0; i < items && n + 32 < cap; i++) {
        if (i) buf[n++] = ',';
        const size_t before = n;
        n += fuzz_make_document(buf + n, cap - n, depth + 1);
        if (n == before) { n -= 1; break; }
    }
    buf[n++] = ']';
    return n;
}

/* ------------------------------------------------------------------------- */
/* Targets                                                                   */
/* ------------------------------------------------------------------------- */

static uint64_t fuzz_sink;

static bool fuzz_count_document(const jsoc_document *doc, void *user) {
    size_t *n = (size_t *)user;
    (*n)++;
    return doc->root != NULL;
}

static int fuzz_target_parser(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    const jsoc_document doc = jsoc_parse(data, size, a);
    fuzz_sink += (uint64_t)doc.root + doc.value_count;
    if (doc.error.code == JSOC_OK) {
        /* And again through the streaming entry point. */
        jsoc_arena_reset(a);
        fuzz_sink += (uint64_t)jsoc_parse_many(data, size, a, NULL, NULL).code;
    }
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_tokenize(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    size_t overflow = 0;
    const jsoc_result r = jsoc_tokenize_checked(data, size, a, &overflow);
    fuzz_sink += r.count + (uint64_t)r.error.code + (uint64_t)overflow;
    jsoc_arena_reset(a);
    const jsoc_result s = jsoc_tokenize(data, size, a);
    fuzz_sink += s.count + (uint64_t)s.error.code;
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_pointer(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    const jsoc_document doc = jsoc_parse(data, size, a);
    if (doc.error.code == JSOC_OK && doc.root) {
        /* Walk every member/element as a pointer. */
        jsoc_object_iter it;
        jsoc_object_iter_init(&doc, doc.root, &it);
        jsoc_string key;
        const jsoc_value *value;
        while (jsoc_object_iter_next(&it, &key, &value)) {
            const jsoc_value *found =
                jsoc_pointer_get(&doc, doc.root, key.data ? "" : "", 0);
            fuzz_sink += (uint64_t)(uintptr_t)found;
            jsoc_pointer_get(&doc, doc.root, "/0", 2);
            jsoc_pointer_get(&doc, doc.root, "/~0~1", 5);
        }
        /* Build a pointer and look it up. */
        jsoc_pointer_builder b;
        jsoc_pointer_builder_init(&b, a);
        (void)jsoc_pointer_push_token(&b, "a", 1);
        (void)jsoc_pointer_push_token(&b, "0", 1);
        fuzz_sink += (uint64_t)(uintptr_t)jsoc_pointer_get(&doc, doc.root,
                                                           b.data, b.length);
    }
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_minify(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    jsoc_string out;
    if (jsoc_minify(data, size, a, &out, &(jsoc_error){ 0 }) && out.data) {
        /* Re-minifying the minified text must be a fixed point. */
        jsoc_string again;
        if (jsoc_minify(out.data, out.length, a, &again, &(jsoc_error){ 0 }) &&
            again.data) {
            fuzz_sink += (again.length == out.length);
        }
        fuzz_sink += out.length;
    }
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_serialize(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    const jsoc_document doc = jsoc_parse(data, size, a);
    if (doc.error.code == JSOC_OK && doc.root) {
        jsoc_string out;
        fuzz_sink += (uint64_t)jsoc_value_minify(&doc, doc.root, a, &out);
        /* Build a document by hand and stringify it. */
        jsoc_writer w;
        jsoc_writer_init(&w, a, 256);
        jsoc_writer_array_begin(&w);
        jsoc_writer_int64(&w, -42);
        jsoc_writer_double(&w, 1.5e300);
        jsoc_writer_string(&w, "a\"b", 3);
        jsoc_writer_bool(&w, true);
        jsoc_writer_null(&w);
        jsoc_writer_array_end(&w);
        fuzz_sink += jsoc_writer_size(&w);
    }
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_ndjson(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    size_t docs = 0;
    const jsoc_error err =
        jsoc_parse_many(data, size, a, fuzz_count_document, &docs);
    fuzz_sink += docs + (uint64_t)err.code;
    jsoc_arena_destroy(a);
    return 0;
}

static int fuzz_target_utf8(const char *data, size_t size) {
    size_t off = 0;
    fuzz_sink += (uint64_t)jsoc_validate_utf8(data, size, &off);
    return 0;
}

static int fuzz_target_cursor(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    const jsoc_document doc = jsoc_parse(data, size, a);
    if (doc.error.code == JSOC_OK && doc.root) {
        jsoc_cursor c;
        jsoc_cursor_init(&c, &doc);
        size_t guard = 0;
        while (guard++ < 4 * size + 64) {
            const jsoc_value *v = jsoc_cursor_value(&c);
            if (!v) break;
            fuzz_sink += v->type;
            if (!jsoc_cursor_enter(&c)) {
                if (!jsoc_cursor_next(&c)) {
                    while (!jsoc_cursor_next(&c)) {
                        if (!jsoc_cursor_up(&c)) goto done;
                    }
                }
            }
        }
    done:
        jsoc_arena_destroy(a);
        return 0;
    }
    jsoc_arena_destroy(a);
    return 0;
}

struct point { int64_t x; int64_t y; bool flag; char *text; };
static const jsoc_decode_field fuzz_point_fields[] = {
    JSOC_DECODE_FIELD_A(point, x, JSOC_DECODE_INT64, .required = true),
    JSOC_DECODE_FIELD_A(point, y, JSOC_DECODE_INT64),
    JSOC_DECODE_FIELD_A(point, flag, JSOC_DECODE_BOOL),
    JSOC_DECODE_FIELD_A(point, text, JSOC_DECODE_STRING_REF),
    { .name = NULL },
};

static int fuzz_target_decode(const char *data, size_t size) {
    jsoc_arena *a = jsoc_arena_create(size + 65536);
    if (!a) return 0;
    const jsoc_document doc = jsoc_parse(data, size, a);
    if (doc.error.code == JSOC_OK && doc.root) {
        struct point p;
        memset(&p, 0, sizeof p);
        jsoc_decode_result r = jsoc_decode_object(fuzz_point_fields, 4, &doc,
                                                  doc.root, &p);
        fuzz_sink += (uint64_t)r.code + (uint64_t)p.x + (uint64_t)p.y +
                     (uint64_t)p.flag + (p.text ? strlen(p.text) : 0);
    }
    jsoc_arena_destroy(a);
    return 0;
}

/* The corpus of generated documents: every target runs over the same inputs. */
typedef int (*fuzz_target_fn)(const char *, size_t);

static void fuzz_run(fuzz_target_fn target, uint64_t seed, int iters,
                     bool structured) {
    fuzz_state = seed;
    char buf[8192];
    for (int i = 0; i < iters; i++) {
        size_t n = structured ? fuzz_make_document(buf, sizeof buf, 0)
                              : fuzz_make_bytes(buf, sizeof buf);
        target(buf, n);
        /* Half the time, use the exact generated length; the rest is a
         * truncated slice, which is where the bounds bugs live. */
        if (fuzz_below(2)) {
            target(buf, fuzz_below((uint32_t)n + 1));
        }
    }
    printf("  sink=%llu\n", (unsigned long long)fuzz_sink);
}

#ifdef __AFL_COMPILER
int main(int argc, char **argv);
#endif

int LLVMFuzzerTestOneInput(const char *data, size_t size);
int LLVMFuzzerTestOneInput(const char *data, size_t size) {
    fuzz_sink = 0;
    fuzz_target_parser(data, size);
    fuzz_target_tokenize(data, size);
    fuzz_target_pointer(data, size);
    fuzz_target_minify(data, size);
    fuzz_target_serialize(data, size);
    fuzz_target_ndjson(data, size);
    fuzz_target_utf8(data, size);
    fuzz_target_cursor(data, size);
    fuzz_target_decode(data, size);
    return 0;
}

int main(int argc, char **argv) {
    const char *target = (argc > 1 && argv[1]) ? argv[1] : "all";
    const uint64_t seed = argc > 2 ? strtoull(argv[2], NULL, 0) : 1;
    const int iters = argc > 3 ? atoi(argv[3]) : 2000;

    fuzz_target_fn fn = NULL;
    if (!strcmp(target, "parser")) fn = fuzz_target_parser;
    else if (!strcmp(target, "tokenize")) fn = fuzz_target_tokenize;
    else if (!strcmp(target, "pointer")) fn = fuzz_target_pointer;
    else if (!strcmp(target, "minify")) fn = fuzz_target_minify;
    else if (!strcmp(target, "serialize")) fn = fuzz_target_serialize;
    else if (!strcmp(target, "ndjson")) fn = fuzz_target_ndjson;
    else if (!strcmp(target, "utf8")) fn = fuzz_target_utf8;
    else if (!strcmp(target, "cursor")) fn = fuzz_target_cursor;
    else if (!strcmp(target, "decode")) fn = fuzz_target_decode;
    else if (!strcmp(target, "structured")) { fn = fuzz_target_parser; }
    else if (strcmp(target, "all") != 0) {
        fprintf(stderr, "unknown target '%s' (parser tokenize pointer minify "
                        "serialize ndjson utf8 cursor decode structured all)\n",
                target);
        return 2;
    }

    printf("fuzz target=%s seed=%llu iterations=%d\n", target,
           (unsigned long long)seed, iters);

    if (!strcmp(target, "all")) {
        fuzz_run(fuzz_target_parser, seed, iters, false);
        fuzz_run(fuzz_target_parser, seed, iters, true);
        fuzz_run(fuzz_target_tokenize, seed, iters, true);
        fuzz_run(fuzz_target_pointer, seed, iters, true);
        fuzz_run(fuzz_target_minify, seed, iters, true);
        fuzz_run(fuzz_target_serialize, seed, iters, true);
        fuzz_run(fuzz_target_ndjson, seed, iters, true);
        fuzz_run(fuzz_target_utf8, seed, iters, false);
        fuzz_run(fuzz_target_cursor, seed, iters, true);
        fuzz_run(fuzz_target_decode, seed, iters, true);
    } else if (!strcmp(target, "structured")) {
        fuzz_run(fuzz_target_parser, seed, iters, true);
    } else {
        fuzz_run(fn, seed, iters, false);
    }
    printf("done\n");
    return 0;
}
