#include "../include/connstate.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* helper: feed a sequence of events and return last action */
static ConnAction feed(ConnState *cs, ConnEvent ev) {
    return connstate_step(cs, ev);
}

/* helper: reach ONLINE from DISCONNECTED via the happy path */
static void reach_online(ConnState *cs) {
    connstate_init(cs);
    connstate_step(cs, EV_BREACH);
    connstate_step(cs, EV_HOSTKEY_OK);
    connstate_step(cs, EV_AUTH_OK);
    connstate_step(cs, EV_PROBE_OK);
}

/* ── init ─────────────────────────────────────────────────────────── */

static int test_init(void) {
    ConnState cs;
    connstate_init(&cs);
    ASSERT("init phase", cs.phase == CONN_DISCONNECTED);
    ASSERT("init attempt", cs.attempt == 0);
    ASSERT("init reason", cs.last_reason == SSH_OK);
    PASS("init");
    return 0;
}

/* ── normal connect flow ──────────────────────────────────────────── */

static int test_normal_connect(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);

    act = feed(&cs, EV_BREACH);
    ASSERT("breach → start_connect", act == ACT_START_CONNECT);
    ASSERT("breach → connecting", cs.phase == CONN_CONNECTING);
    ASSERT("breach resets attempt", cs.attempt == 0);

    act = feed(&cs, EV_TCP_UP);
    ASSERT("tcp_up → none", act == ACT_NONE);
    ASSERT("tcp_up stays connecting", cs.phase == CONN_CONNECTING);

    act = feed(&cs, EV_HOSTKEY_OK);
    ASSERT("hostkey_ok → do_auth", act == ACT_DO_AUTH);
    ASSERT("hostkey_ok stays connecting", cs.phase == CONN_CONNECTING);

    act = feed(&cs, EV_AUTH_OK);
    ASSERT("auth_ok → do_probe", act == ACT_DO_PROBE);
    ASSERT("auth_ok stays connecting", cs.phase == CONN_CONNECTING);

    act = feed(&cs, EV_PROBE_OK);
    ASSERT("probe_ok → go_online", act == ACT_GO_ONLINE);
    ASSERT("probe_ok → online", cs.phase == CONN_ONLINE);
    ASSERT("probe_ok resets attempt", cs.attempt == 0);

    PASS("normal_connect");
    return 0;
}

/* ── TOFU: unknown host → trust → connect ────────────────────────── */

static int test_tofu_trust(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);

    act = feed(&cs, EV_HOSTKEY_UNKNOWN);
    ASSERT("hostkey_unknown → wait_hostkey", act == ACT_WAIT_HOSTKEY);
    ASSERT("hostkey_unknown → awaiting", cs.phase == CONN_AWAITING_HOSTKEY);

    act = feed(&cs, EV_TRUST);
    ASSERT("trust → do_auth", act == ACT_DO_AUTH);
    ASSERT("trust → connecting", cs.phase == CONN_CONNECTING);

    act = feed(&cs, EV_AUTH_OK);
    ASSERT("auth_ok → do_probe", act == ACT_DO_PROBE);

    act = feed(&cs, EV_PROBE_OK);
    ASSERT("probe_ok → go_online", act == ACT_GO_ONLINE);
    ASSERT("trust flow → online", cs.phase == CONN_ONLINE);

    PASS("tofu_trust");
    return 0;
}

/* ── TOFU: unknown host → decline ────────────────────────────────── */

static int test_tofu_decline(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    feed(&cs, EV_HOSTKEY_UNKNOWN);

    act = feed(&cs, EV_DECLINE);
    ASSERT("decline → teardown", act == ACT_TEARDOWN);
    ASSERT("decline → disconnected", cs.phase == CONN_DISCONNECTED);

    PASS("tofu_decline");
    return 0;
}

/* ── abort during AWAITING_HOSTKEY ───────────────────────────────── */

static int test_tofu_abort(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    feed(&cs, EV_HOSTKEY_UNKNOWN);

    act = feed(&cs, EV_ABORT);
    ASSERT("abort hostkey → teardown", act == ACT_TEARDOWN);
    ASSERT("abort hostkey → disconnected", cs.phase == CONN_DISCONNECTED);

    PASS("tofu_abort");
    return 0;
}

/* ── host key mismatch: hard security stop ───────────────────────── */

static int test_hostkey_mismatch(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);

    act = feed(&cs, EV_HOSTKEY_MISMATCH);
    ASSERT("mismatch → show_failure", act == ACT_SHOW_FAILURE);
    ASSERT("mismatch → disconnected", cs.phase == CONN_DISCONNECTED);
    ASSERT("mismatch sets reason", cs.last_reason == SSH_ERR_HOSTKEY_MISMATCH);

    PASS("hostkey_mismatch");
    return 0;
}

/* ── auth failure ─────────────────────────────────────────────────── */

static int test_auth_fail(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    feed(&cs, EV_HOSTKEY_OK);

    act = feed(&cs, EV_AUTH_FAIL);
    ASSERT("auth_fail → show_failure", act == ACT_SHOW_FAILURE);
    ASSERT("auth_fail → disconnected", cs.phase == CONN_DISCONNECTED);
    ASSERT("auth_fail sets reason", cs.last_reason == SSH_ERR_AUTH);

    PASS("auth_fail");
    return 0;
}

/* ── probe failure ────────────────────────────────────────────────── */

static int test_probe_fail(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    feed(&cs, EV_HOSTKEY_OK);
    feed(&cs, EV_AUTH_OK);

    act = feed(&cs, EV_PROBE_FAIL);
    ASSERT("probe_fail → show_failure", act == ACT_SHOW_FAILURE);
    ASSERT("probe_fail → disconnected", cs.phase == CONN_DISCONNECTED);
    ASSERT("probe_fail sets reason", cs.last_reason == SSH_ERR_NO_SHELL);

    PASS("probe_fail");
    return 0;
}

/* ── generic failure (caller pre-sets reason) ────────────────────── */

static int test_generic_fail(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    cs.last_reason = SSH_ERR_DNS;

    act = feed(&cs, EV_FAIL);
    ASSERT("fail → show_failure", act == ACT_SHOW_FAILURE);
    ASSERT("fail → disconnected", cs.phase == CONN_DISCONNECTED);
    ASSERT("fail preserves reason", cs.last_reason == SSH_ERR_DNS);

    PASS("generic_fail");
    return 0;
}

/* ── abort during connect ─────────────────────────────────────────── */

static int test_abort_connecting(void) {
    ConnState cs;
    ConnAction act;

    connstate_init(&cs);
    feed(&cs, EV_BREACH);

    act = feed(&cs, EV_ABORT);
    ASSERT("abort → teardown", act == ACT_TEARDOWN);
    ASSERT("abort → disconnected", cs.phase == CONN_DISCONNECTED);

    PASS("abort_connecting");
    return 0;
}

/* ── close from ONLINE ────────────────────────────────────────────── */

static int test_close_online(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);

    act = feed(&cs, EV_CLOSE);
    ASSERT("close → teardown", act == ACT_TEARDOWN);
    ASSERT("close → disconnected", cs.phase == CONN_DISCONNECTED);

    PASS("close_online");
    return 0;
}

/* ── drop from ONLINE → REACQUIRING ──────────────────────────────── */

static int test_drop_to_reacquiring(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);

    act = feed(&cs, EV_DROP);
    ASSERT("drop → schedule_backoff", act == ACT_SCHEDULE_BACKOFF);
    ASSERT("drop → reacquiring", cs.phase == CONN_REACQUIRING);
    ASSERT("drop sets attempt=1", cs.attempt == 1);

    PASS("drop_to_reacquiring");
    return 0;
}

/* ── reconnect succeeds (full path) ──────────────────────────────── */

static int test_reconnect_success(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);

    act = feed(&cs, EV_BACKOFF_EXPIRED);
    ASSERT("backoff_expired → start_connect", act == ACT_START_CONNECT);
    ASSERT("backoff_expired stays reacquiring", cs.phase == CONN_REACQUIRING);

    act = feed(&cs, EV_HOSTKEY_OK);
    ASSERT("hostkey_ok in reacquiring → do_auth", act == ACT_DO_AUTH);

    act = feed(&cs, EV_AUTH_OK);
    ASSERT("auth_ok in reacquiring → do_probe", act == ACT_DO_PROBE);

    act = feed(&cs, EV_PROBE_OK);
    ASSERT("probe_ok in reacquiring → go_online", act == ACT_GO_ONLINE);
    ASSERT("probe_ok → online", cs.phase == CONN_ONLINE);
    ASSERT("probe_ok resets attempt", cs.attempt == 0);

    PASS("reconnect_success");
    return 0;
}

/* ── reconnect via EV_RECONNECT_OK ───────────────────────────────── */

static int test_reconnect_ok_event(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);
    feed(&cs, EV_BACKOFF_EXPIRED);

    act = feed(&cs, EV_RECONNECT_OK);
    ASSERT("reconnect_ok → go_online", act == ACT_GO_ONLINE);
    ASSERT("reconnect_ok → online", cs.phase == CONN_ONLINE);
    ASSERT("reconnect_ok resets attempt", cs.attempt == 0);

    PASS("reconnect_ok_event");
    return 0;
}

/* ── reconnect budget exhaustion → SEVERED ───────────────────────── */

static int test_budget_exhaustion(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP); /* attempt=1 */

    /* fail 4 more times → attempt reaches 5 (still under budget=6) */
    for (int i = 0; i < 4; i++) {
        act = feed(&cs, EV_FAIL);
        ASSERT("fail below budget → schedule_backoff", act == ACT_SCHEDULE_BACKOFF);
        ASSERT("fail below budget → still reacquiring", cs.phase == CONN_REACQUIRING);
    }
    ASSERT("attempt is 5 before sever", cs.attempt == 5);

    /* 6th failure → sever */
    act = feed(&cs, EV_FAIL);
    ASSERT("6th fail → severed action", act == ACT_SEVERED);
    ASSERT("6th fail → severed phase", cs.phase == CONN_SEVERED);
    ASSERT("attempt is 6 at sever", cs.attempt == 6);

    PASS("budget_exhaustion");
    return 0;
}

/* ── re-breach from SEVERED ──────────────────────────────────────── */

static int test_rebreach_from_severed(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);
    /* drive to sever */
    for (int i = 0; i < 5; i++) feed(&cs, EV_FAIL);
    ASSERT("reached severed", cs.phase == CONN_SEVERED);

    act = feed(&cs, EV_BREACH);
    ASSERT("breach from severed → start_connect", act == ACT_START_CONNECT);
    ASSERT("breach from severed → connecting", cs.phase == CONN_CONNECTING);
    ASSERT("breach resets attempt", cs.attempt == 0);

    PASS("rebreach_from_severed");
    return 0;
}

/* ── TOFU during REACQUIRING ─────────────────────────────────────── */

static int test_tofu_during_reacquiring(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);
    feed(&cs, EV_BACKOFF_EXPIRED);

    act = feed(&cs, EV_HOSTKEY_UNKNOWN);
    ASSERT("hostkey_unknown in reacquiring → wait_hostkey", act == ACT_WAIT_HOSTKEY);
    ASSERT("hostkey_unknown → awaiting", cs.phase == CONN_AWAITING_HOSTKEY);

    act = feed(&cs, EV_TRUST);
    ASSERT("trust in reacquiring → do_auth", act == ACT_DO_AUTH);
    ASSERT("trust → connecting", cs.phase == CONN_CONNECTING);

    PASS("tofu_during_reacquiring");
    return 0;
}

/* ── hostkey mismatch during REACQUIRING ─────────────────────────── */

static int test_mismatch_during_reacquiring(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);
    feed(&cs, EV_BACKOFF_EXPIRED);

    act = feed(&cs, EV_HOSTKEY_MISMATCH);
    ASSERT("mismatch in reacquiring → show_failure", act == ACT_SHOW_FAILURE);
    ASSERT("mismatch in reacquiring → disconnected", cs.phase == CONN_DISCONNECTED);
    ASSERT("mismatch sets reason", cs.last_reason == SSH_ERR_HOSTKEY_MISMATCH);

    PASS("mismatch_during_reacquiring");
    return 0;
}

/* ── auth fail during REACQUIRING ────────────────────────────────── */

static int test_auth_fail_reacquiring(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP); /* attempt=1 */
    feed(&cs, EV_BACKOFF_EXPIRED);
    feed(&cs, EV_HOSTKEY_OK);

    act = feed(&cs, EV_AUTH_FAIL);
    ASSERT("auth_fail in reacquiring → schedule_backoff", act == ACT_SCHEDULE_BACKOFF);
    ASSERT("auth_fail stays reacquiring", cs.phase == CONN_REACQUIRING);
    ASSERT("auth_fail sets reason", cs.last_reason == SSH_ERR_AUTH);
    ASSERT("attempt incremented", cs.attempt == 2);

    PASS("auth_fail_reacquiring");
    return 0;
}

/* ── close from REACQUIRING ──────────────────────────────────────── */

static int test_close_reacquiring(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    feed(&cs, EV_DROP);

    act = feed(&cs, EV_CLOSE);
    ASSERT("close reacquiring → teardown", act == ACT_TEARDOWN);
    ASSERT("close reacquiring → disconnected", cs.phase == CONN_DISCONNECTED);

    PASS("close_reacquiring");
    return 0;
}

/* ── ignore irrelevant events ────────────────────────────────────── */

static int test_ignored_events(void) {
    ConnState cs;
    ConnAction act;

    /* DISCONNECTED ignores non-BREACH */
    connstate_init(&cs);
    act = feed(&cs, EV_PROBE_OK);
    ASSERT("disconnected ignores probe_ok", act == ACT_NONE);
    ASSERT("disconnected phase unchanged", cs.phase == CONN_DISCONNECTED);

    act = feed(&cs, EV_DROP);
    ASSERT("disconnected ignores drop", act == ACT_NONE);

    /* ONLINE ignores BREACH */
    reach_online(&cs);
    act = feed(&cs, EV_BREACH);
    ASSERT("online ignores breach", act == ACT_NONE);
    ASSERT("online phase unchanged", cs.phase == CONN_ONLINE);

    /* AWAITING_HOSTKEY ignores probe events */
    connstate_init(&cs);
    feed(&cs, EV_BREACH);
    feed(&cs, EV_HOSTKEY_UNKNOWN);
    act = feed(&cs, EV_PROBE_OK);
    ASSERT("awaiting_hostkey ignores probe_ok", act == ACT_NONE);
    ASSERT("awaiting_hostkey phase unchanged", cs.phase == CONN_AWAITING_HOSTKEY);

    PASS("ignored_events");
    return 0;
}

/* ── should_sever ────────────────────────────────────────────────── */

static int test_should_sever(void) {
    ASSERT("attempt 0 → not sever", !connstate_should_sever(0));
    ASSERT("attempt 1 → not sever", !connstate_should_sever(1));
    ASSERT("attempt 5 → not sever", !connstate_should_sever(5));
    ASSERT("attempt 6 → sever",      connstate_should_sever(6));
    ASSERT("attempt 7 → sever",      connstate_should_sever(7));
    PASS("should_sever");
    return 0;
}

/* ── backoff_delay ───────────────────────────────────────────────── */

static double fixed_rng_zero(void) { return 0.0; }
static double fixed_rng_half(void) { return 0.5; }
static double fixed_rng_one(void)  { return 0.9999; }

static int test_backoff_delay(void) {
    /* with rng=0.0, delay is always 0 */
    connstate_set_rng(fixed_rng_zero);
    ASSERT("attempt 0 → 0.0", connstate_backoff_delay(0) == 0.0);
    ASSERT("attempt 1, rng=0 → 0.0", connstate_backoff_delay(1) == 0.0);
    ASSERT("attempt 5, rng=0 → 0.0", connstate_backoff_delay(5) == 0.0);

    /* with rng=0.5, delay is cap/2 */
    connstate_set_rng(fixed_rng_half);
    double d1 = connstate_backoff_delay(1); /* cap=2, expect 1.0 */
    ASSERT("attempt 1, rng=0.5 → 1.0", fabs(d1 - 1.0) < 1e-9);
    double d2 = connstate_backoff_delay(2); /* cap=4, expect 2.0 */
    ASSERT("attempt 2, rng=0.5 → 2.0", fabs(d2 - 2.0) < 1e-9);
    double d3 = connstate_backoff_delay(3); /* cap=8, expect 4.0 */
    ASSERT("attempt 3, rng=0.5 → 4.0", fabs(d3 - 4.0) < 1e-9);
    double d4 = connstate_backoff_delay(4); /* cap=16, expect 8.0 */
    ASSERT("attempt 4, rng=0.5 → 8.0", fabs(d4 - 8.0) < 1e-9);
    double d5 = connstate_backoff_delay(5); /* cap=30 (capped), expect 15.0 */
    ASSERT("attempt 5, rng=0.5 → 15.0 (capped)", fabs(d5 - 15.0) < 1e-9);
    double d6 = connstate_backoff_delay(6); /* still capped at 30 */
    ASSERT("attempt 6, rng=0.5 → 15.0 (capped)", fabs(d6 - 15.0) < 1e-9);

    /* with rng≈1.0, delay approaches max cap */
    connstate_set_rng(fixed_rng_one);
    double d5hi = connstate_backoff_delay(5);
    ASSERT("attempt 5, rng≈1.0 < 30.0", d5hi < 30.0);
    ASSERT("attempt 5, rng≈1.0 > 29.0", d5hi > 29.0);

    /* cap exponential growth: attempts 5+ hit the max cap */
    connstate_set_rng(fixed_rng_half);
    ASSERT("attempt 10 still capped", fabs(connstate_backoff_delay(10) - 15.0) < 1e-9);

    connstate_set_rng(NULL); /* restore default */
    PASS("backoff_delay");
    return 0;
}

/* ── validate ────────────────────────────────────────────────────── */

static int test_validate(void) {
    SshConfig cfg;

    /* valid agent config */
    memset(&cfg, 0, sizeof cfg);
    strncpy(cfg.host, "mac.local", sizeof cfg.host - 1);
    cfg.port = 22;
    strncpy(cfg.user, "jake", sizeof cfg.user - 1);
    cfg.auth = SSH_AUTH_AGENT;
    ASSERT("valid agent config", connstate_validate(&cfg));

    /* valid password config */
    cfg.auth = SSH_AUTH_PASSWORD;
    strncpy(cfg.passkey, "s3cr3t", sizeof cfg.passkey - 1);
    ASSERT("valid password config", connstate_validate(&cfg));

    /* empty host */
    memset(&cfg, 0, sizeof cfg);
    cfg.port = 22;
    strncpy(cfg.user, "jake", sizeof cfg.user - 1);
    cfg.auth = SSH_AUTH_AGENT;
    ASSERT("empty host → invalid", !connstate_validate(&cfg));

    /* port out of range */
    memset(&cfg, 0, sizeof cfg);
    strncpy(cfg.host, "mac.local", sizeof cfg.host - 1);
    cfg.port = 0;
    strncpy(cfg.user, "jake", sizeof cfg.user - 1);
    cfg.auth = SSH_AUTH_AGENT;
    ASSERT("port 0 → invalid", !connstate_validate(&cfg));

    cfg.port = 65536;
    ASSERT("port 65536 → invalid", !connstate_validate(&cfg));

    cfg.port = 65535;
    ASSERT("port 65535 → valid", connstate_validate(&cfg));

    cfg.port = 1;
    ASSERT("port 1 → valid", connstate_validate(&cfg));

    /* empty user */
    memset(&cfg, 0, sizeof cfg);
    strncpy(cfg.host, "mac.local", sizeof cfg.host - 1);
    cfg.port = 22;
    cfg.auth = SSH_AUTH_AGENT;
    ASSERT("empty user → invalid", !connstate_validate(&cfg));

    /* password auth with empty passkey */
    memset(&cfg, 0, sizeof cfg);
    strncpy(cfg.host, "mac.local", sizeof cfg.host - 1);
    cfg.port = 22;
    strncpy(cfg.user, "jake", sizeof cfg.user - 1);
    cfg.auth = SSH_AUTH_PASSWORD;
    /* passkey is empty */
    ASSERT("password auth empty passkey → invalid", !connstate_validate(&cfg));

    /* NULL pointer */
    ASSERT("NULL cfg → invalid", !connstate_validate(NULL));

    PASS("validate");
    return 0;
}

/* ── reason_lex ──────────────────────────────────────────────────── */

static int test_reason_lex(void) {
    ASSERT("dns → no_route",
           connstate_reason_lex(SSH_ERR_DNS) == LEX_CONN_ERR_NO_ROUTE);
    ASSERT("refused → port_closed",
           connstate_reason_lex(SSH_ERR_REFUSED) == LEX_CONN_ERR_PORT_CLOSED);
    ASSERT("timeout → timeout",
           connstate_reason_lex(SSH_ERR_TIMEOUT) == LEX_CONN_ERR_TIMEOUT);
    ASSERT("handshake → timeout",
           connstate_reason_lex(SSH_ERR_HANDSHAKE) == LEX_CONN_ERR_TIMEOUT);
    ASSERT("hostkey_mismatch → hostkey_mismatch",
           connstate_reason_lex(SSH_ERR_HOSTKEY_MISMATCH) == LEX_CONN_ERR_HOSTKEY_MISMATCH);
    ASSERT("auth → access_denied",
           connstate_reason_lex(SSH_ERR_AUTH) == LEX_CONN_ACCESS_DENIED);
    ASSERT("no_shell → no_shell",
           connstate_reason_lex(SSH_ERR_NO_SHELL) == LEX_CONN_ERR_NO_SHELL);
    ASSERT("io → no_route",
           connstate_reason_lex(SSH_ERR_IO) == LEX_CONN_ERR_NO_ROUTE);
    ASSERT("oom → no_route",
           connstate_reason_lex(SSH_ERR_OOM) == LEX_CONN_ERR_NO_ROUTE);
    PASS("reason_lex");
    return 0;
}

/* ── phase_str ───────────────────────────────────────────────────── */

static int test_phase_str(void) {
    ASSERT("disconnected str non-null",  connstate_phase_str(CONN_DISCONNECTED) != NULL);
    ASSERT("connecting str non-null",    connstate_phase_str(CONN_CONNECTING) != NULL);
    ASSERT("awaiting str non-null",      connstate_phase_str(CONN_AWAITING_HOSTKEY) != NULL);
    ASSERT("online str non-null",        connstate_phase_str(CONN_ONLINE) != NULL);
    ASSERT("reacquiring str non-null",   connstate_phase_str(CONN_REACQUIRING) != NULL);
    ASSERT("severed str non-null",       connstate_phase_str(CONN_SEVERED) != NULL);
    ASSERT("disconnected str correct",
           strcmp(connstate_phase_str(CONN_DISCONNECTED), "DISCONNECTED") == 0);
    ASSERT("online str correct",
           strcmp(connstate_phase_str(CONN_ONLINE), "ONLINE") == 0);
    PASS("phase_str");
    return 0;
}

/* ── fail from ONLINE triggers reconnect (EV_FAIL path) ─────────── */

static int test_fail_from_online(void) {
    ConnState cs;
    ConnAction act;

    reach_online(&cs);
    cs.last_reason = SSH_ERR_IO;

    act = feed(&cs, EV_FAIL);
    ASSERT("fail from online → schedule_backoff", act == ACT_SCHEDULE_BACKOFF);
    ASSERT("fail from online → reacquiring", cs.phase == CONN_REACQUIRING);
    ASSERT("fail from online attempt=1", cs.attempt == 1);

    PASS("fail_from_online");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_init();
    failures += test_normal_connect();
    failures += test_tofu_trust();
    failures += test_tofu_decline();
    failures += test_tofu_abort();
    failures += test_hostkey_mismatch();
    failures += test_auth_fail();
    failures += test_probe_fail();
    failures += test_generic_fail();
    failures += test_abort_connecting();
    failures += test_close_online();
    failures += test_drop_to_reacquiring();
    failures += test_reconnect_success();
    failures += test_reconnect_ok_event();
    failures += test_budget_exhaustion();
    failures += test_rebreach_from_severed();
    failures += test_tofu_during_reacquiring();
    failures += test_mismatch_during_reacquiring();
    failures += test_auth_fail_reacquiring();
    failures += test_close_reacquiring();
    failures += test_ignored_events();
    failures += test_should_sever();
    failures += test_backoff_delay();
    failures += test_validate();
    failures += test_reason_lex();
    failures += test_phase_str();
    failures += test_fail_from_online();

    if (failures == 0) {
        printf("All connstate tests passed.\n");
        return 0;
    }
    printf("%d connstate test(s) failed.\n", failures);
    return 1;
}
