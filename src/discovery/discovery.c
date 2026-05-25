#define _POSIX_C_SOURCE 200809L

#include "discovery.h"
#include "log.h"

#define JSMN_STATIC
#include "jsmn.h"

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
    /* devicectl deadlocks when told to stream JSON to stdout
       (`--json-output -`) over a non-interactive SSH exec, and it also
       prints a human-readable table to stdout even with --json-output set.
       So: write JSON to a temp file, discard devicectl's own stdout/stderr,
       cat the file (the only thing the worker reads), then propagate the
       exit code so the 127 / non-zero mapping still applies. */
    int n = snprintf(buf, cap,
        "f=$(mktemp /tmp/ostrich-devicectl.XXXXXX); "
        "xcrun devicectl list devices --json-output \"$f\" >/dev/null 2>&1; "
        "rc=$?; cat \"$f\"; rm -f \"$f\"; exit $rc");
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

/* ── jsmn helpers ──────────────────────────────────────────────── */

/* Token limits for each parser's jsmn workspace. */
#define PARSE_LIST_TOKS   512
#define PARSE_BUNDLE_TOKS 2048
#define PARSE_SIMCTL_TOKS 4096
#define PARSE_DEVCTL_TOKS 2048
#define MAX_TARGETS       256

/* True when the jsmn string token matches key exactly. */
static bool jstr_eq(const char *js, const jsmntok_t *t, const char *key) {
    int klen = (int)strlen(key);
    return t->type == JSMN_STRING && (t->end - t->start) == klen &&
           memcmp(js + t->start, key, (size_t)klen) == 0;
}

/* Copy jsmn string token content into dst[cap].  Returns false on type mismatch. */
static bool jstr_copy(const char *js, const jsmntok_t *t, char *dst, size_t cap) {
    if (t->type != JSMN_STRING || cap == 0) return false;
    size_t len = (size_t)(t->end - t->start);
    if (len >= cap) len = cap - 1;
    memcpy(dst, js + t->start, len);
    dst[len] = '\0';
    return true;
}

/* Return the total token count for the subtree rooted at toks[i].
   Key strings (size==1) include their value; objects/arrays include all children. */
static int tok_tree_size(const jsmntok_t *t, int i) {
    int total = 1;
    if (t[i].type == JSMN_OBJECT) {
        /* size = number of keys; each key (STRING,size=1) wraps its value */
        for (int j = 0; j < t[i].size; j++) total += tok_tree_size(t, i + total);
    } else if (t[i].type == JSMN_ARRAY) {
        for (int j = 0; j < t[i].size; j++) total += tok_tree_size(t, i + total);
    } else if (t[i].type == JSMN_STRING && t[i].size > 0) {
        /* key string: add its value subtree */
        total += tok_tree_size(t, i + 1);
    }
    return total;
}

/* Map jsmn error to DiscStatus. */
static DiscStatus jsmn_err(int r) {
    return (r == JSMN_ERROR_NOMEM) ? DISC_ERR_OOM : DISC_ERR_PARSE;
}

/* Populate *out with strings from an JSMN_ARRAY of string tokens at toks[arr]. */
static DiscStatus extract_strlist(Arena *a, const char *js, const jsmntok_t *toks, int arr, int n,
                                  StrList *out) {
    int cnt = toks[arr].size;
    out->items = NULL;
    out->count = 0;
    if (cnt <= 0) return DISC_OK;
    char(*items)[256] = arena_alloc(a, 256 * (size_t)cnt, 1);
    if (!items) return DISC_ERR_OOM;
    int si = arr + 1, n_out = 0;
    for (int k = 0; k < cnt && si < n; k++) {
        if (toks[si].type == JSMN_STRING) {
            jstr_copy(js, &toks[si], items[n_out], 256);
            n_out++;
        }
        si += tok_tree_size(toks, si);
    }
    out->items = items;
    out->count = n_out;
    return DISC_OK;
}

/* ── parsers ───────────────────────────────────────────────────── */

/*
 * Parse `xcodebuild -list -json` output into schemes[] and configs[].
 * Tolerates unknown keys at every level (token-walk, jsmn pattern).
 * Workspace JSON has no "configurations" array; configs is left empty.
 */
DiscStatus disc_parse_list(Arena *a, Str json, StrList *schemes, StrList *configs) {
    schemes->items = NULL;
    schemes->count = 0;
    configs->items = NULL;
    configs->count = 0;

    if (!json.data || json.len == 0) return DISC_ERR_PARSE;

    jsmntok_t *toks = arena_alloc(a, sizeof(jsmntok_t) * PARSE_LIST_TOKS, _Alignof(jsmntok_t));
    if (!toks) return DISC_ERR_OOM;

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json.data, json.len, toks, PARSE_LIST_TOKS);
    if (r < 0) return jsmn_err(r);
    int n = r;

    if (n == 0 || toks[0].type != JSMN_OBJECT) return DISC_ERR_PARSE;

    /* Locate the "project" or "workspace" inner object. */
    int inner = -1;
    int i = 1;
    for (int k = 0; k < toks[0].size && i < n; k++) {
        if ((jstr_eq(json.data, &toks[i], "project") || jstr_eq(json.data, &toks[i], "workspace")) &&
            i + 1 < n && toks[i + 1].type == JSMN_OBJECT) {
            inner = i + 1;
        }
        i += tok_tree_size(toks, i);
    }
    if (inner < 0) return DISC_ERR_PARSE;

    /* Scan the inner object for "schemes" and "configurations" arrays. */
    int schemes_arr = -1, configs_arr = -1;
    i = inner + 1;
    for (int k = 0; k < toks[inner].size && i < n; k++) {
        if (jstr_eq(json.data, &toks[i], "schemes") && i + 1 < n && toks[i + 1].type == JSMN_ARRAY)
            schemes_arr = i + 1;
        else if (jstr_eq(json.data, &toks[i], "configurations") && i + 1 < n &&
                 toks[i + 1].type == JSMN_ARRAY)
            configs_arr = i + 1;
        i += tok_tree_size(toks, i);
    }

    if (schemes_arr >= 0) {
        DiscStatus s = extract_strlist(a, json.data, toks, schemes_arr, n, schemes);
        if (s != DISC_OK) return s;
    }
    if (configs_arr >= 0) {
        DiscStatus s = extract_strlist(a, json.data, toks, configs_arr, n, configs);
        if (s != DISC_OK) return s;
    }
    LOG_INFO(LG_DISC, "parse-list schemes=%d configs=%d",
             schemes->count, configs->count);
    return DISC_OK;
}

/*
 * Parse `xcodebuild -showBuildSettings -json` and extract
 * PRODUCT_BUNDLE_IDENTIFIER from the first target's buildSettings.
 * No Arena: jsmn tokens are stack-allocated.
 */
DiscStatus disc_parse_bundle_id(Str json, char *out, size_t cap) {
    if (!json.data || json.len == 0 || !out || cap == 0) return DISC_ERR_PARSE;

    jsmntok_t toks[PARSE_BUNDLE_TOKS];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json.data, json.len, toks, PARSE_BUNDLE_TOKS);
    if (r < 0) return jsmn_err(r);
    int n = r;

    /* Top-level is an array of per-target objects. */
    if (n == 0 || toks[0].type != JSMN_ARRAY) return DISC_ERR_PARSE;

    int i = 1;
    for (int el = 0; el < toks[0].size && i < n; el++) {
        if (toks[i].type != JSMN_OBJECT) {
            i += tok_tree_size(toks, i);
            continue;
        }
        int obj_size = toks[i].size;
        int j = i + 1;
        int bs_idx = -1;

        /* Look for "buildSettings" key. */
        for (int k = 0; k < obj_size && j < n; k++) {
            if (jstr_eq(json.data, &toks[j], "buildSettings") && j + 1 < n &&
                toks[j + 1].type == JSMN_OBJECT) {
                bs_idx = j + 1;
                break;
            }
            j += tok_tree_size(toks, j);
        }

        if (bs_idx >= 0) {
            int bs_size = toks[bs_idx].size;
            int ki = bs_idx + 1;
            for (int k = 0; k < bs_size && ki < n; k++) {
                if (jstr_eq(json.data, &toks[ki], "PRODUCT_BUNDLE_IDENTIFIER") && ki + 1 < n &&
                    toks[ki + 1].type == JSMN_STRING) {
                    jstr_copy(json.data, &toks[ki + 1], out, cap);
                    LOG_DEBUG(LG_DISC, "parse-bundle-id bundle_id=%s", out);
                    return DISC_OK;
                }
                ki += tok_tree_size(toks, ki);
            }
        }
        i += tok_tree_size(toks, i);
    }
    return DISC_ERR_PARSE;
}

/*
 * Parse `xcrun simctl list devices --json` into a TargetList.
 * All entries are marked is_simulator=true; booted is inferred from "state"=="Booted".
 */
DiscStatus disc_parse_simctl(Arena *a, Str json, TargetList *out) {
    out->items = NULL;
    out->count = 0;
    if (!json.data || json.len == 0) return DISC_ERR_PARSE;

    jsmntok_t *toks = arena_alloc(a, sizeof(jsmntok_t) * PARSE_SIMCTL_TOKS, _Alignof(jsmntok_t));
    if (!toks) return DISC_ERR_OOM;

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json.data, json.len, toks, PARSE_SIMCTL_TOKS);
    if (r < 0) return jsmn_err(r);
    int n = r;

    if (n == 0 || toks[0].type != JSMN_OBJECT) return DISC_ERR_PARSE;

    /* Locate the "devices" object. */
    int devices_obj = -1;
    int i = 1;
    for (int k = 0; k < toks[0].size && i < n; k++) {
        if (jstr_eq(json.data, &toks[i], "devices") && i + 1 < n &&
            toks[i + 1].type == JSMN_OBJECT) {
            devices_obj = i + 1;
        }
        i += tok_tree_size(toks, i);
    }
    if (devices_obj < 0) return DISC_ERR_PARSE;

    Target *items = arena_alloc(a, sizeof(Target) * MAX_TARGETS, _Alignof(Target));
    if (!items) return DISC_ERR_OOM;
    int count = 0;

    /* Iterate runtime-keyed arrays inside "devices". */
    int di = devices_obj + 1;
    for (int k = 0; k < toks[devices_obj].size && di < n; k++) {
        /* di = runtime-id key (STRING,size=1); di+1 = device array */
        if (di + 1 < n && toks[di + 1].type == JSMN_ARRAY) {
            int arr = di + 1;
            int si = arr + 1;
            for (int s = 0; s < toks[arr].size && si < n; s++) {
                if (toks[si].type != JSMN_OBJECT) {
                    si += tok_tree_size(toks, si);
                    continue;
                }
                Target t;
                memset(&t, 0, sizeof(t));
                t.is_simulator = true;

                int fi = si + 1;
                for (int f = 0; f < toks[si].size && fi < n; f++) {
                    if (jstr_eq(json.data, &toks[fi], "name") && fi + 1 < n)
                        jstr_copy(json.data, &toks[fi + 1], t.name, sizeof(t.name));
                    else if (jstr_eq(json.data, &toks[fi], "udid") && fi + 1 < n)
                        jstr_copy(json.data, &toks[fi + 1], t.udid, sizeof(t.udid));
                    else if (jstr_eq(json.data, &toks[fi], "state") && fi + 1 < n)
                        t.booted = jstr_eq(json.data, &toks[fi + 1], "Booted");
                    fi += tok_tree_size(toks, fi);
                }
                if (count < MAX_TARGETS) items[count++] = t;
                si += tok_tree_size(toks, si);
            }
        }
        di += tok_tree_size(toks, di);
    }

    out->items = items;
    out->count = count;
    LOG_INFO(LG_DISC, "parse-simctl targets=%d", count);
    return DISC_OK;
}

/*
 * Parse `xcrun devicectl list devices --json-output -` into a TargetList.
 * All entries are marked is_simulator=false, booted=false.
 * Name comes from deviceProperties.name; udid from hardwareProperties.udid.
 */
DiscStatus disc_parse_devicectl(Arena *a, Str json, TargetList *out) {
    out->items = NULL;
    out->count = 0;
    if (!json.data || json.len == 0) return DISC_ERR_PARSE;

    jsmntok_t *toks = arena_alloc(a, sizeof(jsmntok_t) * PARSE_DEVCTL_TOKS, _Alignof(jsmntok_t));
    if (!toks) return DISC_ERR_OOM;

    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, json.data, json.len, toks, PARSE_DEVCTL_TOKS);
    if (r < 0) return jsmn_err(r);
    int n = r;

    if (n == 0 || toks[0].type != JSMN_OBJECT) return DISC_ERR_PARSE;

    /* Locate "result" object. */
    int result_obj = -1;
    int i = 1;
    for (int k = 0; k < toks[0].size && i < n; k++) {
        if (jstr_eq(json.data, &toks[i], "result") && i + 1 < n &&
            toks[i + 1].type == JSMN_OBJECT) {
            result_obj = i + 1;
        }
        i += tok_tree_size(toks, i);
    }
    if (result_obj < 0) return DISC_ERR_PARSE;

    /* Locate "devices" array inside "result". */
    int devices_arr = -1;
    i = result_obj + 1;
    for (int k = 0; k < toks[result_obj].size && i < n; k++) {
        if (jstr_eq(json.data, &toks[i], "devices") && i + 1 < n &&
            toks[i + 1].type == JSMN_ARRAY) {
            devices_arr = i + 1;
        }
        i += tok_tree_size(toks, i);
    }
    if (devices_arr < 0) return DISC_ERR_PARSE;

    Target *items = arena_alloc(a, sizeof(Target) * MAX_TARGETS, _Alignof(Target));
    if (!items) return DISC_ERR_OOM;
    int count = 0;

    int di = devices_arr + 1;
    for (int d = 0; d < toks[devices_arr].size && di < n; d++) {
        if (toks[di].type != JSMN_OBJECT) {
            di += tok_tree_size(toks, di);
            continue;
        }
        Target t;
        memset(&t, 0, sizeof(t));
        t.is_simulator = false;

        int fi = di + 1;
        for (int f = 0; f < toks[di].size && fi < n; f++) {
            if (jstr_eq(json.data, &toks[fi], "deviceProperties") && fi + 1 < n &&
                toks[fi + 1].type == JSMN_OBJECT) {
                /* Extract "name" from deviceProperties. */
                int dpi = fi + 2;
                for (int dp = 0; dp < toks[fi + 1].size && dpi < n; dp++) {
                    if (jstr_eq(json.data, &toks[dpi], "name") && dpi + 1 < n)
                        jstr_copy(json.data, &toks[dpi + 1], t.name, sizeof(t.name));
                    dpi += tok_tree_size(toks, dpi);
                }
            } else if (jstr_eq(json.data, &toks[fi], "hardwareProperties") && fi + 1 < n &&
                       toks[fi + 1].type == JSMN_OBJECT) {
                /* Extract "udid" from hardwareProperties. */
                int hpi = fi + 2;
                for (int hp = 0; hp < toks[fi + 1].size && hpi < n; hp++) {
                    if (jstr_eq(json.data, &toks[hpi], "udid") && hpi + 1 < n)
                        jstr_copy(json.data, &toks[hpi + 1], t.udid, sizeof(t.udid));
                    hpi += tok_tree_size(toks, hpi);
                }
            }
            fi += tok_tree_size(toks, fi);
        }
        if (count < MAX_TARGETS) items[count++] = t;
        di += tok_tree_size(toks, di);
    }

    out->items = items;
    out->count = count;
    LOG_INFO(LG_DISC, "parse-devicectl targets=%d", count);
    return DISC_OK;
}
