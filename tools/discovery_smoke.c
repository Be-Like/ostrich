/* Dev-only smoke: connect → SCAN HOST → print blueprints → optionally
   READ_BLUEPRINT → RESOLVE_BUNDLE_ID → SWEEP FOR TARGETS → close.
   Usage: discovery_smoke <host> <user> <scan_root> [port] [depth] [--abort]
                          [--blueprint <path>] [--sweep]
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
    bool        do_sweep   = false;
    const char *blueprint  = NULL;  /* --blueprint <path> */

    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--abort") == 0) {
            do_abort = true;
        } else if (strcmp(argv[i], "--sweep") == 0) {
            do_sweep = true;
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
    snprintf(cmd.cfg.host, sizeof(cmd.cfg.host), "%s", host);
    snprintf(cmd.cfg.user, sizeof(cmd.cfg.user), "%s", user);

    /* Auth: password from OSTRICH_PASS env when set, else ssh-agent.
       Reading from the env keeps the secret out of argv and shell history. */
    const char *pass = getenv("OSTRICH_PASS");
    if (pass && pass[0]) {
        cmd.cfg.auth = SSH_AUTH_PASSWORD;
        snprintf(cmd.cfg.passkey, sizeof(cmd.cfg.passkey), "%s", pass);
        printf("Auth: password (OSTRICH_PASS)\n");
    } else {
        cmd.cfg.auth = SSH_AUTH_AGENT;
        printf("Auth: ssh-agent\n");
    }

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
    if (!blueprint) goto bundle_done;

    printf("\nREAD_BLUEPRINT: '%s'\n", blueprint);
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_READ_BLUEPRINT;
    snprintf(dcmd.project, sizeof(dcmd.project), "%s", blueprint);

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit(READ_BLUEPRINT): ring full\n");
        goto bundle_done;
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
                goto bundle_done;
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
        goto bundle_done;
    }

    printf("\nRESOLVE_BUNDLE_ID: scheme='%s' config='Debug'\n", first_scheme);
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_RESOLVE_BUNDLE_ID;
    snprintf(dcmd.project, sizeof(dcmd.project), "%s", blueprint);
    snprintf(dcmd.scheme,  sizeof(dcmd.scheme),  "%s", first_scheme);
    snprintf(dcmd.config,  sizeof(dcmd.config),  "Debug");

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit(RESOLVE_BUNDLE_ID): ring full\n");
        goto bundle_done;
    }

    for (;;) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            switch (dev.kind) {
            case DEV_BUNDLE_ID:
                printf("BUNDLE_ID: %s\n", dev.bundle_id);
                goto bundle_done;
            case DEV_BUNDLE_ID_FAILED:
                printf("BUNDLE_ID_FAILED: %s\n", disc_status_str(dev.disc_status));
                goto bundle_done;
            default:
                break;
            }
        } else {
            sleep_ms(10);
        }
    }
bundle_done:

    if (!do_sweep) goto done;

    printf("\nSWEEP_TARGETS: devicectl + simctl\n");
    memset(&dcmd, 0, sizeof(dcmd));
    dcmd.kind = DCMD_SWEEP_TARGETS;

    if (!session_disc_submit(s, &dcmd)) {
        fprintf(stderr, "session_disc_submit(SWEEP_TARGETS): ring full\n");
        goto done;
    }

    for (;;) {
        SessionDiscEvent dev;
        if (session_disc_poll(s, &dev)) {
            switch (dev.kind) {
            case DEV_TARGET:
                printf("  TARGET: %s  udid=%s%s%s\n",
                       dev.target.name,
                       dev.target.udid,
                       dev.target.is_simulator ? " [sim]" : " [device]",
                       dev.target.booted       ? " [booted]" : "");
                break;
            case DEV_SWEEP_COMPLETE:
                printf("SWEEP_COMPLETE: %d target(s) found\n", dev.count);
                goto done;
            case DEV_SWEEP_FAILED:
                printf("SWEEP_FAILED: %s\n", disc_status_str(dev.disc_status));
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
