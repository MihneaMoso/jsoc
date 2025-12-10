#include "dynarray.h"
#include <stdlib.h>
#include <string.h>

#define DA_MIN_CAPACITY 4
#define DA_GROWTH_FACTOR 2
#define DA_SHRINK_THRESHOLD 4   // shrink when size < capacity / 4

// ------------------------------------------------------------
// Create array
// ------------------------------------------------------------
dynarray_t *da_create(size_t elem_size, size_t capacity, void (*free_fn)(void *)) {
    if (elem_size == 0) return NULL;

    dynarray_t *arr = malloc(sizeof(*arr));
    if (!arr) return NULL;

    if (capacity < DA_MIN_CAPACITY) capacity = DA_MIN_CAPACITY;

    arr->data = calloc(capacity, elem_size);
    if (!arr->data) {
        free(arr);
        return NULL;
    }

    arr->size = 0;
    arr->capacity = capacity;
    arr->elem_size = elem_size;
    arr->free_fn = free_fn;

    return arr;
}

// ------------------------------------------------------------
// Free array
// ------------------------------------------------------------
void da_free(dynarray_t *arr) {
    if (!arr) return;

    if (arr->free_fn) {
        // Cleanup all elements
        for (size_t i = 0; i < arr->size; i++) {
            arr->free_fn((char *)arr->data + i * arr->elem_size);
        }
    }

    free(arr->data);
    free(arr);
}

// ------------------------------------------------------------
size_t da_size(const dynarray_t *arr) {
    return arr ? arr->size : 0;
}

// ------------------------------------------------------------
void *da_get(const dynarray_t *arr, size_t index) {
    if (!arr || index >= arr->size) return NULL;
    return (char *)arr->data + index * arr->elem_size;
}

// ------------------------------------------------------------
int da_set(dynarray_t *arr, size_t index, const void *value) {
    if (!arr || index >= arr->size || !value) return 0;
    memcpy((char *)arr->data + index * arr->elem_size, value, arr->elem_size);
    return 1;
}

// ------------------------------------------------------------
// Resize buffer
// ------------------------------------------------------------
int da_resize(dynarray_t *arr, size_t new_capacity) {
    void *new_data = realloc(arr->data, new_capacity * arr->elem_size);
    if (!new_data) return 0;

    arr->data = new_data;
    arr->capacity = new_capacity;
    return 1;
}

// ------------------------------------------------------------
// Push value (append)
// ------------------------------------------------------------
int da_push(dynarray_t *arr, const void *value) {
    if (!arr || !value) return 0;

    if (arr->size == arr->capacity) {
        if (!da_resize(arr, arr->capacity * DA_GROWTH_FACTOR)) return 0;
    }

    memcpy((char *)arr->data + arr->size * arr->elem_size, value, arr->elem_size);
    arr->size++;
    return 1;
}

// ------------------------------------------------------------
// Pop last element
// ------------------------------------------------------------
int da_pop(dynarray_t *arr, void *out) {
    if (!arr || arr->size == 0) return 0;

    arr->size--;

    void *element = (char *)arr->data + arr->size * arr->elem_size;

    if (out)
        memcpy(out, element, arr->elem_size);

    if (arr->free_fn)
        arr->free_fn(element);

    // Shrink capacity if under threshold
    if (arr->size > 0 && arr->size < arr->capacity / DA_SHRINK_THRESHOLD) {
        size_t new_capacity = arr->capacity / DA_GROWTH_FACTOR;
        if (new_capacity < DA_MIN_CAPACITY) new_capacity = DA_MIN_CAPACITY;
        da_resize(arr, new_capacity);
    }

    return 1;
}

// ------------------------------------------------------------
// Insert at index
// ------------------------------------------------------------
int da_insert(dynarray_t *arr, size_t index, const void *value) {
    if (!arr || !value || index > arr->size) return 0;

    if (arr->size == arr->capacity) {
        if (!da_resize(arr, arr->capacity * DA_GROWTH_FACTOR)) return 0;
    }

    void *dest = (char *)arr->data + (index + 1) * arr->elem_size;
    void *src  = (char *)arr->data + index * arr->elem_size;

    memmove(dest, src, (arr->size - index) * arr->elem_size);
    memcpy(src, value, arr->elem_size);

    arr->size++;
    return 1;
}

// ------------------------------------------------------------
// Remove at index
// ------------------------------------------------------------
int da_remove(dynarray_t *arr, size_t index) {
    if (!arr || index >= arr->size) return 0;

    void *element = (char *)arr->data + index * arr->elem_size;

    if (arr->free_fn)
        arr->free_fn(element);

    void *dest = element;
    void *src = (char *)element + arr->elem_size;

    memmove(dest, src, (arr->size - index - 1) * arr->elem_size);

    arr->size--;

    // shrink if needed
    if (arr->size > 0 && arr->size < arr->capacity / DA_SHRINK_THRESHOLD) {
        size_t new_capacity = arr->capacity / DA_GROWTH_FACTOR;
        if (new_capacity < DA_MIN_CAPACITY) new_capacity = DA_MIN_CAPACITY;
        da_resize(arr, new_capacity);
    }

    return 1;
}
