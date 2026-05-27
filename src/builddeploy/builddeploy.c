#include "builddeploy.h"

#define JSMN_STATIC
#include "jsmn.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ── internal helpers ────────────────────────────────────────────── */

/* Single-quote-escape a path/string for shell safety.
   Mirrors the identical helper in discovery.c. */
static BdStatus bd_quote(const char *s, char *buf, size_t cap) {
    size_t pos = 0;

#define PUTC(c) \
    do { if (pos + 1 >= cap) return BD_ERR_OOM; \
         buf[pos++] = (char)(c); } while (0)

    PUTC('\'');
    for (const char *ch = s; *ch; ch++) {
        if (*ch == '\'') {
            PUTC('\''); PUTC('\\'); PUTC('\''); PUTC('\'');
        } else {
            PUTC(*ch);
        }
    }
    PUTC('\'');
    buf[pos] = '\0';

#undef PUTC
    return BD_OK;
}

static bool str_ends_with(const char *data, size_t len,
                           const char *suffix, size_t suf_len) {
    return len >= suf_len &&
           memcmp(data + len - suf_len, suffix, suf_len) == 0;
}

/* ── jsmn helpers (same patterns as discovery.c) ─────────────────── */

#define PARSE_SETTINGS_TOKS 2048

static bool jstr_eq(const char *js, const jsmntok_t *t, const char *key) {
    int klen = (int)strlen(key);
    return t->type == JSMN_STRING && (t->end - t->start) == klen &&
           memcmp(js + t->start, key, (size_t)klen) == 0;
}

static bool jstr_copy(const char *js, const jsmntok_t *t,
                      char *dst, size_t cap) {
    if (t->type != JSMN_STRING || cap == 0) return false;
    size_t len = (size_t)(t->end - t->start);
    if (len >= cap) len = cap - 1;
    memcpy(dst, js + t->start, len);
    dst[len] = '\0';
    return true;
}

static int tok_tree_size(const jsmntok_t *t, int i) {
    int total = 1;
    if (t[i].type == JSMN_OBJECT) {
        for (int j = 0; j < t[i].size; j++) total += tok_tree_size(t, i + total);
    } else if (t[i].type == JSMN_ARRAY) {
        for (int j = 0; j < t[i].size; j++) total += tok_tree_size(t, i + total);
    } else if (t[i].type == JSMN_STRING && t[i].size > 0) {
        total += tok_tree_size(t, i + 1);
    }
    return total;
}

/* ── destination ─────────────────────────────────────────────────── */

/*
 * Build the xcodebuild -destination value.
 * No target (COMPILE without a lock): generic iOS.
 * Specific target (device or simulator): id=<udid>.
 */
BdStatus bd_destination(const Target *tgt, bool has_target,
                        char *buf, size_t cap) {
    int n;
    if (!has_target || !tgt) {
        n = snprintf(buf, cap, "generic/platform=iOS");
    } else {
        n = snprintf(buf, cap, "id=%s", tgt->udid);
    }
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

/* ── command builders ────────────────────────────────────────────── */

BdStatus bd_settings_cmd(const RunConfig *rc, const Target *tgt,
                         bool has_target, char *buf, size_t cap) {
    char qpath[4096], qscheme[1024], qconfig[512];
    char dest[512], qdest[600];
    BdStatus s;

    if ((s = bd_quote(rc->project, qpath,   sizeof(qpath)))   != BD_OK) return s;
    if ((s = bd_quote(rc->scheme,  qscheme,  sizeof(qscheme))) != BD_OK) return s;
    if ((s = bd_quote(rc->config,  qconfig,  sizeof(qconfig))) != BD_OK) return s;
    if ((s = bd_destination(tgt, has_target, dest, sizeof(dest)))  != BD_OK) return s;
    if ((s = bd_quote(dest, qdest, sizeof(qdest)))             != BD_OK) return s;

    size_t path_len = strlen(rc->project);
    bool is_ws = str_ends_with(rc->project, path_len,
                               ".xcworkspace", sizeof(".xcworkspace") - 1);
    const char *flag = is_ws ? "-workspace" : "-project";

    int n = snprintf(buf, cap,
        "xcodebuild -showBuildSettings -json %s %s"
        " -scheme %s -configuration %s -destination %s",
        flag, qpath, qscheme, qconfig, qdest);
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

/*
 * Build the xcodebuild build command, wrapped in setsid so the
 * entire process group is killable.  The shell emits a PID marker
 * line before exec'ing xcodebuild, letting the worker recover the
 * PGID for the two-pronged abort.
 *
 * We pass project/scheme/config/dest via env-var assignment so the
 * single-quote-escaped values work in the outer shell without needing
 * nested single-quote escaping inside the setsid'd sh -c argument.
 */
BdStatus bd_build_cmd(const RunConfig *rc, const Target *tgt,
                      bool has_target, char *buf, size_t cap) {
    char qpath[4096], qscheme[1024], qconfig[512];
    char dest[512], qdest[600];
    BdStatus s;

    if ((s = bd_quote(rc->project, qpath,   sizeof(qpath)))   != BD_OK) return s;
    if ((s = bd_quote(rc->scheme,  qscheme,  sizeof(qscheme))) != BD_OK) return s;
    if ((s = bd_quote(rc->config,  qconfig,  sizeof(qconfig))) != BD_OK) return s;
    if ((s = bd_destination(tgt, has_target, dest, sizeof(dest)))  != BD_OK) return s;
    if ((s = bd_quote(dest, qdest, sizeof(qdest)))             != BD_OK) return s;

    size_t path_len = strlen(rc->project);
    bool is_ws = str_ends_with(rc->project, path_len,
                               ".xcworkspace", sizeof(".xcworkspace") - 1);
    const char *flag = is_ws ? "-workspace" : "-project";

    /* The setsid'd shell is the session leader; $$ is its PID = PGID.
       The inner shell expands $__BD_* env vars with double-quoting. */
    int n = snprintf(buf, cap,
        "__BD_PROJ=%s __BD_SCHEME=%s __BD_CONFIG=%s __BD_DEST=%s "
        "setsid sh -c "
        "'printf \"__OSTRICH_PGID__%%d\\n\" $$; "
        "exec xcodebuild %s \"$__BD_PROJ\""
        " -scheme \"$__BD_SCHEME\""
        " -configuration \"$__BD_CONFIG\""
        " -destination \"$__BD_DEST\"'",
        qpath, qscheme, qconfig, qdest, flag);
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

BdStatus bd_boot_cmd(const Target *tgt, char *buf, size_t cap) {
    char qudid[300];
    BdStatus s = bd_quote(tgt->udid, qudid, sizeof(qudid));
    if (s != BD_OK) return s;

    int n = snprintf(buf, cap, "xcrun simctl boot %s", qudid);
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

BdStatus bd_bootstatus_cmd(const Target *tgt, char *buf, size_t cap) {
    char qudid[300];
    BdStatus s = bd_quote(tgt->udid, qudid, sizeof(qudid));
    if (s != BD_OK) return s;

    int n = snprintf(buf, cap, "xcrun simctl bootstatus %s --wait", qudid);
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

BdStatus bd_install_cmd(const Target *tgt, const char *app_path,
                        char *buf, size_t cap) {
    char qudid[300], qapp[4096];
    BdStatus s;

    if ((s = bd_quote(tgt->udid,  qudid, sizeof(qudid))) != BD_OK) return s;
    if ((s = bd_quote(app_path,   qapp,  sizeof(qapp)))  != BD_OK) return s;

    int n;
    if (tgt->is_simulator) {
        n = snprintf(buf, cap, "xcrun simctl install %s %s", qudid, qapp);
    } else {
        n = snprintf(buf, cap,
            "xcrun devicectl device install app --device %s %s",
            qudid, qapp);
    }
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

/*
 * Launch the app with --console so its stdout/stderr streams to the
 * worker channel.  Wrapped in setsid + PID marker so the DevConsole
 * channel can be forcefully closed if the terminate path doesn't EOF it.
 */
BdStatus bd_launch_cmd(const Target *tgt, const char *bundle_id,
                       char *buf, size_t cap) {
    char qudid[300], qbundle[512];
    BdStatus s;

    if ((s = bd_quote(tgt->udid,  qudid,   sizeof(qudid)))   != BD_OK) return s;
    if ((s = bd_quote(bundle_id,  qbundle,  sizeof(qbundle))) != BD_OK) return s;

    int n;
    if (tgt->is_simulator) {
        n = snprintf(buf, cap,
            "__BD_UDID=%s __BD_BUNDLE=%s "
            "setsid sh -c "
            "'printf \"__OSTRICH_PGID__%%d\\n\" $$; "
            "exec xcrun simctl launch --console"
            " \"$__BD_UDID\" \"$__BD_BUNDLE\"'",
            qudid, qbundle);
    } else {
        n = snprintf(buf, cap,
            "__BD_UDID=%s __BD_BUNDLE=%s "
            "setsid sh -c "
            "'printf \"__OSTRICH_PGID__%%d\\n\" $$; "
            "exec xcrun devicectl device process launch --console"
            " --device \"$__BD_UDID\" \"$__BD_BUNDLE\"'",
            qudid, qbundle);
    }
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

BdStatus bd_terminate_cmd(const Target *tgt, const char *bundle_id,
                           char *buf, size_t cap) {
    char qudid[300], qbundle[512];
    BdStatus s;

    if ((s = bd_quote(tgt->udid,  qudid,   sizeof(qudid)))   != BD_OK) return s;
    if ((s = bd_quote(bundle_id,  qbundle,  sizeof(qbundle))) != BD_OK) return s;

    int n;
    if (tgt->is_simulator) {
        n = snprintf(buf, cap, "xcrun simctl terminate %s %s", qudid, qbundle);
    } else {
        n = snprintf(buf, cap,
            "xcrun devicectl device process terminate"
            " --device %s --bundle-identifier %s",
            qudid, qbundle);
    }
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

BdStatus bd_kill_cmd(long pgid, char *buf, size_t cap) {
    int n = snprintf(buf, cap, "kill -- -%ld", pgid);
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

/* ── parsers ─────────────────────────────────────────────────────── */

/*
 * Extract the built .app path from `xcodebuild -showBuildSettings -json`.
 * Looks for BUILT_PRODUCTS_DIR and FULL_PRODUCT_NAME in the first
 * target's buildSettings; concatenates them as the complete .app path.
 */
BdStatus bd_parse_product_path(Str settings_json, char *out, size_t cap) {
    if (!settings_json.data || settings_json.len == 0 || !out || cap == 0)
        return BD_ERR_PARSE;

    jsmntok_t toks[PARSE_SETTINGS_TOKS];
    jsmn_parser p;
    jsmn_init(&p);
    int r = jsmn_parse(&p, settings_json.data, settings_json.len,
                       toks, PARSE_SETTINGS_TOKS);
    if (r < 0) return BD_ERR_PARSE;
    int n = r;

    /* Top-level is an array of per-target objects. */
    if (n == 0 || toks[0].type != JSMN_ARRAY) return BD_ERR_PARSE;

    int i = 1;
    for (int el = 0; el < toks[0].size && i < n; el++) {
        if (toks[i].type != JSMN_OBJECT) {
            i += tok_tree_size(toks, i);
            continue;
        }

        /* Find the "buildSettings" object. */
        int bs_idx = -1;
        int j = i + 1;
        for (int k = 0; k < toks[i].size && j < n; k++) {
            if (jstr_eq(settings_json.data, &toks[j], "buildSettings") &&
                j + 1 < n && toks[j + 1].type == JSMN_OBJECT) {
                bs_idx = j + 1;
                break;
            }
            j += tok_tree_size(toks, j);
        }

        if (bs_idx >= 0) {
            char built_dir[2048]  = {0};
            char product_name[512] = {0};

            int ki = bs_idx + 1;
            for (int k = 0; k < toks[bs_idx].size && ki < n; k++) {
                if (jstr_eq(settings_json.data, &toks[ki],
                            "BUILT_PRODUCTS_DIR") &&
                    ki + 1 < n && toks[ki + 1].type == JSMN_STRING) {
                    jstr_copy(settings_json.data, &toks[ki + 1],
                              built_dir, sizeof(built_dir));
                } else if (jstr_eq(settings_json.data, &toks[ki],
                                   "FULL_PRODUCT_NAME") &&
                           ki + 1 < n && toks[ki + 1].type == JSMN_STRING) {
                    jstr_copy(settings_json.data, &toks[ki + 1],
                              product_name, sizeof(product_name));
                }
                ki += tok_tree_size(toks, ki);
            }

            if (built_dir[0] && product_name[0]) {
                int written = snprintf(out, cap, "%s/%s",
                                       built_dir, product_name);
                if (written < 0 || (size_t)written >= cap) return BD_ERR_OOM;
                return BD_OK;
            }
        }

        i += tok_tree_size(toks, i);
    }
    return BD_ERR_PARSE;
}

/*
 * Search a raw chunk for the PID marker emitted by bd_build_cmd and
 * bd_launch_cmd.  Returns true and sets *out_pgid if found.
 */
bool bd_parse_pid_marker(Str chunk, long *out_pgid) {
    static const char marker[] = "__OSTRICH_PGID__";
    const size_t mlen = sizeof(marker) - 1;

    if (!chunk.data || chunk.len < mlen || !out_pgid) return false;

    for (size_t i = 0; i + mlen <= chunk.len; i++) {
        if (memcmp(chunk.data + i, marker, mlen) != 0) continue;

        /* Copy the digit sequence after the marker into a null-terminated
           buffer so strtol can safely parse it. */
        size_t num_start = i + mlen;
        char num_buf[24];
        size_t num_len = 0;
        while (num_start + num_len < chunk.len &&
               num_len < sizeof(num_buf) - 1) {
            char c = chunk.data[num_start + num_len];
            if (c < '0' || c > '9') break;
            num_buf[num_len++] = c;
        }
        if (num_len == 0) continue;
        num_buf[num_len] = '\0';

        long pgid = strtol(num_buf, NULL, 10);
        if (pgid > 0) {
            *out_pgid = pgid;
            return true;
        }
    }
    return false;
}

/* ── remediation text ────────────────────────────────────────────── */

BdStatus bd_setsid_help_block(const char *user, const char *host, int port,
                               char *buf, size_t cap) {
    int n;
    if (port == 22) {
        n = snprintf(buf, cap,
            "> \xe2\x94\x80\xe2\x94\x80 REMEDIATION \xe2\x94\x80\xe2\x94\x80\n"
            "REMOTE MAC IS MISSING setsid.\n"
            "\n"
            "To install, connect to the Mac:\n"
            "    ssh %s@%s\n"
            "\n"
            "Then on the Mac, run:\n"
            "    brew install util-linux\n"
            "\n"
            "Then add ONE of the following to ~/.zshenv, matching your Mac:\n"
            "    # Apple Silicon:\n"
            "    echo 'export PATH=\"/opt/homebrew/opt/util-linux/bin:$PATH\"' >> ~/.zshenv\n"
            "    # Intel:\n"
            "    echo 'export PATH=\"/usr/local/opt/util-linux/bin:$PATH\"' >> ~/.zshenv\n"
            "\n"
            "(Substitute your own path if util-linux is installed elsewhere.)\n"
            "\n"
            "Verify the fix with (from this host):\n"
            "    ssh %s@%s 'command -v setsid'\n"
            "\n"
            "(See README \"Remote Mac (SSH target)\" for context.)\n"
            "> \xe2\x94\x80\xe2\x94\x80 END REMEDIATION \xe2\x94\x80\xe2\x94\x80\n",
            user, host, user, host);
    } else {
        n = snprintf(buf, cap,
            "> \xe2\x94\x80\xe2\x94\x80 REMEDIATION \xe2\x94\x80\xe2\x94\x80\n"
            "REMOTE MAC IS MISSING setsid.\n"
            "\n"
            "To install, connect to the Mac:\n"
            "    ssh -p %d %s@%s\n"
            "\n"
            "Then on the Mac, run:\n"
            "    brew install util-linux\n"
            "\n"
            "Then add ONE of the following to ~/.zshenv, matching your Mac:\n"
            "    # Apple Silicon:\n"
            "    echo 'export PATH=\"/opt/homebrew/opt/util-linux/bin:$PATH\"' >> ~/.zshenv\n"
            "    # Intel:\n"
            "    echo 'export PATH=\"/usr/local/opt/util-linux/bin:$PATH\"' >> ~/.zshenv\n"
            "\n"
            "(Substitute your own path if util-linux is installed elsewhere.)\n"
            "\n"
            "Verify the fix with (from this host):\n"
            "    ssh -p %d %s@%s 'command -v setsid'\n"
            "\n"
            "(See README \"Remote Mac (SSH target)\" for context.)\n"
            "> \xe2\x94\x80\xe2\x94\x80 END REMEDIATION \xe2\x94\x80\xe2\x94\x80\n",
            port, user, host, port, user, host);
    }
    if (n < 0 || (size_t)n >= cap) return BD_ERR_OOM;
    return BD_OK;
}

/* ── classification ──────────────────────────────────────────────── */

LexKey bd_reason_lex(BdStatus st) {
    switch (st) {
    case BD_ERR_XCODE_MISSING:  return LEX_REC_ERR_XCODE;
    case BD_ERR_SETSID_MISSING: return LEX_REC_ERR_SETSID;
    case BD_ERR_BUILD:          return LEX_RUN_BUILD_FAILED;
    case BD_ERR_PARSE:          return LEX_RUN_BUILD_FAILED;
    case BD_ERR_BOOT:
    case BD_ERR_INSTALL:
    case BD_ERR_LAUNCH:         return LEX_RUN_DEPLOY_FAILED;
    default:                    return LEX_RUN_BUILD_FAILED;
    }
}

const char *bd_status_str(BdStatus st) {
    switch (st) {
    case BD_OK:                 return "ok";
    case BD_ERR_XCODE_MISSING:  return "xcode not found";
    case BD_ERR_SETSID_MISSING: return "setsid not found";
    case BD_ERR_BUILD:          return "build failed";
    case BD_ERR_BOOT:           return "boot failed";
    case BD_ERR_INSTALL:        return "install failed";
    case BD_ERR_LAUNCH:         return "launch failed";
    case BD_ERR_PARSE:          return "parse error";
    case BD_ERR_OOM:            return "out of memory";
    default:                    return "(unknown)";
    }
}
