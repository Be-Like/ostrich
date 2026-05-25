#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OSTRICH_DEBUG

LogStatus log_init(void)                                           { return LOG_OK; }
void      log_shutdown(void)                                       {}
void      log_set_thread_tag(const char *tag)                      { (void)tag; }
void      log_emit(LogLevel lv, LogSubsys s, const char *f, ...)   { (void)lv; (void)s; (void)f; }
void      log_blob(LogLevel lv, LogSubsys s, const char *l,
                   const char *d, size_t n)                        { (void)lv; (void)s; (void)l; (void)d; (void)n; }
const char *log_status_str(LogStatus s) {
    return s == LOG_OK ? "ok" : "err:file";
}

#else /* OSTRICH_DEBUG */

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX  2048
#define LOG_BLOB_BUF  65536
#define LOG_TAG_LEN   8

static const char *s_lev[] = { "ERROR", "WARN",  "INFO",  "DEBUG", "TRACE" };
static const char *s_sub[] = { "app",   "ssh",   "conn",  "sess",  "disc", "store", "ui", "run" };

typedef struct {
    int             fd;
    int             level;
    int             mirror;
    struct timespec t0;
} LogState;

static LogState g_log = { .fd = -1, .level = LOG_INFO, .mirror = 0 };

_Thread_local static char thr_tag[LOG_TAG_LEN] = "main";

static int parse_level(const char *s) {
    if (!s) return LOG_INFO;
    char low[16] = {0};
    for (int i = 0; s[i] && i < 15; i++)
        low[i] = (char)tolower((unsigned char)s[i]);
    if (strcmp(low, "error") == 0) return LOG_ERROR;
    if (strcmp(low, "warn")  == 0) return LOG_WARN;
    if (strcmp(low, "info")  == 0) return LOG_INFO;
    if (strcmp(low, "debug") == 0) return LOG_DEBUG;
    if (strcmp(low, "trace") == 0) return LOG_TRACE;
    return LOG_INFO;
}

static void make_dirs(const char *path) {
    char tmp[512];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

static void resolve_path(char *buf, size_t buflen) {
    const char *ov = getenv("OSTRICH_LOG_FILE");
    if (ov) {
        snprintf(buf, buflen, "%s", ov);
        return;
    }
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg) {
        snprintf(buf, buflen, "%s/ostrich/ostrich.log", xdg);
    } else {
        const char *home = getenv("HOME");
        snprintf(buf, buflen, "%s/.local/state/ostrich/ostrich.log",
                 home ? home : "/tmp");
    }
}

LogStatus log_init(void) {
    char path[512];
    resolve_path(path, sizeof(path));

    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        make_dirs(dir);
    }

    char path1[520];
    snprintf(path1, sizeof(path1), "%s.1", path);
    rename(path, path1);

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return LOG_ERR_FILE;

    const char *stderr_ev = getenv("OSTRICH_LOG_STDERR");

    g_log.fd     = fd;
    g_log.level  = parse_level(getenv("OSTRICH_LOG"));
    g_log.mirror = (stderr_ev && strcmp(stderr_ev, "1") == 0) ? 1 : 0;
    clock_gettime(CLOCK_MONOTONIC, &g_log.t0);

    snprintf(thr_tag, sizeof(thr_tag), "main");
    return LOG_OK;
}

void log_shutdown(void) {
    if (g_log.fd >= 0) {
        close(g_log.fd);
        g_log.fd = -1;
    }
}

void log_set_thread_tag(const char *tag) {
    snprintf(thr_tag, sizeof(thr_tag), "%s", tag);
}

static int format_hdr(char *buf, size_t buflen, LogLevel lv, LogSubsys sub) {
    struct timespec tw, tm_mon;
    clock_gettime(CLOCK_REALTIME,  &tw);
    clock_gettime(CLOCK_MONOTONIC, &tm_mon);

    struct tm tm_wall;
    localtime_r(&tw.tv_sec, &tm_wall);
    long ms = tw.tv_nsec / 1000000L;

    double delta = (double)(tm_mon.tv_sec  - g_log.t0.tv_sec) +
                   (double)(tm_mon.tv_nsec - g_log.t0.tv_nsec) * 1e-9;

    return snprintf(buf, buflen,
        "%04d-%02d-%02dT%02d:%02d:%02d.%03ld +%.3fs %-5s [%s] [%s] ",
        tm_wall.tm_year + 1900, tm_wall.tm_mon + 1, tm_wall.tm_mday,
        tm_wall.tm_hour, tm_wall.tm_min, tm_wall.tm_sec, ms,
        delta,
        s_lev[lv],
        s_sub[sub],
        thr_tag);
}

static void emit_buf(const char *buf, int len) {
    if (len <= 0) return;
    write(g_log.fd, buf, (size_t)len);
    if (g_log.mirror) write(STDERR_FILENO, buf, (size_t)len);
}

void log_emit(LogLevel lv, LogSubsys sub, const char *fmt, ...) {
    if (g_log.fd < 0 || (int)lv > g_log.level) return;

    char buf[LOG_LINE_MAX];
    int n = format_hdr(buf, sizeof(buf), lv, sub);
    if (n < 0 || n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 2;

    va_list ap;
    va_start(ap, fmt);
    int rem = (int)sizeof(buf) - n - 1;
    if (rem > 0) {
        int w = vsnprintf(buf + n, (size_t)rem, fmt, ap);
        if (w > 0) n += (w < rem ? w : rem - 1);
    }
    va_end(ap);

    if (n < (int)sizeof(buf) - 1 && (n == 0 || buf[n - 1] != '\n'))
        buf[n++] = '\n';

    emit_buf(buf, n);
}

void log_blob(LogLevel lv, LogSubsys sub, const char *label,
              const char *data, size_t len) {
    if (g_log.fd < 0 || (int)lv > g_log.level) return;

    char buf[LOG_BLOB_BUF];
    int n = format_hdr(buf, sizeof(buf), lv, sub);
    if (n < 0 || n >= (int)sizeof(buf)) return;

    {
        int w = snprintf(buf + n, sizeof(buf) - (size_t)n,
                         "%s [%zu bytes]:\n", label, len);
        if (w < 0 || (size_t)(n + w) >= sizeof(buf)) return;
        n += w;
    }

    /* Reserve room for a trailing newline + "... N bytes elided\n" (max ~46 chars). */
    const int reserve = 48;
    size_t avail = ((int)sizeof(buf) - n - reserve > 0)
                   ? (size_t)((int)sizeof(buf) - n - reserve)
                   : 0;
    size_t copy = len < avail ? len : avail;
    if (copy > 0) {
        memcpy(buf + n, data, copy);
        n += (int)copy;
    }

    if (n > 0 && buf[n - 1] != '\n')
        buf[n++] = '\n';

    if (copy < len) {
        int w = snprintf(buf + n, sizeof(buf) - (size_t)n,
                         "... %zu bytes elided\n", len - copy);
        if (w > 0 && n + w < (int)sizeof(buf)) n += w;
    }

    emit_buf(buf, n);
}

const char *log_status_str(LogStatus s) {
    switch (s) {
    case LOG_OK:       return "ok";
    case LOG_ERR_FILE: return "err:file";
    default:           return "unknown";
    }
}

#endif /* OSTRICH_DEBUG */
