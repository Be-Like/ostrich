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
#define APP_MAX_SCHEMES    64
#define APP_MAX_CONFIGS    64
#define APP_MAX_PRESETS    64

struct App {
    Ui       *ui;
    Session  *session;
    Arena    *arena;            /* app arena, for known_hosts growth     */
    Arena    *blueprints_arena; /* reset at each SCAN HOST               */
    Arena    *bp_read_arena;    /* reset at each READ_BLUEPRINT          */
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

    /* slice B — scheme / config / bundle-id */
    bool          reading_blueprint;
    bool          resolving_bundle_id;
    DiscStatus    blueprint_err;
    bool          scheme_user_edited;
    bool          config_user_edited;
    bool          bundle_id_user_edited;
    char        (*scheme_items)[256]; /* lives in bp_read_arena              */
    char        (*config_items)[256]; /* lives in bp_read_arena              */
    StrList       schemes;
    StrList       configs;

    /* slice C — presets */
    Arena        *presets_arena;  /* 128 KB; reset at each CONN_ONLINE       */
    PresetList    presets;
    int           preset_selected; /* -1 = none                              */
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

    Arena *bp_read_arena = arena_create(64 * 1024); /* 64 KB for schemes/configs */
    if (!bp_read_arena) {
        arena_destroy(bp_arena);
        session_close(session);
        ui_shutdown(ui);
        return APP_ERR;
    }

    Arena *presets_arena = arena_create(128 * 1024); /* 128 KB for presets */
    if (!presets_arena) {
        arena_destroy(bp_read_arena);
        arena_destroy(bp_arena);
        session_close(session);
        ui_shutdown(ui);
        return APP_ERR;
    }

    App *app = arena_alloc(a, sizeof(App), _Alignof(App));
    if (!app) {
        arena_destroy(presets_arena);
        arena_destroy(bp_read_arena);
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
    app->bp_read_arena            = bp_read_arena;
    app->presets_arena            = presets_arena;
    app->form.selected_known_host = -1;
    app->phase                    = CONN_DISCONNECTED;
    app->blueprint_selected       = -1;
    app->preset_selected          = -1;
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
            app->user_host[0]    = '\0';
            app->overlay_open    = false;
            app->scanning        = false;
            app->conn_key[0]     = '\0';
            app->presets         = (PresetList){0};
            app->preset_selected = -1;
        }
    }

    /* CONN_ONLINE transition: build conn_key and restore scan root + presets. */
    if (app->phase == CONN_ONLINE && prev_phase != CONN_ONLINE) {
        Conn tmp = {0};
        snprintf(tmp.host, sizeof(tmp.host), "%s", app->live_cfg.host);
        tmp.port = app->live_cfg.port;
        snprintf(tmp.user, sizeof(tmp.user), "%s", app->live_cfg.user);
        store_conn_key(&tmp, app->conn_key, sizeof(app->conn_key));
        scanroot_load(app->conn_key, app->run_cfg.scan_root,
                      sizeof(app->run_cfg.scan_root));

        /* Load presets for this connection. */
        arena_reset(app->presets_arena);
        app->presets      = (PresetList){0};
        preset_load(app->presets_arena, app->conn_key, &app->presets);
        /* Ensure items array is allocated even when the store file is missing. */
        if (!app->presets.items)
            app->presets.items = arena_alloc(app->presets_arena,
                                             sizeof(Preset) * APP_MAX_PRESETS,
                                             _Alignof(Preset));
        app->preset_selected = app->presets.active_index;
        /* Apply the last-active preset to the working run config. */
        if (app->preset_selected >= 0 &&
            app->preset_selected < app->presets.count) {
            const Preset *p = &app->presets.items[app->preset_selected];
            snprintf(app->run_cfg.project,   sizeof(app->run_cfg.project),
                     "%s", p->project);
            snprintf(app->run_cfg.scheme,    sizeof(app->run_cfg.scheme),
                     "%s", p->scheme);
            snprintf(app->run_cfg.config,    sizeof(app->run_cfg.config),
                     "%s", p->config);
            snprintf(app->run_cfg.bundle_id, sizeof(app->run_cfg.bundle_id),
                     "%s", p->bundle_id);
        }
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
        case DEV_SCHEME:
            if (app->scheme_items && app->schemes.count < APP_MAX_SCHEMES) {
                snprintf(app->scheme_items[app->schemes.count], 256,
                         "%s", dev.scheme);
                if (!app->scheme_user_edited && app->run_cfg.scheme[0] == '\0')
                    snprintf(app->run_cfg.scheme, sizeof(app->run_cfg.scheme),
                             "%s", dev.scheme);
                app->schemes.count++;
            }
            break;
        case DEV_CONFIG:
            if (app->config_items && app->configs.count < APP_MAX_CONFIGS) {
                snprintf(app->config_items[app->configs.count], 256,
                         "%s", dev.config);
                app->configs.count++;
            }
            break;
        case DEV_BLUEPRINT_READ_COMPLETE:
            app->reading_blueprint = false;
            app->blueprint_err     = DISC_OK;
            if (!app->config_user_edited && app->run_cfg.config[0] == '\0') {
                const char *chosen = NULL;
                for (int i = 0; i < app->configs.count; i++) {
                    if (strcmp(app->config_items[i], "Debug") == 0) {
                        chosen = app->config_items[i];
                        break;
                    }
                }
                if (!chosen && app->configs.count > 0)
                    chosen = app->config_items[0];
                if (chosen)
                    snprintf(app->run_cfg.config, sizeof(app->run_cfg.config),
                             "%s", chosen);
            }
            if (!app->bundle_id_user_edited &&
                app->run_cfg.scheme[0]  != '\0' &&
                app->run_cfg.config[0]  != '\0' &&
                app->run_cfg.project[0] != '\0' &&
                app->phase == CONN_ONLINE) {
                SessionDiscCmd dcmd = {0};
                dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
                snprintf(dcmd.project, sizeof(dcmd.project), "%s",
                         app->run_cfg.project);
                snprintf(dcmd.scheme, sizeof(dcmd.scheme), "%s",
                         app->run_cfg.scheme);
                snprintf(dcmd.config, sizeof(dcmd.config), "%s",
                         app->run_cfg.config);
                session_disc_submit(app->session, &dcmd);
                app->resolving_bundle_id = true;
            }
            break;
        case DEV_BLUEPRINT_FAILED:
            app->reading_blueprint = false;
            app->blueprint_err     = dev.disc_status;
            break;
        case DEV_BUNDLE_ID:
            if (!app->bundle_id_user_edited)
                snprintf(app->run_cfg.bundle_id, sizeof(app->run_cfg.bundle_id),
                         "%s", dev.bundle_id);
            app->resolving_bundle_id = false;
            break;
        case DEV_BUNDLE_ID_FAILED:
            app->resolving_bundle_id = false;
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
    rv.scanning              = app->scanning;
    rv.scan_done             = app->scan_done;
    rv.scan_err              = app->scan_err;
    rv.blueprints            = &app->blueprints;
    rv.blueprint_selected    = app->blueprint_selected;
    rv.reading_blueprint     = app->reading_blueprint;
    rv.resolving_bundle_id   = app->resolving_bundle_id;
    rv.blueprint_err         = app->blueprint_err;
    rv.schemes               = &app->schemes;
    rv.configs               = &app->configs;
    rv.presets               = &app->presets;
    rv.preset_selected       = app->preset_selected;
    rv.target_selected       = -1;
    rv.readiness             = disc_readiness(&app->run_cfg, false);

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
        app->blueprint_selected    = ri.pick_blueprint;
        snprintf(app->run_cfg.project, sizeof(app->run_cfg.project),
                 "%s", app->blueprints.items[ri.pick_blueprint].path);
        app->run_cfg.scheme[0]     = '\0';
        app->run_cfg.config[0]     = '\0';
        app->run_cfg.bundle_id[0]  = '\0';
        app->scheme_user_edited    = false;
        app->config_user_edited    = false;
        app->bundle_id_user_edited = false;
        app->blueprint_err         = DISC_OK;
        app->resolving_bundle_id   = false;
        if (online) {
            arena_reset(app->bp_read_arena);
            app->scheme_items = arena_alloc(app->bp_read_arena,
                                            sizeof(*app->scheme_items) * APP_MAX_SCHEMES,
                                            _Alignof(char));
            app->config_items = arena_alloc(app->bp_read_arena,
                                            sizeof(*app->config_items) * APP_MAX_CONFIGS,
                                            _Alignof(char));
            app->schemes.items = app->scheme_items;
            app->schemes.count = 0;
            app->configs.items = app->config_items;
            app->configs.count = 0;
            app->reading_blueprint = true;
            SessionDiscCmd dcmd = {0};
            dcmd.kind = DCMD_READ_BLUEPRINT;
            snprintf(dcmd.project, sizeof(dcmd.project), "%s",
                     app->run_cfg.project);
            session_disc_submit(app->session, &dcmd);
        }
    }
    if (ri.scheme_edited) {
        app->scheme_user_edited = true;
        if (online && !app->bundle_id_user_edited &&
            app->run_cfg.scheme[0]  != '\0' &&
            app->run_cfg.config[0]  != '\0' &&
            app->run_cfg.project[0] != '\0') {
            SessionDiscCmd dcmd = {0};
            dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
            snprintf(dcmd.project, sizeof(dcmd.project), "%s",
                     app->run_cfg.project);
            snprintf(dcmd.scheme, sizeof(dcmd.scheme), "%s",
                     app->run_cfg.scheme);
            snprintf(dcmd.config, sizeof(dcmd.config), "%s",
                     app->run_cfg.config);
            session_disc_submit(app->session, &dcmd);
            app->resolving_bundle_id = true;
        }
    }
    if (ri.config_edited) {
        app->config_user_edited = true;
        if (online && !app->bundle_id_user_edited &&
            app->run_cfg.scheme[0]  != '\0' &&
            app->run_cfg.config[0]  != '\0' &&
            app->run_cfg.project[0] != '\0') {
            SessionDiscCmd dcmd = {0};
            dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
            snprintf(dcmd.project, sizeof(dcmd.project), "%s",
                     app->run_cfg.project);
            snprintf(dcmd.scheme, sizeof(dcmd.scheme), "%s",
                     app->run_cfg.scheme);
            snprintf(dcmd.config, sizeof(dcmd.config), "%s",
                     app->run_cfg.config);
            session_disc_submit(app->session, &dcmd);
            app->resolving_bundle_id = true;
        }
    }
    if (ri.bundle_id_edited)
        app->bundle_id_user_edited = true;

    /* ── preset intents ─────────────────────────────────────────────── */
    if (ri.pick_preset >= 0 && ri.pick_preset < app->presets.count) {
        app->preset_selected          = ri.pick_preset;
        app->presets.active_index     = ri.pick_preset;
        const Preset *p               = &app->presets.items[ri.pick_preset];
        snprintf(app->run_cfg.project,   sizeof(app->run_cfg.project),
                 "%s", p->project);
        snprintf(app->run_cfg.scheme,    sizeof(app->run_cfg.scheme),
                 "%s", p->scheme);
        snprintf(app->run_cfg.config,    sizeof(app->run_cfg.config),
                 "%s", p->config);
        snprintf(app->run_cfg.bundle_id, sizeof(app->run_cfg.bundle_id),
                 "%s", p->bundle_id);
        app->scheme_user_edited    = false;
        app->config_user_edited    = false;
        app->bundle_id_user_edited = false;
        if (app->conn_key[0] != '\0')
            preset_save(app->conn_key, &app->presets);
    }
    if (ri.preset_new && ri.preset_name[0] != '\0' &&
        app->presets.items && app->presets.count < APP_MAX_PRESETS) {
        int     idx = app->presets.count;
        Preset *p   = &app->presets.items[idx];
        memset(p, 0, sizeof(*p));
        snprintf(p->name,      sizeof(p->name),      "%s", ri.preset_name);
        snprintf(p->project,   sizeof(p->project),   "%s", app->run_cfg.project);
        snprintf(p->scheme,    sizeof(p->scheme),    "%s", app->run_cfg.scheme);
        snprintf(p->config,    sizeof(p->config),    "%s", app->run_cfg.config);
        snprintf(p->bundle_id, sizeof(p->bundle_id), "%s", app->run_cfg.bundle_id);
        app->presets.count++;
        app->presets.active_index = idx;
        app->preset_selected      = idx;
        if (app->conn_key[0] != '\0')
            preset_save(app->conn_key, &app->presets);
    }
    if (ri.preset_rename && ri.preset_name[0] != '\0' &&
        app->presets.items &&
        app->preset_selected >= 0 && app->preset_selected < app->presets.count) {
        snprintf(app->presets.items[app->preset_selected].name,
                 sizeof(app->presets.items[app->preset_selected].name),
                 "%s", ri.preset_name);
        if (app->conn_key[0] != '\0')
            preset_save(app->conn_key, &app->presets);
    }
    if (ri.preset_delete && app->presets.items &&
        app->preset_selected >= 0 && app->preset_selected < app->presets.count) {
        int del = app->preset_selected;
        int n   = app->presets.count;
        for (int i = del; i < n - 1; i++)
            app->presets.items[i] = app->presets.items[i + 1];
        app->presets.count--;
        if (app->presets.count == 0) {
            app->preset_selected      = -1;
            app->presets.active_index = -1;
        } else {
            int new_sel               = (del < app->presets.count) ? del
                                                                    : app->presets.count - 1;
            app->preset_selected      = new_sel;
            app->presets.active_index = new_sel;
        }
        if (app->conn_key[0] != '\0')
            preset_save(app->conn_key, &app->presets);
    }

    return keep_going;
}

void app_shutdown(App *app) {
    session_close(app->session);
    ui_shutdown(app->ui);
    arena_destroy(app->blueprints_arena);
    arena_destroy(app->bp_read_arena);
    arena_destroy(app->presets_arena);
}
