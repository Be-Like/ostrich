#include <stdlib.h>

#include "arena.h"
#include "app.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

int main(void) {
    Arena *a = arena_create(APP_ARENA_BYTES);
    if (!a) return EXIT_FAILURE;

    AppOptions opts = {
        .title    = "ostrich",
        .width    = 1280,
        .height   = 800,
        .font_dir = "assets/fonts",
        .headless = false,
    };

    App *app = NULL;
    if (app_init(a, opts, &app) != APP_OK) {
        arena_destroy(a);
        return EXIT_FAILURE;
    }

    while (app_tick(app)) { }

    app_shutdown(app);
    arena_destroy(a);
    return EXIT_SUCCESS;
}
