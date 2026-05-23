#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "arena.h"
#include "connstate.h"
#include "store.h"

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

/* Read-only view-model the app builds each frame. */
typedef struct {
    ConnPhase   phase;
    const char *user_host;          /* bar: "user@host"        */
    const char *reason;             /* failure line            */
    const char *fingerprint;        /* host-key prompt         */
    bool        show_hostkey_prompt; /* unknown host           */
    bool        show_mismatch;       /* security stop          */
    const Conn *known_hosts;
    int         known_count;
} UiConnView;

/* Mutable form the user edits; app owns it across frames. */
typedef struct {
    char    host[256];
    char    port[8];
    char    user[128];
    char    passkey[256];
    SshAuth auth;
    bool    remember;
    int     selected_known_host; /* -1 = none */
} ConnForm;

/* Discrete intents returned by ui_frame each frame. */
typedef struct {
    bool breach, abort, close, update, save, trust, decline;
    int  select_host; /* -1 = none */
} UiIntents;

/* Stand up window + GL + ImGui + theme + fonts. Allocates the
   Ui handle and font bytes from `a`. */
UiStatus ui_init(Arena *a, UiOptions opts, Ui **out);

/* Render exactly one frame. Returns false when the window
   should close (close button or Ctrl-Q), true otherwise.
   Writes discrete intents to *out (zeroed then filled). */
bool ui_frame(Ui *ui, const UiConnView *view,
              ConnForm *form, UiIntents *out);

/* Tear down ImGui, the GL context, and GLFW cleanly. */
void ui_shutdown(Ui *ui);

/* Human-readable reason for a UiStatus, for the UI/CLI. */
const char *ui_status_str(UiStatus st);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
