#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Ui Ui;

typedef enum {
    UI_OK = 0,
    UI_ERR_NO_DISPLAY, /* no reachable display server      */
    UI_ERR_GLFW,       /* glfwInit / window / GL ctx failed */
    UI_ERR_GL,         /* GL/loader init failed             */
    UI_ERR_FONT,       /* a vendored TTF was missing/bad    */
    UI_ERR_OOM         /* arena exhausted during init       */
} UiStatus;

typedef struct {
    const char *title;    /* window title ("ostrich")       */
    int width, height;    /* initial window size            */
    const char *font_dir; /* dir holding JetBrainsMono TTFs */
    bool headless;        /* hidden window (for ui_test)    */
} UiOptions;

/* Stand up window + GL + ImGui + theme + fonts. Allocates the
   Ui handle and font bytes from `a`. */
UiStatus ui_init(Arena *a, UiOptions opts, Ui **out);

/* Render exactly one frame. Returns false when the window
   should close (close button or Ctrl-Q), true otherwise. */
bool ui_frame(Ui *ui);

/* Tear down ImGui, the GL context, and GLFW cleanly. */
void ui_shutdown(Ui *ui);

/* Human-readable reason for a UiStatus, for the UI/CLI. */
const char *ui_status_str(UiStatus st);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
