#define _POSIX_C_SOURCE 200809L

#include "../include/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

#define TEST_LOG   "/tmp/ostrich_log_test.log"
#define TEST_LOG_1 "/tmp/ostrich_log_test.log.1"

static void cleanup(void) {
    log_shutdown();
    unlink(TEST_LOG);
    unlink(TEST_LOG_1);
}

static void set_log_file(void) {
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
}

static char *read_log(size_t *out_len) {
    FILE *f = fopen(TEST_LOG, "rb");
    if (!f) { *out_len = 0; return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); *out_len = 0; return NULL; }
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* log_init creates the log file */
static int test_init_creates_file(void) {
    cleanup();
    set_log_file();

    LogStatus s = log_init();
    ASSERT("init ok", s == LOG_OK);
    ASSERT("file created", file_exists(TEST_LOG));

    cleanup();
    PASS("init_creates_file");
    return 0;
}

/* second log_init rotates the previous file to .1 */
static int test_rotate(void) {
    cleanup();
    set_log_file();

    ASSERT("first init", log_init() == LOG_OK);
    LOG_INFO(LG_APP, "first run");
    log_shutdown();

    ASSERT("first log exists", file_exists(TEST_LOG));

    ASSERT("second init", log_init() == LOG_OK);
    ASSERT("rotated file exists", file_exists(TEST_LOG_1));

    cleanup();
    PASS("rotate");
    return 0;
}

/* emitted records contain all expected columnar fields */
static int test_emit_format(void) {
    cleanup();
    set_log_file();
    unsetenv("OSTRICH_LOG");

    ASSERT("init", log_init() == LOG_OK);
    LOG_INFO(LG_DISC, "hello %s", "world");
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable", buf != NULL);

    ASSERT("has T separator",  strstr(buf, "T") != NULL);
    ASSERT("has delta +",      strstr(buf, "+") != NULL);
    ASSERT("has INFO level",   strstr(buf, "INFO") != NULL);
    ASSERT("has [disc]",       strstr(buf, "[disc]") != NULL);
    ASSERT("has [main]",       strstr(buf, "[main]") != NULL);
    ASSERT("has message",      strstr(buf, "hello world") != NULL);

    free(buf);
    cleanup();
    PASS("emit_format");
    return 0;
}

/* at INFO level, LOG_DEBUG produces no output */
static int test_level_gating_info(void) {
    cleanup();
    set_log_file();
    unsetenv("OSTRICH_LOG");

    ASSERT("init", log_init() == LOG_OK);
    LOG_DEBUG(LG_APP, "should not appear");
    LOG_INFO(LG_APP, "should appear");
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable", buf != NULL);
    ASSERT("debug absent",  strstr(buf, "should not appear") == NULL);
    ASSERT("info present",  strstr(buf, "should appear") != NULL);

    free(buf);
    cleanup();
    PASS("level_gating_info");
    return 0;
}

/* with OSTRICH_LOG=debug, LOG_DEBUG is logged */
static int test_level_gating_debug(void) {
    cleanup();
    set_log_file();
    setenv("OSTRICH_LOG", "debug", 1);

    ASSERT("init", log_init() == LOG_OK);
    LOG_DEBUG(LG_APP, "debug message");
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable", buf != NULL);
    ASSERT("debug present", strstr(buf, "debug message") != NULL);
    ASSERT("DEBUG label",   strstr(buf, "DEBUG") != NULL);

    free(buf);
    unsetenv("OSTRICH_LOG");
    cleanup();
    PASS("level_gating_debug");
    return 0;
}

/* custom thread tag appears in emitted records */
static int test_thread_tag(void) {
    cleanup();
    set_log_file();
    unsetenv("OSTRICH_LOG");

    ASSERT("init", log_init() == LOG_OK);
    log_set_thread_tag("wkr");
    LOG_INFO(LG_SESS, "tagged msg");
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable", buf != NULL);
    ASSERT("has [wkr]",     strstr(buf, "[wkr]") != NULL);

    free(buf);
    cleanup();
    PASS("thread_tag");
    return 0;
}

/* log_blob with small data writes all content, no elision */
static int test_blob_no_elision(void) {
    cleanup();
    set_log_file();
    unsetenv("OSTRICH_LOG");

    ASSERT("init", log_init() == LOG_OK);
    const char *data = "line one\nline two\n";
    LOG_BLOB(LOG_INFO, LG_DISC, "output", data, strlen(data));
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable",    buf != NULL);
    ASSERT("label present",    strstr(buf, "output") != NULL);
    ASSERT("byte count",       strstr(buf, "bytes") != NULL);
    ASSERT("data present",     strstr(buf, "line one") != NULL);
    ASSERT("no elision",       strstr(buf, "bytes elided") == NULL);

    free(buf);
    cleanup();
    PASS("blob_no_elision");
    return 0;
}

/* log_blob with large data truncates and emits an elision marker */
static int test_blob_cap(void) {
    cleanup();
    set_log_file();
    unsetenv("OSTRICH_LOG");

    ASSERT("init", log_init() == LOG_OK);

    /* 128 KB is well over the blob data cap (~65 KB) */
    size_t large = 128 * 1024;
    char *data = malloc(large);
    ASSERT("alloc", data != NULL);
    memset(data, 'x', large);

    LOG_BLOB(LOG_INFO, LG_DISC, "bigout", data, large);
    free(data);
    log_shutdown();

    size_t len;
    char *buf = read_log(&len);
    ASSERT("file readable",    buf != NULL);
    ASSERT("label present",    strstr(buf, "bigout") != NULL);
    ASSERT("byte count shown", strstr(buf, "131072 bytes") != NULL);
    ASSERT("elision marker",   strstr(buf, "bytes elided") != NULL);
    /* output is bounded to the blob buffer, far below 128 KB */
    ASSERT("size bounded", len < 70000);

    free(buf);
    cleanup();
    PASS("blob_cap");
    return 0;
}

/* log_status_str returns recognisable strings */
static int test_status_str(void) {
    ASSERT("ok str",  strcmp(log_status_str(LOG_OK),       "ok")       == 0);
    ASSERT("err str", strcmp(log_status_str(LOG_ERR_FILE), "err:file") == 0);
    PASS("status_str");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_init_creates_file();
    failures += test_rotate();
    failures += test_emit_format();
    failures += test_level_gating_info();
    failures += test_level_gating_debug();
    failures += test_thread_tag();
    failures += test_blob_no_elision();
    failures += test_blob_cap();
    failures += test_status_str();

    if (failures == 0) {
        printf("All log tests passed.\n");
        return 0;
    }
    printf("%d log test(s) failed.\n", failures);
    return 1;
}
