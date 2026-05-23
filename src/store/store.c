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

StoreStatus store_path(char *buf, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int n;
    if (xdg && xdg[0] != '\0') {
        n = snprintf(buf, cap, "%s/ostrich/connections", xdg);
    } else {
        const char *home = getenv("HOME");
        if (!home || home[0] == '\0') return STORE_ERR_IO;
        n = snprintf(buf, cap, "%s/.config/ostrich/connections", home);
    }
    if (n < 0 || (size_t)n >= cap) return STORE_ERR_IO;
    return STORE_OK;
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
