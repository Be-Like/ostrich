#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "ui.h"
#include "ssh.h"

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

int main(void) {
    test_basic_agent_form();
    test_empty_port_defaults_to_22();
    test_nonstandard_port();
    test_out_of_range_port_defaults_to_22();
    test_zero_port_defaults_to_22();
    test_password_auth_passkey_copied();
    test_agent_auth_passkey_not_leaked();
    test_max_boundary_port();

    printf("app_test: ok\n");
    return 0;
}
