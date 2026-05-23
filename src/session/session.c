#define _POSIX_C_SOURCE 200809L

#include "session.h"
#include "arena.h"
#include "connstate.h"
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

#define CMD_RING_CAP           16
#define EVENT_RING_CAP         32
#define WORKER_ARENA_SZ        (512 * 1024)
#define CONNECT_TIMEOUT_SEC    30.0
#define KEEPALIVE_INTERVAL_SEC 30

/* ── Session control block (flagged malloc) ──────────────────────── */

struct Session {
    SpscRing  *cmd_ring;    /* UI→worker: SessionCmd records   */
    SpscRing  *event_ring;  /* worker→UI: SessionEvent records */
    int        pipe_read;   /* worker reads wakeup bytes       */
    int        pipe_write;  /* UI writes to wake worker        */
    pthread_t  thread;
    atomic_int running;     /* 0 = worker must stop            */
};

/* ── worker-private sub-phase ─────────────────────────────────────── */

typedef enum {
    SUB_IDLE,          /* waiting for CMD_BREACH              */
    SUB_HANDSHAKE,     /* TCP connect + SSH handshake         */
    SUB_AWAIT_HOSTKEY, /* paused for CMD_TRUST / CMD_DECLINE  */
    SUB_AUTH,          /* authenticating                      */
    SUB_PROBE,         /* running liveness probe              */
    SUB_ONLINE,        /* connected, keepalive loop           */
    SUB_BACKOFF,       /* waiting before reconnect attempt    */
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

/* ── emit helper ──────────────────────────────────────────────────── */

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

/* ── disconnect / reset helpers ───────────────────────────────────── */

static void disconnect_ssh(WorkerCtx *ctx)
{
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

/* ── command handlers ─────────────────────────────────────────────── */

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
        /* handshake done — inspect host key */
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
            /* SSH_HOSTKEY_OK */
            connstate_step(&ctx->cs, EV_HOSTKEY_OK);
            ctx->sub = SUB_AUTH;
        }
        return; /* next iteration drives auth */

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
        return; /* next iteration drives probe */

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
        return; /* keepalive handled by poll timeout in main loop */
    }
}

/* ── poll timeout computation ─────────────────────────────────────── */

static int compute_timeout_ms(const WorkerCtx *ctx)
{
    switch (ctx->sub) {
    case SUB_IDLE:
    case SUB_AWAIT_HOSTKEY:
        return -1; /* block until woken via pipe */

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

        /* drain self-pipe wakeup bytes */
        if (fds[0].revents & POLLIN) {
            char buf[64];
            (void)read(s->pipe_read, buf, sizeof(buf));
        }

        /* drain command ring */
        drain_cmds(&ctx);

        /* keepalive: triggered only on poll timeout (nready == 0) */
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

        /* advance sub-phase state machine */
        drive_sub(&ctx);
    }

    if (ctx.ssh) ssh_disconnect(ctx.ssh);
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

    int pipefd[2];
    if (pipe(pipefd) != 0) {
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

void session_close(Session *s)
{
    if (!s) return;
    atomic_store(&s->running, 0);
    char byte = 0;
    (void)write(s->pipe_write, &byte, 1);
    pthread_join(s->thread, NULL);
    close(s->pipe_write);
    close(s->pipe_read);
    spsc_destroy(s->event_ring);
    spsc_destroy(s->cmd_ring);
    free(s);
}

const char *session_status_str(SshStatus st)
{
    return ssh_status_str(st);
}
