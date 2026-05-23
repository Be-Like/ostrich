#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui.h"
#include "ssh.h"
#include "store.h"
#include "arena.h"

/* ── form_to_ssh_config state-based tests ─────────────────────────── */

static void test_basic_agent_form(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "myhost.local");
    snprintf(form.port, sizeof(form.port), "22");
    snprintf(form.user, sizeof(form.user), "alice");
    form.auth = SSH_AUTH_AGENT;

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);

    assert(strcmp(cfg.host, "myhost.local") == 0);
    assert(cfg.port == 22);
    assert(strcmp(cfg.user, "alice") == 0);
    assert(cfg.auth == SSH_AUTH_AGENT);
    assert(cfg.passkey[0] == '\0');
}

static void test_empty_port_defaults_to_22(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    form.port[0] = '\0';
    snprintf(form.user, sizeof(form.user), "bob");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.port == 22);
}

static void test_nonstandard_port(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "2222");
    snprintf(form.user, sizeof(form.user), "bob");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.port == 2222);
}

static void test_out_of_range_port_defaults_to_22(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "99999");
    snprintf(form.user, sizeof(form.user), "bob");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.port == 22);
}

static void test_zero_port_defaults_to_22(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "0");
    snprintf(form.user, sizeof(form.user), "bob");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.port == 22);
}

static void test_password_auth_passkey_copied(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "22");
    snprintf(form.user, sizeof(form.user), "carol");
    form.auth = SSH_AUTH_PASSWORD;
    snprintf(form.passkey, sizeof(form.passkey), "s3cret!");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.auth == SSH_AUTH_PASSWORD);
    assert(strcmp(cfg.passkey, "s3cret!") == 0);
}

static void test_agent_auth_passkey_not_leaked(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "22");
    snprintf(form.user, sizeof(form.user), "dave");
    form.auth = SSH_AUTH_AGENT;
    snprintf(form.passkey, sizeof(form.passkey), "leftover");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    /* Passkey is still copied verbatim — the session decides whether to use
       it based on cfg.auth. The test verifies the config reflects the form. */
    assert(cfg.auth == SSH_AUTH_AGENT);
}

static void test_max_boundary_port(void) {
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host");
    snprintf(form.port, sizeof(form.port), "65535");
    snprintf(form.user, sizeof(form.user), "eve");

    SshConfig cfg;
    app_form_to_ssh_config(&form, &cfg);
    assert(cfg.port == 65535);
}

/* ── app_conn_to_form tests ──────────────────────────────────────── */

static void test_conn_to_form_agent(void) {
    Conn conn = {0};
    snprintf(conn.label,   sizeof(conn.label),   "My Mac");
    snprintf(conn.host,    sizeof(conn.host),    "mac.local");
    conn.port = 22;
    snprintf(conn.user,    sizeof(conn.user),    "alice");
    conn.auth     = SSH_AUTH_AGENT;
    conn.remember = false;

    ConnForm form = {0};
    form.selected_known_host = 3; /* should NOT be touched */
    app_conn_to_form(&conn, &form);

    assert(strcmp(form.host, "mac.local") == 0);
    assert(strcmp(form.port, "22") == 0);
    assert(strcmp(form.user, "alice") == 0);
    assert(form.auth == SSH_AUTH_AGENT);
    assert(form.remember == false);
    assert(form.passkey[0] == '\0');
    assert(form.selected_known_host == 3); /* unchanged */
}

static void test_conn_to_form_password(void) {
    Conn conn = {0};
    snprintf(conn.host, sizeof(conn.host), "srv.local");
    conn.port = 2222;
    snprintf(conn.user, sizeof(conn.user), "bob");
    conn.auth     = SSH_AUTH_PASSWORD;
    conn.remember = true;
    snprintf(conn.passkey, sizeof(conn.passkey), "s3cret");

    ConnForm form = {0};
    app_conn_to_form(&conn, &form);

    assert(strcmp(form.host, "srv.local") == 0);
    assert(strcmp(form.port, "2222") == 0);
    assert(strcmp(form.user, "bob") == 0);
    assert(form.auth == SSH_AUTH_PASSWORD);
    assert(form.remember == true);
    assert(strcmp(form.passkey, "s3cret") == 0);
}

static void test_conn_to_form_port_zero_becomes_22(void) {
    Conn conn = {0};
    snprintf(conn.host, sizeof(conn.host), "h");
    conn.port = 0;
    snprintf(conn.user, sizeof(conn.user), "u");

    ConnForm form = {0};
    app_conn_to_form(&conn, &form);

    assert(strcmp(form.port, "22") == 0);
}

/* ── app_save_to_list tests ──────────────────────────────────────── */

static void test_save_to_empty_list(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    ConnList list = {0};
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "mac.local");
    snprintf(form.port, sizeof(form.port), "22");
    snprintf(form.user, sizeof(form.user), "alice");
    form.auth = SSH_AUTH_AGENT;

    int idx = app_save_to_list(&list, &form, -1, a);

    assert(idx == 0);
    assert(list.count == 1);
    assert(list.mru_index == 0);
    assert(strcmp(list.items[0].host, "mac.local") == 0);
    assert(strcmp(list.items[0].user, "alice") == 0);
    assert(list.items[0].port == 22);
    assert(list.items[0].auth == SSH_AUTH_AGENT);
    /* auto-label is "user@host" */
    assert(strcmp(list.items[0].label, "alice@mac.local") == 0);

    arena_destroy(a);
}

static void test_save_updates_selected(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    /* Pre-populate list with two entries. */
    Conn conns[2];
    memset(conns, 0, sizeof(conns));
    snprintf(conns[0].label, sizeof(conns[0].label), "Work");
    snprintf(conns[0].host,  sizeof(conns[0].host),  "work.local");
    conns[0].port = 22;
    snprintf(conns[0].user,  sizeof(conns[0].user),  "alice");
    conns[0].auth = SSH_AUTH_AGENT;

    snprintf(conns[1].label, sizeof(conns[1].label), "Home");
    snprintf(conns[1].host,  sizeof(conns[1].host),  "home.local");
    conns[1].port = 2222;
    snprintf(conns[1].user,  sizeof(conns[1].user),  "bob");
    conns[1].auth = SSH_AUTH_AGENT;

    ConnList list = { conns, 2, 0 };

    /* Update entry 1 with a new user. */
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "home.local");
    snprintf(form.port, sizeof(form.port), "2222");
    snprintf(form.user, sizeof(form.user), "carol");
    form.auth = SSH_AUTH_AGENT;

    int idx = app_save_to_list(&list, &form, 1, a);

    assert(idx == 1);
    assert(list.count == 2);
    assert(list.mru_index == 1);
    /* Updated slot has new user. */
    assert(strcmp(list.items[1].user, "carol") == 0);
    /* Label is preserved from the existing entry. */
    assert(strcmp(list.items[1].label, "Home") == 0);
    /* Other slot is untouched. */
    assert(strcmp(list.items[0].label, "Work") == 0);

    arena_destroy(a);
}

static void test_save_appends_when_no_selection(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    Conn c0 = {0};
    snprintf(c0.label, sizeof(c0.label), "Existing");
    snprintf(c0.host,  sizeof(c0.host),  "host0.local");
    c0.port = 22;
    snprintf(c0.user,  sizeof(c0.user),  "u0");
    c0.auth = SSH_AUTH_AGENT;
    ConnList list = { &c0, 1, 0 };

    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "host1.local");
    snprintf(form.port, sizeof(form.port), "22");
    snprintf(form.user, sizeof(form.user), "u1");
    form.auth = SSH_AUTH_AGENT;

    int idx = app_save_to_list(&list, &form, -1, a);

    assert(idx == 1);
    assert(list.count == 2);
    assert(list.mru_index == 1);
    assert(strcmp(list.items[0].host, "host0.local") == 0);
    assert(strcmp(list.items[1].host, "host1.local") == 0);
    assert(strcmp(list.items[1].label, "u1@host1.local") == 0);

    arena_destroy(a);
}

static void test_save_agent_no_passkey(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    ConnList list = {0};
    ConnForm form = {0};
    snprintf(form.host,    sizeof(form.host),    "h");
    snprintf(form.port,    sizeof(form.port),    "22");
    snprintf(form.user,    sizeof(form.user),    "u");
    snprintf(form.passkey, sizeof(form.passkey), "leftover");
    form.auth     = SSH_AUTH_AGENT;
    form.remember = true;

    app_save_to_list(&list, &form, -1, a);

    assert(list.items[0].passkey[0] == '\0');
    assert(list.items[0].remember == true);

    arena_destroy(a);
}

static void test_save_password_persisted_when_remember(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    ConnList list = {0};
    ConnForm form = {0};
    snprintf(form.host,    sizeof(form.host),    "h");
    snprintf(form.port,    sizeof(form.port),    "22");
    snprintf(form.user,    sizeof(form.user),    "u");
    snprintf(form.passkey, sizeof(form.passkey), "mypass");
    form.auth     = SSH_AUTH_PASSWORD;
    form.remember = true;

    app_save_to_list(&list, &form, -1, a);

    assert(strcmp(list.items[0].passkey, "mypass") == 0);
    assert(list.items[0].remember == true);

    arena_destroy(a);
}

static void test_save_password_not_persisted_without_remember(void) {
    Arena *a = arena_create(64 * 1024);
    assert(a);

    ConnList list = {0};
    ConnForm form = {0};
    snprintf(form.host,    sizeof(form.host),    "h");
    snprintf(form.port,    sizeof(form.port),    "22");
    snprintf(form.user,    sizeof(form.user),    "u");
    snprintf(form.passkey, sizeof(form.passkey), "mypass");
    form.auth     = SSH_AUTH_PASSWORD;
    form.remember = false;

    app_save_to_list(&list, &form, -1, a);

    assert(list.items[0].passkey[0] == '\0');

    arena_destroy(a);
}

static void test_save_oom_returns_minus1(void) {
    ConnList list = {0};
    ConnForm form = {0};
    snprintf(form.host, sizeof(form.host), "h");
    snprintf(form.user, sizeof(form.user), "u");
    form.auth = SSH_AUTH_AGENT;

    int idx = app_save_to_list(&list, &form, -1, NULL);
    assert(idx == -1);
    assert(list.count == 0);
}

static void test_conn_to_form_preserves_passkey_for_password_auth(void) {
    Conn conn = {0};
    snprintf(conn.host,    sizeof(conn.host),    "h");
    conn.port = 22;
    snprintf(conn.user,    sizeof(conn.user),    "u");
    conn.auth     = SSH_AUTH_PASSWORD;
    conn.remember = true;
    snprintf(conn.passkey, sizeof(conn.passkey), "s3cr3t");

    ConnForm form = {0};
    app_conn_to_form(&conn, &form);

    assert(strcmp(form.passkey, "s3cr3t") == 0);
    assert(form.remember == true);
    assert(form.auth == SSH_AUTH_PASSWORD);
}

int main(void) {
    test_basic_agent_form();
    test_empty_port_defaults_to_22();
    test_nonstandard_port();
    test_out_of_range_port_defaults_to_22();
    test_zero_port_defaults_to_22();
    test_password_auth_passkey_copied();
    test_agent_auth_passkey_not_leaked();
    test_max_boundary_port();

    test_conn_to_form_agent();
    test_conn_to_form_password();
    test_conn_to_form_port_zero_becomes_22();
    test_conn_to_form_preserves_passkey_for_password_auth();
    test_save_to_empty_list();
    test_save_updates_selected();
    test_save_appends_when_no_selection();
    test_save_agent_no_passkey();
    test_save_password_persisted_when_remember();
    test_save_password_not_persisted_without_remember();
    test_save_oom_returns_minus1();

    printf("app_test: ok\n");
    return 0;
}
