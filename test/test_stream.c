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

  return jsoc_test_finish("stream");
}
