#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include "arena.h"
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct App App;

typedef struct {
    const char *title;
    int         width, height;
    const char *font_dir;
    bool        headless;
} AppOptions;

typedef enum {
    APP_OK = 0,
    APP_ERR
} AppStatus;

/* Stand up the app: init UI, zero the ConnForm. */
AppStatus app_init(Arena *a, AppOptions opts, App **out);

/* Pump one frame: build the view-model, call ui_frame, handle intents.
   Returns false when the window should close. */
bool app_tick(App *app);

/* Tear down the app and all subsystems. */
void app_shutdown(App *app);

#ifdef __cplusplus
}
#endif

#endif /* APP_H */
