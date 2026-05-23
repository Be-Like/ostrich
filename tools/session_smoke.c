/* Dev-only smoke: open session → CMD_BREACH → poll to ONLINE → close.
   Requires a real SSH server. Not part of `make test`. */
#include "session.h"
#include "connstate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: session_smoke <host> <user> [port]\n");
        return 1;
    }

    Session  *s  = NULL;
    SshStatus st = session_open(&s);
    if (st != SSH_OK) {
        fprintf(stderr, "session_open: %s\n", session_status_str(st));
        return 1;
    }
    printf("Worker started.\n");

    SessionCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = CMD_BREACH;
    snprintf(cmd.cfg.host, sizeof(cmd.cfg.host), "%s", argv[1]);
    snprintf(cmd.cfg.user, sizeof(cmd.cfg.user), "%s", argv[2]);
    cmd.cfg.port = (argc >= 4) ? atoi(argv[3]) : 22;
    cmd.cfg.auth = SSH_AUTH_AGENT;

    if (!session_submit(s, &cmd)) {
        fprintf(stderr, "session_submit: ring full\n");
        session_close(s);
        return 1;
    }
    printf("CMD_BREACH → %s:%d as %s\n",
           cmd.cfg.host, cmd.cfg.port, cmd.cfg.user);

    for (;;) {
        SessionEvent ev;
        if (session_poll(s, &ev)) {
            printf("Event: phase=%-18s reason=%s%s%s\n",
                   connstate_phase_str(ev.phase),
                   session_status_str(ev.reason),
                   ev.hostkey_unknown  ? " [unknown-hostkey]"  : "",
                   ev.hostkey_mismatch ? " [hostkey-mismatch]" : "");

            if (ev.hostkey_unknown) {
                printf("  fingerprint: %s — auto-trusting\n", ev.fingerprint);
                SessionCmd trust;
                memset(&trust, 0, sizeof(trust));
                trust.kind = CMD_TRUST;
                session_submit(s, &trust);
            }

            if (ev.phase == CONN_ONLINE) {
                printf("ONLINE at %s — sleeping 5s\n", ev.user_host);
                sleep(5);
                break;
            }

            if (ev.phase == CONN_DISCONNECTED || ev.phase == CONN_SEVERED) {
                fprintf(stderr, "Connection ended: %s\n",
                        session_status_str(ev.reason));
                session_close(s);
                return 1;
            }
        } else {
            usleep(10000); /* 10 ms */
        }
    }

    session_close(s);
    printf("Session closed cleanly.\n");
    return 0;
}
