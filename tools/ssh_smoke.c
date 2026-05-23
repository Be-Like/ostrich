/* Dev-only smoke: connect to a real Mac via ssh-agent, run the true
   probe, and print the result. Not part of `make test`. */
#include "ssh.h"
#include "arena.h"

#include <libssh2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void poll_fd(int fd)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLOUT };
    poll(&pfd, 1, 5000);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: ssh_smoke <host> <user> [port]\n");
        return 1;
    }

    Arena *arena = arena_create(1024 * 1024);
    if (!arena) { fprintf(stderr, "arena_create failed\n"); return 1; }

    SshConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.host, sizeof(cfg.host), "%s", argv[1]);
    snprintf(cfg.user, sizeof(cfg.user), "%s", argv[2]);
    cfg.port = (argc >= 4) ? atoi(argv[3]) : 22;
    cfg.auth = SSH_AUTH_AGENT;

    Ssh      *s  = NULL;
    int       fd = -1;
    SshStatus st = ssh_connect_start(arena, cfg, &s, &fd);
    if (st != SSH_OK) {
        fprintf(stderr, "ssh_connect_start: %s\n", ssh_status_str(st));
        return 1;
    }
    printf("Connecting to %s:%d as %s...\n", cfg.host, cfg.port, cfg.user);

    /* handshake loop */
    while ((st = ssh_handshake_step(s)) == SSH_AGAIN) poll_fd(fd);
    if (st != SSH_OK) {
        fprintf(stderr, "ssh_handshake_step: %s\n", ssh_status_str(st));
        ssh_disconnect(s);
        return 1;
    }
    printf("SSH handshake complete.\n");

    /* host key check */
    SshHostKeyVerdict verdict;
    char fp[128] = {0};
    st = ssh_hostkey_check(s, &verdict, fp, sizeof(fp));
    if (st != SSH_OK) {
        fprintf(stderr, "ssh_hostkey_check: %s\n", ssh_status_str(st));
        ssh_disconnect(s);
        return 1;
    }

    const char *vstr = (verdict == SSH_HOSTKEY_OK)       ? "OK" :
                       (verdict == SSH_HOSTKEY_UNKNOWN)   ? "UNKNOWN" :
                                                            "MISMATCH";
    printf("Host key verdict: %s  fingerprint: %s\n", vstr, fp);

    if (verdict == SSH_HOSTKEY_MISMATCH) {
        fprintf(stderr, "Host key mismatch — aborting.\n");
        ssh_disconnect(s);
        return 1;
    }

    if (verdict == SSH_HOSTKEY_UNKNOWN) {
        printf("Unknown host — trusting for smoke test...\n");
        st = ssh_hostkey_trust(s);
        if (st != SSH_OK) {
            fprintf(stderr, "ssh_hostkey_trust: %s\n", ssh_status_str(st));
            ssh_disconnect(s);
            return 1;
        }
    }

    /* agent auth loop */
    printf("Authenticating via ssh-agent...\n");
    while ((st = ssh_auth_step(s)) == SSH_AGAIN) poll_fd(fd);
    if (st != SSH_OK) {
        fprintf(stderr, "ssh_auth_step: %s\n", ssh_status_str(st));
        ssh_disconnect(s);
        return 1;
    }
    printf("Authenticated.\n");

    /* probe loop */
    printf("Running liveness probe (exec true)...\n");
    int exit_code = -1;
    while ((st = ssh_probe_step(s, &exit_code)) == SSH_AGAIN) poll_fd(fd);
    if (st != SSH_OK) {
        fprintf(stderr, "ssh_probe_step: %s (exit=%d)\n",
                ssh_status_str(st), exit_code);
        ssh_disconnect(s);
        return 1;
    }
    printf("Probe passed (exit=%d). Session ONLINE.\n", exit_code);

    ssh_disconnect(s);
    arena_destroy(arena);
    return 0;
}
