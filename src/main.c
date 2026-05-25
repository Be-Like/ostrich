#include <stdlib.h>
#include <unistd.h>

#include "arena.h"
#include "app.h"
#include "log.h"

#define APP_ARENA_BYTES (8 * 1024 * 1024)

int main(void) {
    /* Must run before any thread spawns; no-op in release builds. */
    log_init();
    LOG_INFO(LG_APP, "start pid=%d build=%s %s", (int)getpid(), __DATE__, __TIME__);

    Arena *a = arena_create(APP_ARENA_BYTES);
    if (!a) {
        log_shutdown();
        return EXIT_FAILURE;
    }

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
        log_shutdown();
        return EXIT_FAILURE;
    }

    while (app_tick(app)) { }

    app_shutdown(app);
    arena_destroy(a);
    log_shutdown();
    return EXIT_SUCCESS;
}
