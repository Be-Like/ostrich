#define _POSIX_C_SOURCE 200809L

#include "discovery.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ── internal helpers ──────────────────────────────────────────── */

/* Write single-quote-escaped path into buf[0..cap).
   Shells interpret the result as the literal path value.
   Returns DISC_ERR_OOM if buf is too small. */
static DiscStatus quote_path(const char *path, char *buf, size_t cap) {
    size_t pos = 0;

#define PUTC(c) \
    do { if (pos + 1 >= cap) return DISC_ERR_OOM; \
         buf[pos++] = (char)(c); } while (0)

    PUTC('\'');
    for (const char *ch = path; *ch; ch++) {
        if (*ch == '\'') {
            /* end quote, backslash-escaped quote, re-open quote */
            PUTC('\''); PUTC('\\'); PUTC('\''); PUTC('\'');
        } else {
            PUTC(*ch);
        }
    }
    PUTC('\'');
    buf[pos] = '\0';

#undef PUTC
    return DISC_OK;
}

static bool str_ends_with(const char *data, size_t len,
                           const char *suffix, size_t suf_len) {
    return len >= suf_len &&
           memcmp(data + len - suf_len, suffix, suf_len) == 0;
}

static bool str_contains(const char *data, size_t len,
                          const char *needle, size_t needle_len) {
    if (needle_len == 0) return true;
    if (len < needle_len) return false;
    for (size_t i = 0; i <= len - needle_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

/* ── command construction ──────────────────────────────────────── */

DiscStatus disc_scan_cmd(const char *root, int max_depth,
                         char *buf, size_t cap) {
    char qroot[4096];
    DiscStatus s = quote_path(root, qroot, sizeof(qroot));
    if (s != DISC_OK) return s;

    int n = snprintf(buf, cap,
        "find %s -maxdepth %d"
        " \\( -name Pods -o -name Carthage -o -name .build"
        " -o -name DerivedData -o -name node_modules \\) -prune"
        " -o \\( -name '*.xcodeproj' -o -name '*.xcworkspace' \\) -print",
        qroot, max_depth);
    if (n < 0 || (size_t)n >= cap) return DISC_ERR_OOM;
    return DISC_OK;
}

DiscStatus disc_list_cmd(const char *project_path,
                         char *buf, size_t cap) {
    char qpath[4096];
    DiscStatus s = quote_path(project_path, qpath, sizeof(qpath));
    if (s != DISC_OK) return s;

    size_t path_len = strlen(project_path);
    bool is_ws = str_ends_with(project_path, path_len,
                               ".xcworkspace", sizeof(".xcworkspace") - 1);
    const char *flag = is_ws ? "-workspace" : "-project";

    int n = snprintf(buf, cap, "xcodebuild -list -json %s %s", flag, qpath);
    if (n < 0 || (size_t)n >= cap) return DISC_ERR_OOM;
    return DISC_OK;
}

DiscStatus disc_build_settings_cmd(const char *project_path,
                                   const char *scheme,
                                   const char *config,
                                   char *buf, size_t cap) {
    char qpath[4096], qscheme[1024], qconfig[512];
    DiscStatus s;

    if ((s = quote_path(project_path, qpath,   sizeof(qpath)))   != DISC_OK) return s;
    if ((s = quote_path(scheme,       qscheme,  sizeof(qscheme))) != DISC_OK) return s;
    if ((s = quote_path(config,       qconfig,  sizeof(qconfig))) != DISC_OK) return s;

    size_t path_len = strlen(project_path);
    bool is_ws = str_ends_with(project_path, path_len,
                               ".xcworkspace", sizeof(".xcworkspace") - 1);
    const char *flag = is_ws ? "-workspace" : "-project";

    int n = snprintf(buf, cap,
        "xcodebuild -showBuildSettings -json %s %s -scheme %s -configuration %s",
        flag, qpath, qscheme, qconfig);
    if (n < 0 || (size_t)n >= cap) return DISC_ERR_OOM;
    return DISC_OK;
}

DiscStatus disc_simctl_cmd(char *buf, size_t cap) {
    int n = snprintf(buf, cap, "xcrun simctl list devices --json");
    if (n < 0 || (size_t)n >= cap) return DISC_ERR_OOM;
    return DISC_OK;
}

DiscStatus disc_devicectl_cmd(char *buf, size_t cap) {
    int n = snprintf(buf, cap, "xcrun devicectl list devices --json-output -");
    if (n < 0 || (size_t)n >= cap) return DISC_ERR_OOM;
    return DISC_OK;
}

/* ── curation ──────────────────────────────────────────────────── */

/*
 * Artifact: every .xcodeproj contains a "project.xcworkspace" that is
 * an Xcode-generated stub.  It always matches *.xcodeproj/project.xcworkspace
 * and must be dropped before presenting the list.
 */
static const char k_artifact[] = ".xcodeproj/project.xcworkspace";
static const char k_ws_ext[]   = ".xcworkspace";

DiscStatus disc_curate_blueprints(Arena *a, Str find_out,
                                  int max_depth, BlueprintList *out) {
    (void)max_depth; /* depth already applied by the remote find */

    out->items = NULL;
    out->count = 0;

    if (!find_out.data || find_out.len == 0) return DISC_OK;

    /* Count newlines as an upper bound on the number of paths. */
    int line_count = 0;
    for (size_t i = 0; i < find_out.len; i++) {
        if (find_out.data[i] == '\n') line_count++;
    }
    if (find_out.data[find_out.len - 1] != '\n') line_count++;
    if (line_count == 0) return DISC_OK;

    Blueprint *items = arena_alloc(a,
                                   sizeof(Blueprint) * (size_t)line_count,
                                   _Alignof(Blueprint));
    if (!items) return DISC_ERR_OOM;
    memset(items, 0, sizeof(Blueprint) * (size_t)line_count);

    /* First pass: collect non-artifact paths. */
    int n = 0;
    const char *p   = find_out.data;
    const char *end = find_out.data + find_out.len;
    while (p < end && n < line_count) {
        const char *nl       = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t      line_len = (size_t)(line_end - p);

        if (line_len == 0) { p = nl ? nl + 1 : end; continue; }

        /* Drop *.xcodeproj/project.xcworkspace artifacts. */
        if (str_contains(p, line_len, k_artifact, sizeof(k_artifact) - 1)) {
            p = nl ? nl + 1 : end;
            continue;
        }

        bool is_ws = str_ends_with(p, line_len, k_ws_ext, sizeof(k_ws_ext) - 1);

        size_t copy_len = line_len < sizeof(items[n].path) - 1
                        ? line_len : sizeof(items[n].path) - 1;
        memcpy(items[n].path, p, copy_len);
        items[n].path[copy_len] = '\0';
        items[n].is_workspace   = is_ws;
        n++;

        p = nl ? nl + 1 : end;
    }

    /* Second pass: when a .xcworkspace and .xcodeproj share a directory
       and stem, prefer the workspace and mark the project for removal. */
    for (int i = 0; i < n; i++) {
        if (!items[i].is_workspace) continue;

        const char *ws_path    = items[i].path;
        const char *last_slash = strrchr(ws_path, '/');
        if (!last_slash) continue;

        size_t      dir_len     = (size_t)(last_slash - ws_path);
        const char *ws_name     = last_slash + 1;
        size_t      ws_name_len = strlen(ws_name);
        size_t      ws_ext_len  = sizeof(k_ws_ext) - 1;

        if (ws_name_len <= ws_ext_len) continue;
        size_t stem_len = ws_name_len - ws_ext_len;

        /* Build the expected sibling project path. */
        char proj_path[sizeof(items[0].path)];
        int r = snprintf(proj_path, sizeof(proj_path),
                         "%.*s/%.*s.xcodeproj",
                         (int)dir_len, ws_path,
                         (int)stem_len, ws_name);
        if (r < 0 || (size_t)r >= sizeof(proj_path)) continue;

        for (int j = 0; j < n; j++) {
            if (!items[j].is_workspace &&
                strcmp(items[j].path, proj_path) == 0) {
                items[j].path[0] = '\0'; /* mark for removal */
            }
        }
    }

    /* Compact: move surviving entries to the front. */
    int final_count = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].path[0] != '\0') {
            if (final_count != i) items[final_count] = items[i];
            final_count++;
        }
    }

    out->items = items;
    out->count = final_count;
    return DISC_OK;
}

/* ── readiness ─────────────────────────────────────────────────── */

Readiness disc_readiness(const RunConfig *rc, bool target_sel) {
    if (rc->project[0]   == '\0') return READY_NO_PROJECT;
    if (rc->scheme[0]    == '\0') return READY_NO_SCHEME;
    if (rc->config[0]    == '\0') return READY_NO_CONFIG;
    if (rc->bundle_id[0] == '\0') return READY_NO_BUNDLE_ID;
    if (!target_sel)               return READY_NO_TARGET;
    return READY_OK;
}

/* ── status string ─────────────────────────────────────────────── */

const char *disc_status_str(DiscStatus st) {
    switch (st) {
    case DISC_OK:                 return "ok";
    case DISC_ERR_XCODE_MISSING:  return "xcode not found";
    case DISC_ERR_COMMAND_FAILED: return "command failed";
    case DISC_ERR_PARSE:          return "parse error";
    case DISC_ERR_OOM:            return "out of memory";
    default:                      return "(unknown)";
    }
}
