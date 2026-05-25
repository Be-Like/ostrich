#include "../include/builddeploy.h"
#include <stdio.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) do { if (!(cond)) FAIL(name); } while (0)

/* ── helpers ──────────────────────────────────────────────────────── */

static RunConfig make_config(const char *project, const char *scheme,
                              const char *config) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    snprintf(rc.project, sizeof(rc.project), "%s", project);
    snprintf(rc.scheme,  sizeof(rc.scheme),  "%s", scheme);
    snprintf(rc.config,  sizeof(rc.config),  "%s", config);
    snprintf(rc.bundle_id, sizeof(rc.bundle_id), "com.example.app");
    return rc;
}

static Target make_device(const char *udid) {
    Target t;
    memset(&t, 0, sizeof(t));
    snprintf(t.udid, sizeof(t.udid), "%s", udid);
    snprintf(t.name, sizeof(t.name), "iPhone 15");
    t.is_simulator = false;
    t.booted       = false;
    return t;
}

static Target make_sim(const char *udid) {
    Target t;
    memset(&t, 0, sizeof(t));
    snprintf(t.udid, sizeof(t.udid), "%s", udid);
    snprintf(t.name, sizeof(t.name), "iPhone 15 Sim");
    t.is_simulator = true;
    t.booted       = false;
    return t;
}

/* True if needle appears somewhere in haystack. */
static int has(const char *haystack, const char *needle) {
    return strstr(haystack, needle) != NULL;
}

/* ── bd_destination ───────────────────────────────────────────────── */

static int test_destination_no_target(void) {
    char buf[256];
    BdStatus s = bd_destination(NULL, false, buf, sizeof(buf));
    ASSERT("no-target ok", s == BD_OK);
    ASSERT("no-target generic", strcmp(buf, "generic/platform=iOS") == 0);
    PASS("destination_no_target");
    return 0;
}

static int test_destination_device(void) {
    char buf[256];
    Target t = make_device("00008110-001234567890123A");
    BdStatus s = bd_destination(&t, true, buf, sizeof(buf));
    ASSERT("device ok", s == BD_OK);
    ASSERT("device id=", has(buf, "id=00008110-001234567890123A"));
    PASS("destination_device");
    return 0;
}

static int test_destination_sim(void) {
    char buf[256];
    Target t = make_sim("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");
    BdStatus s = bd_destination(&t, true, buf, sizeof(buf));
    ASSERT("sim ok", s == BD_OK);
    ASSERT("sim id=", has(buf, "id=AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"));
    PASS("destination_sim");
    return 0;
}

/* ── bd_settings_cmd ─────────────────────────────────────────────── */

static int test_settings_cmd_no_target(void) {
    char buf[4096];
    RunConfig rc = make_config("/proj/App.xcodeproj", "App", "Debug");
    BdStatus s = bd_settings_cmd(&rc, NULL, false, buf, sizeof(buf));
    ASSERT("settings no-target ok", s == BD_OK);
    ASSERT("settings xcodebuild",   has(buf, "xcodebuild"));
    ASSERT("settings -showBuildSettings", has(buf, "-showBuildSettings"));
    ASSERT("settings -json",        has(buf, "-json"));
    ASSERT("settings -project",     has(buf, "-project"));
    ASSERT("settings path quoted",  has(buf, "'/proj/App.xcodeproj'"));
    ASSERT("settings scheme",       has(buf, "-scheme"));
    ASSERT("settings config",       has(buf, "-configuration"));
    ASSERT("settings generic dest", has(buf, "generic/platform=iOS"));
    PASS("settings_cmd_no_target");
    return 0;
}

static int test_settings_cmd_workspace(void) {
    char buf[4096];
    RunConfig rc = make_config("/proj/App.xcworkspace", "App", "Release");
    Target t = make_device("UDID-1");
    BdStatus s = bd_settings_cmd(&rc, &t, true, buf, sizeof(buf));
    ASSERT("workspace ok", s == BD_OK);
    ASSERT("workspace flag", has(buf, "-workspace"));
    ASSERT("no -project",   !has(buf, "-project"));
    PASS("settings_cmd_workspace");
    return 0;
}

static int test_settings_cmd_space_in_path(void) {
    char buf[4096];
    RunConfig rc = make_config("/my projects/App.xcodeproj", "App", "Debug");
    BdStatus s = bd_settings_cmd(&rc, NULL, false, buf, sizeof(buf));
    ASSERT("space ok", s == BD_OK);
    /* Path with space must be single-quoted so the shell sees one argument. */
    ASSERT("space quoted", has(buf, "'/my projects/App.xcodeproj'"));
    PASS("settings_cmd_space_in_path");
    return 0;
}

/* ── bd_build_cmd ────────────────────────────────────────────────── */

static int test_build_cmd_has_setsid_and_marker(void) {
    char buf[8192];
    RunConfig rc = make_config("/proj/App.xcodeproj", "MyScheme", "Debug");
    BdStatus s = bd_build_cmd(&rc, NULL, false, buf, sizeof(buf));
    ASSERT("build ok",     s == BD_OK);
    ASSERT("has setsid",   has(buf, "setsid"));
    ASSERT("has sh -c",    has(buf, "sh -c"));
    ASSERT("has marker",   has(buf, "__OSTRICH_PGID__"));
    ASSERT("has xcodebuild", has(buf, "xcodebuild"));
    ASSERT("has -showBuildSettings not present", !has(buf, "-showBuildSettings"));
    PASS("build_cmd_has_setsid_and_marker");
    return 0;
}

static int test_build_cmd_no_target_generic_dest(void) {
    char buf[8192];
    RunConfig rc = make_config("/proj/App.xcodeproj", "App", "Debug");
    BdStatus s = bd_build_cmd(&rc, NULL, false, buf, sizeof(buf));
    ASSERT("no-target ok", s == BD_OK);
    ASSERT("generic dest",  has(buf, "generic/platform=iOS"));
    PASS("build_cmd_no_target");
    return 0;
}

static int test_build_cmd_single_quote_escape(void) {
    char buf[8192];
    /* Scheme name with a single quote */
    RunConfig rc = make_config("/proj/App.xcodeproj", "My'Scheme", "Debug");
    BdStatus s = bd_build_cmd(&rc, NULL, false, buf, sizeof(buf));
    ASSERT("quote escape ok", s == BD_OK);
    /* Single-quote-escaped value contains '\'' sequence */
    ASSERT("escaped quote", has(buf, "'\\''"));
    PASS("build_cmd_single_quote_escape");
    return 0;
}

/* ── bd_boot_cmd / bd_bootstatus_cmd ─────────────────────────────── */

static int test_boot_cmd(void) {
    char buf[1024];
    Target t = make_sim("SIM-UDID-1234");
    BdStatus s = bd_boot_cmd(&t, buf, sizeof(buf));
    ASSERT("boot ok",     s == BD_OK);
    ASSERT("boot simctl", has(buf, "simctl"));
    ASSERT("boot verb",   has(buf, "boot"));
    ASSERT("boot udid",   has(buf, "SIM-UDID-1234"));
    PASS("boot_cmd");
    return 0;
}

static int test_bootstatus_cmd(void) {
    char buf[1024];
    Target t = make_sim("SIM-UDID-1234");
    BdStatus s = bd_bootstatus_cmd(&t, buf, sizeof(buf));
    ASSERT("bootstatus ok",     s == BD_OK);
    ASSERT("bootstatus simctl", has(buf, "simctl"));
    ASSERT("bootstatus verb",   has(buf, "bootstatus"));
    ASSERT("bootstatus wait",   has(buf, "--wait"));
    PASS("bootstatus_cmd");
    return 0;
}

/* ── bd_install_cmd ───────────────────────────────────────────────── */

static int test_install_cmd_device(void) {
    char buf[4096];
    Target t = make_device("DEV-UDID-XYZ");
    BdStatus s = bd_install_cmd(&t, "/build/MyApp.app", buf, sizeof(buf));
    ASSERT("install device ok",       s == BD_OK);
    ASSERT("install device devicectl", has(buf, "devicectl"));
    ASSERT("install device udid",     has(buf, "DEV-UDID-XYZ"));
    ASSERT("install device app path", has(buf, "'/build/MyApp.app'"));
    ASSERT("no simctl",               !has(buf, "simctl"));
    PASS("install_cmd_device");
    return 0;
}

static int test_install_cmd_sim(void) {
    char buf[4096];
    Target t = make_sim("SIM-UDID-XYZ");
    BdStatus s = bd_install_cmd(&t, "/build/MyApp.app", buf, sizeof(buf));
    ASSERT("install sim ok",     s == BD_OK);
    ASSERT("install sim simctl", has(buf, "simctl"));
    ASSERT("install sim install", has(buf, "install"));
    ASSERT("install sim udid",   has(buf, "SIM-UDID-XYZ"));
    ASSERT("no devicectl",       !has(buf, "devicectl"));
    PASS("install_cmd_sim");
    return 0;
}

/* ── bd_launch_cmd ───────────────────────────────────────────────── */

static int test_launch_cmd_device(void) {
    char buf[4096];
    Target t = make_device("DEV-UDID-LAUNCH");
    BdStatus s = bd_launch_cmd(&t, "com.example.App", buf, sizeof(buf));
    ASSERT("launch device ok",       s == BD_OK);
    ASSERT("launch device setsid",   has(buf, "setsid"));
    ASSERT("launch device marker",   has(buf, "__OSTRICH_PGID__"));
    ASSERT("launch device console",  has(buf, "--console"));
    ASSERT("launch device devicectl", has(buf, "devicectl"));
    ASSERT("launch device udid",     has(buf, "DEV-UDID-LAUNCH"));
    ASSERT("launch device bundle",   has(buf, "com.example.App"));
    ASSERT("no simctl",              !has(buf, "simctl"));
    PASS("launch_cmd_device");
    return 0;
}

static int test_launch_cmd_sim(void) {
    char buf[4096];
    Target t = make_sim("SIM-UDID-LAUNCH");
    BdStatus s = bd_launch_cmd(&t, "com.example.App", buf, sizeof(buf));
    ASSERT("launch sim ok",      s == BD_OK);
    ASSERT("launch sim setsid",  has(buf, "setsid"));
    ASSERT("launch sim marker",  has(buf, "__OSTRICH_PGID__"));
    ASSERT("launch sim console", has(buf, "--console"));
    ASSERT("launch sim simctl",  has(buf, "simctl"));
    ASSERT("launch sim udid",    has(buf, "SIM-UDID-LAUNCH"));
    ASSERT("no devicectl",       !has(buf, "devicectl"));
    PASS("launch_cmd_sim");
    return 0;
}

/* ── bd_terminate_cmd ────────────────────────────────────────────── */

static int test_terminate_cmd_device(void) {
    char buf[2048];
    Target t = make_device("DEV-UDID-TERM");
    BdStatus s = bd_terminate_cmd(&t, "com.example.App", buf, sizeof(buf));
    ASSERT("terminate device ok",      s == BD_OK);
    ASSERT("terminate devicectl",      has(buf, "devicectl"));
    ASSERT("terminate udid",           has(buf, "DEV-UDID-TERM"));
    ASSERT("terminate bundle",         has(buf, "com.example.App"));
    ASSERT("no simctl",                !has(buf, "simctl"));
    PASS("terminate_cmd_device");
    return 0;
}

static int test_terminate_cmd_sim(void) {
    char buf[2048];
    Target t = make_sim("SIM-UDID-TERM");
    BdStatus s = bd_terminate_cmd(&t, "com.example.App", buf, sizeof(buf));
    ASSERT("terminate sim ok",    s == BD_OK);
    ASSERT("terminate simctl",    has(buf, "simctl"));
    ASSERT("terminate terminate", has(buf, "terminate"));
    ASSERT("terminate udid",      has(buf, "SIM-UDID-TERM"));
    ASSERT("no devicectl",        !has(buf, "devicectl"));
    PASS("terminate_cmd_sim");
    return 0;
}

/* ── bd_kill_cmd ─────────────────────────────────────────────────── */

static int test_kill_cmd(void) {
    char buf[256];
    BdStatus s = bd_kill_cmd(12345, buf, sizeof(buf));
    ASSERT("kill ok",   s == BD_OK);
    ASSERT("kill cmd",  has(buf, "kill"));
    ASSERT("kill pgid", has(buf, "-12345"));
    ASSERT("kill --",   has(buf, "--"));
    PASS("kill_cmd");
    return 0;
}

/* ── bd_parse_product_path ───────────────────────────────────────── */

/* Canonical -showBuildSettings -json fixture (minimal). */
static const char *k_settings_json =
    "[{\"action\":\"build\","
    "\"buildSettings\":{"
    "\"BUILT_PRODUCTS_DIR\":\"/Users/jake/DerivedData/App/Build/Products/Debug-iphoneos\","
    "\"FULL_PRODUCT_NAME\":\"MyApp.app\","
    "\"PRODUCT_BUNDLE_IDENTIFIER\":\"com.example.MyApp\""
    "},\"target\":\"MyApp\"}]";

static int test_parse_product_path_happy(void) {
    char out[4096];
    Str json = { k_settings_json, strlen(k_settings_json) };
    BdStatus s = bd_parse_product_path(json, out, sizeof(out));
    ASSERT("parse ok",         s == BD_OK);
    ASSERT("parse dir",        has(out, "/Users/jake/DerivedData/App/Build/Products/Debug-iphoneos"));
    ASSERT("parse product",    has(out, "MyApp.app"));
    ASSERT("parse slash",      has(out, "/MyApp.app"));
    PASS("parse_product_path_happy");
    return 0;
}

static int test_parse_product_path_malformed(void) {
    char out[256];
    const char *bad = "not json at all {{{{";
    Str json = { bad, strlen(bad) };
    BdStatus s = bd_parse_product_path(json, out, sizeof(out));
    ASSERT("malformed err", s == BD_ERR_PARSE);
    PASS("parse_product_path_malformed");
    return 0;
}

static int test_parse_product_path_empty(void) {
    char out[256];
    Str json = { "", 0 };
    BdStatus s = bd_parse_product_path(json, out, sizeof(out));
    ASSERT("empty err", s == BD_ERR_PARSE);
    PASS("parse_product_path_empty");
    return 0;
}

static int test_parse_product_path_missing_fields(void) {
    /* JSON is valid but lacks BUILT_PRODUCTS_DIR / FULL_PRODUCT_NAME */
    const char *j = "[{\"buildSettings\":{\"OTHER_KEY\":\"val\"},\"target\":\"T\"}]";
    char out[256];
    Str json = { j, strlen(j) };
    BdStatus s = bd_parse_product_path(json, out, sizeof(out));
    ASSERT("missing fields err", s == BD_ERR_PARSE);
    PASS("parse_product_path_missing_fields");
    return 0;
}

/* ── bd_parse_pid_marker ─────────────────────────────────────────── */

static int test_parse_pid_marker_found(void) {
    const char *chunk = "some build output\n__OSTRICH_PGID__99321\nmore output\n";
    long pgid = 0;
    Str s = { chunk, strlen(chunk) };
    bool found = bd_parse_pid_marker(s, &pgid);
    ASSERT("marker found",    found);
    ASSERT("marker pgid",     pgid == 99321);
    PASS("parse_pid_marker_found");
    return 0;
}

static int test_parse_pid_marker_not_found(void) {
    const char *chunk = "xcodebuild output without any marker\n";
    long pgid = 0;
    Str s = { chunk, strlen(chunk) };
    bool found = bd_parse_pid_marker(s, &pgid);
    ASSERT("no marker", !found);
    PASS("parse_pid_marker_not_found");
    return 0;
}

static int test_parse_pid_marker_at_start(void) {
    const char *chunk = "__OSTRICH_PGID__1\nrest";
    long pgid = 0;
    Str s = { chunk, strlen(chunk) };
    bool found = bd_parse_pid_marker(s, &pgid);
    ASSERT("start found", found);
    ASSERT("start pgid",  pgid == 1);
    PASS("parse_pid_marker_at_start");
    return 0;
}

static int test_parse_pid_marker_empty_chunk(void) {
    long pgid = 0;
    Str s = { "", 0 };
    bool found = bd_parse_pid_marker(s, &pgid);
    ASSERT("empty no marker", !found);
    PASS("parse_pid_marker_empty");
    return 0;
}

static int test_parse_pid_marker_partial_prefix(void) {
    /* prefix present but no digits follow */
    const char *chunk = "__OSTRICH_PGID__\n";
    long pgid = 0;
    Str s = { chunk, strlen(chunk) };
    bool found = bd_parse_pid_marker(s, &pgid);
    ASSERT("no digits → not found", !found);
    PASS("parse_pid_marker_no_digits");
    return 0;
}

/* ── bd_reason_lex ───────────────────────────────────────────────── */

static int test_reason_lex(void) {
    ASSERT("xcode missing → rec err", bd_reason_lex(BD_ERR_XCODE_MISSING) == LEX_REC_ERR_XCODE);
    ASSERT("build → build failed",    bd_reason_lex(BD_ERR_BUILD) == LEX_RUN_BUILD_FAILED);
    ASSERT("parse → build failed",    bd_reason_lex(BD_ERR_PARSE) == LEX_RUN_BUILD_FAILED);
    ASSERT("boot → deploy failed",    bd_reason_lex(BD_ERR_BOOT)    == LEX_RUN_DEPLOY_FAILED);
    ASSERT("install → deploy failed", bd_reason_lex(BD_ERR_INSTALL) == LEX_RUN_DEPLOY_FAILED);
    ASSERT("launch → deploy failed",  bd_reason_lex(BD_ERR_LAUNCH)  == LEX_RUN_DEPLOY_FAILED);
    /* build vs deploy are distinct */
    ASSERT("build != deploy", bd_reason_lex(BD_ERR_BUILD) != bd_reason_lex(BD_ERR_INSTALL));
    PASS("reason_lex");
    return 0;
}

/* ── bd_status_str ───────────────────────────────────────────────── */

static int test_status_str(void) {
    ASSERT("ok str",      bd_status_str(BD_OK)[0] != '\0');
    ASSERT("xcode str",   bd_status_str(BD_ERR_XCODE_MISSING)[0] != '\0');
    ASSERT("build str",   bd_status_str(BD_ERR_BUILD)[0] != '\0');
    ASSERT("boot str",    bd_status_str(BD_ERR_BOOT)[0] != '\0');
    ASSERT("install str", bd_status_str(BD_ERR_INSTALL)[0] != '\0');
    ASSERT("launch str",  bd_status_str(BD_ERR_LAUNCH)[0] != '\0');
    ASSERT("parse str",   bd_status_str(BD_ERR_PARSE)[0] != '\0');
    ASSERT("oom str",     bd_status_str(BD_ERR_OOM)[0] != '\0');
    PASS("status_str");
    return 0;
}

/* ── OOM / tiny buffer ───────────────────────────────────────────── */

static int test_oom_tiny_buf(void) {
    char buf[4];
    RunConfig rc = make_config("/proj/App.xcodeproj", "App", "Debug");
    ASSERT("settings oom", bd_settings_cmd(&rc, NULL, false, buf, sizeof(buf)) == BD_ERR_OOM);

    Target t = make_sim("SIM");
    ASSERT("boot oom",       bd_boot_cmd(&t, buf, sizeof(buf))       == BD_ERR_OOM);
    ASSERT("bootstatus oom", bd_bootstatus_cmd(&t, buf, sizeof(buf)) == BD_ERR_OOM);
    ASSERT("install oom",    bd_install_cmd(&t, "/app", buf, sizeof(buf)) == BD_ERR_OOM);
    ASSERT("terminate oom",  bd_terminate_cmd(&t, "com.x", buf, sizeof(buf)) == BD_ERR_OOM);
    ASSERT("kill oom",       bd_kill_cmd(123, buf, sizeof(buf))      == BD_ERR_OOM);
    ASSERT("dest oom",       bd_destination(&t, true, buf, sizeof(buf)) == BD_ERR_OOM);

    PASS("oom_tiny_buf");
    return 0;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(void) {
    int failures = 0;

    failures += test_destination_no_target();
    failures += test_destination_device();
    failures += test_destination_sim();

    failures += test_settings_cmd_no_target();
    failures += test_settings_cmd_workspace();
    failures += test_settings_cmd_space_in_path();

    failures += test_build_cmd_has_setsid_and_marker();
    failures += test_build_cmd_no_target_generic_dest();
    failures += test_build_cmd_single_quote_escape();

    failures += test_boot_cmd();
    failures += test_bootstatus_cmd();

    failures += test_install_cmd_device();
    failures += test_install_cmd_sim();

    failures += test_launch_cmd_device();
    failures += test_launch_cmd_sim();

    failures += test_terminate_cmd_device();
    failures += test_terminate_cmd_sim();

    failures += test_kill_cmd();

    failures += test_parse_product_path_happy();
    failures += test_parse_product_path_malformed();
    failures += test_parse_product_path_empty();
    failures += test_parse_product_path_missing_fields();

    failures += test_parse_pid_marker_found();
    failures += test_parse_pid_marker_not_found();
    failures += test_parse_pid_marker_at_start();
    failures += test_parse_pid_marker_empty_chunk();
    failures += test_parse_pid_marker_partial_prefix();

    failures += test_reason_lex();
    failures += test_status_str();
    failures += test_oom_tiny_buf();

    if (failures == 0) {
        printf("all builddeploy tests passed\n");
        return 0;
    }
    printf("%d builddeploy test(s) FAILED\n", failures);
    return 1;
}
