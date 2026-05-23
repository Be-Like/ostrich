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

static void setup(void) {
    snprintf(g_xdg_dir,   sizeof(g_xdg_dir),   "/tmp/ost_store_%d", (int)getpid());
    snprintf(g_conn_path, sizeof(g_conn_path),  "%s/ostrich/connections", g_xdg_dir);
    snprintf(g_tmp_path,  sizeof(g_tmp_path),   "%s.tmp", g_conn_path);

    /* clean slate */
    unlink(g_conn_path);
    unlink(g_tmp_path);
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

/* ── cleanup temp dir ─────────────────────────────────────────────────── */

static void cleanup(void) {
    unlink(g_conn_path);
    unlink(g_tmp_path);
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

    cleanup();
    arena_destroy(g_arena);

    if (failures == 0) {
        printf("All store tests passed.\n");
        return 0;
    }
    printf("%d store test(s) failed.\n", failures);
    return 1;
}
