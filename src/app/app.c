#include "app.h"
#include "arena.h"
#include "ui.h"
#include "session.h"
#include "connstate.h"
#include "lexicon.h"
#include "store.h"

#include <stdio.h>
#include <string.h>

struct App {
    Ui       *ui;
    Session  *session;
    Arena    *arena; /* app arena, for growing known_hosts on save */
    ConnForm  form;
    ConnList  known_hosts; /* loaded from store; items in app arena  */

    /* persistent view state, updated from session events each frame */
    ConnPhase phase;
    SshStatus last_reason;
    char      user_host[384];
    char      fingerprint[128];
    bool      hostkey_unknown;
    bool      hostkey_mismatch;
};

AppStatus app_init(Arena *a, AppOptions opts, App **out) {
    *out = NULL;

    UiOptions ui_opts = {
        .title    = opts.title,
        .width    = opts.width,
        .height   = opts.height,
        .font_dir = opts.font_dir,
        .headless = opts.headless,
    };

    Ui      *ui = NULL;
    UiStatus st = ui_init(a, ui_opts, &ui);
    if (st != UI_OK) {
        fprintf(stderr, "ostrich: %s\n", ui_status_str(st));
        return APP_ERR;
    }

    Session  *session = NULL;
    SshStatus ss      = session_open(&session);
    if (ss != SSH_OK) {
        fprintf(stderr, "ostrich: failed to open session\n");
        ui_shutdown(ui);
        return APP_ERR;
    }

    App *app = arena_alloc(a, sizeof(App), _Alignof(App));
    if (!app) {
        session_close(session);
        ui_shutdown(ui);
        return APP_ERR;
    }
    memset(app, 0, sizeof(*app));
    app->ui                       = ui;
    app->session                  = session;
    app->arena                    = a;
    app->form.selected_known_host = -1;
    app->phase                    = CONN_DISCONNECTED;
    snprintf(app->form.port, sizeof(app->form.port), "22");

    /* Load saved connections; non-fatal if the store is missing or empty. */
    store_load(a, &app->known_hosts);
    if (app->known_hosts.count > 0) {
        int mru = app->known_hosts.mru_index;
        app_conn_to_form(&app->known_hosts.items[mru], &app->form);
        app->form.selected_known_host = mru;
    }

    *out = app;
    return APP_OK;
}

bool app_tick(App *app) {
    /* Drain all pending session events and update view state. */
    SessionEvent ev;
    while (session_poll(app->session, &ev)) {
        app->phase            = ev.phase;
        app->last_reason      = ev.reason;
        app->hostkey_unknown  = ev.hostkey_unknown;
        app->hostkey_mismatch = ev.hostkey_mismatch;
        if (ev.user_host[0])
            memcpy(app->user_host, ev.user_host, sizeof(app->user_host));
        if (ev.fingerprint[0])
            memcpy(app->fingerprint, ev.fingerprint, sizeof(app->fingerprint));
        if (ev.phase == CONN_DISCONNECTED)
            app->user_host[0] = '\0';
    }

    UiConnView view = {0};
    view.phase               = app->phase;
    view.user_host           = app->user_host;
    view.fingerprint         = app->fingerprint;
    view.show_hostkey_prompt = app->hostkey_unknown;
    view.show_mismatch       = app->hostkey_mismatch;
    view.known_hosts         = app->known_hosts.items;
    view.known_count         = app->known_hosts.count;
    if (app->phase == CONN_DISCONNECTED && app->last_reason != SSH_OK)
        view.reason = lex(connstate_reason_lex(app->last_reason));

    UiIntents intents = {0};
    intents.select_host = -1;
    bool keep_going = ui_frame(app->ui, &view, &app->form, &intents);

    if (intents.select_host >= 0 &&
        intents.select_host < app->known_hosts.count) {
        app_conn_to_form(&app->known_hosts.items[intents.select_host],
                         &app->form);
        app->form.selected_known_host = intents.select_host;
    }
    if (intents.save) {
        int new_idx = app_save_to_list(&app->known_hosts, &app->form,
                                       app->form.selected_known_host,
                                       app->arena);
        if (new_idx >= 0) {
            app->form.selected_known_host = new_idx;
            store_save(&app->known_hosts);
        }
    }
    if (intents.breach) {
        SshConfig cfg;
        app_form_to_ssh_config(&app->form, &cfg);
        if (connstate_validate(&cfg)) {
            SessionCmd cmd = {.kind = CMD_BREACH, .cfg = cfg};
            session_submit(app->session, &cmd);
            app->last_reason      = SSH_OK;
            app->hostkey_unknown  = false;
            app->hostkey_mismatch = false;
        }
    }
    if (intents.abort) {
        SessionCmd cmd = {.kind = CMD_ABORT};
        session_submit(app->session, &cmd);
    }
    if (intents.trust) {
        SessionCmd cmd = {.kind = CMD_TRUST};
        session_submit(app->session, &cmd);
    }
    if (intents.decline) {
        SessionCmd cmd = {.kind = CMD_DECLINE};
        session_submit(app->session, &cmd);
    }

    return keep_going;
}

void app_shutdown(App *app) {
    session_close(app->session);
    ui_shutdown(app->ui);
}
