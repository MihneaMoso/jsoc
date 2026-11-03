#pragma once
#include "export.h"
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

typedef struct jsoc_arena {
    uint8_t *base;
    size_t offset;
    size_t capacity;
    struct jsoc_arena *next;
} jsoc_arena;

JSOC_API jsoc_arena *jsoc_arena_create(size_t initial_capacity) JSOC_NODISCARD JSOC_NOINLINE;
JSOC_API void jsoc_arena_destroy(jsoc_arena *a) JSOC_NOINLINE;
JSOC_API void jsoc_arena_reset(jsoc_arena *a) JSOC_NOINLINE;
JSOC_API void *jsoc_arena_alloc(jsoc_arena *a, size_t size, size_t align) JSOC_NODISCARD;
JSOC_API void *jsoc_arena_alloc_zero(jsoc_arena *a, size_t size, size_t align) JSOC_NODISCARD;

#define jsoc_arena_alloc_t(a, T, count) \
    ((T *)jsoc_arena_alloc((a), (count) * sizeof(T), alignof(T)))
#define jsoc_arena_alloc_t_zero(a, T, count) \
    ((T *)jsoc_arena_alloc_zero((a), (count) * sizeof(T), alignof(T)))
