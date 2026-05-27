#ifndef SESSION_H
#define SESSION_H

#include "connstate.h"
#include "discovery.h"
#include "runstate.h"
#include "ssh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Session Session;

/* ── connection commands / events ─────────────────────────────────── */

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

/* ── discovery commands / events ──────────────────────────────────── */

typedef enum {
    DCMD_SCAN_HOST,
    DCMD_ABORT_SCAN,
    DCMD_READ_BLUEPRINT,    /* xcodebuild -list for a chosen project    */
    DCMD_RESOLVE_BUNDLE_ID, /* xcodebuild -showBuildSettings for bundle */
    DCMD_SWEEP_TARGETS      /* devicectl + simctl concurrently          */
} SessionDiscCmdKind;

typedef struct {
    SessionDiscCmdKind kind;
    char               root[1024];    /* DCMD_SCAN_HOST: directory to scan          */
    int                max_depth;     /* DCMD_SCAN_HOST: depth cap (0 → 8)          */
    char               project[1024]; /* DCMD_READ_BLUEPRINT / DCMD_RESOLVE_BUNDLE_ID */
    char               scheme[256];   /* DCMD_RESOLVE_BUNDLE_ID                     */
    char               config[128];   /* DCMD_RESOLVE_BUNDLE_ID                     */
} SessionDiscCmd;

typedef enum {
    DEV_BLUEPRINT,              /* one curated project path                      */
    DEV_SCAN_COMPLETE,          /* scan ended cleanly; count = total found        */
    DEV_SCAN_FAILED,            /* scan ended with error; disc_status set         */
    DEV_SCHEME,                 /* one scheme string from xcodebuild -list        */
    DEV_CONFIG,                 /* one config string from xcodebuild -list        */
    DEV_BLUEPRINT_READ_COMPLETE,/* -list job succeeded; count = schemes+configs   */
    DEV_BLUEPRINT_FAILED,       /* -list job failed; disc_status set              */
    DEV_BUNDLE_ID,              /* resolved PRODUCT_BUNDLE_IDENTIFIER             */
    DEV_BUNDLE_ID_FAILED,       /* bundle id resolve failed; disc_status set      */
    DEV_TARGET,                 /* one device or simulator                        */
    DEV_SWEEP_COMPLETE,         /* both jobs done; count = total targets found    */
    DEV_SWEEP_FAILED            /* sweep ended with error; disc_status set        */
} SessionDiscEventKind;

typedef struct {
    SessionDiscEventKind kind;
    union {
        Blueprint  blueprint;      /* DEV_BLUEPRINT                               */
        int        count;          /* DEV_SCAN_COMPLETE / DEV_BLUEPRINT_READ_COMPLETE / DEV_SWEEP_COMPLETE */
        DiscStatus disc_status;    /* DEV_SCAN_FAILED / DEV_BLUEPRINT_FAILED / DEV_BUNDLE_ID_FAILED / DEV_SWEEP_FAILED */
        char       scheme[256];    /* DEV_SCHEME                                  */
        char       config[128];    /* DEV_CONFIG                                  */
        char       bundle_id[256]; /* DEV_BUNDLE_ID                               */
        Target     target;         /* DEV_TARGET                                  */
    };
} SessionDiscEvent;

/* ── run command/event family ─────────────────────────────────────── */

#define RUN_CHUNK_CAP       4096
#define RUN_CMD_SUMMARY_CAP  256

typedef enum {
    RCMD_EXECUTE,
    RCMD_COMPILE,
    RCMD_ABORT,
    RCMD_SET_KC_PASS   /* writes WorkerCtx.kc_pass; clears on empty string */
} SessionRunCmdKind;

typedef struct {
    SessionRunCmdKind kind;
    RunConfig         cfg;
    Target            target;
    bool              has_target;
    char              kc_pass[256];   /* valid for RCMD_SET_KC_PASS */
} SessionRunCmd;

typedef enum {
    REV_PHASE,
    REV_BUILD_LOG,
    REV_DEVICE_LOG,
    REV_STALE,
    REV_BUILD_MARK
} SessionRunEventKind;

typedef struct {
    SessionRunEventKind kind;
    RunPhase            phase;
    BdStatus            reason;
    bool                stale;
    int                 len;
    char                chunk[RUN_CHUNK_CAP];
    char                cmd_summary[RUN_CMD_SUMMARY_CAP];
} SessionRunEvent;

bool session_run_submit  (Session *s, const SessionRunCmd *cmd);
bool session_run_poll    (Session *s, SessionRunEvent *out);

/* Enqueue RCMD_SET_KC_PASS with the given passkey (or clear with "").
   Must be called before session_run_submit(EXECUTE/COMPILE, …) to take
   effect on the next chain.  Returns false when the run command ring is
   full. */
bool session_set_kc_pass (Session *s, const char *kc_pass);

/* ── connection API ───────────────────────────────────────────────── */

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

/* ── discovery API ────────────────────────────────────────────────── */

/* Submit a discovery command from the UI thread.
   Only effective while the session is CONN_ONLINE.
   Returns false if the discovery command ring is full. */
bool        session_disc_submit(Session *s, const SessionDiscCmd *cmd);

/* Drain one discovery event from the worker.
   Returns false when the ring is empty. */
bool        session_disc_poll(Session *s, SessionDiscEvent *out);

#ifdef __cplusplus
}
#endif

#endif /* SESSION_H */
