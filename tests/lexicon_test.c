#include "../include/lexicon.h"
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

static int test_all_keys_non_empty(void) {
    for (int k = 0; k < LEX__COUNT; k++) {
        const char *s = lex((LexKey)k);
        ASSERT("key resolves to non-NULL", s != NULL);
        ASSERT("key resolves to non-empty string", s[0] != '\0');
    }
    PASS("all_keys_non_empty");
    return 0;
}

static int test_identity_string(void) {
    const char *s = lex(LEX_IDENTITY);
    ASSERT("identity is non-NULL", s != NULL);
    ASSERT("identity contains OSTRICH", strstr(s, "OSTRICH") != NULL);
    ASSERT("identity contains infiltration console",
           strstr(s, "infiltration console") != NULL);
    PASS("identity_string");
    return 0;
}

static int test_voice_prefix(void) {
    const char *s = lex(LEX_VOICE_PREFIX);
    ASSERT("voice prefix is non-NULL", s != NULL);
    ASSERT("voice prefix is >", strcmp(s, ">") == 0);
    PASS("voice_prefix");
    return 0;
}

static int test_footer_name(void) {
    const char *s = lex(LEX_FOOTER_NAME);
    ASSERT("footer name is non-NULL", s != NULL);
    ASSERT("footer name is ostrich", strcmp(s, "ostrich") == 0);
    PASS("footer_name");
    return 0;
}

static int test_footer_online(void) {
    const char *s = lex(LEX_FOOTER_ONLINE);
    ASSERT("footer online is non-NULL", s != NULL);
    ASSERT("footer online is ONLINE", strcmp(s, "ONLINE") == 0);
    PASS("footer_online");
    return 0;
}

static int test_wordmark_non_empty(void) {
    const char *s = lex(LEX_WORDMARK);
    ASSERT("wordmark is non-NULL", s != NULL);
    ASSERT("wordmark is non-empty", s[0] != '\0');
    PASS("wordmark_non_empty");
    return 0;
}

static int test_out_of_range_stable(void) {
    /* just past the last valid key */
    const char *s = lex(LEX__COUNT);
    ASSERT("out-of-range returns non-NULL", s != NULL);
    ASSERT("out-of-range returns non-empty placeholder", s[0] != '\0');

    /* a large value should also return a stable placeholder */
    const char *s2 = lex((LexKey)9999);
    ASSERT("large out-of-range returns non-NULL", s2 != NULL);
    ASSERT("large out-of-range returns same placeholder as LEX__COUNT",
           strcmp(s, s2) == 0);

    PASS("out_of_range_stable");
    return 0;
}

static int test_conn_overlay_keys(void) {
    ASSERT("uplink exact", strcmp(lex(LEX_CONN_UPLINK), "OSTRICH // UPLINK") == 0);
    ASSERT("breach exact", strcmp(lex(LEX_CONN_BREACH), "BREACH") == 0);
    ASSERT("abort exact",
           strcmp(lex(LEX_CONN_ABORT), "\xe2\x96\xa0 ABORT") == 0);
    ASSERT("known_hosts exact",
           strcmp(lex(LEX_CONN_KNOWN_HOSTS), "KNOWN HOSTS") == 0);
    ASSERT("no_known_hosts exact",
           strcmp(lex(LEX_CONN_NO_KNOWN_HOSTS), "// NO KNOWN HOSTS") == 0);
    PASS("conn_overlay_keys");
    return 0;
}

static int test_conn_phase_keys(void) {
    ASSERT("breaching exact",
           strcmp(lex(LEX_CONN_BREACHING),
                  "BREACHING PERIMETER\xe2\x80\xa6") == 0);
    ASSERT("access_granted exact",
           strcmp(lex(LEX_CONN_ACCESS_GRANTED), "ACCESS GRANTED") == 0);
    ASSERT("welcome exact",
           strcmp(lex(LEX_CONN_WELCOME),
                  "\xe2\x80\xa6WELCOME, OPERATOR.") == 0);
    ASSERT("access_denied exact",
           strcmp(lex(LEX_CONN_ACCESS_DENIED), "ACCESS DENIED") == 0);
    ASSERT("online exact", strcmp(lex(LEX_CONN_ONLINE), "* ONLINE") == 0);
    ASSERT("reacquiring exact",
           strcmp(lex(LEX_CONN_REACQUIRING),
                  "REACQUIRING SIGNAL\xe2\x80\xa6") == 0);
    ASSERT("severed exact",
           strcmp(lex(LEX_CONN_SEVERED), "LINK SEVERED") == 0);
    PASS("conn_phase_keys");
    return 0;
}

static int test_conn_failure_keys(void) {
    ASSERT("err_no_route exact",
           strcmp(lex(LEX_CONN_ERR_NO_ROUTE),
                  "HOST UNREACHABLE // NO ROUTE") == 0);
    ASSERT("err_port_closed exact",
           strcmp(lex(LEX_CONN_ERR_PORT_CLOSED),
                  "PERIMETER SEALED // PORT CLOSED") == 0);
    ASSERT("err_timeout exact",
           strcmp(lex(LEX_CONN_ERR_TIMEOUT), "NO RESPONSE // TIMEOUT") == 0);
    ASSERT("err_hostkey_mismatch exact",
           strcmp(lex(LEX_CONN_ERR_HOSTKEY_MISMATCH),
                  "HOST KEY MISMATCH // POSSIBLE INTERCEPTION") == 0);
    ASSERT("err_no_shell exact",
           strcmp(lex(LEX_CONN_ERR_NO_SHELL),
                  "NO FOOTHOLD // SHELL DENIED") == 0);
    ASSERT("unknown_host prefix exact",
           strcmp(lex(LEX_CONN_UNKNOWN_HOST), "UNKNOWN HOST //") == 0);
    PASS("conn_failure_keys");
    return 0;
}

static int test_conn_field_keys(void) {
    ASSERT("field_host exact",
           strcmp(lex(LEX_CONN_FIELD_HOST), "HOST") == 0);
    ASSERT("field_port exact",
           strcmp(lex(LEX_CONN_FIELD_PORT), "PORT") == 0);
    ASSERT("field_user exact",
           strcmp(lex(LEX_CONN_FIELD_USER), "USER") == 0);
    ASSERT("field_auth exact",
           strcmp(lex(LEX_CONN_FIELD_AUTH), "AUTH") == 0);
    ASSERT("auth_agent exact",
           strcmp(lex(LEX_CONN_AUTH_AGENT), "SSH-AGENT") == 0);
    ASSERT("auth_passkey exact",
           strcmp(lex(LEX_CONN_AUTH_PASSKEY), "PASSKEY") == 0);
    ASSERT("remember_passkey exact",
           strcmp(lex(LEX_CONN_REMEMBER_PASSKEY), "REMEMBER PASSKEY") == 0);
    PASS("conn_field_keys");
    return 0;
}

static int test_conn_hostkey_action_keys(void) {
    ASSERT("trust exact",
           strcmp(lex(LEX_CONN_TRUST), "TRUST") == 0);
    ASSERT("decline exact",
           strcmp(lex(LEX_CONN_DECLINE), "DECLINE") == 0);
    ASSERT("save exact",
           strcmp(lex(LEX_CONN_SAVE), "SAVE") == 0);
    PASS("conn_hostkey_action_keys");
    return 0;
}

static int test_conn_bar_control_keys(void) {
    ASSERT("update exact",
           strcmp(lex(LEX_CONN_UPDATE), "UPDATE") == 0);
    ASSERT("close exact",
           strcmp(lex(LEX_CONN_CLOSE), "CLOSE") == 0);
    PASS("conn_bar_control_keys");
    return 0;
}

static int test_recon_action_keys(void) {
    ASSERT("scan_host exact",
           strcmp(lex(LEX_REC_SCAN_HOST), "\xe2\x8c\x96 SCAN HOST") == 0);
    ASSERT("abort_scan exact",
           strcmp(lex(LEX_REC_ABORT_SCAN), "\xe2\x96\xa0 ABORT SCAN") == 0);
    ASSERT("blueprints exact",
           strcmp(lex(LEX_REC_BLUEPRINTS), "BLUEPRINTS RECOVERED") == 0);
    ASSERT("no_blueprints exact",
           strcmp(lex(LEX_REC_NO_BLUEPRINTS), "// NO BLUEPRINTS") == 0);
    ASSERT("sweep exact",
           strcmp(lex(LEX_REC_SWEEP), "\xe2\x86\xbb SWEEP FOR TARGETS") == 0);
    ASSERT("targets exact",
           strcmp(lex(LEX_REC_TARGETS), "TARGETS IN RANGE") == 0);
    ASSERT("no_targets exact",
           strcmp(lex(LEX_REC_NO_TARGETS), "// NO TARGETS IN RANGE") == 0);
    ASSERT("no_op exact",
           strcmp(lex(LEX_REC_NO_OP), "// NO OPERATION CONFIGURED") == 0);
    ASSERT("ready exact",
           strcmp(lex(LEX_REC_READY), "READY") == 0);
    ASSERT("err_xcode exact",
           strcmp(lex(LEX_REC_ERR_XCODE), "XCODE NOT FOUND") == 0);
    ASSERT("err_inventory exact",
           strcmp(lex(LEX_REC_ERR_INVENTORY), "COULD NOT READ INVENTORY") == 0);
    PASS("recon_action_keys");
    return 0;
}

static int test_recon_field_keys(void) {
    ASSERT("field_scan_root exact",
           strcmp(lex(LEX_REC_FIELD_SCAN_ROOT), "SCAN ROOT") == 0);
    ASSERT("field_scheme exact",
           strcmp(lex(LEX_REC_FIELD_SCHEME), "SCHEME") == 0);
    ASSERT("field_config exact",
           strcmp(lex(LEX_REC_FIELD_CONFIG), "CONFIG") == 0);
    ASSERT("field_bundle_id exact",
           strcmp(lex(LEX_REC_FIELD_BUNDLE_ID), "BUNDLE ID") == 0);
    ASSERT("field_preset exact",
           strcmp(lex(LEX_REC_FIELD_PRESET), "PRESET") == 0);
    PASS("recon_field_keys");
    return 0;
}

static int test_preset_action_keys(void) {
    ASSERT("preset_new exact",
           strcmp(lex(LEX_REC_PRESET_NEW), "NEW PRESET") == 0);
    ASSERT("preset_rename exact",
           strcmp(lex(LEX_REC_PRESET_RENAME), "RENAME") == 0);
    ASSERT("preset_delete exact",
           strcmp(lex(LEX_REC_PRESET_DELETE), "DELETE") == 0);
    PASS("preset_action_keys");
    return 0;
}

static int test_lex_count_consistency(void) {
    /* Update this constant when new keys are added. */
    ASSERT("LEX__COUNT is 54", LEX__COUNT == 54);
    PASS("lex_count_consistency");
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_all_keys_non_empty();
    failures += test_identity_string();
    failures += test_voice_prefix();
    failures += test_footer_name();
    failures += test_footer_online();
    failures += test_wordmark_non_empty();
    failures += test_out_of_range_stable();
    failures += test_conn_overlay_keys();
    failures += test_conn_phase_keys();
    failures += test_conn_failure_keys();
    failures += test_conn_field_keys();
    failures += test_conn_hostkey_action_keys();
    failures += test_conn_bar_control_keys();
    failures += test_recon_action_keys();
    failures += test_recon_field_keys();
    failures += test_preset_action_keys();
    failures += test_lex_count_consistency();

    if (failures == 0) {
        printf("All lexicon tests passed.\n");
        return 0;
    }
    printf("%d lexicon test(s) failed.\n", failures);
    return 1;
}
