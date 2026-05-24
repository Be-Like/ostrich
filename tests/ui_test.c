#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "arena.h"
#include "ui.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

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
        UiIntents intents = {0};
        int keep_going = ui_frame(ui, &view, &form, &intents);
        (void)keep_going;
        /* Resting view emits no action intents. */
        assert(!intents.breach);
        assert(!intents.abort);
        assert(!intents.close);
        assert(intents.select_host == -1);
    }

    /* State-based test: TOFU prompt (AWAITING_HOSTKEY) emits no spurious trust/decline. */
    {
        UiConnView tofu_view          = {0};
        tofu_view.phase               = CONN_AWAITING_HOSTKEY;
        tofu_view.show_hostkey_prompt = true;
        tofu_view.fingerprint         = "SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        for (int i = 0; i < 3; i++) {
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &tofu_view, &form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &mismatch_view, &form, &intents);
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
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents);
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
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &pw_view, &pw_form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &connecting_view, &pw_form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &online_view, &form, &intents);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &reacq_view, &form, &intents);
            assert(!intents.close);
            assert(!intents.update);
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
            UiIntents intents = {0};
            intents.select_host = -1;
            ui_frame(ui, &update_view, &update_form, &intents);
            assert(!intents.breach);
            assert(!intents.close);
        }
    }

    ui_shutdown(ui);
    arena_destroy(a);
    printf("ui_test: ok\n");
    return 0;
}
