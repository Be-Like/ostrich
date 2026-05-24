#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "ui.h"
#include "discovery.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

/* Default-initialized recon view (no scan yet). */
static UiReconView make_recon_view(void) {
    UiReconView rv = {0};
    rv.blueprint_selected = -1;
    rv.preset_selected    = -1;
    rv.target_selected    = -1;
    return rv;
}

/* Default-initialized recon intents (no picks). */
static UiReconIntents make_recon_intents(void) {
    UiReconIntents ri = {0};
    ri.pick_blueprint = -1;
    ri.pick_preset    = -1;
    ri.pick_target    = -1;
    return ri;
}

int main(void) {
    Arena *a = arena_create(APP_ARENA_BYTES);
    assert(a != NULL);

    UiOptions opts;
    opts.title    = "ui_test";
    opts.width    = 800;
    opts.height   = 600;
    opts.font_dir = "assets/fonts";
    opts.headless = 1; /* hidden window */

    Ui *ui   = NULL;
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
    UiConnView view    = {0};
    ConnForm   form    = {0};
    form.selected_known_host = -1;

    for (int i = 0; i < 3; i++) {
        UiIntents    intents = {0};
        UiReconView  rv      = make_recon_view();
        RunConfig    rf      = {0};
        UiReconIntents ri    = make_recon_intents();
        intents.select_host  = -1;
        int keep_going = ui_frame(ui, &view, &form, &intents, &rv, &rf, &ri);
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
        UiConnView tofu_view          = {0};
        tofu_view.phase               = CONN_AWAITING_HOSTKEY;
        tofu_view.show_hostkey_prompt = true;
        tofu_view.fingerprint         = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &tofu_view, &form, &intents, &rv, &rf, &ri);
            assert(!intents.trust);
            assert(!intents.decline);
            assert(!intents.breach);
        }
    }

    /* State-based test: mismatch stop (DISCONNECTED + show_mismatch) emits no spurious trust. */
    {
        UiConnView mismatch_view    = {0};
        mismatch_view.phase         = CONN_DISCONNECTED;
        mismatch_view.show_mismatch = true;
        mismatch_view.fingerprint   = "SHA256:BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &mismatch_view, &form, &intents, &rv, &rf, &ri);
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
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri);
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
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri);
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
        pw_form.auth     = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = false;

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri);
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
        pw_form.auth     = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = true;

        UiConnView pw_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents, &rv, &rf, &ri);
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
        pw_form.auth     = SSH_AUTH_PASSWORD;
        snprintf(pw_form.passkey, sizeof(pw_form.passkey), "s3cr3t");
        pw_form.remember = true;

        UiConnView connecting_view = {0};
        connecting_view.phase      = CONN_CONNECTING;
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &connecting_view, &pw_form, &intents, &rv, &rf, &ri);
            assert(!intents.breach);
            assert(!intents.abort); /* no keyboard Escape was pressed */
        }
        assert(pw_form.remember); /* disabled checkbox must not clear the flag */
    }

    /* State-based test: ONLINE view (bar phase) emits no spurious close/update. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!intents.close);
            assert(!intents.update);
            assert(!intents.breach);
        }
    }

    /* State-based test: REACQUIRING view (bar phase) emits no spurious intents. */
    {
        UiConnView reacq_view = {0};
        reacq_view.phase     = CONN_REACQUIRING;
        reacq_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &reacq_view, &form, &intents, &rv, &rf, &ri);
            assert(!intents.close);
            assert(!intents.update);
        }
    }

    /* State-based test: bar spacing — ONLINE and REACQUIRING both render across
     * multiple frames without crash and emit no spurious intents (guards the
     * bar_h / v_pad sizing change in draw_conn_bar). */
    {
        const char *hosts[] = {"alice@mac.local", "bob@192.168.1.10", ""};
        ConnPhase   phases[] = {CONN_ONLINE, CONN_REACQUIRING};

        for (int pi = 0; pi < 2; pi++) {
            for (int hi = 0; hi < 3; hi++) {
                UiConnView bar_view = {0};
                bar_view.phase     = phases[pi];
                bar_view.user_host = hosts[hi];

                for (int i = 0; i < 5; i++) {
                    UiIntents    intents = {0};
                    UiReconView  rv      = make_recon_view();
                    RunConfig    rf      = {0};
                    UiReconIntents ri    = make_recon_intents();
                    intents.select_host  = -1;
                    ui_frame(ui, &bar_view, &form, &intents, &rv, &rf, &ri);
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
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &agent_view, &agent_form, &intents, &rv, &rf, &ri);
            assert(!intents.breach);
            assert(!intents.abort);
        }
    }

    /* State-based test: overlay with SSH_AUTH_PASSWORD (HOST/PORT/USER/PASSKEY nav
     * fields) renders without crash and emits no spurious intents (nav enabled). */
    {
        ConnForm pw_nav_form = {0};
        pw_nav_form.selected_known_host = -1;
        snprintf(pw_nav_form.host,    sizeof(pw_nav_form.host),    "mac.local");
        snprintf(pw_nav_form.user,    sizeof(pw_nav_form.user),    "bob");
        snprintf(pw_nav_form.port,    sizeof(pw_nav_form.port),    "22");
        snprintf(pw_nav_form.passkey, sizeof(pw_nav_form.passkey), "hunter2");
        pw_nav_form.auth = SSH_AUTH_PASSWORD;

        UiConnView pw_nav_view = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &pw_nav_view, &pw_nav_form, &intents, &rv, &rf, &ri);
            assert(!intents.breach);
            assert(!intents.abort);
        }
    }

    /* State-based test: ONLINE + overlay_open (UPDATE mode) emits no spurious breach. */
    {
        UiConnView update_view = {0};
        update_view.phase        = CONN_ONLINE;
        update_view.user_host    = "alice@mac.local";
        update_view.overlay_open = true;

        ConnForm update_form = {0};
        update_form.selected_known_host = -1;
        snprintf(update_form.host, sizeof(update_form.host), "mac.local");
        snprintf(update_form.user, sizeof(update_form.user), "alice");
        snprintf(update_form.port, sizeof(update_form.port), "22");

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &update_view, &update_form, &intents, &rv, &rf, &ri);
            assert(!intents.breach);
            assert(!intents.close);
        }
    }

    /* ── Recon panel state-based tests ──────────────────────────────── */

    /* State-based test: ONLINE with empty recon view (no scan yet) renders
     * without crash and emits no spurious scan/abort_scan. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!ri.scan);
            assert(!ri.abort_scan);
            assert(ri.pick_blueprint == -1);
        }
    }

    /* State-based test: ONLINE with scan_done=true, empty blueprints renders
     * NO_BLUEPRINTS state without crash and emits no spurious pick. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            rv.scan_done         = true;
            rv.scan_err          = DISC_OK;
            rv.blueprints        = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!ri.scan);
            assert(ri.pick_blueprint == -1);
        }
    }

    /* State-based test: ONLINE with populated blueprint list renders
     * BLUEPRINTS_RECOVERED without crash and emits no spurious picks. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        Blueprint bp_items[2] = {0};
        snprintf(bp_items[0].path, sizeof(bp_items[0].path),
                 "/Users/alice/App/App.xcworkspace");
        bp_items[0].is_workspace = true;
        snprintf(bp_items[1].path, sizeof(bp_items[1].path),
                 "/Users/alice/Lib/Lib.xcodeproj");
        bp_items[1].is_workspace = false;
        BlueprintList bp_list = { .items = bp_items, .count = 2 };

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            rv.scan_done         = true;
            rv.scan_err          = DISC_OK;
            rv.blueprints        = &bp_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(ri.pick_blueprint == -1); /* no user interaction */
        }
    }

    /* State-based test: ONLINE with scanning=true renders ABORT SCAN button
     * without crash and emits no spurious abort_scan without user input. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            rv.scanning          = true;
            snprintf(rf.scan_root, sizeof(rf.scan_root),
                     "/Users/alice/Developer");
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!ri.abort_scan); /* no keyboard/mouse */
            assert(!ri.scan);
        }
    }

    /* State-based test: ONLINE with DISC_ERR_XCODE_MISSING renders failure
     * state without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            rv.scan_done         = true;
            rv.scan_err          = DISC_ERR_XCODE_MISSING;
            rv.blueprints        = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!ri.scan);
        }
    }

    /* State-based test: ONLINE with DISC_ERR_COMMAND_FAILED renders failure
     * state without crash. */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        BlueprintList empty_list = {0};
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconView  rv      = make_recon_view();
            RunConfig    rf      = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            rv.scan_done         = true;
            rv.scan_err          = DISC_ERR_COMMAND_FAILED;
            rv.blueprints        = &empty_list;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            assert(!ri.scan);
        }
    }

    /* State-based test: scan_root field persists across frames without
     * corruption (manual edit state is stable). */
    {
        UiConnView online_view = {0};
        online_view.phase     = CONN_ONLINE;
        online_view.user_host = "alice@mac.local";

        UiReconView  rv = make_recon_view();
        RunConfig    rf = {0};
        snprintf(rf.scan_root, sizeof(rf.scan_root), "/Users/alice/Developer");
        for (int i = 0; i < 3; i++) {
            UiIntents    intents = {0};
            UiReconIntents ri    = make_recon_intents();
            intents.select_host  = -1;
            ui_frame(ui, &online_view, &form, &intents, &rv, &rf, &ri);
            /* scan_root must not be corrupted by rendering */
            assert(strcmp(rf.scan_root, "/Users/alice/Developer") == 0);
        }
    }

    ui_shutdown(ui);
    arena_destroy(a);
    printf("ui_test: ok\n");
    return 0;
}
