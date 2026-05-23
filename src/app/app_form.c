#include "app.h"
#include "ui.h"
#include "ssh.h"
#include "store.h"
#include "arena.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void app_form_to_ssh_config(const ConnForm *form, SshConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->host, sizeof(cfg->host), "%s", form->host);
    int port = (form->port[0] != '\0') ? atoi(form->port) : 22;
    cfg->port = (port > 0 && port <= 65535) ? port : 22;
    snprintf(cfg->user, sizeof(cfg->user), "%s", form->user);
    cfg->auth = form->auth;
    snprintf(cfg->passkey, sizeof(cfg->passkey), "%s", form->passkey);
}

void app_conn_to_form(const Conn *conn, ConnForm *form) {
    snprintf(form->host,    sizeof(form->host),    "%s", conn->host);
    int port = (conn->port > 0 && conn->port <= 65535) ? conn->port : 22;
    snprintf(form->port,    sizeof(form->port),    "%d", port);
    snprintf(form->user,    sizeof(form->user),    "%s", conn->user);
    snprintf(form->passkey, sizeof(form->passkey), "%s", conn->passkey);
    form->auth     = conn->auth;
    form->remember = conn->remember;
}

int app_save_to_list(ConnList *list, const ConnForm *form,
                     int selected_idx, Arena *a) {
    Conn c;
    memset(&c, 0, sizeof(c));
    snprintf(c.host, sizeof(c.host), "%s", form->host);
    int port = (form->port[0] != '\0') ? atoi(form->port) : 22;
    c.port = (port > 0 && port <= 65535) ? port : 22;
    snprintf(c.user, sizeof(c.user), "%s", form->user);
    c.auth     = form->auth;
    c.remember = form->remember;
    if (c.auth == SSH_AUTH_PASSWORD && c.remember)
        snprintf(c.passkey, sizeof(c.passkey), "%s", form->passkey);

    if (selected_idx >= 0 && selected_idx < list->count) {
        snprintf(c.label, sizeof(c.label), "%s", list->items[selected_idx].label);
        list->items[selected_idx] = c;
        list->mru_index = selected_idx;
        return selected_idx;
    }

    if (!a) return -1;
    int new_idx = list->count;
    Conn *new_items = arena_alloc(a, sizeof(Conn) * (size_t)(new_idx + 1),
                                  _Alignof(Conn));
    if (!new_items) return -1;
    if (list->items && list->count > 0)
        memcpy(new_items, list->items, sizeof(Conn) * (size_t)list->count);
    snprintf(c.label, sizeof(c.label), "%.31s@%.31s", form->user, form->host);
    new_items[new_idx]  = c;
    list->items         = new_items;
    list->count         = new_idx + 1;
    list->mru_index     = new_idx;
    return new_idx;
}
