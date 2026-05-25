/* run_smoke.c — dev smoke: drive EXECUTE or COMPILE against a live Mac.
   Prints phase transitions and streaming log chunks as they arrive.

   Environment variables:
     RUN_HOST=user@host    required; if unset, prints SKIP and exits 77
     RUN_PROJECT=/path/to/Foo.xcodeproj or .xcworkspace   required
     RUN_SCHEME=MyScheme   required
     RUN_BUNDLE=com.example.App   required (for EXECUTE)
     RUN_CONFIG=Debug      optional; defaults to "Debug"
     RUN_UDID=DEVICE-UDID  optional; if set → EXECUTE, else → COMPILE
     RUN_SIM=1             optional; set if the UDID is a simulator
     OSTRICH_PASS=secret   optional; if set, use password auth instead of agent
     PORT=22               optional

   Not part of `make test`. */

#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "connstate.h"
#include "runstate.h"

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

/* Parse "user@host" into separate user and host buffers. */
static void parse_user_host(const char *uh, char *user, size_t usz,
                             char *host, size_t hsz)
{
    const char *at = strchr(uh, '@');
    if (at) {
        size_t ulen = (size_t)(at - uh);
        if (ulen >= usz) ulen = usz - 1;
        memcpy(user, uh, ulen);
        user[ulen] = '\0';
        snprintf(host, hsz, "%s", at + 1);
    } else {
        snprintf(user, usz, "root");
        snprintf(host, hsz, "%s", uh);
    }
}

int main(void)
{
    const char *run_host    = getenv("RUN_HOST");
    const char *run_project = getenv("RUN_PROJECT");
    const char *run_scheme  = getenv("RUN_SCHEME");
    const char *run_bundle  = getenv("RUN_BUNDLE");
    const char *run_config  = getenv("RUN_CONFIG");
    const char *run_udid    = getenv("RUN_UDID");
    const char *run_sim     = getenv("RUN_SIM");
    const char *port_str    = getenv("PORT");
    const char *pass        = getenv("OSTRICH_PASS");

    if (!run_host) {
        printf("SKIP: RUN_HOST not set\n");
        return 77;
    }
    if (!run_project) {
        fprintf(stderr, "RUN_PROJECT not set\n");
        return 1;
    }
    if (!run_scheme) {
        fprintf(stderr, "RUN_SCHEME not set\n");
        return 1;
    }
    if (!run_bundle && run_udid) {
        fprintf(stderr, "RUN_BUNDLE required when RUN_UDID is set (EXECUTE mode)\n");
        return 1;
    }

    int port = 22;
    if (port_str && port_str[0]) port = atoi(port_str);

    bool do_execute = (run_udid != NULL);
    printf("Mode: %s\n", do_execute ? "EXECUTE" : "COMPILE");

    /* Open session */
    Session  *s  = NULL;
    SshStatus st = session_open(&s);
    if (st != SSH_OK) {
        fprintf(stderr, "session_open: %s\n", session_status_str(st));
        return 1;
    }
    printf("Worker started.\n");

    /* Connect */
    SessionCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind     = CMD_BREACH;
    cmd.cfg.port = port;
    parse_user_host(run_host,
                    cmd.cfg.user, sizeof(cmd.cfg.user),
                    cmd.cfg.host, sizeof(cmd.cfg.host));

    if (pass && pass[0]) {
        cmd.cfg.auth = SSH_AUTH_PASSWORD;
        snprintf(cmd.cfg.passkey, sizeof(cmd.cfg.passkey), "%s", pass);
        printf("Auth: password\n");
    } else {
        cmd.cfg.auth = SSH_AUTH_AGENT;
        printf("Auth: ssh-agent\n");
    }

    if (!session_submit(s, &cmd)) {
        fprintf(stderr, "session_submit: ring full\n");
        session_close(s);
        return 1;
    }
    printf("Connecting to %s:%d as %s …\n", cmd.cfg.host, port, cmd.cfg.user);

    /* Wait for ONLINE */
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
    printf("ONLINE.\n\n");

    /* Build and submit the run command */
    SessionRunCmd rcmd;
    memset(&rcmd, 0, sizeof(rcmd));
    rcmd.kind = do_execute ? RCMD_EXECUTE : RCMD_COMPILE;
    snprintf(rcmd.cfg.project,   sizeof(rcmd.cfg.project),   "%s", run_project);
    snprintf(rcmd.cfg.scheme,    sizeof(rcmd.cfg.scheme),     "%s", run_scheme);
    snprintf(rcmd.cfg.config,    sizeof(rcmd.cfg.config),
             "%s", run_config ? run_config : "Debug");
    if (run_bundle)
        snprintf(rcmd.cfg.bundle_id, sizeof(rcmd.cfg.bundle_id), "%s", run_bundle);

    if (do_execute) {
        snprintf(rcmd.target.udid, sizeof(rcmd.target.udid), "%s", run_udid);
        snprintf(rcmd.target.name, sizeof(rcmd.target.name), "%s", run_udid);
        rcmd.target.is_simulator = (run_sim && run_sim[0] == '1');
        rcmd.target.booted       = true;
        rcmd.has_target          = true;
        printf("EXECUTE project=%s scheme=%s udid=%s%s\n",
               run_project, run_scheme, run_udid,
               rcmd.target.is_simulator ? " [sim]" : "");
    } else {
        printf("COMPILE project=%s scheme=%s\n", run_project, run_scheme);
    }

    if (!session_run_submit(s, &rcmd)) {
        fprintf(stderr, "session_run_submit: ring full\n");
        session_close(s);
        return 1;
    }

    /* Poll run events until terminal phase */
    bool done = false;
    int  rc   = 0;
    while (!done) {
        SessionRunEvent rev;
        if (session_run_poll(s, &rev)) {
            switch (rev.kind) {
            case REV_PHASE:
                printf("[PHASE] %s", runstate_phase_str(rev.phase));
                if (rev.reason != BD_OK)
                    printf(" (reason: %s)", bd_status_str(rev.reason));
                printf("\n");
                switch (rev.phase) {
                case RUN_IDLE:
                case RUN_BUILD_FAILED:
                case RUN_DEPLOY_FAILED:
                case RUN_ABORTED:
                    done = true;
                    rc = (rev.phase == RUN_IDLE) ? 0 : 1;
                    break;
                default:
                    break;
                }
                break;
            case REV_BUILD_LOG:
                fwrite(rev.chunk, 1, (size_t)rev.len, stdout);
                break;
            case REV_DEVICE_LOG:
                fwrite(rev.chunk, 1, (size_t)rev.len, stdout);
                break;
            case REV_STALE:
                printf("[STALE]\n");
                break;
            }
        } else {
            sleep_ms(10);
        }
    }

    session_close(s);
    printf("\nSession closed. Exit: %d\n", rc);
    return rc;
}
