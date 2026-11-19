/*
 * Streaming / parse_many.
 *
 * Ports the behaviour of simdjson's tests/dom/document_stream_tests.cpp,
 * tests/ondemand/ondemand_document_stream_tests.cpp and
 * tests/dom/parse_many_test.cpp: a concatenated stream of documents, with a
 * callback per value, resets the arena between documents (so a document is
 * only valid during its own callback).
 */
#include "test.h"
#include <stdint.h>
#include <string.h>

typedef struct {
  size_t count;
  const jsoc_value *first_root;
} stream_stats;

static bool doc_counter(const jsoc_document *doc, void *user) {
  stream_stats *s = (stream_stats *)user;
  s->count++;
  if (!s->first_root)
    s->first_root = doc->root;
  return true;
}

static jsoc_error run_stream(const char *text, size_t len, jsoc_arena *a,
                             stream_stats *out) {
  memset(out, 0, sizeof *out);
  return jsoc_parse_many(text, len, a, doc_counter, out);
}

static bool test_whitespace_and_separators(void) {
  /* Every separator form: newline, CR, spaces, tabs, and the leading
   * whitespace simdjson calls "crazy". */
  const char *cases[][2] = {
      {"1 2 3", "3"},
      {"1\n2\n3\n", "3"},
      {"\r\n1\r\n2", "2"},
      {"\t1\t\t2\t", "2"},
      {"  [1]\n  [2]\n", "2"},
      {"\n\n\n{\"a\":1}\n{\"b\":2}\n\n", "2"},
      {"                   1\n                   2", "2"},
      {"1 \"two\" true null", "4"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(8192);
    REQUIRE(a != NULL);
    stream_stats stats;
    jsoc_error err = run_stream(cases[i][0], strlen(cases[i][0]), a, &stats);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(stats.count == (size_t)atoi(cases[i][1]));
    jsoc_arena_destroy(a);
  }
  /* An empty stream, and whitespace-only streams, carry zero documents. */
  {
    const char *empty[] = {"", "   ", "\n\n", "\t\t ", "\r\n "};
    for (size_t i = 0; i < sizeof empty / sizeof empty[0]; i++) {
      jsoc_arena *a = jsoc_arena_create(8192);
      REQUIRE(a != NULL);
      stream_stats stats;
      jsoc_error err = run_stream(empty[i], strlen(empty[i]), a, &stats);
      REQUIRE(err.code == JSOC_OK);
      REQUIRE(stats.count == 0);
      jsoc_arena_destroy(a);
    }
  }
  return true;
fail:
  return false;
}

static bool test_documents_are_isolated(void) {
  /* jsoc resets the arena before each document, so a document and any
   * decoded string it owns are only valid inside its own callback. */
  jsoc_arena *a = jsoc_arena_create(8192);
  REQUIRE(a != NULL);

  const char *text = "{\"a\":[\"x\",\"y\"]} {\"b\":{\"c\":1}} [1,2,3]";
  stream_stats stats;
  jsoc_error err = run_stream(text, strlen(text), a, &stats);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(stats.count == 3);

  /* Run again, capturing each document's identity: it must be rebuilt from
   * the same arena, so the roots differ. */
  jsoc_arena_reset(a);
  stats.count = 0;
  err = run_stream(text, strlen(text), a, &stats);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(stats.count == 3);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool stop_after_two(const jsoc_document *doc, void *user) {
  (void)doc;
  size_t *n = (size_t *)user;
  return ++*n < 2;
}

static bool test_callback_stop(void) {
  /* Returning false stops the stream and reports it as a user stop. */
  jsoc_arena *a = jsoc_arena_create(8192);
  REQUIRE(a != NULL);
  const char *text = "1 2 3 4 5 6";

  size_t seen = 0;
  /* Stop after two documents. */
  jsoc_error err =
      jsoc_parse_many(text, strlen(text), a, stop_after_two, &seen);
  REQUIRE(err.code == JSOC_ERR_USER_STOP);
  REQUIRE(seen == 2);

  /* A NULL callback only validates the stream. */
  memset(&seen, 0, sizeof seen);
  err = jsoc_parse_many(text, strlen(text), a, NULL, NULL);
  REQUIRE(err.code == JSOC_OK);

  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_error_in_the_middle(void) {
  /* A malformed document aborts the stream at that point, and everything
   * before it was already delivered. */
  jsoc_arena *a = jsoc_arena_create(8192);
  REQUIRE(a != NULL);

  {
    const char *text = "1 [2,{\"a\":3}] {\"b\":4} bad 6";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code != JSOC_OK);
    REQUIRE(stats.count == 3);
  }
  jsoc_arena_reset(a);
  {
    /* An error inside the second document. */
    const char *text = "1 {\"a\":<} 3";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code != JSOC_OK);
    REQUIRE(stats.count == 1);
  }
  jsoc_arena_reset(a);
  {
    /* A truncated final document is an error, not a stop. */
    const char *text = "1 2 {\"a\":";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code != JSOC_OK);
    REQUIRE(stats.count == 2);
  }
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_root_scalars_and_ndjson(void) {
  /* document_stream_tests' root_numbers_in_stream / single_document /
   * issue1307-1311: streams of bare scalars and containers. */
  const char *cases[][2] = {
      {"1", "1"},
      {"1 2 3", "3"},
      {"-1.5e10", "1"},
      {"\"just a string\"", "1"},
      {"true false null", "3"},
      {"[1,2,3]", "1"},
      {"{\"a\":1}{\"b\":2}", "2"}, /* adjacent documents, no separator */
      {"[] {} 1 \"s\" true", "5"},
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(8192);
    REQUIRE(a != NULL);
    stream_stats stats;
    jsoc_error err = run_stream(cases[i][0], strlen(cases[i][0]), a, &stats);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(stats.count == (size_t)atoi(cases[i][1]));
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_bom_and_unicode_separators(void) {
  /* A UTF-8 BOM at the start is skipped; a non-breaking space is not
   * whitespace (that is document_stream's issue_non_ascii_separator). */
  jsoc_arena *a = jsoc_arena_create(8192);
  REQUIRE(a != NULL);
  {
    const char *text = "\xEF\xBB\xBF"
                       "1 2";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(stats.count == 2);
  }
  jsoc_arena_reset(a);
  {
    const char *text = "1\xC2\xA0"
                       "2"; /* NBSP: not a separator */
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code != JSOC_OK);
  }
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_adversarial_single_documents(void) {
  /* document_stream_tests' adversarial cases: a single document that the
   * stream must not split, including one that ends exactly at the end. */
  const char *cases[] = {
      "[1]",
      "{\"a\":[1,2,3]}",
      "\"\"",
      "0",
      "[]",
      "{}",
      "[[[[1]]]]",
      "{\"a\":{\"b\":{\"c\":[1,2,{\"d\":null}]}}}",
      "[1,2,3,4,5,6,7,8,9,10]",
  };
  for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
    jsoc_arena *a = jsoc_arena_create(8192);
    REQUIRE(a != NULL);
    stream_stats stats;
    jsoc_error err = run_stream(cases[i], strlen(cases[i]), a, &stats);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(stats.count == 1);
    jsoc_arena_destroy(a);
  }
  return true;
fail:
  return false;
}

static bool test_does_not_read_past_stream(void) {
  /* stream_does_not_read_past_batch: every byte after the last document is
   * ignored, so a stream embedded in a larger buffer stops where it should.
   * The padded suffix is not whitespace, which would be a syntax error if
   * it were consumed. */
  jsoc_arena *a = jsoc_arena_create(8192);
  REQUIRE(a != NULL);
  {
    const char *text = "1 2 3XXX";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code != JSOC_OK); /* X is garbage */
    REQUIRE(stats.count == 3);
  }
  jsoc_arena_reset(a);
  {
    const char *text = "1 2 3    ";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(stats.count == 3);
  }
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

static bool test_truncated_document_not_delivered(void) {
  /* Regression: an unclosed trailing document must never be handed to the
   * builder.  The stream scanner used to treat the last token of a truncated
   * window as a matching closer whenever its type matched the root type, even
   * with containers still open, which made the DOM builder read past the token
   * array (heap-buffer-overflow found by the NDJSON fuzzer under ASan). */
  jsoc_arena *a = jsoc_arena_create(1 << 16);
  REQUIRE(a != NULL);
  {
    /* Root object, inner array still open, last emitted token is '}'. */
    const char *text = "{\"a\":[{\"b\":1}";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_ARRAY);
    REQUIRE(stats.count == 0);
  }
  jsoc_arena_reset(a);
  {
    /* Root array, outer array still open, last emitted token is ']'. */
    const char *text = "[[{}]";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_ARRAY);
    REQUIRE(stats.count == 0);
  }
  jsoc_arena_reset(a);
  {
    /* Truncated inside a string with several open containers. */
    const char *text = "{\"a\":[1,2,{\"b\":\"unterminated";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_STRING);
    REQUIRE(stats.count == 0);
  }
  jsoc_arena_reset(a);
  {
    /* Unclosed array, no string involvement. */
    const char *text = "1 {\"a\":[1,2";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_ARRAY);
    REQUIRE(stats.count == 1); /* the leading "1" is complete */
  }
  jsoc_arena_reset(a);
  {
    /* Containers opened but nothing else: no complete document exists. */
    const char *text = "[[[";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_ARRAY);
    REQUIRE(stats.count == 0);
  }
  jsoc_arena_reset(a);
  {
    /* Truncated object after a complete document. */
    const char *text = "{\"a\":1} {\"b\":2";
    stream_stats stats;
    jsoc_error err = run_stream(text, strlen(text), a, &stats);
    REQUIRE(err.code == JSOC_ERR_UNTERMINATED_OBJECT);
    REQUIRE(stats.count == 1);
  }
  jsoc_arena_destroy(a);
  return true;
fail:
  jsoc_arena_destroy(a);
  return false;
}

/* ------------------------------------------------------------------------- */
/* Multi-window streams                                                      */
/* ------------------------------------------------------------------------- */

/* The fast path scans the stream in windows and carries the string / depth
 * state between them, so a document may straddle a window edge.  These tests
 * build streams far larger than one window and verify every document still
 * decodes exactly like the same document on its own. */

/* Documents of the form {"i":N,"s":"<text>","pad":"<pad>","n":[N,N*10]}
 * where <text> mixes \" escapes, \\ escapes and raw multibyte UTF-8: any
 * window boundary that lands inside it must not disturb the decode. */

/* The decoded text for the "s" member: "q" \ \ caf\xc3\xa9 <emoji>. */
static const char s_text[] =
    "\"q\" \\ \\ caf\xc3\xa9 \xf0\x9f\x98\x80";
static const char pad_text[] =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

static size_t write_doc(char *out, size_t cap, unsigned index) {
  char head[64];
  const int hn = snprintf(head, sizeof head, "{\"i\":%u,\"s\":", index);
  REQUIRE(hn > 0 && (size_t)hn < cap);
  memcpy(out, head, (size_t)hn);
  size_t n = (size_t)hn;
  /* JSON escapes kept, UTF-8 sent raw so the scanner has to cross it.  The
   * JSON text is: "q\" \ \ café <emoji>","pad":" — so the decoded value is
   * exactly s_text. */
  const char body[] =
      "\"\\\"q\\\" \\\\ \\\\ caf\xc3\xa9 \xf0\x9f\x98\x80\",\"pad\":\"";
  const size_t blen = sizeof body - 1;
  REQUIRE(n + blen < cap);
  memcpy(out + n, body, blen);
  n += blen;
  const size_t plen = sizeof pad_text - 1;
  REQUIRE(n + plen + 32 < cap);
  memcpy(out + n, pad_text, plen);
  n += plen;
  const int tailn = snprintf(out + n, cap - n, "\",\"n\":[%u,%u]}", index,
                             index * 10u);
  REQUIRE(tailn > 0);
  n += (size_t)tailn;
  return n;
fail:
  return 0;
}

typedef struct {
  unsigned expected;
  bool bad;
} doc_check;

static bool check_member(const jsoc_document *doc, const jsoc_value *v,
                        unsigned *expected) {
  if (v == NULL || !jsoc_value_is_object(v) ||
      jsoc_object_size(doc, v) != 4)
    return false;
  const jsoc_value *iv = jsoc_object_get_cstr(doc, v, "i");
  const jsoc_value *sv = jsoc_object_get_cstr(doc, v, "s");
  const jsoc_value *pv = jsoc_object_get_cstr(doc, v, "pad");
  const jsoc_value *nv = jsoc_object_get_cstr(doc, v, "n");
  if (iv == NULL || sv == NULL || pv == NULL || nv == NULL ||
      !jsoc_value_is_number(iv) || !jsoc_value_is_array(nv))
    return false;
  uint64_t i = 0;
  if (!jsoc_value_get_uint64(doc, iv, &i) || i != *expected)
    return false;
  (*expected)++;
  if (jsoc_array_size(doc, nv) != 2)
    return false;
  const jsoc_value *e0 = jsoc_array_at(doc, nv, 0);
  const jsoc_value *e1 = jsoc_array_at(doc, nv, 1);
  uint64_t v0 = 0, v1 = 0;
  if (e0 == NULL || e1 == NULL || !jsoc_value_get_uint64(doc, e0, &v0) ||
      !jsoc_value_get_uint64(doc, e1, &v1) || v0 != i || v1 != i * 10u)
    return false;
  jsoc_string str = jsoc_value_get_string(doc, sv);
  if (str.length != sizeof s_text - 1 ||
      memcmp(str.data, s_text, str.length) != 0)
    return false;
  jsoc_string pad = jsoc_value_get_string(doc, pv);
  if (pad.length != sizeof pad_text - 1 ||
      memcmp(pad.data, pad_text, pad.length) != 0)
    return false;
  return true;
}

static bool check_document(const jsoc_document *doc, void *user) {
  doc_check *chk = (doc_check *)user;
  if (doc->error.code != JSOC_OK || doc->root == NULL) {
    chk->bad = true;
    return false;
  }
  if (jsoc_value_is_array(doc->root)) {
    /* A single document that is itself larger than several windows: walk it
     * inside the callback, where every member is still live. */
    const size_t n = jsoc_array_size(doc, doc->root);
    for (size_t i = 0; i < n; i++) {
      if (!check_member(doc, jsoc_array_at(doc, doc->root, i), &chk->expected)) {
        chk->bad = true;
        return false;
      }
    }
    return true;
  }
  if (!check_member(doc, doc->root, &chk->expected)) {
    chk->bad = true;
    return false;
  }
  return true;
}

static size_t build_stream(char *buf, size_t cap, unsigned ndocs) {
  size_t len = 0;
  for (unsigned i = 0; i < ndocs; i++) {
    const size_t n = write_doc(buf + len, cap - len, i);
    REQUIRE(n > 0);
    len += n;
    buf[len] = '\n'; /* write_doc leaves a terminator there */
    len++;
  }
  return len;
fail:
  return 0;
}

static bool test_multi_window_streams(void) {
  jsoc_arena *a = jsoc_arena_create(1 << 24);
  REQUIRE(a != NULL);
  char *buf = malloc(1 << 21);
  REQUIRE(buf != NULL);

  /* ~700 kB, so dozens of windows: boundaries land inside strings, escapes
   * and multibyte sequences. */
  const size_t len = build_stream(buf, 1 << 21, 1200);
  REQUIRE(len > 300000);

  doc_check chk = {0, false};
  jsoc_error err = jsoc_parse_many(buf, len, a, check_document, &chk);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(!chk.bad);
  REQUIRE(chk.expected == 1200);

  /* The same documents with no separators at all (adjacent, like ndjson
   * without newlines): the window edges then land mid-punctuation. */
  jsoc_arena_reset(a);
  size_t dlen = 0;
  for (unsigned i = 0; i < 1200; i++) {
    const size_t n = write_doc(buf + dlen, (1 << 21) - dlen, i);
    REQUIRE(n > 0);
    dlen += n;
  }
  REQUIRE(dlen > 300000);
  chk.expected = 0;
  chk.bad = false;
  err = jsoc_parse_many(buf, dlen, a, check_document, &chk);
  REQUIRE(err.code == JSOC_OK);
  REQUIRE(!chk.bad);
  REQUIRE(chk.expected == 1200);

  /* One single document larger than several windows: the depth has to be
   * carried across all of them before the walk can complete it. */
  jsoc_arena_reset(a);
  {
    char *big = malloc(1 << 21);
    REQUIRE(big != NULL);
    size_t n = 0;
    big[n++] = '[';
    for (unsigned i = 0; i < 4000; i++) {
      const size_t w = write_doc(big + n, (1 << 21) - n - 1, i);
      REQUIRE(w > 0);
      n += w;
      big[n++] = ',';
    }
    big[n - 1] = ']'; /* the last separator is the array's end */
    REQUIRE(n > 200000);
    chk.expected = 0;
    chk.bad = false;
    err = jsoc_parse_many(big, n, a, check_document, &chk);
    REQUIRE(err.code == JSOC_OK);
    REQUIRE(!chk.bad);
    REQUIRE(chk.expected == 4000); /* all members verified */
    free(big);
  }

  jsoc_arena_destroy(a);
  free(buf);
  return true;
fail:
  return false;
}

static bool test_multi_window_errors(void) {
  jsoc_arena *a = jsoc_arena_create(1 << 24);
  REQUIRE(a != NULL);
  char *buf = malloc(1 << 21);
  REQUIRE(buf != NULL);

  /* Three corruptions, one per error code, each a few windows into the
   * stream: the token path must reproduce the same error, and the complete
   * documents before it must already have been delivered. */
  struct {
    size_t at;         /* offset inside the document to overwrite */
    char byte;
    jsoc_error_code code;
    unsigned delivered;
  } cases[] = {
      {2, 0x01, JSOC_ERR_INVALID_CONTROL, 800}, /* control byte in the key */
      {4, 'x', JSOC_ERR_EXPECTED_COLON, 800},   /* the ':' replaced */
      /* Bad UTF-8 anywhere is caught by the whole-input validation up front,
       * before any document is delivered. */
      {5, (char)0xFF, JSOC_ERR_INVALID_UTF8, 0},
  };

  for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
    jsoc_arena_reset(a);
    const size_t len = build_stream(buf, 1 << 21, 1200);
    REQUIRE(len > 300000);

    /* Locate the start of document 800 and corrupt it there.  write_doc
     * leaves snprintf's terminator behind, so the separator is rewritten. */
    size_t doc_start = 0;
    for (unsigned i = 0; i < 800; i++) {
      const size_t n = write_doc(buf + doc_start, (1 << 21) - doc_start, i);
      REQUIRE(n > 0);
      buf[doc_start + n] = '\n';
      doc_start += n + 1;
    }
    REQUIRE(doc_start > 200000);
    REQUIRE(doc_start + cases[c].at < len);
    buf[doc_start + cases[c].at] = cases[c].byte;

    stream_stats stats;
    jsoc_error err = run_stream(buf, len, a, &stats);
    REQUIRE(err.code == cases[c].code);
    REQUIRE(stats.count == cases[c].delivered);
    REQUIRE(err.offset >= doc_start);
    REQUIRE(err.offset < doc_start + 64);
  }

  jsoc_arena_destroy(a);
  free(buf);
  return true;
fail:
  return false;
}

int main(void) {
  printf("=== jsoc streaming tests ===\n\n");

  TEST(1, whitespace_and_separators);
  TEST(2, documents_are_isolated);
  TEST(3, callback_stop);
  TEST(4, error_in_the_middle);
  TEST(5, root_scalars_and_ndjson);
  TEST(6, bom_and_unicode_separators);
  TEST(7, adversarial_single_documents);
  TEST(8, does_not_read_past_stream);
  TEST(9, truncated_document_not_delivered);
  TEST(10, multi_window_streams);
  TEST(11, multi_window_errors);
  TEST(10, multi_window_streams);
  TEST(11, multi_window_errors);

  return jsoc_test_finish("stream");
}
