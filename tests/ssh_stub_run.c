/* ssh_stub_run.c — configurable SSH stub for session_run_test.
   Simulates a sequence of channel execs, each with its own output and exit
   code.  The last response can be set as "streaming" (does not EOF until
   g_run_stop_stream is set).  A response with stall_read=true makes
   ssh_channel_read always return SSH_AGAIN and ssh_channel_eof return false,
   which is used to exercise the stall watchdog. */

#define _POSIX_C_SOURCE 200809L
#include "../include/ssh.h"

#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#define STUB_RUN_MAX_RESP 8

typedef struct {
    const char *output;
    size_t      output_len;
    int         exit_code;
    bool        streaming;  /* hold open until g_run_stop_stream */
    bool        stall_read; /* read always returns SSH_AGAIN; eof always false */
} StubRunResp;

StubRunResp g_run_resp[STUB_RUN_MAX_RESP];
int         g_run_resp_count = 0;

/* Test sets this to 1 to make the streaming channel EOF. */
volatile int g_run_stop_stream = 0;

struct Ssh        { int dummy; };
struct SshChannel { int idx; };

static struct Ssh        stub_ssh;
static struct SshChannel stub_ch;

static int stub_pipe_rd = -1;
static int stub_pipe_wr = -1;

int g_exec_next  = 0;  /* next response index to assign on open */
int g_cur_idx    = -1; /* current channel's response index      */
int g_bytes_sent = 0;  /* bytes sent for current exec           */

/* Called by reset_stub() in the test to reset per-test state. */
void stub_run_reset(void)
{
    g_run_resp_count  = 0;
    g_run_stop_stream = 0;
    g_exec_next       = 0;
    g_cur_idx         = -1;
    g_bytes_sent      = 0;
}

SshStatus ssh_connect_start(Arena *a, SshConfig cfg, Ssh **out, int *out_fd)
{
    (void)a; (void)cfg;
    int pipefd[2];
    if (pipe(pipefd) != 0) return SSH_ERR_IO;
    stub_pipe_rd = pipefd[0];
    stub_pipe_wr = pipefd[1];
    /* keep pipe readable so poll() returns each iteration */
    char b = 1;
    (void)write(stub_pipe_wr, &b, 1);
    *out    = &stub_ssh;
    *out_fd = stub_pipe_rd;
    return SSH_OK;
}

SshStatus ssh_handshake_step(Ssh *s) { (void)s; return SSH_OK; }

SshStatus ssh_hostkey_check(Ssh *s, SshHostKeyVerdict *v, char *fp, size_t cap)
{
    (void)s; (void)fp; (void)cap;
    *v = SSH_HOSTKEY_OK;
    return SSH_OK;
}

SshStatus ssh_hostkey_trust(Ssh *s) { (void)s; return SSH_OK; }
SshStatus ssh_auth_step(Ssh *s)     { (void)s; return SSH_OK; }

SshStatus ssh_probe_step(Ssh *s, int *code)
{
    (void)s;
    *code = 0;
    return SSH_OK;
}

SshStatus ssh_keepalive(Ssh *s, int *next)
{
    (void)s;
    *next = 30;
    return SSH_OK;
}

void ssh_disconnect(Ssh *s)
{
    (void)s;
    if (stub_pipe_wr >= 0) { close(stub_pipe_wr); stub_pipe_wr = -1; }
    if (stub_pipe_rd >= 0) { close(stub_pipe_rd); stub_pipe_rd = -1; }
}

SshStatus ssh_channel_open(Ssh *s, SshChannel **out)
{
    (void)s;
    g_cur_idx    = g_exec_next++;
    g_bytes_sent = 0;
    stub_ch.idx  = g_cur_idx;
    *out = &stub_ch;
    return SSH_OK;
}

SshStatus ssh_channel_exec(SshChannel *ch, const char *cmd)
{
    (void)ch; (void)cmd;
    return SSH_OK;
}

bool ssh_channel_eof(SshChannel *ch)
{
    (void)ch;
    int idx = g_cur_idx;
    if (idx < 0 || idx >= g_run_resp_count) return true;
    StubRunResp *r = &g_run_resp[idx];
    if (r->stall_read) return false;
    if (r->streaming) return (g_run_stop_stream != 0) && (g_bytes_sent > 0);
    return g_bytes_sent > 0;
}

SshStatus ssh_channel_read(SshChannel *ch, char *buf, size_t cap, size_t *n)
{
    (void)ch;
    int idx = g_cur_idx;
    if (idx < 0 || idx >= g_run_resp_count) {
        *n = 0;
        return SSH_AGAIN;
    }
    StubRunResp *r = &g_run_resp[idx];

    /* stall_read: never produce bytes and never EOF */
    if (r->stall_read) {
        *n = 0;
        return SSH_AGAIN;
    }

    /* already sent output for this exec */
    if (g_bytes_sent > 0) {
        *n = 0;
        return SSH_AGAIN;
    }

    size_t len = r->output_len;
    if (len > cap) len = cap;
    if (len > 0) memcpy(buf, r->output, len);
    *n = len;
    g_bytes_sent = 1;
    return SSH_OK;
}

SshStatus ssh_channel_exit(SshChannel *ch, int *code)
{
    (void)ch;
    int idx = g_cur_idx;
    *code = (idx >= 0 && idx < g_run_resp_count) ? g_run_resp[idx].exit_code : 0;
    return SSH_OK;
}

void ssh_channel_close(SshChannel *ch) { (void)ch; }

const char *ssh_status_str(SshStatus st)
{
    switch (st) {
    case SSH_OK:                   return "ok";
    case SSH_AGAIN:                return "again";
    case SSH_ERR_DNS:              return "err:dns";
    case SSH_ERR_REFUSED:          return "err:refused";
    case SSH_ERR_TIMEOUT:          return "err:timeout";
    case SSH_ERR_HANDSHAKE:        return "err:handshake";
    case SSH_ERR_HOSTKEY_MISMATCH: return "err:hostkey-mismatch";
    case SSH_ERR_AUTH:             return "err:auth";
    case SSH_ERR_NO_SHELL:         return "err:no-shell";
    case SSH_ERR_IO:               return "err:io";
    case SSH_ERR_OOM:              return "err:oom";
    default:                       return "err:?";
    }
}
