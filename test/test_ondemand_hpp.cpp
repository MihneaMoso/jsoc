/* Correctness tests for the C++26 compile-time-dispatched on-demand decoder. */
#include "ondemand.hpp"
#include "arena.h"

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
typedef struct odh_user {
  uint64_t id;
  jsoc_string name;
  bool verified;
  uint64_t followers_count;
} odh_user;
typedef struct odh_status {
  jsoc_string created_at;
  uint64_t id;
  jsoc_string text;
  odh_user user;
  uint64_t retweet_count;
} odh_status;
typedef struct odh_twitter {
  odh_status *statuses;
  size_t status_count;
} odh_twitter;
}

static int failures = 0;
static int checks = 0;
#define CHECK(c)                                                               \
  do {                                                                         \
    checks++;                                                                  \
    if (!(c)) {                                                                \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c);                 \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static bool decode(const char *json, odh_twitter &out, jsoc_arena *a) {
  jsoc_ondemand od;
  jsoc_error err;
  if (!jsoc_ondemand_scan(&od, json, std::strlen(json), a, &err)) return false;
  return jsoc::od::decode(od, out, a);
}

static bool eq(jsoc_string s, const char *lit) {
  return s.length == std::strlen(lit) && std::memcmp(s.data, lit, s.length) == 0;
}

int main() {
  jsoc_arena *a = jsoc_arena_create(1 << 16);
  CHECK(a != nullptr);
  if (!a) return 1;

  {
    static odh_status storage[4];
    odh_twitter t{storage, 4};
    CHECK(decode("{\"statuses\":[{\"created_at\":\"now\",\"id\":42,"
                 "\"text\":\"hi\",\"user\":{\"id\":7,\"name\":\"Ada\","
                 "\"verified\":true,\"followers_count\":99},"
                 "\"retweet_count\":3}]}", t, a));
    CHECK(t.status_count == 1);
    CHECK(t.statuses[0].id == 42);
    CHECK(eq(t.statuses[0].text, "hi"));
    CHECK(t.statuses[0].user.id == 7);
    CHECK(eq(t.statuses[0].user.name, "Ada"));
    CHECK(t.statuses[0].user.verified == true);
    CHECK(t.statuses[0].user.followers_count == 99);
    CHECK(t.statuses[0].retweet_count == 3);
  }
  jsoc_arena_reset(a);

  {
    /* Escapes are decoded; unknown fields (including containers) are skipped. */
    static odh_status storage[4];
    odh_twitter t{storage, 4};
    CHECK(decode("{\"other\":[1,{\"x\":2}],\"statuses\":[{"
                 "\"created_at\":\"x\",\"id\":1,"
                 "\"text\":\"a\\tb\\u263a\",\"junk\":{\"k\":[1,2]},"
                 "\"user\":{\"id\":2,\"name\":\"n\",\"verified\":false,"
                 "\"followers_count\":0},\"retweet_count\":0}]}", t, a));
    CHECK(t.status_count == 1);
    CHECK(eq(t.statuses[0].text, "a\tb\xE2\x98\xBA"));
    CHECK(t.statuses[0].user.verified == false);
  }
  jsoc_arena_reset(a);

  jsoc_arena_destroy(a);
  std::printf("=== ondemand_hpp: %d/%d passed (%d failed) ===\n",
              checks - failures, checks, failures);
  return failures == 0 ? 0 : 1;
}
