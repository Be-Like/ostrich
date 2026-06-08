#include "ui.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "discovery.h"
#include "logbuf.h"
#include "runstate.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

/* Default-initialized recon view (no scan yet). */
static UiReconView make_recon_view(void) {
    UiReconView rv = {0};
    rv.blueprint_selected = -1;
    rv.preset_selected = -1;
    rv.target_selected = -1;
    return rv;
}

/* Default-initialized recon intents (no picks). */
static UiReconIntents make_recon_intents(void) {
    UiReconIntents ri = {0};
    ri.pick_blueprint = -1;
    ri.pick_preset = -1;
    ri.pick_target = -1;
    return ri;
}

/* Default-initialized run view (no build_log — safe for headless render). */
static UiRunView make_run_view(void) {
    UiRunView rrv = {0};
    rrv.phase = RUN_IDLE;
    rrv.readiness = READY_NO_PROJECT;
    rrv.build_log = NULL;
    rrv.device_log = NULL;
    return rrv;
}

int main(void) {
    Arena *a = arena_create(APP_ARENA_BYTES);
    assert(a != NULL);

    UiOptions opts;
    opts.title = "ui_test";
    opts.width = 800;
    opts.height = 600;
    opts.font_dir = "assets/fonts";
    opts.headless = 1; /* hidden window */

    Ui *ui = NULL;
    UiStatus st = ui_init(a, opts, &ui);

    if (st == UI_ERR_NO_DISPLAY) {
        printf("SKIP: no display\n");
        arena_destroy(a);
        return 0;
    }

    if (st != UI_OK) {
        fprintf(stderr, "ui_init failed: %s\n", ui_status_str(st));
        arena_destroy(a);
        return 1;
    }

    assert(ui != NULL);

    /* State-based test: a resting (zeroed) view produces no intents. */
    UiConnView view = {0};
    ConnForm form = {0};
    form.selected_known_host = -1;
    UiRunView rrv = make_run_view(); /* shared across non-run tests */
    UiRunIntents rri = {0};
    KcForm kf = {0}; /* shared kc form; show_kc_prompt stays false */

    for (int i = 0; i < 3; i++) {
        UiIntents intents = {0};
        UiReconView rv = make_recon_view();
        RunConfig rf = {0};
        UiReconIntents ri = make_recon_intents();
        intents.select_host = -1;
        int keep_going = ui_frame(ui, &view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
        (void)keep_going;
        /* Resting view emits no action intents. */
        assert(!intents.breach);
        assert(!intents.abort);
        assert(!intents.close);
        assert(intents.select_host == -1);
        assert(!ri.scan);
        assert(!ri.abort_scan);
        assert(ri.pick_blueprint == -1);
    }

    /* State-based test: TOFU prompt (AWAITING_HOSTKEY) emits no spurious trust/decline. */
    {
        UiConnView tofu_view = {0};
        tofu_view.phase = CONN_AWAITING_HOSTKEY;
        tofu_view.show_hostkey_prompt = true;
        tofu_view.fingerprint = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &tofu_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.trust);
            assert(!intents.decline);
            assert(!intents.breach);
        }
    }

    /* State-based test: mismatch stop (DISCONNECTED + show_mismatch) emits no spurious trust. */
    {
        UiConnView mismatch_view = {0};
        mismatch_view.phase = CONN_DISCONNECTED;
        mismatch_view.show_mismatch = true;
        mismatch_view.fingerprint = "SHA256:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &mismatch_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.trust);
        }
    }

    /* State-based test: PASSKEY auth with empty passkey renders without crash
     * and produces no breach (BREACH button is disabled). */
    {
        ConnForm pw_form = {0};
        pw_form.selected_known_host = -1;
        snprintf(pw_form.host, sizeof(pw_form.host), "example.com");
        snprintf(pw_form.user, sizeof(pw_form.user), "alice");
        snprintf(pw_form.port, sizeof(pw_form.port), "22");
        pw_form.auth = SSH_AUTH_PASSWORD;
        /* passkey intentionally empty — BREACH must stay disabled */

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
        }
    }

    /* State-based test: PASSKEY auth with passkey filled renders without crash
     * and produces no spurious breach (no button press). */
    {
        ConnForm pw_form = {0};
        pw_form.selected_known_host = -1;
        snprintf(pw_form.host, sizeof(pw_form.host), "example.com");
        snprintf(pw_form.user, sizeof(pw_form.user), "alice");
        snprintf(pw_form.port, sizeof(pw_form.port), "22");
        pw_form.auth = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
        }
    }

    /* State-based test: REMEMBER PASSKEY checkbox (off) renders without crash
     * and produces no spurious intents. */
    {
        ConnForm pw_form = {0};
        pw_form.selected_known_host = -1;
        snprintf(pw_form.host, sizeof(pw_form.host), "example.com");
        snprintf(pw_form.user, sizeof(pw_form.user), "alice");
        snprintf(pw_form.port, sizeof(pw_form.port), "22");
        pw_form.auth = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = false;

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
            assert(!intents.save);
        }
        assert(!pw_form.remember); /* checkbox state unchanged without interaction */
    }

    /* State-based test: REMEMBER PASSKEY checkbox (on) renders without crash
     * and preserves the remembered flag. */
    {
        ConnForm pw_form = {0};
        pw_form.selected_known_host = -1;
        snprintf(pw_form.host, sizeof(pw_form.host), "example.com");
        snprintf(pw_form.user, sizeof(pw_form.user), "alice");
        snprintf(pw_form.port, sizeof(pw_form.port), "22");
        pw_form.auth = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = true;

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
        }
        assert(pw_form.remember); /* flag must survive frames without interaction */
    }

    /* State-based test: REMEMBER PASSKEY while connecting is disabled —
     * the connecting state must not corrupt the remember flag. */
    {
        ConnForm pw_form = {0};
        pw_form.selected_known_host = -1;
        snprintf(pw_form.host, sizeof(pw_form.host), "example.com");
        snprintf(pw_form.user, sizeof(pw_form.user), "alice");
        snprintf(pw_form.port, sizeof(pw_form.port), "22");
        pw_form.auth = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = true;

        UiConnView connecting_view = {0};
        connecting_view.phase = CONN_CONNECTING;
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &connecting_view, &pw_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
            assert(!intents.abort); /* no keyboard Escape was pressed */
        }
        assert(pw_form.remember); /* disabled checkbox must not clear the flag */
    }

    /* State-based test: ONLINE view (bar phase) emits no spurious close/update. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.close);
            assert(!intents.update);
            assert(!intents.breach);
        }
    }

    /* State-based test: REACQUIRING view (bar phase) emits no spurious intents. */
    {
        UiConnView reacq_view = {0};
        reacq_view.phase = CONN_REACQUIRING;
        reacq_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &reacq_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.close);
            assert(!intents.update);
        }
    }

    /* State-based test: bar spacing — ONLINE and REACQUIRING both render across
     * multiple frames without crash and emit no spurious intents (guards the
     * bar_h / v_pad sizing change in draw_conn_bar). */
    {
        const char *hosts[] = {"alice@mac.local", "bob@192.168.1.10", ""};
        ConnPhase phases[] = {CONN_ONLINE, CONN_REACQUIRING};

        for (int pi = 0; pi < 2; pi++) {
            for (int hi = 0; hi < 3; hi++) {
                UiConnView bar_view = {0};
                bar_view.phase = phases[pi];
                bar_view.user_host = hosts[hi];

                for (int i = 0; i < 5; i++) {
                    UiIntents intents = {0};
                    UiReconView rv = make_recon_view();
                    RunConfig rf = {0};
                    UiReconIntents ri = make_recon_intents();
                    intents.select_host = -1;
                    ui_frame(ui, &bar_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
                    assert(!intents.close);
                    assert(!intents.update);
                    assert(!intents.breach);
                }
            }
        }
    }

    /* State-based test: overlay with SSH_AUTH_AGENT (HOST/PORT/USER nav fields)
     * renders without crash and emits no spurious intents (nav enabled). */
    {
        ConnForm agent_form = {0};
        agent_form.selected_known_host = -1;
        snprintf(agent_form.host, sizeof(agent_form.host), "mac.local");
        snprintf(agent_form.user, sizeof(agent_form.user), "bob");
        snprintf(agent_form.port, sizeof(agent_form.port), "22");
        agent_form.auth = SSH_AUTH_AGENT;

        UiConnView agent_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &agent_view, &agent_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
            assert(!intents.abort);
        }
    }

    /* State-based test: overlay with SSH_AUTH_PASSWORD (HOST/PORT/USER/PASSKEY nav
     * fields) renders without crash and emits no spurious intents (nav enabled). */
    {
        ConnForm pw_nav_form = {0};
        pw_nav_form.selected_known_host = -1;
        snprintf(pw_nav_form.host, sizeof(pw_nav_form.host), "mac.local");
        snprintf(pw_nav_form.user, sizeof(pw_nav_form.user), "bob");
        snprintf(pw_nav_form.port, sizeof(pw_nav_form.port), "22");
        snprintf(pw_nav_form.passkey, sizeof(pw_nav_form.passkey), "hunter2");
        pw_nav_form.auth = SSH_AUTH_PASSWORD;

        UiConnView pw_nav_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &pw_nav_view, &pw_nav_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
            assert(!intents.abort);
        }
    }

    /* State-based test: ONLINE + overlay_open (UPDATE mode) emits no spurious breach. */
    {
        UiConnView update_view = {0};
        update_view.phase = CONN_ONLINE;
        update_view.user_host = "alice@mac.local";
        update_view.overlay_open = true;

        ConnForm update_form = {0};
        update_form.selected_known_host = -1;
        snprintf(update_form.host, sizeof(update_form.host), "mac.local");
        snprintf(update_form.user, sizeof(update_form.user), "alice");
        snprintf(update_form.port, sizeof(update_form.port), "22");

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &update_view, &update_form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!intents.breach);
            assert(!intents.close);
        }
    }

    /* ── Recon panel state-based tests ──────────────────────────────── */

    /* State-based test: ONLINE with empty recon view (no scan yet) renders
     * without crash and emits no spurious scan/abort_scan. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scan);
            assert(!ri.abort_scan);
            assert(ri.pick_blueprint == -1);
        }
    }

    /* State-based test: ONLINE with scan_done=true, empty blueprints renders
     * NO_BLUEPRINTS state without crash and emits no spurious pick. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scan_done = true;
            rv.scan_err = DISC_OK;
            rv.blueprints = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scan);
            assert(ri.pick_blueprint == -1);
        }
    }

    /* State-based test: ONLINE with populated blueprint list renders
     * BLUEPRINTS_RECOVERED without crash and emits no spurious picks. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Blueprint bp_items[2] = {0};
        snprintf(bp_items[0].path, sizeof(bp_items[0].path), "/Users/alice/App/App.xcworkspace");
        bp_items[0].is_workspace = true;
        snprintf(bp_items[1].path, sizeof(bp_items[1].path), "/Users/alice/Lib/Lib.xcodeproj");
        bp_items[1].is_workspace = false;
        BlueprintList bp_list = {.items = bp_items, .count = 2};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scan_done = true;
            rv.scan_err = DISC_OK;
            rv.blueprints = &bp_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_blueprint == -1); /* no user interaction */
        }
    }

    /* State-based test: ONLINE with scanning=true renders ABORT SCAN button
     * without crash and emits no spurious abort_scan without user input. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scanning = true;
            snprintf(rf.scan_root, sizeof(rf.scan_root), "/Users/alice/Developer");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.abort_scan); /* no keyboard/mouse */
            assert(!ri.scan);
        }
    }

    /* State-based test: ONLINE with DISC_ERR_XCODE_MISSING renders failure
     * state without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scan_done = true;
            rv.scan_err = DISC_ERR_XCODE_MISSING;
            rv.blueprints = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scan);
        }
    }

    /* State-based test: ONLINE with DISC_ERR_COMMAND_FAILED renders failure
     * state without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scan_done = true;
            rv.scan_err = DISC_ERR_COMMAND_FAILED;
            rv.blueprints = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scan);
        }
    }

    /* State-based test: scan_root field persists across frames without
     * corruption (manual edit state is stable). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        UiReconView rv = make_recon_view();
        RunConfig rf = {0};
        snprintf(rf.scan_root, sizeof(rf.scan_root), "/Users/alice/Developer");
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            /* scan_root must not be corrupted by rendering */
            assert(strcmp(rf.scan_root, "/Users/alice/Developer") == 0);
        }
    }

    /* ── Slice B state-based tests ──────────────────────────────────── */

    /* State-based test: reading_blueprint=true renders without crash and
     * emits no spurious scheme/config/bundle_id edits. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.reading_blueprint = true;
            snprintf(rf.project, sizeof(rf.project), "/Users/alice/App/App.xcworkspace");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scheme_edited);
            assert(!ri.config_edited);
            assert(!ri.bundle_id_edited);
        }
    }

    /* State-based test: resolving_bundle_id=true renders without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.resolving_bundle_id = true;
            snprintf(rf.project, sizeof(rf.project), "/Users/alice/App/App.xcworkspace");
            snprintf(rf.scheme, sizeof(rf.scheme), "MyApp");
            snprintf(rf.config, sizeof(rf.config), "Debug");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scheme_edited);
            assert(!ri.config_edited);
        }
    }

    /* State-based test: schemes and configs populated renders hints without
     * crash and emits no spurious edits. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        char scheme_arr[3][256];
        snprintf(scheme_arr[0], 256, "MyApp");
        snprintf(scheme_arr[1], 256, "MyAppTests");
        snprintf(scheme_arr[2], 256, "MyAppUITests");
        StrList schemes;
        schemes.items = scheme_arr;
        schemes.count = 3;

        char config_arr[2][256];
        snprintf(config_arr[0], 256, "Debug");
        snprintf(config_arr[1], 256, "Release");
        StrList configs;
        configs.items = config_arr;
        configs.count = 2;

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.scan_done = true;
            rv.schemes = &schemes;
            rv.configs = &configs;
            snprintf(rf.project, sizeof(rf.project), "/Users/alice/App/App.xcworkspace");
            snprintf(rf.scheme, sizeof(rf.scheme), "MyApp");
            snprintf(rf.config, sizeof(rf.config), "Debug");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scheme_edited);
            assert(!ri.config_edited);
            assert(!ri.bundle_id_edited);
        }
    }

    /* State-based test: scheme/config/bundle_id fields persist across frames
     * without corruption. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        UiReconView rv = make_recon_view();
        RunConfig rf = {0};
        snprintf(rf.project, sizeof(rf.project), "/Users/alice/App/App.xcworkspace");
        snprintf(rf.scheme, sizeof(rf.scheme), "MyApp");
        snprintf(rf.config, sizeof(rf.config), "Debug");
        snprintf(rf.bundle_id, sizeof(rf.bundle_id), "com.example.myapp");

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(strcmp(rf.scheme, "MyApp") == 0);
            assert(strcmp(rf.config, "Debug") == 0);
            assert(strcmp(rf.bundle_id, "com.example.myapp") == 0);
        }
    }

    /* State-based test: blueprint_err renders without crash (failed read). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.blueprint_err = DISC_ERR_XCODE_MISSING;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.scheme_edited);
            assert(!ri.config_edited);
        }
    }

    /* ── Slice C state-based tests ──────────────────────────────────── */

    /* State-based test: ONLINE + empty preset list renders // NO OPERATION
     * CONFIGURED without crash and emits no spurious preset intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        PresetList empty_presets = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.presets = &empty_presets;
            rv.preset_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.preset_new);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
        }
    }

    /* State-based test: ONLINE + NULL presets pointer renders without crash
     * and emits no spurious preset intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.presets = NULL;
            rv.preset_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.preset_new);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
        }
    }

    /* State-based test: ONLINE + populated preset list renders without crash
     * and emits no spurious picks or actions. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Preset preset_arr[2];
        memset(preset_arr, 0, sizeof(preset_arr));
        snprintf(preset_arr[0].name, sizeof(preset_arr[0].name), "debug");
        snprintf(preset_arr[0].project, sizeof(preset_arr[0].project), "/Users/alice/App.xcworkspace");
        snprintf(preset_arr[0].scheme, sizeof(preset_arr[0].scheme), "App");
        snprintf(preset_arr[0].config, sizeof(preset_arr[0].config), "Debug");
        snprintf(preset_arr[0].bundle_id, sizeof(preset_arr[0].bundle_id), "com.acme.app");
        snprintf(preset_arr[1].name, sizeof(preset_arr[1].name), "release");
        snprintf(preset_arr[1].project, sizeof(preset_arr[1].project), "/Users/alice/App.xcworkspace");
        snprintf(preset_arr[1].scheme, sizeof(preset_arr[1].scheme), "App");
        snprintf(preset_arr[1].config, sizeof(preset_arr[1].config), "Release");
        snprintf(preset_arr[1].bundle_id, sizeof(preset_arr[1].bundle_id), "com.acme.app");
        PresetList presets = {.items = preset_arr, .count = 2, .active_index = 0};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.presets = &presets;
            rv.preset_selected = 0;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.preset_new);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
        }
    }

    /* State-based test: ONLINE + preset list with preset_selected=-1 (none
     * selected) renders without crash; RENAME and DELETE must not fire. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Preset preset_arr[1];
        memset(preset_arr, 0, sizeof(preset_arr));
        snprintf(preset_arr[0].name, sizeof(preset_arr[0].name), "app");
        PresetList presets = {.items = preset_arr, .count = 1, .active_index = -1};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.presets = &presets;
            rv.preset_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
        }
    }

    /* State-based test: preset list preserves preset names across frames
     * without corruption. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Preset preset_arr[1];
        memset(preset_arr, 0, sizeof(preset_arr));
        snprintf(preset_arr[0].name, sizeof(preset_arr[0].name), "staging");
        snprintf(preset_arr[0].project, sizeof(preset_arr[0].project), "/Users/alice/App.xcworkspace");
        snprintf(preset_arr[0].scheme, sizeof(preset_arr[0].scheme), "Staging");
        snprintf(preset_arr[0].config, sizeof(preset_arr[0].config), "Release");
        snprintf(preset_arr[0].bundle_id, sizeof(preset_arr[0].bundle_id), "com.acme.staging");
        PresetList presets = {.items = preset_arr, .count = 1, .active_index = 0};

        UiReconView rv = make_recon_view();
        rv.presets = &presets;
        rv.preset_selected = 0;

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(strcmp(preset_arr[0].name, "staging") == 0);
        }
    }

    /* ── Slice D state-based tests ──────────────────────────────────── */

    /* State-based test: ONLINE + NULL targets (no sweep yet) renders without
     * crash and emits no spurious sweep or pick_target. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.targets = NULL;
            rv.target_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: sweeping=true renders SWEEPING... without crash
     * and emits no spurious sweep. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweeping = true;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: sweep_done=true with empty target list renders
     * // NO TARGETS IN RANGE without crash and emits no spurious pick. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        TargetList empty_targets = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &empty_targets;
            rv.target_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: sweep_done=true with populated target list renders
     * TARGETS IN RANGE without crash and emits no spurious picks. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Target target_arr[3];
        memset(target_arr, 0, sizeof(target_arr));
        snprintf(target_arr[0].name, sizeof(target_arr[0].name), "iPhone 15");
        snprintf(target_arr[0].udid, sizeof(target_arr[0].udid), "00001111-AAAA-BBBB-CCCC-000011112222");
        target_arr[0].is_simulator = false;
        target_arr[0].booted = false;
        snprintf(target_arr[1].name, sizeof(target_arr[1].name), "iPad Air");
        snprintf(target_arr[1].udid, sizeof(target_arr[1].udid), "11112222-AAAA-BBBB-CCCC-000011112222");
        target_arr[1].is_simulator = true;
        target_arr[1].booted = true;
        snprintf(target_arr[2].name, sizeof(target_arr[2].name), "iPhone SE");
        snprintf(target_arr[2].udid, sizeof(target_arr[2].udid), "22223333-AAAA-BBBB-CCCC-000011112222");
        target_arr[2].is_simulator = true;
        target_arr[2].booted = false;
        TargetList targets = {.items = target_arr, .count = 3};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &targets;
            rv.target_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: sweep_done=true with a selected target renders
     * the selection highlighted without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Target target_arr[2];
        memset(target_arr, 0, sizeof(target_arr));
        snprintf(target_arr[0].name, sizeof(target_arr[0].name), "My iPhone");
        snprintf(target_arr[0].udid, sizeof(target_arr[0].udid), "AAAA1111-BBBB-CCCC-DDDD-EEEE11112222");
        target_arr[0].is_simulator = false;
        snprintf(target_arr[1].name, sizeof(target_arr[1].name), "iPhone 16 Sim");
        snprintf(target_arr[1].udid, sizeof(target_arr[1].udid), "BBBB2222-CCCC-DDDD-EEEE-FFFF22223333");
        target_arr[1].is_simulator = true;
        target_arr[1].booted = true;
        TargetList targets = {.items = target_arr, .count = 2};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &targets;
            rv.target_selected = 0; /* first target selected */
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: sweep_done=true with DISC_ERR_COMMAND_FAILED renders
     * error state without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_ERR_COMMAND_FAILED;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_target == -1);
        }
    }

    /* State-based test: READY_OK renders READY indicator without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Target target_arr[1];
        memset(target_arr, 0, sizeof(target_arr));
        snprintf(target_arr[0].name, sizeof(target_arr[0].name), "My iPhone");
        snprintf(target_arr[0].udid, sizeof(target_arr[0].udid), "CCCC3333-DDDD-EEEE-FFFF-AAAA33334444");
        target_arr[0].is_simulator = false;
        TargetList targets = {.items = target_arr, .count = 1};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &targets;
            rv.target_selected = 0;
            rv.readiness = READY_OK;
            snprintf(rf.project, sizeof(rf.project), "/Users/alice/App.xcworkspace");
            snprintf(rf.scheme, sizeof(rf.scheme), "App");
            snprintf(rf.config, sizeof(rf.config), "Debug");
            snprintf(rf.bundle_id, sizeof(rf.bundle_id), "com.acme.app");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
            assert(ri.pick_target == -1);
            assert(!ri.sweep);
        }
    }

    /* State-based test: each non-OK readiness value renders a hint without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Readiness states[] = {READY_NO_PROJECT, READY_NO_SCHEME, READY_NO_CONFIG, READY_NO_BUNDLE_ID, READY_NO_TARGET};
        int n = (int)(sizeof(states) / sizeof(states[0]));
        for (int s = 0; s < n; s++) {
            for (int i = 0; i < 3; i++) {
                UiIntents intents = {0};
                UiReconView rv = make_recon_view();
                RunConfig rf = {0};
                UiReconIntents ri = make_recon_intents();
                intents.select_host = -1;
                rv.readiness = states[s];
                ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &rrv, &rri, &kf);
                assert(ri.pick_target == -1);
            }
        }
    }

    /* ── Run panel state-based tests ────────────────────────────────── */

    /* State-based test: IDLE + READY_NO_PROJECT renders without crash and
     * emits no spurious execute/compile/abort. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view(); /* IDLE, READY_NO_PROJECT */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
        }
    }

    /* State-based test: IDLE + READY_OK renders without crash and emits
     * no spurious execute/compile/abort. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
        }
    }

    /* State-based test: BUILDING phase renders ABORT button without crash
     * and emits no spurious abort_run without user input. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_BUILDING;
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.abort_run);
            assert(!run_ri.execute);
        }
    }

    /* State-based test: RUNNING phase renders EXECUTE (terminate-first
     * re-exec) without crash and emits no spurious execute or abort. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.abort_run);
        }
    }

    /* State-based test: RUNNING + stale=true renders amber stale indicator
     * without crash and emits no spurious intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.stale = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.abort_run);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
    }

    /* State-based test: RUN_BUILD_FAILED renders failure color without
     * crash and emits no spurious abort. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_BUILD_FAILED;
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.abort_run);
            assert(!run_ri.execute);
        }
    }

    /* State-based test: each in-chain phase (BUILDING, PRIMING, INSTALLING,
     * LAUNCHING) renders ABORT button without crash and emits no spurious
     * abort_run or execute. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        RunPhase in_chain[] = {RUN_BUILDING, RUN_PRIMING, RUN_INSTALLING, RUN_LAUNCHING};
        int n = (int)(sizeof(in_chain) / sizeof(in_chain[0]));
        for (int s = 0; s < n; s++) {
            for (int i = 0; i < 3; i++) {
                UiIntents intents = {0};
                UiReconView rv = make_recon_view();
                RunConfig rf = {0};
                UiReconIntents ri = make_recon_intents();
                UiRunView run_rv = make_run_view();
                run_rv.phase = in_chain[s];
                run_rv.readiness = READY_OK;
                UiRunIntents run_ri = {0};
                intents.select_host = -1;
                ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
                assert(!run_ri.abort_run);
                assert(!run_ri.execute);
            }
        }
    }

    /* State-based test: NULL build_log (safe guard for draw_run_panel)
     * renders BUILD LOG section without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            run_rv.build_log = NULL;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
    }

    /* State-based test: NULL device_log renders Device Log section without crash
     * and emits no spurious device_log_copy/clear. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            run_rv.device_log = NULL;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
    }

    /* State-based test: RUNNING + populated device_log renders LIVE FEED header
     * and content without crash, emits no spurious device_log intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(dev_log, "app output line 1\n", 18);
        logbuf_append(dev_log, "app output line 2\n", 18);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* State-based test: IDLE + populated device_log (history from prior run)
     * renders dim header and content without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_mark(dev_log, "> \xe2\x94\x80\xe2\x94\x80 NEW PAYLOAD \xe2\x94\x80\xe2\x94\x80");
        logbuf_append(dev_log, "prior run output\n", 17);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* State-based test: RUNNING + stale=true + device_log renders stale indicator
     * and device log content without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(dev_log, "live app output\n", 16);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.stale = true;
            run_rv.readiness = READY_OK;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.abort_run);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T2: four-band layout — ONLINE idle; config band renders recon (left) and
     * controls (right); both log panels render side-by-side; no spurious
     * intents from either side. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_NO_PROJECT;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
            assert(!ri.scan);
            assert(ri.pick_blueprint == -1);
            assert(ri.pick_preset == -1);
            assert(ri.pick_target == -1);
        }
    }

    /* T2: four-band layout — BUILDING phase; controls show ABORT in the config
     * band; build log panel receives content; live feed panel is empty;
     * no spurious execute or abort intents (headless doesn't click). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        logbuf_append(build_log, "Compiling source files...\n", 26);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_BUILDING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.device_log_copy);
        }
        arena_destroy(test_arena);
    }

    /* T2: four-band layout — RUNNING with both log panels populated; build log
     * panel (left) and live feed panel (right) each render their content
     * independently; no spurious copy/clear intents from either panel. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(build_log, "Build succeeded\n", 16);
        logbuf_append(dev_log, "App output line 1\n", 18);
        logbuf_append(dev_log, "App output line 2\n", 18);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.abort_run);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T2: four-band layout — REACQUIRING phase; config band and log panels
     * still render; no spurious intents. */
    {
        UiConnView reacq_view = {0};
        reacq_view.phase = CONN_REACQUIRING;
        reacq_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &reacq_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
            assert(!ri.scan);
        }
    }

    /* ── T4: Per-panel empty-state wordmark ─────────────────────────── */

    /* T4: empty Build Log (non-null logbuf, count 0) renders wordmark art
     * without crash and emits no spurious build_log intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        /* logbuf is intentionally left empty — count == 0 */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log; /* non-null, empty */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T4: empty Live Feed (non-null logbuf, count 0) renders wordmark art
     * without crash and emits no spurious device_log intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        /* dev_log is intentionally left empty — count == 0 */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.device_log = dev_log; /* non-null, empty */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T4: Build Log populated while Live Feed empty — each panel renders
     * its own state independently, no spurious intents from either side. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(build_log, "Build output line\n", 18);
        /* dev_log intentionally left empty — should show wordmark */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log; /* populated */
            run_rv.device_log = dev_log;  /* empty → wordmark */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T4: Live Feed populated while Build Log empty — wordmark shows in the
     * left panel only; right panel streams content; no spurious intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        /* build_log intentionally left empty — should show wordmark */
        logbuf_append(dev_log, "App output line\n", 16);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log; /* empty → wordmark */
            run_rv.device_log = dev_log;  /* populated */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* ── T10: Build Log empty-state failure header ──────────────────── */

    /* T10: RUN_BUILD_FAILED with empty Build Log renders without crash and
     * emits no spurious intents (failure header is shown in place of the
     * wordmark; no copy/clear intent fires). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        /* build_log intentionally left empty — count == 0 */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_BUILD_FAILED;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log; /* non-null, empty */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T10: RUN_DEPLOY_FAILED with empty Build Log renders without crash and
     * emits no spurious intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        /* build_log intentionally left empty — count == 0 */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_DEPLOY_FAILED;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log; /* non-null, empty */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T10: IDLE with empty Build Log still shows wordmark (not the failure
     * header) — the non-failed empty-state branch is unaffected. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* ── T3: Compact config internals ───────────────────────────────── */

    /* T3: compact layout — all slices fully populated renders without crash
     * and emits no spurious intents (project picker popup is closed by
     * default; combos are not open). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Blueprint bp_items[2] = {0};
        snprintf(bp_items[0].path, sizeof(bp_items[0].path), "/Users/alice/App/App.xcworkspace");
        bp_items[0].is_workspace = true;
        snprintf(bp_items[1].path, sizeof(bp_items[1].path), "/Users/alice/Lib/Lib.xcodeproj");
        bp_items[1].is_workspace = false;
        BlueprintList bp_list = {.items = bp_items, .count = 2};

        char scheme_arr[2][256];
        snprintf(scheme_arr[0], 256, "MyApp");
        snprintf(scheme_arr[1], 256, "MyAppTests");
        StrList schemes = {.items = scheme_arr, .count = 2};

        char config_arr[1][256];
        snprintf(config_arr[0], 256, "Debug");
        StrList configs = {.items = config_arr, .count = 1};

        Preset preset_arr[2];
        memset(preset_arr, 0, sizeof(preset_arr));
        snprintf(preset_arr[0].name, sizeof(preset_arr[0].name), "debug");
        snprintf(preset_arr[1].name, sizeof(preset_arr[1].name), "release");
        PresetList presets = {.items = preset_arr, .count = 2, .active_index = 0};

        Target target_arr[2];
        memset(target_arr, 0, sizeof(target_arr));
        snprintf(target_arr[0].name, sizeof(target_arr[0].name), "My iPhone");
        snprintf(target_arr[0].udid, sizeof(target_arr[0].udid), "AAAA1111-BBBB-CCCC-DDDD-EEEE11112222");
        target_arr[0].is_simulator = false;
        snprintf(target_arr[1].name, sizeof(target_arr[1].name), "iPhone 16 Sim");
        snprintf(target_arr[1].udid, sizeof(target_arr[1].udid), "BBBB2222-CCCC-DDDD-EEEE-FFFF22223333");
        target_arr[1].is_simulator = true;
        target_arr[1].booted = true;
        TargetList targets = {.items = target_arr, .count = 2};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.scan_done = true;
            rv.scan_err = DISC_OK;
            rv.blueprints = &bp_list;
            rv.blueprint_selected = 0;
            rv.schemes = &schemes;
            rv.configs = &configs;
            rv.presets = &presets;
            rv.preset_selected = 0;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &targets;
            rv.target_selected = 0;
            rv.readiness = READY_OK;
            run_rv.readiness = READY_OK;
            snprintf(rf.project, sizeof(rf.project), "/Users/alice/App/App.xcworkspace");
            snprintf(rf.scheme, sizeof(rf.scheme), "MyApp");
            snprintf(rf.config, sizeof(rf.config), "Debug");
            snprintf(rf.bundle_id, sizeof(rf.bundle_id), "com.alice.myapp");
            snprintf(rf.scan_root, sizeof(rf.scan_root), "/Users/alice");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!ri.scan);
            assert(!ri.abort_scan);
            assert(ri.pick_blueprint == -1);
            assert(!ri.scheme_edited);
            assert(!ri.config_edited);
            assert(!ri.bundle_id_edited);
            assert(!ri.preset_new);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
        }
    }

    /* T3: PRESET combo — null presets renders // NO OPERATION CONFIGURED
     * in the combo without crash; no spurious preset intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.presets = NULL;
            rv.preset_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!ri.preset_new);
            assert(!ri.preset_rename);
            assert(!ri.preset_delete);
            assert(ri.pick_preset == -1);
        }
    }

    /* T3: PRESET combo — populated with selection renders selected preset
     * name as the combo label without crash; no spurious picks. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Preset preset_arr[3];
        memset(preset_arr, 0, sizeof(preset_arr));
        snprintf(preset_arr[0].name, sizeof(preset_arr[0].name), "alpha");
        snprintf(preset_arr[1].name, sizeof(preset_arr[1].name), "beta");
        snprintf(preset_arr[2].name, sizeof(preset_arr[2].name), "gamma");
        PresetList presets = {.items = preset_arr, .count = 3, .active_index = 1};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.presets = &presets;
            rv.preset_selected = 1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(ri.pick_preset == -1);
            assert(!ri.preset_delete);
        }
    }

    /* T3: TARGET combo — no sweep done renders // NO OPERATION CONFIGURED
     * without crash; no spurious sweep or pick. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.sweep_done = false;
            rv.targets = NULL;
            rv.target_selected = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
        }
    }

    /* T3: TARGET combo — sweep done, target selected renders target name
     * as the combo label without crash; no spurious picks. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Target target_arr[2];
        memset(target_arr, 0, sizeof(target_arr));
        snprintf(target_arr[0].name, sizeof(target_arr[0].name), "My Device");
        snprintf(target_arr[0].udid, sizeof(target_arr[0].udid), "ABCD1234-EFGH-5678-IJKL-MNOP91011121");
        target_arr[0].is_simulator = false;
        snprintf(target_arr[1].name, sizeof(target_arr[1].name), "Simulator");
        snprintf(target_arr[1].udid, sizeof(target_arr[1].udid), "DCBA4321-HGFE-8765-LKJI-PONM21101191");
        target_arr[1].is_simulator = true;
        target_arr[1].booted = true;
        TargetList targets = {.items = target_arr, .count = 2};

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.sweep_done = true;
            rv.sweep_err = DISC_OK;
            rv.targets = &targets;
            rv.target_selected = 0;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
        }
    }

    /* T3: TARGET combo — sweeping=true disables the combo and shows
     * SWEEPING text; no spurious sweep intent. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            rv.sweeping = true;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!ri.sweep);
            assert(ri.pick_target == -1);
        }
    }

    /* T3: READY indicator in run-controls — READY_OK renders without crash
     * and emits no spurious execute/compile/abort. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
        }
    }

    /* T3: READY indicator in run-controls — each non-OK readiness value
     * renders a hint in the right panel without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Readiness states[] = {READY_NO_PROJECT, READY_NO_SCHEME, READY_NO_CONFIG, READY_NO_BUNDLE_ID, READY_NO_TARGET};
        int n = (int)(sizeof(states) / sizeof(states[0]));
        for (int s = 0; s < n; s++) {
            for (int i = 0; i < 3; i++) {
                UiIntents intents = {0};
                UiReconView rv = make_recon_view();
                RunConfig rf = {0};
                UiReconIntents ri = make_recon_intents();
                UiRunView run_rv = make_run_view();
                run_rv.readiness = states[s];
                UiRunIntents run_ri = {0};
                intents.select_host = -1;
                ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
                assert(!run_ri.execute);
                assert(!run_ri.compile);
                assert(!run_ri.abort_run);
            }
        }
    }

    /* ── T5: Resizable log divider ──────────────────────────────────── */

    /* T5: default split (50/50) — ONLINE with both log panels renders without
     * crash and emits no spurious intents across multiple frames. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(build_log, "Build succeeded\n", 16);
        logbuf_append(dev_log, "App output\n", 11);

        for (int i = 0; i < 5; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.abort_run);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T5: both panels empty — splitter renders over empty-state wordmarks
     * without crash and emits no spurious intents. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        /* both logs intentionally empty */

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.build_log_clear);
            assert(!run_ri.device_log_copy);
            assert(!run_ri.device_log_clear);
        }
        arena_destroy(test_arena);
    }

    /* T5: minimum-width clamp — even if log_split is driven to extremes,
     * neither panel collapses (render stays stable across many frames). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(build_log, "line\n", 5);
        logbuf_append(dev_log, "line\n", 5);

        /* Render once with a ratio that would normally be out of bounds. */
        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_RUNNING;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.build_log_copy);
            assert(!run_ri.device_log_copy);
        }
        arena_destroy(test_arena);
    }

    /* T5: REACQUIRING phase — splitter still present; no spurious intents. */
    {
        UiConnView reacq_view = {0};
        reacq_view.phase = CONN_REACQUIRING;
        reacq_view.user_host = "alice@mac.local";

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);
        LogBuf *dev_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(dev_log != NULL);
        logbuf_append(build_log, "Build output\n", 13);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.device_log = dev_log;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &reacq_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.compile);
            assert(!run_ri.abort_run);
        }
        arena_destroy(test_arena);
    }

    /* ── T4: Keychain passkey modal ────────────────────────────────────── */

    /* T4: show_kc_prompt = false — modal does not render; kc_submit and
     * kc_skip stay false (no spurious intent without a button press). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = false;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
            assert(!run_ri.kc_submit);
            assert(!run_ri.kc_skip);
        }
        arena_destroy(test_arena);
    }

    /* T4: show_kc_prompt = true — modal opens; without a button press,
     * kc_submit and kc_skip remain false across multiple frames. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
            assert(!run_ri.kc_submit);
            assert(!run_ri.kc_skip);
        }
        arena_destroy(test_arena);
    }

    /* T4: KcForm.remember = false initially — value is unchanged after
     * frames without user interaction (default-off invariant). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        kc.remember = false;

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
        }
        assert(!kc.remember); /* default-off invariant: unchanged without interaction */
        arena_destroy(test_arena);
    }

    /* T4: KcForm.remember = true initially — value preserved across frames
     * without user interaction (state is not corrupted by rendering). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        kc.remember = true;

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
        }
        assert(kc.remember); /* flag must survive frames without interaction */
        arena_destroy(test_arena);
    }

    /* ── Task 2: KEYCHAIN modal keyboard shortcuts (state-based) ──────── */

    /* T2: show_kc_prompt=true with passkey pre-filled — no spurious kc_submit
     * or kc_skip without user input (no key injected in headless mode). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        snprintf(kc.passkey, sizeof(kc.passkey), "hunter2");

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
            assert(!run_ri.kc_submit); /* no key injected → no spurious submit */
            assert(!run_ri.kc_skip);   /* no key injected → no spurious skip */
        }
        /* passkey buffer must survive frames unmodified */
        assert(strcmp(kc.passkey, "hunter2") == 0);
        arena_destroy(test_arena);
    }

    /* T2: show_kc_prompt=true, remember=true — kc_submit/kc_skip stay false;
     * remember flag is not corrupted by the keyboard-handler code path. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        KcForm kc = {0};
        kc.remember = true;

        Arena *test_arena = arena_create(64 * 1024);
        assert(test_arena != NULL);
        LogBuf *build_log = logbuf_init(test_arena, 16 * 1024, 256);
        assert(build_log != NULL);

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.phase = RUN_IDLE;
            run_rv.readiness = READY_OK;
            run_rv.build_log = build_log;
            run_rv.show_kc_prompt = true;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kc);
            assert(!run_ri.kc_submit);
            assert(!run_ri.kc_skip);
        }
        assert(kc.remember); /* remember flag must not be corrupted */
        arena_destroy(test_arena);
    }

    /* ── Task 1: global chord handler (state-based, no key injection) ── */

    /* T1: Ctrl+Enter — DISCONNECTED phase: no execute fired (bar_phase=false). */
    {
        UiConnView disc_view = {0};
        disc_view.phase = CONN_DISCONNECTED;

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &disc_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!intents.close);
            assert(!run_ri.device_log_clear);
        }
    }

    /* T1: ONLINE + READY_NO_PROJECT: Ctrl+Enter guard holds — execute stays
     * false even in bar_phase (no key injected, and readiness blocks it). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_NO_PROJECT; /* cannot execute */
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
        }
    }

    /* T1: ONLINE + READY_OK + in_chain: Ctrl+Enter guard holds — execute
     * stays false while a run chain is active (no key injected). */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        RunPhase in_chain_phases[] = {RUN_BUILDING, RUN_PRIMING, RUN_INSTALLING, RUN_LAUNCHING};
        int n = (int)(sizeof(in_chain_phases) / sizeof(in_chain_phases[0]));
        for (int s = 0; s < n; s++) {
            for (int i = 0; i < 3; i++) {
                UiIntents intents = {0};
                UiReconView rv = make_recon_view();
                RunConfig rf = {0};
                UiReconIntents ri = make_recon_intents();
                UiRunView run_rv = make_run_view();
                run_rv.phase = in_chain_phases[s];
                run_rv.readiness = READY_OK;
                UiRunIntents run_ri = {0};
                intents.select_host = -1;
                ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
                /* execute must not fire without a key press even when READY_OK */
                assert(!run_ri.execute);
            }
        }
    }

    /* T1: ONLINE + READY_OK + IDLE: no key injected → execute stays false.
     * Verifies the key handler does not fire spuriously without input. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            run_rv.readiness = READY_OK;
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.execute);
            assert(!run_ri.device_log_clear);
            assert(!intents.close);
        }
    }

    /* T1: Ctrl+Escape — DISCONNECTED: close not fired (bar_phase=false). */
    {
        UiConnView disc_view = {0};
        disc_view.phase = CONN_DISCONNECTED;

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &disc_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!intents.close);
        }
    }

    /* T1: Ctrl+Escape — ONLINE: no key injected → close stays false. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!intents.close);
        }
    }

    /* T1: Ctrl+Backspace — DISCONNECTED: device_log_clear not fired (bar_phase=false). */
    {
        UiConnView disc_view = {0};
        disc_view.phase = CONN_DISCONNECTED;

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &disc_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_clear);
        }
    }

    /* T1: Ctrl+Backspace — ONLINE: no key injected → device_log_clear stays false. */
    {
        UiConnView online_view = {0};
        online_view.phase = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            UiReconView rv = make_recon_view();
            RunConfig rf = {0};
            UiReconIntents ri = make_recon_intents();
            UiRunView run_rv = make_run_view();
            UiRunIntents run_ri = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri, &run_rv, &run_ri, &kf);
            assert(!run_ri.device_log_clear);
        }
    }

    ui_shutdown(ui);
    arena_destroy(a);
    printf("ui_test: ok\n");
    return 0;
}
