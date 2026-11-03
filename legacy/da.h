#define DA_INIT_CAP 256
#define da_reserve(da, expected_capacity)                                                  \
    do {                                                                                   \
        if ((expected_capacity) > (da)->capacity) {                                        \
            if ((da)->capacity == 0) {                                                     \
                (da)->capacity = DA_INIT_CAP;                                              \
            }                                                                              \
            while ((expected_capacity) > (da)->capacity) {                                 \
                (da)->capacity *= 2;                                                       \
            }                                                                              \
            (da)->items = realloc((da)->items, (da)->capacity * sizeof(*(da)->items));     \
            assert((da)->items != NULL && "Buy more RAM lol");                             \
        }                                                                                  \
    } while (0)

#define da_append(da, item)                  \
    do {                                     \
        da_reserve((da), (da)->count + 1);   \
        (da)->items[(da)->count++] = (item); \
    } while (0)

#define da_delete_at(da, i) \
    do { \
       size_t index = (i); \
       assert(index < (da)->count); \
       memmove(&(da)->items[index], &(da)->items[index + 1], ((da)->count - index - 1)*sizeof(*(da)->items)); \
       (da)->count -= 1; \
    } while(0)

#define da_free(da) free((da).items)

// Append several items to a dynamic array
#define da_append_many(da, new_items, new_items_count)                                      \
    do {                                                                                        \
        da_reserve((da), (da)->count + (new_items_count));                                  \
        memcpy((da)->items + (da)->count, (new_items), (new_items_count)*sizeof(*(da)->items)); \
        (da)->count += (new_items_count);                                                       \
    } while (0)

#define da_resize(da, new_size)     \
    do {                                \
        da_reserve((da), new_size); \
        (da)->count = (new_size);       \
    } while (0)

#define da_pop(da) (da)->items[(assert((da)->count > 0), --(da)->count)]
#define da_first(da) (da)->items[(assert((da)->count > 0), 0)]
#define da_last(da) (da)->items[(assert((da)->count > 0), (da)->count-1)]
#define da_remove_unordered(da, i)               \
    do {                                             \
        size_t j = (i);                              \
        assert(j < (da)->count);                 \
        (da)->items[j] = (da)->items[--(da)->count]; \
    } while(0)

// Foreach over Dynamic Arrays. Example:
// ```c
// typedef struct {
//     int *items;
//     size_t count;
//     size_t capacity;
// } Numbers;
//
// Numbers xs = {0};
//
// da_append(&xs, 69);
// da_append(&xs, 420);
// da_append(&xs, 1337);
//
// da_foreach(int, x, &xs) {
//     // `x` here is a pointer to the current element. You can get its index by taking a difference
//     // between `x` and the start of the array which is `x.items`.
//     size_t index = x - xs.items;
//     log(INFO, "%zu: %d", index, *x);
// }
// ```
#define da_foreach(Type, it, da) for (Type *it = (da)->items; it < (da)->items + (da)->count; ++it)

