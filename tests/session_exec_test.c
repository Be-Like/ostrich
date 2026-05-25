#define _POSIX_C_SOURCE 200809L

#include "../include/log.h"
#include "../include/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

#define TEST_LOG   "/tmp/ostrich_session_exec_test.log"
#define TEST_LOG_1 "/tmp/ostrich_session_exec_test.log.1"

#define POLL_MAX_ITERS 5000

static void sleep_1ms(void) {
    struct timespec ts = { 0, 1000000L };
    nanosleep(&ts, NULL);
}

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

/* Connect the session to the stub SSH, wait for CONN_ONLINE. */
static bool connect_and_wait_online(Session *s)
{
    SessionCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = CMD_BREACH;
    snprintf(cmd.cfg.host, sizeof(cmd.cfg.host), "stubhost");
    snprintf(cmd.cfg.user, sizeof(cmd.cfg.user), "stubuser");
    cmd.cfg.port = 22;
    cmd.cfg.auth = SSH_AUTH_AGENT;
    if (!session_submit(s, &cmd)) return false;

    SessionEvent ev;
    for (int i = 0; i < POLL_MAX_ITERS; i++) {
        if (session_poll(s, &ev) && ev.phase == CONN_ONLINE) return true;
        sleep_1ms();
    }
    return false;
}

/* Submit a scan command and drain disc events until SCAN_COMPLETE/FAILED. */
static bool run_scan_and_wait(Session *s)
{
    SessionDiscCmd dcmd;
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind      = DCMD_SCAN_HOST;
    snprintf(dcmd.root, sizeof(dcmd.root), "/tmp");
    dcmd.max_depth = 1;
    if (!session_disc_submit(s, &dcmd)) return false;

    SessionDiscEvent dev;
    for (int i = 0; i < POLL_MAX_ITERS; i++) {
        if (session_disc_poll(s, &dev)) {
            if (dev.kind == DEV_SCAN_COMPLETE || dev.kind == DEV_SCAN_FAILED)
                return true;
            /* DEV_BLUEPRINT or other intermediate events: keep draining */
            continue;
        }
        sleep_1ms();
    }
    return false;
}

/* Each remote command logs an exec-start record and a done record at INFO. */
static int test_exec_lifecycle_info(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    unsetenv("OSTRICH_LOG");

    ASSERT("log init",   log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));
    ASSERT("scan done",    run_scan_and_wait(s));

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",  buf != NULL);
    ASSERT("exec record",   strstr(buf, "exec job=") != NULL);
    ASSERT("exec kind",     strstr(buf, "kind=scan") != NULL);
    ASSERT("exec cmd",      strstr(buf, "cmd=\"") != NULL);
    ASSERT("done record",   strstr(buf, "done job=") != NULL);
    ASSERT("done exit=0",   strstr(buf, "exit=0") != NULL);
    ASSERT("done bytes",    strstr(buf, "bytes=") != NULL);
    ASSERT("disc subsys",   strstr(buf, "[disc]") != NULL);
    ASSERT("job id shared", strstr(buf, "exec job=0") != NULL);
    ASSERT("done id shared",strstr(buf, "done job=0") != NULL);

    free(buf);
    cleanup();
    PASS("exec_lifecycle_info");
    return 0;
}

/* At DEBUG level the raw output body is logged via LOG_BLOB. */
static int test_exec_lifecycle_debug_blob(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    setenv("OSTRICH_LOG", "debug", 1);

    ASSERT("log init",   log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));
    ASSERT("scan done",    run_scan_and_wait(s));

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",   buf != NULL);
    ASSERT("blob label",     strstr(buf, "output") != NULL);
    ASSERT("stub data",      strstr(buf, "stub output") != NULL);

    free(buf);
    unsetenv("OSTRICH_LOG");
    cleanup();
    PASS("exec_lifecycle_debug_blob");
    return 0;
}

/* Without an active connection no exec records should appear. */
static int test_no_exec_without_connection(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    unsetenv("OSTRICH_LOG");

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",   buf != NULL);
    ASSERT("no exec record", strstr(buf, "exec job=") == NULL);
    ASSERT("no done record", strstr(buf, "done job=") == NULL);

    free(buf);
    cleanup();
    PASS("no_exec_without_connection");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_exec_lifecycle_info();
    failures += test_exec_lifecycle_debug_blob();
    failures += test_no_exec_without_connection();

    if (failures == 0) {
        printf("All session_exec tests passed.\n");
        return 0;
    }
    printf("%d session_exec test(s) failed.\n", failures);
    return 1;
}
