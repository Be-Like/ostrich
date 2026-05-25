#include "../include/logbuf.h"
#include "../include/arena.h"
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* Fresh arena per test — each call returns a new arena with 64 KB. */
static Arena *new_arena(void) { return arena_create(65536); }

/* ── tests ─────────────────────────────────────────────────────────── */

static int test_empty(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    ASSERT("init non-null",   lb != NULL);
    ASSERT("count zero",      logbuf_count(lb) == 0);
    ASSERT("line oob empty",  strcmp(logbuf_line(lb, 0, NULL), "") == 0);
    arena_destroy(a);
    PASS("empty");
    return 0;
}

static int test_single_line(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "hello\n", 6);
    ASSERT("count 1", logbuf_count(lb) == 1);
    size_t len;
    const char *s = logbuf_line(lb, 0, &len);
    ASSERT("content",   strcmp(s, "hello") == 0);
    ASSERT("len 5",     len == 5);
    ASSERT("oob empty", strcmp(logbuf_line(lb, 1, NULL), "") == 0);
    arena_destroy(a);
    PASS("single_line");
    return 0;
}

static int test_multi_line_one_chunk(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "a\nb\nc\n", 6);
    ASSERT("count 3",  logbuf_count(lb) == 3);
    ASSERT("line 0",   strcmp(logbuf_line(lb, 0, NULL), "a") == 0);
    ASSERT("line 1",   strcmp(logbuf_line(lb, 1, NULL), "b") == 0);
    ASSERT("line 2",   strcmp(logbuf_line(lb, 2, NULL), "c") == 0);
    arena_destroy(a);
    PASS("multi_line_one_chunk");
    return 0;
}

static int test_partial_across_calls(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    /* Split "hello\n" across two calls: "hel" then "lo\n" */
    logbuf_append(lb, "hel", 3);
    ASSERT("no line yet", logbuf_count(lb) == 0);
    logbuf_append(lb, "lo\n", 3);
    ASSERT("count 1", logbuf_count(lb) == 1);
    ASSERT("content", strcmp(logbuf_line(lb, 0, NULL), "hello") == 0);
    arena_destroy(a);
    PASS("partial_across_calls");
    return 0;
}

static int test_partial_not_counted(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "no newline", 10);
    ASSERT("partial not counted", logbuf_count(lb) == 0);
    arena_destroy(a);
    PASS("partial_not_counted");
    return 0;
}

static int test_cr_stripped(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "ok\r\n", 4);
    ASSERT("count 1",          logbuf_count(lb) == 1);
    ASSERT("no CR in line",    strcmp(logbuf_line(lb, 0, NULL), "ok") == 0);
    arena_destroy(a);
    PASS("cr_stripped");
    return 0;
}

static int test_line_cap_drops_oldest(void)
{
    Arena *a = new_arena();
    /* max 3 lines */
    LogBuf *lb = logbuf_init(a, 1024, 3);
    logbuf_append(lb, "one\ntwo\nthree\nfour\n", 19);
    ASSERT("count capped at 3", logbuf_count(lb) == 3);
    /* oldest ("one") was dropped */
    ASSERT("oldest dropped",    strcmp(logbuf_line(lb, 0, NULL), "two") == 0);
    ASSERT("line 1",            strcmp(logbuf_line(lb, 1, NULL), "three") == 0);
    ASSERT("line 2",            strcmp(logbuf_line(lb, 2, NULL), "four") == 0);
    arena_destroy(a);
    PASS("line_cap_drops_oldest");
    return 0;
}

static int test_byte_cap_drops_oldest(void)
{
    Arena *a = new_arena();
    /* byte_cap=20, max_lines=16: force byte drops */
    LogBuf *lb = logbuf_init(a, 20, 16);
    /* Each line "AAAA...A\n" (10 chars) stored as 10 content + NUL = 11 bytes.
       Two such lines need 22 bytes, exceeding the 20-byte cap.
       So after the second line the first must be dropped. */
    logbuf_append(lb, "AAAAAAAAAA\n", 11); /* 10 content bytes + NUL = 11 bytes used */
    logbuf_append(lb, "BBBBBBBBBB\n", 11); /* needs 11 more; 11+11=22 > 20: drops A */
    ASSERT("count 1",      logbuf_count(lb) == 1);
    ASSERT("oldest gone",  strstr(logbuf_line(lb, 0, NULL), "BBBBBBBBBB") != NULL);
    arena_destroy(a);
    PASS("byte_cap_drops_oldest");
    return 0;
}

static int test_surviving_lines_not_truncated(void)
{
    Arena *a = new_arena();
    /* cap=30, max=16 */
    LogBuf *lb = logbuf_init(a, 30, 16);
    /* Fill with lines so oldest get dropped; surviving lines must be complete. */
    logbuf_append(lb, "AAAAAAAAAA\n", 11);
    logbuf_append(lb, "BBBBBBBBBB\n", 11);
    logbuf_append(lb, "CCCCCCCCCC\n", 11);
    /* With cap=30: A+B=22 fits; adding C=11 more needs 33 > 30 → drop A.
       After drop A: B+C = 22 fits. */
    ASSERT("count 2", logbuf_count(lb) == 2);
    size_t la, lb2;
    const char *sa = logbuf_line(lb, 0, &la);
    const char *sb = logbuf_line(lb, 1, &lb2);
    /* Each surviving line must be exactly 10 bytes (full, not truncated). */
    ASSERT("line 0 full", la == 10);
    ASSERT("line 1 full", lb2 == 10);
    ASSERT("line 0 content", strncmp(sa, "BBBBBBBBBB", 10) == 0);
    ASSERT("line 1 content", strncmp(sb, "CCCCCCCCCC", 10) == 0);
    arena_destroy(a);
    PASS("surviving_lines_not_truncated");
    return 0;
}

static int test_mark_no_partial(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_mark(lb, "> -- MARK --");
    ASSERT("count 1",   logbuf_count(lb) == 1);
    ASSERT("mark line", strcmp(logbuf_line(lb, 0, NULL), "> -- MARK --") == 0);
    arena_destroy(a);
    PASS("mark_no_partial");
    return 0;
}

static int test_mark_flushes_partial(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    /* Partial "hello" not yet terminated. */
    logbuf_append(lb, "hello", 5);
    logbuf_mark(lb, "> -- MARK --");
    ASSERT("count 2",         logbuf_count(lb) == 2);
    ASSERT("partial flushed", strcmp(logbuf_line(lb, 0, NULL), "hello") == 0);
    ASSERT("mark after",      strcmp(logbuf_line(lb, 1, NULL), "> -- MARK --") == 0);
    arena_destroy(a);
    PASS("mark_flushes_partial");
    return 0;
}

static int test_clear(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "line1\nline2\n", 12);
    logbuf_append(lb, "partial", 7);
    logbuf_clear(lb);
    ASSERT("count 0 after clear", logbuf_count(lb) == 0);
    /* Can still append after clear. */
    logbuf_append(lb, "fresh\n", 6);
    ASSERT("count 1 after refill", logbuf_count(lb) == 1);
    ASSERT("content fresh",        strcmp(logbuf_line(lb, 0, NULL), "fresh") == 0);
    arena_destroy(a);
    PASS("clear");
    return 0;
}

static int test_copy_all(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "foo\nbar\nbaz\n", 12);
    /* Compute needed size. */
    size_t need = logbuf_copy_all(lb, NULL, 0);
    /* "foo\nbar\nbaz\n" = 12 bytes */
    ASSERT("needed 12", need == 12);
    char buf[64];
    size_t n = logbuf_copy_all(lb, buf, sizeof(buf));
    ASSERT("returns same", n == 12);
    buf[n] = '\0';
    ASSERT("content", strcmp(buf, "foo\nbar\nbaz\n") == 0);
    arena_destroy(a);
    PASS("copy_all");
    return 0;
}

static int test_copy_all_cap_exceeded(void)
{
    Arena *a = new_arena();
    LogBuf *lb = logbuf_init(a, 256, 16);
    logbuf_append(lb, "foo\nbar\nbaz\n", 12);
    char small[5];
    size_t need = logbuf_copy_all(lb, small, sizeof(small));
    /* Returns full needed size even when cap is too small. */
    ASSERT("needed > cap", need > sizeof(small));
    ASSERT("needed 12",    need == 12);
    arena_destroy(a);
    PASS("copy_all_cap_exceeded");
    return 0;
}

static int test_ring_wrap(void)
{
    Arena *a = new_arena();
    /* max_lines=4; add 6 lines → oldest 2 dropped, ring index wraps. */
    LogBuf *lb = logbuf_init(a, 1024, 4);
    logbuf_append(lb, "1\n2\n3\n4\n5\n6\n", 12);
    ASSERT("count 4",  logbuf_count(lb) == 4);
    ASSERT("line 0",   strcmp(logbuf_line(lb, 0, NULL), "3") == 0);
    ASSERT("line 1",   strcmp(logbuf_line(lb, 1, NULL), "4") == 0);
    ASSERT("line 2",   strcmp(logbuf_line(lb, 2, NULL), "5") == 0);
    ASSERT("line 3",   strcmp(logbuf_line(lb, 3, NULL), "6") == 0);
    arena_destroy(a);
    PASS("ring_wrap");
    return 0;
}

static int test_byte_and_line_cap_together(void)
{
    Arena *a = new_arena();
    /* cap=50, max=3 */
    LogBuf *lb = logbuf_init(a, 50, 3);
    logbuf_append(lb, "AAAA\nBBBB\nCCCC\nDDDD\nEEEE\n", 25);
    ASSERT("count <= 3", logbuf_count(lb) <= 3);
    /* Last line must always be "EEEE". */
    int c = logbuf_count(lb);
    ASSERT("last is EEEE", strcmp(logbuf_line(lb, c - 1, NULL), "EEEE") == 0);
    arena_destroy(a);
    PASS("byte_and_line_cap_together");
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(void)
{
    int f = 0;
    f += test_empty();
    f += test_single_line();
    f += test_multi_line_one_chunk();
    f += test_partial_across_calls();
    f += test_partial_not_counted();
    f += test_cr_stripped();
    f += test_line_cap_drops_oldest();
    f += test_byte_cap_drops_oldest();
    f += test_surviving_lines_not_truncated();
    f += test_mark_no_partial();
    f += test_mark_flushes_partial();
    f += test_clear();
    f += test_copy_all();
    f += test_copy_all_cap_exceeded();
    f += test_ring_wrap();
    f += test_byte_and_line_cap_together();

    if (f == 0) {
        printf("All logbuf tests passed.\n");
        return 0;
    }
    printf("%d logbuf test(s) failed.\n", f);
    return 1;
}
