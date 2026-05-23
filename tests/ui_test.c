#include <assert.h>
#include <stdio.h>

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

    /* Run a few frames to exercise the frame loop. */
    for (int i = 0; i < 3; i++) {
        int keep_going = ui_frame(ui);
        (void)keep_going;
    }

    ui_shutdown(ui);
    arena_destroy(a);
    printf("ui_test: ok\n");
    return 0;
}
