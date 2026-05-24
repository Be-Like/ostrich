#define _POSIX_C_SOURCE 200809L

#include "store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONNS 64
#define MAX_LINE  512
#define PATH_CAP  1024

static StoreStatus ostrich_path(char *buf, size_t cap, const char *name) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int n;
    if (xdg && xdg[0] != '\0') {
        n = snprintf(buf, cap, "%s/ostrich/%s", xdg, name);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') return STORE_ERR_IO;
        n = snprintf(buf, cap, "%s/.config/ostrich/%s", home, name);
    }
    if (n < 0 || (size_t)n >= cap) return STORE_ERR_IO;
    return STORE_OK;
}

StoreStatus store_path(char *buf, size_t cap) {
    return ostrich_path(buf, cap, "connections");
}

/* Create all directory components of filepath's parent. */
static void mkdirs_for(const char *filepath) {
    char tmp[PATH_CAP];
    size_t len = strlen(filepath);
    if (len == 0 || len >= sizeof(tmp)) return;
    memcpy(tmp, filepath, len + 1);
    char *last = strrchr(tmp, '/');
    if (!last) return;
    *last = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

static StoreStatus write_conn(FILE *f, const Conn *c) {
    if (c->label[0] != '\0')
        if (fprintf(f, "label=%s\n", c->label) < 0) return STORE_ERR_IO;
    if (fprintf(f, "host=%s\n", c->host) < 0) return STORE_ERR_IO;
    if (fprintf(f, "port=%d\n", c->port) < 0) return STORE_ERR_IO;
    if (fprintf(f, "user=%s\n", c->user) < 0) return STORE_ERR_IO;
    const char *astr = (c->auth == SSH_AUTH_AGENT) ? "agent" : "password";
    if (fprintf(f, "auth=%s\n", astr) < 0) return STORE_ERR_IO;
    /* passkey only for password auth with remember enabled */
    if (c->auth == SSH_AUTH_PASSWORD && c->remember && c->passkey[0] != '\0') {
        if (fprintf(f, "remember=1\n") < 0) return STORE_ERR_IO;
        if (fprintf(f, "passkey=%s\n", c->passkey) < 0) return STORE_ERR_IO;
    }
    return STORE_OK;
}

StoreStatus store_save(const ConnList *list) {
    if (!list) return STORE_ERR_IO;

    char path[PATH_CAP];
    StoreStatus s = store_path(path, sizeof(path));
    if (s != STORE_OK) return s;

    mkdirs_for(path);

    char tmp_path[PATH_CAP + 4];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    /* Create with restricted permissions from the start. */
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return STORE_ERR_IO;
    if (fchmod(fd, 0600) != 0) { close(fd); unlink(tmp_path); return STORE_ERR_PERMS; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_path); return STORE_ERR_IO; }

    int mru = (list->count > 0
               && list->mru_index >= 0
               && list->mru_index < list->count)
              ? list->mru_index : -1;
    bool first = true;

    /* Write MRU item first so it loads back as the pre-selected entry. */
    if (mru >= 0) {
        s = write_conn(f, &list->items[mru]);
        if (s != STORE_OK) { fclose(f); unlink(tmp_path); return s; }
        first = false;
    }

    for (int i = 0; i < list->count; i++) {
        if (i == mru) continue;
        if (!first)
            if (fprintf(f, "\n") < 0) { fclose(f); unlink(tmp_path); return STORE_ERR_IO; }
        s = write_conn(f, &list->items[i]);
        if (s != STORE_OK) { fclose(f); unlink(tmp_path); return s; }
        first = false;
    }

    if (fclose(f) != 0) { unlink(tmp_path); return STORE_ERR_IO; }
    if (rename(tmp_path, path) != 0) { unlink(tmp_path); return STORE_ERR_IO; }
    return STORE_OK;
}

StoreStatus store_load(Arena *a, ConnList *out) {
    char path[PATH_CAP];
    StoreStatus s = store_path(path, sizeof(path));
    if (s != STORE_OK) return s;

    out->items     = NULL;
    out->count     = 0;
    out->mru_index = 0;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return STORE_OK;
        return STORE_ERR_IO;
    }

    Conn *items = arena_alloc(a, sizeof(Conn) * MAX_CONNS, _Alignof(Conn));
    if (!items) { fclose(f); return STORE_ERR_OOM; }

    int   count = 0;
    Conn  cur;
    bool  in_record = false;
    char  line[MAX_LINE];

    memset(&cur, 0, sizeof(cur));

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            if (in_record && cur.host[0] != '\0') {
                if (cur.auth == SSH_AUTH_AGENT) {
                    cur.remember = false;
                    memset(cur.passkey, 0, sizeof(cur.passkey));
                }
                if (count < MAX_CONNS) items[count++] = cur;
                memset(&cur, 0, sizeof(cur));
                in_record = false;
            }
            continue;
        }

        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) { fclose(f); return STORE_ERR_PARSE; }
        *eq       = '\0';
        const char *key = line;
        const char *val = eq + 1;
        in_record       = true;

        if (strcmp(key, "label") == 0) {
            strncpy(cur.label, val, sizeof(cur.label) - 1);
        } else if (strcmp(key, "host") == 0) {
            strncpy(cur.host, val, sizeof(cur.host) - 1);
        } else if (strcmp(key, "port") == 0) {
            cur.port = atoi(val);
        } else if (strcmp(key, "user") == 0) {
            strncpy(cur.user, val, sizeof(cur.user) - 1);
        } else if (strcmp(key, "auth") == 0) {
            if (strcmp(val, "agent") == 0)         cur.auth = SSH_AUTH_AGENT;
            else if (strcmp(val, "password") == 0) cur.auth = SSH_AUTH_PASSWORD;
            else { fclose(f); return STORE_ERR_PARSE; }
        } else if (strcmp(key, "remember") == 0) {
            cur.remember = (strcmp(val, "1") == 0);
        } else if (strcmp(key, "passkey") == 0) {
            strncpy(cur.passkey, val, sizeof(cur.passkey) - 1);
        }
        /* unknown keys are silently ignored for forward compatibility */
    }

    /* finalize the last record (no trailing blank line required) */
    if (in_record && cur.host[0] != '\0') {
        if (cur.auth == SSH_AUTH_AGENT) {
            cur.remember = false;
            memset(cur.passkey, 0, sizeof(cur.passkey));
        }
        if (count < MAX_CONNS) items[count++] = cur;
    }

    fclose(f);
    out->items     = items;
    out->count     = count;
    out->mru_index = 0;
    return STORE_OK;
}

const char *store_status_str(StoreStatus st) {
    switch (st) {
    case STORE_OK:        return "OK";
    case STORE_ERR_IO:    return "IO error";
    case STORE_ERR_PARSE: return "parse error";
    case STORE_ERR_PERMS: return "permission error";
    case STORE_ERR_OOM:   return "out of memory";
    default:              return "unknown error";
    }
}

/* ── recon persistence ────────────────────────────────────────────────── */

#define MAX_PRESETS      64
#define MAX_RECON_LINE   2048
#define MAX_RECORD_LINES 16

void store_conn_key(const Conn *c, char *buf, size_t cap) {
    snprintf(buf, cap, "%s@%s:%d", c->user, c->host, c->port);
}

/*
 * Copy every record from `in` to `out` except those where conn==skip_conn.
 * *first_out tracks whether any record has been written yet (for separator).
 */
static StoreStatus copy_records_except(FILE *in, FILE *out,
                                        const char *skip_conn,
                                        bool *first_out) {
    char  line[MAX_RECON_LINE];
    char  rec[MAX_RECORD_LINES][MAX_RECON_LINE];
    int   nlines    = 0;
    char  cur_conn[300] = "";
    bool  in_record = false;

    while (fgets(line, sizeof(line), in)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            if (in_record) {
                if (strcmp(cur_conn, skip_conn) != 0 && nlines > 0) {
                    if (!*first_out && fprintf(out, "\n") < 0)
                        return STORE_ERR_IO;
                    for (int i = 0; i < nlines; i++)
                        if (fprintf(out, "%s\n", rec[i]) < 0)
                            return STORE_ERR_IO;
                    *first_out = false;
                }
                nlines      = 0;
                cur_conn[0] = '\0';
                in_record   = false;
            }
            continue;
        }

        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (eq) {
            char  key[64] = "";
            size_t klen   = (size_t)(eq - line);
            if (klen < sizeof(key)) {
                memcpy(key, line, klen);
                key[klen] = '\0';
            }
            if (strcmp(key, "conn") == 0)
                strncpy(cur_conn, eq + 1, sizeof(cur_conn) - 1);
        }

        if (nlines < MAX_RECORD_LINES)
            strncpy(rec[nlines++], line, MAX_RECON_LINE - 1);
        in_record = true;
    }

    /* flush last record (no trailing blank line required) */
    if (in_record && strcmp(cur_conn, skip_conn) != 0 && nlines > 0) {
        if (!*first_out && fprintf(out, "\n") < 0)
            return STORE_ERR_IO;
        for (int i = 0; i < nlines; i++)
            if (fprintf(out, "%s\n", rec[i]) < 0)
                return STORE_ERR_IO;
        *first_out = false;
    }

    return STORE_OK;
}

/* Open a temp file for atomic writing; caller must fclose + rename or unlink. */
static StoreStatus open_atomic(const char *path, char *tmp_out, size_t tmp_cap,
                                FILE **f_out) {
    mkdirs_for(path);
    int n = snprintf(tmp_out, tmp_cap, "%s.tmp", path);
    if (n < 0 || (size_t)n >= tmp_cap) return STORE_ERR_IO;
    int fd = open(tmp_out, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return STORE_ERR_IO;
    if (fchmod(fd, 0600) != 0) { close(fd); unlink(tmp_out); return STORE_ERR_PERMS; }
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(tmp_out); return STORE_ERR_IO; }
    *f_out = f;
    return STORE_OK;
}

/* ── presets ──────────────────────────────────────────────────────────── */

StoreStatus preset_load(Arena *a, const char *conn_key, PresetList *out) {
    out->items        = NULL;
    out->count        = 0;
    out->active_index = -1;

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "presets");
    if (s != STORE_OK) return s;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return STORE_OK;
        return STORE_ERR_IO;
    }

    Preset *items = arena_alloc(a, sizeof(Preset) * MAX_PRESETS, _Alignof(Preset));
    if (!items) { fclose(f); return STORE_ERR_OOM; }

    int   count    = 0;
    Preset cur;
    bool  in_record   = false;
    bool  cur_active  = false;
    bool  cur_matches = false;
    char  line[MAX_RECON_LINE];

    memset(&cur, 0, sizeof(cur));

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            if (in_record && cur_matches && cur.name[0] != '\0') {
                if (count < MAX_PRESETS) {
                    items[count] = cur;
                    if (cur_active) out->active_index = count;
                    count++;
                }
            }
            memset(&cur, 0, sizeof(cur));
            cur_active  = false;
            cur_matches = false;
            in_record   = false;
            continue;
        }

        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) { fclose(f); return STORE_ERR_PARSE; }
        *eq             = '\0';
        const char *key = line;
        const char *val = eq + 1;
        in_record       = true;

        if (strcmp(key, "conn") == 0) {
            cur_matches = (strcmp(val, conn_key) == 0);
        } else if (cur_matches) {
            if      (strcmp(key, "name")     == 0) strncpy(cur.name,      val, sizeof(cur.name)      - 1);
            else if (strcmp(key, "project")  == 0) strncpy(cur.project,   val, sizeof(cur.project)   - 1);
            else if (strcmp(key, "scheme")   == 0) strncpy(cur.scheme,    val, sizeof(cur.scheme)    - 1);
            else if (strcmp(key, "config")   == 0) strncpy(cur.config,    val, sizeof(cur.config)    - 1);
            else if (strcmp(key, "bundleid") == 0) strncpy(cur.bundle_id, val, sizeof(cur.bundle_id) - 1);
            else if (strcmp(key, "active")   == 0) cur_active = (strcmp(val, "1") == 0);
            /* unknown keys silently ignored */
        }
    }

    if (in_record && cur_matches && cur.name[0] != '\0') {
        if (count < MAX_PRESETS) {
            items[count] = cur;
            if (cur_active) out->active_index = count;
            count++;
        }
    }

    fclose(f);
    out->items = items;
    out->count = count;
    return STORE_OK;
}

static StoreStatus write_preset_rec(FILE *f, const char *conn_key,
                                     const Preset *p, bool active) {
    if (fprintf(f, "conn=%s\n",    conn_key)  < 0) return STORE_ERR_IO;
    if (fprintf(f, "name=%s\n",    p->name)   < 0) return STORE_ERR_IO;
    if (active)
        if (fprintf(f, "active=1\n") < 0) return STORE_ERR_IO;
    if (p->project[0]   && fprintf(f, "project=%s\n",  p->project)   < 0) return STORE_ERR_IO;
    if (p->scheme[0]    && fprintf(f, "scheme=%s\n",   p->scheme)    < 0) return STORE_ERR_IO;
    if (p->config[0]    && fprintf(f, "config=%s\n",   p->config)    < 0) return STORE_ERR_IO;
    if (p->bundle_id[0] && fprintf(f, "bundleid=%s\n", p->bundle_id) < 0) return STORE_ERR_IO;
    return STORE_OK;
}

StoreStatus preset_save(const char *conn_key, const PresetList *l) {
    if (!conn_key || !l) return STORE_ERR_IO;

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "presets");
    if (s != STORE_OK) return s;

    char tmp[PATH_CAP + 4];
    FILE *out;
    s = open_atomic(path, tmp, sizeof(tmp), &out);
    if (s != STORE_OK) return s;

    bool first_out = true;

    FILE *in = fopen(path, "r");
    if (in) {
        s = copy_records_except(in, out, conn_key, &first_out);
        fclose(in);
        if (s != STORE_OK) { fclose(out); unlink(tmp); return s; }
    }

    for (int i = 0; i < l->count; i++) {
        if (!first_out && fprintf(out, "\n") < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        s = write_preset_rec(out, conn_key, &l->items[i], i == l->active_index);
        if (s != STORE_OK) { fclose(out); unlink(tmp); return s; }
        first_out = false;
    }

    if (fclose(out) != 0) { unlink(tmp); return STORE_ERR_IO; }
    if (rename(tmp, path) != 0) { unlink(tmp); return STORE_ERR_IO; }
    return STORE_OK;
}

/* ── remembered target ────────────────────────────────────────────────── */

StoreStatus target_load(const char *conn_key, RememberedTarget *out) {
    if (!conn_key || !out) return STORE_ERR_IO;
    memset(out, 0, sizeof(*out));

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "targets");
    if (s != STORE_OK) return s;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return STORE_OK;
        return STORE_ERR_IO;
    }

    RememberedTarget cur;
    bool  in_record   = false;
    bool  cur_matches = false;
    bool  found       = false;
    char  line[MAX_RECON_LINE];

    memset(&cur, 0, sizeof(cur));

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            if (in_record && cur_matches && cur.udid[0] != '\0') {
                *out  = cur;
                found = true;
            }
            memset(&cur, 0, sizeof(cur));
            cur_matches = false;
            in_record   = false;
            if (found) break;
            continue;
        }

        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) { fclose(f); return STORE_ERR_PARSE; }
        *eq             = '\0';
        const char *key = line;
        const char *val = eq + 1;
        in_record       = true;

        if (strcmp(key, "conn") == 0) {
            cur_matches = (strcmp(val, conn_key) == 0);
        } else if (cur_matches) {
            if      (strcmp(key, "udid") == 0) strncpy(cur.udid, val, sizeof(cur.udid) - 1);
            else if (strcmp(key, "name") == 0) strncpy(cur.name, val, sizeof(cur.name) - 1);
        }
    }

    if (in_record && cur_matches && cur.udid[0] != '\0' && !found)
        *out = cur;

    fclose(f);
    return STORE_OK;
}

StoreStatus target_save(const char *conn_key, const RememberedTarget *t) {
    if (!conn_key || !t) return STORE_ERR_IO;

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "targets");
    if (s != STORE_OK) return s;

    char tmp[PATH_CAP + 4];
    FILE *out;
    s = open_atomic(path, tmp, sizeof(tmp), &out);
    if (s != STORE_OK) return s;

    bool first_out = true;

    FILE *in = fopen(path, "r");
    if (in) {
        s = copy_records_except(in, out, conn_key, &first_out);
        fclose(in);
        if (s != STORE_OK) { fclose(out); unlink(tmp); return s; }
    }

    if (t->udid[0] != '\0') {
        if (!first_out && fprintf(out, "\n") < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        if (fprintf(out, "conn=%s\n", conn_key) < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        if (fprintf(out, "udid=%s\n", t->udid)  < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        if (t->name[0])
            if (fprintf(out, "name=%s\n", t->name) < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
    }

    if (fclose(out) != 0) { unlink(tmp); return STORE_ERR_IO; }
    if (rename(tmp, path) != 0) { unlink(tmp); return STORE_ERR_IO; }
    return STORE_OK;
}

/* ── scan root ────────────────────────────────────────────────────────── */

StoreStatus scanroot_load(const char *conn_key, char *root, size_t cap) {
    if (!conn_key || !root || cap == 0) return STORE_ERR_IO;
    root[0] = '\0';

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "scanroots");
    if (s != STORE_OK) return s;

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return STORE_OK;
        return STORE_ERR_IO;
    }

    char  found_root[1024] = "";
    bool  in_record        = false;
    bool  cur_matches      = false;
    bool  found            = false;
    char  line[MAX_RECON_LINE];

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0) {
            if (in_record && cur_matches && found_root[0] != '\0') {
                snprintf(root, cap, "%s", found_root);
                found = true;
            }
            found_root[0] = '\0';
            cur_matches   = false;
            in_record     = false;
            if (found) break;
            continue;
        }

        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) { fclose(f); return STORE_ERR_PARSE; }
        *eq             = '\0';
        const char *key = line;
        const char *val = eq + 1;
        in_record       = true;

        if (strcmp(key, "conn") == 0) {
            cur_matches = (strcmp(val, conn_key) == 0);
        } else if (cur_matches) {
            if (strcmp(key, "root") == 0)
                strncpy(found_root, val, sizeof(found_root) - 1);
        }
    }

    if (in_record && cur_matches && found_root[0] != '\0' && !found)
        snprintf(root, cap, "%s", found_root);

    fclose(f);
    return STORE_OK;
}

StoreStatus scanroot_save(const char *conn_key, const char *root) {
    if (!conn_key || !root) return STORE_ERR_IO;

    char path[PATH_CAP];
    StoreStatus s = ostrich_path(path, sizeof(path), "scanroots");
    if (s != STORE_OK) return s;

    char tmp[PATH_CAP + 4];
    FILE *out;
    s = open_atomic(path, tmp, sizeof(tmp), &out);
    if (s != STORE_OK) return s;

    bool first_out = true;

    FILE *in = fopen(path, "r");
    if (in) {
        s = copy_records_except(in, out, conn_key, &first_out);
        fclose(in);
        if (s != STORE_OK) { fclose(out); unlink(tmp); return s; }
    }

    if (root[0] != '\0') {
        if (!first_out && fprintf(out, "\n") < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        if (fprintf(out, "conn=%s\n", conn_key) < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
        if (fprintf(out, "root=%s\n", root)     < 0) { fclose(out); unlink(tmp); return STORE_ERR_IO; }
    }

    if (fclose(out) != 0) { unlink(tmp); return STORE_ERR_IO; }
    if (rename(tmp, path) != 0) { unlink(tmp); return STORE_ERR_IO; }
    return STORE_OK;
}
