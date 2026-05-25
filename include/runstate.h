#ifndef RUNSTATE_H
#define RUNSTATE_H

#include <stdbool.h>
#include "builddeploy.h"
#include "lexicon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RUN_IDLE,          /* STANDBY                                */
    RUN_BUILDING,      /* COMPILING EXPLOIT… (incl. settings)    */
    RUN_PRIMING,       /* PRIMING TARGET… (sim boot)             */
    RUN_INSTALLING,    /* DEPLOYING PAYLOAD…                     */
    RUN_LAUNCHING,     /* EXECUTING PAYLOAD…                     */
    RUN_RUNNING,       /* TARGET ACQUIRED // LIVE                */
    RUN_BUILD_FAILED,  /* EXPLOIT FAILED                         */
    RUN_DEPLOY_FAILED, /* DEPLOYMENT FAILED // PAYLOAD REJECTED  */
    RUN_ABORTED        /* OPERATION ABORTED                      */
} RunPhase;

typedef enum {
    RUN_EV_EXECUTE, RUN_EV_COMPILE, RUN_EV_ABORT,
    RUN_EV_SETTINGS_OK, RUN_EV_BUILD_OK, RUN_EV_BUILD_FAIL,
    RUN_EV_PRIME_OK, RUN_EV_PRIME_FAIL,
    RUN_EV_INSTALL_OK, RUN_EV_INSTALL_FAIL,
    RUN_EV_LAUNCH_OK, RUN_EV_LAUNCH_FAIL,
    RUN_EV_CONSOLE_EOF, /* app exited / target gone → clean idle */
    RUN_EV_DROP         /* SSH drop mid-run → aborted            */
} RunEvent;

typedef enum {
    RUN_ACT_NONE,
    RUN_ACT_RESOLVE,         /* start settings query               */
    RUN_ACT_BUILD,           /* start xcodebuild                   */
    RUN_ACT_PRIME,           /* boot simulator                     */
    RUN_ACT_INSTALL,         /* install .app                       */
    RUN_ACT_LAUNCH,          /* launch --console                   */
    RUN_ACT_TERMINATE_FIRST, /* terminate running app, then rebuild */
    RUN_ACT_KILL,            /* kill in-flight build process group */
    RUN_ACT_DONE             /* compile chain finished             */
} RunAction;

typedef struct {
    RunPhase phase;
    int      built_gen;     /* ++ on each successful build        */
    int      deployed_gen;  /* set when a Play launch succeeds    */
    bool     target_is_sim; /* gates the PRIME step               */
    bool     compile_only;  /* set internally when COMPILE starts */
} RunState;

void        runstate_init(RunState *rs);
RunAction   runstate_step(RunState *rs, RunEvent ev);
bool        runstate_stale(const RunState *rs); /* running && built_gen > deployed_gen */
LexKey      runstate_phase_lex(RunPhase p);
LexKey      runstate_reason_lex(RunPhase p, BdStatus st);
const char *runstate_phase_str(RunPhase p);     /* debug/log */

#ifdef __cplusplus
}
#endif

#endif /* RUNSTATE_H */
