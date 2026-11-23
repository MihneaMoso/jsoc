/*
 * Side-by-side serialization / deserialization benchmark: jsoc vs simdjson.
 *
 * Modes (BENCH_MODE=comma,separated; default all):
 *   dom_serialize   parse once, then repeatedly serialize the whole DOM
 *                   (jsoc_document_minify vs simdjson::to_string)
 *   roundtrip       parse + serialize in one timed pass
 *                   (jsoc_minify vs simdjson parse + to_string)
 *   raw_minify      whitespace stripping without building a DOM
 *                   (jsoc_minify [validating] vs simdjson::minify [no validation])
 *   typed_de        deserialize into native structs
 *                   (jsoc_parse + jsoc_decode_object vs ondemand + doc.get<T>)
 *   typed_ser       serialize native structs to JSON
 *                   (jsoc writer hand-written vs simdjson reflected builder)
 *   stream          NDJSON / concatenated documents
 *                   (jsoc_parse_many vs simdjson parse_many)
 *
 * The protocol matches bench/compare_simdjson.cpp: warmups, contiguous timed
 * phases, alternating phase order per file, best-of-run.  Set BENCH_ITERS,
 * BENCH_BLOCK, or BENCH_SETTLE to override.
 *
 * Built by bench/compare_serialization.sh; not part of the default build.
 */

#include "simdjson.h"

extern "C" {
#include "jsoc.h"
#include "decode.h"
#include "encode.h"
#include "ondemand.h"
}
#include "ondemand.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
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
  const char *slash = std::strrchr(path, '/');
  return slash ? slash + 1 : path;
}

static bool has_suffix(const char *s, const char *suffix) {
  size_t n = std::strlen(s), m = std::strlen(suffix);
  return n >= m && std::memcmp(s + n - m, suffix, m) == 0;
}

static int choose_iters(size_t length) {
  if (const char *env = std::getenv("BENCH_ITERS")) {
    int n = std::atoi(env);
    if (n > 0) return n;
  }
  double n = 150e6 / (double)(length ? length : 1);
  if (n < 20.0) n = 20.0;
  if (n > 400.0) n = 400.0;
  return (int)n;
}

struct PhaseConfig {
  int iters;
  int block;
  int settle;
  bool jsoc_first;
};

static PhaseConfig phase_config(size_t length, int index) {
  PhaseConfig c;
  c.iters = choose_iters(length);
  c.block = c.iters;
  c.settle = 2;
  if (const char *env = std::getenv("BENCH_BLOCK")) {
    int n = std::atoi(env);
    if (n > 0) c.block = n;
  }
  if (const char *env = std::getenv("BENCH_SETTLE")) c.settle = std::atoi(env);
  c.jsoc_first = (index & 1) != 0;
  return c;
}

/* Run `iters` timed iterations of each side, keeping the best time. */
template <class JF, class SF>
static void time_pair(JF &&jsoc_fn, SF &&sj_fn, const PhaseConfig &cfg,
                      double *best_jsoc, double *best_sj) {
  *best_jsoc = 0.0;
  *best_sj = 0.0;
  for (int done = 0; done < cfg.iters;) {
    int n = std::min(cfg.block, cfg.iters - done);
    bool jsoc_first = (((done / cfg.block) & 1) == 0) == cfg.jsoc_first;
    for (int phase = 0; phase < 2; phase++) {
      bool run_jsoc = (phase == 0) == jsoc_first;
      for (int k = -cfg.settle; k < n; k++) {
        if (run_jsoc) {
          double t0 = now();
          jsoc_fn();
          double dt = now() - t0;
          if (k >= 0 && (*best_jsoc == 0.0 || dt < *best_jsoc)) *best_jsoc = dt;
        } else {
          double t0 = now();
          sj_fn();
          double dt = now() - t0;
          if (k >= 0 && (*best_sj == 0.0 || dt < *best_sj)) *best_sj = dt;
        }
      }
    }
    done += n;
  }
}

struct Sample {
  const char *path;
  size_t length;
  size_t out_bytes;
  size_t out_bytes_sj;
  double jsoc_s;
  double sj_s;
};

static void print_header(const char *title, const char *unit) {
  std::printf("\n== %s ==\n", title);
  std::printf("%-32s %10s %10s %9s  %s\n", "file", "jsoc", "simdjson",
              "speedup", unit);
  std::printf("%-32s %10s %10s %9s\n", "", "GB/s", "GB/s", "x");
}

static void print_sample(const Sample &s, size_t bytes_for_rate,
                         double jsoc_s, double sj_s) {
  double jg = ((double)bytes_for_rate / 1e9) / jsoc_s;
  double sg = ((double)bytes_for_rate / 1e9) / sj_s;
  double speedup = sj_s / jsoc_s;
  std::printf("%-32s %10.2f %10.2f %8.3fx  (%zu B, %zu/%zu B out)\n",
              base_name(s.path), jg, sg, speedup, s.length, s.out_bytes,
              s.out_bytes_sj);
  std::fflush(stdout);
}

static void print_summary(const std::vector<Sample> &samples, bool use_output) {
  if (samples.empty()) return;
  double log_sum = 0.0, min_sp = samples[0].sj_s / samples[0].jsoc_s;
  double max_sp = min_sp, total_j = 0.0, total_s = 0.0, total_bytes = 0.0;
  size_t wins = 0, losses = 0, ties = 0;
  for (const Sample &s : samples) {
    double sp = s.sj_s / s.jsoc_s;
    log_sum += std::log(sp);
    min_sp = std::min(min_sp, sp);
    max_sp = std::max(max_sp, sp);
    total_j += s.jsoc_s;
    total_s += s.sj_s;
    total_bytes += use_output ? (double)s.out_bytes : (double)s.length;
    if (sp > 1.005) wins++;
    else if (sp < 0.995) losses++;
    else ties++;
  }
  double geo = std::exp(log_sum / (double)samples.size());
  std::printf("  aggregate throughput   jsoc %6.2f GB/s   simdjson %6.2f GB/s\n",
              total_bytes / total_j / 1e9, total_bytes / total_s / 1e9);
  std::printf("  geometric-mean speedup %6.3fx (min %.3fx, max %.3fx)\n",
              geo, min_sp, max_sp);
  std::printf("  jsoc wins %zu, simdjson wins %zu, ties %zu\n", wins, losses,
              ties);
}

/* ------------------------------------------------------------------ */
/* Typed twitter model, for both libraries.                           */
/* ------------------------------------------------------------------ */

namespace sj {
struct User {
  uint64_t id{};
  std::string name, screen_name, location, description;
  bool verified{};
  uint64_t followers_count{}, friends_count{}, statuses_count{};
};
struct Status {
  std::string created_at;
  uint64_t id{};
  std::string text;
  User user;
  uint64_t retweet_count{}, favorite_count{};
};
struct TwitterData {
  std::vector<Status> statuses;
};
} // namespace sj

extern "C" {
typedef struct jsoc_user {
  uint64_t id;
  jsoc_string name, screen_name, location, description;
  bool verified;
  uint64_t followers_count, friends_count, statuses_count;
} jsoc_user;

typedef struct jsoc_status {
  jsoc_string created_at;
  uint64_t id;
  jsoc_string text;
  jsoc_user user;
  uint64_t retweet_count, favorite_count;
} jsoc_status;

typedef struct jsoc_twitter {
  jsoc_status *statuses;
  size_t status_count;
} jsoc_twitter;
}

static const jsoc_decode_field jsoc_user_fields[] = {
    JSOC_DECODE_FIELD_A(jsoc_user, id, JSOC_DECODE_UINT64),
    JSOC_DECODE_FIELD_A(jsoc_user, name, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_user, screen_name, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_user, location, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_user, description, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_user, verified, JSOC_DECODE_BOOL),
    JSOC_DECODE_FIELD_A(jsoc_user, followers_count, JSOC_DECODE_UINT64),
    JSOC_DECODE_FIELD_A(jsoc_user, friends_count, JSOC_DECODE_UINT64),
    JSOC_DECODE_FIELD_A(jsoc_user, statuses_count, JSOC_DECODE_UINT64),
    {.name = NULL},
};

static const jsoc_decode_field jsoc_status_fields[] = {
    JSOC_DECODE_FIELD_A(jsoc_status, created_at, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_status, id, JSOC_DECODE_UINT64),
    JSOC_DECODE_FIELD_A(jsoc_status, text, JSOC_DECODE_STRING_REF),
    JSOC_DECODE_FIELD_A(jsoc_status, user, JSOC_DECODE_STRUCT,
                        .nested = jsoc_user_fields),
    JSOC_DECODE_FIELD_A(jsoc_status, retweet_count, JSOC_DECODE_UINT64),
    JSOC_DECODE_FIELD_A(jsoc_status, favorite_count, JSOC_DECODE_UINT64),
    {.name = NULL},
};

static const jsoc_decode_field jsoc_twitter_fields[] = {
    JSOC_DECODE_FIELD_A(jsoc_twitter, statuses, JSOC_DECODE_ARRAY,
                        .count_offset = offsetof(jsoc_twitter, status_count),
                        .item_kind = JSOC_DECODE_STRUCT,
                        .item_size = sizeof(jsoc_status),
                        .items = jsoc_status_fields),
    {.name = NULL},
};

static const jsoc_encode_field jsoc_user_enc[] = {
    JSOC_ENCODE_FIELD_A(jsoc_user, id, JSOC_ENCODE_UINT64),
    JSOC_ENCODE_FIELD_A(jsoc_user, name, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_user, screen_name, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_user, location, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_user, description, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_user, verified, JSOC_ENCODE_BOOL),
    JSOC_ENCODE_FIELD_A(jsoc_user, followers_count, JSOC_ENCODE_UINT64),
    JSOC_ENCODE_FIELD_A(jsoc_user, friends_count, JSOC_ENCODE_UINT64),
    JSOC_ENCODE_FIELD_A(jsoc_user, statuses_count, JSOC_ENCODE_UINT64),
    {.name = NULL},
};

static const jsoc_encode_field jsoc_status_enc[] = {
    JSOC_ENCODE_FIELD_A(jsoc_status, created_at, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_status, id, JSOC_ENCODE_UINT64),
    JSOC_ENCODE_FIELD_A(jsoc_status, text, JSOC_ENCODE_STRING_RAW),
    JSOC_ENCODE_FIELD_A(jsoc_status, user, JSOC_ENCODE_STRUCT,
                        .nested = jsoc_user_enc),
    JSOC_ENCODE_FIELD_A(jsoc_status, retweet_count, JSOC_ENCODE_UINT64),
    JSOC_ENCODE_FIELD_A(jsoc_status, favorite_count, JSOC_ENCODE_UINT64),
    {.name = NULL},
};

static const jsoc_encode_field jsoc_twitter_enc[] = {
    JSOC_ENCODE_FIELD_A(jsoc_twitter, statuses, JSOC_ENCODE_ARRAY,
                        .count_offset = offsetof(jsoc_twitter, status_count),
                        .item_kind = JSOC_ENCODE_STRUCT,
                        .item_size = sizeof(jsoc_status),
                        .items = jsoc_status_enc,
                        .max_items = 256),
    {.name = NULL},
};

/* ------------------------------------------------------------------ */
/* Per-mode runners                                                    */
/* ------------------------------------------------------------------ */

static bool stream_count_jsoc(const jsoc_document *doc, void *user) {
  size_t *n = (size_t *)user;
  (void)doc;
  (*n)++;
  return true;
}

static std::vector<Sample> run_dom_serialize(int argc, char **argv, int first_arg) {
  std::vector<Sample> samples;
  for (int arg = first_arg; arg < argc; arg++) {
    if (has_suffix(argv[arg], ".ndjson") || has_suffix(argv[arg], ".jsonl"))
      continue;
    std::vector<char> buf;
    size_t length = 0;
    if (!read_file(argv[arg], buf, &length) || length == 0) continue;

    jsoc_arena *parse_arena = jsoc_arena_create(length + 65536);
    jsoc_arena *out_arena = jsoc_arena_create(1024 * 1024);
    if (!parse_arena || !out_arena) { std::fprintf(stderr, "arena failed\n"); return samples; }
    jsoc_document doc = jsoc_parse(buf.data(), length, parse_arena);
    if (doc.error.code != JSOC_OK) {
      std::fprintf(stderr, "%s: jsoc parse: %s\n", argv[arg],
                   jsoc_error_message(doc.error.code));
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(out_arena);
      continue;
    }

    simdjson::dom::parser p;
    auto sjdoc = p.parse(buf.data(), length);
    if (sjdoc.error()) {
      std::fprintf(stderr, "%s: simdjson parse: %s\n", argv[arg],
                   simdjson::error_message(sjdoc.error()));
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(out_arena);
      continue;
    }

    jsoc_string jout{};
    jsoc_arena_reset(out_arena);
    if (!jsoc_document_minify(&doc, out_arena, &jout)) {
      std::fprintf(stderr, "%s: jsoc serialize failed\n", argv[arg]);
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(out_arena);
      continue;
    }
    std::string sout = simdjson::to_string(sjdoc.value());
    if (jout.length == 0 || sout.empty()) {
      std::fprintf(stderr, "%s: empty output\n", argv[arg]);
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(out_arena);
      continue;
    }
    size_t out_bytes = jout.length;

    PhaseConfig cfg = phase_config(length, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          jsoc_arena_reset(out_arena);
          jsoc_string o{};
          if (!jsoc_document_minify(&doc, out_arena, &o))
            std::abort();
        },
        [&] {
          std::string s = simdjson::to_string(sjdoc.value());
          if (s.empty()) std::abort();
        },
        cfg, &jt, &st);
    Sample s{argv[arg], length, out_bytes, sout.size(), jt, st};
    print_sample(s, length, jt, st);
    samples.push_back(s);
    jsoc_arena_destroy(parse_arena);
    jsoc_arena_destroy(out_arena);
  }
  return samples;
}

static std::vector<Sample> run_roundtrip(int argc, char **argv, int first_arg) {
  std::vector<Sample> samples;
  for (int arg = first_arg; arg < argc; arg++) {
    if (has_suffix(argv[arg], ".ndjson") || has_suffix(argv[arg], ".jsonl"))
      continue;
    std::vector<char> buf;
    size_t length = 0;
    if (!read_file(argv[arg], buf, &length) || length == 0) continue;

    jsoc_arena *a = jsoc_arena_create(length + 65536);
    if (!a) return samples;
    jsoc_string jout{};
    jsoc_error err{};
    if (!jsoc_minify(buf.data(), length, a, &jout, &err)) {
      std::fprintf(stderr, "%s: jsoc minify: %s\n", argv[arg],
                   jsoc_error_message(err.code));
      jsoc_arena_destroy(a);
      continue;
    }
    size_t out_bytes = jout.length;

    simdjson::dom::parser p;
    size_t out_bytes_sj = 0;
    {
      auto doc = p.parse(buf.data(), length);
      if (doc.error()) { jsoc_arena_destroy(a); continue; }
      std::string s = simdjson::to_string(doc.value());
      if (s.empty()) { jsoc_arena_destroy(a); continue; }
      out_bytes_sj = s.size();
    }

    PhaseConfig cfg = phase_config(length, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          jsoc_arena_reset(a);
          jsoc_string o{};
          jsoc_error e{};
          if (!jsoc_minify(buf.data(), length, a, &o, &e)) std::abort();
        },
        [&] {
          auto doc = p.parse(buf.data(), length);
          if (doc.error()) std::abort();
          std::string s = simdjson::to_string(doc.value());
          if (s.empty()) std::abort();
        },
        cfg, &jt, &st);
    Sample s{argv[arg], length, out_bytes, out_bytes_sj, jt, st};
    print_sample(s, length, jt, st);
    samples.push_back(s);
    jsoc_arena_destroy(a);
  }
  return samples;
}

static std::vector<Sample> run_raw_minify(int argc, char **argv, int first_arg) {
  std::vector<Sample> samples;
  for (int arg = first_arg; arg < argc; arg++) {
    if (has_suffix(argv[arg], ".ndjson") || has_suffix(argv[arg], ".jsonl"))
      continue;
    std::vector<char> buf;
    size_t length = 0;
    if (!read_file(argv[arg], buf, &length) || length == 0) continue;

    jsoc_arena *a = jsoc_arena_create(length + 65536);
    if (!a) return samples;
    std::vector<char> dst(length + simdjson::SIMDJSON_PADDING);
    size_t dst_len = 0;
    jsoc_string jout{};
    if (!jsoc_minify_raw(buf.data(), length, a, &jout)) {
      jsoc_arena_destroy(a);
      continue;
    }
    if (simdjson::minify(buf.data(), length, dst.data(), dst_len)) {
      jsoc_arena_destroy(a);
      continue;
    }
    size_t out_bytes = jout.length;

    PhaseConfig cfg = phase_config(length, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          jsoc_arena_reset(a);
          jsoc_string o{};
          if (!jsoc_minify_raw(buf.data(), length, a, &o)) std::abort();
        },
        [&] {
          size_t n = 0;
          if (simdjson::minify(buf.data(), length, dst.data(), n))
            std::abort();
        },
        cfg, &jt, &st);
    Sample s{argv[arg], length, out_bytes, dst_len, jt, st};
    print_sample(s, length, jt, st);
    samples.push_back(s);
    jsoc_arena_destroy(a);
  }
  return samples;
}

static std::vector<Sample> run_stream(int argc, char **argv, int first_arg) {
  std::vector<Sample> samples;
  for (int arg = first_arg; arg < argc; arg++) {
    std::vector<char> buf;
    size_t length = 0;
    if (!read_file(argv[arg], buf, &length) || length == 0) continue;

    jsoc_arena *a = jsoc_arena_create(length + 65536);
    if (!a) return samples;
    size_t jn = 0;
    jsoc_error err = jsoc_parse_many(buf.data(), length, a, stream_count_jsoc, &jn);
    if (err.code != JSOC_OK) {
      std::fprintf(stderr, "%s: jsoc parse_many: %s\n", argv[arg],
                   jsoc_error_message(err.code));
      jsoc_arena_destroy(a);
      continue;
    }

    simdjson::dom::parser p;
    size_t sn = 0;
    {
      auto stream = p.parse_many(buf.data(), length,
                                 simdjson::dom::DEFAULT_BATCH_SIZE);
      for (auto doc : stream) {
        if (doc.error()) { sn = 0; break; }
        sn++;
      }
      if (!sn) { jsoc_arena_destroy(a); continue; }
    }

    PhaseConfig cfg = phase_config(length, arg);
    double jt = 0, st = 0;
    time_pair(
        [&] {
          jsoc_arena_reset(a);
          size_t n = 0;
          if (jsoc_parse_many(buf.data(), length, a, stream_count_jsoc, &n).code !=
              JSOC_OK)
            std::abort();
        },
        [&] {
          size_t n = 0;
          auto stream = p.parse_many(buf.data(), length,
                                     simdjson::dom::DEFAULT_BATCH_SIZE);
          for (auto doc : stream) {
            if (doc.error()) std::abort();
            n++;
          }
        },
        cfg, &jt, &st);
    Sample s{argv[arg], length, length, length, jt, st};
    std::printf("%-32s %10.2f %10.2f %8.3fx  (%zu B, %zu docs)\n",
                base_name(argv[arg]), ((double)length / 1e9) / jt,
                ((double)length / 1e9) / st, st / jt, length, jn);
    std::fflush(stdout);
    samples.push_back(s);
    jsoc_arena_destroy(a);
  }
  return samples;
}

#ifndef BENCH_NO_REFLECTION
/* Typed deserialize + serialize of twitter.json (simdjson's static_reflect
 * benchmark shape). */
static std::vector<Sample> run_typed(int argc, char **argv, int first_arg,
                                     bool serialize) {
  std::vector<Sample> samples;
  for (int arg = first_arg; arg < argc; arg++) {
    std::vector<char> buf;
    size_t length = 0;
    if (!read_file(argv[arg], buf, &length) || length == 0) continue;

    /* jsoc: parse once into a long-lived arena, decode to fixed structs. */
    jsoc_arena *parse_arena = jsoc_arena_create(length + 65536);
    jsoc_arena *work_arena = jsoc_arena_create(1024 * 1024);
    if (!parse_arena || !work_arena) return samples;
    static jsoc_status jstorage[256];
    jsoc_twitter jt{};
    jt.statuses = jstorage;
    jt.status_count = 256;
    jsoc_ondemand jod;
    jsoc_error jerr;
    if (!jsoc_ondemand_scan(&jod, buf.data(), length, parse_arena, &jerr)) {
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(work_arena);
      continue;
    }
    bool jok = jsoc::od::decode(jod, jt, work_arena);
    if (!jok) {
      std::fprintf(stderr, "%s: jsoc decode failed\n", argv[arg]);
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(work_arena);
      continue;
    }

    /* simdjson: ondemand + static reflection into matching structs. */
    simdjson::padded_string padded(buf.data(), length);
    simdjson::ondemand::parser p;
    sj::TwitterData st;
    {
      auto doc = p.iterate(padded);
      auto e = doc.get(st);
      if (e) {
        std::fprintf(stderr, "%s: simdjson get<T>: %s\n", argv[arg],
                     simdjson::error_message(e));
        jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(work_arena);
        continue;
      }
    }

    /* Verify that both sides saw the same number of records. */
    if (jt.status_count != st.statuses.size()) {
      std::fprintf(stderr, "%s: record count mismatch (%zu vs %zu)\n",
                   argv[arg], jt.status_count, st.statuses.size());
      jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(work_arena);
      continue;
    }

    size_t out_bytes = length;
    std::string *sj_out = nullptr;
    jsoc_string jout{};
    if (serialize) {
      jsoc_arena_reset(work_arena);
      if (!jsoc_encode_object(jsoc_twitter_enc, 1, &jt, work_arena, &jout)) {
        jsoc_arena_destroy(parse_arena); jsoc_arena_destroy(work_arena);
        continue;
      }
      sj_out = new std::string(simdjson::to_json_string(st));
      if (jout.length == 0 || sj_out->empty()) {
        delete sj_out; jsoc_arena_destroy(parse_arena);
        jsoc_arena_destroy(work_arena);
        continue;
      }
      out_bytes = jout.length;
    }

    PhaseConfig cfg = phase_config(length, arg);
    double jt_s = 0, st_s = 0;
    if (!serialize) {
      time_pair(
          [&] {
            jsoc_arena_reset(work_arena);
            jsoc_twitter out{};
            out.statuses = jstorage;
            out.status_count = 256;
            jsoc_ondemand od;
            jsoc_error derr;
            if (!jsoc_ondemand_scan(&od, buf.data(), length, work_arena, &derr))
              std::abort();
            if (!jsoc::od::decode(od, out, work_arena))
              std::abort();
          },
          [&] {
            sj::TwitterData out;
            auto doc = p.iterate(padded);
            if (doc.get(out)) std::abort();
          },
          cfg, &jt_s, &st_s);
    } else {
      time_pair(
          [&] {
            jsoc_arena_reset(work_arena);
            jsoc_string o{};
            if (!jsoc_encode_object(jsoc_twitter_enc, 1, &jt, work_arena, &o))
              std::abort();
          },
          [&] {
            std::string s = simdjson::to_json_string(st);
            if (s.empty()) std::abort();
          },
          cfg, &jt_s, &st_s);
    }

    Sample s{argv[arg], length, out_bytes, sj_out ? sj_out->size() : 0, jt_s, st_s};
    double rate_bytes = serialize ? out_bytes : length;
    print_sample(s, rate_bytes, jt_s, st_s);
    samples.push_back(s);
    delete sj_out;
    jsoc_arena_destroy(parse_arena);
    jsoc_arena_destroy(work_arena);
  }
  return samples;
}
#endif // BENCH_NO_REFLECTION

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "Usage: %s <file...>\n"
                 "  BENCH_MODE=dom_serialize,roundtrip,raw_minify,stream,typed_de,typed_ser\n"
                 "  BENCH_ITERS=N  BENCH_BLOCK=N  BENCH_SETTLE=N\n",
                 argv[0]);
    return 1;
  }

  std::string modes = "dom_serialize,roundtrip,raw_minify,stream,typed_de,typed_ser";
  if (const char *env = std::getenv("BENCH_MODE")) modes = env;
  auto enabled = [&](const char *m) {
    return modes == "all" || modes.find(m) != std::string::npos;
  };

  std::printf("jsoc vs simdjson serialization/deserialization, same process\n");
  std::printf("simdjson 5.1.1 (ae0fb53); ratio = simdjson time / jsoc time\n");

  if (enabled("dom_serialize")) {
    print_header("DOM serialize (pre-parsed): jsoc_document_minify vs simdjson::to_string",
                 "input GB/s");
    auto s = run_dom_serialize(argc, argv, 1);
    print_summary(s, false);
  }
  if (enabled("roundtrip")) {
    print_header("Parse + serialize: jsoc_minify vs simdjson parse + to_string",
                 "input GB/s");
    auto s = run_roundtrip(argc, argv, 1);
    print_summary(s, false);
  }
  if (enabled("raw_minify")) {
    print_header("Whitespace stripping: jsoc_minify (validating) vs simdjson::minify (raw)",
                 "input GB/s");
    auto s = run_raw_minify(argc, argv, 1);
    print_summary(s, false);
  }
#ifndef BENCH_NO_REFLECTION
  if (enabled("typed_de")) {
    print_header("Typed deserialize: jsoc parse+decode vs ondemand get<T>()",
                 "input GB/s");
    auto s = run_typed(argc, argv, 1, false);
    print_summary(s, false);
  }
  if (enabled("typed_ser")) {
    print_header("Typed serialize: jsoc manual writer vs simdjson reflected builder",
                 "output GB/s");
    auto s = run_typed(argc, argv, 1, true);
    print_summary(s, true);
  }
#endif
  if (enabled("stream")) {
    print_header("NDJSON stream: jsoc_parse_many vs simdjson parse_many",
                 "input GB/s");
    auto s = run_stream(argc, argv, 1);
    print_summary(s, false);
  }
  return 0;
}
