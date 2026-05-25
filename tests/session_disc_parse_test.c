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

#define TEST_LOG   "/tmp/ostrich_session_disc_parse_test.log"
#define TEST_LOG_1 "/tmp/ostrich_session_disc_parse_test.log.1"

#define POLL_MAX_ITERS 5000

/* Configurable stub globals (defined in ssh_stub_disc.c). */
extern const char *g_stub_output;
extern size_t      g_stub_output_len;
extern int         g_stub_exit;

/* Valid xcodebuild -list -json output with 2 schemes and 2 configs. */
static const char k_valid_list_json[] =
    "{\"project\":{\"name\":\"TestApp\","
    "\"schemes\":[\"TestApp\",\"TestAppTests\"],"
    "\"configurations\":[\"Debug\",\"Release\"],"
    "\"targets\":[\"TestApp\"]}}";

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

/* Submit READ_BLUEPRINT and drain until COMPLETE or FAILED. */
static bool run_blueprint_read_and_wait(Session *s, SessionDiscEvent *out_ev)
{
    SessionDiscCmd dcmd;
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_READ_BLUEPRINT;
    snprintf(dcmd.project, sizeof(dcmd.project), "/tmp/TestApp.xcodeproj");
    if (!session_disc_submit(s, &dcmd)) return false;

    for (int i = 0; i < POLL_MAX_ITERS; i++) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            if (dev.kind == DEV_BLUEPRINT_READ_COMPLETE ||
                dev.kind == DEV_BLUEPRINT_FAILED) {
                if (out_ev) *out_ev = dev;
                return true;
            }
            /* DEV_SCHEME / DEV_CONFIG: keep draining */
            continue;
        }
        sleep_1ms();
    }
    return false;
}

/* ── tests ──────────────────────────────────────────────────────── */

/* Successful parse logs scheme/config counts at INFO. */
static int test_parse_success_logs_counts(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    unsetenv("OSTRICH_LOG");

    g_stub_output     = k_valid_list_json;
    g_stub_output_len = sizeof(k_valid_list_json) - 1;
    g_stub_exit       = 0;

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));

    SessionDiscEvent ev;
    ASSERT("blueprint read done", run_blueprint_read_and_wait(s, &ev));
    ASSERT("completed ok", ev.kind == DEV_BLUEPRINT_READ_COMPLETE);

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",   buf != NULL);
    ASSERT("parse-list",     strstr(buf, "parse-list") != NULL);
    ASSERT("schemes=2",      strstr(buf, "schemes=2") != NULL);
    ASSERT("configs=2",      strstr(buf, "configs=2") != NULL);
    ASSERT("disc subsys",    strstr(buf, "[disc]") != NULL);

    free(buf);
    cleanup();

    g_stub_output     = "stub output\n";
    g_stub_output_len = 12;
    g_stub_exit       = 0;

    PASS("parse_success_logs_counts");
    return 0;
}

/* Parse failure logs WARN with disc status. */
static int test_parse_failure_logs_warn(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    unsetenv("OSTRICH_LOG");

    /* Default stub output "stub output\n" is not valid JSON. */
    g_stub_output     = "stub output\n";
    g_stub_output_len = 12;
    g_stub_exit       = 0;

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));

    SessionDiscEvent ev;
    ASSERT("blueprint read done", run_blueprint_read_and_wait(s, &ev));
    ASSERT("failed as expected",  ev.kind == DEV_BLUEPRINT_FAILED);

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",   buf != NULL);
    ASSERT("parse-fail",     strstr(buf, "parse-fail") != NULL);
    ASSERT("disc subsys",    strstr(buf, "[disc]") != NULL);
    ASSERT("status text",    strstr(buf, "parse error") != NULL);

    free(buf);
    cleanup();
    PASS("parse_failure_logs_warn");
    return 0;
}

/* Parse failure also logs the raw output slice via LOG_BLOB at WARN. */
static int test_parse_failure_blob_contains_raw_output(void)
{
    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    setenv("OSTRICH_LOG", "warn", 1);

    g_stub_output     = "stub output\n";
    g_stub_output_len = 12;
    g_stub_exit       = 0;

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));

    SessionDiscEvent ev;
    ASSERT("blueprint read done", run_blueprint_read_and_wait(s, &ev));
    ASSERT("failed as expected",  ev.kind == DEV_BLUEPRINT_FAILED);

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",       buf != NULL);
    ASSERT("raw-output label",   strstr(buf, "raw-output") != NULL);
    ASSERT("stub text in blob",  strstr(buf, "stub output") != NULL);

    free(buf);
    unsetenv("OSTRICH_LOG");
    cleanup();
    PASS("parse_failure_blob_contains_raw_output");
    return 0;
}

/* Successful parse at DEBUG level also logs bundle_id detail. */
static int test_bundle_id_success_logs_debug(void)
{
    static const char k_bundle_json[] =
        "[{\"action\":\"build\",\"buildSettings\":{"
        "\"PRODUCT_BUNDLE_IDENTIFIER\":\"com.example.testapp\","
        "\"OTHER\":\"val\"},\"target\":\"TestApp\"}]";

    cleanup();
    setenv("OSTRICH_LOG_FILE", TEST_LOG, 1);
    setenv("OSTRICH_LOG", "debug", 1);

    g_stub_output     = k_bundle_json;
    g_stub_output_len = sizeof(k_bundle_json) - 1;
    g_stub_exit       = 0;

    ASSERT("log init", log_init() == LOG_OK);

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online",       connect_and_wait_online(s));

    /* Submit RESOLVE_BUNDLE_ID (needs project, scheme, config). */
    SessionDiscCmd dcmd;
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
    snprintf(dcmd.project, sizeof(dcmd.project), "/tmp/TestApp.xcodeproj");
    snprintf(dcmd.scheme,  sizeof(dcmd.scheme),  "TestApp");
    snprintf(dcmd.config,  sizeof(dcmd.config),  "Debug");
    ASSERT("submit", session_disc_submit(s, &dcmd));

    SessionDiscEvent ev;
    for (int i = 0; i < POLL_MAX_ITERS; i++) {
        if (session_disc_poll(s, &ev)) {
            if (ev.kind == DEV_BUNDLE_ID || ev.kind == DEV_BUNDLE_ID_FAILED)
                break;
        }
        sleep_1ms();
    }
    ASSERT("bundle id ok", ev.kind == DEV_BUNDLE_ID);

    session_close(s);
    log_shutdown();

    char *buf = read_log();
    ASSERT("log readable",         buf != NULL);
    ASSERT("parse-bundle-id",      strstr(buf, "parse-bundle-id") != NULL);
    ASSERT("bundle_id value",      strstr(buf, "com.example.testapp") != NULL);

    free(buf);
    unsetenv("OSTRICH_LOG");
    cleanup();

    g_stub_output     = "stub output\n";
    g_stub_output_len = 12;
    g_stub_exit       = 0;

    PASS("bundle_id_success_logs_debug");
    return 0;
}

int main(void)
{
    int failures = 0;
    failures += test_parse_success_logs_counts();
    failures += test_parse_failure_logs_warn();
    failures += test_parse_failure_blob_contains_raw_output();
    failures += test_bundle_id_success_logs_debug();

    if (failures == 0) {
        printf("All session_disc_parse tests passed.\n");
        return 0;
    }
    printf("%d session_disc_parse test(s) failed.\n", failures);
    return 1;
}
