#include "runstate.h"

void runstate_init(RunState *rs) {
    rs->phase         = RUN_IDLE;
    rs->built_gen     = 0;
    rs->deployed_gen  = 0;
    rs->target_is_sim = false;
    rs->compile_only  = false;
}

static RunAction start_build(RunState *rs, bool compile_only) {
    rs->phase        = RUN_BUILDING;
    rs->compile_only = compile_only;
    return RUN_ACT_RESOLVE;
}

RunAction runstate_step(RunState *rs, RunEvent ev) {
    switch (rs->phase) {

    case RUN_IDLE:
        if (ev == RUN_EV_EXECUTE) return start_build(rs, false);
        if (ev == RUN_EV_COMPILE) return start_build(rs, true);
        return RUN_ACT_NONE;

    case RUN_BUILDING:
        switch (ev) {
        case RUN_EV_SETTINGS_OK:
            return RUN_ACT_BUILD;
        case RUN_EV_BUILD_OK:
            rs->built_gen++;
            if (rs->compile_only) {
                rs->compile_only = false;
                rs->phase        = RUN_IDLE;
                return RUN_ACT_DONE;
            }
            if (rs->target_is_sim) {
                rs->phase = RUN_PRIMING;
                return RUN_ACT_PRIME;
            }
            rs->phase = RUN_INSTALLING;
            return RUN_ACT_INSTALL;
        case RUN_EV_BUILD_FAIL:
            rs->phase = RUN_BUILD_FAILED;
            return RUN_ACT_NONE;
        case RUN_EV_ABORT:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_KILL;
        case RUN_EV_DROP:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_NONE;
        case RUN_EV_CONSOLE_EOF:
            /* old DevConsole closing after terminate-first; ignore */
            return RUN_ACT_NONE;
        default:
            return RUN_ACT_NONE;
        }

    case RUN_PRIMING:
        switch (ev) {
        case RUN_EV_PRIME_OK:
            rs->phase = RUN_INSTALLING;
            return RUN_ACT_INSTALL;
        case RUN_EV_PRIME_FAIL:
            rs->phase = RUN_DEPLOY_FAILED;
            return RUN_ACT_NONE;
        case RUN_EV_ABORT:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_KILL;
        case RUN_EV_DROP:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_NONE;
        default:
            return RUN_ACT_NONE;
        }

    case RUN_INSTALLING:
        switch (ev) {
        case RUN_EV_INSTALL_OK:
            rs->phase = RUN_LAUNCHING;
            return RUN_ACT_LAUNCH;
        case RUN_EV_INSTALL_FAIL:
            rs->phase = RUN_DEPLOY_FAILED;
            return RUN_ACT_NONE;
        case RUN_EV_ABORT:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_KILL;
        case RUN_EV_DROP:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_NONE;
        default:
            return RUN_ACT_NONE;
        }

    case RUN_LAUNCHING:
        switch (ev) {
        case RUN_EV_LAUNCH_OK:
            rs->deployed_gen = rs->built_gen;
            rs->phase        = RUN_RUNNING;
            return RUN_ACT_NONE;
        case RUN_EV_LAUNCH_FAIL:
            rs->phase = RUN_DEPLOY_FAILED;
            return RUN_ACT_NONE;
        case RUN_EV_ABORT:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_KILL;
        case RUN_EV_DROP:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_NONE;
        default:
            return RUN_ACT_NONE;
        }

    case RUN_RUNNING:
        switch (ev) {
        case RUN_EV_EXECUTE:
            /* terminate-first: kill app, then start fresh chain */
            rs->phase        = RUN_BUILDING;
            rs->compile_only = false;
            return RUN_ACT_TERMINATE_FIRST;
        case RUN_EV_COMPILE:
            /* parallel build while app stays live */
            rs->compile_only = true;
            return RUN_ACT_RESOLVE;
        case RUN_EV_SETTINGS_OK:
            return RUN_ACT_BUILD;
        case RUN_EV_BUILD_OK:
            rs->built_gen++;
            rs->compile_only = false;
            return RUN_ACT_DONE;
        case RUN_EV_BUILD_FAIL:
            rs->compile_only = false;
            return RUN_ACT_NONE;
        case RUN_EV_ABORT:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_KILL;
        case RUN_EV_CONSOLE_EOF:
            rs->phase = RUN_IDLE;
            return RUN_ACT_NONE;
        case RUN_EV_DROP:
            rs->phase = RUN_ABORTED;
            return RUN_ACT_NONE;
        default:
            return RUN_ACT_NONE;
        }

    case RUN_BUILD_FAILED:
    case RUN_DEPLOY_FAILED:
    case RUN_ABORTED:
        if (ev == RUN_EV_EXECUTE) return start_build(rs, false);
        if (ev == RUN_EV_COMPILE) return start_build(rs, true);
        return RUN_ACT_NONE;

    default:
        return RUN_ACT_NONE;
    }
}

bool runstate_stale(const RunState *rs) {
    return rs->phase == RUN_RUNNING && rs->built_gen > rs->deployed_gen;
}

LexKey runstate_phase_lex(RunPhase p) {
    switch (p) {
    case RUN_IDLE:          return LEX_RUN_STANDBY;
    case RUN_BUILDING:      return LEX_RUN_BUILDING;
    case RUN_PRIMING:       return LEX_RUN_PRIMING;
    case RUN_INSTALLING:    return LEX_RUN_INSTALLING;
    case RUN_LAUNCHING:     return LEX_RUN_LAUNCHING;
    case RUN_RUNNING:       return LEX_RUN_RUNNING;
    case RUN_BUILD_FAILED:  return LEX_RUN_BUILD_FAILED;
    case RUN_DEPLOY_FAILED: return LEX_RUN_DEPLOY_FAILED;
    case RUN_ABORTED:       return LEX_RUN_ABORTED;
    default:                return LEX_RUN_STANDBY;
    }
}

LexKey runstate_reason_lex(RunPhase p, BdStatus st) {
    switch (p) {
    case RUN_BUILD_FAILED:
        if (st == BD_ERR_XCODE_MISSING)  return LEX_REC_ERR_XCODE;
        if (st == BD_ERR_SETSID_MISSING) return LEX_REC_ERR_SETSID;
        return LEX_RUN_BUILD_FAILED;
    case RUN_DEPLOY_FAILED:
        return LEX_RUN_DEPLOY_FAILED;
    case RUN_ABORTED:
        return LEX_RUN_ABORTED;
    default:
        return runstate_phase_lex(p);
    }
}

const char *runstate_phase_str(RunPhase p) {
    switch (p) {
    case RUN_IDLE:          return "IDLE";
    case RUN_BUILDING:      return "BUILDING";
    case RUN_PRIMING:       return "PRIMING";
    case RUN_INSTALLING:    return "INSTALLING";
    case RUN_LAUNCHING:     return "LAUNCHING";
    case RUN_RUNNING:       return "RUNNING";
    case RUN_BUILD_FAILED:  return "BUILD_FAILED";
    case RUN_DEPLOY_FAILED: return "DEPLOY_FAILED";
    case RUN_ABORTED:       return "ABORTED";
    default:                return "UNKNOWN";
    }
}
