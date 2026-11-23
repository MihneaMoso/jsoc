#pragma once

/*
 * Compile-time-dispatched on-demand decoding (C++26, P2996 reflection).
 *
 * The jsoc counterpart of simdjson's `parser.iterate(buf).get<T>()`.  Field
 * names come from the reflected struct members and are matched through a
 * compile-time bucket selector, so the walk has no runtime field table and
 * builds no DOM.  `jsoc_string` members are zero-copy slices into the input.
 *
 * Struct convention: integral/bool members decode from numbers, jsoc_string
 * / std::string from strings, nested structs from objects, and a pointer
 * member `T *items` paired with an integral member `<name>_count` from an
 * array (count is capacity on entry, element count on return).
 */

#include "ondemand.h"

#include <meta>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <ranges>

namespace jsoc {
namespace od {
namespace detail {

template <typename T> struct fields {
  static constexpr auto members = std::define_static_array(
      std::meta::nonstatic_data_members_of(^^T,
                                           std::meta::access_context::unchecked()));
  static constexpr size_t count = members.size();
};

template <typename T> struct selector {
  const char *names[fields<T>::count];
  uint8_t lens[fields<T>::count];
  int8_t bucket[256];
  int8_t next[fields<T>::count];

  constexpr selector() : names{}, lens{}, bucket{}, next{} {
    for (int i = 0; i < 256; i++) bucket[i] = -1;
    size_t k = 0;
    template for (constexpr auto m : fields<T>::members) {
      constexpr auto nm = std::meta::identifier_of(m);
      names[k] = nm.data();
      lens[k] = (uint8_t)nm.size();
      const int8_t b = (int8_t)(unsigned char)nm[0];
      next[k] = bucket[b];
      bucket[b] = (int8_t)k;
      k++;
    }
  }

  inline int find(const char *key, uint32_t len) const {
    for (int8_t i = bucket[(unsigned char)key[0]]; i >= 0; i = next[i]) {
      if (lens[i] == len && std::memcmp(names[i], key, len) == 0) return (int)i;
    }
    return -1;
  }
};

inline unsigned char at(const jsoc_ondemand &od, size_t p) {
  return (unsigned char)od.input[od.indexes[p]];
}

inline size_t skip(const jsoc_ondemand &od, size_t p) {
  const unsigned char c = at(od, p);
  if (c == '"') return p + 2;
  if (c == '{' || c == '[') {
    unsigned d = 1;
    size_t q = p + 1;
    while (d) {
      const unsigned char x = at(od, q++);
      if (x == '{' || x == '[')
        d++;
      else if (x == '}' || x == ']')
        d--;
    }
    return q;
  }
  return p + 1;
}

inline bool get_string(const jsoc_ondemand &od, size_t p, const char **b,
                       uint32_t *l) {
  if (at(od, p) != '"') return false;
  *b = od.input + od.indexes[p] + 1;
  *l = od.indexes[p + 1] - od.indexes[p] - 1;
  return true;
}

inline uint64_t get_uint(const jsoc_ondemand &od, size_t p) {
  const char *s = od.input + od.indexes[p];
  uint64_t m = 0;
  while (*s >= '0' && *s <= '9') {
    m = m * 10 + (uint64_t)(*s - '0');
    s++;
  }
  return m;
}

template <typename M>
inline constexpr bool is_string_v =
    std::is_same_v<std::remove_cv_t<M>, jsoc_string> ||
    std::is_same_v<std::remove_cv_t<M>, std::string>;

template <typename M> inline bool store(const jsoc_ondemand &, size_t, M &,
                                        jsoc_arena *);

template <> inline bool store<uint64_t>(const jsoc_ondemand &od, size_t p,
                                        uint64_t &dst, jsoc_arena *) {
  const unsigned char c = at(od, p);
  if (c < '0' || c > '9') return false;
  dst = get_uint(od, p);
  return true;
}
template <> inline bool store<int64_t>(const jsoc_ondemand &od, size_t p,
                                       int64_t &dst, jsoc_arena *) {
  const unsigned char c = at(od, p);
  if (c == '-') {
    dst = -(int64_t)get_uint(od, p + 1);
    return true;
  }
  if (c >= '0' && c <= '9') {
    dst = (int64_t)get_uint(od, p);
    return true;
  }
  return false;
}
template <> inline bool store<bool>(const jsoc_ondemand &od, size_t p,
                                    bool &dst, jsoc_arena *) {
  const char *s = od.input + od.indexes[p];
  if (*s == 't') { dst = true; return true; }
  if (*s == 'f') { dst = false; return true; }
  return false;
}
template <> inline bool store<jsoc_string>(const jsoc_ondemand &od, size_t p,
                                           jsoc_string &dst, jsoc_arena *a) {
  if (at(od, p) != '"') return false;
  const char *raw = od.input + od.indexes[p] + 1;
  const uint32_t len = od.indexes[p + 1] - od.indexes[p] - 1;
  if (JSOC_LIKELY(!__builtin_memchr(raw, '\\', len))) {
    /* Zero-copy slice into the input, which outlives the decoded struct. */
    dst = jsoc_string{(char *)raw, len};
    return true;
  }
  dst = jsoc_ondemand_string(&od, p, a);
  return dst.data != NULL;
}
template <> inline bool store<std::string>(const jsoc_ondemand &od, size_t p,
                                           std::string &dst, jsoc_arena *a) {
  jsoc_string s = jsoc_ondemand_string(&od, p, a);
  if (!s.data) return false;
  dst.assign(s.data, s.length);
  return true;
}

template <typename T> inline bool decode_obj(const jsoc_ondemand &od, size_t p,
                                             T &out, jsoc_arena *arena,
                                             size_t *end);

template <typename Elem, typename Count>
inline bool decode_arr(const jsoc_ondemand &od, size_t p, Elem *items,
                       Count &count, jsoc_arena *arena, size_t *end) {
  if (at(od, p) != '[') return false;
  const size_t cap = (size_t)count;
  count = 0;
  size_t q = p + 1;
  if (at(od, q) == ']') { *end = q + 1; return true; }
  size_t k = 0;
  while (true) {
    if (k >= cap) return false;
    size_t e = 0;
    if constexpr (std::is_class_v<Elem>) {
      if (!decode_obj(od, q, items[k], arena, &e)) return false;
    } else {
      if (!store<Elem>(od, q, items[k], arena)) return false;
      e = skip(od, q);
    }
    q = e;
    k++;
    count = (Count)k;
    if (at(od, q) == ']') { *end = q + 1; return true; }
  }
}

/* Decode one matched member at compile-time index `I` in T. */
template <size_t I, typename T>
inline bool decode_member(const jsoc_ondemand &od, size_t v, T &out,
                          jsoc_arena *arena, size_t *next) {
  constexpr auto m = fields<T>::members[I];
  using M = typename [:std::meta::type_of(m):];
  constexpr auto nm = std::meta::identifier_of(m);

  if constexpr (is_string_v<M>) {
    *next = skip(od, v);
    return store<M>(od, v, out.[:m:], arena);
  } else if constexpr (std::is_pointer_v<M> &&
                std::is_class_v<std::remove_pointer_t<M>>) {
    /* array: find the sibling `<name>_count` member */
    bool found = false;
    static_assert(fields<T>::count > 0);
    template for (constexpr auto cm : fields<T>::members) {
      if (!found) {
        constexpr auto cnm = std::meta::identifier_of(cm);
        constexpr bool ends_count = cnm.size() > 6 &&
            cnm.substr(cnm.size() - 6) == std::string_view("_count");
        if constexpr (ends_count) {
          constexpr auto stem = cnm.substr(0, cnm.size() - 6);
          constexpr bool same_stem = stem.size() <= nm.size()
              ? stem == nm.substr(0, stem.size())
              : nm == stem.substr(0, nm.size());
          if constexpr (same_stem) {
            using C = typename [:std::meta::type_of(cm):];
            *next = 0;
            found = decode_arr(od, v, out.[:m:], out.[:cm:], arena, next);
          }
        }
      }
    }
    return found;
  } else if constexpr (std::is_class_v<M>) {
    *next = 0;
    return decode_obj(od, v, out.[:m:], arena, next);
  } else {
    *next = skip(od, v);
    return store<M>(od, v, out.[:m:], arena);
  }
}

template <typename T>
inline bool decode_obj(const jsoc_ondemand &od, size_t p, T &out,
                       jsoc_arena *arena, size_t *end) {
  if (at(od, p) != '{') return false;
  static constexpr selector<T> sel;

  bool seen[fields<T>::count] = {false};
  size_t q = p + 1;
  if (at(od, q) == '}') { *end = q + 1; return true; }
  while (true) {
    const char *key; uint32_t klen;
    if (!get_string(od, q, &key, &klen)) return false;
    const size_t v = q + 2;
    const int fi = klen ? sel.find(key, klen) : -1;

    size_t next;
    if (fi >= 0 && !seen[fi]) {
      bool ok = false;
      template for (constexpr auto I : std::define_static_array(
                        std::views::iota(size_t(0), fields<T>::count))) {
        if (!ok && I == (size_t)fi) ok = decode_member<I>(od, v, out, arena, &next);
      }
      if (!ok) return false;
      seen[fi] = true;
    } else {
      next = skip(od, v);
    }
    q = next;
    if (at(od, q) == '}') { *end = q + 1; break; }
  }
  return true;
}

} // namespace detail

template <typename T>
inline bool decode(const jsoc_ondemand &od, T &out, jsoc_arena *arena) {
  if (od.count == 0) return false;
  size_t end = 0;
  if (!detail::decode_obj(od, 0, out, arena, &end)) return false;
  return end == od.count;
}

} // namespace od
} // namespace jsoc
