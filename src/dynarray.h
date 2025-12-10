#ifndef DYNARRAY_H
#define DYNARRAY_H

#include <stddef.h>

typedef struct {
    void *data;
    size_t size;
    size_t capacity;
    size_t elem_size;
    void (*free_fn)(void *);
} dynarray_t;

// Core operations
dynarray_t *da_create(size_t elem_size, size_t capacity, void (*free_fn)(void *));
void da_free(dynarray_t *arr);

size_t da_size(const dynarray_t *arr);
void *da_get(const dynarray_t *arr, size_t index);
int   da_set(dynarray_t *arr, size_t index, const void *value);

// Adding / removing
int da_push(dynarray_t *arr, const void *value);
int da_insert(dynarray_t *arr, size_t index, const void *value);
int da_pop(dynarray_t *arr, void *out);
int da_remove(dynarray_t *arr, size_t index);

// Internal helpers (you can keep static in .c)
int da_resize(dynarray_t *arr, size_t new_capacity);

#endif
