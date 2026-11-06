/*
 * Chunked bump allocator.
 *
 * The caller holds a stable handle (`jsoc_arena *`).  The handle always
 * describes the *current* chunk, so allocation stays a pointer bump with no
 * linked-list walk.  When the current chunk is full a new, larger chunk is
 * allocated and the old chunk's bookkeeping is copied into a retired node
 * linked off the handle.  Old bytes are never copied, so growth is O(1) in
 * the amount of live data and pointers handed out earlier stay valid until
 * the arena is reset or destroyed.
 */
#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSOC_ARENA_MIN_CAP ((size_t)4096)
#define JSOC_ARENA_MAX_CAP (SIZE_MAX / 2)

JSOC_API jsoc_arena *jsoc_arena_create(size_t initial_capacity) {
  if (initial_capacity < JSOC_ARENA_MIN_CAP)
    initial_capacity = JSOC_ARENA_MIN_CAP;

  jsoc_arena *a = malloc(sizeof *a);
  if (JSOC_UNLIKELY(!a))
    return NULL;

  uint8_t *base = malloc(initial_capacity);
  if (JSOC_UNLIKELY(!base)) {
    free(a);
    return NULL;
  }

  a->base = base;
  a->offset = 0;
  a->capacity = initial_capacity;
  a->next = NULL;
  return a;
}

JSOC_API void jsoc_arena_destroy(jsoc_arena *a) {
  while (a) {
    jsoc_arena *next = a->next;
    free(a->base);
    free(a);
    a = next;
  }
}

JSOC_API void jsoc_arena_reset(jsoc_arena *a) {
  if (!a)
    return;

  jsoc_arena *node = a->next;
  while (node) {
    jsoc_arena *next = node->next;
    free(node->base);
    free(node);
    node = next;
  }

  a->next = NULL;
  a->offset = 0;
}

static JSOC_NOINLINE void *arena_grow(jsoc_arena *a, size_t size,
                                      size_t align) {
  /* Worst case start offset after aligning the fresh chunk. */
  if (JSOC_UNLIKELY(size > SIZE_MAX - align))
    return NULL;
  size_t needed = size + align;

  size_t new_cap = a->capacity;
  while (new_cap < needed) {
    if (JSOC_UNLIKELY(new_cap > JSOC_ARENA_MAX_CAP / 2)) {
      new_cap = needed;
      break;
    }
    new_cap *= 2;
  }
  /* Even a small overflow should at least double, for amortised O(1). */
  if (new_cap < a->capacity * 2 && a->capacity <= JSOC_ARENA_MAX_CAP / 2) {
    new_cap = a->capacity * 2;
  }

  uint8_t *new_base = malloc(new_cap);
  if (JSOC_UNLIKELY(!new_base))
    return NULL;

  jsoc_arena *retired = malloc(sizeof *retired);
  if (JSOC_UNLIKELY(!retired)) {
    free(new_base);
    return NULL;
  }

  retired->base = a->base;
  retired->offset = a->offset;
  retired->capacity = a->capacity;
  retired->next = a->next;

  a->base = new_base;
  a->offset = 0;
  a->capacity = new_cap;
  a->next = retired;

  uintptr_t addr = (uintptr_t)new_base;
  uintptr_t aligned = (addr + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
  a->offset = (size_t)(aligned - addr) + size;
  return (void *)aligned;
}

JSOC_API void *jsoc_arena_alloc(jsoc_arena *a, size_t size, size_t align) {
  if (JSOC_UNLIKELY(!a || size == 0))
    return NULL;
  if (JSOC_UNLIKELY(align == 0))
    align = 1;
  if (JSOC_UNLIKELY((align & (align - 1)) != 0))
    return NULL;

  uintptr_t addr = (uintptr_t)(a->base + a->offset);
  uintptr_t aligned = (addr + (uintptr_t)align - 1u) & ~((uintptr_t)align - 1u);
  size_t padding = (size_t)(aligned - addr);
  size_t needed = padding + size;

  /* `offset <= capacity` is an invariant, so this subtraction cannot wrap. */
  if (JSOC_LIKELY(needed <= a->capacity - a->offset)) {
    a->offset += needed;
    return (void *)aligned;
  }

  return arena_grow(a, size, align);
}

JSOC_API void *jsoc_arena_alloc_zero(jsoc_arena *a, size_t size, size_t align) {
  void *ptr = jsoc_arena_alloc(a, size, align);
  if (ptr)
    memset(ptr, 0, size);
  return ptr;
}
