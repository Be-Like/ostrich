/* session_run_test.c — unit tests for the session run subsystem (T5).
   Uses ssh_stub_run.c (configurable per-exec stub) and compiles session.c
   directly with -DRUN_STALL_SEC_OVERRIDE=0.05 so the watchdog test runs
   in under a second. */

#define _POSIX_C_SOURCE 200809L

#include "../include/session.h"
#include "../include/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── stub globals (defined in ssh_stub_run.c) ─────────────────────── */

typedef struct {
    const char *output;
    size_t      output_len;
    int         exit_code;
    bool        streaming;
    bool        stall_read;
} StubRunResp;

extern StubRunResp  g_run_resp[];
extern int          g_run_resp_count;
extern volatile int g_run_stop_stream;
extern volatile int g_run_simulate_drop;
extern int          g_exec_next;
extern int          g_cur_idx;
extern int          g_bytes_sent[];
extern char         g_exec_cmds[][2048];

void stub_run_reset(void);

/* ── test macros ──────────────────────────────────────────────────── */

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) { printf("FAIL: %s (assert: %s)\n", (name), #cond); return 1; } } while (0)

#define POLL_MAX 5000  /* 5 seconds max per assertion */

/* ── test data ────────────────────────────────────────────────────── */

/* Minimal xcodebuild -showBuildSettings -json output that bd_parse_product_path
   can extract a product path from. */
static const char k_settings_json[] =
    "[{\"action\":\"build\","
    "\"buildSettings\":{"
    "\"BUILT_PRODUCTS_DIR\":\"/tmp\","
    "\"FULL_PRODUCT_NAME\":\"Test.app\","
    "\"PRODUCT_BUNDLE_IDENTIFIER\":\"com.test.App\""
    "},\"target\":\"Test\"}]";

/* Build output with an embedded PID marker. */
static const char k_build_output[] =
    "xcodebuild output\n__OSTRICH_PGID__99001\nBUILD SUCCEEDED\n";

static const char k_install_output[] = "install ok\n";
static const char k_device_output[]  = "app log line\n";

/* ── helpers ──────────────────────────────────────────────────────── */

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static bool connect_and_wait_online(Session *s)
{
    SessionCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind     = CMD_BREACH;
    cmd.cfg.port = 22;
    cmd.cfg.auth = SSH_AUTH_AGENT;
    snprintf(cmd.cfg.host, sizeof(cmd.cfg.host), "stubhost");
    snprintf(cmd.cfg.user, sizeof(cmd.cfg.user), "stubuser");

    if (!session_submit(s, &cmd)) return false;

    SessionEvent ev;
    for (int i = 0; i < POLL_MAX; i++) {
        if (session_poll(s, &ev) && ev.phase == CONN_ONLINE) return true;
        sleep_ms(1);
    }
    return false;
}

static SessionRunCmd make_execute_cmd(void)
{
    SessionRunCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = RCMD_EXECUTE;
    snprintf(cmd.cfg.project,   sizeof(cmd.cfg.project),   "/tmp/Test.xcodeproj");
    snprintf(cmd.cfg.scheme,    sizeof(cmd.cfg.scheme),    "Test");
    snprintf(cmd.cfg.config,    sizeof(cmd.cfg.config),    "Debug");
    snprintf(cmd.cfg.bundle_id, sizeof(cmd.cfg.bundle_id), "com.test.App");
    snprintf(cmd.target.name,   sizeof(cmd.target.name),   "iPhone");
    snprintf(cmd.target.udid,   sizeof(cmd.target.udid),   "DEVICE-UDID-001");
    cmd.target.is_simulator = false;
    cmd.target.booted       = true;
    cmd.has_target          = true;
    return cmd;
}

static SessionRunCmd make_compile_cmd(void)
{
    SessionRunCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = RCMD_COMPILE;
    snprintf(cmd.cfg.project,   sizeof(cmd.cfg.project),   "/tmp/Test.xcodeproj");
    snprintf(cmd.cfg.scheme,    sizeof(cmd.cfg.scheme),    "Test");
    snprintf(cmd.cfg.config,    sizeof(cmd.cfg.config),    "Debug");
    snprintf(cmd.cfg.bundle_id, sizeof(cmd.cfg.bundle_id), "com.test.App");
    cmd.has_target = false;
    return cmd;
}

/* Predicate type for drain_until */
typedef bool (*RunEvPred)(const SessionRunEvent *ev, void *data);

/* Poll run events for up to POLL_MAX ms; return true when pred fires. */
static bool drain_until(Session *s, RunEvPred pred, void *data)
{
    SessionRunEvent ev;
    for (int i = 0; i < POLL_MAX; i++) {
        if (session_run_poll(s, &ev)) {
            if (pred(&ev, data)) return true;
        }
        sleep_ms(1);
    }
    return false;
}

/* Same but with a shorter timeout (ms). */
static bool drain_until_ms(Session *s, RunEvPred pred, void *data, int timeout_ms)
{
    SessionRunEvent ev;
    for (int i = 0; i < timeout_ms; i++) {
        if (session_run_poll(s, &ev)) {
            if (pred(&ev, data)) return true;
        }
        sleep_ms(1);
    }
    return false;
}

static bool phase_is(const SessionRunEvent *ev, void *p)
{
    return ev->kind == REV_PHASE && ev->phase == *(RunPhase *)p;
}

static bool got_build_log(const SessionRunEvent *ev, void *p)
{
    (void)p;
    return ev->kind == REV_BUILD_LOG && ev->len > 0;
}

static bool got_device_log(const SessionRunEvent *ev, void *p)
{
    (void)p;
    return ev->kind == REV_DEVICE_LOG && ev->len > 0;
}

/* ── Tests ────────────────────────────────────────────────────────── */

/* 1. EXECUTE happy path: device target (no sim priming).
   Chain: settings → build → install → launch → running → console_eof → idle. */
static int test_execute_happy_path(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    /* Streaming launch channel: won't EOF until g_run_stop_stream is set. */
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 4;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    /* Phase: BUILDING */
    RunPhase p = RUN_BUILDING;
    ASSERT("phase building", drain_until(s, phase_is, &p));

    /* Build log chunk should arrive */
    ASSERT("build log chunk", drain_until(s, got_build_log, NULL));

    /* Phase: INSTALLING */
    p = RUN_INSTALLING;
    ASSERT("phase installing", drain_until(s, phase_is, &p));

    /* Phase: LAUNCHING */
    p = RUN_LAUNCHING;
    ASSERT("phase launching", drain_until(s, phase_is, &p));

    /* Phase: RUNNING */
    p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));

    /* Device log chunk should arrive from the streaming channel */
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* Signal stream end → console EOF → IDLE */
    g_run_stop_stream = 1;
    p = RUN_IDLE;
    ASSERT("phase idle after console eof", drain_until(s, phase_is, &p));

    /* 4 execs: settings, build, install, launch */
    ASSERT("4 execs used", g_exec_next == 4);

    session_close(s);
    PASS("execute_happy_path");
    return 0;
}

/* 2. COMPILE only: no target → no install, no launch. */
static int test_compile_only(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp_count = 2;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_compile_cmd();
    ASSERT("submit compile", session_run_submit(s, &cmd));

    RunPhase p = RUN_BUILDING;
    ASSERT("phase building", drain_until(s, phase_is, &p));

    ASSERT("build log chunk", drain_until(s, got_build_log, NULL));

    /* Should return to IDLE after build (compile_only, RUN_ACT_DONE). */
    p = RUN_IDLE;
    ASSERT("phase idle after compile", drain_until(s, phase_is, &p));

    /* Only 2 execs: settings + build */
    ASSERT("only 2 execs for compile", g_exec_next == 2);

    session_close(s);
    PASS("compile_only");
    return 0;
}

/* Collect all REV_BUILD_LOG chunks until a phase event matching *p arrives.
   Returns true if the target phase was reached; appends chunk bytes into
   buf (NUL-terminated, up to cap-1 bytes). */
static bool collect_build_log_until_phase(Session *s, RunPhase target,
                                          char *buf, size_t cap)
{
    size_t pos = 0;
    SessionRunEvent ev;
    for (int i = 0; i < POLL_MAX; i++) {
        if (session_run_poll(s, &ev)) {
            if (ev.kind == REV_BUILD_LOG && ev.len > 0) {
                size_t space = cap - 1 - pos;
                size_t copy  = (size_t)ev.len < space ? (size_t)ev.len : space;
                memcpy(buf + pos, ev.chunk, copy);
                pos += copy;
                buf[pos] = '\0';
            }
            if (ev.kind == REV_PHASE && ev.phase == target)
                return true;
        }
        sleep_ms(1);
    }
    return false;
}

/* 3. Build failure (non-zero exit from build step, no pgid marker).
   The setsid help block must appear in the build log before the
   RUN_BUILD_FAILED phase event. */
static int test_build_failure(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        "build error\n", 12, 1, false, false };  /* exit 1, no pgid marker */
    g_run_resp_count = 2;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    char build_log[8192];
    build_log[0] = '\0';
    ASSERT("phase build_failed",
           collect_build_log_until_phase(s, RUN_BUILD_FAILED,
                                         build_log, sizeof(build_log)));

    /* Help block must be in the build log. */
    ASSERT("help block header in build log",
           strstr(build_log, "REMOTE MAC IS MISSING setsid.") != NULL);
    ASSERT("ssh invocation in build log",
           strstr(build_log, "ssh ") != NULL);

    /* Only 2 execs: settings + build; no install or launch. */
    ASSERT("only 2 execs for build fail", g_exec_next == 2);

    session_close(s);
    PASS("build_failure");
    return 0;
}

/* 3b. EXECUTE with a SETTINGS-step failure (e.g. xcodebuild can't resolve
   the destination from id=<udid>).  The user must still see *some*
   diagnostic in the build log — not just the EXPLOIT FAILED banner.

   Currently fails: SETTINGS stdout/stderr is buffered into settings_buf
   for bd_parse_product_path and never reaches REV_BUILD_LOG, so when
   SETTINGS exits non-zero the build log panel is empty.  Fixing this
   means teeing SETTINGS bytes into emit_build_log as they stream. */
static int test_execute_settings_failure_shows_log(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        "xcodebuild: error: Unable to find a destination matching id=...\n",
        64, 1, false, false };  /* exit 1, settings step only */
    g_run_resp_count = 1;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();  /* has_target=true */
    ASSERT("submit execute", session_run_submit(s, &cmd));

    char build_log[8192];
    build_log[0] = '\0';
    ASSERT("phase build_failed",
           collect_build_log_until_phase(s, RUN_BUILD_FAILED,
                                         build_log, sizeof(build_log)));

    /* Only SETTINGS ran; BUILD never started. */
    ASSERT("only 1 exec for settings fail", g_exec_next == 1);

    /* The xcodebuild error must be surfaced to the user. */
    ASSERT("settings stderr surfaced in build log",
           strstr(build_log, "Unable to find") != NULL);

    session_close(s);
    PASS("execute_settings_failure_shows_log");
    return 0;
}

/* 4. Install failure (non-zero exit from install step). */
static int test_install_failure(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        "install failed\n", 15, 1, false, false };  /* exit 1 → install fail */
    g_run_resp_count = 3;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_DEPLOY_FAILED;
    ASSERT("phase deploy_failed on install", drain_until(s, phase_is, &p));

    /* 3 execs: settings, build, install; no launch. */
    ASSERT("only 3 execs for install fail", g_exec_next == 3);

    session_close(s);
    PASS("install_failure");
    return 0;
}

/* 5. Launch channel exits with non-zero code.
   The state machine transitions: LAUNCH_OK → RUNNING (emitted on exec),
   then DevConsole gets non-zero exit → RUN_EV_DROP → ABORTED.
   We set g_run_stop_stream=1 immediately so the streaming channel EOFs
   as soon as we've read its data. */
static int test_launch_nonzero_exit(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    /* streaming, exit 1 — stop immediately so the channel EOFs right away */
    g_run_resp[3] = (StubRunResp){
        "launch error\n", 13, 1, true, false };
    g_run_resp_count = 4;
    g_run_stop_stream = 1;  /* pre-set: EOF after first read */

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    /* RUNNING is emitted when launch exec succeeds (before DevConsole runs). */
    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));

    /* DevConsole gets exit=1; RUN_EV_LAUNCH_FAIL from RUNNING is not handled
       by the state machine (RUNNING only handles CONSOLE_EOF → IDLE and
       DROP → ABORTED).  The implementation falls through with the current
       phase unchanged and emits a RUNNING+BD_ERR_LAUNCH event to signal
       the error.  We assert we get a REV_PHASE event with BD_ERR_LAUNCH. */
    bool got_error = false;
    SessionRunEvent ev;
    for (int i = 0; i < POLL_MAX; i++) {
        if (session_run_poll(s, &ev)) {
            if (ev.kind == REV_PHASE && ev.reason == BD_ERR_LAUNCH) {
                got_error = true;
                break;
            }
        }
        sleep_ms(1);
    }
    ASSERT("got phase event with BD_ERR_LAUNCH on nonzero exit", got_error);

    session_close(s);
    PASS("launch_nonzero_exit");
    return 0;
}

/* 6. Normal console EOF (exit 0) → IDLE. */
static int test_console_eof(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    /* streaming, exit 0 — EOF when g_run_stop_stream */
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 4;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));

    /* Trigger EOF with exit 0 → IDLE */
    g_run_stop_stream = 1;
    p = RUN_IDLE;
    ASSERT("phase idle after console eof exit-0", drain_until(s, phase_is, &p));

    session_close(s);
    PASS("console_eof");
    return 0;
}

/* 7. Watchdog stall test.
   With -DRUN_STALL_SEC_OVERRIDE=0.05 the watchdog fires after 50 ms.
   The build step uses stall_read=true so read always returns SSH_AGAIN
   and eof always returns false.  The worker should detect the stall and
   resolve to RUN_BUILD_FAILED within a few hundred ms. */
static int test_watchdog_stall(void)
{
    stub_run_reset();
    /* Settings step: OK */
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    /* Build step: stall — read always SSH_AGAIN, eof always false */
    g_run_resp[1] = (StubRunResp){ NULL, 0, 0, false, true };
    g_run_resp_count = 2;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    /* Build phase should start */
    RunPhase p = RUN_BUILDING;
    ASSERT("phase building", drain_until(s, phase_is, &p));

    /* With a 50ms stall window, BUILD_FAILED should arrive within 1 second. */
    p = RUN_BUILD_FAILED;
    ASSERT("stall watchdog fires → build_failed",
           drain_until_ms(s, phase_is, &p, 1000));

    session_close(s);
    PASS("watchdog_stall");
    return 0;
}

/* ── T6 Tests ─────────────────────────────────────────────────────── */

/* 8. ABORT mid-build: two-pronged kill.
   The build channel is streaming (won't EOF); we submit ABORT while it
   is in flight.  The worker should issue a kill exec, close local
   channels, and resolve to RUN_ABORTED. */
static int test_abort_mid_build(void)
{
    stub_run_reset();
    /* settings: ok */
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    /* build: streaming so the chain stalls waiting for more data;
       the pgid marker is in the output so build_pgid is set. */
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, true, false };
    /* kill channel: no output, exit 0 (abort exec, fire-and-forget) */
    g_run_resp_count = 2;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    /* Wait for build phase to confirm chain is running */
    RunPhase p = RUN_BUILDING;
    ASSERT("phase building", drain_until(s, phase_is, &p));
    /* Ensure at least one build-log chunk arrived (pgid marker parsed) */
    ASSERT("build log chunk", drain_until(s, got_build_log, NULL));

    /* Submit ABORT */
    SessionRunCmd abort_cmd;
    memset(&abort_cmd, 0, sizeof(abort_cmd));
    abort_cmd.kind = RCMD_ABORT;
    ASSERT("submit abort", session_run_submit(s, &abort_cmd));

    /* Should resolve to RUN_ABORTED */
    p = RUN_ABORTED;
    ASSERT("phase aborted", drain_until(s, phase_is, &p));

    /* Kill exec must have been issued: exec 0=settings, 1=build, 2=kill */
    ASSERT("kill exec was issued", g_exec_next >= 3);
    ASSERT("kill cmd contains kill",
           strstr(g_exec_cmds[2], "kill") != NULL ||
           strstr(g_exec_cmds[2], "99001") != NULL);
    /* No terminate (DevConsole was not streaming) */
    ASSERT("only 3 execs for abort-mid-build", g_exec_next == 3);

    session_close(s);
    PASS("abort_mid_build");
    return 0;
}

/* 9. ABORT while running: terminate the app.
   App is in RUNNING state (DevConsole streaming).  ABORT should send
   a terminate exec and resolve to RUN_ABORTED. */
static int test_abort_running(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    /* streaming DevConsole */
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 4;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* Submit ABORT while running */
    SessionRunCmd abort_cmd;
    memset(&abort_cmd, 0, sizeof(abort_cmd));
    abort_cmd.kind = RCMD_ABORT;
    ASSERT("submit abort", session_run_submit(s, &abort_cmd));

    p = RUN_ABORTED;
    ASSERT("phase aborted", drain_until(s, phase_is, &p));

    /* Terminate exec at index 4 (after settings=0, build=1, install=2, launch=3) */
    ASSERT("terminate exec was issued", g_exec_next >= 5);
    ASSERT("terminate cmd contains terminate or bundle",
           strstr(g_exec_cmds[4], "terminate") != NULL ||
           strstr(g_exec_cmds[4], "com.test.App") != NULL);

    session_close(s);
    PASS("abort_running");
    return 0;
}

/* 10. Terminate-first re-EXECUTE: EXECUTE while running terminates the old
    instance and then starts a fresh chain.  The terminate exec must happen
    BEFORE the new chain's settings exec. */
static int test_terminate_first(void)
{
    stub_run_reset();
    /* First run */
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    /* Second run (terminate at 4, then settings+build at 5+6, install at 7,
       launch at 8 — stop with build failure to keep test simple) */
    /* index 4: terminate (fire-and-forget, no pre-set response needed) */
    g_run_resp[5] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[6] = (StubRunResp){
        "rebuild error\n", 14, 1, false, false };  /* build fail → stop early */
    g_run_resp_count = 7;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    /* First EXECUTE */
    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running (first run)", drain_until(s, phase_is, &p));
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* Second EXECUTE while running (terminate-first) */
    ASSERT("submit second execute", session_run_submit(s, &cmd));

    /* Phase should jump to BUILDING as terminate-first starts */
    p = RUN_BUILDING;
    ASSERT("phase building (second run)", drain_until(s, phase_is, &p));

    /* Second run build fails */
    p = RUN_BUILD_FAILED;
    ASSERT("phase build_failed", drain_until(s, phase_is, &p));

    /* Ordering: exec[4] = terminate; exec[5] = settings (second run).
       The terminate must precede the settings exec. */
    ASSERT("terminate at exec[4]",
           strstr(g_exec_cmds[4], "terminate") != NULL ||
           strstr(g_exec_cmds[4], "com.test.App") != NULL);
    ASSERT("settings at exec[5] follows terminate",
           strstr(g_exec_cmds[5], "showBuildSettings") != NULL ||
           strstr(g_exec_cmds[5], "xcodebuild") != NULL);

    session_close(s);
    PASS("terminate_first");
    return 0;
}

/* 11. SSH drop mid-run: channel read error on DevConsole causes
    RUN_EV_DROP → RUN_ABORTED without crashing. */
static int test_drop_mid_run(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 4;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* Simulate transport drop: next channel read returns SSH_ERR_IO */
    g_run_simulate_drop = 1;

    /* DevConsole read error → RUN_EV_DROP → RUN_ABORTED */
    p = RUN_ABORTED;
    ASSERT("phase aborted on drop", drain_until(s, phase_is, &p));

    session_close(s);
    PASS("drop_mid_run");
    return 0;
}

static bool got_stale_true(const SessionRunEvent *ev, void *p)
{
    (void)p;
    return ev->kind == REV_STALE && ev->stale;
}

static bool got_stale_false(const SessionRunEvent *ev, void *p)
{
    (void)p;
    return ev->kind == REV_STALE && !ev->stale;
}

/* 12. COMPILE-while-running: app is running (DevConsole streaming), a
    compile-only chain starts on a second channel, build succeeds, and
    REV_STALE(true) is emitted while the DevConsole stays alive. */
static int test_compile_while_running(void)
{
    stub_run_reset();
    /* First EXECUTE: settings(0) build(1) install(2) launch(3, streaming) */
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    /* COMPILE-while-running: settings(4) build(5) */
    g_run_resp[4] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[5] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp_count = 6;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    /* First EXECUTE → RUNNING */
    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* COMPILE while app is running */
    SessionRunCmd compile_cmd = make_compile_cmd();
    ASSERT("submit compile while running", session_run_submit(s, &compile_cmd));

    /* Build log chunk from the compile chain confirms second channel opened */
    ASSERT("compile build log chunk", drain_until(s, got_build_log, NULL));

    /* REV_STALE(true) must arrive after compile succeeds (built_gen > deployed_gen) */
    ASSERT("stale emitted true after compile", drain_until(s, got_stale_true, NULL));

    /* DevConsole was still alive: stop stream → IDLE via console EOF */
    g_run_stop_stream = 1;
    p = RUN_IDLE;
    ASSERT("phase idle after console eof", drain_until(s, phase_is, &p));

    /* 6 execs: settings(0) build(1) install(2) launch(3)
                compile-settings(4) compile-build(5) */
    ASSERT("6 execs total", g_exec_next == 6);
    ASSERT("compile settings at exec[4]",
           strstr(g_exec_cmds[4], "showBuildSettings") != NULL);

    session_close(s);
    PASS("compile_while_running");
    return 0;
}

/* 13. Stale clears on re-deploy: after a COMPILE-while-running makes the
    state stale, a subsequent EXECUTE that redeploys emits REV_STALE(false). */
static int test_stale_clears_on_execute(void)
{
    stub_run_reset();
    /* First EXECUTE */
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    /* COMPILE-while-running */
    g_run_resp[4] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[5] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    /* Second EXECUTE (from IDLE after console EOF) */
    g_run_resp[6] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[7] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[8] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    /* launch for second run: streaming, but g_run_stop_stream will already
       be 1 so it EOFs immediately after sending data */
    g_run_resp[9] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 10;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    /* First EXECUTE → RUNNING */
    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    RunPhase p = RUN_RUNNING;
    ASSERT("phase running", drain_until(s, phase_is, &p));
    ASSERT("device log chunk", drain_until(s, got_device_log, NULL));

    /* COMPILE-while-running → stale */
    SessionRunCmd compile_cmd = make_compile_cmd();
    ASSERT("submit compile", session_run_submit(s, &compile_cmd));
    ASSERT("stale becomes true", drain_until(s, got_stale_true, NULL));

    /* End first run: stop DevConsole → IDLE */
    g_run_stop_stream = 1;
    p = RUN_IDLE;
    ASSERT("phase idle after first run", drain_until(s, phase_is, &p));

    /* Second EXECUTE → new deploy → stale should clear */
    ASSERT("submit second execute", session_run_submit(s, &cmd));
    p = RUN_RUNNING;
    ASSERT("phase running second time", drain_until(s, phase_is, &p));

    /* REV_STALE(false) is emitted at the LAUNCH_OK point of the second deploy */
    ASSERT("stale cleared after redeploy", drain_until(s, got_stale_false, NULL));

    session_close(s);
    PASS("stale_clears_on_execute");
    return 0;
}

/* 14. Build marks: REV_BUILD_MARK fires for every chain step with non-empty
   command, and precedes that step's first REV_BUILD_LOG chunk.
   Also verifies compile-only path emits marks for settings and build only. */
static int test_build_marks(void)
{
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp[2] = (StubRunResp){
        k_install_output, sizeof(k_install_output) - 1, 0, false, false };
    g_run_resp[3] = (StubRunResp){
        k_device_output, sizeof(k_device_output) - 1, 0, true, false };
    g_run_resp_count = 4;

    Session *s = NULL;
    ASSERT("session open", session_open(&s) == SSH_OK);
    ASSERT("online", connect_and_wait_online(s));

    /* Collect all events from EXECUTE until RUNNING into a buffer. */
#define MARK_TEST_EV_CAP 256
    static SessionRunEvent collected[MARK_TEST_EV_CAP];
    int nev = 0;

    SessionRunCmd cmd = make_execute_cmd();
    ASSERT("submit execute", session_run_submit(s, &cmd));

    /* Drain until RUN_RUNNING, collecting all events. */
    bool reached_running = false;
    for (int i = 0; i < POLL_MAX && !reached_running; i++) {
        SessionRunEvent ev;
        if (session_run_poll(s, &ev)) {
            if (nev < MARK_TEST_EV_CAP) collected[nev++] = ev;
            if (ev.kind == REV_PHASE && ev.phase == RUN_RUNNING)
                reached_running = true;
        } else {
            sleep_ms(1);
        }
    }
    ASSERT("reached running", reached_running);

    /* Verify ordering: for each REV_BUILD_LOG, a REV_BUILD_MARK with a
       non-empty command must have appeared earlier in the sequence. */
    bool mark_seen = false;
    for (int i = 0; i < nev; i++) {
        if (collected[i].kind == REV_BUILD_MARK) {
            ASSERT("build mark has non-empty command",
                   collected[i].cmd_summary[0] != '\0');
            mark_seen = true;
        }
        if (collected[i].kind == REV_BUILD_LOG && collected[i].len > 0) {
            ASSERT("build log preceded by a build mark", mark_seen);
        }
        /* Reset mark_seen on each INSTALLING/LAUNCHING phase transition
           to enforce per-step ordering. */
        if (collected[i].kind == REV_PHASE &&
            (collected[i].phase == RUN_INSTALLING ||
             collected[i].phase == RUN_LAUNCHING)) {
            mark_seen = false;
        }
    }

    /* Count marks: expect settings, build, install, launch = 4. */
    int mark_count = 0;
    for (int i = 0; i < nev; i++)
        if (collected[i].kind == REV_BUILD_MARK) mark_count++;
    ASSERT("four marks for execute (settings, build, install, launch)",
           mark_count == 4);

    g_run_stop_stream = 1;
    session_close(s);
    PASS("build_marks_execute");

    /* Part 2: compile-only should emit exactly 2 marks (settings + build). */
    stub_run_reset();
    g_run_resp[0] = (StubRunResp){
        k_settings_json, sizeof(k_settings_json) - 1, 0, false, false };
    g_run_resp[1] = (StubRunResp){
        k_build_output, sizeof(k_build_output) - 1, 0, false, false };
    g_run_resp_count = 2;

    ASSERT("session open compile", session_open(&s) == SSH_OK);
    ASSERT("online compile", connect_and_wait_online(s));

    SessionRunCmd compile_cmd = make_compile_cmd();
    ASSERT("submit compile", session_run_submit(s, &compile_cmd));

    nev = 0;
    bool reached_idle = false;
    for (int i = 0; i < POLL_MAX && !reached_idle; i++) {
        SessionRunEvent ev;
        if (session_run_poll(s, &ev)) {
            if (nev < MARK_TEST_EV_CAP) collected[nev++] = ev;
            if (ev.kind == REV_PHASE && ev.phase == RUN_IDLE)
                reached_idle = true;
        } else {
            sleep_ms(1);
        }
    }
    ASSERT("compile reached idle", reached_idle);

    mark_count = 0;
    for (int i = 0; i < nev; i++)
        if (collected[i].kind == REV_BUILD_MARK) mark_count++;
    ASSERT("two marks for compile (settings, build)", mark_count == 2);

    for (int i = 0; i < nev; i++) {
        if (collected[i].kind == REV_BUILD_MARK)
            ASSERT("compile mark non-empty cmd", collected[i].cmd_summary[0] != '\0');
    }

    session_close(s);
    PASS("build_marks_compile");
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(void)
{
    /* Initialize debug log to /tmp so we don't pollute the real log. */
    (void)setenv("OSTRICH_LOG_FILE", "/tmp/ostrich_session_run_test.log", 1);
    (void)setenv("OSTRICH_LOG",      "trace", 1);
    log_init();

    int rc = 0;
    rc |= test_execute_happy_path();
    rc |= test_compile_only();
    rc |= test_build_failure();
    rc |= test_execute_settings_failure_shows_log();
    rc |= test_install_failure();
    rc |= test_launch_nonzero_exit();
    rc |= test_console_eof();
    rc |= test_watchdog_stall();
    rc |= test_abort_mid_build();
    rc |= test_abort_running();
    rc |= test_terminate_first();
    rc |= test_drop_mid_run();
    rc |= test_compile_while_running();
    rc |= test_stale_clears_on_execute();
    rc |= test_build_marks();

    log_shutdown();
    unlink("/tmp/ostrich_session_run_test.log");
    unlink("/tmp/ostrich_session_run_test.log.1");
    return rc;
}
