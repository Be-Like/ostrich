#include <stdio.h>
#include <stdlib.h>

#include "arena.h"
#include "ui.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

int main(void) {
    Arena *app = arena_create(APP_ARENA_BYTES);
    if (!app) return EXIT_FAILURE;

    UiOptions opts = {
        .title    = "ostrich",
        .width    = 1280,
        .height   = 800,
        .font_dir = "assets/fonts",
        .headless = false,
    };

    Ui *ui = NULL;
    UiStatus st = ui_init(app, opts, &ui);
    if (st != UI_OK) {
        fprintf(stderr, "ostrich: %s\n", ui_status_str(st));
        arena_destroy(app);
        return EXIT_FAILURE;
    }

    while (ui_frame(ui)) { }

    ui_shutdown(ui);
    arena_destroy(app);
    return EXIT_SUCCESS;
}
