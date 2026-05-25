#define _POSIX_C_SOURCE 200809L

#include "../include/log.h"
#include "../include/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

#define TEST_LOG   "/tmp/ostrich_session_test.log"
#define TEST_LOG_1 "/tmp/ostrich_session_test.log.1"

static void cleanup(void) {
    log_shutdown();
    unlink(TEST_LOG);
    unlink(TEST_LOG_1);
}

static char *read_log(void) {
    FILE *f = fopen(TEST_LOG, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Opening and immediately closing a session causes the worker thread
   to start, log its startup record tagged [wkr], and exit cleanly. */
static int test_worker_thread_tag(void) {
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    unsetenv("OSTRICH_LOG");

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    SshStatus ss = session_open(&s);
    ASSERT("session open", ss == SSH_OK);
    ASSERT("session not null", s != NULL);

    /* session_close joins the worker thread, so by return the worker
       has completed and all its log records are flushed. */
    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable", buf != NULL);
    ASSERT("worker tag [wkr]",    strstr(buf, "[wkr]") != NULL);
    ASSERT("worker started msg",  strstr(buf, "worker started") != NULL);
    ASSERT("worker stopped msg",  strstr(buf, "worker stopped") != NULL);

    free(buf);
    cleanup();
    PASS("worker_thread_tag");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_worker_thread_tag();

    if (failures == 0) {
        printf("All session tests passed.\n");
        return 0;
    }
    printf("%d session test(s) failed.\n", failures);
    return 1;
}
