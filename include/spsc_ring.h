#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SpscRing SpscRing;

/* Allocate a lock-free SPSC ring for elements of `elem_size` bytes.
   `capacity_pow2` must be a power of two (e.g. 8, 16, 32).
   Returns NULL on OOM or invalid capacity. */
SpscRing *spsc_create(size_t elem_size, size_t capacity_pow2);

/* Copy `elem` into the ring. Returns false if the ring is full. */
bool spsc_push(SpscRing *r, const void *elem);

/* Copy the oldest element into `out`. Returns false if the ring is empty. */
bool spsc_pop(SpscRing *r, void *out);

/* Free the ring. */
void spsc_destroy(SpscRing *r);

#ifdef __cplusplus
}
#endif

#endif /* SPSC_RING_H */
