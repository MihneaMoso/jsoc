#pragma once
#include "export.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <stdalign.h>

typedef struct jsoc_arena {
  uint8_t *base;
  size_t offset;
  size_t capacity;
  struct jsoc_arena *next;
} jsoc_arena;

JSOC_API jsoc_arena *
jsoc_arena_create(size_t initial_capacity) JSOC_NODISCARD JSOC_NOINLINE;
JSOC_API void jsoc_arena_destroy(jsoc_arena *a) JSOC_NOINLINE;
JSOC_API void jsoc_arena_reset(jsoc_arena *a) JSOC_NOINLINE;
JSOC_API void *jsoc_arena_alloc(jsoc_arena *a, size_t size,
                                size_t align) JSOC_NODISCARD;
JSOC_API void *jsoc_arena_alloc_zero(jsoc_arena *a, size_t size,
                                     size_t align) JSOC_NODISCARD;

/* Inline fast path: when the current chunk has room, bump the offset without
 * a call into the library.  Falls back to jsoc_arena_alloc on growth. */
static inline void *jsoc_arena_bump(jsoc_arena *a, size_t size, size_t align) {
  if (JSOC_UNLIKELY(!a || size == 0))
    return NULL;
  uintptr_t addr = (uintptr_t)(a->base + a->offset);
  uintptr_t aligned = (addr + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
  size_t need = (size_t)(aligned - addr) + size;
  if (JSOC_LIKELY(need <= a->capacity - a->offset)) {
    a->offset += need;
    return (void *)aligned;
  }
  return jsoc_arena_alloc(a, size, align);
}

#define jsoc_arena_alloc_t(a, T, count)                                        \
  ((T *)jsoc_arena_alloc((a), (count) * sizeof(T), alignof(T)))
#define jsoc_arena_alloc_t_zero(a, T, count)                                   \
  ((T *)jsoc_arena_alloc_zero((a), (count) * sizeof(T), alignof(T)))

#ifdef __cplusplus
}
#endif
