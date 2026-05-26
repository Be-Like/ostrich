#ifndef LEXICON_H
#define LEXICON_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LEX_IDENTITY,       /* "OSTRICH // infiltration console" */
    LEX_WORDMARK,       /* the static ASCII ostrich banner   */
    LEX_FOOTER_NAME,    /* "ostrich"                          */
    LEX_FOOTER_ONLINE,  /* "ONLINE"                           */
    LEX_VOICE_PREFIX,   /* ">" (magenta voice signature)      */

    /* ── connection overlay ─────────────────────────────────────────── */
    LEX_CONN_UPLINK,           /* "OSTRICH // UPLINK"              */
    LEX_CONN_BREACH,           /* "BREACH"                         */
    LEX_CONN_ABORT,            /* "■ ABORT"                        */
    LEX_CONN_KNOWN_HOSTS,      /* "KNOWN HOSTS"                    */
    LEX_CONN_NO_KNOWN_HOSTS,   /* "// NO KNOWN HOSTS"              */

    /* ── connection phases ──────────────────────────────────────────── */
    LEX_CONN_BREACHING,        /* "BREACHING PERIMETER…"           */
    LEX_CONN_ACCESS_GRANTED,   /* "ACCESS GRANTED"                 */
    LEX_CONN_WELCOME,          /* "…WELCOME, OPERATOR."            */
    LEX_CONN_ACCESS_DENIED,    /* "ACCESS DENIED"                  */
    LEX_CONN_ONLINE,           /* "* ONLINE"                       */
    LEX_CONN_REACQUIRING,      /* "REACQUIRING SIGNAL…"            */
    LEX_CONN_SEVERED,          /* "LINK SEVERED"                   */

    /* ── connection failures ────────────────────────────────────────── */
    LEX_CONN_ERR_NO_ROUTE,         /* "HOST UNREACHABLE // NO ROUTE"              */
    LEX_CONN_ERR_PORT_CLOSED,      /* "PERIMETER SEALED // PORT CLOSED"           */
    LEX_CONN_ERR_TIMEOUT,          /* "NO RESPONSE // TIMEOUT"                    */
    LEX_CONN_ERR_HOSTKEY_MISMATCH, /* "HOST KEY MISMATCH // POSSIBLE INTERCEPTION" */
    LEX_CONN_ERR_NO_SHELL,         /* "NO FOOTHOLD // SHELL DENIED"               */
    LEX_CONN_UNKNOWN_HOST,         /* "UNKNOWN HOST //" (prefix; UI appends fp)   */

    /* ── form field labels ──────────────────────────────────────────── */
    LEX_CONN_FIELD_HOST,       /* "HOST"             */
    LEX_CONN_FIELD_PORT,       /* "PORT"             */
    LEX_CONN_FIELD_USER,       /* "USER"             */
    LEX_CONN_FIELD_AUTH,       /* "AUTH"             */
    LEX_CONN_AUTH_AGENT,       /* "SSH-AGENT"        */
    LEX_CONN_AUTH_PASSKEY,     /* "PASSKEY"          */
    LEX_CONN_REMEMBER_PASSKEY, /* "REMEMBER PASSKEY" */

    /* ── host-key trust actions ────────────────────────────────────── */
    LEX_CONN_TRUST,    /* "TRUST"   */
    LEX_CONN_DECLINE,  /* "DECLINE" */

    /* ── known-hosts overlay ───────────────────────────────────────── */
    LEX_CONN_SAVE,     /* "SAVE"    */

    /* ── connection-bar controls ───────────────────────────────────── */
    LEX_CONN_UPDATE,   /* "UPDATE"  */
    LEX_CONN_CLOSE,    /* "CLOSE"   */

    /* ── discovery / recon ──────────────────────────────────────────── */
    LEX_REC_SCAN_HOST,      /* "\xe2\x8c\x96 SCAN HOST"           */
    LEX_REC_ABORT_SCAN,     /* "\xe2\x96\xa0 ABORT SCAN"          */
    LEX_REC_BLUEPRINTS,     /* "BLUEPRINTS RECOVERED"             */
    LEX_REC_NO_BLUEPRINTS,  /* "// NO BLUEPRINTS"                 */
    LEX_REC_SWEEP,          /* "\xe2\x86\xbb SWEEP FOR TARGETS"   */
    LEX_REC_TARGETS,        /* "TARGETS IN RANGE"                 */
    LEX_REC_NO_TARGETS,     /* "// NO TARGETS IN RANGE"           */
    LEX_REC_NO_OP,          /* "// NO OPERATION CONFIGURED"       */
    LEX_REC_READY,          /* "READY"                            */
    LEX_REC_ERR_XCODE,      /* "XCODE NOT FOUND"                  */
    LEX_REC_ERR_SETSID,     /* "SETSID NOT FOUND"                 */
    LEX_REC_ERR_INVENTORY,  /* "COULD NOT READ INVENTORY"         */

    /* ── recon field labels ─────────────────────────────────────────── */
    LEX_REC_FIELD_SCAN_ROOT, /* "SCAN ROOT" */
    LEX_REC_FIELD_SCHEME,    /* "SCHEME"    */
    LEX_REC_FIELD_CONFIG,    /* "CONFIG"    */
    LEX_REC_FIELD_BUNDLE_ID, /* "BUNDLE ID" */
    LEX_REC_FIELD_PRESET,    /* "PRESET"    */

    /* ── preset actions ────────────────────────────────────────────────── */
    LEX_REC_PRESET_NEW,    /* "NEW PRESET" */
    LEX_REC_PRESET_RENAME, /* "RENAME"     */
    LEX_REC_PRESET_DELETE, /* "DELETE"     */

    /* ── build / deploy controls ───────────────────────────────────────── */
    LEX_RUN_EXECUTE,         /* "\xe2\x96\xb6 EXECUTE"                    */
    LEX_RUN_COMPILE,         /* "COMPILE"                                  */
    LEX_RUN_ABORT,           /* "\xe2\x96\xa0 ABORT"                       */

    /* ── run-state labels ──────────────────────────────────────────────── */
    LEX_RUN_STANDBY,         /* "STANDBY"                                  */
    LEX_RUN_BUILDING,        /* "COMPILING EXPLOIT\xe2\x80\xa6"            */
    LEX_RUN_PRIMING,         /* "PRIMING TARGET\xe2\x80\xa6"               */
    LEX_RUN_INSTALLING,      /* "DEPLOYING PAYLOAD\xe2\x80\xa6"            */
    LEX_RUN_LAUNCHING,       /* "EXECUTING PAYLOAD\xe2\x80\xa6"            */
    LEX_RUN_RUNNING,         /* "TARGET ACQUIRED // LIVE"                  */
    LEX_RUN_BUILD_FAILED,    /* "EXPLOIT FAILED"                           */
    LEX_RUN_DEPLOY_FAILED,   /* "DEPLOYMENT FAILED // PAYLOAD REJECTED"    */
    LEX_RUN_ABORTED,         /* "OPERATION ABORTED"                        */

    /* ── device log / stale ────────────────────────────────────────────── */
    LEX_RUN_LIVE_FEED,       /* "LIVE FEED // INTERCEPTING"                */
    LEX_RUN_NEW_PAYLOAD,     /* "> \xe2\x94\x80\xe2\x94\x80 NEW PAYLOAD \xe2\x94\x80\xe2\x94\x80" */
    LEX_RUN_STALE,           /* "PAYLOAD STALE // NEW EXPLOIT READY"       */

    /* ── log empty states ──────────────────────────────────────────────── */
    LEX_RUN_BUILD_EMPTY,     /* "// NO PAYLOAD COMPILED"                   */
    LEX_RUN_DEVICE_EMPTY,    /* "// NO SIGNAL \xe2\x80\x94 TARGET DARK"    */

    /* ── build log step demarcation ────────────────────────────────────── */
    LEX_RUN_STEP_HEADER_FMT, /* "> \xe2\x94\x80\xe2\x94\x80 %s // %s \xe2\x94\x80\xe2\x94\x80" */

    LEX__COUNT
} LexKey;

const char *lex(LexKey key);

#ifdef __cplusplus
}
#endif

#endif /* LEXICON_H */
