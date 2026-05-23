#include "connstate.h"
#include <math.h>
#include <stdlib.h>

#define BACKOFF_BASE_SEC  2.0
#define BACKOFF_MAX_SEC  30.0
#define RECONNECT_BUDGET  6

static double default_rng01(void) {
    return (double)rand() / ((double)RAND_MAX + 1.0);
}

static double (*rng01_fn)(void) = default_rng01;

void connstate_set_rng(double (*fn)(void)) {
    rng01_fn = (fn != NULL) ? fn : default_rng01;
}

void connstate_init(ConnState *cs) {
    cs->phase       = CONN_DISCONNECTED;
    cs->attempt     = 0;
    cs->last_reason = SSH_OK;
}

static ConnAction reconnect_fail(ConnState *cs) {
    cs->attempt++;
    if (connstate_should_sever(cs->attempt)) {
        cs->phase = CONN_SEVERED;
        return ACT_SEVERED;
    }
    return ACT_SCHEDULE_BACKOFF;
}

ConnAction connstate_step(ConnState *cs, ConnEvent ev) {
    switch (cs->phase) {

    case CONN_DISCONNECTED:
        if (ev == EV_BREACH) {
            cs->phase       = CONN_CONNECTING;
            cs->attempt     = 0;
            cs->last_reason = SSH_OK;
            return ACT_START_CONNECT;
        }
        return ACT_NONE;

    case CONN_CONNECTING:
        switch (ev) {
        case EV_TCP_UP:
            return ACT_NONE;
        case EV_HOSTKEY_OK:
            return ACT_DO_AUTH;
        case EV_HOSTKEY_UNKNOWN:
            cs->phase = CONN_AWAITING_HOSTKEY;
            return ACT_WAIT_HOSTKEY;
        case EV_HOSTKEY_MISMATCH:
            cs->phase       = CONN_DISCONNECTED;
            cs->last_reason = SSH_ERR_HOSTKEY_MISMATCH;
            return ACT_SHOW_FAILURE;
        case EV_AUTH_OK:
            return ACT_DO_PROBE;
        case EV_AUTH_FAIL:
            cs->phase       = CONN_DISCONNECTED;
            cs->last_reason = SSH_ERR_AUTH;
            return ACT_SHOW_FAILURE;
        case EV_PROBE_OK:
            cs->phase   = CONN_ONLINE;
            cs->attempt = 0;
            return ACT_GO_ONLINE;
        case EV_PROBE_FAIL:
            cs->phase       = CONN_DISCONNECTED;
            cs->last_reason = SSH_ERR_NO_SHELL;
            return ACT_SHOW_FAILURE;
        case EV_FAIL:
            cs->phase = CONN_DISCONNECTED;
            return ACT_SHOW_FAILURE;
        case EV_ABORT:
        case EV_CLOSE:
            cs->phase = CONN_DISCONNECTED;
            return ACT_TEARDOWN;
        default:
            return ACT_NONE;
        }

    case CONN_AWAITING_HOSTKEY:
        switch (ev) {
        case EV_TRUST:
            cs->phase = CONN_CONNECTING;
            return ACT_DO_AUTH;
        case EV_DECLINE:
        case EV_ABORT:
            cs->phase = CONN_DISCONNECTED;
            return ACT_TEARDOWN;
        default:
            return ACT_NONE;
        }

    case CONN_ONLINE:
        switch (ev) {
        case EV_DROP:
        case EV_FAIL:
            cs->phase   = CONN_REACQUIRING;
            cs->attempt = 1;
            return ACT_SCHEDULE_BACKOFF;
        case EV_CLOSE:
            cs->phase = CONN_DISCONNECTED;
            return ACT_TEARDOWN;
        default:
            return ACT_NONE;
        }

    case CONN_REACQUIRING:
        switch (ev) {
        case EV_BACKOFF_EXPIRED:
            return ACT_START_CONNECT;
        case EV_TCP_UP:
            return ACT_NONE;
        case EV_HOSTKEY_OK:
            return ACT_DO_AUTH;
        case EV_HOSTKEY_UNKNOWN:
            cs->phase = CONN_AWAITING_HOSTKEY;
            return ACT_WAIT_HOSTKEY;
        case EV_HOSTKEY_MISMATCH:
            cs->phase       = CONN_DISCONNECTED;
            cs->last_reason = SSH_ERR_HOSTKEY_MISMATCH;
            return ACT_SHOW_FAILURE;
        case EV_AUTH_OK:
            return ACT_DO_PROBE;
        case EV_AUTH_FAIL:
            cs->last_reason = SSH_ERR_AUTH;
            return reconnect_fail(cs);
        case EV_PROBE_OK:
        case EV_RECONNECT_OK:
            cs->phase   = CONN_ONLINE;
            cs->attempt = 0;
            return ACT_GO_ONLINE;
        case EV_PROBE_FAIL:
            cs->last_reason = SSH_ERR_NO_SHELL;
            return reconnect_fail(cs);
        case EV_FAIL:
        case EV_DROP:
            return reconnect_fail(cs);
        case EV_ABORT:
        case EV_CLOSE:
            cs->phase = CONN_DISCONNECTED;
            return ACT_TEARDOWN;
        default:
            return ACT_NONE;
        }

    case CONN_SEVERED:
        if (ev == EV_BREACH) {
            cs->phase       = CONN_CONNECTING;
            cs->attempt     = 0;
            cs->last_reason = SSH_OK;
            return ACT_START_CONNECT;
        }
        return ACT_NONE;

    default:
        return ACT_NONE;
    }
}

double connstate_backoff_delay(int attempt) {
    if (attempt <= 0) return 0.0;
    double cap = BACKOFF_BASE_SEC * pow(2.0, (double)(attempt - 1));
    if (cap > BACKOFF_MAX_SEC) cap = BACKOFF_MAX_SEC;
    return rng01_fn() * cap;
}

bool connstate_should_sever(int attempt) {
    return attempt >= RECONNECT_BUDGET;
}

bool connstate_validate(const SshConfig *cfg) {
    if (!cfg)                                             return false;
    if (cfg->host[0] == '\0')                            return false;
    if (cfg->port < 1 || cfg->port > 65535)              return false;
    if (cfg->user[0] == '\0')                            return false;
    if (cfg->auth == SSH_AUTH_PASSWORD
        && cfg->passkey[0] == '\0')                      return false;
    return true;
}

LexKey connstate_reason_lex(SshStatus st) {
    switch (st) {
    case SSH_ERR_DNS:              return LEX_CONN_ERR_NO_ROUTE;
    case SSH_ERR_REFUSED:          return LEX_CONN_ERR_PORT_CLOSED;
    case SSH_ERR_TIMEOUT:          return LEX_CONN_ERR_TIMEOUT;
    case SSH_ERR_HANDSHAKE:        return LEX_CONN_ERR_TIMEOUT;
    case SSH_ERR_HOSTKEY_MISMATCH: return LEX_CONN_ERR_HOSTKEY_MISMATCH;
    case SSH_ERR_AUTH:             return LEX_CONN_ACCESS_DENIED;
    case SSH_ERR_NO_SHELL:         return LEX_CONN_ERR_NO_SHELL;
    case SSH_ERR_IO:               return LEX_CONN_ERR_NO_ROUTE;
    case SSH_ERR_OOM:              return LEX_CONN_ERR_NO_ROUTE;
    default:                       return LEX_CONN_ERR_NO_ROUTE;
    }
}

const char *connstate_phase_str(ConnPhase p) {
    switch (p) {
    case CONN_DISCONNECTED:     return "DISCONNECTED";
    case CONN_CONNECTING:       return "CONNECTING";
    case CONN_AWAITING_HOSTKEY: return "AWAITING_HOSTKEY";
    case CONN_ONLINE:           return "ONLINE";
    case CONN_REACQUIRING:      return "REACQUIRING";
    case CONN_SEVERED:          return "SEVERED";
    default:                    return "UNKNOWN";
    }
}
