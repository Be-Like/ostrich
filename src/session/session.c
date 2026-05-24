#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "arena.h"
#include "connstate.h"
#include "discovery.h"
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

/* ── discovery job engine constants ───────────────────────────────── */

#define DISC_MAX_JOBS      4
#define DISC_JOB_ARENA_SZ  (1024 * 1024)  /* 1 MB per job arena   */
#define DISC_JOB_BUF_CAP   (512 * 1024)   /* 512 KB output buffer */
#define DISC_READ_CHUNK    4096
#define DISC_SCAN_DEPTH_DEFAULT 8

/* ── Session control block (flagged malloc) ──────────────────────── */

struct Session {
    SpscRing  *cmd_ring;         /* UI→worker: SessionCmd records      */
    SpscRing  *event_ring;       /* worker→UI: SessionEvent records    */
    SpscRing  *disc_cmd_ring;    /* UI→worker: SessionDiscCmd records  */
    SpscRing  *disc_event_ring;  /* worker→UI: SessionDiscEvent records */
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
    /* sweep group tracking (one sweep at a time) */
    int              sweep_group_remaining; /* sweep jobs still in flight */
    bool             sweep_group_failed;    /* SWEEP_FAILED already emitted */
    int              sweep_total;           /* DEV_TARGET events emitted so far */
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
}

/* Push a disc event; spin briefly until space (UI drains each frame). */
static void push_disc_ev(WorkerCtx *ctx, const SessionDiscEvent *ev)
{
    while (!spsc_push(ctx->s->disc_event_ring, ev))
        ;
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
            SshStatus st = ssh_channel_open(ctx->ssh, &j->ch);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
            j->state = DJOB_EXEC;
            again    = true;
            break;
        }
        case DJOB_EXEC: {
            SshStatus st = ssh_channel_exec(j->ch, j->cmd);
            if (st == SSH_AGAIN) return;
            if (st != SSH_OK) { fail_disc_job(ctx, i, DISC_ERR_COMMAND_FAILED); return; }
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
                if (ds != DISC_OK) { fail_disc_job(ctx, i, ds); return; }
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
        if (ctx->disc_jobs[i].kind == DJOB_KIND_NONE) continue;
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
        /* bound poll while disc jobs are in flight so reads stay responsive */
        if (has_active_disc_jobs(ctx)) return 10;
        return ctx->keepalive_next * 1000;
    }
    return -1;
}

/* ── worker thread ─────────────────────────────────────────────────── */

static void *worker_fn(void *arg)
{
    Session *s = arg;

    WorkerCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.s              = s;
    ctx.ssh_fd         = -1;
    ctx.sub            = SUB_IDLE;
    ctx.keepalive_next = KEEPALIVE_INTERVAL_SEC;
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

    connstate_init(&ctx.cs);

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

        if (ctx.sub == SUB_ONLINE && ctx.ssh)
            drive_disc_jobs(&ctx);
    }

    if (ctx.ssh) ssh_disconnect(ctx.ssh);
    for (int i = 0; i < DISC_MAX_JOBS; i++) {
        if (ctx.disc_arenas[i]) arena_destroy(ctx.disc_arenas[i]);
    }
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

    int pipefd[2];
    if (pipe(pipefd) != 0) {
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
