#ifndef CONNSTATE_H
#define CONNSTATE_H

#include "lexicon.h"
#include "ssh.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONN_DISCONNECTED,
    CONN_CONNECTING,
    CONN_AWAITING_HOSTKEY,
    CONN_ONLINE,
    CONN_REACQUIRING,
    CONN_SEVERED
} ConnPhase;

/* inputs to the machine (from the worker / timers / UI cmds) */
typedef enum {
    EV_BREACH,
    EV_TCP_UP,
    EV_HOSTKEY_UNKNOWN,
    EV_HOSTKEY_MISMATCH,
    EV_HOSTKEY_OK,
    EV_TRUST,
    EV_DECLINE,
    EV_AUTH_OK,
    EV_AUTH_FAIL,
    EV_PROBE_OK,
    EV_PROBE_FAIL,
    EV_FAIL,
    EV_ABORT,
    EV_CLOSE,
    EV_DROP,
    EV_RECONNECT_OK,
    EV_BACKOFF_EXPIRED
} ConnEvent;

/* actions the imperative shell must perform */
typedef enum {
    ACT_NONE,
    ACT_START_CONNECT,
    ACT_WAIT_HOSTKEY,
    ACT_DO_AUTH,
    ACT_DO_PROBE,
    ACT_GO_ONLINE,
    ACT_SHOW_FAILURE,
    ACT_SCHEDULE_BACKOFF,
    ACT_TEARDOWN,
    ACT_SEVERED
} ConnAction;

typedef struct {
    ConnPhase phase;
    int       attempt;     /* reconnect attempt count */
    SshStatus last_reason; /* for failure display     */
} ConnState;

void        connstate_init(ConnState *cs);
ConnAction  connstate_step(ConnState *cs, ConnEvent ev);
double      connstate_backoff_delay(int attempt); /* sec, with jitter */
bool        connstate_should_sever(int attempt);
bool        connstate_validate(const SshConfig *cfg);
LexKey      connstate_reason_lex(SshStatus st);
const char *connstate_phase_str(ConnPhase p);

/* Override the [0,1) RNG used by connstate_backoff_delay.
   Pass NULL to restore the default rand()-based source.
   For test determinism only. */
void connstate_set_rng(double (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* CONNSTATE_H */
