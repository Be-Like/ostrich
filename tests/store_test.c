#define _POSIX_C_SOURCE 200809L

#include "../include/store.h"
#include "../include/arena.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PASS(name) printf("PASS: %s\n", (name))
#define FAIL(name) do { printf("FAIL: %s\n", (name)); return 1; } while (0)
#define ASSERT(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s [assertion: %s]\n", __func__, (name)); return 1; } } while (0)

static Arena *g_arena;
static char   g_xdg_dir[512];
static char   g_conn_path[600];
static char   g_tmp_path[604];
static char   g_preset_path[600];
static char   g_target_path[600];
static char   g_scanroot_path[600];

static void setup(void) {
    snprintf(g_xdg_dir,       sizeof(g_xdg_dir),       "/tmp/ost_store_%d", (int)getpid());
    snprintf(g_conn_path,     sizeof(g_conn_path),      "%s/ostrich/connections", g_xdg_dir);
    snprintf(g_tmp_path,      sizeof(g_tmp_path),       "%s.tmp", g_conn_path);
    snprintf(g_preset_path,   sizeof(g_preset_path),    "%s/ostrich/presets",   g_xdg_dir);
    snprintf(g_target_path,   sizeof(g_target_path),    "%s/ostrich/targets",   g_xdg_dir);
    snprintf(g_scanroot_path, sizeof(g_scanroot_path),  "%s/ostrich/scanroots", g_xdg_dir);

    /* clean slate */
    unlink(g_conn_path);
    unlink(g_tmp_path);
    unlink(g_preset_path);
    unlink(g_target_path);
    unlink(g_scanroot_path);
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/ostrich", g_xdg_dir);
    rmdir(dir);
    rmdir(g_xdg_dir);
    mkdir(g_xdg_dir, 0700);

    setenv("XDG_CONFIG_HOME", g_xdg_dir, 1);
    arena_reset(g_arena);
}

/* ── no file: empty list, no error ──────────────────────────────────── */

static int test_no_file(void) {
    setup();
    ConnList out = {0};
    StoreStatus s = store_load(g_arena, &out);
    ASSERT("no_file → STORE_OK",  s == STORE_OK);
    ASSERT("no_file → count 0",   out.count == 0);
    PASS("no_file");
    return 0;
}

/* ── agent connection round-trip ─────────────────────────────────────── */

static int test_agent_roundtrip(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.label, "My Mac",    sizeof(c.label) - 1);
    strncpy(c.host,  "mac.local", sizeof(c.host)  - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth = SSH_AUTH_AGENT;

    ConnList in_list = { &c, 1, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("agent save → OK", s == STORE_OK);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("agent load → OK",    s == STORE_OK);
    ASSERT("agent count",        out.count == 1);
    ASSERT("agent label",        strcmp(out.items[0].label, "My Mac") == 0);
    ASSERT("agent host",         strcmp(out.items[0].host, "mac.local") == 0);
    ASSERT("agent port",         out.items[0].port == 22);
    ASSERT("agent user",         strcmp(out.items[0].user, "alice") == 0);
    ASSERT("agent auth",         out.items[0].auth == SSH_AUTH_AGENT);
    ASSERT("agent mru_index",    out.mru_index == 0);

    PASS("agent_roundtrip");
    return 0;
}

/* ── agent connection never stores a secret ──────────────────────────── */

static int test_agent_no_secret(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth     = SSH_AUTH_AGENT;
    c.remember = true; /* agent with remember set — still no passkey on disk */
    strncpy(c.passkey, "shouldnotbesaved", sizeof(c.passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    store_save(&in_list);

    ConnList out = {0};
    store_load(g_arena, &out);
    ASSERT("agent_no_secret count",    out.count == 1);
    ASSERT("agent_no_secret passkey",  out.items[0].passkey[0] == '\0');
    ASSERT("agent_no_secret remember", out.items[0].remember == false);

    /* verify passkey literal is absent from the file */
    FILE *f = fopen(g_conn_path, "r");
    ASSERT("file exists", f != NULL);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT("passkey key absent",   strstr(buf, "passkey") == NULL);
    ASSERT("passkey value absent", strstr(buf, "shouldnotbesaved") == NULL);

    PASS("agent_no_secret");
    return 0;
}

/* ── opt-in passkey persisted when remember is set ───────────────────── */

static int test_passkey_persisted(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth     = SSH_AUTH_PASSWORD;
    c.remember = true;
    strncpy(c.passkey, "my_secret_pass", sizeof(c.passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    store_save(&in_list);

    ConnList out = {0};
    store_load(g_arena, &out);
    ASSERT("passkey count",    out.count == 1);
    ASSERT("passkey auth",     out.items[0].auth == SSH_AUTH_PASSWORD);
    ASSERT("passkey remember", out.items[0].remember == true);
    ASSERT("passkey value",    strcmp(out.items[0].passkey, "my_secret_pass") == 0);

    PASS("passkey_persisted");
    return 0;
}

/* ── passkey not saved when remember is off ──────────────────────────── */

static int test_no_passkey_without_remember(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth     = SSH_AUTH_PASSWORD;
    c.remember = false;
    strncpy(c.passkey, "temporary_pass", sizeof(c.passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    store_save(&in_list);

    ConnList out = {0};
    store_load(g_arena, &out);
    ASSERT("no_remember count",        out.count == 1);
    ASSERT("no_remember passkey empty", out.items[0].passkey[0] == '\0');
    ASSERT("no_remember remember off",  out.items[0].remember == false);

    FILE *f = fopen(g_conn_path, "r");
    ASSERT("file exists", f != NULL);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT("passkey key absent", strstr(buf, "passkey") == NULL);

    PASS("no_passkey_without_remember");
    return 0;
}

/* ── MRU item is saved first and loads back as item[0] ───────────────── */

static int test_mru_ordering(void) {
    setup();

    Conn conns[3];
    memset(conns, 0, sizeof(conns));

    strncpy(conns[0].host, "host0.local", sizeof(conns[0].host) - 1);
    conns[0].port = 22;
    strncpy(conns[0].user, "user0", sizeof(conns[0].user) - 1);
    conns[0].auth = SSH_AUTH_AGENT;

    strncpy(conns[1].host, "host1.local", sizeof(conns[1].host) - 1);
    conns[1].port = 22;
    strncpy(conns[1].user, "user1", sizeof(conns[1].user) - 1);
    conns[1].auth = SSH_AUTH_AGENT;

    strncpy(conns[2].host, "host2.local", sizeof(conns[2].host) - 1);
    conns[2].port = 22;
    strncpy(conns[2].user, "user2", sizeof(conns[2].user) - 1);
    conns[2].auth = SSH_AUTH_AGENT;

    /* MRU is conns[1] */
    ConnList in_list = { conns, 3, 1 };
    StoreStatus s = store_save(&in_list);
    ASSERT("mru save → OK", s == STORE_OK);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("mru load → OK",           s == STORE_OK);
    ASSERT("mru count",               out.count == 3);
    ASSERT("mru first is conns[1]",   strcmp(out.items[0].host, "host1.local") == 0);
    ASSERT("mru second is conns[0]",  strcmp(out.items[1].host, "host0.local") == 0);
    ASSERT("mru third is conns[2]",   strcmp(out.items[2].host, "host2.local") == 0);
    ASSERT("mru mru_index is 0",      out.mru_index == 0);

    PASS("mru_ordering");
    return 0;
}

/* ── saved file has 0600 permissions (US-43) ─────────────────────────── */

static int test_file_permissions(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth = SSH_AUTH_AGENT;

    ConnList in_list = { &c, 1, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("perms save → OK", s == STORE_OK);

    struct stat st;
    ASSERT("stat succeeds",    stat(g_conn_path, &st) == 0);
    ASSERT("file is 0600",    (st.st_mode & 0777) == 0600);

    PASS("file_permissions");
    return 0;
}

/* ── temp file is removed after successful save (atomic write) ────────── */

static int test_tmp_file_gone(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth = SSH_AUTH_AGENT;

    ConnList in_list = { &c, 1, 0 };
    store_save(&in_list);

    ASSERT("tmp file gone", access(g_tmp_path, F_OK) != 0);

    PASS("tmp_file_gone");
    return 0;
}

/* ── empty list saves and loads cleanly ──────────────────────────────── */

static int test_empty_list(void) {
    setup();

    ConnList in_list = { NULL, 0, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("empty save → OK", s == STORE_OK);
    ASSERT("empty file exists", access(g_conn_path, F_OK) == 0);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("empty load → OK", s == STORE_OK);
    ASSERT("empty count 0",   out.count == 0);

    PASS("empty_list");
    return 0;
}

/* ── multiple connections: all fields survive serialize/deserialize ───── */

static int test_multiple_connections(void) {
    setup();

    Conn conns[4];
    memset(conns, 0, sizeof(conns));

    strncpy(conns[0].label, "Work Mac",   sizeof(conns[0].label) - 1);
    strncpy(conns[0].host,  "work.local", sizeof(conns[0].host)  - 1);
    conns[0].port = 22;
    strncpy(conns[0].user, "alice", sizeof(conns[0].user) - 1);
    conns[0].auth = SSH_AUTH_AGENT;

    strncpy(conns[1].label, "Home Mac",   sizeof(conns[1].label) - 1);
    strncpy(conns[1].host,  "home.local", sizeof(conns[1].host)  - 1);
    conns[1].port = 2222;
    strncpy(conns[1].user, "alice", sizeof(conns[1].user) - 1);
    conns[1].auth     = SSH_AUTH_PASSWORD;
    conns[1].remember = true;
    strncpy(conns[1].passkey, "homepass", sizeof(conns[1].passkey) - 1);

    strncpy(conns[2].host, "lab.local", sizeof(conns[2].host) - 1);
    conns[2].port = 22;
    strncpy(conns[2].user, "bob", sizeof(conns[2].user) - 1);
    conns[2].auth = SSH_AUTH_AGENT;

    strncpy(conns[3].host, "dev.local", sizeof(conns[3].host) - 1);
    conns[3].port = 22;
    strncpy(conns[3].user, "dev", sizeof(conns[3].user) - 1);
    conns[3].auth     = SSH_AUTH_PASSWORD;
    conns[3].remember = false;
    strncpy(conns[3].passkey, "temppass", sizeof(conns[3].passkey) - 1);

    ConnList in_list = { conns, 4, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("multi save → OK", s == STORE_OK);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("multi load → OK", s == STORE_OK);
    ASSERT("multi count",     out.count == 4);

    ASSERT("multi[0] label",    strcmp(out.items[0].label, "Work Mac") == 0);
    ASSERT("multi[0] host",     strcmp(out.items[0].host,  "work.local") == 0);
    ASSERT("multi[0] port",     out.items[0].port == 22);
    ASSERT("multi[0] user",     strcmp(out.items[0].user, "alice") == 0);
    ASSERT("multi[0] auth",     out.items[0].auth == SSH_AUTH_AGENT);

    ASSERT("multi[1] label",    strcmp(out.items[1].label, "Home Mac") == 0);
    ASSERT("multi[1] port",     out.items[1].port == 2222);
    ASSERT("multi[1] remember", out.items[1].remember == true);
    ASSERT("multi[1] passkey",  strcmp(out.items[1].passkey, "homepass") == 0);

    ASSERT("multi[2] user",     strcmp(out.items[2].user, "bob") == 0);
    ASSERT("multi[2] auth",     out.items[2].auth == SSH_AUTH_AGENT);

    /* password auth with remember=false: passkey must not survive */
    ASSERT("multi[3] passkey empty",   out.items[3].passkey[0] == '\0');
    ASSERT("multi[3] remember false",  out.items[3].remember == false);

    PASS("multiple_connections");
    return 0;
}

/* ── store_path resolves to a valid path ─────────────────────────────── */

static int test_store_path(void) {
    setup(); /* sets XDG_CONFIG_HOME */
    char buf[1024];
    StoreStatus s = store_path(buf, sizeof(buf));
    ASSERT("store_path ok",    s == STORE_OK);
    ASSERT("store_path nonempty", buf[0] != '\0');
    size_t len = strlen(buf);
    ASSERT("store_path ends /connections",
           len > 12 && strcmp(buf + len - 12, "/connections") == 0);
    PASS("store_path");
    return 0;
}

/* ── store_status_str returns non-NULL for all codes ─────────────────── */

static int test_status_str(void) {
    ASSERT("ok str",    store_status_str(STORE_OK)         != NULL);
    ASSERT("io str",    store_status_str(STORE_ERR_IO)     != NULL);
    ASSERT("parse str", store_status_str(STORE_ERR_PARSE)  != NULL);
    ASSERT("perms str", store_status_str(STORE_ERR_PERMS)  != NULL);
    ASSERT("oom str",   store_status_str(STORE_ERR_OOM)    != NULL);
    PASS("status_str");
    return 0;
}

/* ── store_conn_key ───────────────────────────────────────────────────── */

static int test_conn_key(void) {
    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;

    char buf[300];
    store_conn_key(&c, buf, sizeof(buf));
    ASSERT("conn_key format", strcmp(buf, "alice@mac.local:22") == 0);

    c.port = 2222;
    store_conn_key(&c, buf, sizeof(buf));
    ASSERT("conn_key non-std port", strcmp(buf, "alice@mac.local:2222") == 0);

    PASS("conn_key");
    return 0;
}

/* ── preset round-trip ────────────────────────────────────────────────── */

static int test_preset_roundtrip(void) {
    setup();

    const char *key = "alice@mac.local:22";

    Preset presets[2];
    memset(presets, 0, sizeof(presets));

    strncpy(presets[0].name,      "app",                     sizeof(presets[0].name)      - 1);
    strncpy(presets[0].project,   "/Users/alice/App.xcworkspace", sizeof(presets[0].project) - 1);
    strncpy(presets[0].scheme,    "App",                     sizeof(presets[0].scheme)    - 1);
    strncpy(presets[0].config,    "Debug",                   sizeof(presets[0].config)    - 1);
    strncpy(presets[0].bundle_id, "com.acme.app",            sizeof(presets[0].bundle_id) - 1);

    strncpy(presets[1].name,      "staging",                 sizeof(presets[1].name)      - 1);
    strncpy(presets[1].project,   "/Users/alice/App.xcworkspace", sizeof(presets[1].project) - 1);
    strncpy(presets[1].scheme,    "Staging",                 sizeof(presets[1].scheme)    - 1);
    strncpy(presets[1].config,    "Release",                 sizeof(presets[1].config)    - 1);
    strncpy(presets[1].bundle_id, "com.acme.staging",        sizeof(presets[1].bundle_id) - 1);

    PresetList in_list = { presets, 2, 1 }; /* active = staging */
    StoreStatus s = preset_save(key, &in_list);
    ASSERT("preset save → OK", s == STORE_OK);

    PresetList out = {0};
    s = preset_load(g_arena, key, &out);
    ASSERT("preset load → OK",        s == STORE_OK);
    ASSERT("preset count",            out.count == 2);
    ASSERT("preset active_index",     out.active_index == 1);

    ASSERT("preset[0] name",          strcmp(out.items[0].name,      "app")                         == 0);
    ASSERT("preset[0] project",       strcmp(out.items[0].project,   "/Users/alice/App.xcworkspace") == 0);
    ASSERT("preset[0] scheme",        strcmp(out.items[0].scheme,    "App")                          == 0);
    ASSERT("preset[0] config",        strcmp(out.items[0].config,    "Debug")                        == 0);
    ASSERT("preset[0] bundle_id",     strcmp(out.items[0].bundle_id, "com.acme.app")                 == 0);

    ASSERT("preset[1] name",          strcmp(out.items[1].name,      "staging")                      == 0);
    ASSERT("preset[1] bundle_id",     strcmp(out.items[1].bundle_id, "com.acme.staging")              == 0);

    PASS("preset_roundtrip");
    return 0;
}

/* ── preset_save preserves other connections' records ─────────────────── */

static int test_preset_isolation(void) {
    setup();

    const char *key1 = "alice@mac1.local:22";
    const char *key2 = "bob@mac2.local:22";

    /* Save one preset for key1 */
    Preset p1;
    memset(&p1, 0, sizeof(p1));
    strncpy(p1.name,      "work",           sizeof(p1.name)      - 1);
    strncpy(p1.project,   "/work/App.xcodeproj", sizeof(p1.project) - 1);
    strncpy(p1.scheme,    "Work",           sizeof(p1.scheme)    - 1);
    strncpy(p1.config,    "Debug",          sizeof(p1.config)    - 1);
    strncpy(p1.bundle_id, "com.work.app",   sizeof(p1.bundle_id) - 1);
    PresetList l1 = { &p1, 1, 0 };
    ASSERT("save key1 → OK", preset_save(key1, &l1) == STORE_OK);

    /* Save one preset for key2 */
    Preset p2;
    memset(&p2, 0, sizeof(p2));
    strncpy(p2.name,      "home",           sizeof(p2.name)      - 1);
    strncpy(p2.project,   "/home/App.xcodeproj", sizeof(p2.project) - 1);
    strncpy(p2.scheme,    "Home",           sizeof(p2.scheme)    - 1);
    strncpy(p2.config,    "Release",        sizeof(p2.config)    - 1);
    strncpy(p2.bundle_id, "com.home.app",   sizeof(p2.bundle_id) - 1);
    PresetList l2 = { &p2, 1, 0 };
    ASSERT("save key2 → OK", preset_save(key2, &l2) == STORE_OK);

    /* Overwrite key1 with different presets */
    Preset p1b;
    memset(&p1b, 0, sizeof(p1b));
    strncpy(p1b.name,      "new",           sizeof(p1b.name)      - 1);
    strncpy(p1b.project,   "/new/App.xcodeproj", sizeof(p1b.project) - 1);
    strncpy(p1b.scheme,    "New",           sizeof(p1b.scheme)    - 1);
    strncpy(p1b.config,    "Debug",         sizeof(p1b.config)    - 1);
    strncpy(p1b.bundle_id, "com.new.app",   sizeof(p1b.bundle_id) - 1);
    PresetList l1b = { &p1b, 1, 0 };
    ASSERT("overwrite key1 → OK", preset_save(key1, &l1b) == STORE_OK);

    /* key2 must still have its original preset */
    PresetList out2 = {0};
    ASSERT("load key2 → OK", preset_load(g_arena, key2, &out2) == STORE_OK);
    ASSERT("key2 count",     out2.count == 1);
    ASSERT("key2 name",      strcmp(out2.items[0].name, "home") == 0);
    ASSERT("key2 config",    strcmp(out2.items[0].config, "Release") == 0);

    /* key1 must have only the new preset */
    PresetList out1 = {0};
    ASSERT("load key1 → OK", preset_load(g_arena, key1, &out1) == STORE_OK);
    ASSERT("key1 count",     out1.count == 1);
    ASSERT("key1 name",      strcmp(out1.items[0].name, "new") == 0);

    PASS("preset_isolation");
    return 0;
}

/* ── preset no file → empty list ─────────────────────────────────────── */

static int test_preset_no_file(void) {
    setup();
    PresetList out = {0};
    StoreStatus s = preset_load(g_arena, "alice@mac.local:22", &out);
    ASSERT("no preset file → OK",    s == STORE_OK);
    ASSERT("no preset file → count", out.count == 0);
    ASSERT("no preset file → active", out.active_index == -1);
    PASS("preset_no_file");
    return 0;
}

/* ── preset unknown keys are ignored on load ─────────────────────────── */

static int test_preset_unknown_keys(void) {
    setup();

    /* Ensure the ostrich subdir exists before writing directly */
    char ostrich_dir[600];
    snprintf(ostrich_dir, sizeof(ostrich_dir), "%s/ostrich", g_xdg_dir);
    mkdir(ostrich_dir, 0700);

    /* Write a hand-crafted record with an unknown key */
    FILE *f = fopen(g_preset_path, "w");
    ASSERT("open preset file", f != NULL);
    fprintf(f, "conn=alice@mac.local:22\n");
    fprintf(f, "name=mypreset\n");
    fprintf(f, "futureprop=somevalue\n"); /* unknown key */
    fprintf(f, "project=/Users/alice/App.xcodeproj\n");
    fprintf(f, "scheme=App\n");
    fprintf(f, "config=Debug\n");
    fprintf(f, "bundleid=com.acme.app\n");
    fclose(f);

    PresetList out = {0};
    StoreStatus s = preset_load(g_arena, "alice@mac.local:22", &out);
    ASSERT("unknown key → OK",      s == STORE_OK);
    ASSERT("unknown key → count",   out.count == 1);
    ASSERT("unknown key → name",    strcmp(out.items[0].name,    "mypreset") == 0);
    ASSERT("unknown key → project", strcmp(out.items[0].project, "/Users/alice/App.xcodeproj") == 0);
    ASSERT("unknown key → scheme",  strcmp(out.items[0].scheme,  "App") == 0);

    PASS("preset_unknown_keys");
    return 0;
}

/* ── preset active_index = -1 when no active marker ─────────────────── */

static int test_preset_no_active(void) {
    setup();

    Preset p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name,   "noactive", sizeof(p.name) - 1);
    strncpy(p.scheme, "App",      sizeof(p.scheme) - 1);
    strncpy(p.config, "Debug",    sizeof(p.config) - 1);
    PresetList in_list = { &p, 1, -1 }; /* -1 = no active */
    ASSERT("save no-active → OK", preset_save("alice@mac.local:22", &in_list) == STORE_OK);

    PresetList out = {0};
    ASSERT("load no-active → OK", preset_load(g_arena, "alice@mac.local:22", &out) == STORE_OK);
    ASSERT("no active_index",     out.active_index == -1);
    ASSERT("count",               out.count == 1);

    PASS("preset_no_active");
    return 0;
}

/* ── remembered target round-trip ────────────────────────────────────── */

static int test_target_roundtrip(void) {
    setup();

    const char *key = "alice@mac.local:22";

    RememberedTarget t;
    memset(&t, 0, sizeof(t));
    strncpy(t.udid, "00008110-001A2D4C0123456E", sizeof(t.udid) - 1);
    strncpy(t.name, "iPhone 15 Pro",             sizeof(t.name) - 1);

    StoreStatus s = target_save(key, &t);
    ASSERT("target save → OK", s == STORE_OK);

    RememberedTarget out;
    memset(&out, 0, sizeof(out));
    s = target_load(key, &out);
    ASSERT("target load → OK",  s == STORE_OK);
    ASSERT("target udid",       strcmp(out.udid, "00008110-001A2D4C0123456E") == 0);
    ASSERT("target name",       strcmp(out.name, "iPhone 15 Pro") == 0);

    PASS("target_roundtrip");
    return 0;
}

/* ── target: no kind field survives a round-trip (no kind in struct) ─── */
/* (kind is never stored — this test verifies the file has no kind line) */

static int test_target_no_kind(void) {
    setup();

    const char *key = "alice@mac.local:22";

    RememberedTarget t;
    memset(&t, 0, sizeof(t));
    strncpy(t.udid, "SIMULATOR-UDID-1234", sizeof(t.udid) - 1);
    strncpy(t.name, "iPhone 15 Simulator", sizeof(t.name) - 1);

    target_save(key, &t);

    /* Read the file; confirm no "kind" key is present */
    FILE *f = fopen(g_target_path, "r");
    ASSERT("target file exists", f != NULL);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT("no kind key in file", strstr(buf, "kind") == NULL);

    PASS("target_no_kind");
    return 0;
}

/* ── target no file → empty (zero udid) ─────────────────────────────── */

static int test_target_no_file(void) {
    setup();
    RememberedTarget out;
    memset(&out, 0, sizeof(out));
    StoreStatus s = target_load("alice@mac.local:22", &out);
    ASSERT("no target file → OK",   s == STORE_OK);
    ASSERT("no target file → udid", out.udid[0] == '\0');
    PASS("target_no_file");
    return 0;
}

/* ── target isolation: save for conn1 leaves conn2 intact ─────────────── */

static int test_target_isolation(void) {
    setup();

    RememberedTarget t1, t2;
    memset(&t1, 0, sizeof(t1));
    memset(&t2, 0, sizeof(t2));
    strncpy(t1.udid, "UDID-1", sizeof(t1.udid) - 1);
    strncpy(t1.name, "Dev Phone", sizeof(t1.name) - 1);
    strncpy(t2.udid, "UDID-2", sizeof(t2.udid) - 1);
    strncpy(t2.name, "Sim iPad", sizeof(t2.name) - 1);

    ASSERT("save t1",  target_save("alice@mac1.local:22", &t1) == STORE_OK);
    ASSERT("save t2",  target_save("bob@mac2.local:22",   &t2) == STORE_OK);

    /* Update t1 */
    RememberedTarget t1b;
    memset(&t1b, 0, sizeof(t1b));
    strncpy(t1b.udid, "UDID-1B", sizeof(t1b.udid) - 1);
    strncpy(t1b.name, "New Phone", sizeof(t1b.name) - 1);
    ASSERT("overwrite t1", target_save("alice@mac1.local:22", &t1b) == STORE_OK);

    /* t2 must be unchanged */
    RememberedTarget out2;
    memset(&out2, 0, sizeof(out2));
    ASSERT("load t2 → OK", target_load("bob@mac2.local:22", &out2) == STORE_OK);
    ASSERT("t2 udid intact", strcmp(out2.udid, "UDID-2") == 0);
    ASSERT("t2 name intact", strcmp(out2.name, "Sim iPad") == 0);

    PASS("target_isolation");
    return 0;
}

/* ── scan root round-trip ─────────────────────────────────────────────── */

static int test_scanroot_roundtrip(void) {
    setup();

    const char *key  = "alice@mac.local:22";
    const char *root = "/Users/alice/Developer";

    StoreStatus s = scanroot_save(key, root);
    ASSERT("scanroot save → OK", s == STORE_OK);

    char out[512];
    s = scanroot_load(key, out, sizeof(out));
    ASSERT("scanroot load → OK",  s == STORE_OK);
    ASSERT("scanroot value",      strcmp(out, "/Users/alice/Developer") == 0);

    PASS("scanroot_roundtrip");
    return 0;
}

/* ── scan root with spaces in path ───────────────────────────────────── */

static int test_scanroot_spaces(void) {
    setup();

    const char *key  = "alice@mac.local:22";
    const char *root = "/Users/alice/My Projects";

    scanroot_save(key, root);

    char out[512];
    scanroot_load(key, out, sizeof(out));
    ASSERT("scanroot with spaces", strcmp(out, "/Users/alice/My Projects") == 0);

    PASS("scanroot_spaces");
    return 0;
}

/* ── scan root no file → empty string ───────────────────────────────── */

static int test_scanroot_no_file(void) {
    setup();
    char out[512];
    out[0] = 'x'; /* ensure it gets cleared */
    StoreStatus s = scanroot_load("alice@mac.local:22", out, sizeof(out));
    ASSERT("no scanroot file → OK",   s == STORE_OK);
    ASSERT("no scanroot file → empty", out[0] == '\0');
    PASS("scanroot_no_file");
    return 0;
}

/* ── scan root isolation ─────────────────────────────────────────────── */

static int test_scanroot_isolation(void) {
    setup();

    scanroot_save("alice@mac1.local:22", "/Users/alice/Dev");
    scanroot_save("bob@mac2.local:22",   "/Users/bob/Projects");
    /* Update alice's root */
    scanroot_save("alice@mac1.local:22", "/Users/alice/NewDev");

    char out[512];
    scanroot_load("bob@mac2.local:22", out, sizeof(out));
    ASSERT("bob root intact", strcmp(out, "/Users/bob/Projects") == 0);

    PASS("scanroot_isolation");
    return 0;
}

/* ── preset file is 0600 ─────────────────────────────────────────────── */

static int test_recon_file_permissions(void) {
    setup();

    Preset p;
    memset(&p, 0, sizeof(p));
    strncpy(p.name,   "test", sizeof(p.name) - 1);
    strncpy(p.scheme, "App",  sizeof(p.scheme) - 1);
    strncpy(p.config, "Debug", sizeof(p.config) - 1);
    PresetList l = { &p, 1, 0 };
    ASSERT("preset save → OK", preset_save("alice@mac.local:22", &l) == STORE_OK);

    struct stat st;
    ASSERT("preset stat ok",  stat(g_preset_path, &st) == 0);
    ASSERT("preset is 0600",  (st.st_mode & 0777) == 0600);

    RememberedTarget t;
    memset(&t, 0, sizeof(t));
    strncpy(t.udid, "UDID-1", sizeof(t.udid) - 1);
    ASSERT("target save → OK", target_save("alice@mac.local:22", &t) == STORE_OK);

    ASSERT("target stat ok",  stat(g_target_path, &st) == 0);
    ASSERT("target is 0600",  (st.st_mode & 0777) == 0600);

    PASS("recon_file_permissions");
    return 0;
}

/* ── kc_remember=true + kc_passkey round-trip ────────────────────────── */

static int test_kc_passkey_roundtrip(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth       = SSH_AUTH_PASSWORD;
    c.remember   = true;
    strncpy(c.passkey, "sshpass", sizeof(c.passkey) - 1);
    c.kc_remember = true;
    strncpy(c.kc_passkey, "keychainpass", sizeof(c.kc_passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("kc roundtrip save → OK", s == STORE_OK);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("kc roundtrip load → OK",       s == STORE_OK);
    ASSERT("kc roundtrip count",           out.count == 1);
    ASSERT("kc roundtrip kc_remember",     out.items[0].kc_remember == true);
    ASSERT("kc roundtrip kc_passkey",      strcmp(out.items[0].kc_passkey, "keychainpass") == 0);
    ASSERT("kc roundtrip ssh passkey",     strcmp(out.items[0].passkey, "sshpass") == 0);

    PASS("kc_passkey_roundtrip");
    return 0;
}

/* ── kc_remember=false: kc_passkey is empty after round-trip ─────────── */

static int test_kc_passkey_off_by_default(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth        = SSH_AUTH_AGENT;
    c.kc_remember = false;
    strncpy(c.kc_passkey, "shouldnotbesaved", sizeof(c.kc_passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    store_save(&in_list);

    ConnList out = {0};
    store_load(g_arena, &out);
    ASSERT("kc off kc_remember false",   out.items[0].kc_remember == false);
    ASSERT("kc off kc_passkey empty",    out.items[0].kc_passkey[0] == '\0');

    FILE *f = fopen(g_conn_path, "r");
    ASSERT("file exists", f != NULL);
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    ASSERT("kc_passkey key absent from file",    strstr(buf, "kc_passkey") == NULL);
    ASSERT("kc_passkey value absent from file",  strstr(buf, "shouldnotbesaved") == NULL);

    PASS("kc_passkey_off_by_default");
    return 0;
}

/* ── agent conn with kc_remember=true round-trips correctly ──────────── */

static int test_kc_passkey_with_agent_auth(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth        = SSH_AUTH_AGENT;
    c.kc_remember = true;
    strncpy(c.kc_passkey, "keychainonly", sizeof(c.kc_passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("agent kc save → OK", s == STORE_OK);

    ConnList out = {0};
    s = store_load(g_arena, &out);
    ASSERT("agent kc load → OK",        s == STORE_OK);
    ASSERT("agent kc auth",             out.items[0].auth == SSH_AUTH_AGENT);
    ASSERT("agent kc_remember",         out.items[0].kc_remember == true);
    ASSERT("agent kc_passkey",          strcmp(out.items[0].kc_passkey, "keychainonly") == 0);
    ASSERT("agent ssh passkey empty",   out.items[0].passkey[0] == '\0');
    ASSERT("agent ssh remember false",  out.items[0].remember == false);

    PASS("kc_passkey_with_agent_auth");
    return 0;
}

/* ── legacy file (pre-T2, no kc fields) loads with safe defaults ─────── */

static int test_kc_passkey_legacy_compat(void) {
    setup();

    /* Ensure the ostrich dir exists before writing directly */
    char ostrich_dir[600];
    snprintf(ostrich_dir, sizeof(ostrich_dir), "%s/ostrich", g_xdg_dir);
    mkdir(ostrich_dir, 0700);

    /* Write a hand-crafted pre-T2 record (no kc_remember / kc_passkey) */
    FILE *f = fopen(g_conn_path, "w");
    ASSERT("open legacy conn file", f != NULL);
    fprintf(f, "host=mac.local\n");
    fprintf(f, "port=22\n");
    fprintf(f, "user=alice\n");
    fprintf(f, "auth=password\n");
    fprintf(f, "remember=1\n");
    fprintf(f, "passkey=sshsecret\n");
    fclose(f);

    ConnList out = {0};
    StoreStatus s = store_load(g_arena, &out);
    ASSERT("legacy load → OK",           s == STORE_OK);
    ASSERT("legacy count",               out.count == 1);
    ASSERT("legacy host",                strcmp(out.items[0].host, "mac.local") == 0);
    ASSERT("legacy ssh passkey intact",  strcmp(out.items[0].passkey, "sshsecret") == 0);
    ASSERT("legacy kc_remember false",   out.items[0].kc_remember == false);
    ASSERT("legacy kc_passkey empty",    out.items[0].kc_passkey[0] == '\0');

    PASS("kc_passkey_legacy_compat");
    return 0;
}

/* ── 0600 permissions hold after format extension ─────────────────────── */

static int test_kc_passkey_file_permissions(void) {
    setup();

    Conn c;
    memset(&c, 0, sizeof(c));
    strncpy(c.host, "mac.local", sizeof(c.host) - 1);
    c.port = 22;
    strncpy(c.user, "alice", sizeof(c.user) - 1);
    c.auth        = SSH_AUTH_AGENT;
    c.kc_remember = true;
    strncpy(c.kc_passkey, "keychainpass", sizeof(c.kc_passkey) - 1);

    ConnList in_list = { &c, 1, 0 };
    StoreStatus s = store_save(&in_list);
    ASSERT("kc perms save → OK", s == STORE_OK);

    struct stat st;
    ASSERT("kc stat succeeds",  stat(g_conn_path, &st) == 0);
    ASSERT("kc file is 0600",  (st.st_mode & 0777) == 0600);

    PASS("kc_passkey_file_permissions");
    return 0;
}

/* ── cleanup temp dir ─────────────────────────────────────────────────── */

static void cleanup(void) {
    unlink(g_conn_path);
    unlink(g_tmp_path);
    unlink(g_preset_path);
    unlink(g_target_path);
    unlink(g_scanroot_path);
    char dir[600];
    snprintf(dir, sizeof(dir), "%s/ostrich", g_xdg_dir);
    rmdir(dir);
    rmdir(g_xdg_dir);
}

int main(void) {
    g_arena = arena_create(1024 * 1024);
    if (!g_arena) { printf("FAIL: arena_create\n"); return 1; }

    int failures = 0;
    failures += test_no_file();
    failures += test_agent_roundtrip();
    failures += test_agent_no_secret();
    failures += test_passkey_persisted();
    failures += test_no_passkey_without_remember();
    failures += test_mru_ordering();
    failures += test_file_permissions();
    failures += test_tmp_file_gone();
    failures += test_empty_list();
    failures += test_multiple_connections();
    failures += test_store_path();
    failures += test_status_str();
    /* recon families */
    failures += test_conn_key();
    failures += test_preset_roundtrip();
    failures += test_preset_isolation();
    failures += test_preset_no_file();
    failures += test_preset_unknown_keys();
    failures += test_preset_no_active();
    failures += test_target_roundtrip();
    failures += test_target_no_kind();
    failures += test_target_no_file();
    failures += test_target_isolation();
    failures += test_scanroot_roundtrip();
    failures += test_scanroot_spaces();
    failures += test_scanroot_no_file();
    failures += test_scanroot_isolation();
    failures += test_recon_file_permissions();
    /* keychain passkey (T2) */
    failures += test_kc_passkey_roundtrip();
    failures += test_kc_passkey_off_by_default();
    failures += test_kc_passkey_with_agent_auth();
    failures += test_kc_passkey_legacy_compat();
    failures += test_kc_passkey_file_permissions();

    cleanup();
    arena_destroy(g_arena);

    if (failures == 0) {
        printf("All store tests passed.\n");
        return 0;
    }
    printf("%d store test(s) failed.\n", failures);
    return 1;
}
