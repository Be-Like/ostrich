#define _POSIX_C_SOURCE 200809L

#include "../include/discovery.h"
#include "../include/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) \
    do { if (!(cond)) { \
        printf("FAIL: %s [assertion: %s]\n", __func__, (name)); \
        return 1; \
    } } while (0)

#define CMD_CAP 8192

static Arena *g_arena;

/* ── disc_scan_cmd ──────────────────────────────────────────────── */

static int test_scan_cmd_basic(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_scan_cmd("/Users/jake/Dev", 5, buf, sizeof(buf));
    ASSERT("returns OK",           s == DISC_OK);
    ASSERT("contains quoted root", strstr(buf, "'/Users/jake/Dev'") != NULL);
    ASSERT("contains -maxdepth 5", strstr(buf, "-maxdepth 5") != NULL);
    ASSERT("prunes Pods",          strstr(buf, "Pods") != NULL);
    ASSERT("prunes Carthage",      strstr(buf, "Carthage") != NULL);
    ASSERT("prunes .build",        strstr(buf, ".build") != NULL);
    ASSERT("prunes DerivedData",   strstr(buf, "DerivedData") != NULL);
    ASSERT("prunes node_modules",  strstr(buf, "node_modules") != NULL);
    ASSERT("finds xcodeproj",      strstr(buf, "*.xcodeproj") != NULL);
    ASSERT("finds xcworkspace",    strstr(buf, "*.xcworkspace") != NULL);
    PASS("scan_cmd_basic");
    return 0;
}

static int test_scan_cmd_spaces_in_root(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_scan_cmd("/Users/jake/My Projects", 3, buf, sizeof(buf));
    ASSERT("returns OK",                s == DISC_OK);
    ASSERT("root single-quoted",        strstr(buf, "'/Users/jake/My Projects'") != NULL);
    ASSERT("contains -maxdepth 3",      strstr(buf, "-maxdepth 3") != NULL);
    PASS("scan_cmd_spaces_in_root");
    return 0;
}

static int test_scan_cmd_single_quote_in_root(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_scan_cmd("/Users/jake/It's Mine", 4, buf, sizeof(buf));
    ASSERT("returns OK",          s == DISC_OK);
    /* The quote escape sequence '\'' must appear in the command. */
    ASSERT("escape sequence present", strstr(buf, "'\\''"  ) != NULL);
    ASSERT("path prefix present",     strstr(buf, "/Users/jake/It") != NULL);
    ASSERT("path suffix present",     strstr(buf, "s Mine") != NULL);
    PASS("scan_cmd_single_quote_in_root");
    return 0;
}

static int test_scan_cmd_maxdepth(void) {
    char buf[CMD_CAP];
    disc_scan_cmd("/root", 12, buf, sizeof(buf));
    ASSERT("maxdepth 12", strstr(buf, "-maxdepth 12") != NULL);
    PASS("scan_cmd_maxdepth");
    return 0;
}

static int test_scan_cmd_buf_too_small(void) {
    char buf[10];
    DiscStatus s = disc_scan_cmd("/very/long/root/path", 5, buf, sizeof(buf));
    ASSERT("returns OOM", s == DISC_ERR_OOM);
    PASS("scan_cmd_buf_too_small");
    return 0;
}

/* ── disc_list_cmd ──────────────────────────────────────────────── */

static int test_list_cmd_xcodeproj(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_list_cmd("/path/to/MyApp.xcodeproj", buf, sizeof(buf));
    ASSERT("returns OK",       s == DISC_OK);
    ASSERT("uses -project",    strstr(buf, "-project") != NULL);
    ASSERT("no -workspace",    strstr(buf, "-workspace") == NULL);
    ASSERT("contains -list",   strstr(buf, "-list") != NULL);
    ASSERT("contains -json",   strstr(buf, "-json") != NULL);
    ASSERT("quoted path",      strstr(buf, "'/path/to/MyApp.xcodeproj'") != NULL);
    PASS("list_cmd_xcodeproj");
    return 0;
}

static int test_list_cmd_xcworkspace(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_list_cmd("/path/to/MyApp.xcworkspace", buf, sizeof(buf));
    ASSERT("returns OK",       s == DISC_OK);
    ASSERT("uses -workspace",  strstr(buf, "-workspace") != NULL);
    ASSERT("no -project",      strstr(buf, "-project") == NULL);
    ASSERT("quoted path",      strstr(buf, "'/path/to/MyApp.xcworkspace'") != NULL);
    PASS("list_cmd_xcworkspace");
    return 0;
}

/* ── disc_build_settings_cmd ────────────────────────────────────── */

static int test_build_settings_cmd_project(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_build_settings_cmd(
        "/path/to/MyApp.xcodeproj", "MyApp", "Debug", buf, sizeof(buf));
    ASSERT("returns OK",              s == DISC_OK);
    ASSERT("uses -project",           strstr(buf, "-project") != NULL);
    ASSERT("-showBuildSettings",      strstr(buf, "-showBuildSettings") != NULL);
    ASSERT("-json",                   strstr(buf, "-json") != NULL);
    ASSERT("quoted project path",     strstr(buf, "'/path/to/MyApp.xcodeproj'") != NULL);
    ASSERT("quoted scheme",           strstr(buf, "'MyApp'") != NULL);
    ASSERT("quoted config",           strstr(buf, "'Debug'") != NULL);
    ASSERT("-scheme present",         strstr(buf, "-scheme") != NULL);
    ASSERT("-configuration present",  strstr(buf, "-configuration") != NULL);
    PASS("build_settings_cmd_project");
    return 0;
}

static int test_build_settings_cmd_workspace(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_build_settings_cmd(
        "/path/to/MyApp.xcworkspace", "MyApp", "Release", buf, sizeof(buf));
    ASSERT("returns OK",        s == DISC_OK);
    ASSERT("uses -workspace",   strstr(buf, "-workspace") != NULL);
    ASSERT("no -project",       strstr(buf, "-project") == NULL);
    ASSERT("quoted workspace",  strstr(buf, "'/path/to/MyApp.xcworkspace'") != NULL);
    ASSERT("quoted config",     strstr(buf, "'Release'") != NULL);
    PASS("build_settings_cmd_workspace");
    return 0;
}

static int test_build_settings_cmd_spaces(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_build_settings_cmd(
        "/path/to/My App.xcodeproj", "My Scheme", "Debug", buf, sizeof(buf));
    ASSERT("returns OK",        s == DISC_OK);
    ASSERT("quoted path",       strstr(buf, "'/path/to/My App.xcodeproj'") != NULL);
    ASSERT("quoted scheme",     strstr(buf, "'My Scheme'") != NULL);
    PASS("build_settings_cmd_spaces");
    return 0;
}

/* ── disc_simctl_cmd ────────────────────────────────────────────── */

static int test_simctl_cmd(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_simctl_cmd(buf, sizeof(buf));
    ASSERT("returns OK",    s == DISC_OK);
    ASSERT("xcrun simctl",  strstr(buf, "xcrun simctl") != NULL);
    ASSERT("list devices",  strstr(buf, "list devices") != NULL);
    ASSERT("--json",        strstr(buf, "--json") != NULL);
    PASS("simctl_cmd");
    return 0;
}

/* ── disc_devicectl_cmd ─────────────────────────────────────────── */

static int test_devicectl_cmd(void) {
    char buf[CMD_CAP];
    DiscStatus s = disc_devicectl_cmd(buf, sizeof(buf));
    ASSERT("returns OK",      s == DISC_OK);
    ASSERT("xcrun devicectl", strstr(buf, "xcrun devicectl") != NULL);
    ASSERT("list devices",    strstr(buf, "list devices") != NULL);
    ASSERT("--json-output",   strstr(buf, "--json-output") != NULL);
    PASS("devicectl_cmd");
    return 0;
}

/* ── disc_curate_blueprints ─────────────────────────────────────── */

static int test_curate_empty(void) {
    arena_reset(g_arena);
    Str find_out = { .data = "", .len = 0 };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",  s == DISC_OK);
    ASSERT("count is 0",  out.count == 0);
    PASS("curate_empty");
    return 0;
}

static int test_curate_artifact_dropped(void) {
    arena_reset(g_arena);
    const char *data =
        "/Users/jake/Dev/MyApp.xcodeproj/project.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",     s == DISC_OK);
    ASSERT("artifact gone",  out.count == 0);
    PASS("curate_artifact_dropped");
    return 0;
}

static int test_curate_standalone_project(void) {
    arena_reset(g_arena);
    const char *data =
        "/Users/jake/Dev/MyLib.xcodeproj\n"
        "/Users/jake/Dev/MyLib.xcodeproj/project.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",        s == DISC_OK);
    ASSERT("count is 1",        out.count == 1);
    ASSERT("path matches",      strcmp(out.items[0].path,
                                       "/Users/jake/Dev/MyLib.xcodeproj") == 0);
    ASSERT("not workspace",     out.items[0].is_workspace == false);
    PASS("curate_standalone_project");
    return 0;
}

static int test_curate_workspace_preferred(void) {
    arena_reset(g_arena);
    /* workspace and project co-located: project should be dropped */
    const char *data =
        "/Users/jake/Dev/MyApp.xcodeproj\n"
        "/Users/jake/Dev/MyApp.xcodeproj/project.xcworkspace\n"
        "/Users/jake/Dev/MyApp.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",        s == DISC_OK);
    ASSERT("count is 1",        out.count == 1);
    ASSERT("workspace kept",    strcmp(out.items[0].path,
                                       "/Users/jake/Dev/MyApp.xcworkspace") == 0);
    ASSERT("is_workspace true", out.items[0].is_workspace == true);
    PASS("curate_workspace_preferred");
    return 0;
}

static int test_curate_workspace_preferred_order_reversed(void) {
    arena_reset(g_arena);
    /* workspace listed before xcodeproj — same result */
    const char *data =
        "/Users/jake/Dev/MyApp.xcworkspace\n"
        "/Users/jake/Dev/MyApp.xcodeproj\n"
        "/Users/jake/Dev/MyApp.xcodeproj/project.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",     s == DISC_OK);
    ASSERT("count is 1",     out.count == 1);
    ASSERT("workspace kept", out.items[0].is_workspace == true);
    PASS("curate_workspace_preferred_order_reversed");
    return 0;
}

static int test_curate_mixed(void) {
    arena_reset(g_arena);
    /* MyApp: co-located workspace+project → workspace wins.
       OtherLib: only project, no workspace → project kept. */
    const char *data =
        "/Users/jake/Dev/MyApp.xcodeproj\n"
        "/Users/jake/Dev/MyApp.xcodeproj/project.xcworkspace\n"
        "/Users/jake/Dev/MyApp.xcworkspace\n"
        "/Users/jake/Dev/OtherLib/OtherLib.xcodeproj\n"
        "/Users/jake/Dev/OtherLib/OtherLib.xcodeproj/project.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",      s == DISC_OK);
    ASSERT("count is 2",      out.count == 2);

    /* Order: MyApp.xcworkspace first (from input order), then OtherLib.xcodeproj */
    bool found_ws   = false;
    bool found_proj = false;
    for (int i = 0; i < out.count; i++) {
        if (strcmp(out.items[i].path, "/Users/jake/Dev/MyApp.xcworkspace") == 0 &&
            out.items[i].is_workspace) {
            found_ws = true;
        }
        if (strcmp(out.items[i].path,
                   "/Users/jake/Dev/OtherLib/OtherLib.xcodeproj") == 0 &&
            !out.items[i].is_workspace) {
            found_proj = true;
        }
    }
    ASSERT("workspace found", found_ws);
    ASSERT("standalone proj found", found_proj);
    PASS("curate_mixed");
    return 0;
}

static int test_curate_no_trailing_newline(void) {
    arena_reset(g_arena);
    /* find output without a trailing newline */
    const char *data = "/Users/jake/Dev/MyLib.xcodeproj";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",   s == DISC_OK);
    ASSERT("count is 1",   out.count == 1);
    ASSERT("path correct", strcmp(out.items[0].path,
                                   "/Users/jake/Dev/MyLib.xcodeproj") == 0);
    PASS("curate_no_trailing_newline");
    return 0;
}

static int test_curate_only_workspaces(void) {
    arena_reset(g_arena);
    const char *data =
        "/Users/jake/Dev/Alpha.xcworkspace\n"
        "/Users/jake/Dev/Beta.xcworkspace\n";
    Str find_out = { .data = data, .len = strlen(data) };
    BlueprintList out = {0};
    DiscStatus s = disc_curate_blueprints(g_arena, find_out, 5, &out);
    ASSERT("returns OK",  s == DISC_OK);
    ASSERT("count is 2",  out.count == 2);
    ASSERT("both ws",     out.items[0].is_workspace && out.items[1].is_workspace);
    PASS("curate_only_workspaces");
    return 0;
}

/* ── disc_readiness ─────────────────────────────────────────────── */

static int test_readiness_no_project(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    ASSERT("READY_NO_PROJECT", disc_readiness(&rc, false) == READY_NO_PROJECT);
    PASS("readiness_no_project");
    return 0;
}

static int test_readiness_no_scheme(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    strncpy(rc.project, "/path/to/App.xcodeproj", sizeof(rc.project) - 1);
    ASSERT("READY_NO_SCHEME", disc_readiness(&rc, false) == READY_NO_SCHEME);
    PASS("readiness_no_scheme");
    return 0;
}

static int test_readiness_no_config(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    strncpy(rc.project, "/path/to/App.xcodeproj", sizeof(rc.project) - 1);
    strncpy(rc.scheme,  "MyScheme",               sizeof(rc.scheme)  - 1);
    ASSERT("READY_NO_CONFIG", disc_readiness(&rc, false) == READY_NO_CONFIG);
    PASS("readiness_no_config");
    return 0;
}

static int test_readiness_no_bundle_id(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    strncpy(rc.project, "/path/to/App.xcodeproj", sizeof(rc.project)   - 1);
    strncpy(rc.scheme,  "MyScheme",               sizeof(rc.scheme)    - 1);
    strncpy(rc.config,  "Debug",                  sizeof(rc.config)    - 1);
    ASSERT("READY_NO_BUNDLE_ID", disc_readiness(&rc, false) == READY_NO_BUNDLE_ID);
    PASS("readiness_no_bundle_id");
    return 0;
}

static int test_readiness_no_target(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    strncpy(rc.project,   "/path/to/App.xcodeproj", sizeof(rc.project)   - 1);
    strncpy(rc.scheme,    "MyScheme",               sizeof(rc.scheme)    - 1);
    strncpy(rc.config,    "Debug",                  sizeof(rc.config)    - 1);
    strncpy(rc.bundle_id, "com.example.app",        sizeof(rc.bundle_id) - 1);
    ASSERT("READY_NO_TARGET", disc_readiness(&rc, false) == READY_NO_TARGET);
    PASS("readiness_no_target");
    return 0;
}

static int test_readiness_ok(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    strncpy(rc.project,   "/path/to/App.xcodeproj", sizeof(rc.project)   - 1);
    strncpy(rc.scheme,    "MyScheme",               sizeof(rc.scheme)    - 1);
    strncpy(rc.config,    "Debug",                  sizeof(rc.config)    - 1);
    strncpy(rc.bundle_id, "com.example.app",        sizeof(rc.bundle_id) - 1);
    ASSERT("READY_OK", disc_readiness(&rc, true) == READY_OK);
    PASS("readiness_ok");
    return 0;
}

static int test_readiness_target_selected_does_not_override_missing_fields(void) {
    RunConfig rc;
    memset(&rc, 0, sizeof(rc));
    /* target_sel=true but no project */
    ASSERT("still NO_PROJECT", disc_readiness(&rc, true) == READY_NO_PROJECT);
    PASS("readiness_target_does_not_override_fields");
    return 0;
}

/* ── disc_status_str ────────────────────────────────────────────── */

static int test_status_str(void) {
    ASSERT("DISC_OK str",                 disc_status_str(DISC_OK)                 != NULL);
    ASSERT("DISC_ERR_XCODE_MISSING str",  disc_status_str(DISC_ERR_XCODE_MISSING)  != NULL);
    ASSERT("DISC_ERR_COMMAND_FAILED str", disc_status_str(DISC_ERR_COMMAND_FAILED) != NULL);
    ASSERT("DISC_ERR_PARSE str",          disc_status_str(DISC_ERR_PARSE)          != NULL);
    ASSERT("DISC_ERR_OOM str",            disc_status_str(DISC_ERR_OOM)            != NULL);
    ASSERT("DISC_OK non-empty",           disc_status_str(DISC_OK)[0]              != '\0');
    ASSERT("unknown non-null",            disc_status_str((DiscStatus)999)         != NULL);
    PASS("status_str");
    return 0;
}

/* ── main ───────────────────────────────────────────────────────── */

int main(void) {
    g_arena = arena_create(1024 * 1024);
    if (!g_arena) { printf("FAIL: arena_create\n"); return 1; }

    int failures = 0;

    /* disc_scan_cmd */
    failures += test_scan_cmd_basic();
    failures += test_scan_cmd_spaces_in_root();
    failures += test_scan_cmd_single_quote_in_root();
    failures += test_scan_cmd_maxdepth();
    failures += test_scan_cmd_buf_too_small();

    /* disc_list_cmd */
    failures += test_list_cmd_xcodeproj();
    failures += test_list_cmd_xcworkspace();

    /* disc_build_settings_cmd */
    failures += test_build_settings_cmd_project();
    failures += test_build_settings_cmd_workspace();
    failures += test_build_settings_cmd_spaces();

    /* disc_simctl_cmd */
    failures += test_simctl_cmd();

    /* disc_devicectl_cmd */
    failures += test_devicectl_cmd();

    /* disc_curate_blueprints */
    failures += test_curate_empty();
    failures += test_curate_artifact_dropped();
    failures += test_curate_standalone_project();
    failures += test_curate_workspace_preferred();
    failures += test_curate_workspace_preferred_order_reversed();
    failures += test_curate_mixed();
    failures += test_curate_no_trailing_newline();
    failures += test_curate_only_workspaces();

    /* disc_readiness */
    failures += test_readiness_no_project();
    failures += test_readiness_no_scheme();
    failures += test_readiness_no_config();
    failures += test_readiness_no_bundle_id();
    failures += test_readiness_no_target();
    failures += test_readiness_ok();
    failures += test_readiness_target_selected_does_not_override_missing_fields();

    /* disc_status_str */
    failures += test_status_str();

    arena_destroy(g_arena);

    if (failures == 0) {
        printf("All discovery tests passed.\n");
        return 0;
    }
    printf("%d discovery test(s) failed.\n", failures);
    return 1;
}
