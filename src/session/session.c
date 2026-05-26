#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "arena.h"
#include "builddeploy.h"
#include "connstate.h"
#include "discovery.h"
#include "log.h"
#include "runstate.h"
#include "spsc_ring.h"
#include "ssh.h"

#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── constants ────────────────────────────────────────────────────── */

#define CMD_RING_CAP            16
#define EVENT_RING_CAP          32
#define DISC_CMD_RING_CAP        8
#define DISC_EVENT_RING_CAP    128
#define WORKER_ARENA_SZ        (512 * 1024)
#define CONNECT_TIMEOUT_SEC    30.0
#define KEEPALIVE_INTERVAL_SEC 30

/* ── run subsystem constants ──────────────────────────────────────── */

#define RUN_CMD_RING_CAP         8
#define RUN_EVENT_RING_CAP      64
#define RUN_ARENA_SZ        (256 * 1024)
#define RUN_SETTINGS_BUF_CAP (256 * 1024)
#define RUN_READ_CHUNK       4096
#ifdef RUN_STALL_SEC_OVERRIDE
#define RUN_STALL_SEC RUN_STALL_SEC_OVERRIDE
#else
#define RUN_STALL_SEC 120.0
#endif
#define RUN_CHAIN_SLOT       DISC_MAX_JOBS       /* = 4, for open_owner seam */
#define RUN_ABORT_SLOT       (DISC_MAX_JOBS + 1) /* = 5, for open_owner seam */

/* ── discovery job engine constants ───────────────────────────────── */

#define DISC_MAX_JOBS      4
#define DISC_JOB_ARENA_SZ  (1024 * 1024)  /* 1 MB per job arena   */
#define DISC_JOB_BUF_CAP   (512 * 1024)   /* 512 KB output buffer */
#define DISC_READ_CHUNK    4096
#define DISC_SCAN_DEPTH_DEFAULT 8
#define DISC_JOB_TIMEOUT_SEC    60.0      /* fail a stuck remote command */

/* ── Session control block (flagged malloc) ──────────────────────── */

struct Session {
    SpscRing  *cmd_ring;         /* UI→worker: SessionCmd records      */
    SpscRing  *event_ring;       /* worker→UI: SessionEvent records    */
    SpscRing  *disc_cmd_ring;    /* UI→worker: SessionDiscCmd records  */
    SpscRing  *disc_event_ring;  /* worker→UI: SessionDiscEvent records */
    SpscRing  *run_cmd_ring;     /* UI→worker: SessionRunCmd records   */
    SpscRing  *run_event_ring;   /* worker→UI: SessionRunEvent records */
    int        pipe_read;        /* worker reads wakeup bytes          */
    int        pipe_write;       /* UI writes to wake worker           */
    pthread_t  thread;
    atomic_int running;          /* 0 = worker must stop               */
};

/* ── discovery job types (worker-private) ─────────────────────────── */

typedef enum {
    DJOB_KIND_NONE,
    DJOB_KIND_SCAN,
    DJOB_KIND_READ_BLUEPRINT,
    DJOB_KIND_RESOLVE_BUNDLE_ID,
    DJOB_KIND_SWEEP_DEVICECTL,  /* devicectl half of a sweep */
    DJOB_KIND_SWEEP_SIMCTL,     /* simctl half of a sweep    */
} DiscJobKind;

typedef enum {
    DJOB_OPEN,  /* opening the SSH channel       */
    DJOB_EXEC,  /* starting remote command       */
    DJOB_READ,  /* accumulating channel output   */
    DJOB_EXIT,  /* collecting exit code          */
    DJOB_EMIT,  /* emitting curated results      */
} DiscJobState;

typedef struct {
    DiscJobKind   kind;
    DiscJobState  state;
    SshChannel   *ch;
    Arena        *arena;       /* from disc_arenas pool; reset on done */
    char         *buf;         /* output accumulation (arena-alloc'd) */
    size_t        buf_len;
    /* per-job watchdog: fail if the remote command never finishes */
    struct timespec deadline;
    bool          has_deadline;
#ifdef OSTRICH_DEBUG
    struct timespec exec_start; /* wall-clock when DJOB_EXEC succeeded  */
#endif
    char          cmd[2048];   /* remote command string                */
    /* scan-specific */
    int           scan_depth;
    /* emit phase (shared index across all streaming kinds) */
    int           emit_idx;
    /* DJOB_KIND_SCAN emit */
    BlueprintList curated;
    /* DJOB_KIND_READ_BLUEPRINT emit */
    StrList       schemes;
    StrList       configs;
    /* DJOB_KIND_RESOLVE_BUNDLE_ID result */
    char          bundle_id[256];
    /* DJOB_KIND_SWEEP_* emit data */
    TargetList    targets;
} DiscJob;

/* ── run chain types (worker-private) ────────────────────────────── */

typedef enum {
    RCHAIN_STEP_SETTINGS,
    RCHAIN_STEP_BUILD,
    RCHAIN_STEP_PRIME_BOOT,
    RCHAIN_STEP_PRIME_STATUS,
    RCHAIN_STEP_INSTALL,
    RCHAIN_STEP_LAUNCH,
} RChainStep;

typedef enum {
    RCHAIN_OPEN,
    RCHAIN_EXEC,
    RCHAIN_READ,
    RCHAIN_EXIT,
} RChainState;

typedef struct {
    bool        active;
    RChainStep  step;
    RChainState state;
    SshChannel *ch;
    RunConfig   cfg;
    Target      target;
    bool        has_target;
    bool        compile_only;
    char       *settings_buf;  /* arena-allocated; NULL when not accumulating */
    size_t      settings_len;
    char        app_path[1024]; /* parsed product path */
    long        build_pgid;     /* parsed from PID marker (for ABORT in T6) */
    struct timespec stall_deadline;
    bool            has_stall_deadline;
} RunChain;

typedef enum {
    DCON_IDLE,
    DCON_STREAMING,
    DCON_EXIT,
} DevConState;

typedef struct {
    DevConState  state;
    SshChannel  *ch;
    char         bundle_id[256];
    char         udid[128];
    bool         is_sim;
} DevConsole;

/* ── run abort types (worker-private) ────────────────────────────── */

typedef enum {
    RABORT_IDLE,
    RABORT_TERM_OPEN,  /* opening channel for terminate */
    RABORT_TERM_EXEC,  /* exec terminate (may SSH_AGAIN) */
    RABORT_KILL_OPEN,  /* opening channel for kill */
    RABORT_KILL_EXEC,  /* exec kill (may SSH_AGAIN) */
    RABORT_COMPLETE,   /* emit aborted or start pending chain */
} RunAbortState;

typedef struct {
    RunAbortState state;
    SshChannel   *ch;
    bool          need_term;
    bool          need_kill;
    char          term_cmd[1024];
    char          kill_cmd[256];
    bool          is_reexecute;   /* terminate-first: start new chain on complete */
    SessionRunCmd pending;        /* saved execute cmd for terminate-first */
} RunAbort;

/* ── worker-private sub-phase ─────────────────────────────────────── */

typedef enum {
    SUB_IDLE,
    SUB_HANDSHAKE,
    SUB_AWAIT_HOSTKEY,
    SUB_AUTH,
    SUB_PROBE,
    SUB_ONLINE,
    SUB_BACKOFF,
} SubPhase;

typedef struct {
    Session         *s;
    Arena           *arena;
    Ssh             *ssh;
    int              ssh_fd;
    ConnState        cs;
    SubPhase         sub;
    SshConfig        cfg;
    char             fingerprint[128];
    struct timespec  deadline;
    bool             has_deadline;
    int              keepalive_next;
    /* discovery job engine */
    DiscJob          disc_jobs[DISC_MAX_JOBS];
    Arena           *disc_arenas[DISC_MAX_JOBS];
    int              open_owner; /* job index holding the channel-open seam; -1 = free */
    /* sweep group tracking (one sweep at a time) */
    int              sweep_group_remaining; /* sweep jobs still in flight */
    bool             sweep_group_failed;    /* SWEEP_FAILED already emitted */
    int              sweep_total;           /* DEV_TARGET events emitted so far */
    /* run subsystem */
    RunChain         run_chain;
    DevConsole       dev_console;
    RunAbort         run_abort;
    RunState         rs;
    Arena           *run_arena;
} WorkerCtx;

/* ── time helpers ─────────────────────────────────────────────────── */

static struct timespec mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static struct timespec deadline_in(double sec)
{
    struct timespec dl = mono_now();
    long s  = (long)sec;
    long ns = (long)((sec - (double)s) * 1e9);
    dl.tv_sec  += s;
    dl.tv_nsec += ns;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    return dl;
}

static int ms_until(const struct timespec *dl)
{
    struct timespec now = mono_now();
    long ms = (dl->tv_sec  - now.tv_sec)  * 1000L
            + (dl->tv_nsec - now.tv_nsec) / 1000000L;
    return (ms > 0) ? (int)ms : 0;
}

static bool deadline_past(const struct timespec *dl)
{
    return ms_until(dl) == 0;
}

/* ── emit helpers ─────────────────────────────────────────────────── */

static void emit_ev(WorkerCtx *ctx, ConnPhase phase, SshStatus reason,
                    bool hk_unknown, bool hk_mismatch)
{
    SessionEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.phase            = phase;
    ev.reason           = reason;
    ev.hostkey_unknown  = hk_unknown;
    ev.hostkey_mismatch = hk_mismatch;
    if (ctx->cfg.user[0] || ctx->cfg.host[0]) {
        snprintf(ev.user_host, sizeof(ev.user_host),
                 "%s@%s", ctx->cfg.user, ctx->cfg.host);
    }
    if (hk_unknown || hk_mismatch)
        memcpy(ev.fingerprint, ctx->fingerprint, sizeof(ev.fingerprint));
    spsc_push(ctx->s->event_ring, &ev);

    LOG_INFO(LG_CONN, "phase → %s%s%s reason=%s",
             connstate_phase_str(phase),
             hk_unknown  ? " [unknown-hostkey]"  : "",
             hk_mismatch ? " [hostkey-mismatch]" : "",
             ssh_status_str(reason));
}

/* Push a disc event; spin briefly until space (UI drains each frame). */
static void push_disc_ev(WorkerCtx *ctx, const SessionDiscEvent *ev)
{
    while (!spsc_push(ctx->s->disc_event_ring, ev))
        ;
}

/* ── run subsystem helpers ────────────────────────────────────────── */

static void push_run_ev(WorkerCtx *ctx, const SessionRunEvent *ev)
{
    while (!spsc_push(ctx->s->run_event_ring, ev))
        ;
}

static void emit_run_phase(WorkerCtx *ctx, RunPhase phase, BdStatus reason)
{
    SessionRunEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind   = REV_PHASE;
    ev.phase  = phase;
    ev.reason = reason;
    push_run_ev(ctx, &ev);
    LOG_INFO(LG_RUN, "phase → %s reason=%s",
             runstate_phase_str(phase), bd_status_str(reason));
}

static void emit_build_log(WorkerCtx *ctx, const char *bytes, size_t n)
{
    while (n > 0) {
        size_t chunk = n < RUN_CHUNK_CAP ? n : (size_t)RUN_CHUNK_CAP;
        SessionRunEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = REV_BUILD_LOG;
        ev.len  = (int)chunk;
        memcpy(ev.chunk, bytes, chunk);
        push_run_ev(ctx, &ev);
        bytes += chunk;
        n     -= chunk;
    }
}

static void emit_build_mark(WorkerCtx *ctx, RunPhase phase, const char *cmd)
{
    SessionRunEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind  = REV_BUILD_MARK;
    ev.phase = phase;
    size_t n = strlen(cmd);
    if (n >= RUN_CMD_SUMMARY_CAP) n = RUN_CMD_SUMMARY_CAP - 1;
    memcpy(ev.cmd_summary, cmd, n);
    ev.cmd_summary[n] = '\0';
    push_run_ev(ctx, &ev);
}

static void emit_device_log(WorkerCtx *ctx, const char *bytes, size_t n)
{
    while (n > 0) {
        size_t chunk = n < RUN_CHUNK_CAP ? n : (size_t)RUN_CHUNK_CAP;
        SessionRunEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = REV_DEVICE_LOG;
        ev.len  = (int)chunk;
        memcpy(ev.chunk, bytes, chunk);
        push_run_ev(ctx, &ev);
        bytes += chunk;
        n     -= chunk;
    }
}

static void emit_stale(WorkerCtx *ctx, bool stale)
{
    SessionRunEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind  = REV_STALE;
    ev.stale = stale;
    push_run_ev(ctx, &ev);
    LOG_INFO(LG_RUN, "stale=%s built_gen=%d deployed_gen=%d",
             stale ? "true" : "false", ctx->rs.built_gen, ctx->rs.deployed_gen);
}

static RunEvent step_to_fail_ev(RChainStep step)
{
    switch (step) {
    case RCHAIN_STEP_SETTINGS:
    case RCHAIN_STEP_BUILD:        return RUN_EV_BUILD_FAIL;
    case RCHAIN_STEP_PRIME_BOOT:
    case RCHAIN_STEP_PRIME_STATUS: return RUN_EV_PRIME_FAIL;
    case RCHAIN_STEP_INSTALL:      return RUN_EV_INSTALL_FAIL;
    case RCHAIN_STEP_LAUNCH:       return RUN_EV_LAUNCH_FAIL;
    }
    return RUN_EV_BUILD_FAIL;
}

#ifdef OSTRICH_DEBUG
static const char *rchain_step_str(RChainStep step)
{
    switch (step) {
    case RCHAIN_STEP_SETTINGS:     return "settings";
    case RCHAIN_STEP_BUILD:        return "build";
    case RCHAIN_STEP_PRIME_BOOT:   return "prime-boot";
    case RCHAIN_STEP_PRIME_STATUS: return "prime-status";
    case RCHAIN_STEP_INSTALL:      return "install";
    case RCHAIN_STEP_LAUNCH:       return "launch";
    }
    return "?";
}
#endif

/* Start a RunChain without stepping the state machine (state already
   transitioned by the caller). */
static void start_run_chain(WorkerCtx *ctx, const SessionRunCmd *cmd, bool compile_only)
{
    arena_reset(ctx->run_arena);
    RunChain *rc = &ctx->run_chain;
    memset(rc, 0, sizeof(*rc));
    rc->cfg          = cmd->cfg;
    rc->target       = cmd->target;
    rc->has_target   = cmd->has_target;
    rc->compile_only = compile_only;
    ctx->rs.target_is_sim = (!compile_only) && cmd->has_target && cmd->target.is_simulator;
    rc->step   = RCHAIN_STEP_SETTINGS;
    rc->state  = RCHAIN_OPEN;
    rc->active = true;
}

/* Capture kill/terminate commands, tear down local channels, and start
   the abort state machine.  Called for both RCMD_ABORT and
   terminate-first re-EXECUTE (is_reexecute=true). */
static void start_run_abort(WorkerCtx *ctx, bool is_reexecute,
                             const SessionRunCmd *pending)
{
    RunAbort   *ra = &ctx->run_abort;
    RunChain   *rc = &ctx->run_chain;
    DevConsole *dc = &ctx->dev_console;

    memset(ra, 0, sizeof(*ra));
    ra->is_reexecute = is_reexecute;
    if (pending) ra->pending = *pending;

    /* Build kill command if RunChain has a parsed pgid. */
    if (rc->active && rc->build_pgid > 0) {
        if (bd_kill_cmd(rc->build_pgid, ra->kill_cmd, sizeof(ra->kill_cmd)) == BD_OK)
            ra->need_kill = true;
    }

    /* Build terminate command if DevConsole is active. */
    if (dc->state != DCON_IDLE) {
        Target tgt;
        memset(&tgt, 0, sizeof(tgt));
        memcpy(tgt.udid, dc->udid, sizeof(tgt.udid));
        tgt.is_simulator = dc->is_sim;
        if (bd_terminate_cmd(&tgt, dc->bundle_id, ra->term_cmd,
                             sizeof(ra->term_cmd)) == BD_OK)
            ra->need_term = true;
    }

    /* Tear down local channels immediately. */
    if (rc->active) {
        if (ctx->open_owner == RUN_CHAIN_SLOT) ctx->open_owner = -1;
        if (rc->ch) { ssh_channel_close(rc->ch); rc->ch = NULL; }
        arena_reset(ctx->run_arena);
        rc->settings_buf = NULL;
        rc->settings_len = 0;
        rc->active = false;
    }
    if (dc->state != DCON_IDLE) {
        if (dc->ch) { ssh_channel_close(dc->ch); dc->ch = NULL; }
        dc->state = DCON_IDLE;
    }

    ra->state = RABORT_TERM_OPEN;
    LOG_INFO(LG_RUN, "abort: start need_term=%d need_kill=%d is_reexecute=%d",
             ra->need_term, ra->need_kill, is_reexecute);
}

static void fail_run_chain(WorkerCtx *ctx, RunEvent ev, BdStatus reason)
{
    RunChain *rc = &ctx->run_chain;
    if (ctx->open_owner == RUN_CHAIN_SLOT) ctx->open_owner = -1;
    if (rc->ch) { ssh_channel_close(rc->ch); rc->ch = NULL; }
    arena_reset(ctx->run_arena);
    rc->settings_buf = NULL;
    rc->settings_len = 0;
    rc->active = false;

    RunAction act = runstate_step(&ctx->rs, ev);
    (void)act;
    emit_run_phase(ctx, ctx->rs.phase, reason);
    LOG_WARN(LG_RUN, "chain fail step=%s reason=%s",
             rchain_step_str(rc->step), bd_status_str(reason));
}

static void handle_step_exit(WorkerCtx *ctx, int exit_code)
{
    RunChain *rc = &ctx->run_chain;

    switch (rc->step) {
    case RCHAIN_STEP_SETTINGS: {
        if (exit_code != 0) {
            BdStatus r = (exit_code == 127) ? BD_ERR_XCODE_MISSING : BD_ERR_BUILD;
            fail_run_chain(ctx, RUN_EV_BUILD_FAIL, r);
            return;
        }
        Str raw = { rc->settings_buf, rc->settings_len };
        BdStatus bs = bd_parse_product_path(raw, rc->app_path, sizeof(rc->app_path));
        if (bs != BD_OK) {
            fail_run_chain(ctx, RUN_EV_BUILD_FAIL, BD_ERR_PARSE);
            return;
        }
        rc->settings_buf = NULL;
        rc->settings_len = 0;
        RunAction act = runstate_step(&ctx->rs, RUN_EV_SETTINGS_OK);
        (void)act;
        rc->step  = RCHAIN_STEP_BUILD;
        rc->state = RCHAIN_OPEN;
        break;
    }
    case RCHAIN_STEP_BUILD: {
        if (exit_code != 0 && rc->build_pgid == 0) {
            char block[2048];
            if (bd_setsid_help_block(ctx->cfg.user, ctx->cfg.host,
                                     ctx->cfg.port,
                                     block, sizeof(block)) == BD_OK) {
                emit_build_log(ctx, block, strlen(block));
            }
            fail_run_chain(ctx, RUN_EV_BUILD_FAIL, BD_ERR_SETSID_MISSING);
            return;
        }
        if (exit_code != 0) {
            BdStatus r = (exit_code == 127) ? BD_ERR_XCODE_MISSING : BD_ERR_BUILD;
            fail_run_chain(ctx, RUN_EV_BUILD_FAIL, r);
            return;
        }
        RunAction act = runstate_step(&ctx->rs, RUN_EV_BUILD_OK);
        if (act == RUN_ACT_DONE) {
            emit_run_phase(ctx, ctx->rs.phase, BD_OK);
            rc->active = false;
            if (runstate_stale(&ctx->rs))
                emit_stale(ctx, true);
            return;
        }
        emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        if (act == RUN_ACT_PRIME) {
            rc->step  = RCHAIN_STEP_PRIME_BOOT;
            rc->state = RCHAIN_OPEN;
        } else {
            rc->step  = RCHAIN_STEP_INSTALL;
            rc->state = RCHAIN_OPEN;
        }
        break;
    }
    case RCHAIN_STEP_PRIME_BOOT: {
        if (exit_code != 0) {
            fail_run_chain(ctx, RUN_EV_PRIME_FAIL, BD_ERR_BOOT);
            return;
        }
        rc->step  = RCHAIN_STEP_PRIME_STATUS;
        rc->state = RCHAIN_OPEN;
        break;
    }
    case RCHAIN_STEP_PRIME_STATUS: {
        if (exit_code != 0) {
            fail_run_chain(ctx, RUN_EV_PRIME_FAIL, BD_ERR_BOOT);
            return;
        }
        RunAction act = runstate_step(&ctx->rs, RUN_EV_PRIME_OK);
        (void)act;
        emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        rc->step  = RCHAIN_STEP_INSTALL;
        rc->state = RCHAIN_OPEN;
        break;
    }
    case RCHAIN_STEP_INSTALL: {
        if (exit_code != 0) {
            BdStatus r = (exit_code == 127) ? BD_ERR_XCODE_MISSING : BD_ERR_INSTALL;
            fail_run_chain(ctx, RUN_EV_INSTALL_FAIL, r);
            return;
        }
        RunAction act = runstate_step(&ctx->rs, RUN_EV_INSTALL_OK);
        (void)act;
        emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        rc->step  = RCHAIN_STEP_LAUNCH;
        rc->state = RCHAIN_OPEN;
        break;
    }
    case RCHAIN_STEP_LAUNCH:
        /* Handled at EXEC time via handoff */
        break;
    }
}

static void process_run_chunk(WorkerCtx *ctx, const char *buf, size_t n)
{
    RunChain *rc = &ctx->run_chain;
    switch (rc->step) {
    case RCHAIN_STEP_SETTINGS:
        emit_build_log(ctx, buf, n);
        if (rc->settings_buf && rc->settings_len + n <= RUN_SETTINGS_BUF_CAP) {
            memcpy(rc->settings_buf + rc->settings_len, buf, n);
            rc->settings_len += n;
        }
        break;
    case RCHAIN_STEP_BUILD:
        emit_build_log(ctx, buf, n);
        {
            Str chunk = { buf, n };
            long pgid = 0;
            if (bd_parse_pid_marker(chunk, &pgid) && pgid > 0) {
                rc->build_pgid = pgid;
                LOG_INFO(LG_RUN, "build pgid=%ld", pgid);
            }
        }
        break;
    case RCHAIN_STEP_PRIME_BOOT:
    case RCHAIN_STEP_PRIME_STATUS:
    case RCHAIN_STEP_INSTALL:
        emit_build_log(ctx, buf, n);
        break;
    case RCHAIN_STEP_LAUNCH:
        break;
    }
}

static BdStatus build_step_cmd(WorkerCtx *ctx, char *cmd, size_t cap)
{
    RunChain *rc = &ctx->run_chain;
    switch (rc->step) {
    case RCHAIN_STEP_SETTINGS:
        return bd_settings_cmd(&rc->cfg, &rc->target, rc->has_target, cmd, cap);
    case RCHAIN_STEP_BUILD:
        return bd_build_cmd(&rc->cfg, &rc->target, rc->has_target, cmd, cap);
    case RCHAIN_STEP_PRIME_BOOT:
        return bd_boot_cmd(&rc->target, cmd, cap);
    case RCHAIN_STEP_PRIME_STATUS:
        return bd_bootstatus_cmd(&rc->target, cmd, cap);
    case RCHAIN_STEP_INSTALL:
        return bd_install_cmd(&rc->target, rc->app_path, cmd, cap);
    case RCHAIN_STEP_LAUNCH:
        return bd_launch_cmd(&rc->target, rc->cfg.bundle_id, cmd, cap);
    }
    return BD_ERR_OOM;
}

static void drive_run_abort(WorkerCtx *ctx)
{
    RunAbort *ra = &ctx->run_abort;
    if (ra->state == RABORT_IDLE) return;

    bool again = true;
    while (again && ra->state != RABORT_IDLE) {
        again = false;

        switch (ra->state) {
        case RABORT_IDLE:
            return;

        case RABORT_TERM_OPEN:
            if (!ra->need_term) { ra->state = RABORT_KILL_OPEN; again = true; break; }
            if (ctx->open_owner != -1 && ctx->open_owner != RUN_ABORT_SLOT) return;
            ctx->open_owner = RUN_ABORT_SLOT;
            {
                SshStatus st = ssh_channel_open(ctx->ssh, &ra->ch);
                if (st == SSH_AGAIN) return;
                ctx->open_owner = -1;
                if (st != SSH_OK) {
                    ra->ch = NULL;
                    ra->state = RABORT_KILL_OPEN;
                    again = true;
                    break;
                }
            }
            ra->state = RABORT_TERM_EXEC;
            again = true;
            break;

        case RABORT_TERM_EXEC: {
            SshStatus st = ssh_channel_exec(ra->ch, ra->term_cmd);
            if (st == SSH_AGAIN) return;
            ssh_channel_close(ra->ch);
            ra->ch = NULL;
            LOG_INFO(LG_RUN, "abort: terminate exec'd");
            ra->state = RABORT_KILL_OPEN;
            again = true;
            break;
        }

        case RABORT_KILL_OPEN:
            if (!ra->need_kill) { ra->state = RABORT_COMPLETE; again = true; break; }
            if (ctx->open_owner != -1 && ctx->open_owner != RUN_ABORT_SLOT) return;
            ctx->open_owner = RUN_ABORT_SLOT;
            {
                SshStatus st = ssh_channel_open(ctx->ssh, &ra->ch);
                if (st == SSH_AGAIN) return;
                ctx->open_owner = -1;
                if (st != SSH_OK) {
                    ra->ch = NULL;
                    ra->state = RABORT_COMPLETE;
                    again = true;
                    break;
                }
            }
            ra->state = RABORT_KILL_EXEC;
            again = true;
            break;

        case RABORT_KILL_EXEC: {
            SshStatus st = ssh_channel_exec(ra->ch, ra->kill_cmd);
            if (st == SSH_AGAIN) return;
            ssh_channel_close(ra->ch);
            ra->ch = NULL;
            LOG_INFO(LG_RUN, "abort: kill exec'd");
            ra->state = RABORT_COMPLETE;
            again = true;
            break;
        }

        case RABORT_COMPLETE:
            ra->state = RABORT_IDLE;
            if (ra->is_reexecute) {
                /* Runstate already in BUILDING from handle_run_execute; just start chain. */
                start_run_chain(ctx, &ra->pending, false);
                LOG_INFO(LG_RUN, "abort: terminate-first complete, new chain starting");
            } else {
                RunAction act = runstate_step(&ctx->rs, RUN_EV_ABORT);
                (void)act;
                emit_run_phase(ctx, ctx->rs.phase, BD_OK);
                LOG_INFO(LG_RUN, "abort: resolved to aborted");
            }
            break;
        }
    }
}

static void drive_dev_console(WorkerCtx *ctx)
{
    DevConsole *dc = &ctx->dev_console;

again:
    switch (dc->state) {
    case DCON_IDLE:
        return;
    case DCON_STREAMING: {
        for (;;) {
            if (ssh_channel_eof(dc->ch)) {
                dc->state = DCON_EXIT;
                goto again;
            }
            char tmp[RUN_READ_CHUNK];
            size_t n = 0;
            SshStatus st = ssh_channel_read(dc->ch, tmp, sizeof(tmp), &n);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) {
                ssh_channel_close(dc->ch);
                dc->ch = NULL;
                dc->state = DCON_IDLE;
                RunAction act = runstate_step(&ctx->rs, RUN_EV_DROP);
                (void)act;
                emit_run_phase(ctx, ctx->rs.phase, BD_OK);
                LOG_WARN(LG_RUN, "dev-console io-error → aborted");
                return;
            }
            if (n == 0) return;
            emit_device_log(ctx, tmp, n);
        }
    }
    case DCON_EXIT: {
        int exit_code = -1;
        SshStatus st = ssh_channel_exit(dc->ch, &exit_code);
        if (st == SSH_AGAIN) return;
        ssh_channel_close(dc->ch);
        dc->ch = NULL;
        dc->state = DCON_IDLE;
        LOG_INFO(LG_RUN, "dev-console exit=%d", exit_code);
        if (exit_code != 0) {
            RunAction act = runstate_step(&ctx->rs, RUN_EV_LAUNCH_FAIL);
            (void)act;
            emit_run_phase(ctx, ctx->rs.phase, BD_ERR_LAUNCH);
        } else {
            RunAction act = runstate_step(&ctx->rs, RUN_EV_CONSOLE_EOF);
            (void)act;
            emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        }
        break;
    }
    }
}

static void drive_run_chain(WorkerCtx *ctx)
{
    RunChain *rc = &ctx->run_chain;
    if (!rc->active) return;

    bool again = true;
    while (again && rc->active) {
        again = false;

        switch (rc->state) {
        case RCHAIN_OPEN: {
            if (ctx->open_owner != -1 && ctx->open_owner != RUN_CHAIN_SLOT) return;
            ctx->open_owner = RUN_CHAIN_SLOT;
            SshStatus st = ssh_channel_open(ctx->ssh, &rc->ch);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) {
                ctx->open_owner = -1;
                fail_run_chain(ctx, step_to_fail_ev(rc->step), BD_ERR_BUILD);
                return;
            }
            ctx->open_owner = -1;
            rc->state = RCHAIN_EXEC;
            again = true;
            break;
        }
        case RCHAIN_EXEC: {
            /* Allocate settings buffer on first exec of the settings step */
            if (rc->step == RCHAIN_STEP_SETTINGS && !rc->settings_buf) {
                rc->settings_buf = arena_alloc(ctx->run_arena, RUN_SETTINGS_BUF_CAP, 1);
                if (!rc->settings_buf) {
                    fail_run_chain(ctx, RUN_EV_BUILD_FAIL, BD_ERR_OOM);
                    return;
                }
                rc->settings_len = 0;
            }

            char cmd[4096];
            BdStatus bs = build_step_cmd(ctx, cmd, sizeof(cmd));
            if (bs != BD_OK) {
                fail_run_chain(ctx, step_to_fail_ev(rc->step), bs);
                return;
            }

            SshStatus st = ssh_channel_exec(rc->ch, cmd);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) {
                fail_run_chain(ctx, step_to_fail_ev(rc->step), BD_ERR_BUILD);
                return;
            }

            emit_build_mark(ctx, ctx->rs.phase, cmd);

            LOG_INFO(LG_RUN, "exec step=%s", rchain_step_str(rc->step));

            /* Launch: hand channel to DevConsole immediately */
            if (rc->step == RCHAIN_STEP_LAUNCH) {
                ctx->dev_console.ch    = rc->ch;
                ctx->dev_console.state = DCON_STREAMING;
                size_t blen = strlen(rc->cfg.bundle_id);
                if (blen >= sizeof(ctx->dev_console.bundle_id))
                    blen = sizeof(ctx->dev_console.bundle_id) - 1;
                memcpy(ctx->dev_console.bundle_id, rc->cfg.bundle_id, blen);
                ctx->dev_console.bundle_id[blen] = '\0';
                size_t ulen = strlen(rc->target.udid);
                if (ulen >= sizeof(ctx->dev_console.udid))
                    ulen = sizeof(ctx->dev_console.udid) - 1;
                memcpy(ctx->dev_console.udid, rc->target.udid, ulen);
                ctx->dev_console.udid[ulen] = '\0';
                ctx->dev_console.is_sim = rc->target.is_simulator;
                rc->ch     = NULL;
                rc->active = false;

                RunAction act = runstate_step(&ctx->rs, RUN_EV_LAUNCH_OK);
                (void)act;
                emit_run_phase(ctx, ctx->rs.phase, BD_OK);
                emit_stale(ctx, false);
                LOG_INFO(LG_RUN, "launch: handed to dev-console");
                return;
            }

            /* Other steps: start reading */
            rc->state = RCHAIN_READ;
            rc->stall_deadline     = deadline_in(RUN_STALL_SEC);
            rc->has_stall_deadline = true;
            again = true;
            break;
        }
        case RCHAIN_READ: {
            if (rc->has_stall_deadline && deadline_past(&rc->stall_deadline)) {
                LOG_WARN(LG_RUN, "stall timeout step=%s", rchain_step_str(rc->step));
                fail_run_chain(ctx, step_to_fail_ev(rc->step), BD_ERR_BUILD);
                return;
            }
            for (;;) {
                if (ssh_channel_eof(rc->ch)) {
                    rc->state = RCHAIN_EXIT;
                    again     = true;
                    break;
                }
                char tmp[RUN_READ_CHUNK];
                size_t n = 0;
                SshStatus st = ssh_channel_read(rc->ch, tmp, sizeof(tmp), &n);
                if (st == SSH_AGAIN) break;
                if (st != SSH_OK) {
                    fail_run_chain(ctx, step_to_fail_ev(rc->step), BD_ERR_BUILD);
                    return;
                }
                if (n == 0) break;
                /* Reset stall watchdog on each byte */
                rc->stall_deadline = deadline_in(RUN_STALL_SEC);
                process_run_chunk(ctx, tmp, n);
            }
            break;
        }
        case RCHAIN_EXIT: {
            int exit_code = -1;
            SshStatus st = ssh_channel_exit(rc->ch, &exit_code);
            if (st == SSH_AGAIN) return;
            LOG_INFO(LG_RUN, "exit step=%s code=%d", rchain_step_str(rc->step), exit_code);
            ssh_channel_close(rc->ch);
            rc->ch = NULL;
            handle_step_exit(ctx, exit_code);
            break;
        }
        }
    }
}

static void drive_run(WorkerCtx *ctx)
{
    drive_run_abort(ctx);
    drive_dev_console(ctx);
    drive_run_chain(ctx);
}

static void handle_run_execute(WorkerCtx *ctx, const SessionRunCmd *cmd)
{
    if (ctx->sub != SUB_ONLINE) return;
    if (ctx->run_abort.state != RABORT_IDLE) return; /* abort in progress */

    /* Terminate-first: DevConsole streaming, no chain active → kill app then rebuild. */
    if (!ctx->run_chain.active && ctx->dev_console.state != DCON_IDLE) {
        RunAction act = runstate_step(&ctx->rs, RUN_EV_EXECUTE);
        if (act != RUN_ACT_TERMINATE_FIRST) return;
        emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        start_run_abort(ctx, true, cmd);
        LOG_INFO(LG_RUN, "execute: terminate-first project=\"%.64s\"", cmd->cfg.project);
        return;
    }

    if (ctx->run_chain.active || ctx->dev_console.state != DCON_IDLE) return;

    RunAction act = runstate_step(&ctx->rs, RUN_EV_EXECUTE);
    (void)act;
    emit_run_phase(ctx, ctx->rs.phase, BD_OK);
    start_run_chain(ctx, cmd, false);
    LOG_INFO(LG_RUN, "execute project=\"%.64s\"", cmd->cfg.project);
}

static void handle_run_abort(WorkerCtx *ctx)
{
    if (ctx->sub != SUB_ONLINE) return;
    if (ctx->run_abort.state != RABORT_IDLE) return; /* already aborting */

    bool have_chain   = ctx->run_chain.active;
    bool have_console = ctx->dev_console.state != DCON_IDLE;

    /* Nothing to abort and already idle — ignore. */
    if (!have_chain && !have_console && ctx->rs.phase == RUN_IDLE) return;

    start_run_abort(ctx, false, NULL);
}

static void handle_run_compile(WorkerCtx *ctx, const SessionRunCmd *cmd)
{
    if (ctx->sub != SUB_ONLINE) return;
    if (ctx->run_chain.active) return;
    if (ctx->run_abort.state != RABORT_IDLE) return;

    RunAction act = runstate_step(&ctx->rs, RUN_EV_COMPILE);
    (void)act;
    emit_run_phase(ctx, ctx->rs.phase, BD_OK);
    start_run_chain(ctx, cmd, true);
    LOG_INFO(LG_RUN, "compile project=\"%.64s\"", cmd->cfg.project);
}

static void drain_run_cmds(WorkerCtx *ctx)
{
    SessionRunCmd cmd;
    while (spsc_pop(ctx->s->run_cmd_ring, &cmd)) {
        switch (cmd.kind) {
        case RCMD_EXECUTE: handle_run_execute(ctx, &cmd); break;
        case RCMD_COMPILE: handle_run_compile(ctx, &cmd); break;
        case RCMD_ABORT:   handle_run_abort(ctx); break;
        }
    }
}

/* ── disc job helpers ─────────────────────────────────────────────── */

static bool has_active_disc_jobs(const WorkerCtx *ctx)
{
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx->disc_jobs[i].kind != DJOB_KIND_NONE) return true;
    }
    return false;
}

static SessionDiscEventKind failure_event_for(DiscJobKind kind)
{
    switch (kind) {
    case DJOB_KIND_READ_BLUEPRINT:    return DEV_BLUEPRINT_FAILED;
    case DJOB_KIND_RESOLVE_BUNDLE_ID: return DEV_BUNDLE_ID_FAILED;
    case DJOB_KIND_SWEEP_DEVICECTL:
    case DJOB_KIND_SWEEP_SIMCTL:      return DEV_SWEEP_FAILED;
    default:                          return DEV_SCAN_FAILED;
    }
}

static void fail_disc_job(WorkerCtx *ctx, int i, DiscStatus st)
{
    DiscJob *j = &ctx->disc_jobs[i];
    if (ctx->open_owner == i) ctx->open_owner = -1;
    if (j->ch) { ssh_channel_close(j->ch); j->ch = NULL; }
    arena_reset(ctx->disc_arenas[i]);

    bool is_sweep = (j->kind == DJOB_KIND_SWEEP_DEVICECTL ||
                     j->kind == DJOB_KIND_SWEEP_SIMCTL);
    if (is_sweep) {
        ctx->sweep_group_remaining--;
        if (!ctx->sweep_group_failed) {
            ctx->sweep_group_failed = true;
            SessionDiscEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind        = DEV_SWEEP_FAILED;
            ev.disc_status = st;
            push_disc_ev(ctx, &ev);
        }
    } else {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = failure_event_for(j->kind);
        ev.disc_status = st;
        push_disc_ev(ctx, &ev);
    }
    j->kind = DJOB_KIND_NONE;
}

/* Called before ssh_disconnect; channel pointers will be freed by the
   session teardown, so we null them rather than closing individually. */
static void fail_all_disc_jobs(WorkerCtx *ctx)
{
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        DiscJob *j = &ctx->disc_jobs[i];
        if (j->kind == DJOB_KIND_NONE) continue;
        j->ch = NULL;  /* session teardown frees it */
        arena_reset(ctx->disc_arenas[i]);

        bool is_sweep = (j->kind == DJOB_KIND_SWEEP_DEVICECTL ||
                         j->kind == DJOB_KIND_SWEEP_SIMCTL);
        if (is_sweep) {
            ctx->sweep_group_remaining--;
            if (!ctx->sweep_group_failed) {
                ctx->sweep_group_failed = true;
                SessionDiscEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind        = DEV_SWEEP_FAILED;
                ev.disc_status = DISC_ERR_COMMAND_FAILED;
                push_disc_ev(ctx, &ev);
            }
        } else {
            SessionDiscEvent ev;
            memset(&ev, 0, sizeof(ev));
            ev.kind        = failure_event_for(j->kind);
            ev.disc_status = DISC_ERR_COMMAND_FAILED;
            push_disc_ev(ctx, &ev);
        }
        j->kind = DJOB_KIND_NONE;
    }
}

#ifdef OSTRICH_DEBUG
static const char *djob_kind_str(DiscJobKind k)
{
    switch (k) {
    case DJOB_KIND_SCAN:              return "scan";
    case DJOB_KIND_READ_BLUEPRINT:    return "read-blueprint";
    case DJOB_KIND_RESOLVE_BUNDLE_ID: return "resolve-bundle-id";
    case DJOB_KIND_SWEEP_DEVICECTL:   return "sweep-devicectl";
    case DJOB_KIND_SWEEP_SIMCTL:      return "sweep-simctl";
    default:                          return "none";
    }
}
#endif

/* Drive a single disc job through its state machine.
   Returns without blocking; re-called each worker iteration. */
static void drive_disc_job(WorkerCtx *ctx, int i)
{
    DiscJob *j = &ctx->disc_jobs[i];
    bool again = true;

    while (again && j->kind != DJOB_KIND_NONE) {
        again = false;

        switch (j->state) {
        case DJOB_OPEN: {
            /* libssh2 tracks channel-open progress on the *session*, so two
               opens in flight at once clobber each other and wedge a channel
               (it never reaches EOF). Serialize: a job may drive its open
               only while no other job holds the open seam. Reads after the
               open still run concurrently across channels. */
            if (ctx->open_owner != -1 && ctx->open_owner != i) return;
            ctx->open_owner = i;
            SshStatus st = ssh_channel_open(ctx->ssh, &j->ch);
            if (st == SSH_AGAIN) return;            /* keep holding the seam */
            if (st != SSH_OK) { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
            ctx->open_owner = -1;                   /* seam released */
            j->state = DJOB_EXEC;
            again    = true;
            break;
        }
        case DJOB_EXEC: {
            SshStatus st = ssh_channel_exec(j->ch, j->cmd);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
#ifdef OSTRICH_DEBUG
            j->exec_start = mono_now();
#endif
            LOG_INFO(LG_DISC, "exec job=%d kind=%s cmd=\"%s\"",
                     i, djob_kind_str(j->kind), j->cmd);
            j->state = DJOB_READ;
            again    = true;
            break;
        }
        case DJOB_READ: {
            for (;;) {
                if (ssh_channel_eof(j->ch)) {
                    j->state = DJOB_EXIT;
                    again    = true;
                    break;
                }
                char tmp[DISC_READ_CHUNK];
                size_t n = 0;
                SshStatus st = ssh_channel_read(j->ch, tmp, sizeof(tmp), &n);
                if (st == SSH_AGAIN) break;
                if (st != SSH_OK) {
                    fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED);
                    return;
                }
                if (n == 0) break;
                if (j->buf_len + n > DISC_JOB_BUF_CAP) {
                    fail_disc_job(ctx, i, DISC_ERR_OOM);
                    return;
                }
                memcpy(j->buf + j->buf_len, tmp, n);
                j->buf_len += n;
            }
            break;
        }
        case DJOB_EXIT: {
            int exit_code = -1;
            SshStatus st  = ssh_channel_exit(j->ch, &exit_code);
            if (st == SSH_AGAIN) return;
            ssh_channel_close(j->ch);
            j->ch = NULL;
            if (st != SSH_OK) { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
#ifdef OSTRICH_DEBUG
            {
                struct timespec now = mono_now();
                long ms = (now.tv_sec  - j->exec_start.tv_sec)  * 1000L
                        + (now.tv_nsec - j->exec_start.tv_nsec) / 1000000L;
                LOG_INFO(LG_DISC, "done job=%d exit=%d ms=%ldms bytes=%zu",
                         i, exit_code, ms, j->buf_len);
                LOG_BLOB(LOG_DEBUG, LG_DISC, "output", j->buf, j->buf_len);
            }
#endif
            if (exit_code == 127) { fail_disc_job(ctx, i, DISC_ERR_XCODE_MISSING); return; }
            if (exit_code != 0)   { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
            {
                Str raw = { j->buf, j->buf_len };
                DiscStatus ds = DISC_OK;
                switch (j->kind) {
                case DJOB_KIND_SCAN:
                    ds = disc_curate_blueprints(ctx->disc_arenas[i], raw,
                                                j->scan_depth, &j->curated);
                    break;
                case DJOB_KIND_READ_BLUEPRINT:
                    ds = disc_parse_list(ctx->disc_arenas[i], raw,
                                         &j->schemes, &j->configs);
                    break;
                case DJOB_KIND_RESOLVE_BUNDLE_ID:
                    ds = disc_parse_bundle_id(raw, j->bundle_id, sizeof(j->bundle_id));
                    break;
                case DJOB_KIND_SWEEP_DEVICECTL:
                    ds = disc_parse_devicectl(ctx->disc_arenas[i], raw, &j->targets);
                    break;
                case DJOB_KIND_SWEEP_SIMCTL:
                    ds = disc_parse_simctl(ctx->disc_arenas[i], raw, &j->targets);
                    break;
                default:
                    ds = DISC_ERR_COMMAND_FAILED;
                    break;
                }
                if (ds != DISC_OK) {
                    LOG_WARN(LG_DISC, "parse-fail job=%d kind=%s status=%s",
                             i, djob_kind_str(j->kind), disc_status_str(ds));
                    LOG_BLOB(LOG_WARN, LG_DISC, "raw-output", j->buf, j->buf_len);
                    fail_disc_job(ctx, i, ds);
                    return;
                }
            }
            j->state    = DJOB_EMIT;
            j->emit_idx = 0;
            again       = true;
            break;
        }
        case DJOB_EMIT: {
            switch (j->kind) {
            case DJOB_KIND_SCAN: {
                /* emit one blueprint per push; return if ring full */
                while (j->emit_idx < j->curated.count) {
                    SessionDiscEvent ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.kind      = DEV_BLUEPRINT;
                    ev.blueprint = j->curated.items[j->emit_idx];
                    if (!spsc_push(ctx->s->disc_event_ring, &ev)) return;
                    j->emit_idx++;
                }
                SessionDiscEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind  = DEV_SCAN_COMPLETE;
                ev.count = j->curated.count;
                push_disc_ev(ctx, &ev);
                break;
            }
            case DJOB_KIND_READ_BLUEPRINT: {
                /* emit all schemes then all configs one at a time */
                int total = j->schemes.count + j->configs.count;
                while (j->emit_idx < total) {
                    SessionDiscEvent ev;
                    memset(&ev, 0, sizeof(ev));
                    if (j->emit_idx < j->schemes.count) {
                        ev.kind = DEV_SCHEME;
                        const char *src = j->schemes.items[j->emit_idx];
                        size_t len = strlen(src);
                        if (len >= sizeof(ev.scheme)) len = sizeof(ev.scheme) - 1;
                        memcpy(ev.scheme, src, len);
                        ev.scheme[len] = '\0';
                    } else {
                        int ci = j->emit_idx - j->schemes.count;
                        ev.kind = DEV_CONFIG;
                        const char *src = j->configs.items[ci];
                        size_t len = strlen(src);
                        if (len >= sizeof(ev.config)) len = sizeof(ev.config) - 1;
                        memcpy(ev.config, src, len);
                        ev.config[len] = '\0';
                    }
                    if (!spsc_push(ctx->s->disc_event_ring, &ev)) return;
                    j->emit_idx++;
                }
                SessionDiscEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind  = DEV_BLUEPRINT_READ_COMPLETE;
                ev.count = total;
                push_disc_ev(ctx, &ev);
                break;
            }
            case DJOB_KIND_RESOLVE_BUNDLE_ID: {
                SessionDiscEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.kind = DEV_BUNDLE_ID;
                size_t len = strlen(j->bundle_id);
                if (len >= sizeof(ev.bundle_id)) len = sizeof(ev.bundle_id) - 1;
                memcpy(ev.bundle_id, j->bundle_id, len);
                ev.bundle_id[len] = '\0';
                push_disc_ev(ctx, &ev);
                break;
            }
            case DJOB_KIND_SWEEP_DEVICECTL:
            case DJOB_KIND_SWEEP_SIMCTL: {
                /* Emit targets only if the group hasn't already failed. */
                if (!ctx->sweep_group_failed) {
                    while (j->emit_idx < j->targets.count) {
                        SessionDiscEvent ev;
                        memset(&ev, 0, sizeof(ev));
                        ev.kind   = DEV_TARGET;
                        ev.target = j->targets.items[j->emit_idx];
                        if (!spsc_push(ctx->s->disc_event_ring, &ev)) return;
                        j->emit_idx++;
                        ctx->sweep_total++;
                    }
                }
                ctx->sweep_group_remaining--;
                if (ctx->sweep_group_remaining == 0 && !ctx->sweep_group_failed) {
                    SessionDiscEvent ev;
                    memset(&ev, 0, sizeof(ev));
                    ev.kind  = DEV_SWEEP_COMPLETE;
                    ev.count = ctx->sweep_total;
                    push_disc_ev(ctx, &ev);
                }
                /* outer code clears arena + kind */
                break;
            }
            default:
                fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED);
                return;
            }
            arena_reset(ctx->disc_arenas[i]);
            j->kind = DJOB_KIND_NONE;
            break;
        }
        }
    }
}

static void drive_disc_jobs(WorkerCtx *ctx)
{
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        DiscJob *j = &ctx->disc_jobs[i];
        if (j->kind == DJOB_KIND_NONE) continue;
        /* Arm the watchdog on first sight; fail a job that overruns it. */
        if (!j->has_deadline) {
            j->deadline     = deadline_in(DISC_JOB_TIMEOUT_SEC);
            j->has_deadline = true;
        }
        if (deadline_past(&j->deadline)) {
            fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED);
            continue;
        }
        drive_disc_job(ctx, i);
    }
}

/* ── disc command handlers ────────────────────────────────────────── */

static void handle_disc_scan(WorkerCtx *ctx, const SessionDiscCmd *cmd)
{
    if (ctx->sub != SUB_ONLINE) return;

    int slot = -1;
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx->disc_jobs[i].kind == DJOB_KIND_NONE) { slot = i; break; }
    }
    if (slot < 0) return; /* table full; silently drop */

    DiscJob *j = &ctx->disc_jobs[slot];
    memset(j, 0, sizeof(*j));

    int depth = cmd->max_depth > 0 ? cmd->max_depth : DISC_SCAN_DEPTH_DEFAULT;
    DiscStatus ds = disc_scan_cmd(cmd->root, depth, j->cmd, sizeof(j->cmd));
    if (ds != DISC_OK) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_SCAN_FAILED;
        ev.disc_status = ds;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->buf = arena_alloc(ctx->disc_arenas[slot], DISC_JOB_BUF_CAP, 1);
    if (!j->buf) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_SCAN_FAILED;
        ev.disc_status = DISC_ERR_OOM;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->scan_depth = depth;
    j->kind       = DJOB_KIND_SCAN;
    j->state      = DJOB_OPEN;
}

static void handle_disc_abort(WorkerCtx *ctx)
{
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        DiscJob *j = &ctx->disc_jobs[i];
        if (j->kind != DJOB_KIND_SCAN) continue;
        if (j->ch) { ssh_channel_close(j->ch); j->ch = NULL; }
        arena_reset(ctx->disc_arenas[i]);
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_SCAN_FAILED;
        ev.disc_status = DISC_ERR_COMMAND_FAILED;
        push_disc_ev(ctx, &ev);
        j->kind = DJOB_KIND_NONE;
    }
}

static void handle_disc_read_blueprint(WorkerCtx *ctx, const SessionDiscCmd *cmd)
{
    if (ctx->sub != SUB_ONLINE) return;

    int slot = -1;
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx->disc_jobs[i].kind == DJOB_KIND_NONE) { slot = i; break; }
    }
    if (slot < 0) return;

    DiscJob *j = &ctx->disc_jobs[slot];
    memset(j, 0, sizeof(*j));

    DiscStatus ds = disc_list_cmd(cmd->project, j->cmd, sizeof(j->cmd));
    if (ds != DISC_OK) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_BLUEPRINT_FAILED;
        ev.disc_status = ds;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->buf = arena_alloc(ctx->disc_arenas[slot], DISC_JOB_BUF_CAP, 1);
    if (!j->buf) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_BLUEPRINT_FAILED;
        ev.disc_status = DISC_ERR_OOM;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->kind  = DJOB_KIND_READ_BLUEPRINT;
    j->state = DJOB_OPEN;
}

static void handle_disc_resolve_bundle_id(WorkerCtx *ctx, const SessionDiscCmd *cmd)
{
    if (ctx->sub != SUB_ONLINE) return;

    int slot = -1;
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx->disc_jobs[i].kind == DJOB_KIND_NONE) { slot = i; break; }
    }
    if (slot < 0) return;

    DiscJob *j = &ctx->disc_jobs[slot];
    memset(j, 0, sizeof(*j));

    DiscStatus ds = disc_build_settings_cmd(cmd->project, cmd->scheme, cmd->config,
                                            j->cmd, sizeof(j->cmd));
    if (ds != DISC_OK) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_BUNDLE_ID_FAILED;
        ev.disc_status = ds;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->buf = arena_alloc(ctx->disc_arenas[slot], DISC_JOB_BUF_CAP, 1);
    if (!j->buf) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_BUNDLE_ID_FAILED;
        ev.disc_status = DISC_ERR_OOM;
        push_disc_ev(ctx, &ev);
        return;
    }

    j->kind  = DJOB_KIND_RESOLVE_BUNDLE_ID;
    j->state = DJOB_OPEN;
}

static void handle_disc_sweep(WorkerCtx *ctx)
{
    if (ctx->sub != SUB_ONLINE) return;

    /* Require no sweep already in flight; find two free slots. */
    int slots[2] = {-1, -1};
    int found    = 0;
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        DiscJobKind k = ctx->disc_jobs[i].kind;
        if (k == DJOB_KIND_SWEEP_DEVICECTL || k == DJOB_KIND_SWEEP_SIMCTL)
            return;  /* sweep in flight — silently drop */
        if (k == DJOB_KIND_NONE && found < 2)
            slots[found++] = i;
    }
    if (found < 2) {
        SessionDiscEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind        = DEV_SWEEP_FAILED;
        ev.disc_status = DISC_ERR_OOM;
        push_disc_ev(ctx, &ev);
        return;
    }

    DiscStatus ds = DISC_OK;
    DiscJob   *jd = &ctx->disc_jobs[slots[0]];
    DiscJob   *js = &ctx->disc_jobs[slots[1]];
    memset(jd, 0, sizeof(*jd));
    memset(js, 0, sizeof(*js));

    ds = disc_devicectl_cmd(jd->cmd, sizeof(jd->cmd));
    if (ds != DISC_OK) goto fail;
    jd->buf = arena_alloc(ctx->disc_arenas[slots[0]], DISC_JOB_BUF_CAP, 1);
    if (!jd->buf) { ds = DISC_ERR_OOM; goto fail; }

    ds = disc_simctl_cmd(js->cmd, sizeof(js->cmd));
    if (ds != DISC_OK) goto fail;
    js->buf = arena_alloc(ctx->disc_arenas[slots[1]], DISC_JOB_BUF_CAP, 1);
    if (!js->buf) { ds = DISC_ERR_OOM; goto fail; }

    ctx->sweep_group_remaining = 2;
    ctx->sweep_group_failed    = false;
    ctx->sweep_total           = 0;

    jd->kind  = DJOB_KIND_SWEEP_DEVICECTL;
    jd->state = DJOB_OPEN;
    js->kind  = DJOB_KIND_SWEEP_SIMCTL;
    js->state = DJOB_OPEN;
    return;

fail:
    arena_reset(ctx->disc_arenas[slots[0]]);
    arena_reset(ctx->disc_arenas[slots[1]]);
    SessionDiscEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind        = DEV_SWEEP_FAILED;
    ev.disc_status = ds;
    push_disc_ev(ctx, &ev);
}

static void drain_disc_cmds(WorkerCtx *ctx)
{
    SessionDiscCmd cmd;
    while (spsc_pop(ctx->s->disc_cmd_ring, &cmd)) {
        switch (cmd.kind) {
        case DCMD_SCAN_HOST:         handle_disc_scan(ctx, &cmd);              break;
        case DCMD_ABORT_SCAN:        handle_disc_abort(ctx);                   break;
        case DCMD_READ_BLUEPRINT:    handle_disc_read_blueprint(ctx, &cmd);    break;
        case DCMD_RESOLVE_BUNDLE_ID: handle_disc_resolve_bundle_id(ctx, &cmd); break;
        case DCMD_SWEEP_TARGETS:     handle_disc_sweep(ctx);                   break;
        }
    }
}

/* ── disconnect / reset helpers ───────────────────────────────────── */

static void disconnect_ssh(WorkerCtx *ctx)
{
    fail_all_disc_jobs(ctx);

    /* Tear down run abort if in flight. */
    RunAbort *ra = &ctx->run_abort;
    if (ra->state != RABORT_IDLE) {
        if (ctx->open_owner == RUN_ABORT_SLOT) ctx->open_owner = -1;
        if (ra->ch) ra->ch = NULL;  /* session teardown frees it */
        bool was_reexecute = ra->is_reexecute;
        ra->state = RABORT_IDLE;
        /* For terminate-first, rs is already BUILDING — emit DROP. */
        if (was_reexecute) {
            RunAction act = runstate_step(&ctx->rs, RUN_EV_DROP);
            (void)act;
            emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        }
        LOG_WARN(LG_RUN, "run-abort dropped on disconnect");
    }

    /* Tear down run chain */
    RunChain *rc = &ctx->run_chain;
    if (rc->active) {
        rc->ch = NULL;  /* session teardown frees it */
        arena_reset(ctx->run_arena);
        rc->settings_buf = NULL;
        rc->settings_len = 0;
        rc->active = false;
        RunAction act = runstate_step(&ctx->rs, RUN_EV_DROP);
        (void)act;
        emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        LOG_WARN(LG_RUN, "chain dropped on disconnect");
    }

    /* Tear down DevConsole */
    DevConsole *dc = &ctx->dev_console;
    if (dc->state != DCON_IDLE) {
        dc->ch = NULL;  /* session teardown frees it */
        dc->state = DCON_IDLE;
        if (ctx->rs.phase == RUN_RUNNING) {
            RunAction act = runstate_step(&ctx->rs, RUN_EV_DROP);
            (void)act;
            emit_run_phase(ctx, ctx->rs.phase, BD_OK);
        }
        LOG_WARN(LG_RUN, "dev-console dropped on disconnect");
    }

    ctx->open_owner = -1;
    if (ctx->ssh) {
        ssh_disconnect(ctx->ssh);
        ctx->ssh = NULL;
    }
    ctx->ssh_fd = -1;
    arena_reset(ctx->arena);
}

/* Advance connstate with ev, disconnect, and emit the appropriate
   phase event.  Called whenever a connection step fails. */
static void on_fail(WorkerCtx *ctx, ConnEvent ev, SshStatus reason)
{
    ctx->cs.last_reason = reason;
    ConnAction act = connstate_step(&ctx->cs, ev);

    disconnect_ssh(ctx);
    ctx->has_deadline = false;

    if (act == ACT_SEVERED || ctx->cs.phase == CONN_SEVERED) {
        ctx->sub = SUB_IDLE;
        emit_ev(ctx, CONN_SEVERED, reason, false, false);
    } else if (ctx->cs.phase == CONN_REACQUIRING) {
        double delay      = connstate_backoff_delay(ctx->cs.attempt);
        ctx->deadline     = deadline_in(delay);
        ctx->has_deadline = true;
        ctx->sub          = SUB_BACKOFF;
        emit_ev(ctx, CONN_REACQUIRING, reason, false, false);
    } else {
        ctx->sub = SUB_IDLE;
        emit_ev(ctx, CONN_DISCONNECTED, reason, false, false);
    }
}

/* ── connection command handlers ──────────────────────────────────── */

static void handle_breach(WorkerCtx *ctx, const SshConfig *cfg)
{
    disconnect_ssh(ctx);
    ctx->sub          = SUB_IDLE;
    ctx->has_deadline = false;
    connstate_init(&ctx->cs);

    ctx->cfg = *cfg;
    LOG_INFO(LG_CONN, "breach: connecting to %s@%s:%d",
             cfg->user, cfg->host, cfg->port);

    SshStatus st = ssh_connect_start(ctx->arena, ctx->cfg,
                                     &ctx->ssh, &ctx->ssh_fd);
    if (st != SSH_OK) {
        connstate_step(&ctx->cs, EV_BREACH);
        on_fail(ctx, EV_FAIL, st);
        return;
    }

    connstate_step(&ctx->cs, EV_BREACH);
    ctx->sub          = SUB_HANDSHAKE;
    ctx->deadline     = deadline_in(CONNECT_TIMEOUT_SEC);
    ctx->has_deadline = true;
    emit_ev(ctx, CONN_CONNECTING, SSH_OK, false, false);
}

static void handle_abort_close(WorkerCtx *ctx, ConnEvent ev)
{
    connstate_step(&ctx->cs, ev);
    disconnect_ssh(ctx);
    ctx->sub          = SUB_IDLE;
    ctx->has_deadline = false;
    emit_ev(ctx, CONN_DISCONNECTED, SSH_OK, false, false);
}

static void handle_trust(WorkerCtx *ctx)
{
    if (ctx->sub != SUB_AWAIT_HOSTKEY) return;

    SshStatus st = ssh_hostkey_trust(ctx->ssh);
    if (st != SSH_OK) {
        on_fail(ctx, EV_DECLINE, st);
        return;
    }
    connstate_step(&ctx->cs, EV_TRUST);
    ctx->sub          = SUB_AUTH;
    ctx->deadline     = deadline_in(CONNECT_TIMEOUT_SEC);
    ctx->has_deadline = true;
}

static void handle_decline(WorkerCtx *ctx)
{
    if (ctx->sub != SUB_AWAIT_HOSTKEY) return;
    handle_abort_close(ctx, EV_DECLINE);
}

static void drain_cmds(WorkerCtx *ctx)
{
    SessionCmd cmd;
    while (spsc_pop(ctx->s->cmd_ring, &cmd)) {
        switch (cmd.kind) {
        case CMD_BREACH:
            handle_breach(ctx, &cmd.cfg);
            break;
        case CMD_ABORT:
            if (ctx->sub != SUB_IDLE && ctx->sub != SUB_ONLINE)
                handle_abort_close(ctx, EV_ABORT);
            break;
        case CMD_CLOSE:
            if (ctx->sub != SUB_IDLE)
                handle_abort_close(ctx, EV_CLOSE);
            break;
        case CMD_TRUST:
            handle_trust(ctx);
            break;
        case CMD_DECLINE:
            handle_decline(ctx);
            break;
        }
    }
}

/* ── sub-phase driver ─────────────────────────────────────────────── */

static void drive_sub(WorkerCtx *ctx)
{
    SshStatus st;

    switch (ctx->sub) {
    case SUB_IDLE:
    case SUB_AWAIT_HOSTKEY:
        return;

    case SUB_BACKOFF:
        if (ctx->has_deadline && deadline_past(&ctx->deadline)) {
            ctx->has_deadline = false;
            connstate_step(&ctx->cs, EV_BACKOFF_EXPIRED);
            st = ssh_connect_start(ctx->arena, ctx->cfg,
                                   &ctx->ssh, &ctx->ssh_fd);
            if (st != SSH_OK) {
                on_fail(ctx, EV_FAIL, st);
                return;
            }
            ctx->sub          = SUB_HANDSHAKE;
            ctx->deadline     = deadline_in(CONNECT_TIMEOUT_SEC);
            ctx->has_deadline = true;
        }
        return;

    case SUB_HANDSHAKE:
        if (ctx->has_deadline && deadline_past(&ctx->deadline)) {
            on_fail(ctx, EV_FAIL, SSH_ERR_TIMEOUT);
            return;
        }
        st = ssh_handshake_step(ctx->ssh);
        if (st == SSH_AGAIN) return;
        if (st != SSH_OK) {
            on_fail(ctx, EV_FAIL, st);
            return;
        }
        {
            SshHostKeyVerdict verdict;
            st = ssh_hostkey_check(ctx->ssh, &verdict,
                                   ctx->fingerprint, sizeof(ctx->fingerprint));
            if (st != SSH_OK) {
                on_fail(ctx, EV_FAIL, st);
                return;
            }
            if (verdict == SSH_HOSTKEY_MISMATCH) {
                connstate_step(&ctx->cs, EV_HOSTKEY_MISMATCH);
                SessionEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.phase            = CONN_DISCONNECTED;
                ev.reason           = SSH_ERR_HOSTKEY_MISMATCH;
                ev.hostkey_mismatch = true;
                snprintf(ev.user_host, sizeof(ev.user_host),
                         "%s@%s", ctx->cfg.user, ctx->cfg.host);
                memcpy(ev.fingerprint, ctx->fingerprint, sizeof(ev.fingerprint));
                spsc_push(ctx->s->event_ring, &ev);
                disconnect_ssh(ctx);
                ctx->sub          = SUB_IDLE;
                ctx->has_deadline = false;
                return;
            }
            if (verdict == SSH_HOSTKEY_UNKNOWN) {
                connstate_step(&ctx->cs, EV_HOSTKEY_UNKNOWN);
                ctx->sub          = SUB_AWAIT_HOSTKEY;
                ctx->has_deadline = false;
                emit_ev(ctx, CONN_AWAITING_HOSTKEY, SSH_OK, true, false);
                return;
            }
            connstate_step(&ctx->cs, EV_HOSTKEY_OK);
            ctx->sub = SUB_AUTH;
            LOG_INFO(LG_CONN, "handshake ok host=%s → AUTH", ctx->cfg.host);
        }
        return;

    case SUB_AUTH:
        if (ctx->has_deadline && deadline_past(&ctx->deadline)) {
            on_fail(ctx, EV_AUTH_FAIL, SSH_ERR_TIMEOUT);
            return;
        }
        st = ssh_auth_step(ctx->ssh);
        if (st == SSH_AGAIN) return;
        if (st != SSH_OK) {
            on_fail(ctx, EV_AUTH_FAIL, SSH_ERR_AUTH);
            return;
        }
        connstate_step(&ctx->cs, EV_AUTH_OK);
        ctx->sub = SUB_PROBE;
        LOG_INFO(LG_CONN, "auth ok → PROBE");
        return;

    case SUB_PROBE:
        if (ctx->has_deadline && deadline_past(&ctx->deadline)) {
            on_fail(ctx, EV_PROBE_FAIL, SSH_ERR_TIMEOUT);
            return;
        }
        {
            int exit_code = -1;
            st = ssh_probe_step(ctx->ssh, &exit_code);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) {
                on_fail(ctx, EV_PROBE_FAIL, SSH_ERR_NO_SHELL);
                return;
            }
            connstate_step(&ctx->cs, EV_PROBE_OK);
            ctx->sub            = SUB_ONLINE;
            ctx->has_deadline   = false;
            ctx->keepalive_next = KEEPALIVE_INTERVAL_SEC;
            emit_ev(ctx, CONN_ONLINE, SSH_OK, false, false);
        }
        return;

    case SUB_ONLINE:
        return;
    }
}

/* ── poll timeout computation ─────────────────────────────────────── */

static int compute_timeout_ms(const WorkerCtx *ctx)
{
    switch (ctx->sub) {
    case SUB_IDLE:
    case SUB_AWAIT_HOSTKEY:
        return -1;

    case SUB_HANDSHAKE:
    case SUB_AUTH:
    case SUB_PROBE:
        if (ctx->has_deadline) {
            int ms = ms_until(&ctx->deadline);
            return ms > 0 ? ms : 1;
        }
        return (int)(CONNECT_TIMEOUT_SEC * 1000.0);

    case SUB_BACKOFF:
        if (ctx->has_deadline) {
            int ms = ms_until(&ctx->deadline);
            return ms > 0 ? ms : 0;
        }
        return 1000;

    case SUB_ONLINE:
        /* bound poll while disc jobs or run chain are in flight */
        if (has_active_disc_jobs(ctx) ||
            ctx->run_chain.active ||
            ctx->dev_console.state != DCON_IDLE ||
            ctx->run_abort.state != RABORT_IDLE)
            return 10;
        return ctx->keepalive_next * 1000;
    }
    return -1;
}

/* ── worker thread ─────────────────────────────────────────────────── */

static void *worker_fn(void *arg)
{
    Session *s = arg;

    log_set_thread_tag("wkr");
    LOG_INFO(LG_SESS, "worker started");

    WorkerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.s              = s;
    ctx.ssh_fd         = -1;
    ctx.sub            = SUB_IDLE;
    ctx.keepalive_next = KEEPALIVE_INTERVAL_SEC;
    ctx.open_owner     = -1;
    ctx.arena          = arena_create(WORKER_ARENA_SZ);
    if (!ctx.arena) {
        atomic_store(&s->running, 0);
        return NULL;
    }

    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        ctx.disc_arenas[i] = arena_create(DISC_JOB_ARENA_SZ);
        if (!ctx.disc_arenas[i]) {
            for (int j = 0; j < i; j++) arena_destroy(ctx.disc_arenas[j]);
            arena_destroy(ctx.arena);
            atomic_store(&s->running, 0);
            return NULL;
        }
    }

    ctx.run_arena = arena_create(RUN_ARENA_SZ);
    if (!ctx.run_arena) {
        for (int i = 0; i < DISC_MAX_JOBS; i++) arena_destroy(ctx.disc_arenas[i]);
        arena_destroy(ctx.arena);
        atomic_store(&s->running, 0);
        return NULL;
    }

    connstate_init(&ctx.cs);
    runstate_init(&ctx.rs);

    while (atomic_load(&s->running)) {
        struct pollfd fds[2];
        fds[0].fd      = s->pipe_read;
        fds[0].events  = POLLIN;
        fds[0].revents = 0;
        int nfds = 1;

        if (ctx.ssh_fd >= 0) {
            fds[1].fd      = ctx.ssh_fd;
            fds[1].events  = POLLIN | POLLOUT;
            fds[1].revents = 0;
            nfds = 2;
        }

        int nready = poll(fds, nfds, compute_timeout_ms(&ctx));

        if (fds[0].revents & POLLIN) {
            char buf[64];
            (void)read(s->pipe_read, buf, sizeof(buf));
        }

        drain_cmds(&ctx);
        drain_disc_cmds(&ctx);
        drain_run_cmds(&ctx);

        if (ctx.sub == SUB_ONLINE && nready == 0 && ctx.ssh) {
            SshStatus kst = ssh_keepalive(ctx.ssh, &ctx.keepalive_next);
            if (kst != SSH_OK && kst != SSH_AGAIN) {
                ConnAction act = connstate_step(&ctx.cs, EV_DROP);
                disconnect_ssh(&ctx);
                if (act == ACT_SEVERED || ctx.cs.phase == CONN_SEVERED) {
                    ctx.sub          = SUB_IDLE;
                    ctx.has_deadline = false;
                    emit_ev(&ctx, CONN_SEVERED, kst, false, false);
                } else {
                    double delay      = connstate_backoff_delay(ctx.cs.attempt);
                    ctx.deadline     = deadline_in(delay);
                    ctx.has_deadline = true;
                    ctx.sub          = SUB_BACKOFF;
                    emit_ev(&ctx, CONN_REACQUIRING, kst, false, false);
                }
            }
        }

        drive_sub(&ctx);

        if (ctx.sub == SUB_ONLINE && ctx.ssh) {
            drive_disc_jobs(&ctx);
            drive_run(&ctx);
        }
    }

    LOG_INFO(LG_SESS, "worker stopped");

    if (ctx.ssh) ssh_disconnect(ctx.ssh);
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx.disc_arenas[i]) arena_destroy(ctx.disc_arenas[i]);
    }
    if (ctx.run_arena) arena_destroy(ctx.run_arena);
    if (ctx.arena) arena_destroy(ctx.arena);
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────── */

SshStatus session_open(Session **out)
{
    *out = NULL;

    Session *s = malloc(sizeof(*s));
    if (!s) return SSH_ERR_OOM;
    memset(s, 0, sizeof(*s));

    s->cmd_ring = spsc_create(sizeof(SessionCmd), CMD_RING_CAP);
    if (!s->cmd_ring) { free(s); return SSH_ERR_OOM; }

    s->event_ring = spsc_create(sizeof(SessionEvent), EVENT_RING_CAP);
    if (!s->event_ring) {
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_OOM;
    }

    s->disc_cmd_ring = spsc_create(sizeof(SessionDiscCmd), DISC_CMD_RING_CAP);
    if (!s->disc_cmd_ring) {
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_OOM;
    }

    s->disc_event_ring = spsc_create(sizeof(SessionDiscEvent), DISC_EVENT_RING_CAP);
    if (!s->disc_event_ring) {
        spsc_destroy(s->disc_cmd_ring);
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_OOM;
    }

    s->run_cmd_ring = spsc_create(sizeof(SessionRunCmd), RUN_CMD_RING_CAP);
    if (!s->run_cmd_ring) {
        spsc_destroy(s->disc_event_ring);
        spsc_destroy(s->disc_cmd_ring);
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_OOM;
    }

    s->run_event_ring = spsc_create(sizeof(SessionRunEvent), RUN_EVENT_RING_CAP);
    if (!s->run_event_ring) {
        spsc_destroy(s->run_cmd_ring);
        spsc_destroy(s->disc_event_ring);
        spsc_destroy(s->disc_cmd_ring);
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_OOM;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        spsc_destroy(s->run_event_ring);
        spsc_destroy(s->run_cmd_ring);
        spsc_destroy(s->disc_event_ring);
        spsc_destroy(s->disc_cmd_ring);
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_IO;
    }
    s->pipe_read  = pipefd[0];
    s->pipe_write = pipefd[1];

    atomic_store(&s->running, 1);

    if (pthread_create(&s->thread, NULL, worker_fn, s) != 0) {
        close(s->pipe_read);
        close(s->pipe_write);
        spsc_destroy(s->run_event_ring);
        spsc_destroy(s->run_cmd_ring);
        spsc_destroy(s->disc_event_ring);
        spsc_destroy(s->disc_cmd_ring);
        spsc_destroy(s->event_ring);
        spsc_destroy(s->cmd_ring);
        free(s);
        return SSH_ERR_IO;
    }

    *out = s;
    return SSH_OK;
}

bool session_submit(Session *s, const SessionCmd *cmd)
{
    bool ok = spsc_push(s->cmd_ring, cmd);
    if (ok) {
        char byte = 1;
        (void)write(s->pipe_write, &byte, 1);
    }
    return ok;
}

bool session_poll(Session *s, SessionEvent *out)
{
    return spsc_pop(s->event_ring, out);
}

bool session_disc_submit(Session *s, const SessionDiscCmd *cmd)
{
    bool ok = spsc_push(s->disc_cmd_ring, cmd);
    if (ok) {
        char byte = 1;
        (void)write(s->pipe_write, &byte, 1);
    }
    return ok;
}

bool session_disc_poll(Session *s, SessionDiscEvent *out)
{
    return spsc_pop(s->disc_event_ring, out);
}

void session_close(Session *s)
{
    if (!s) return;
    atomic_store(&s->running, 0);
    char byte = 0;
    (void)write(s->pipe_write, &byte, 1);
    pthread_join(s->thread, NULL);
    close(s->pipe_write);
    close(s->pipe_read);
    spsc_destroy(s->run_event_ring);
    spsc_destroy(s->run_cmd_ring);
    spsc_destroy(s->disc_event_ring);
    spsc_destroy(s->disc_cmd_ring);
    spsc_destroy(s->event_ring);
    spsc_destroy(s->cmd_ring);
    free(s);
}

const char *session_status_str(SshStatus st)
{
    return ssh_status_str(st);
}

bool session_run_submit(Session *s, const SessionRunCmd *cmd)
{
    bool ok = spsc_push(s->run_cmd_ring, cmd);
    if (ok) {
        char byte = 1;
        (void)write(s->pipe_write, &byte, 1);
    }
    return ok;
}

bool session_run_poll(Session *s, SessionRunEvent *out)
{
    return spsc_pop(s->run_event_ring, out);
}
