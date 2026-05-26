#define _POSIX_C_SOURCE 200809L

#include "ssh.h"
#include "log.h"

#include <libssh2.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* probe_state values */
#define PROBE_OPEN  0
#define PROBE_EXEC  1
#define PROBE_DRAIN 2
#define PROBE_CLOSE 3

struct Ssh {
    /* libssh2 handles */
    LIBSSH2_SESSION  *session;
    LIBSSH2_KNOWNHOSTS *known_hosts;

    int               sock_fd;
    SshConfig         cfg;
    Arena            *arena;

    /* TCP connect completion flag */
    int               tcp_connected;

    /* agent auth state */
    LIBSSH2_AGENT    *agent;
    int               agent_connected;
    int               agent_listed;
    struct libssh2_agent_publickey *agent_prev;
    struct libssh2_agent_publickey *agent_identity;

    /* probe state machine */
    LIBSSH2_CHANNEL  *probe_channel;
    int               probe_state;

    /* host key cached for trust operation */
    const char       *hostkey;
    size_t            hostkey_len;
    int               hostkey_type;

    char              known_hosts_path[512];
};

struct SshChannel {
    LIBSSH2_CHANNEL *channel;
    bool             merge_applied;
};

/* ── helpers ─────────────────────────────────────────────────────────── */

static int s_libssh2_initialized = 0;

static int hostkey_typemask(int key_type)
{
    int base = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    switch (key_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return base | LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return base | LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return base | LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return base | LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return base | LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return base | LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return base | LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    }
}

/* ── public API ──────────────────────────────────────────────────────── */

SshStatus ssh_connect_start(Arena *a, SshConfig cfg, Ssh **out, int *out_fd)
{
    *out    = NULL;
    *out_fd = -1;

    if (!s_libssh2_initialized) {
        if (libssh2_init(0) != 0) return SSH_ERR_IO;
        s_libssh2_initialized = 1;
    }

    int port = cfg.port ? cfg.port : 22;

    /* DNS resolution */
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(cfg.host, portstr, &hints, &res) != 0 || !res)
        return SSH_ERR_DNS;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return SSH_ERR_IO;
    }

    /* non-blocking socket */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    int tcp_connected = 0;
    if (rc == 0) {
        tcp_connected = 1;
    } else if (errno != EINPROGRESS) {
        close(fd);
        return (errno == ECONNREFUSED) ? SSH_ERR_REFUSED : SSH_ERR_IO;
    }

    Ssh *s = arena_alloc(a, sizeof(Ssh), _Alignof(Ssh));
    if (!s) {
        close(fd);
        return SSH_ERR_OOM;
    }
    memset(s, 0, sizeof(*s));

    s->session = libssh2_session_init();
    if (!s->session) {
        close(fd);
        return SSH_ERR_OOM;
    }
    libssh2_session_set_blocking(s->session, 0);
    libssh2_keepalive_config(s->session, 1, 30);

    s->known_hosts = libssh2_knownhost_init(s->session);
    if (!s->known_hosts) {
        libssh2_session_free(s->session);
        close(fd);
        return SSH_ERR_OOM;
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(s->known_hosts_path, sizeof(s->known_hosts_path),
                 "%s/.ssh/known_hosts", home);
        /* ignore read error — file may not exist yet */
        libssh2_knownhost_readfile(s->known_hosts, s->known_hosts_path,
                                   LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }

    s->arena         = a;
    s->cfg           = cfg;
    s->cfg.port      = port;
    s->sock_fd       = fd;
    s->tcp_connected = tcp_connected;

    *out    = s;
    *out_fd = fd;
    return SSH_OK;
}

SshStatus ssh_handshake_step(Ssh *s)
{
    if (!s->tcp_connected) {
        /* non-blocking check: is the socket writable yet? */
        struct pollfd pfd = { .fd = s->sock_fd, .events = POLLOUT | POLLERR };
        int ready = poll(&pfd, 1, 0);
        if (ready <= 0) return SSH_AGAIN;

        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s->sock_fd, SOL_SOCKET, SO_ERROR, &err, &len);

        if (err == ECONNREFUSED) {
            LOG_WARN(LG_SSH, "TCP connect refused %s:%d", s->cfg.host, s->cfg.port);
            return SSH_ERR_REFUSED;
        }
        if (err == ETIMEDOUT) {
            LOG_WARN(LG_SSH, "TCP connect timed out %s:%d", s->cfg.host, s->cfg.port);
            return SSH_ERR_TIMEOUT;
        }
        if (err != 0) {
            LOG_WARN(LG_SSH, "TCP socket error %d %s:%d", err, s->cfg.host, s->cfg.port);
            return SSH_ERR_IO;
        }

        s->tcp_connected = 1;
    }

    int rc = libssh2_session_handshake(s->session, s->sock_fd);
    if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
    if (rc < 0) {
#ifdef OSTRICH_DEBUG
        char *errmsg = NULL;
        int lssh_err = libssh2_session_last_error(s->session, &errmsg, NULL, 0);
        LOG_WARN(LG_SSH, "SSH handshake failed: err=%d %s",
                 lssh_err, errmsg ? errmsg : "");
#endif
        return SSH_ERR_HANDSHAKE;
    }
    return SSH_OK;
}

SshStatus ssh_hostkey_check(Ssh *s, SshHostKeyVerdict *verdict,
                            char *fp, size_t fp_cap)
{
    size_t     key_len  = 0;
    int        key_type = 0;
    const char *key = libssh2_session_hostkey(s->session, &key_len, &key_type);
    if (!key) return SSH_ERR_HANDSHAKE;

    s->hostkey      = key;
    s->hostkey_len  = key_len;
    s->hostkey_type = key_type;

    int check_port = s->cfg.port;
    struct libssh2_knownhost *found = NULL;
    int check = libssh2_knownhost_checkp(s->known_hosts,
                                         s->cfg.host, check_port,
                                         key, key_len,
                                         hostkey_typemask(key_type),
                                         &found);

    /* build hex fingerprint from SHA-256 hash */
    if (fp && fp_cap > 0) {
        const char *hash = libssh2_hostkey_hash(s->session,
                                                LIBSSH2_HOSTKEY_HASH_SHA256);
        if (hash) {
            size_t pos = 0;
            for (int i = 0; i < 32 && pos + 3 < fp_cap; i++) {
                pos += (size_t)snprintf(fp + pos, fp_cap - pos,
                                        "%02x", (unsigned char)hash[i]);
            }
            fp[pos < fp_cap ? pos : fp_cap - 1] = '\0';
        } else {
            snprintf(fp, fp_cap, "(unavailable)");
        }
    }

    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:    *verdict = SSH_HOSTKEY_OK;       break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: *verdict = SSH_HOSTKEY_UNKNOWN;  break;
    default:                               *verdict = SSH_HOSTKEY_MISMATCH; break;
    }
    return SSH_OK;
}

SshStatus ssh_hostkey_trust(Ssh *s)
{
    if (!s->hostkey) return SSH_ERR_IO;

    /* build the host entry: [host]:port for non-22, else plain host */
    char host_entry[320];
    if (s->cfg.port == 22) {
        snprintf(host_entry, sizeof(host_entry), "%s", s->cfg.host);
    } else {
        snprintf(host_entry, sizeof(host_entry), "[%s]:%d",
                 s->cfg.host, s->cfg.port);
    }

    struct libssh2_knownhost *store = NULL;
    int rc = libssh2_knownhost_addc(s->known_hosts,
                                    host_entry, NULL,
                                    s->hostkey, s->hostkey_len,
                                    NULL, 0,
                                    hostkey_typemask(s->hostkey_type),
                                    &store);
    if (rc < 0) return SSH_ERR_IO;

    if (s->known_hosts_path[0]) {
        rc = libssh2_knownhost_writefile(s->known_hosts,
                                         s->known_hosts_path,
                                         LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        if (rc < 0) return SSH_ERR_IO;
    }
    return SSH_OK;
}

SshStatus ssh_auth_step(Ssh *s)
{
    if (s->cfg.auth == SSH_AUTH_PASSWORD) {
        int rc = libssh2_userauth_password(s->session,
                                           s->cfg.user, s->cfg.passkey);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc < 0) {
#ifdef OSTRICH_DEBUG
            char *errmsg = NULL;
            int lssh_err = libssh2_session_last_error(s->session, &errmsg, NULL, 0);
            LOG_WARN(LG_SSH, "password auth failed user=%s: err=%d %s",
                     s->cfg.user, lssh_err, errmsg ? errmsg : "");
#endif
            return SSH_ERR_AUTH;
        }
        return SSH_OK;
    }

    /* SSH_AUTH_AGENT */
    if (!s->agent) {
        s->agent = libssh2_agent_init(s->session);
        if (!s->agent) return SSH_ERR_IO;
    }

    if (!s->agent_connected) {
        int rc = libssh2_agent_connect(s->agent);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc < 0) {
#ifdef OSTRICH_DEBUG
            char *errmsg = NULL;
            int lssh_err = libssh2_session_last_error(s->session, &errmsg, NULL, 0);
            LOG_WARN(LG_SSH, "agent connect failed: err=%d %s",
                     lssh_err, errmsg ? errmsg : "");
#endif
            return SSH_ERR_AUTH;
        }
        s->agent_connected = 1;
    }

    if (!s->agent_listed) {
        int rc = libssh2_agent_list_identities(s->agent);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc < 0) {
#ifdef OSTRICH_DEBUG
            char *errmsg = NULL;
            int lssh_err = libssh2_session_last_error(s->session, &errmsg, NULL, 0);
            LOG_WARN(LG_SSH, "agent list identities failed: err=%d %s",
                     lssh_err, errmsg ? errmsg : "");
#endif
            return SSH_ERR_AUTH;
        }
        s->agent_listed = 1;
    }

    /* iterate through identities until one succeeds */
    while (1) {
        if (!s->agent_identity) {
            int rc = libssh2_agent_get_identity(s->agent,
                                                &s->agent_identity,
                                                s->agent_prev);
            if (rc == 1) {
                LOG_WARN(LG_SSH, "agent auth: no identity accepted for user=%s",
                         s->cfg.user);
                return SSH_ERR_AUTH; /* no more identities */
            }
            if (rc < 0)  return SSH_ERR_AUTH;
        }

        int rc = libssh2_agent_userauth(s->agent, s->cfg.user,
                                        s->agent_identity);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc == 0)                    return SSH_OK;

        /* this identity failed — try the next one */
        s->agent_prev     = s->agent_identity;
        s->agent_identity = NULL;
    }
}

SshStatus ssh_probe_step(Ssh *s, int *exit_code)
{
    /* state 0: open channel */
    if (s->probe_state == PROBE_OPEN) {
        s->probe_channel = libssh2_channel_open_session(s->session);
        if (!s->probe_channel) {
            if (libssh2_session_last_errno(s->session) == LIBSSH2_ERROR_EAGAIN)
                return SSH_AGAIN;
            LOG_WARN(LG_SSH, "probe: channel open failed");
            return SSH_ERR_NO_SHELL;
        }
        s->probe_state = PROBE_EXEC;
    }

    /* state 1: exec "true" */
    if (s->probe_state == PROBE_EXEC) {
        int rc = libssh2_channel_exec(s->probe_channel, "true");
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc < 0) {
            LOG_WARN(LG_SSH, "probe: exec 'true' failed");
            libssh2_channel_free(s->probe_channel);
            s->probe_channel = NULL;
            s->probe_state   = PROBE_OPEN;
            return SSH_ERR_NO_SHELL;
        }
        s->probe_state = PROBE_DRAIN;
    }

    /* state 2: drain stdout until remote EOF */
    if (s->probe_state == PROBE_DRAIN) {
        while (!libssh2_channel_eof(s->probe_channel)) {
            char   buf[256];
            ssize_t n = libssh2_channel_read(s->probe_channel,
                                             buf, sizeof(buf));
            if (n == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
            if (n < 0) {
                LOG_WARN(LG_SSH, "probe: read failed");
                libssh2_channel_free(s->probe_channel);
                s->probe_channel = NULL;
                s->probe_state   = PROBE_OPEN;
                return SSH_ERR_NO_SHELL;
            }
        }
        s->probe_state = PROBE_CLOSE;
    }

    /* state 3: close channel and collect exit code */
    if (s->probe_state == PROBE_CLOSE) {
        int rc = libssh2_channel_close(s->probe_channel);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;

        *exit_code = libssh2_channel_get_exit_status(s->probe_channel);
        libssh2_channel_free(s->probe_channel);
        s->probe_channel = NULL;
        s->probe_state   = PROBE_OPEN;

        if (*exit_code != 0)
            LOG_WARN(LG_SSH, "probe: 'true' exited %d", *exit_code);
        return (*exit_code == 0) ? SSH_OK : SSH_ERR_NO_SHELL;
    }

    return SSH_ERR_IO; /* unreachable */
}

SshStatus ssh_channel_open(Ssh *s, SshChannel **out)
{
    /* Idempotent on retry: allocate the SshChannel slot once on the first
       call, then reuse it across SSH_AGAIN retries. libssh2 tracks open-
       session progress internally — each retry only needs the same handle
       to write the result into. Without this, the caller's tight retry
       loop leaks an 8-byte arena allocation per call and exhausts the
       worker arena (see ssh_channel_open callers in session.c). */
    SshChannel *ch = *out;
    if (!ch) {
        ch = arena_alloc(s->arena, sizeof(SshChannel), _Alignof(SshChannel));
        if (!ch) return SSH_ERR_OOM;
        ch->channel       = NULL;
        ch->merge_applied = false;
        *out = ch;
    }

    /* Step 1: open the session channel (idempotent: skip if already open). */
    if (!ch->channel) {
        ch->channel = libssh2_channel_open_session(s->session);
        if (!ch->channel) {
            if (libssh2_session_last_errno(s->session) == LIBSSH2_ERROR_EAGAIN)
                return SSH_AGAIN;
            return SSH_ERR_IO;
        }
    }

    /* Step 2: merge stderr into stdout so xcodebuild's stderr-bound
       diagnostics reach the Build Log and don't stall the channel's
       flow-control window. Applied once; retried idempotently on EAGAIN. */
    if (!ch->merge_applied) {
        int rc = libssh2_channel_handle_extended_data2(
                     ch->channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE);
        if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
        if (rc != 0) {
            libssh2_channel_free(ch->channel);
            ch->channel = NULL;
            return SSH_ERR_IO;
        }
        ch->merge_applied = true;
    }

    return SSH_OK;
}

SshStatus ssh_channel_exec(SshChannel *ch, const char *cmd)
{
    int rc = libssh2_channel_exec(ch->channel, cmd);
    if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
    if (rc < 0)                     return SSH_ERR_IO;
    return SSH_OK;
}

SshStatus ssh_channel_read(SshChannel *ch, char *buf, size_t cap,
                           size_t *out_n)
{
    ssize_t n = libssh2_channel_read(ch->channel, buf, cap);
    if (n == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
    if (n < 0)                     return SSH_ERR_IO;
    *out_n = (size_t)n;
    return SSH_OK;
}

bool ssh_channel_eof(SshChannel *ch)
{
    return libssh2_channel_eof(ch->channel) != 0;
}

SshStatus ssh_channel_exit(SshChannel *ch, int *out_code)
{
    int rc = libssh2_channel_close(ch->channel);
    if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
    if (rc < 0)                     return SSH_ERR_IO;
    *out_code = libssh2_channel_get_exit_status(ch->channel);
    return SSH_OK;
}

void ssh_channel_close(SshChannel *ch)
{
    if (!ch || !ch->channel) return;
    libssh2_channel_free(ch->channel);
    ch->channel = NULL;
}

SshStatus ssh_keepalive(Ssh *s, int *seconds_to_next)
{
    int rc = libssh2_keepalive_send(s->session, seconds_to_next);
    if (rc == LIBSSH2_ERROR_EAGAIN) return SSH_AGAIN;
    if (rc < 0)                     return SSH_ERR_IO;
    return SSH_OK;
}

void ssh_disconnect(Ssh *s)
{
    if (!s) return;

    if (s->probe_channel) {
        libssh2_channel_close(s->probe_channel);
        libssh2_channel_free(s->probe_channel);
        s->probe_channel = NULL;
    }

    if (s->agent) {
        libssh2_agent_disconnect(s->agent);
        libssh2_agent_free(s->agent);
        s->agent = NULL;
    }

    if (s->known_hosts) {
        libssh2_knownhost_free(s->known_hosts);
        s->known_hosts = NULL;
    }

    if (s->session) {
        libssh2_session_disconnect(s->session, "Normal shutdown");
        libssh2_session_free(s->session);
        s->session = NULL;
    }

    if (s->sock_fd >= 0) {
        close(s->sock_fd);
        s->sock_fd = -1;
    }
}

const char *ssh_status_str(SshStatus st)
{
    switch (st) {
    case SSH_OK:                  return "OK";
    case SSH_AGAIN:               return "AGAIN";
    case SSH_ERR_DNS:             return "DNS resolution failed";
    case SSH_ERR_REFUSED:         return "connection refused";
    case SSH_ERR_TIMEOUT:         return "connection timed out";
    case SSH_ERR_HANDSHAKE:       return "SSH handshake failed";
    case SSH_ERR_HOSTKEY_MISMATCH: return "host key mismatch";
    case SSH_ERR_AUTH:            return "authentication failed";
    case SSH_ERR_NO_SHELL:        return "shell denied";
    case SSH_ERR_IO:              return "I/O error";
    case SSH_ERR_OOM:             return "out of memory";
    default:                      return "unknown error";
    }
}
