#ifndef SESSION_H
#define SESSION_H

#include "connstate.h"
#include "ssh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Session Session;

typedef enum {
    CMD_BREACH,
    CMD_ABORT,
    CMD_CLOSE,
    CMD_TRUST,
    CMD_DECLINE
} SessionCmdKind;

typedef struct {
    SessionCmdKind kind;
    SshConfig      cfg;  /* valid for CMD_BREACH */
} SessionCmd;

typedef struct {
    ConnPhase phase;
    SshStatus reason;            /* meaningful on failure */
    bool      hostkey_unknown;
    bool      hostkey_mismatch;
    char      user_host[384];
    char      fingerprint[128];
} SessionEvent;

/* Allocate the control block and spawn the worker thread.
   Uses malloc (flagged non-arena allocation — cross-thread lifetime).
   Returns SSH_OK on success; *out is only valid then. */
SshStatus   session_open(Session **out);

/* Submit a command from the UI thread to the worker.
   Writes a wakeup byte to the self-pipe.
   Returns false if the command ring is full. */
bool        session_submit(Session *s, const SessionCmd *cmd);

/* Drain one event from the worker. Returns false when the ring is empty. */
bool        session_poll(Session *s, SessionEvent *out);

/* Signal shutdown, join the worker thread, and free all resources. */
void        session_close(Session *s);

const char *session_status_str(SshStatus st);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
