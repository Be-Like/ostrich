#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN  = 1,
    LOG_INFO  = 2,
    LOG_DEBUG = 3,
    LOG_TRACE = 4
} LogLevel;

typedef enum {
    LG_APP   = 0,
    LG_SSH   = 1,
    LG_CONN  = 2,
    LG_SESS  = 3,
    LG_DISC  = 4,
    LG_STORE = 5,
    LG_UI    = 6,
    LG_RUN   = 7
} LogSubsys;

typedef enum {
    LOG_OK       = 0,
    LOG_ERR_FILE = 1
} LogStatus;

LogStatus   log_init(void);
void        log_shutdown(void);
void        log_set_thread_tag(const char *tag);
void        log_emit(LogLevel level, LogSubsys sub, const char *fmt, ...);
void        log_blob(LogLevel level, LogSubsys sub, const char *label,
                     const char *data, size_t len);
const char *log_status_str(LogStatus s);

#ifdef OSTRICH_DEBUG
#  define LOG_ERROR(sub, ...) log_emit(LOG_ERROR, (sub), __VA_ARGS__)
#  define LOG_WARN(sub,  ...) log_emit(LOG_WARN,  (sub), __VA_ARGS__)
#  define LOG_INFO(sub,  ...) log_emit(LOG_INFO,  (sub), __VA_ARGS__)
#  define LOG_DEBUG(sub, ...) log_emit(LOG_DEBUG, (sub), __VA_ARGS__)
#  define LOG_TRACE(sub, ...) log_emit(LOG_TRACE, (sub), __VA_ARGS__)
#  define LOG_BLOB(lvl, sub, label, data, len) \
          log_blob((lvl), (sub), (label), (data), (len))
#else
#  define LOG_ERROR(sub, ...) ((void)0)
#  define LOG_WARN(sub,  ...) ((void)0)
#  define LOG_INFO(sub,  ...) ((void)0)
#  define LOG_DEBUG(sub, ...) ((void)0)
#  define LOG_TRACE(sub, ...) ((void)0)
#  define LOG_BLOB(lvl, sub, label, data, len) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
