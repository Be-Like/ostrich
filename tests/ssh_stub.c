#define _POSIX_C_SOURCE 200809L

#include "../include/ssh.h"

#include <string.h>
#include <unistd.h>

/* Minimal SSH stub for session_exec_test.
   Simulates an immediate successful connection, then a single channel
   exec that returns "stub output\n" and exits 0.  No real network I/O. */

struct Ssh        { int dummy; };
struct SshChannel { int dummy; };

static struct Ssh        stub_ssh;
static struct SshChannel stub_ch;

/* pipe(2) fd pair: gives the worker a pollable fd that is always readable */
static int stub_pipe_rd = -1;
static int stub_pipe_wr = -1;

/* resets per channel_open so multiple jobs within one test work */
static int channel_output_sent = 0;

SshStatus ssh_connect_start(Arena *a, SshConfig cfg, Ssh **out, int *out_fd)
{
    (void)a; (void)cfg;
    int pipefd[2];
    if (pipe(pipefd) != 0) return SSH_ERR_IO;
    stub_pipe_rd = pipefd[0];
    stub_pipe_wr = pipefd[1];
    /* keep pipe readable so poll() returns immediately every iteration */
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

SshStatus ssh_probe_step(Ssh *s, int *exit_code)
{
    (void)s;
    *exit_code = 0;
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
    channel_output_sent = 0;
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
    return channel_output_sent != 0;
}

SshStatus ssh_channel_read(SshChannel *ch, char *buf, size_t cap, size_t *n)
{
    (void)ch;
    if (channel_output_sent) { *n = 0; return SSH_AGAIN; }
    const char *data = "stub output\n";
    size_t len = strlen(data);
    if (len > cap) len = cap;
    memcpy(buf, data, len);
    *n = len;
    channel_output_sent = 1;
    return SSH_OK;
}

SshStatus ssh_channel_exit(SshChannel *ch, int *code)
{
    (void)ch;
    *code = 0;
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
