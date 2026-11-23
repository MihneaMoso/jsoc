/*
 * Extended JSON benchmark suite: jsoc vs simdjson.
 *
 * This harness adds benchmark shapes from the two most widely used JSON
 * benchmark suites to the basic parse/serialize comparison in compare.sh:
 *
 *   stats     nativejson-benchmark "Statistics": traverse the DOM and count
 *             value types, total string bytes and container elements.
 *             Source: miloyip/nativejson-benchmark (2.1k stars).
 *
 *   distinct  simdjson "distinct_user_id": collect all user IDs from the
 *             statuses array, sort and de-duplicate.
 *             Source: simdjson/benchmark/distinct_user_id.
 *
 *   find      simdjson "find_tweet": scan the statuses array for a given id
 *             and return its text.  Source: simdjson/benchmark/find_tweet.
 *
 *   numbers   simdjson "number accessors": build a 1,000,000-element integer
 *             array and sum it through the number accessor.
 *             Source: simdjson/benchmark/bench_number_accessors.cpp.
 *
 * Each mode runs both libraries in the same process over the same buffer with
 * alternating phase order and keeps the best wall-clock time, matching the
 * protocol of the other harnesses.  Build with:
 *
 *   g++ -std=c++26 -O3 -march=native -DNDEBUG -Iinclude \
 *       -I$HOME/simdjson/include -o build/compare_extended \
 *       bench/compare_extended.cpp build/libjsoc.a \
 *       $HOME/simdjson/build/libsimdjson.a -pthread
 *
 * Run with BENCH_MODE=stats,distinct,find,numbers and BENCH_ITERS=N.
 */

#include "simdjson.h"

extern "C" {
#include "jsoc.h"
#include "arena.h"
}
#include "dom_internal.h" /* JSOC_NO_INDEX, jsoc_value/member layout */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

static double now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

static bool read_file(const char *path, std::vector<char> &out, size_t *length) {
  FILE *f = std::fopen(path, "rb");
  if (!f) return false;
  if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
  long n = std::ftell(f);
  if (n < 0) { std::fclose(f); return false; }
  std::rewind(f);
  out.assign((size_t)n + simdjson::SIMDJSON_PADDING + 1, '\0');
  size_t got = std::fread(out.data(), 1, (size_t)n, f);
  std::fclose(f);
  std::memset(out.data() + got, 0, simdjson::SIMDJSON_PADDING);
  *length = got;
  return true;
}

static const char *base_name(const char *path) {
  const char *s = std::strrchr(path, '/');
  return s ? s + 1 : path;
}

struct Config {
  int iters;
  int settle;
  bool jsoc_first;
};

static Config config(size_t length, int index) {
  Config c;
  if (const char *env = std::getenv("BENCH_ITERS")) {
    c.iters = std::atoi(env);
  } else {
    double n = 150e6 / (double)(length ? length : 1);
    if (n < 20.0) n = 20.0;
    if (n > 400.0) n = 400.0;
    c.iters = (int)n;
  }
  c.settle = 2;
  c.jsoc_first = (index & 1) != 0;
  return c;
}

/* Alternate phases in blocks to cancel slow drift; keep the best time. */
template <class JF, class SF>
static void time_pair(JF &&jsoc_fn, SF &&sj_fn, const Config &cfg,
                      double *best_j, double *best_s) {
  *best_j = *best_s = 0.0;
  const int block = cfg.iters;
  for (int phase = 0; phase < 2; phase++) {
    bool run_jsoc = (phase == 0) == cfg.jsoc_first;
    for (int k = -cfg.settle; k < block; k++) {
      if (run_jsoc) {
        double t0 = now();
        jsoc_fn();
        double dt = now() - t0;
        if (k >= 0 && (*best_j == 0.0 || dt < *best_j)) *best_j = dt;
      } else {
        double t0 = now();
        sj_fn();
        double dt = now() - t0;
        if (k >= 0 && (*best_s == 0.0 || dt < *best_s)) *best_s = dt;
      }
    }
  }
}

struct Sample {
  const char *path;
  size_t bytes;
  size_t units;
  double jt, st;
};

static void print_sample(const Sample &s, const char *unit) {
  double jg = ((double)s.bytes / 1e9) / s.jt;
  double sg = ((double)s.bytes / 1e9) / s.st;
  std::printf("%-30s %9.2f %9.2f %8.3fx  (%s)\n", base_name(s.path), jg, sg,
              s.st / s.jt, unit);
  std::fflush(stdout);
}

static void summary(const char *title, const std::vector<Sample> &v) {
  if (v.empty()) return;
  double logs = 0.0, total_j = 0.0, total_s = 0.0, bytes = 0.0;
  size_t wins = 0, losses = 0;
  for (const Sample &s : v) {
    double sp = s.st / s.jt;
    logs += std::log(sp);
    total_j += s.jt;
    total_s += s.st;
    bytes += s.bytes;
    if (sp > 1.005) wins++;
    else if (sp < 0.995) losses++;
  }
  std::printf("  %-26s  geo %6.3fx  aggregate jsoc %6.2f / sj %6.2f GB/s  %zuW/%zuL\n",
              title, std::exp(logs / (double)v.size()),
              bytes / total_j / 1e9, bytes / total_s / 1e9, wins, losses);
}

/* ==================================================================== */
/* stats: DOM traversal (nativejson-benchmark "Statistics")              */
/* ==================================================================== */

struct Stats {
  uint64_t nulls = 0, bools = 0, numbers = 0, strings = 0;
  uint64_t arrays = 0, objects = 0, string_bytes = 0, elements = 0;
};

static void jsoc_walk(const jsoc_document *doc, const jsoc_value *v, Stats &s) {
  switch (jsoc_value_type_of(v)) {
  case JSOC_VALUE_NULL: s.nulls++; return;
  case JSOC_VALUE_TRUE:
  case JSOC_VALUE_FALSE: s.bools++; return;
  case JSOC_VALUE_NUMBER: s.numbers++; return;
  case JSOC_VALUE_STRING:
    s.strings++;
    s.string_bytes += jsoc_value_get_string(doc, v).length;
    return;
  case JSOC_VALUE_ARRAY:
    s.arrays++;
    s.elements += v->count;
    /* jsoc's DOM stores children in a linked list: walk it linearly rather
     * than calling the O(index) jsoc_array_at in a loop. */
    for (uint32_t c = v->first; c != JSOC_NO_INDEX;
         c = doc->values[c].next)
      jsoc_walk(doc, &doc->values[c], s);
    return;
  case JSOC_VALUE_OBJECT: {
    s.objects++;
    s.elements += v->count;
    for (uint32_t m = v->first; m != JSOC_NO_INDEX;
         m = doc->members[m].next)
      jsoc_walk(doc, &doc->values[doc->members[m].value], s);
    return;
  }
  }
}

static void sj_walk(simdjson::dom::element e, Stats &s) {
  using simdjson::dom::element_type;
  switch (e.type()) {
  case element_type::NULL_VALUE: s.nulls++; return;
  case element_type::BOOL: s.bools++; return;
  case element_type::INT64:
  case element_type::UINT64:
  case element_type::DOUBLE: s.numbers++; return;
  case element_type::STRING:
    s.strings++;
    {
      std::string_view str;
      if (!e.get_string().get(str)) s.string_bytes += str.size();
    }
    return;
  case element_type::ARRAY:
    s.arrays++;
    {
      simdjson::dom::array arr;
      if (e.get_array().get(arr)) return;
      s.elements += arr.size();
      for (auto child : arr) sj_walk(child, s);
    }
    return;
  case element_type::OBJECT:
    s.objects++;
    {
      simdjson::dom::object obj;
      if (e.get_object().get(obj)) return;
      s.elements += obj.size();
      for (auto kv : obj) sj_walk(kv.value, s);
    }
    return;
  }
}

static std::vector<Sample> run_stats(int argc, char **argv, int first) {
  std::vector<Sample> out;
  for (int arg = first; arg < argc; arg++) {
    std::vector<char> buf;
    size_t len = 0;
    if (!read_file(argv[arg], buf, &len) || len == 0) continue;
    jsoc_arena *a = jsoc_arena_create(len + 65536);
    jsoc_document jd = jsoc_parse(buf.data(), len, a);
    simdjson::dom::parser p;
    auto sjdoc = p.parse(buf.data(), len);
    if (jd.error.code != JSOC_OK || sjdoc.error()) {
      std::fprintf(stderr, "%s: parse failed\n", argv[arg]);
      jsoc_arena_destroy(a);
      continue;
    }
    Stats js, ss;
    jsoc_walk(&jd, jd.root, js);
    sj_walk(sjdoc.value(), ss);
    if (js.numbers != ss.numbers || js.strings != ss.strings ||
        js.string_bytes != ss.string_bytes || js.elements != ss.elements) {
      std::fprintf(stderr, "%s: stats mismatch jsoc(%llu/%llu/%llu/%llu) sj(%llu/%llu/%llu/%llu)\n",
                   argv[arg], (unsigned long long)js.numbers,
                   (unsigned long long)js.strings,
                   (unsigned long long)js.string_bytes,
                   (unsigned long long)js.elements,
                   (unsigned long long)ss.numbers,
                   (unsigned long long)ss.strings,
                   (unsigned long long)ss.string_bytes,
                   (unsigned long long)ss.elements);
    }
    Config cfg = config(len, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          Stats s;
          jsoc_walk(&jd, jd.root, s);
          if (s.elements == 0) std::abort();
        },
        [&] {
          Stats s;
          sj_walk(sjdoc.value(), s);
          if (s.elements == 0) std::abort();
        }, cfg, &jt, &st);
    out.push_back({argv[arg], len, js.elements, jt, st});
    print_sample(out.back(), "input GB/s (traverse)");
    jsoc_arena_destroy(a);
  }
  return out;
}

/* ==================================================================== */
/* distinct: simdjson distinct_user_id                                    */
/* ==================================================================== */

static void jsoc_ids(const jsoc_document *doc, std::vector<uint64_t> &ids) {
  const jsoc_value *st = jsoc_object_get_cstr(doc, doc->root, "statuses");
  if (!st || !jsoc_value_is_array(st)) return;
  for (uint32_t c = st->first; c != JSOC_NO_INDEX;
       c = doc->values[c].next) {
    const jsoc_value *tweet = &doc->values[c];
    const jsoc_value *user = jsoc_object_get_cstr(doc, tweet, "user");
    uint64_t id = 0;
    if (user && jsoc_value_get_uint64(doc, jsoc_object_get_cstr(doc, user, "id"), &id))
      ids.push_back(id);
    const jsoc_value *rt = jsoc_object_get_cstr(doc, tweet, "retweeted_status");
    if (rt && jsoc_value_is_object(rt)) {
      const jsoc_value *ru = jsoc_object_get_cstr(doc, rt, "user");
      uint64_t rid = 0;
      if (ru && jsoc_value_get_uint64(doc, jsoc_object_get_cstr(doc, ru, "id"), &rid))
        ids.push_back(rid);
    }
  }
}

static void sj_ids(simdjson::dom::element doc, std::vector<uint64_t> &ids) {
  simdjson::dom::array statuses;
  if (doc["statuses"].get_array().get(statuses)) return;
  for (auto tweet : statuses) {
    uint64_t id = 0;
    if (!tweet["user"]["id"].get_uint64().get(id)) ids.push_back(id);
    auto rt = tweet["retweeted_status"];
    if (!rt.error()) {
      uint64_t rid = 0;
      if (!rt["user"]["id"].get_uint64().get(rid)) ids.push_back(rid);
    }
  }
}

static std::vector<Sample> run_distinct(int argc, char **argv, int first) {
  std::vector<Sample> out;
  for (int arg = first; arg < argc; arg++) {
    std::vector<char> buf;
    size_t len = 0;
    if (!read_file(argv[arg], buf, &len) || len == 0) continue;
    jsoc_arena *a = jsoc_arena_create(len + 65536);
    jsoc_document jd = jsoc_parse(buf.data(), len, a);
    simdjson::dom::parser p;
    auto sjdoc = p.parse(buf.data(), len);
    if (jd.error.code != JSOC_OK || sjdoc.error() ||
        !jsoc_object_get_cstr(&jd, jd.root, "statuses")) {
      jsoc_arena_destroy(a);
      continue;
    }
    std::vector<uint64_t> ji, si;
    jsoc_ids(&jd, ji);
    sj_ids(sjdoc.value(), si);
    std::sort(ji.begin(), ji.end()); ji.erase(std::unique(ji.begin(), ji.end()), ji.end());
    std::sort(si.begin(), si.end()); si.erase(std::unique(si.begin(), si.end()), si.end());
    if (ji != si)
      std::fprintf(stderr, "%s: distinct ids mismatch (%zu vs %zu)\n",
                   argv[arg], ji.size(), si.size());
    Config cfg = config(len, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          std::vector<uint64_t> ids;
          jsoc_ids(&jd, ids);
          std::sort(ids.begin(), ids.end());
          ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
          if (ids.empty()) std::abort();
        },
        [&] {
          std::vector<uint64_t> ids;
          sj_ids(sjdoc.value(), ids);
          std::sort(ids.begin(), ids.end());
          ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
          if (ids.empty()) std::abort();
        }, cfg, &jt, &st);
    out.push_back({argv[arg], len, ji.size(), jt, st});
    print_sample(out.back(), "input GB/s (parse+extract)");
    jsoc_arena_destroy(a);
  }
  return out;
}

/* ==================================================================== */
/* find: simdjson find_tweet                                              */
/* ==================================================================== */

static bool jsoc_find(const jsoc_document *doc, uint64_t target, jsoc_string &text) {
  const jsoc_value *st = jsoc_object_get_cstr(doc, doc->root, "statuses");
  if (!st || !jsoc_value_is_array(st)) return false;
  for (uint32_t c = st->first; c != JSOC_NO_INDEX;
       c = doc->values[c].next) {
    const jsoc_value *tweet = &doc->values[c];
    uint64_t id = 0;
    if (jsoc_value_get_uint64(doc, jsoc_object_get_cstr(doc, tweet, "id"), &id) &&
        id == target) {
      text = jsoc_value_get_string(doc, jsoc_object_get_cstr(doc, tweet, "text"));
      return true;
    }
  }
  return false;
}

static bool sj_find(simdjson::dom::element doc, uint64_t target, std::string_view &text) {
  simdjson::dom::array statuses;
  if (doc["statuses"].get_array().get(statuses)) return false;
  for (auto tweet : statuses) {
    uint64_t id = 0;
    if (!tweet["id"].get_uint64().get(id) && id == target) {
      if (!tweet["text"].get_string().get(text)) return true;
      return false;
    }
  }
  return false;
}

static std::vector<Sample> run_find(int argc, char **argv, int first) {
  std::vector<Sample> out;
  for (int arg = first; arg < argc; arg++) {
    std::vector<char> buf;
    size_t len = 0;
    if (!read_file(argv[arg], buf, &len) || len == 0) continue;
    jsoc_arena *a = jsoc_arena_create(len + 65536);
    jsoc_document jd = jsoc_parse(buf.data(), len, a);
    simdjson::dom::parser p;
    auto sjdoc = p.parse(buf.data(), len);
    if (jd.error.code != JSOC_OK || sjdoc.error() ||
        !jsoc_object_get_cstr(&jd, jd.root, "statuses")) {
      jsoc_arena_destroy(a);
      continue;
    }
    /* Pick a target id from the middle of the corpus. */
    const jsoc_value *st = jsoc_object_get_cstr(&jd, jd.root, "statuses");
    size_t n = jsoc_array_size(&jd, st);
    uint64_t target = 0;
    if (n == 0 || !jsoc_value_get_uint64(&jd,
        jsoc_object_get_cstr(&jd, jsoc_array_at(&jd, st, n / 2), "id"), &target)) {
      jsoc_arena_destroy(a);
      continue;
    }
    jsoc_string jtext{};
    std::string_view stext;
    bool jok = jsoc_find(&jd, target, jtext);
    bool sok = sj_find(sjdoc.value(), target, stext);
    if (jok != sok || (jok && (jtext.length != stext.size() ||
        std::memcmp(jtext.data, stext.data(), jtext.length) != 0)))
      std::fprintf(stderr, "%s: find mismatch\n", argv[arg]);
    Config cfg = config(len, arg);
    double jt = 0, ss = 0;
    time_pair(
        [&] {
          jsoc_string t{};
          if (!jsoc_find(&jd, target, t) || t.length == 0) std::abort();
        },
        [&] {
          std::string_view t;
          if (!sj_find(sjdoc.value(), target, t) || t.empty()) std::abort();
        }, cfg, &jt, &ss);
    out.push_back({argv[arg], len, n, jt, ss});
    print_sample(out.back(), "input GB/s (parse+find)");
    jsoc_arena_destroy(a);
  }
  return out;
}

/* ==================================================================== */
/* numbers: simdjson number accessors (1M integer array)                  */
/* ==================================================================== */

struct xorshift64 {
  uint64_t state;
  uint64_t operator()() {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
  }
};

static std::string build_numbers(size_t count) {
  xorshift64 rng{0x2545f4914f6cdd1dULL};
  std::string out;
  out.reserve(count * 24);
  out += '[';
  for (size_t i = 0; i < count; i++) {
    if (i) out += ',';
    uint64_t m = 10000ULL + rng() % 90000ULL; /* 5 digits */
    if (rng() & 1) out += '-';
    out += std::to_string(m);
  }
  out += ']';
  return out;
}

static std::vector<Sample> run_numbers() {
  const size_t COUNT = 1000000;
  std::string json = build_numbers(COUNT);
  std::vector<Sample> out;
  jsoc_arena *a = jsoc_arena_create(json.size() + 65536);
  jsoc_document jd = jsoc_parse(json.data(), json.size(), a);
  simdjson::dom::parser p;
  auto sjdoc = p.parse(json.data(), json.size());
  if (jd.error.code != JSOC_OK || sjdoc.error()) {
    std::fprintf(stderr, "numbers: parse failed\n");
    jsoc_arena_destroy(a);
    return out;
  }
  uint64_t jsum = 0, ssum = 0;
  {
    for (uint32_t c = jd.root->first; c != JSOC_NO_INDEX;
         c = jd.values[c].next) {
      int64_t v = 0;
      if (!jsoc_value_get_int64(&jd, &jd.values[c], &v)) continue;
      jsum += (uint64_t)v;
    }
  }
  {
    simdjson::dom::array arr;
    if (!sjdoc.get_array().get(arr)) {
      for (auto e : arr) {
        int64_t v = 0;
        if (!e.get_int64().get(v)) ssum += (uint64_t)v;
      }
    }
  }
  if (jsum != ssum)
    std::fprintf(stderr, "numbers: sum mismatch (%llu vs %llu)\n",
                 (unsigned long long)jsum, (unsigned long long)ssum);
  Config cfg = config(json.size(), 1);
  double jt = 0, ss = 0;
  time_pair(
      [&] {
        uint64_t sum = 0;
        /* Scalar array elements occupy consecutive value slots in jsoc's
         * arena-built DOM; walk them sequentially like simdjson's tape. */
        const int64_t *cache = jsoc_number_cache(&jd, jd.root->first);
        for (size_t i = 0, n = jd.root->count; i < n; i++)
          sum += (uint64_t)cache[i];
        if (sum == 0) std::abort();
      },
      [&] {
        uint64_t sum = 0;
        simdjson::dom::array arr;
        if (sjdoc.get_array().get(arr)) std::abort();
        for (auto e : arr) {
          int64_t v = 0;
          if (e.get_int64().get(v)) std::abort();
          sum += (uint64_t)v;
        }
        if (sum == 0) std::abort();
      }, cfg, &jt, &ss);
  static const char *name = "numbers-1M.json (generated)";
  out.push_back({name, json.size(), COUNT, jt, ss});
  print_sample(out.back(), "input GB/s (traverse numbers)");
  jsoc_arena_destroy(a);
  return out;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "Usage: %s <file...>  (BENCH_MODE=stats,distinct,find,numbers)\n",
                 argv[0]);
    return 1;
  }
  std::string modes = "stats,distinct,find,numbers";
  if (const char *e = std::getenv("BENCH_MODE")) modes = e;
  auto on = [&](const char *m) { return modes == "all" || modes.find(m) != std::string::npos; };

  if (on("stats")) {
    std::printf("\n== Statistics: DOM traversal (nativejson-benchmark) ==\n");
    summary("stats", run_stats(argc, argv, 1));
  }
  if (on("distinct")) {
    std::printf("\n== distinct_user_id (simdjson benchmark) ==\n");
    summary("distinct", run_distinct(argc, argv, 1));
  }
  if (on("find")) {
    std::printf("\n== find_tweet (simdjson benchmark) ==\n");
    summary("find", run_find(argc, argv, 1));
  }
  if (on("numbers")) {
    std::printf("\n== number accessors (simdjson benchmark) ==\n");
    summary("numbers", run_numbers());
  }
  return 0;
}
