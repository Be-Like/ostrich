#include "app.h"
#include "arena.h"
#include "ui.h"
#include "session.h"
#include "connstate.h"
#include "lexicon.h"
#include "store.h"
#include "discovery.h"

#include <stdio.h>
#include <string.h>

#define APP_MAX_BLUEPRINTS 256

struct App {
    Ui       *ui;
    Session  *session;
    Arena    *arena;            /* app arena, for known_hosts growth     */
    Arena    *blueprints_arena; /* reset at each SCAN HOST               */
    ConnForm  form;
    ConnList  known_hosts; /* loaded from store; items in app arena  */

    /* persistent view state, updated from session events each frame */
    ConnPhase phase;
    SshStatus last_reason;
    char      user_host[384];
    char      fingerprint[128];
    bool      hostkey_unknown;
    bool      hostkey_mismatch;

    /* UPDATE / close / switch */
    SshConfig live_cfg;     /* identity of the currently-live connection */
    bool      overlay_open; /* UPDATE: show overlay while bar-phase      */

    /* recon state */
    RunConfig     run_cfg;            /* mutable run config + scan root      */
    Blueprint    *bp_items;           /* lives in blueprints_arena           */
    BlueprintList blueprints;         /* .items = bp_items                   */
    bool          scanning;
    bool          scan_done;
    DiscStatus    scan_err;
    int           blueprint_selected; /* -1 = none                           */
    char          conn_key[256];      /* user@host:port for store calls      */
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

    Arena *bp_arena = arena_create(1024 * 1024); /* 1 MB for streamed blueprints */
    if (!bp_arena) {
        session_close(session);
        ui_shutdown(ui);
        return APP_ERR;
    }

    App *app = arena_alloc(a, sizeof(App), _Alignof(App));
    if (!app) {
        arena_destroy(bp_arena);
        session_close(session);
        ui_shutdown(ui);
        return APP_ERR;
    }
    memset(app, 0, sizeof(*app));
    app->ui                       = ui;
    app->session                  = session;
    app->arena                    = a;
    app->blueprints_arena         = bp_arena;
    app->form.selected_known_host = -1;
    app->phase                    = CONN_DISCONNECTED;
    app->blueprint_selected       = -1;
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
    /* Track phase before draining events to detect ONLINE transition. */
    ConnPhase prev_phase = app->phase;

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
        if (ev.phase == CONN_DISCONNECTED || ev.phase == CONN_SEVERED) {
            app->user_host[0] = '\0';
            app->overlay_open = false;
            app->scanning     = false;
            app->conn_key[0]  = '\0';
        }
    }

    /* CONN_ONLINE transition: build conn_key and restore scan root. */
    if (app->phase == CONN_ONLINE && prev_phase != CONN_ONLINE) {
        Conn tmp = {0};
        snprintf(tmp.host, sizeof(tmp.host), "%s", app->live_cfg.host);
        tmp.port = app->live_cfg.port;
        snprintf(tmp.user, sizeof(tmp.user), "%s", app->live_cfg.user);
        store_conn_key(&tmp, app->conn_key, sizeof(app->conn_key));
        scanroot_load(app->conn_key, app->run_cfg.scan_root,
                      sizeof(app->run_cfg.scan_root));
    }

    /* Drain discovery events. */
    SessionDiscEvent dev;
    while (session_disc_poll(app->session, &dev)) {
        switch (dev.kind) {
        case DEV_BLUEPRINT:
            if (app->bp_items && app->blueprints.count < APP_MAX_BLUEPRINTS) {
                app->bp_items[app->blueprints.count] = dev.blueprint;
                app->blueprints.count++;
            }
            break;
        case DEV_SCAN_COMPLETE:
            app->scanning  = false;
            app->scan_done = true;
            app->scan_err  = DISC_OK;
            break;
        case DEV_SCAN_FAILED:
            app->scanning  = false;
            app->scan_done = true;
            app->scan_err  = dev.disc_status;
            break;
        default:
            break;
        }
    }

    UiConnView view = {0};
    view.phase               = app->phase;
    view.user_host           = app->user_host;
    view.fingerprint         = app->fingerprint;
    view.show_hostkey_prompt = app->hostkey_unknown;
    view.show_mismatch       = app->hostkey_mismatch;
    view.overlay_open        = app->overlay_open;
    view.known_hosts         = app->known_hosts.items;
    view.known_count         = app->known_hosts.count;
    view.reason = app_phase_reason(app->phase, app->last_reason);

    UiReconView rv = {0};
    rv.scanning            = app->scanning;
    rv.scan_done           = app->scan_done;
    rv.scan_err            = app->scan_err;
    rv.blueprints          = &app->blueprints;
    rv.blueprint_selected  = app->blueprint_selected;
    rv.preset_selected     = -1;
    rv.target_selected     = -1;
    rv.readiness           = disc_readiness(&app->run_cfg, false);

    UiIntents     intents = {0};
    UiReconIntents ri     = {0};
    intents.select_host  = -1;
    ri.pick_blueprint    = -1;
    ri.pick_preset       = -1;
    ri.pick_target       = -1;
    bool keep_going = ui_frame(app->ui, &view, &app->form, &intents,
                               &rv, &app->run_cfg, &ri);

    bool connected = (app->phase == CONN_ONLINE ||
                      app->phase == CONN_REACQUIRING);

    if (intents.select_host >= 0 &&
        intents.select_host < app->known_hosts.count) {
        /* Close the live session first when switching to a different host. */
        if (connected && intents.select_host != app->form.selected_known_host) {
            SessionCmd cmd = {.kind = CMD_CLOSE};
            session_submit(app->session, &cmd);
            app->overlay_open = false;
        }
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
    if (intents.update) {
        /* Re-open the overlay pre-filled with the live connection's details. */
        app->overlay_open = true;
    }
    if (intents.close) {
        /* Disconnect and return to the resting overlay. */
        SessionCmd cmd = {.kind = CMD_CLOSE};
        session_submit(app->session, &cmd);
        app->overlay_open     = false;
        app->last_reason      = SSH_OK;
        app->hostkey_unknown  = false;
        app->hostkey_mismatch = false;
    }
    if (intents.breach) {
        SshConfig cfg;
        app_form_to_ssh_config(&app->form, &cfg);
        if (connstate_validate(&cfg)) {
            bool do_breach = true;
            if (connected && app->overlay_open) {
                /* UPDATE mode: compare new identity against live connection. */
                SshConfig new_id;
                app_form_to_ssh_config(&app->form, &new_id);
                bool id_changed =
                    strcmp(app->live_cfg.host, new_id.host) != 0 ||
                    app->live_cfg.port != new_id.port              ||
                    strcmp(app->live_cfg.user, new_id.user) != 0   ||
                    app->live_cfg.auth != new_id.auth;
                if (id_changed) {
                    /* Identity changed: close current session then reconnect. */
                    SessionCmd close_cmd = {.kind = CMD_CLOSE};
                    session_submit(app->session, &close_cmd);
                } else {
                    /* Only metadata changed: persist with no reconnect. */
                    int new_idx = app_save_to_list(&app->known_hosts, &app->form,
                                                   app->form.selected_known_host,
                                                   app->arena);
                    if (new_idx >= 0) {
                        app->form.selected_known_host = new_idx;
                        store_save(&app->known_hosts);
                    }
                    app->overlay_open = false;
                    do_breach = false;
                }
            }
            if (do_breach) {
                SessionCmd cmd = {.kind = CMD_BREACH, .cfg = cfg};
                session_submit(app->session, &cmd);
                app->live_cfg         = cfg;
                app->last_reason      = SSH_OK;
                app->hostkey_unknown  = false;
                app->hostkey_mismatch = false;
                app->overlay_open     = false;
            }
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

    /* ── recon intents ───────────────────────────────────────────── */
    bool online = (app->phase == CONN_ONLINE);

    if (ri.scan && online && !app->scanning) {
        arena_reset(app->blueprints_arena);
        app->bp_items = arena_alloc(app->blueprints_arena,
                                    sizeof(Blueprint) * APP_MAX_BLUEPRINTS,
                                    _Alignof(Blueprint));
        app->blueprints.items = app->bp_items;
        app->blueprints.count = 0;
        app->scanning         = true;
        app->scan_done        = false;
        app->scan_err         = DISC_OK;
        app->blueprint_selected = -1;
        if (app->conn_key[0] != '\0')
            scanroot_save(app->conn_key, app->run_cfg.scan_root);
        SessionDiscCmd dcmd = {0};
        dcmd.kind = DCMD_SCAN_HOST;
        snprintf(dcmd.root, sizeof(dcmd.root), "%s", app->run_cfg.scan_root);
        session_disc_submit(app->session, &dcmd);
    }
    if (ri.abort_scan && app->scanning) {
        SessionDiscCmd dcmd = {.kind = DCMD_ABORT_SCAN};
        session_disc_submit(app->session, &dcmd);
    }
    if (ri.pick_blueprint >= 0 &&
        ri.pick_blueprint < app->blueprints.count) {
        app->blueprint_selected = ri.pick_blueprint;
        snprintf(app->run_cfg.project, sizeof(app->run_cfg.project),
                 "%s", app->blueprints.items[ri.pick_blueprint].path);
    }

    return keep_going;
}

void app_shutdown(App *app) {
    session_close(app->session);
    ui_shutdown(app->ui);
    arena_destroy(app->blueprints_arena);
}
