#ifndef SSH_H
#define SSH_H

#include "arena.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SSH_OK = 0,
    SSH_AGAIN,
    SSH_ERR_DNS,
    SSH_ERR_REFUSED,
    SSH_ERR_TIMEOUT,
    SSH_ERR_HANDSHAKE,
    SSH_ERR_HOSTKEY_MISMATCH,
    SSH_ERR_AUTH,
    SSH_ERR_NO_SHELL,
    SSH_ERR_IO,
    SSH_ERR_OOM
} SshStatus;

typedef enum { SSH_AUTH_AGENT, SSH_AUTH_PASSWORD } SshAuth;

typedef struct {
    char    host[256];
    int     port;         /* default 22 */
    char    user[128];
    SshAuth auth;
    char    passkey[256]; /* used only for SSH_AUTH_PASSWORD */
} SshConfig;

typedef enum {
    SSH_HOSTKEY_OK,
    SSH_HOSTKEY_UNKNOWN,
    SSH_HOSTKEY_MISMATCH
} SshHostKeyVerdict;

typedef struct Ssh        Ssh;        /* opaque session  */
typedef struct SshChannel SshChannel; /* opaque channel  */

/* Start a non-blocking TCP+SSH connection. Returns the socket fd via
   out_fd for the caller to poll(). Returns SSH_OK if the connection
   attempt was successfully initiated (TCP may still be connecting). */
SshStatus   ssh_connect_start(Arena *a, SshConfig cfg,
                              Ssh **out, int *out_fd);

/* Drive the SSH handshake one step. Returns SSH_AGAIN while in
   progress, SSH_OK when the handshake is complete. */
SshStatus   ssh_handshake_step(Ssh *s);

/* Check the remote host key against ~/.ssh/known_hosts. Returns
   SSH_OK and writes the verdict and hex fingerprint to *verdict / fp.
   Caches the raw key for a subsequent ssh_hostkey_trust call. */
SshStatus   ssh_hostkey_check(Ssh *s, SshHostKeyVerdict *verdict,
                              char *fp, size_t fp_cap);

/* Append the cached host key to ~/.ssh/known_hosts. Call only after
   ssh_hostkey_check returned SSH_HOSTKEY_UNKNOWN. */
SshStatus   ssh_hostkey_trust(Ssh *s);

/* Drive one authentication step (agent or password). Returns
   SSH_AGAIN while in progress, SSH_OK on success. */
SshStatus   ssh_auth_step(Ssh *s);

/* Open an exec channel, run "true", and verify a clean exit.
   Returns SSH_AGAIN while in progress, SSH_OK on a zero exit,
   SSH_ERR_NO_SHELL otherwise. *exit_code is set on completion. */
SshStatus   ssh_probe_step(Ssh *s, int *exit_code);

/* Open an additional session channel (multi-channel-ready seam).
   Returns SSH_AGAIN while in progress, SSH_OK on success. */
SshStatus   ssh_channel_open(Ssh *s, SshChannel **out);

/* Start running cmd on an opened channel. SSH_AGAIN while the request
   is in flight, SSH_OK once accepted. */
SshStatus   ssh_channel_exec(SshChannel *ch, const char *cmd);

/* Read available stdout into buf. SSH_AGAIN if it would block; SSH_OK
   with *out_n bytes written (*out_n == 0 at EOF — confirm with
   ssh_channel_eof). */
SshStatus   ssh_channel_read(SshChannel *ch, char *buf, size_t cap,
                             size_t *out_n);

/* Returns true when the remote side has signalled EOF on the channel. */
bool        ssh_channel_eof(SshChannel *ch);

/* Drive close and collect exit code. SSH_AGAIN while the close
   handshake is in flight, SSH_OK once the exit code is available. */
SshStatus   ssh_channel_exit(SshChannel *ch, int *out_code);

/* Free all resources for the channel. Call after ssh_channel_exit
   returns SSH_OK (or to abort an in-flight channel). */
void        ssh_channel_close(SshChannel *ch);

/* Send a keepalive; sets *seconds_to_next to the suggested interval.
   Returns SSH_AGAIN if the send would block. */
SshStatus   ssh_keepalive(Ssh *s, int *seconds_to_next);

/* Disconnect and release all libssh2 resources. The Ssh struct itself
   is arena-allocated and is not freed here. */
void        ssh_disconnect(Ssh *s);

const char *ssh_status_str(SshStatus st);

#ifdef __cplusplus
}
#endif

#endif /* SSH_H */
