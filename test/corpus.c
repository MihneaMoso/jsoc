/*
 * Corpus runner.
 *
 * Walks one or more files/directories and checks each JSON document against
 * the naming convention used by JSONTestSuite and simdjson-data:
 *
 *   y_*.json / pass*.json   must parse
 *   n_*.json / fail*.json   must be rejected
 *   i_*.json                implementation-defined: not scored
 *   *EXCLUDE*               skipped
 *
 * Every successfully parsed document is additionally walked to verify that
 * the DOM is well formed, that every slice is inside the input, and that the
 * node/member counts are exact.
 */
#define _POSIX_C_SOURCE 200809L
#include "dom_internal.h"
#include "jsoc.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
  size_t files;
  size_t pass_ok;
  size_t fail_ok;
  size_t impl_defined;
  size_t skipped;
  size_t unexpected_pass;
  size_t unexpected_fail;
  size_t broken_dom;
} stats;

typedef enum { EXPECT_DONT_CARE, EXPECT_OK, EXPECT_ERROR } expectation;

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool has_suffix(const char *s, const char *suffix) {
  const size_t n = strlen(s);
  const size_t m = strlen(suffix);
  return n >= m && strcmp(s + n - m, suffix) == 0;
}

static expectation expectation_for(const char *path) {
  const char *name = base_name(path);
  if (strstr(name, "EXCLUDE") != NULL)
    return EXPECT_DONT_CARE;
  if (strncmp(name, "y_", 2) == 0 || strncmp(name, "pass", 4) == 0) {
    return EXPECT_OK;
  }
  if (strncmp(name, "n_", 2) == 0 || strncmp(name, "fail", 4) == 0) {
    return EXPECT_ERROR;
  }
  /* A file with no naming convention at all is scored by the directory it
   * lives in: simdjson's jsonexamples/ holds real-world documents, every one
   * of which must parse, so it defaults to EXPECT_OK there. */
  if (strstr(path, "/jsonexamples/") != NULL ||
      has_suffix(path, "/jsonexamples")) {
    return EXPECT_OK;
  }
  return EXPECT_DONT_CARE;
}

static char *read_file(const char *path, size_t *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  const long end = ftell(f);
  if (end < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  const size_t len = (size_t)end;
  char *buf = malloc(len + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  const size_t got = fread(buf, 1, len, f);
  fclose(f);
  if (got != len) {
    free(buf);
    return NULL;
  }
  buf[len] = '\0';
  *out_len = len;
  return buf;
}

/* Walk the tree, validating indices, counts, and slices. */
static bool walk_value(const jsoc_document *doc, const jsoc_value *v,
                       size_t depth, size_t *value_seen, size_t *member_seen) {
  if (!v || depth > 1024)
    return false;
  if (v < doc->values || v >= doc->values + doc->value_count)
    return false;
  (*value_seen)++;

  const jsoc_value_type type = jsoc_value_type_of(v);
  switch (type) {
  case JSOC_VALUE_NULL:
  case JSOC_VALUE_FALSE:
  case JSOC_VALUE_TRUE:
    return true;

  case JSOC_VALUE_NUMBER:
  case JSOC_VALUE_STRING: {
    jsoc_string raw = jsoc_value_raw(doc, v);
    if (!raw.data || raw.length > doc->length)
      return false;
    if ((size_t)(raw.data - doc->input) > doc->length - raw.length)
      return false;
    if (type == JSOC_VALUE_STRING) {
      jsoc_string decoded = jsoc_value_get_string(doc, v);
      if (!decoded.data)
        return false;
    }
    if (type == JSOC_VALUE_NUMBER) {
      double d;
      /* Valid JSON numbers may exceed the finite double range; those
       * are allowed to fail the getter, but must never crash. */
      (void)jsoc_value_get_double(doc, v, &d);
      int64_t i;
      uint64_t u;
      (void)jsoc_value_get_int64(doc, v, &i);
      (void)jsoc_value_get_uint64(doc, v, &u);
    }
    return true;
  }

  case JSOC_VALUE_ARRAY: {
    if (v->count != 0 && v->first >= doc->value_count)
      return false;
    uint32_t child = v->first;
    for (uint32_t i = 0; i < v->count; i++) {
      if (child >= doc->value_count)
        return false;
      if (!walk_value(doc, &doc->values[child], depth + 1, value_seen,
                      member_seen)) {
        return false;
      }
      child = doc->values[child].next;
    }
    return child == UINT32_MAX;
  }

  case JSOC_VALUE_OBJECT: {
    if (v->count != 0 && v->first >= doc->member_count)
      return false;
    uint32_t m = v->first;
    for (uint32_t i = 0; i < v->count; i++) {
      if (m >= doc->member_count)
        return false;
      const jsoc_member *member = &doc->members[m];
      (*member_seen)++;
      if (member->key_length > doc->length)
        return false;
      if ((size_t)member->key_offset > doc->length - member->key_length) {
        return false;
      }
      if (member->value >= doc->value_count)
        return false;
      if (!walk_value(doc, &doc->values[member->value], depth + 1, value_seen,
                      member_seen)) {
        return false;
      }
      m = member->next;
    }
    return m == UINT32_MAX;
  }

  default:
    return false;
  }
}

static bool validate_dom(const jsoc_document *doc) {
  if (!doc->root || !doc->values)
    return false;
  const size_t root = (size_t)(doc->root - doc->values);
  if (root >= doc->value_count)
    return false;
  size_t values = 0;
  size_t members = 0;
  if (!walk_value(doc, doc->root, 0, &values, &members))
    return false;
  return values == doc->value_count && members == doc->member_count;
}

typedef struct {
  bool broken;
} corpus_callback_state;

static bool corpus_document(const jsoc_document *doc, void *user) {
  corpus_callback_state *state = user;
  if (!validate_dom(doc))
    state->broken = true;
  return true;
}

static void run_file(const char *path, stats *st) {
  st->files++;
  size_t len = 0;
  char *text = read_file(path, &len);
  if (!text) {
    fprintf(stderr, "cannot read %s: %s\n", path, strerror(errno));
    st->broken_dom++;
    return;
  }

  jsoc_arena *arena = jsoc_arena_create(len / 2 + 4096);
  if (!arena) {
    free(text);
    st->broken_dom++;
    return;
  }

  const expectation want = expectation_for(path);
  jsoc_error error;
  if (has_suffix(path, ".ndjson")) {
    corpus_callback_state state = {false};
    error = jsoc_parse_many(text, len, arena, corpus_document, &state);
    if (state.broken) {
      fprintf(stderr, "BROKEN DOM: %s\n", path);
      st->broken_dom++;
    }
  } else {
    jsoc_document doc = jsoc_parse(text, len, arena);
    error = doc.error;
    if (error.code == JSOC_OK && !validate_dom(&doc)) {
      fprintf(stderr, "BROKEN DOM: %s\n", path);
      st->broken_dom++;
    }
  }

  switch (want) {
  case EXPECT_OK:
    if (error.code != JSOC_OK) {
      fprintf(stderr, "SHOULD PASS: %s (error %s at %zu)\n", path,
              jsoc_error_message(error.code), error.offset);
      st->unexpected_fail++;
    } else {
      st->pass_ok++;
    }
    break;
  case EXPECT_ERROR:
    if (error.code == JSOC_OK) {
      fprintf(stderr, "SHOULD FAIL: %s\n", path);
      st->unexpected_pass++;
    } else {
      st->fail_ok++;
    }
    break;
  case EXPECT_DONT_CARE:
    if (strstr(base_name(path), "EXCLUDE") != NULL) {
      st->skipped++;
    } else {
      st->impl_defined++;
    }
    break;
  }

  jsoc_arena_destroy(arena);
  free(text);
}

static void run_path(const char *path, stats *st) {
  struct stat s;
  if (stat(path, &s) != 0) {
    fprintf(stderr, "cannot stat %s: %s\n", path, strerror(errno));
    return;
  }
  if (S_ISREG(s.st_mode)) {
    if (has_suffix(path, ".json") || has_suffix(path, ".ndjson")) {
      run_file(path, st);
    }
    return;
  }
  if (!S_ISDIR(s.st_mode))
    return;

  DIR *dir = opendir(path);
  if (!dir) {
    fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
    return;
  }
  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    const size_t need = strlen(path) + strlen(ent->d_name) + 2;
    char *child = malloc(need);
    if (!child)
      continue;
    snprintf(child, need, "%s/%s", path, ent->d_name);
    run_path(child, st);
    free(child);
  }
  closedir(dir);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <file-or-directory>...\n", argv[0]);
    return 2;
  }

  stats st = {0};
  for (int i = 1; i < argc; i++)
    run_path(argv[i], &st);

  printf("\nfiles=%zu pass_ok=%zu fail_ok=%zu impl_defined=%zu skipped=%zu\n",
         st.files, st.pass_ok, st.fail_ok, st.impl_defined, st.skipped);
  printf("unexpected_pass=%zu unexpected_fail=%zu broken_dom=%zu\n",
         st.unexpected_pass, st.unexpected_fail, st.broken_dom);

  return (st.unexpected_pass || st.unexpected_fail || st.broken_dom) ? 1 : 0;
}
