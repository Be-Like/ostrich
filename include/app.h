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

/* Pure form helpers — no I/O or threads; testable without a session. */
void app_form_to_ssh_config(const ConnForm *form, SshConfig *cfg);

/* Compute the overlay reason string from the current phase and last
   SSH error.  Returns lex(LEX_CONN_SEVERED) when SEVERED, the matching
   failure string when DISCONNECTED with a non-OK reason, and NULL
   otherwise (no reason line to show). */
const char *app_phase_reason(ConnPhase phase, SshStatus last_reason);

/* Copy a saved Conn's fields into the mutable form (host, port, user,
   auth, passkey, remember). Does not touch selected_known_host. */
void app_conn_to_form(const Conn *conn, ConnForm *form);

/* Upsert the current form into `list`.  If selected_idx is a valid
   index, update that slot (preserving its label) and set mru_index to
   it.  Otherwise append a new entry (auto-label "user@host") using `a`
   for the backing array.  Returns the mru_index of the affected entry,
   or -1 on OOM (append path only). */
int app_save_to_list(ConnList *list, const ConnForm *form,
                     int selected_idx, Arena *a);

/* Stand up the app: init UI, open the session, zero the ConnForm. */
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
