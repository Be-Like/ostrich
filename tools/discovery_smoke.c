/* Dev-only smoke: connect → SCAN HOST → print blueprints → close.
   Usage: discovery_smoke <host> <user> <scan_root> [port] [depth] [--abort]
   Not part of `make test`. Pass --abort to abort after the first blueprint. */
#define _DEFAULT_SOURCE
#include "session.h"
#include "connstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: discovery_smoke <host> <user> <scan_root>"
                        " [port] [depth] [--abort]\n");
        return 1;
    }

    const char *host      = argv[1];
    const char *user      = argv[2];
    const char *scan_root = argv[3];
    int         port      = 22;
    int         max_depth = 0;  /* 0 → engine default (8) */
    bool        do_abort  = false;

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--abort") == 0) {
            do_abort = true;
        } else {
            int v = atoi(argv[i]);
            if (v > 0 && v <= 65535 && port == 22 && v != 22)
                port = v;
            else if (v > 0)
                max_depth = v;
        }
    }

    Session  *s  = NULL;
    SshStatus st = session_open(&s);
    if (st != SSH_OK) {
        fprintf(stderr, "session_open: %s\n", session_status_str(st));
        return 1;
    }
    printf("Worker started.\n");

    /* initiate connection */
    SessionCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind     = CMD_BREACH;
    cmd.cfg.port = port;
    cmd.cfg.auth = SSH_AUTH_AGENT;
    snprintf(cmd.cfg.host, sizeof(cmd.cfg.host), "%s", host);
    snprintf(cmd.cfg.user, sizeof(cmd.cfg.user), "%s", user);

    if (!session_submit(s, &cmd)) {
        fprintf(stderr, "session_submit: ring full\n");
        session_close(s);
        return 1;
    }
    printf("CMD_BREACH → %s:%d as %s\n", host, port, user);

    /* poll until ONLINE */
    for (;;) {
        SessionEvent ev;
        if (session_poll(s, &ev)) {
            printf("Conn: %s\n", connstate_phase_str(ev.phase));
            if (ev.hostkey_unknown) {
                printf("  fingerprint: %s — auto-trusting\n", ev.fingerprint);
                SessionCmd trust;
                memset(&trust, 0, sizeof(trust));
                trust.kind = CMD_TRUST;
                session_submit(s, &trust);
            }
            if (ev.phase == CONN_ONLINE) break;
            if (ev.phase == CONN_DISCONNECTED || ev.phase == CONN_SEVERED) {
                fprintf(stderr, "Connection failed: %s\n",
                        session_status_str(ev.reason));
                session_close(s);
                return 1;
            }
        } else {
            sleep_ms(10);
        }
    }

    printf("ONLINE — scanning '%s' (depth=%s)\n",
           scan_root, max_depth ? "custom" : "default");

    /* submit scan */
    SessionDiscCmd dcmd;
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind      = DCMD_SCAN_HOST;
    dcmd.max_depth = max_depth;
    snprintf(dcmd.root, sizeof(dcmd.root), "%s", scan_root);

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit: ring full\n");
        session_close(s);
        return 1;
    }

    /* poll disc events */
    bool abort_sent = false;
    for (;;) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            switch (dev.kind) {
            case DEV_BLUEPRINT:
                printf("  BLUEPRINT: %s%s\n",
                       dev.blueprint.path,
                       dev.blueprint.is_workspace ? " [workspace]" : "");
                if (do_abort && !abort_sent) {
                    printf("  [--abort: sending DCMD_ABORT_SCAN]\n");
                    SessionDiscCmd abort_cmd;
                    memset(&abort_cmd, 0, sizeof(abort_cmd));
                    abort_cmd.kind = DCMD_ABORT_SCAN;
                    session_disc_submit(s, &abort_cmd);
                    abort_sent = true;
                }
                break;
            case DEV_SCAN_COMPLETE:
                printf("SCAN_COMPLETE: %d blueprint(s) found\n", dev.count);
                goto done;
            case DEV_SCAN_FAILED:
                printf("SCAN_FAILED: %s\n", disc_status_str(dev.disc_status));
                goto done;
            }
        } else {
            sleep_ms(10);
        }
    }

done:
    session_close(s);
    printf("Session closed cleanly.\n");
    return 0;
}
