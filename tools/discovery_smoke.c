/* Dev-only smoke: connect → SCAN HOST → print blueprints → optionally
   READ_BLUEPRINT → RESOLVE_BUNDLE_ID → close.
   Usage: discovery_smoke <host> <user> <scan_root> [port] [depth] [--abort]
                          [--blueprint <path>]
   Not part of `make test`. */
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
                        " [port] [depth] [--abort] [--blueprint <path>]\n");
        return 1;
    }

    const char *host       = argv[1];
    const char *user       = argv[2];
    const char *scan_root  = argv[3];
    int         port       = 22;
    int         max_depth  = 0;  /* 0 → engine default (8) */
    bool        do_abort   = false;
    const char *blueprint  = NULL;  /* --blueprint <path> */

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--abort") == 0) {
            do_abort = true;
        } else if (strcmp(argv[i], "--blueprint") == 0 && i + 1 < argc) {
            blueprint = argv[++i];
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

    /* poll disc events until scan completes */
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
                goto scan_done;
            case DEV_SCAN_FAILED:
                printf("SCAN_FAILED: %s\n", disc_status_str(dev.disc_status));
                goto scan_done;
            default:
                break;
            }
        } else {
            sleep_ms(10);
        }
    }
scan_done:

    /* optionally drive READ_BLUEPRINT + RESOLVE_BUNDLE_ID */
    if (!blueprint) goto done;

    printf("\nREAD_BLUEPRINT: '%s'\n", blueprint);
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_READ_BLUEPRINT;
    snprintf(dcmd.project, sizeof(dcmd.project), "%s", blueprint);

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit(READ_BLUEPRINT): ring full\n");
        goto done;
    }

    char first_scheme[256] = {0};
    for (;;) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            switch (dev.kind) {
            case DEV_SCHEME:
                printf("  SCHEME: %s\n", dev.scheme);
                if (first_scheme[0] == '\0')
                    snprintf(first_scheme, sizeof(first_scheme), "%s", dev.scheme);
                break;
            case DEV_CONFIG:
                printf("  CONFIG: %s\n", dev.config);
                break;
            case DEV_BLUEPRINT_READ_COMPLETE:
                printf("BLUEPRINT_READ_COMPLETE: %d items\n", dev.count);
                goto read_done;
            case DEV_BLUEPRINT_FAILED:
                printf("BLUEPRINT_FAILED: %s\n", disc_status_str(dev.disc_status));
                goto done;
            default:
                break;
            }
        } else {
            sleep_ms(10);
        }
    }
read_done:

    if (first_scheme[0] == '\0') {
        printf("No schemes found; skipping RESOLVE_BUNDLE_ID\n");
        goto done;
    }

    printf("\nRESOLVE_BUNDLE_ID: scheme='%s' config='Debug'\n", first_scheme);
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
    snprintf(dcmd.project, sizeof(dcmd.project), "%s", blueprint);
    snprintf(dcmd.scheme,  sizeof(dcmd.scheme),  "%s", first_scheme);
    snprintf(dcmd.config,  sizeof(dcmd.config),  "Debug");

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit(RESOLVE_BUNDLE_ID): ring full\n");
        goto done;
    }

    for (;;) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            switch (dev.kind) {
            case DEV_BUNDLE_ID:
                printf("BUNDLE_ID: %s\n", dev.bundle_id);
                goto done;
            case DEV_BUNDLE_ID_FAILED:
                printf("BUNDLE_ID_FAILED: %s\n", disc_status_str(dev.disc_status));
                goto done;
            default:
                break;
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
