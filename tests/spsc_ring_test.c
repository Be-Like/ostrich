#include "../include/spsc_ring.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* push/pop a single int */
static int test_fifo_ordering(void) {
    SpscRing *r = spsc_create(sizeof(int), 8);
    ASSERT("create", r != NULL);

    int vals[] = {10, 20, 30, 40, 50};
    for (int i = 0; i < 5; i++)
        ASSERT("push", spsc_push(r, &vals[i]));

    for (int i = 0; i < 5; i++) {
        int got = -1;
        ASSERT("pop ok", spsc_pop(r, &got));
        ASSERT("fifo order", got == vals[i]);
    }

    spsc_destroy(r);
    PASS("fifo_ordering");
    return 0;
}

/* fill the ring, drain it, refill — indices must wrap */
static int test_wraparound(void) {
    SpscRing *r = spsc_create(sizeof(int), 4);
    ASSERT("create", r != NULL);

    /* two full cycles to exercise index wraparound */
    for (int cycle = 0; cycle < 2; cycle++) {
        for (int i = 0; i < 4; i++)
            ASSERT("push in cycle", spsc_push(r, &i));

        for (int i = 0; i < 4; i++) {
            int got = -1;
            ASSERT("pop in cycle", spsc_pop(r, &got));
            ASSERT("wraparound value", got == i);
        }
    }

    spsc_destroy(r);
    PASS("wraparound");
    return 0;
}

/* pop on empty ring returns false */
static int test_empty_edge(void) {
    SpscRing *r = spsc_create(sizeof(int), 4);
    ASSERT("create", r != NULL);

    int out = 0;
    ASSERT("pop empty returns false", !spsc_pop(r, &out));

    /* push one, pop it, then pop again */
    int v = 7;
    ASSERT("push", spsc_push(r, &v));
    ASSERT("pop ok", spsc_pop(r, &out));
    ASSERT("pop again empty", !spsc_pop(r, &out));

    spsc_destroy(r);
    PASS("empty_edge");
    return 0;
}

/* push on full ring returns false */
static int test_full_edge(void) {
    SpscRing *r = spsc_create(sizeof(int), 4);
    ASSERT("create", r != NULL);

    int v = 1;
    ASSERT("push 1", spsc_push(r, &v));
    ASSERT("push 2", spsc_push(r, &v));
    ASSERT("push 3", spsc_push(r, &v));
    ASSERT("push 4", spsc_push(r, &v));
    ASSERT("push 5 full returns false", !spsc_push(r, &v));

    /* drain one slot, now one push should succeed */
    int out = 0;
    ASSERT("pop to make room", spsc_pop(r, &out));
    ASSERT("push after drain", spsc_push(r, &v));
    ASSERT("push when full again returns false", !spsc_push(r, &v));

    spsc_destroy(r);
    PASS("full_edge");
    return 0;
}

/* capacity-1 is the ring's last valid slot; exactly cap pushes fill it */
static int test_capacity_boundary(void) {
    const size_t cap = 2;
    SpscRing *r = spsc_create(sizeof(uint32_t), cap);
    ASSERT("create cap=2", r != NULL);

    uint32_t a = 0xAAu, b = 0xBBu;
    ASSERT("push a", spsc_push(r, &a));
    ASSERT("push b", spsc_push(r, &b));

    /* full — can't push */
    uint32_t extra = 0xCCu;
    ASSERT("push to full returns false", !spsc_push(r, &extra));

    uint32_t out = 0;
    ASSERT("pop a", spsc_pop(r, &out));
    ASSERT("value a", out == a);
    ASSERT("pop b", spsc_pop(r, &out));
    ASSERT("value b", out == b);
    ASSERT("empty after drain", !spsc_pop(r, &out));

    spsc_destroy(r);
    PASS("capacity_boundary");
    return 0;
}

/* spsc_create rejects non-power-of-two capacity */
static int test_invalid_capacity(void) {
    SpscRing *r = spsc_create(sizeof(int), 3);
    ASSERT("non-pow2 returns NULL", r == NULL);

    SpscRing *r2 = spsc_create(sizeof(int), 0);
    ASSERT("zero capacity returns NULL", r2 == NULL);

    SpscRing *r3 = spsc_create(0, 8);
    ASSERT("zero elem_size returns NULL", r3 == NULL);

    PASS("invalid_capacity");
    return 0;
}

/* push structs, not just ints */
static int test_struct_elements(void) {
    typedef struct { int x; int y; } Point;

    SpscRing *r = spsc_create(sizeof(Point), 8);
    ASSERT("create", r != NULL);

    Point pts[] = {{1, 2}, {3, 4}, {5, 6}};
    for (int i = 0; i < 3; i++)
        ASSERT("push point", spsc_push(r, &pts[i]));

    for (int i = 0; i < 3; i++) {
        Point got = {0, 0};
        ASSERT("pop point ok", spsc_pop(r, &got));
        ASSERT("point x matches", got.x == pts[i].x);
        ASSERT("point y matches", got.y == pts[i].y);
    }

    spsc_destroy(r);
    PASS("struct_elements");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_fifo_ordering();
    failures += test_wraparound();
    failures += test_empty_edge();
    failures += test_full_edge();
    failures += test_capacity_boundary();
    failures += test_invalid_capacity();
    failures += test_struct_elements();

    if (failures == 0) {
        printf("All spsc_ring tests passed.\n");
        return 0;
    }
    printf("%d spsc_ring test(s) failed.\n", failures);
    return 1;
}
