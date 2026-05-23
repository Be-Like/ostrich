#include "arena.h"
#include <stdlib.h>

struct Arena {
    char   *data;
    size_t  cap;
    size_t  pos;
};

Arena *arena_create(size_t cap) {
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->data = malloc(cap);
    if (!a->data) {
        free(a);
        return NULL;
    }
    a->cap = cap;
    a->pos = 0;
    return a;
}

void *arena_alloc(Arena *a, size_t size, size_t align) {
    size_t aligned = (a->pos + align - 1) & ~(align - 1);
    if (aligned + size > a->cap) return NULL;
    a->pos = aligned + size;
    return a->data + aligned;
}

void arena_reset(Arena *a) {
    a->pos = 0;
}

void arena_destroy(Arena *a) {
    if (!a) return;
    free(a->data);
    free(a);
}
