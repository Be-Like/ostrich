#ifndef LOGBUF_H
#define LOGBUF_H

#include <stddef.h>
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LogBuf LogBuf;

/* Allocate a LogBuf from `a`.  Bounds the byte store to `byte_cap` bytes and
   the line count to `max_lines`; drops whole oldest lines when either cap is
   hit.  Lines are never truncated.  Returns NULL on arena exhaustion. */
LogBuf *logbuf_init(Arena *a, size_t byte_cap, int max_lines);

/* Append raw bytes; assembles lines incrementally, carrying a partial line
   across calls.  Splits on '\n'; '\r' is stripped. */
void logbuf_append(LogBuf *lb, const char *bytes, size_t n);

/* Flush any pending partial line, then insert `line` as a complete line. */
void logbuf_mark(LogBuf *lb, const char *line);

/* Discard all lines and the pending partial. */
void logbuf_clear(LogBuf *lb);

/* Number of complete lines currently stored. */
int logbuf_count(const LogBuf *lb);

/* Borrowed NUL-terminated view of line `i` for rendering.
   `*out_len` is set to the line byte length (not including NUL).
   Returns "" with *out_len == 0 when i is out of range. */
const char *logbuf_line(const LogBuf *lb, int i, size_t *out_len);

/* Flatten all lines into `out`, separated by '\n'.
   Returns bytes needed (may exceed `cap`); writes at most `cap` bytes.
   Safe to call with out==NULL / cap==0 to probe the required size. */
size_t logbuf_copy_all(const LogBuf *lb, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LOGBUF_H */
