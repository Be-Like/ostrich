#include "app.h"
#include "arena.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>

struct App {
    Ui      *ui;
    ConnForm form;
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

    App *app = arena_alloc(a, sizeof(App), _Alignof(App));
    if (!app) {
        ui_shutdown(ui);
        return APP_ERR;
    }
    memset(app, 0, sizeof(*app));
    app->ui                       = ui;
    app->form.selected_known_host = -1;

    *out = app;
    return APP_OK;
}

bool app_tick(App *app) {
    UiConnView view    = {0};
    UiIntents  intents = {0};
    return ui_frame(app->ui, &view, &app->form, &intents);
}

void app_shutdown(App *app) {
    ui_shutdown(app->ui);
}
