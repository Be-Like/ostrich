#include "logbuf.h"
#include <string.h>
#include <stdalign.h>

typedef struct {
    int off; /* byte offset into store */
    int len; /* content length, not including NUL */
} LineSlot;

struct LogBuf {
    char     *store;      /* NUL-terminated lines packed here */
    int       byte_cap;   /* usable capacity */
    int       store_end;  /* next free byte index */

    LineSlot *lines;      /* ring of line descriptors, max_lines slots */
    int       max_lines;
    int       head;       /* ring head: index of oldest line */
    int       count;      /* number of complete lines */

    char     *partial;    /* accumulator for the current incomplete line */
    int       partial_len;
};

/* ── internal helpers ──────────────────────────────────────────────────── */

/* After dropping lines from the head, compact the byte store so that the
   oldest surviving line's data begins at offset 0. */
static void compact(LogBuf *lb)
{
    if (lb->count == 0) {
        lb->store_end = 0;
        return;
    }
    int first_off = lb->lines[lb->head].off;
    if (first_off == 0) return;
    int data_len = lb->store_end - first_off;
    memmove(lb->store, lb->store + first_off, (size_t)data_len);
    for (int i = 0; i < lb->count; i++)
        lb->lines[(lb->head + i) % lb->max_lines].off -= first_off;
    lb->store_end = data_len;
}

/* Drop the oldest complete line (does not compact). */
static void drop_oldest(LogBuf *lb)
{
    if (lb->count == 0) return;
    lb->head = (lb->head + 1) % lb->max_lines;
    lb->count--;
}

/* Drop oldest lines until the store would have `need` free bytes after
   compaction, then compact once. */
static void make_space(LogBuf *lb, int need)
{
    while (lb->count > 0) {
        int first_off = lb->lines[lb->head].off;
        int used      = lb->store_end - first_off;
        if (used + need <= lb->byte_cap) break;
        drop_oldest(lb);
    }
    compact(lb);
}

/* Write `text` of `len` bytes as a complete line, enforcing both caps. */
static void commit_line(LogBuf *lb, const char *text, int len)
{
    int need = len + 1; /* content + NUL */
    if (need > lb->byte_cap) return; /* line too long to ever store; discard */

    /* Enforce line cap first (drop one oldest line if full). */
    if (lb->count >= lb->max_lines)
        drop_oldest(lb);

    /* Enforce byte cap (may drop additional lines, then compact). */
    make_space(lb, need);

    /* Write line into store. */
    int off = lb->store_end;
    memcpy(lb->store + off, text, (size_t)len);
    lb->store[off + len] = '\0';
    lb->store_end += need;

    int slot = (lb->head + lb->count) % lb->max_lines;
    lb->lines[slot].off = off;
    lb->lines[slot].len = len;
    lb->count++;
}

/* ── public API ─────────────────────────────────────────────────────────── */

LogBuf *logbuf_init(Arena *a, size_t byte_cap, int max_lines)
{
    LogBuf *lb = arena_alloc(a, sizeof(LogBuf), alignof(LogBuf));
    if (!lb) return NULL;

    lb->store = arena_alloc(a, byte_cap + 1, 1);
    if (!lb->store) return NULL;

    lb->lines = arena_alloc(a, (size_t)max_lines * sizeof(LineSlot),
                            alignof(LineSlot));
    if (!lb->lines) return NULL;

    lb->partial = arena_alloc(a, byte_cap, 1);
    if (!lb->partial) return NULL;

    lb->byte_cap    = (int)byte_cap;
    lb->max_lines   = max_lines;
    lb->store_end   = 0;
    lb->head        = 0;
    lb->count       = 0;
    lb->partial_len = 0;
    return lb;
}

void logbuf_append(LogBuf *lb, const char *bytes, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = bytes[i];
        if (c == '\n') {
            commit_line(lb, lb->partial, lb->partial_len);
            lb->partial_len = 0;
        } else if (c != '\r') {
            if (lb->partial_len < lb->byte_cap)
                lb->partial[lb->partial_len++] = c;
        }
    }
}

void logbuf_mark(LogBuf *lb, const char *line)
{
    if (lb->partial_len > 0) {
        commit_line(lb, lb->partial, lb->partial_len);
        lb->partial_len = 0;
    }
    commit_line(lb, line, (int)strlen(line));
}

void logbuf_clear(LogBuf *lb)
{
    lb->head        = 0;
    lb->count       = 0;
    lb->store_end   = 0;
    lb->partial_len = 0;
}

int logbuf_count(const LogBuf *lb)
{
    return lb->count;
}

const char *logbuf_line(const LogBuf *lb, int i, size_t *out_len)
{
    if (i < 0 || i >= lb->count) {
        if (out_len) *out_len = 0;
        return "";
    }
    int slot = (lb->head + i) % lb->max_lines;
    if (out_len) *out_len = (size_t)lb->lines[slot].len;
    return lb->store + lb->lines[slot].off;
}

size_t logbuf_copy_all(const LogBuf *lb, char *out, size_t cap)
{
    size_t needed  = 0;
    size_t written = 0;
    for (int i = 0; i < lb->count; i++) {
        int    slot  = (lb->head + i) % lb->max_lines;
        size_t len   = (size_t)lb->lines[slot].len;
        size_t total = len + 1; /* content + '\n' */
        needed += total;
        if (out && written + total <= cap) {
            memcpy(out + written, lb->store + lb->lines[slot].off, len);
            out[written + len] = '\n';
            written += total;
        }
    }
    return needed;
}
