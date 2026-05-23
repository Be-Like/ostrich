#include "spsc_ring.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct SpscRing {
    size_t          elem_size;
    size_t          cap;
    size_t          mask;
    _Atomic size_t  head; /* consumer index — only consumer writes */
    _Atomic size_t  tail; /* producer index — only producer writes */
    char           *buf;
};

SpscRing *spsc_create(size_t elem_size, size_t capacity_pow2) {
    if (elem_size == 0 || capacity_pow2 == 0) return NULL;
    /* verify power of two */
    if ((capacity_pow2 & (capacity_pow2 - 1)) != 0) return NULL;

    SpscRing *r = malloc(sizeof(SpscRing));
    if (!r) return NULL;

    r->buf = malloc(elem_size * capacity_pow2);
    if (!r->buf) {
        free(r);
        return NULL;
    }

    r->elem_size = elem_size;
    r->cap       = capacity_pow2;
    r->mask      = capacity_pow2 - 1;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return r;
}

bool spsc_push(SpscRing *r, const void *elem) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (tail - head >= r->cap) return false;
    memcpy(r->buf + (tail & r->mask) * r->elem_size, elem, r->elem_size);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return true;
}

bool spsc_pop(SpscRing *r, void *out) {
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head == tail) return false;
    memcpy(out, r->buf + (head & r->mask) * r->elem_size, r->elem_size);
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    return true;
}

void spsc_destroy(SpscRing *r) {
    if (!r) return;
    free(r->buf);
    free(r);
}
