#include "../include/arena.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

static int test_basic_alloc(void) {
    Arena *a = arena_create(256);
    ASSERT("create returns non-null", a != NULL);

    void *p = arena_alloc(a, 16, 1);
    ASSERT("alloc returns non-null", p != NULL);

    void *q = arena_alloc(a, 16, 1);
    ASSERT("second alloc returns non-null", q != NULL);
    ASSERT("second alloc does not overlap first", (char *)q >= (char *)p + 16);

    arena_destroy(a);
    PASS("basic_alloc");
    return 0;
}

static int test_alignment(void) {
    Arena *a = arena_create(1024);
    ASSERT("create", a != NULL);

    /* alloc 1 byte to misalign the cursor */
    void *p1 = arena_alloc(a, 1, 1);
    ASSERT("misalign alloc", p1 != NULL);

    /* next alloc with align=8 must be 8-byte aligned */
    void *p8 = arena_alloc(a, 8, 8);
    ASSERT("aligned alloc returns non-null", p8 != NULL);
    ASSERT("pointer is 8-byte aligned", ((size_t)p8 % 8) == 0);

    /* align=16 */
    void *p16 = arena_alloc(a, 4, 16);
    ASSERT("aligned alloc 16 returns non-null", p16 != NULL);
    ASSERT("pointer is 16-byte aligned", ((size_t)p16 % 16) == 0);

    arena_destroy(a);
    PASS("alignment");
    return 0;
}

static int test_reset_reuse(void) {
    Arena *a = arena_create(128);
    ASSERT("create", a != NULL);

    void *p = arena_alloc(a, 64, 1);
    ASSERT("first alloc", p != NULL);

    /* write a pattern to confirm memory is reusable */
    memset(p, 0xAB, 64);

    arena_reset(a);

    /* after reset, should get the same base pointer back */
    void *q = arena_alloc(a, 64, 1);
    ASSERT("post-reset alloc", q != NULL);
    ASSERT("post-reset alloc at same start", q == p);

    arena_destroy(a);
    PASS("reset_reuse");
    return 0;
}

static int test_exhaustion(void) {
    Arena *a = arena_create(32);
    ASSERT("create", a != NULL);

    void *p = arena_alloc(a, 16, 1);
    ASSERT("first alloc fits", p != NULL);

    void *q = arena_alloc(a, 16, 1);
    ASSERT("second alloc fits", q != NULL);

    /* arena now full; next alloc must return NULL */
    void *r = arena_alloc(a, 1, 1);
    ASSERT("exhausted returns NULL", r == NULL);

    arena_destroy(a);
    PASS("exhaustion");
    return 0;
}

static int test_exact_fit(void) {
    Arena *a = arena_create(16);
    ASSERT("create", a != NULL);

    void *p = arena_alloc(a, 16, 1);
    ASSERT("exact fit alloc", p != NULL);

    void *q = arena_alloc(a, 1, 1);
    ASSERT("one byte past exact fit returns NULL", q == NULL);

    arena_destroy(a);
    PASS("exact_fit");
    return 0;
}

static int test_reset_then_refill(void) {
    Arena *a = arena_create(64);
    ASSERT("create", a != NULL);

    void *p = arena_alloc(a, 64, 1);
    ASSERT("fill arena", p != NULL);

    void *overflow = arena_alloc(a, 1, 1);
    ASSERT("overflow returns NULL before reset", overflow == NULL);

    arena_reset(a);

    void *after = arena_alloc(a, 64, 1);
    ASSERT("refill after reset", after != NULL);

    arena_destroy(a);
    PASS("reset_then_refill");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_basic_alloc();
    failures += test_alignment();
    failures += test_reset_reuse();
    failures += test_exhaustion();
    failures += test_exact_fit();
    failures += test_reset_then_refill();

    if (failures == 0) {
        printf("All arena tests passed.\n");
        return 0;
    }
    printf("%d arena test(s) failed.\n", failures);
    return 1;
}
