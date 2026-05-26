#include "../include/runstate.h"
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* helpers */

static RunAction step(RunState *rs, RunEvent ev) {
    return runstate_step(rs, ev);
}

/* reach RUNNING from IDLE via the device (non-sim) happy path */
static void reach_running(RunState *rs) {
    runstate_init(rs);
    rs->target_is_sim = false;
    runstate_step(rs, RUN_EV_EXECUTE);
    runstate_step(rs, RUN_EV_SETTINGS_OK);
    runstate_step(rs, RUN_EV_BUILD_OK);
    runstate_step(rs, RUN_EV_INSTALL_OK);
    runstate_step(rs, RUN_EV_LAUNCH_OK);
}

/* ── init ─────────────────────────────────────────────────────────── */

static int test_init(void) {
    RunState rs;
    runstate_init(&rs);
    ASSERT("init phase",        rs.phase        == RUN_IDLE);
    ASSERT("init built_gen",    rs.built_gen    == 0);
    ASSERT("init deployed_gen", rs.deployed_gen == 0);
    ASSERT("init not stale",    !runstate_stale(&rs));
    PASS("init");
    return 0;
}

/* ── EXECUTE happy path (device) ─────────────────────────────────── */

static int test_execute_device(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    rs.target_is_sim = false;

    act = step(&rs, RUN_EV_EXECUTE);
    ASSERT("execute → building",    rs.phase == RUN_BUILDING);
    ASSERT("execute → resolve",     act      == RUN_ACT_RESOLVE);

    act = step(&rs, RUN_EV_SETTINGS_OK);
    ASSERT("settings_ok → building",    rs.phase == RUN_BUILDING);
    ASSERT("settings_ok → build",       act      == RUN_ACT_BUILD);

    act = step(&rs, RUN_EV_BUILD_OK);
    ASSERT("build_ok → installing",     rs.phase == RUN_INSTALLING);
    ASSERT("build_ok → install",        act      == RUN_ACT_INSTALL);
    ASSERT("build_ok bumps built_gen",  rs.built_gen == 1);

    act = step(&rs, RUN_EV_INSTALL_OK);
    ASSERT("install_ok → launching",    rs.phase == RUN_LAUNCHING);
    ASSERT("install_ok → launch",       act      == RUN_ACT_LAUNCH);

    act = step(&rs, RUN_EV_LAUNCH_OK);
    ASSERT("launch_ok → running",       rs.phase == RUN_RUNNING);
    ASSERT("launch_ok → none",          act      == RUN_ACT_NONE);
    ASSERT("launch sets deployed_gen",  rs.deployed_gen == rs.built_gen);
    ASSERT("not stale after launch",    !runstate_stale(&rs));

    PASS("execute_device");
    return 0;
}

/* ── EXECUTE happy path (simulator) ─────────────────────────────── */

static int test_execute_sim(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    rs.target_is_sim = true;

    step(&rs, RUN_EV_EXECUTE);         /* → building */
    step(&rs, RUN_EV_SETTINGS_OK);     /* → build */

    act = step(&rs, RUN_EV_BUILD_OK);
    ASSERT("sim build_ok → priming",    rs.phase == RUN_PRIMING);
    ASSERT("sim build_ok → prime",      act      == RUN_ACT_PRIME);

    act = step(&rs, RUN_EV_PRIME_OK);
    ASSERT("prime_ok → installing",     rs.phase == RUN_INSTALLING);
    ASSERT("prime_ok → install",        act      == RUN_ACT_INSTALL);

    step(&rs, RUN_EV_INSTALL_OK);
    step(&rs, RUN_EV_LAUNCH_OK);
    ASSERT("sim reach running",         rs.phase == RUN_RUNNING);

    PASS("execute_sim");
    return 0;
}

/* ── COMPILE from idle (build-only) ─────────────────────────────── */

static int test_compile_from_idle(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);

    act = step(&rs, RUN_EV_COMPILE);
    ASSERT("compile → building",        rs.phase == RUN_BUILDING);
    ASSERT("compile → resolve",         act      == RUN_ACT_RESOLVE);

    act = step(&rs, RUN_EV_SETTINGS_OK);
    ASSERT("settings_ok stays building",rs.phase == RUN_BUILDING);
    ASSERT("settings_ok → build",       act      == RUN_ACT_BUILD);

    act = step(&rs, RUN_EV_BUILD_OK);
    ASSERT("compile build_ok → idle",   rs.phase == RUN_IDLE);
    ASSERT("compile build_ok → done",   act      == RUN_ACT_DONE);
    ASSERT("compile bumps built_gen",   rs.built_gen == 1);

    PASS("compile_from_idle");
    return 0;
}

/* ── terminate-first re-EXECUTE ─────────────────────────────────── */

static int test_terminate_first(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);
    ASSERT("precondition: running",     rs.phase == RUN_RUNNING);

    act = step(&rs, RUN_EV_EXECUTE);
    ASSERT("re-execute → building",         rs.phase == RUN_BUILDING);
    ASSERT("re-execute → terminate_first",  act      == RUN_ACT_TERMINATE_FIRST);

    /* DevConsole EOF arrives while we are already rebuilding — must be ignored */
    act = step(&rs, RUN_EV_CONSOLE_EOF);
    ASSERT("console_eof in building → none",    act      == RUN_ACT_NONE);
    ASSERT("console_eof in building stays bld", rs.phase == RUN_BUILDING);

    /* chain continues normally */
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_OK);
    step(&rs, RUN_EV_LAUNCH_OK);
    ASSERT("after re-execute reach running",    rs.phase == RUN_RUNNING);

    PASS("terminate_first");
    return 0;
}

/* ── COMPILE-while-running ───────────────────────────────────────── */

static int test_compile_while_running(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);
    int initial_built    = rs.built_gen;
    int initial_deployed = rs.deployed_gen;

    act = step(&rs, RUN_EV_COMPILE);
    ASSERT("compile-while-running phase stays running",  rs.phase == RUN_RUNNING);
    ASSERT("compile-while-running → resolve",            act      == RUN_ACT_RESOLVE);

    act = step(&rs, RUN_EV_SETTINGS_OK);
    ASSERT("settings_ok stays running",  rs.phase == RUN_RUNNING);
    ASSERT("settings_ok → build",        act      == RUN_ACT_BUILD);

    ASSERT("not stale yet",  !runstate_stale(&rs));

    act = step(&rs, RUN_EV_BUILD_OK);
    ASSERT("compile build_ok stays running",  rs.phase == RUN_RUNNING);
    ASSERT("compile build_ok → done",         act      == RUN_ACT_DONE);
    ASSERT("built_gen bumped",                rs.built_gen    == initial_built + 1);
    ASSERT("deployed_gen unchanged",          rs.deployed_gen == initial_deployed);
    ASSERT("now stale",                       runstate_stale(&rs));

    PASS("compile_while_running");
    return 0;
}

/* ── compile-while-running build FAIL ───────────────────────────── */

static int test_compile_while_running_fail(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);

    step(&rs, RUN_EV_COMPILE);
    step(&rs, RUN_EV_SETTINGS_OK);

    act = step(&rs, RUN_EV_BUILD_FAIL);
    ASSERT("compile fail stays running",    rs.phase == RUN_RUNNING);
    ASSERT("compile fail → none",           act      == RUN_ACT_NONE);
    ASSERT("not stale (compile failed)",    !runstate_stale(&rs));

    PASS("compile_while_running_fail");
    return 0;
}

/* ── stale clears on redeploy ────────────────────────────────────── */

static int test_stale_cleared_on_redeploy(void) {
    RunState rs;

    reach_running(&rs);

    /* make it stale via a COMPILE */
    step(&rs, RUN_EV_COMPILE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    ASSERT("stale before redeploy",  runstate_stale(&rs));

    /* re-EXECUTE → terminates old, rebuilds, re-launches */
    step(&rs, RUN_EV_EXECUTE);       /* terminate-first → building */
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_OK);
    step(&rs, RUN_EV_LAUNCH_OK);

    ASSERT("not stale after redeploy",  !runstate_stale(&rs));
    ASSERT("deployed_gen == built_gen", rs.deployed_gen == rs.built_gen);

    PASS("stale_cleared_on_redeploy");
    return 0;
}

/* ── failure edges ───────────────────────────────────────────────── */

static int test_build_fail(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);

    act = step(&rs, RUN_EV_BUILD_FAIL);
    ASSERT("build_fail → build_failed",  rs.phase == RUN_BUILD_FAILED);
    ASSERT("build_fail → none",          act      == RUN_ACT_NONE);

    PASS("build_fail");
    return 0;
}

static int test_prime_fail(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    rs.target_is_sim = true;
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);

    act = step(&rs, RUN_EV_PRIME_FAIL);
    ASSERT("prime_fail → deploy_failed", rs.phase == RUN_DEPLOY_FAILED);
    ASSERT("prime_fail → none",          act      == RUN_ACT_NONE);

    PASS("prime_fail");
    return 0;
}

static int test_install_fail(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);

    act = step(&rs, RUN_EV_INSTALL_FAIL);
    ASSERT("install_fail → deploy_failed",  rs.phase == RUN_DEPLOY_FAILED);
    ASSERT("install_fail → none",           act      == RUN_ACT_NONE);

    PASS("install_fail");
    return 0;
}

static int test_launch_fail(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_OK);

    act = step(&rs, RUN_EV_LAUNCH_FAIL);
    ASSERT("launch_fail → deploy_failed",  rs.phase == RUN_DEPLOY_FAILED);
    ASSERT("launch_fail → none",           act      == RUN_ACT_NONE);

    PASS("launch_fail");
    return 0;
}

/* ── CONSOLE_EOF → idle ──────────────────────────────────────────── */

static int test_console_eof_to_idle(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);

    act = step(&rs, RUN_EV_CONSOLE_EOF);
    ASSERT("console_eof → idle",  rs.phase == RUN_IDLE);
    ASSERT("console_eof → none",  act      == RUN_ACT_NONE);
    ASSERT("not stale after eof", !runstate_stale(&rs));

    PASS("console_eof_to_idle");
    return 0;
}

/* ── DROP → aborted ──────────────────────────────────────────────── */

static int test_drop_in_building(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);

    act = step(&rs, RUN_EV_DROP);
    ASSERT("drop in building → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("drop in building → none",     act      == RUN_ACT_NONE);

    PASS("drop_in_building");
    return 0;
}

static int test_drop_while_running(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);

    act = step(&rs, RUN_EV_DROP);
    ASSERT("drop while running → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("drop while running → none",     act      == RUN_ACT_NONE);

    PASS("drop_while_running");
    return 0;
}

/* ── ABORT from various states ───────────────────────────────────── */

static int test_abort_in_building(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);

    act = step(&rs, RUN_EV_ABORT);
    ASSERT("abort in building → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("abort in building → kill",     act      == RUN_ACT_KILL);

    PASS("abort_in_building");
    return 0;
}

static int test_abort_in_priming(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    rs.target_is_sim = true;
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);

    act = step(&rs, RUN_EV_ABORT);
    ASSERT("abort in priming → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("abort in priming → kill",     act      == RUN_ACT_KILL);

    PASS("abort_in_priming");
    return 0;
}

static int test_abort_in_installing(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);

    act = step(&rs, RUN_EV_ABORT);
    ASSERT("abort in installing → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("abort in installing → kill",     act      == RUN_ACT_KILL);

    PASS("abort_in_installing");
    return 0;
}

static int test_abort_in_launching(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_OK);

    act = step(&rs, RUN_EV_ABORT);
    ASSERT("abort in launching → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("abort in launching → kill",     act      == RUN_ACT_KILL);

    PASS("abort_in_launching");
    return 0;
}

static int test_abort_while_running(void) {
    RunState rs;
    RunAction act;

    reach_running(&rs);

    act = step(&rs, RUN_EV_ABORT);
    ASSERT("abort while running → aborted",  rs.phase == RUN_ABORTED);
    ASSERT("abort while running → kill",     act      == RUN_ACT_KILL);

    PASS("abort_while_running");
    return 0;
}

/* ── retry from terminal states ──────────────────────────────────── */

static int test_retry_from_build_failed(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_FAIL);
    ASSERT("precondition: build_failed",  rs.phase == RUN_BUILD_FAILED);

    act = step(&rs, RUN_EV_EXECUTE);
    ASSERT("retry from build_failed → building",  rs.phase == RUN_BUILDING);
    ASSERT("retry from build_failed → resolve",   act      == RUN_ACT_RESOLVE);

    PASS("retry_from_build_failed");
    return 0;
}

static int test_retry_from_deploy_failed(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_FAIL);
    ASSERT("precondition: deploy_failed",  rs.phase == RUN_DEPLOY_FAILED);

    act = step(&rs, RUN_EV_EXECUTE);
    ASSERT("retry from deploy_failed → building",  rs.phase == RUN_BUILDING);
    ASSERT("retry from deploy_failed → resolve",   act      == RUN_ACT_RESOLVE);

    PASS("retry_from_deploy_failed");
    return 0;
}

static int test_retry_from_aborted(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_ABORT);
    ASSERT("precondition: aborted",  rs.phase == RUN_ABORTED);

    act = step(&rs, RUN_EV_EXECUTE);
    ASSERT("retry from aborted → building",  rs.phase == RUN_BUILDING);
    ASSERT("retry from aborted → resolve",   act      == RUN_ACT_RESOLVE);

    PASS("retry_from_aborted");
    return 0;
}

static int test_compile_from_terminal(void) {
    RunState rs;
    RunAction act;

    runstate_init(&rs);
    step(&rs, RUN_EV_EXECUTE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_FAIL);

    act = step(&rs, RUN_EV_COMPILE);
    ASSERT("compile from build_failed → building",  rs.phase == RUN_BUILDING);
    ASSERT("compile from build_failed → resolve",   act      == RUN_ACT_RESOLVE);

    PASS("compile_from_terminal");
    return 0;
}

/* ── phase lex ───────────────────────────────────────────────────── */

static int test_phase_lex(void) {
    RunPhase phases[] = {
        RUN_IDLE, RUN_BUILDING, RUN_PRIMING, RUN_INSTALLING,
        RUN_LAUNCHING, RUN_RUNNING, RUN_BUILD_FAILED,
        RUN_DEPLOY_FAILED, RUN_ABORTED
    };
    int n = (int)(sizeof(phases) / sizeof(phases[0]));
    for (int i = 0; i < n; i++) {
        LexKey k = runstate_phase_lex(phases[i]);
        const char *s = lex(k);
        ASSERT("phase_lex returns non-empty",  s != NULL && s[0] != '\0');
        ASSERT("phase_lex no stray (?)",  strcmp(s, "(?)") != 0);
    }
    ASSERT("idle → standby",         runstate_phase_lex(RUN_IDLE)          == LEX_RUN_STANDBY);
    ASSERT("building → building",    runstate_phase_lex(RUN_BUILDING)      == LEX_RUN_BUILDING);
    ASSERT("priming → priming",      runstate_phase_lex(RUN_PRIMING)       == LEX_RUN_PRIMING);
    ASSERT("installing → installing",runstate_phase_lex(RUN_INSTALLING)    == LEX_RUN_INSTALLING);
    ASSERT("launching → launching",  runstate_phase_lex(RUN_LAUNCHING)     == LEX_RUN_LAUNCHING);
    ASSERT("running → running",      runstate_phase_lex(RUN_RUNNING)       == LEX_RUN_RUNNING);
    ASSERT("build_failed → bfailed", runstate_phase_lex(RUN_BUILD_FAILED)  == LEX_RUN_BUILD_FAILED);
    ASSERT("deploy_failed → dfailed",runstate_phase_lex(RUN_DEPLOY_FAILED) == LEX_RUN_DEPLOY_FAILED);
    ASSERT("aborted → aborted",      runstate_phase_lex(RUN_ABORTED)       == LEX_RUN_ABORTED);
    PASS("phase_lex");
    return 0;
}

/* ── reason lex ──────────────────────────────────────────────────── */

static int test_reason_lex(void) {
    LexKey k;
    const char *s;

    /* build failures */
    k = runstate_reason_lex(RUN_BUILD_FAILED, BD_ERR_BUILD);
    s = lex(k);
    ASSERT("bd_err_build no stray (?)",     strcmp(s, "(?)") != 0);
    ASSERT("bd_err_build → build_failed",   k == LEX_RUN_BUILD_FAILED);

    k = runstate_reason_lex(RUN_BUILD_FAILED, BD_ERR_XCODE_MISSING);
    s = lex(k);
    ASSERT("xcode_missing no stray (?)",    strcmp(s, "(?)") != 0);
    ASSERT("xcode_missing → rec_err_xcode", k == LEX_REC_ERR_XCODE);

    k = runstate_reason_lex(RUN_BUILD_FAILED, BD_ERR_SETSID_MISSING);
    s = lex(k);
    ASSERT("setsid_missing no stray (?)",     strcmp(s, "(?)") != 0);
    ASSERT("setsid_missing → rec_err_setsid", k == LEX_REC_ERR_SETSID);

    /* deploy failures */
    k = runstate_reason_lex(RUN_DEPLOY_FAILED, BD_ERR_INSTALL);
    s = lex(k);
    ASSERT("bd_err_install no stray (?)",       strcmp(s, "(?)") != 0);
    ASSERT("bd_err_install → deploy_failed",    k == LEX_RUN_DEPLOY_FAILED);

    k = runstate_reason_lex(RUN_DEPLOY_FAILED, BD_ERR_LAUNCH);
    s = lex(k);
    ASSERT("bd_err_launch no stray (?)",        strcmp(s, "(?)") != 0);
    ASSERT("bd_err_launch → deploy_failed",     k == LEX_RUN_DEPLOY_FAILED);

    k = runstate_reason_lex(RUN_DEPLOY_FAILED, BD_ERR_BOOT);
    s = lex(k);
    ASSERT("bd_err_boot no stray (?)",          strcmp(s, "(?)") != 0);
    ASSERT("bd_err_boot → deploy_failed",       k == LEX_RUN_DEPLOY_FAILED);

    /* aborted */
    k = runstate_reason_lex(RUN_ABORTED, BD_OK);
    s = lex(k);
    ASSERT("aborted no stray (?)",  strcmp(s, "(?)") != 0);
    ASSERT("aborted → aborted",     k == LEX_RUN_ABORTED);

    PASS("reason_lex");
    return 0;
}

/* ── phase_str coverage ──────────────────────────────────────────── */

static int test_phase_str(void) {
    RunPhase phases[] = {
        RUN_IDLE, RUN_BUILDING, RUN_PRIMING, RUN_INSTALLING,
        RUN_LAUNCHING, RUN_RUNNING, RUN_BUILD_FAILED,
        RUN_DEPLOY_FAILED, RUN_ABORTED
    };
    int n = (int)(sizeof(phases) / sizeof(phases[0]));
    for (int i = 0; i < n; i++) {
        const char *s = runstate_phase_str(phases[i]);
        ASSERT("phase_str non-empty",    s != NULL && s[0] != '\0');
        ASSERT("phase_str not UNKNOWN",  strcmp(s, "UNKNOWN") != 0);
    }
    PASS("phase_str");
    return 0;
}

/* ── stale rule ──────────────────────────────────────────────────── */

static int test_stale_rule(void) {
    RunState rs;

    /* not stale when idle */
    runstate_init(&rs);
    ASSERT("idle not stale",  !runstate_stale(&rs));

    /* not stale right after launch (gens equal) */
    reach_running(&rs);
    ASSERT("running not stale initially",  !runstate_stale(&rs));
    ASSERT("built == deployed",  rs.built_gen == rs.deployed_gen);

    /* COMPILE while running bumps built_gen → stale */
    step(&rs, RUN_EV_COMPILE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    ASSERT("stale after compile",       runstate_stale(&rs));
    ASSERT("built > deployed",          rs.built_gen > rs.deployed_gen);

    /* another COMPILE: even more stale (still just stale) */
    step(&rs, RUN_EV_COMPILE);
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    ASSERT("still stale after 2nd compile",  runstate_stale(&rs));

    /* re-EXECUTE clears stale */
    step(&rs, RUN_EV_EXECUTE);       /* terminate-first */
    step(&rs, RUN_EV_SETTINGS_OK);
    step(&rs, RUN_EV_BUILD_OK);
    step(&rs, RUN_EV_INSTALL_OK);
    step(&rs, RUN_EV_LAUNCH_OK);
    ASSERT("not stale after redeploy",       !runstate_stale(&rs));
    ASSERT("deployed matches built",         rs.deployed_gen == rs.built_gen);

    /* not stale in terminal states */
    step(&rs, RUN_EV_ABORT);
    ASSERT("aborted not stale",  !runstate_stale(&rs));

    PASS("stale_rule");
    return 0;
}

/* ── idle ignores irrelevant events ─────────────────────────────── */

static int test_idle_ignores(void) {
    RunState rs;
    RunAction act;

    RunEvent ignored[] = {
        RUN_EV_ABORT, RUN_EV_SETTINGS_OK, RUN_EV_BUILD_OK,
        RUN_EV_BUILD_FAIL, RUN_EV_INSTALL_OK, RUN_EV_INSTALL_FAIL,
        RUN_EV_LAUNCH_OK, RUN_EV_LAUNCH_FAIL, RUN_EV_CONSOLE_EOF,
        RUN_EV_DROP
    };
    int n = (int)(sizeof(ignored) / sizeof(ignored[0]));
    for (int i = 0; i < n; i++) {
        runstate_init(&rs);
        act = step(&rs, ignored[i]);
        ASSERT("idle ignores event → none",  act == RUN_ACT_NONE);
        ASSERT("idle ignores event → idle",  rs.phase == RUN_IDLE);
    }

    PASS("idle_ignores");
    return 0;
}

/* ── build-gen counter monotonicity ─────────────────────────────── */

static int test_built_gen_monotonic(void) {
    RunState rs;

    runstate_init(&rs);

    for (int i = 1; i <= 3; i++) {
        step(&rs, RUN_EV_COMPILE);
        step(&rs, RUN_EV_SETTINGS_OK);
        step(&rs, RUN_EV_BUILD_OK);
        ASSERT("built_gen == i", rs.built_gen == i);
        ASSERT("phase back to idle", rs.phase == RUN_IDLE);
    }

    PASS("built_gen_monotonic");
    return 0;
}

int main(void) {
    int ret = 0;
    ret |= test_init();
    ret |= test_execute_device();
    ret |= test_execute_sim();
    ret |= test_compile_from_idle();
    ret |= test_terminate_first();
    ret |= test_compile_while_running();
    ret |= test_compile_while_running_fail();
    ret |= test_stale_cleared_on_redeploy();
    ret |= test_build_fail();
    ret |= test_prime_fail();
    ret |= test_install_fail();
    ret |= test_launch_fail();
    ret |= test_console_eof_to_idle();
    ret |= test_drop_in_building();
    ret |= test_drop_while_running();
    ret |= test_abort_in_building();
    ret |= test_abort_in_priming();
    ret |= test_abort_in_installing();
    ret |= test_abort_in_launching();
    ret |= test_abort_while_running();
    ret |= test_retry_from_build_failed();
    ret |= test_retry_from_deploy_failed();
    ret |= test_retry_from_aborted();
    ret |= test_compile_from_terminal();
    ret |= test_phase_lex();
    ret |= test_reason_lex();
    ret |= test_phase_str();
    ret |= test_stale_rule();
    ret |= test_idle_ignores();
    ret |= test_built_gen_monotonic();
    return ret;
}
