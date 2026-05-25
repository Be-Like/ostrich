#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "arena.h"
#include "connstate.h"
#include "discovery.h"
#include "logbuf.h"
#include "runstate.h"
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
    bool        overlay_open;       /* UPDATE: show overlay during bar phase */
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

/* Read-only recon view-model the app builds each frame.
   Fields for slices B–D are zero/NULL until those slices land. */
typedef struct {
    /* slice A — scan + blueprints */
    bool              scanning;           /* scan in progress             */
    bool              scan_done;          /* at least one scan completed  */
    DiscStatus        scan_err;           /* meaningful when scan_done    */
    const BlueprintList *blueprints;      /* NULL until first scan        */
    int               blueprint_selected; /* -1 = none                    */

    /* slice B — scheme / config / bundle-id (stubs until task 10) */
    bool              reading_blueprint;
    bool              resolving_bundle_id;
    DiscStatus        blueprint_err;
    const StrList    *schemes;
    const StrList    *configs;

    /* slice C — presets (stub until task 11) */
    const PresetList *presets;
    int               preset_selected;   /* -1 = none                    */

    /* slice D — targets + READY (stub until task 12) */
    bool              sweeping;
    bool              sweep_done;
    DiscStatus        sweep_err;
    const TargetList *targets;
    int               target_selected;   /* -1 = none                    */

    /* readiness */
    Readiness         readiness;
} UiReconView;

/* Discrete recon intents returned by ui_frame each frame.
   Slice B–D fields are set to -1 / false until those slices land. */
typedef struct {
    /* slice A */
    bool scan;
    bool abort_scan;
    int  pick_blueprint;  /* -1 = no pick; >=0 = index chosen             */

    /* slice B (stubs) */
    bool scheme_edited;
    bool config_edited;
    bool bundle_id_edited;

    /* slice C (stubs) */
    bool preset_new;
    bool preset_rename;
    bool preset_delete;
    int  pick_preset;     /* -1 = no pick                                  */
    char preset_name[64]; /* name for new/renamed preset                   */

    /* slice D (stubs) */
    bool sweep;
    int  pick_target;     /* -1 = no pick                                  */
} UiReconIntents;

/* Read-only run view-model the app builds each frame. */
typedef struct {
    RunPhase  phase;      /* mirrored run phase                            */
    bool      stale;      /* built_gen > deployed_gen while running        */
    Readiness readiness;  /* for EXECUTE/COMPILE enablement                */
    LogBuf   *build_log;  /* Build Log buffer (never NULL after app init)  */
    LogBuf   *device_log; /* Device Log buffer (T9; may be NULL for now)   */
} UiRunView;

/* Discrete run intents returned by ui_frame each frame. */
typedef struct {
    bool execute;          /* request EXECUTE (or terminate-first re-exec) */
    bool compile;          /* request build-only COMPILE                   */
    bool abort_run;        /* request ABORT (universal stop)               */
    bool build_log_copy;   /* user pressed COPY on Build Log               */
    bool build_log_clear;  /* user pressed CLEAR on Build Log              */
    bool device_log_copy;  /* (T9) user pressed COPY on Device Log         */
    bool device_log_clear; /* (T9) user pressed CLEAR on Device Log        */
} UiRunIntents;

/* Stand up window + GL + ImGui + theme + fonts. Allocates the
   Ui handle and font bytes from `a`. */
UiStatus ui_init(Arena *a, UiOptions opts, Ui **out);

/* Render exactly one frame. Returns false when the window
   should close (close button or Ctrl-Q), true otherwise.
   Writes discrete intents to *out (zeroed then filled).
   rv/rf/ri handle the recon panel; rv may be NULL (no panel).
   rrv/rri handle the run panel; rrv may be NULL (no panel). */
bool ui_frame(Ui *ui,
              const UiConnView *cv, ConnForm *cf, UiIntents *ci,
              const UiReconView *rv, RunConfig *rf, UiReconIntents *ri,
              const UiRunView *rrv, UiRunIntents *rri);

/* Tear down ImGui, the GL context, and GLFW cleanly. */
void ui_shutdown(Ui *ui);

/* Human-readable reason for a UiStatus, for the UI/CLI. */
const char *ui_status_str(UiStatus st);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
