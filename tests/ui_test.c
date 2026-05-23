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

    ui_shutdown(ui);
    arena_destroy(a);
    printf("ui_test: ok\n");
    return 0;
}
