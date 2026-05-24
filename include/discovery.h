#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stddef.h>
#include <stdbool.h>

#include "arena.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A borrowed byte span (non-owning, not null-terminated). */
typedef struct { const char *data; size_t len; } Str;

typedef enum {
    DISC_OK = 0,
    DISC_ERR_XCODE_MISSING,   /* xcrun/xcodebuild absent (exit 127)  */
    DISC_ERR_COMMAND_FAILED,  /* non-zero exit, not parseable        */
    DISC_ERR_PARSE,           /* output did not parse                */
    DISC_ERR_OOM              /* arena/token space exhausted         */
} DiscStatus;

/* ── command construction (shell-safe; single-quote-escapes paths) ── */
DiscStatus disc_scan_cmd(const char *root, int max_depth,
                         char *buf, size_t cap);
DiscStatus disc_list_cmd(const char *project_path,
                         char *buf, size_t cap);
DiscStatus disc_build_settings_cmd(const char *project_path,
                                   const char *scheme,
                                   const char *config,
                                   char *buf, size_t cap);
DiscStatus disc_simctl_cmd(char *buf, size_t cap);
DiscStatus disc_devicectl_cmd(char *buf, size_t cap);

/* ── results ─────────────────────────────────────────────────────── */
typedef struct { char path[1024]; bool is_workspace; } Blueprint;
typedef struct { Blueprint *items; int count; } BlueprintList;

typedef struct {
    char name[256];
    char udid[128];
    bool is_simulator;  /* simctl vs devicectl; inferred   */
    bool booted;        /* simulator booted state          */
} Target;
typedef struct { Target *items; int count; } TargetList;

typedef struct { char (*items)[256]; int count; } StrList;

/* ── parse + curate (raw bytes → structs in arena `a`) ───────────── */
DiscStatus disc_curate_blueprints(Arena *a, Str find_out,
                                  int max_depth, BlueprintList *out);
DiscStatus disc_parse_list(Arena *a, Str json,
                           StrList *schemes, StrList *configs);
DiscStatus disc_parse_bundle_id(Str json, char *out, size_t cap);
DiscStatus disc_parse_simctl(Arena *a, Str json, TargetList *out);
DiscStatus disc_parse_devicectl(Arena *a, Str json, TargetList *out);

/* ── readiness (pure) ────────────────────────────────────────────── */
typedef struct {
    char project[1024];
    char scheme[256];
    char config[128];
    char bundle_id[256];
    char scan_root[1024]; /* search root for SCAN HOST; not part of readiness */
} RunConfig;

typedef enum {
    READY_OK = 0,
    READY_NO_PROJECT,
    READY_NO_SCHEME,
    READY_NO_CONFIG,
    READY_NO_BUNDLE_ID,
    READY_NO_TARGET
} Readiness;

Readiness   disc_readiness(const RunConfig *rc, bool target_sel);
const char *disc_status_str(DiscStatus st);

#ifdef __cplusplus
}
#endif

#endif /* DISCOVERY_H */
