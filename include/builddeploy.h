#ifndef BUILDDEPLOY_H
#define BUILDDEPLOY_H

#include "discovery.h"
#include "lexicon.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BD_OK = 0,
    BD_ERR_XCODE_MISSING,   /* exit 127                              */
    BD_ERR_SETSID_MISSING,  /* wrapper failed before marker (pgid=0) */
    BD_ERR_BUILD,           /* xcodebuild non-zero  → build fail     */
    BD_ERR_BOOT,            /* boot/bootstatus      → deploy fail    */
    BD_ERR_INSTALL,         /* install non-zero     → deploy fail    */
    BD_ERR_LAUNCH,          /* launch non-zero      → deploy fail    */
    BD_ERR_PARSE,           /* settings did not parse                */
    BD_ERR_UNLOCK_FAILED,   /* security unlock-keychain non-zero     */
    BD_ERR_OOM
} BdStatus;

/* command construction (shell-safe; single-quote-escaped) */
BdStatus bd_settings_cmd  (const RunConfig *, const Target *,
                           bool has_target, char *, size_t);
BdStatus bd_build_cmd     (const RunConfig *, const Target *,
                           bool has_target, char *, size_t);
BdStatus bd_bootstatus_cmd(const Target *, char *, size_t); /* boot-if-needed + wait (-b) */
BdStatus bd_install_cmd   (const Target *, const char *app_path,
                           char *, size_t);
BdStatus bd_launch_cmd    (const Target *, const char *bundle_id,
                           char *, size_t); /* --console, setsid, PID marker */
BdStatus bd_terminate_cmd (const Target *, const char *bundle_id,
                           char *, size_t);
BdStatus bd_kill_cmd      (long pgid, char *, size_t);
BdStatus bd_destination   (const Target *, bool has_target,
                           char *, size_t);

/* parse (raw bytes → values) */
BdStatus bd_parse_product_path(Str settings_json, char *out, size_t cap);
bool     bd_parse_pid_marker  (Str chunk, long *out_pgid);
bool     bd_parse_exit_marker (Str chunk, int  *out_code); /* __OSTRICH_EXIT__ marker */

/* remediation text */
BdStatus bd_setsid_help_block(const char *user, const char *host, int port,
                               char *buf, size_t cap);
BdStatus bd_unlock_cmd(const char *kc_pass, char *buf, size_t cap);
BdStatus bd_unlock_help_block(const char *user, const char *host, int port,
                               char *buf, size_t cap);
BdStatus bd_codesign_hint_block(const char *user, const char *host, int port,
                                 char *buf, size_t cap);

/* classification */
LexKey      bd_reason_lex (BdStatus st);
const char *bd_status_str (BdStatus st);

#ifdef __cplusplus
}
#endif

#endif /* BUILDDEPLOY_H */
