#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Arena Arena;

/* Reserve `cap` bytes up front. Returns NULL on OOM. */
Arena *arena_create(size_t cap);

/* Bump-allocate `size` bytes aligned to `align`.
   Returns NULL if the arena is exhausted. */
void *arena_alloc(Arena *a, size_t size, size_t align);

/* Roll the arena back to empty; memory is reused, not freed. */
void arena_reset(Arena *a);

/* Release the whole arena. */
void arena_destroy(Arena *a);

#ifdef __cplusplus
}
#endif

#endif /* ARENA_H */
