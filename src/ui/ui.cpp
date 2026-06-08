/* C++ internals for the ui module — hidden behind include/ui.h. */

#include "ui.h"

#include <GLFW/glfw3.h>

#include "arena.h"
#include "framestats.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "lexicon.h"
#include "logbuf.h"
#include "runstate.h"

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *GLSL_VERSION = "#version 150";

/* ── palette — context/theme.md ─────────────────────────────────────── */
static const ImVec4 C_BG = {0.027f, 0.035f, 0.051f, 1.0f};          /* #07090d */
static const ImVec4 C_PANEL = {0.051f, 0.067f, 0.090f, 1.0f};       /* #0d1117 */
static const ImVec4 C_CYAN_DIM = {0.055f, 0.353f, 0.388f, 1.0f};    /* #0e5a63 */
static const ImVec4 C_MAGENTA_DIM = {0.431f, 0.078f, 0.341f, 1.0f}; /* #6e1457 */
static const ImVec4 C_CYAN = {0.000f, 0.941f, 1.000f, 1.0f};        /* #00f0ff */
static const ImVec4 C_MAGENTA = {1.000f, 0.169f, 0.839f, 1.0f};     /* #ff2bd6 */
/* semantic — defined, reserved for meaning */
static const ImVec4 C_OK = {0.098f, 1.000f, 0.478f, 1.0f};   /* #19ff7a */
static const ImVec4 C_FAIL = {1.000f, 0.231f, 0.314f, 1.0f}; /* #ff3b50 */
static const ImVec4 C_BUSY = {1.000f, 0.690f, 0.000f, 1.0f}; /* #ffb000 */
static const ImVec4 C_TEXT = {0.784f, 0.816f, 0.847f, 1.0f}; /* #c8d0d8 */
static const ImVec4 C_NONE = {0.000f, 0.000f, 0.000f, 0.0f}; /* transparent */

static void apply_theme(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImVec4 *c = s.Colors;

    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.ScrollbarRounding = 2.0f;
    s.GrabRounding = 2.0f;
    s.TabRounding = 0.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;

    c[ImGuiCol_Text] = C_TEXT;
    c[ImGuiCol_TextDisabled] = {C_TEXT.x * 0.4f, C_TEXT.y * 0.4f, C_TEXT.z * 0.4f, 1.0f};
    c[ImGuiCol_WindowBg] = C_PANEL;
    c[ImGuiCol_ChildBg] = C_BG;
    c[ImGuiCol_PopupBg] = C_PANEL;
    c[ImGuiCol_Border] = C_CYAN_DIM;
    c[ImGuiCol_BorderShadow] = C_NONE;
    c[ImGuiCol_FrameBg] = C_BG;
    c[ImGuiCol_FrameBgHovered] = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.4f};
    c[ImGuiCol_FrameBgActive] = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.7f};
    c[ImGuiCol_TitleBg] = C_BG;
    c[ImGuiCol_TitleBgActive] = C_PANEL;
    c[ImGuiCol_TitleBgCollapsed] = C_BG;
    c[ImGuiCol_MenuBarBg] = C_BG;
    c[ImGuiCol_ScrollbarBg] = C_BG;
    c[ImGuiCol_ScrollbarGrab] = C_CYAN_DIM;
    c[ImGuiCol_ScrollbarGrabHovered] = C_CYAN;
    c[ImGuiCol_ScrollbarGrabActive] = C_CYAN;
    c[ImGuiCol_CheckMark] = C_CYAN;
    c[ImGuiCol_CheckboxSelectedBg] = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.3f};
    c[ImGuiCol_SliderGrab] = C_CYAN_DIM;
    c[ImGuiCol_SliderGrabActive] = C_CYAN;
    c[ImGuiCol_Button] = C_MAGENTA_DIM;
    c[ImGuiCol_ButtonHovered] = C_MAGENTA;
    c[ImGuiCol_ButtonActive] = C_MAGENTA;
    c[ImGuiCol_Header] = C_PANEL;
    c[ImGuiCol_HeaderHovered] = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.5f};
    c[ImGuiCol_HeaderActive] = C_CYAN_DIM;
    c[ImGuiCol_Separator] = C_CYAN_DIM;
    c[ImGuiCol_SeparatorHovered] = C_CYAN;
    c[ImGuiCol_SeparatorActive] = C_CYAN;
    c[ImGuiCol_ResizeGrip] = {C_MAGENTA_DIM.x, C_MAGENTA_DIM.y, C_MAGENTA_DIM.z, 0.4f};
    c[ImGuiCol_ResizeGripHovered] = C_MAGENTA;
    c[ImGuiCol_ResizeGripActive] = C_MAGENTA;
    c[ImGuiCol_InputTextCursor] = C_CYAN;
    c[ImGuiCol_Tab] = C_BG;
    c[ImGuiCol_TabHovered] = C_CYAN_DIM;
    c[ImGuiCol_TabSelected] = C_PANEL;
    c[ImGuiCol_TabSelectedOverline] = C_CYAN;
    c[ImGuiCol_TabDimmed] = C_BG;
    c[ImGuiCol_TabDimmedSelected] = {C_PANEL.x, C_PANEL.y, C_PANEL.z, 0.7f};
    c[ImGuiCol_TabDimmedSelectedOverline] = C_NONE;
    c[ImGuiCol_DockingPreview] = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.3f};
    c[ImGuiCol_DockingEmptyBg] = C_BG;
    c[ImGuiCol_PlotLines] = C_CYAN;
    c[ImGuiCol_PlotLinesHovered] = C_CYAN;
    c[ImGuiCol_PlotHistogram] = C_CYAN_DIM;
    c[ImGuiCol_PlotHistogramHovered] = C_CYAN;
    c[ImGuiCol_TableHeaderBg] = C_PANEL;
    c[ImGuiCol_TableBorderStrong] = C_CYAN_DIM;
    c[ImGuiCol_TableBorderLight] = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.3f};
    c[ImGuiCol_TableRowBg] = C_NONE;
    c[ImGuiCol_TableRowBgAlt] = {C_PANEL.x, C_PANEL.y, C_PANEL.z, 0.5f};
    c[ImGuiCol_TextLink] = C_CYAN;
    c[ImGuiCol_TextSelectedBg] = {C_MAGENTA_DIM.x, C_MAGENTA_DIM.y, C_MAGENTA_DIM.z, 0.5f};
    c[ImGuiCol_TreeLines] = C_CYAN_DIM;
    c[ImGuiCol_DragDropTarget] = C_CYAN;
    c[ImGuiCol_DragDropTargetBg] = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.15f};
    c[ImGuiCol_UnsavedMarker] = C_CYAN;
    c[ImGuiCol_NavCursor] = C_CYAN;
    c[ImGuiCol_NavWindowingHighlight] = C_CYAN;
    c[ImGuiCol_NavWindowingDimBg] = {0.0f, 0.0f, 0.0f, 0.6f};
    c[ImGuiCol_ModalWindowDimBg] = {0.0f, 0.0f, 0.0f, 0.6f};
}

struct Ui {
    GLFWwindow *window;
    FrameStats fs;
    double last_time;
    double online_since; /* glfwGetTime() when phase last became ONLINE; 0 = not online */
    ConnPhase prev_phase;
    float log_split; /* build-log / live-feed split ratio (0..1), session-only */
};

static bool has_display(void) {
#ifdef __linux__
    return getenv("DISPLAY") != nullptr || getenv("WAYLAND_DISPLAY") != nullptr;
#else
    return true;
#endif
}

enum class ReadErr { Ok, NotFound, Oom };

static void *read_file_arena(Arena *a, const char *path, long *out_size, ReadErr &err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        err = ReadErr::NotFound;
        return nullptr;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        err = ReadErr::NotFound;
        return nullptr;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        err = ReadErr::NotFound;
        return nullptr;
    }
    rewind(f);
    void *buf = arena_alloc(a, (size_t)size, 1);
    if (!buf) {
        fclose(f);
        err = ReadErr::Oom;
        return nullptr;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if ((long)n != size) {
        err = ReadErr::NotFound;
        return nullptr;
    }
    *out_size = size;
    err = ReadErr::Ok;
    return buf;
}

static void draw_overlay(void) {
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const ImGuiIO &io = ImGui::GetIO();
    const float w = io.DisplaySize.x;
    const float h = io.DisplaySize.y;

    /* scanlines: faint dark stripe on every other row */
    const ImU32 scan = IM_COL32(0, 0, 0, 8);
    for (float y = 0.0f; y < h; y += 2.0f) dl->AddRectFilled({0.0f, y}, {w, y + 1.0f}, scan);

    /* vignette: four gradient fills fading inward from each edge */
    const ImU32 edge = IM_COL32(0, 0, 0, 110);
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float vw = w * 0.42f;
    const float vh = h * 0.42f;

    dl->AddRectFilledMultiColor({0.0f, 0.0f}, {vw, h}, edge, clear, clear, edge);  /* left  */
    dl->AddRectFilledMultiColor({w - vw, 0.0f}, {w, h}, clear, edge, edge, clear); /* right */
    dl->AddRectFilledMultiColor({0.0f, 0.0f}, {w, vh}, edge, edge, clear, clear);  /* top   */
    dl->AddRectFilledMultiColor({0.0f, h - vh}, {w, h}, clear, clear, edge, edge); /* bottom */
}

/* ── BREACH overlay ─────────────────────────────────────────────────── */
static void draw_breach_overlay(const UiConnView *view, ConnForm *form, UiIntents *out) {
    const ImGuiIO &io = ImGui::GetIO();
    const float cx = io.DisplaySize.x * 0.5f;
    const float cy = io.DisplaySize.y * 0.5f;
    bool connecting = (view->phase == CONN_CONNECTING || view->phase == CONN_AWAITING_HOSTKEY);

    ImGui::SetNextWindowPos({cx, cy}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.0f, 0.0f}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##breach_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    bool first_show = ImGui::IsWindowAppearing();
    bool passkey_enter = false;

    /* Header */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
    ImGui::TextUnformatted(lex(LEX_CONN_UPLINK));
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    /* Form fields */
    ImGuiInputTextFlags ro = connecting ? ImGuiInputTextFlags_ReadOnly : 0;
    const float lw = 60.0f; /* label column width */

    /* HOST */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_HOST));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1.0f);
    if (first_show && !connecting) ImGui::SetKeyboardFocusHere();
    bool host_enter =
        ImGui::InputText("##host", form->host, sizeof(form->host), ro | ImGuiInputTextFlags_EnterReturnsTrue);

    /* PORT */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_PORT));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(90.0f);
    bool port_enter = ImGui::InputText("##port", form->port, sizeof(form->port),
                                       ro | ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal);

    /* USER */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_USER));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1.0f);
    bool user_enter =
        ImGui::InputText("##user", form->user, sizeof(form->user), ro | ImGuiInputTextFlags_EnterReturnsTrue);

    /* AUTH toggle: SSH-AGENT or PASSKEY */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_AUTH));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    {
        const char *auth_label =
            (form->auth == SSH_AUTH_PASSWORD) ? lex(LEX_CONN_AUTH_PASSKEY) : lex(LEX_CONN_AUTH_AGENT);
        if (connecting) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
            ImGui::TextUnformatted(auth_label);
            ImGui::PopStyleColor();
        } else {
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("##auth", auth_label)) {
                if (ImGui::Selectable(lex(LEX_CONN_AUTH_AGENT), form->auth == SSH_AUTH_AGENT))
                    form->auth = SSH_AUTH_AGENT;
                if (ImGui::Selectable(lex(LEX_CONN_AUTH_PASSKEY), form->auth == SSH_AUTH_PASSWORD))
                    form->auth = SSH_AUTH_PASSWORD;
                ImGui::EndCombo();
            }
        }
    }

    /* PASSKEY field — only visible when auth == SSH_AUTH_PASSWORD */
    if (form->auth == SSH_AUTH_PASSWORD) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_CONN_AUTH_PASSKEY));
        ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(-1.0f);
        passkey_enter = ImGui::InputText("##passkey", form->passkey, sizeof(form->passkey),
                                         ro | ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    }

    /* REMEMBER PASSKEY — checkbox left, copy right: [ ] REMEMBER PASSKEY.
       This row breaks the label-column grid (the copy is wider than lw),
       so we draw the widget first and the label after via SameLine(). */
    if (form->auth == SSH_AUTH_PASSWORD) {
        if (connecting) ImGui::BeginDisabled();
        ImGui::Checkbox("##remember", &form->remember);
        if (connecting) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_CONN_REMEMBER_PASSKEY));
        ImGui::PopStyleColor();
    }

    /* KNOWN HOSTS */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_KNOWN_HOSTS));
    ImGui::PopStyleColor();

    if (view->known_count == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_CONN_NO_KNOWN_HOSTS));
        ImGui::PopStyleColor();
    } else {
        int max_vis = (view->known_count < 4) ? view->known_count : 4;
        float list_h = ImGui::GetTextLineHeightWithSpacing() * (float)max_vis;
        ImGui::BeginChild("##kh", {0.0f, list_h}, false);
        for (int i = 0; i < view->known_count; ++i) {
            const Conn *c = &view->known_hosts[i];
            const char *lbl = (c->label[0] != '\0') ? c->label : c->host;
            char buf[270]; /* "o " + max(label=63, host=255) + null */
            snprintf(buf, sizeof(buf), "o %s", lbl);
            bool sel = (form->selected_known_host == i);
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? C_CYAN : C_TEXT);
            if (!connecting && ImGui::Selectable(buf, sel))
                out->select_host = i;
            else if (connecting)
                ImGui::TextUnformatted(buf);
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* Status line: TOFU prompt, mismatch stop, connecting, failure, or idle */
    if (view->show_hostkey_prompt) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_BUSY);
        ImGui::Text("%s %s", lex(LEX_CONN_UNKNOWN_HOST),
                    (view->fingerprint && view->fingerprint[0]) ? view->fingerprint : "");
        ImGui::PopStyleColor();
    } else if (view->show_mismatch) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
        ImGui::TextUnformatted(lex(LEX_CONN_ERR_HOSTKEY_MISMATCH));
        if (view->fingerprint && view->fingerprint[0]) ImGui::Text("  %s", view->fingerprint);
        ImGui::PopStyleColor();
    } else if (connecting) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_BUSY);
        ImGui::TextUnformatted(lex(LEX_CONN_BREACHING));
        ImGui::PopStyleColor();
    } else if (view->reason && view->reason[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_MAGENTA);
        ImGui::Text("%s %s", lex(LEX_VOICE_PREFIX), view->reason);
        ImGui::PopStyleColor();
    } else {
        ImGui::TextUnformatted(" "); /* placeholder keeps height stable */
    }

    ImGui::Spacing();

    /* Action buttons */
    if (view->show_hostkey_prompt) {
        /* TOFU: TRUST (green) and DECLINE (red) */
        ImGui::PushStyleColor(ImGuiCol_Button, {C_OK.x * 0.25f, C_OK.y * 0.25f, C_OK.z * 0.25f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {C_OK.x * 0.45f, C_OK.y * 0.45f, C_OK.z * 0.45f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_OK);
        if (ImGui::Button(lex(LEX_CONN_TRUST))) out->trust = true;
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button, {C_FAIL.x * 0.25f, C_FAIL.y * 0.25f, C_FAIL.z * 0.25f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {C_FAIL.x * 0.45f, C_FAIL.y * 0.45f, C_FAIL.z * 0.45f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_FAIL);
        if (ImGui::Button(lex(LEX_CONN_DECLINE))) out->decline = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) out->decline = true;
        ImGui::PopStyleColor(3);
    } else if (connecting) {
        if (ImGui::Button(lex(LEX_CONN_ABORT))) out->abort = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) out->abort = true;
    } else {
        bool valid = (form->host[0] != '\0' && form->user[0] != '\0' &&
                      (form->auth == SSH_AUTH_AGENT || form->passkey[0] != '\0'));
        bool enter = host_enter || port_enter || user_enter || passkey_enter;

        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button(lex(LEX_CONN_BREACH)) || (valid && enter)) out->breach = true;
        if (!valid) ImGui::EndDisabled();

        ImGui::SameLine();

        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button(lex(LEX_CONN_SAVE))) out->save = true;
        if (!valid) ImGui::EndDisabled();
    }

    ImGui::End();
}

/* ── connection bar ─────────────────────────────────────────────────── */
/* ── recon panel (ONLINE / REACQUIRING) — compact 4-row layout ──────── */
static void draw_recon_panel(const UiReconView *rv, RunConfig *rf, UiReconIntents *ri, ImVec2 pos, ImVec2 sz) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN_DIM);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##recon_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    const float avail = ImGui::GetContentRegionAvail().x;

    /* ── Row 1: PROJECT path + [v] picker popup ───────────────────────── */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted("PROJECT");
    ImGui::PopStyleColor();
    ImGui::SameLine(70.0f);
    {
        const float btn_w = ImGui::CalcTextSize("[v]").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SetNextItemWidth(-(btn_w + ImGui::GetStyle().ItemSpacing.x));
    }
    ImGui::InputTextWithHint("##project", "path/to/App.xcworkspace", rf->project, sizeof(rf->project));
    ImGui::SameLine();
    if (ImGui::Button("[v]")) ImGui::OpenPopup("##project_picker");

    /* Project picker popup: scan controls + blueprint results */
    if (ImGui::BeginPopup("##project_picker")) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_REC_FIELD_SCAN_ROOT));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(280.0f);
        if (rv->scanning) ImGui::BeginDisabled();
        ImGui::InputText("##scan_root", rf->scan_root, sizeof(rf->scan_root));
        if (rv->scanning) ImGui::EndDisabled();

        if (rv->scanning) {
            if (ImGui::Button(lex(LEX_REC_ABORT_SCAN))) ri->abort_scan = true;
        } else {
            bool can_scan = (rf->scan_root[0] != '\0');
            if (!can_scan) ImGui::BeginDisabled();
            if (ImGui::Button(lex(LEX_REC_SCAN_HOST))) ri->scan = true;
            if (!can_scan) ImGui::EndDisabled();
        }

        ImGui::Separator();

        if (rv->scan_done) {
            if (rv->scan_err == DISC_ERR_XCODE_MISSING) {
                ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
                ImGui::Text("%s %s", lex(LEX_VOICE_PREFIX), lex(LEX_REC_ERR_XCODE));
                ImGui::PopStyleColor();
            } else if (rv->scan_err != DISC_OK) {
                ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
                ImGui::Text("%s %s", lex(LEX_VOICE_PREFIX), lex(LEX_REC_ERR_INVENTORY));
                ImGui::PopStyleColor();
            } else if (!rv->blueprints || rv->blueprints->count == 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
                ImGui::TextUnformatted(lex(LEX_REC_NO_BLUEPRINTS));
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
                ImGui::TextUnformatted(lex(LEX_REC_BLUEPRINTS));
                ImGui::PopStyleColor();
                int vis = rv->blueprints->count < 8 ? rv->blueprints->count : 8;
                float list_h = ImGui::GetTextLineHeightWithSpacing() * (float)vis;
                ImGui::BeginChild("##blueprints", {0.0f, list_h}, false);
                for (int i = 0; i < rv->blueprints->count; i++) {
                    const Blueprint *bp = &rv->blueprints->items[i];
                    bool sel = (rv->blueprint_selected == i);
                    const char *disp = bp->path;
                    const char *sl = strrchr(bp->path, '/');
                    if (sl) disp = sl + 1;
                    char buf[1040]; /* glyph(3) + space(1) + path component(1024) + null */
                    snprintf(buf, sizeof(buf), "%s %s", bp->is_workspace ? "\xe2\x8c\x96" : "\xe2\x97\x8b", disp);
                    ImGui::PushStyleColor(ImGuiCol_Text, sel ? C_CYAN : C_TEXT);
                    if (ImGui::Selectable(buf, sel)) {
                        ri->pick_blueprint = i;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bp->path);
                }
                ImGui::EndChild();
            }
        } else if (!rv->scanning) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted(lex(LEX_REC_NO_OP));
            ImGui::PopStyleColor();
        }

        ImGui::EndPopup();
    }

    /* ── Row 2: SCHEME / CONFIG / BUNDLE ID — 3-column table ─────────── */
    bool reading = rv->reading_blueprint;
    bool resolving = rv->resolving_bundle_id;

    if (ImGui::BeginTable("##row2_fields", 3, ImGuiTableFlags_None)) {
        ImGui::TableNextRow();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_REC_FIELD_SCHEME));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (reading) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##scheme", rf->scheme, sizeof(rf->scheme));
        if (!reading && ImGui::IsItemEdited()) ri->scheme_edited = true;
        if (rv->schemes && rv->schemes->count > 0 && ImGui::IsItemHovered()) {
            char hint[1024];
            int off = snprintf(hint, sizeof(hint), "> discovered:");
            for (int i = 0; i < rv->schemes->count && off < (int)sizeof(hint) - 2; i++)
                off += snprintf(hint + off, sizeof(hint) - (size_t)off, "\n  %s", rv->schemes->items[i]);
            ImGui::SetTooltip("%s", hint);
        }
        if (reading) ImGui::EndDisabled();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_REC_FIELD_CONFIG));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (reading) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##config", rf->config, sizeof(rf->config));
        if (!reading && ImGui::IsItemEdited()) ri->config_edited = true;
        if (rv->configs && rv->configs->count > 0 && ImGui::IsItemHovered()) {
            char hint[512];
            int off = snprintf(hint, sizeof(hint), "> discovered:");
            for (int i = 0; i < rv->configs->count && off < (int)sizeof(hint) - 2; i++)
                off += snprintf(hint + off, sizeof(hint) - (size_t)off, "\n  %s", rv->configs->items[i]);
            ImGui::SetTooltip("%s", hint);
        }
        if (reading) ImGui::EndDisabled();

        ImGui::TableNextColumn();
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_REC_FIELD_BUNDLE_ID));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##bundle_id", resolving ? "RESOLVING..." : "", rf->bundle_id, sizeof(rf->bundle_id));
        if (ImGui::IsItemEdited()) ri->bundle_id_edited = true;

        ImGui::EndTable();
    }

    /* ── Row 3: PRESET dropdown + new / rename / delete ──────────────── */
    static char s_new_name[64];
    static char s_rename_buf[64];

    bool has_presets = (rv->presets && rv->presets->count > 0);
    bool has_sel = has_presets && rv->preset_selected >= 0 && rv->preset_selected < rv->presets->count;
    const char *preset_label = has_sel ? rv->presets->items[rv->preset_selected].name : lex(LEX_REC_NO_OP);

    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_REC_FIELD_PRESET));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(avail * 0.28f);
    if (ImGui::BeginCombo("##preset_combo", preset_label)) {
        if (!has_presets) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted(lex(LEX_REC_NO_OP));
            ImGui::PopStyleColor();
        } else {
            for (int i = 0; i < rv->presets->count; i++) {
                const Preset *p = &rv->presets->items[i];
                bool sel = (rv->preset_selected == i);
                ImGui::PushStyleColor(ImGuiCol_Text, sel ? C_CYAN : C_TEXT);
                if (ImGui::Selectable(p->name, sel)) ri->pick_preset = i;
                ImGui::PopStyleColor();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button(lex(LEX_REC_PRESET_NEW))) {
        memset(s_new_name, 0, sizeof(s_new_name));
        ImGui::OpenPopup("##preset_new");
    }
    ImGui::SameLine();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button(lex(LEX_REC_PRESET_RENAME))) {
        if (has_sel) snprintf(s_rename_buf, sizeof(s_rename_buf), "%s", rv->presets->items[rv->preset_selected].name);
        ImGui::OpenPopup("##preset_rename");
    }
    if (!has_sel) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!has_sel) ImGui::BeginDisabled();
    if (ImGui::Button(lex(LEX_REC_PRESET_DELETE))) ri->preset_delete = true;
    if (!has_sel) ImGui::EndDisabled();

    if (ImGui::BeginPopup("##preset_new")) {
        ImGui::SetNextItemWidth(200.0f);
        bool enter = ImGui::InputText("##pname", s_new_name, sizeof(s_new_name), ImGuiInputTextFlags_EnterReturnsTrue);
        bool ok = ImGui::Button("OK") || enter;
        if (ok && s_new_name[0] != '\0') {
            snprintf(ri->preset_name, sizeof(ri->preset_name), "%s", s_new_name);
            ri->preset_new = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##preset_rename")) {
        ImGui::SetNextItemWidth(200.0f);
        bool enter =
            ImGui::InputText("##prename", s_rename_buf, sizeof(s_rename_buf), ImGuiInputTextFlags_EnterReturnsTrue);
        bool ok = ImGui::Button("OK") || enter;
        if (ok && s_rename_buf[0] != '\0') {
            snprintf(ri->preset_name, sizeof(ri->preset_name), "%s", s_rename_buf);
            ri->preset_rename = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ── Row 4: TARGET dropdown + sweep ──────────────────────────────── */
    bool has_targets = (rv->targets && rv->targets->count > 0);
    bool has_target_sel = has_targets && rv->target_selected >= 0 && rv->target_selected < rv->targets->count;

    const char *target_label = lex(LEX_REC_NO_OP);
    if (rv->sweep_done) {
        if (rv->sweep_err != DISC_OK)
            target_label = lex(LEX_REC_ERR_INVENTORY);
        else if (!has_targets)
            target_label = lex(LEX_REC_NO_TARGETS);
        else if (has_target_sel)
            target_label = rv->targets->items[rv->target_selected].name;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted("TARGET");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (rv->sweeping) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(avail * 0.28f);
    if (ImGui::BeginCombo("##target_combo", target_label)) {
        if (!rv->sweep_done) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted(lex(LEX_REC_NO_OP));
            ImGui::PopStyleColor();
        } else if (rv->sweep_err != DISC_OK) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
            ImGui::Text("%s %s", lex(LEX_VOICE_PREFIX), lex(LEX_REC_ERR_INVENTORY));
            ImGui::PopStyleColor();
        } else if (!has_targets) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted(lex(LEX_REC_NO_TARGETS));
            ImGui::PopStyleColor();
        } else {
            for (int i = 0; i < rv->targets->count; i++) {
                const Target *t = &rv->targets->items[i];
                bool sel = (rv->target_selected == i);
                char buf[320];
                if (t->is_simulator)
                    snprintf(buf, sizeof(buf), "%s%s [SIM]", t->booted ? "\xe2\x97\x8f " : "\xe2\x97\x8b ", t->name);
                else
                    snprintf(buf, sizeof(buf), "\xe2\x97\x8f %s [DEV]", t->name);
                ImGui::PushStyleColor(ImGuiCol_Text, sel ? C_CYAN : C_TEXT);
                if (ImGui::Selectable(buf, sel)) ri->pick_target = i;
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", t->udid);
            }
        }
        ImGui::EndCombo();
    }
    if (rv->sweeping) ImGui::EndDisabled();
    ImGui::SameLine();
    if (rv->sweeping) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_BUSY);
        ImGui::TextUnformatted("SWEEPING\xe2\x80\xa6");
        ImGui::PopStyleColor();
    } else {
        if (ImGui::Button(lex(LEX_REC_SWEEP))) ri->sweep = true;
    }

    ImGui::End();
}

/* ── run controls (config band — right region) ───────────────────────── */
static void draw_run_controls(const UiRunView *rv, UiRunIntents *ri, ImVec2 pos, ImVec2 sz) {
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN_DIM);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##run_controls", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    bool in_chain = (rv->phase == RUN_BUILDING || rv->phase == RUN_PRIMING || rv->phase == RUN_INSTALLING ||
                     rv->phase == RUN_LAUNCHING);
    bool running = (rv->phase == RUN_RUNNING);
    bool failed = (rv->phase == RUN_BUILD_FAILED || rv->phase == RUN_DEPLOY_FAILED);
    bool aborted = (rv->phase == RUN_ABORTED);

    ImVec4 phase_col = C_CYAN_DIM;
    if (running)
        phase_col = C_OK;
    else if (failed)
        phase_col = C_FAIL;
    else if (aborted || in_chain)
        phase_col = C_BUSY;

    ImGui::PushStyleColor(ImGuiCol_Text, phase_col);
    ImGui::TextUnformatted(lex(runstate_phase_lex(rv->phase)));
    ImGui::PopStyleColor();

    /* BUILD ▷ INSTALL ▷ LAUNCH progression */
    {
        struct {
            const char *label;
            RunPhase phase;
        } steps[] = {
            {"BUILD", RUN_BUILDING},
            {"INSTALL", RUN_INSTALLING},
            {"LAUNCH", RUN_LAUNCHING},
        };

        int n = 3;
        for (int i = 0; i < n; i++) {
            bool active = (rv->phase == steps[i].phase);
            bool done = false;
            if (steps[i].phase == RUN_BUILDING &&
                (rv->phase == RUN_INSTALLING || rv->phase == RUN_LAUNCHING || running))
                done = true;
            if (steps[i].phase == RUN_INSTALLING && (rv->phase == RUN_LAUNCHING || running)) done = true;
            if (steps[i].phase == RUN_LAUNCHING && running) done = true;

            ImVec4 col = done ? C_OK : (active ? C_BUSY : C_CYAN_DIM);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(steps[i].label);
            ImGui::PopStyleColor();
            if (i < n - 1) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
                ImGui::TextUnformatted(" \xe2\x96\xb7 "); /* ▷ */
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool can_execute = (rv->readiness == READY_OK) && !in_chain;
    bool can_compile = (rv->readiness == READY_OK || rv->readiness == READY_NO_TARGET) && !in_chain;

    if (in_chain) {
        if (ImGui::Button(lex(LEX_RUN_ABORT))) ri->abort_run = true;
    } else {
        if (!can_execute) ImGui::BeginDisabled();
        if (ImGui::Button(lex(LEX_RUN_EXECUTE))) ri->execute = true;
        if (!can_execute) ImGui::EndDisabled();
    }

    ImGui::SameLine();

    if (!can_compile) ImGui::BeginDisabled();
    if (ImGui::Button(lex(LEX_RUN_COMPILE))) ri->compile = true;
    if (!can_compile) ImGui::EndDisabled();

    if (rv->stale) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, C_BUSY);
        ImGui::TextUnformatted(lex(LEX_RUN_STALE));
        ImGui::PopStyleColor();
    }

    /* READY indicator */
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    switch (rv->readiness) {
        case READY_OK:
            ImGui::PushStyleColor(ImGuiCol_Text, C_OK);
            ImGui::TextUnformatted(lex(LEX_REC_READY));
            ImGui::PopStyleColor();
            break;
        case READY_NO_PROJECT:
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted("// NO PROJECT");
            ImGui::PopStyleColor();
            break;
        case READY_NO_SCHEME:
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::Text("// NO %s", lex(LEX_REC_FIELD_SCHEME));
            ImGui::PopStyleColor();
            break;
        case READY_NO_CONFIG:
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::Text("// NO %s", lex(LEX_REC_FIELD_CONFIG));
            ImGui::PopStyleColor();
            break;
        case READY_NO_BUNDLE_ID:
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::Text("// NO %s", lex(LEX_REC_FIELD_BUNDLE_ID));
            ImGui::PopStyleColor();
            break;
        case READY_NO_TARGET:
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted("// NO TARGET LOCKED");
            ImGui::PopStyleColor();
            break;
    }

    ImGui::End();
}

/* ── build log panel (bottom-left) ──────────────────────────────────── */
static void draw_build_log(const UiRunView *rv, UiRunIntents *ri, ImVec2 pos, ImVec2 sz) {
    bool failed = (rv->phase == RUN_BUILD_FAILED || rv->phase == RUN_DEPLOY_FAILED);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN_DIM);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##build_log_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted("BUILD LOG");
    ImGui::PopStyleColor();

    {
        const float btn_pad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float spc = ImGui::GetStyle().ItemSpacing.x;
        float copy_w = ImGui::CalcTextSize("COPY").x + btn_pad;
        float clr_w = ImGui::CalcTextSize("CLR").x + btn_pad;
        float btns_w = copy_w + spc + clr_w;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btns_w);

        ImGui::PushStyleColor(ImGuiCol_Button, C_CYAN_DIM);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_Text, C_BG);
        if (ImGui::SmallButton("COPY")) {
            ri->build_log_copy = true;
            if (rv->build_log) {
                size_t needed = logbuf_copy_all(rv->build_log, nullptr, 0);
                if (needed > 0) {
                    char *tmp = new char[needed + 1];
                    logbuf_copy_all(rv->build_log, tmp, needed + 1);
                    tmp[needed] = '\0';
                    ImGui::SetClipboardText(tmp);
                    delete[] tmp;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("CLR")) ri->build_log_clear = true;
        ImGui::PopStyleColor(4);
    }

    ImGui::BeginChild("##build_log_content", {0.0f, ImGui::GetContentRegionAvail().y}, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    static bool s_build_auto_scroll = true;

    int count = rv->build_log ? logbuf_count(rv->build_log) : 0;
    if (count == 0) {
        if (failed) {
            /* Pre-output failure: no chunks arrived yet but the chain
               already resolved to a terminal state.  Show the failure
               label instead of the wordmark so the operator can see the
               result rather than the (misleading) idle empty state. */
            const char *label = lex(runstate_phase_lex(rv->phase));
            const ImVec2 lbl_sz = ImGui::CalcTextSize(label);
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float x_lbl = center_offset(avail.x, lbl_sz.x);
            const float y_lbl = center_offset(avail.y, lbl_sz.y);
            ImGui::SetCursorPos({x_lbl, y_lbl});
            ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
        } else {
            const char *wm = lex(LEX_WORDMARK);
            const char *cap = lex(LEX_RUN_BUILD_EMPTY);
            const ImVec2 wm_sz = ImGui::CalcTextSize(wm);
            const ImVec2 cap_sz = ImGui::CalcTextSize(cap);
            const float gap = ImGui::GetTextLineHeightWithSpacing() * 0.5f;
            const float blk_h = wm_sz.y + gap + cap_sz.y;
            const ImVec2 avail = ImGui::GetContentRegionAvail();
            const float x_wm = center_offset(avail.x, wm_sz.x);
            const float x_cap = center_offset(avail.x, cap_sz.x);
            const float y0 = center_offset(avail.y, blk_h);
            ImGui::SetCursorPos({x_wm, y0});
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
            ImGui::TextUnformatted(wm);
            ImGui::PopStyleColor();
            ImGui::SetCursorPos({x_cap, y0 + wm_sz.y + gap});
            ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
            ImGui::TextUnformatted(cap);
            ImGui::PopStyleColor();
        }
    } else {
        if (failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
            ImGui::TextUnformatted(lex(runstate_phase_lex(rv->phase)));
            ImGui::PopStyleColor();
        }
        for (int i = 0; i < count; i++) {
            size_t len = 0;
            const char *ln = logbuf_line(rv->build_log, i, &len);
            ImGui::TextUnformatted(ln, ln + len);
        }
    }

    float scroll_y = ImGui::GetScrollY();
    float scroll_max = ImGui::GetScrollMaxY();
    if (scroll_max > 0.0f && scroll_y < scroll_max - 4.0f)
        s_build_auto_scroll = false;
    else
        s_build_auto_scroll = true;
    if (s_build_auto_scroll) ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

/* ── live feed panel (bottom-right) ─────────────────────────────────── */
static void draw_live_feed(const UiRunView *rv, UiRunIntents *ri, ImVec2 pos, ImVec2 sz) {
    bool running = (rv->phase == RUN_RUNNING);

    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN_DIM);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##live_feed_panel", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, running ? C_OK : C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_RUN_LIVE_FEED));
    ImGui::PopStyleColor();

    {
        const float btn_pad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float spc = ImGui::GetStyle().ItemSpacing.x;
        float copy_w = ImGui::CalcTextSize("COPY").x + btn_pad;
        float clr_w = ImGui::CalcTextSize("CLR").x + btn_pad;
        float btns_w = copy_w + spc + clr_w;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btns_w);

        ImGui::PushStyleColor(ImGuiCol_Button, C_CYAN_DIM);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_Text, C_BG);
        if (ImGui::SmallButton("COPY##dev")) {
            ri->device_log_copy = true;
            if (rv->device_log) {
                size_t needed = logbuf_copy_all(rv->device_log, nullptr, 0);
                if (needed > 0) {
                    char *tmp = new char[needed + 1];
                    logbuf_copy_all(rv->device_log, tmp, needed + 1);
                    tmp[needed] = '\0';
                    ImGui::SetClipboardText(tmp);
                    delete[] tmp;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("CLR##dev")) ri->device_log_clear = true;
        ImGui::PopStyleColor(4);
    }

    ImGui::BeginChild("##live_feed_content", {0.0f, ImGui::GetContentRegionAvail().y}, false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    static bool s_device_auto_scroll = true;

    int dev_count = rv->device_log ? logbuf_count(rv->device_log) : 0;
    if (dev_count == 0) {
        const char *wm = lex(LEX_WORDMARK);
        const char *cap = lex(LEX_RUN_DEVICE_EMPTY);
        const ImVec2 wm_sz = ImGui::CalcTextSize(wm);
        const ImVec2 cap_sz = ImGui::CalcTextSize(cap);
        const float gap = ImGui::GetTextLineHeightWithSpacing() * 0.5f;
        const float blk_h = wm_sz.y + gap + cap_sz.y;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float x_wm = center_offset(avail.x, wm_sz.x);
        const float x_cap = center_offset(avail.x, cap_sz.x);
        const float y0 = center_offset(avail.y, blk_h);
        ImGui::SetCursorPos({x_wm, y0});
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
        ImGui::TextUnformatted(wm);
        ImGui::PopStyleColor();
        ImGui::SetCursorPos({x_cap, y0 + wm_sz.y + gap});
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(cap);
        ImGui::PopStyleColor();
    } else {
        for (int i = 0; i < dev_count; i++) {
            size_t len = 0;
            const char *ln = logbuf_line(rv->device_log, i, &len);
            ImGui::TextUnformatted(ln, ln + len);
        }
    }

    float dev_scroll_y = ImGui::GetScrollY();
    float dev_scroll_max = ImGui::GetScrollMaxY();
    if (dev_scroll_max > 0.0f && dev_scroll_y < dev_scroll_max - 4.0f)
        s_device_auto_scroll = false;
    else
        s_device_auto_scroll = true;
    if (s_device_auto_scroll) ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::End();
}

/* ── application-wide keyboard handler ───────────────────────────────── */
static void handle_global_keys(const UiConnView *cv, const UiRunView *rrv, UiIntents *ci, UiRunIntents *rri) {
    /* Modal owns the keyboard: suppress all global chords while open. */
    if (ImGui::IsPopupOpen("##kc_vault")) return;

    bool bar_phase = (cv->phase == CONN_ONLINE || cv->phase == CONN_REACQUIRING);

    /* Ctrl+Enter → EXECUTE (both main Enter and keypad Enter). */
    if (rri && bar_phase) {
        bool in_chain = (rrv->phase == RUN_BUILDING || rrv->phase == RUN_PRIMING || rrv->phase == RUN_INSTALLING ||
                         rrv->phase == RUN_LAUNCHING);
        bool can_execute = (rrv->readiness == READY_OK) && !in_chain;
        if (can_execute) {
            if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter))
                rri->execute = true;
        }
    }

    /* Ctrl+Backspace → clear Device Log. */
    if (rri && bar_phase) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backspace)) rri->device_log_clear = true;
    }

    /* Ctrl+Escape → CLOSE connection (ONLINE / REACQUIRING only). */
    if (bar_phase) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Escape)) ci->close = true;
    }
}

/* ── keychain passkey modal ──────────────────────────────────────────── */
static void draw_kc_modal(const UiRunView *rv, KcForm *kf, UiRunIntents *ri) {
    if (rv->show_kc_prompt) ImGui::OpenPopup("##kc_vault");

    const ImGuiIO &io = ImGui::GetIO();
    const float cx = io.DisplaySize.x * 0.5f;
    const float cy = io.DisplaySize.y * 0.5f;
    ImGui::SetNextWindowPos({cx, cy}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({420.0f, 0.0f}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    bool open = ImGui::BeginPopupModal("##kc_vault", nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!open) return;

    /* Title */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
    ImGui::TextUnformatted(lex(LEX_KC_MODAL_TITLE));
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    /* KEYCHAIN PASSKEY field */
    const float lw = 140.0f;
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_KC_FIELD_PASSKEY));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool kc_passkey_enter = ImGui::InputText("##kc_passkey", kf->passkey, sizeof(kf->passkey),
                                             ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);

    /* REMEMBER KEYCHAIN checkbox */
    ImGui::Checkbox("##kc_remember", &kf->remember);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_KC_CHECKBOX_REMEMBER));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    /* ENTER button */
    if (ImGui::Button(lex(LEX_KC_BUTTON_ENTER))) {
        ri->kc_submit = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    /* SKIP button */
    if (ImGui::Button(lex(LEX_KC_BUTTON_SKIP))) {
        ri->kc_skip = true;
        ImGui::CloseCurrentPopup();
    }

    /* keyboard: Enter → submit, Escape → skip */
    bool kb_enter =
        kc_passkey_enter || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    if (kb_enter) {
        ri->kc_submit = true;
        ImGui::CloseCurrentPopup();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ri->kc_skip = true;
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

static void draw_conn_bar(const UiConnView *view, double online_since, UiIntents *out) {
    const ImGuiIO &io = ImGui::GetIO();
    const float avail_w = io.DisplaySize.x;
    const float pad = ImGui::GetStyle().FramePadding.y;
    const float v_pad = pad * 2.0f;
    const float bar_h = ImGui::GetTextLineHeight() + v_pad * 2.0f;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({avail_w, bar_h});
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, C_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{pad * 2.0f, v_pad});
    ImGui::Begin("##conn_bar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    /* user@host */
    if (view->user_host && view->user_host[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
        ImGui::TextUnformatted(view->user_host);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    if (view->phase == CONN_REACQUIRING) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_BUSY);
        ImGui::TextUnformatted(lex(LEX_CONN_REACQUIRING));
        ImGui::PopStyleColor();
    } else {
        /* CONN_ONLINE: grant stamp fades after 2.5 s, then * ONLINE pulse */
        double now = glfwGetTime();
        double elapsed = (online_since > 0.0) ? now - online_since : 9999.0;

        if (elapsed < 2.5) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_OK);
            ImGui::TextUnformatted(lex(LEX_CONN_ACCESS_GRANTED));
            ImGui::PopStyleColor();
        } else {
            float pulse = 0.7f + 0.3f * (float)sin(now * 1.8);
            ImVec4 c_pulse = {C_OK.x, C_OK.y, C_OK.z, pulse};
            ImGui::PushStyleColor(ImGuiCol_Text, c_pulse);
            ImGui::TextUnformatted(lex(LEX_CONN_ONLINE));
            ImGui::PopStyleColor();
        }
    }

    /* UPDATE and CLOSE buttons — right-aligned */
    {
        const float btn_pad = ImGui::GetStyle().FramePadding.x * 2.0f;
        const float spc = ImGui::GetStyle().ItemSpacing.x;
        float update_w = ImGui::CalcTextSize(lex(LEX_CONN_UPDATE)).x + btn_pad;
        float close_w = ImGui::CalcTextSize(lex(LEX_CONN_CLOSE)).x + btn_pad;
        float btns_w = update_w + spc + close_w;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - btns_w);
        ImGui::PushStyleColor(ImGuiCol_Button, C_CYAN_DIM);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_CYAN);
        ImGui::PushStyleColor(ImGuiCol_Text, C_BG);
        if (ImGui::SmallButton(lex(LEX_CONN_UPDATE))) out->update = true;
        ImGui::SameLine();
        if (ImGui::SmallButton(lex(LEX_CONN_CLOSE))) out->close = true;
        ImGui::PopStyleColor(4);
    }

    ImGui::End();
}

static void teardown(GLFWwindow *window) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    if (window) glfwDestroyWindow(window);
    glfwTerminate();
}

UiStatus ui_init(Arena *a, UiOptions opts, Ui **out) {
    *out = nullptr;

    if (!has_display()) return UI_ERR_NO_DISPLAY;

    if (!glfwInit()) return UI_ERR_GLFW;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    if (opts.headless) glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *window =
        glfwCreateWindow(opts.width, opts.height, opts.title ? opts.title : "ostrich", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return UI_ERR_GLFW;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return UI_ERR_GLFW;
    }

    if (!ImGui_ImplOpenGL3_Init(GLSL_VERSION)) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
        return UI_ERR_GL;
    }

    /* HiDPI: scale font size and style by the content scale. */
    float xscale = 1.0f, yscale = 1.0f;
    glfwGetWindowContentScale(window, &xscale, &yscale);
    float scale = (xscale > 0.0f) ? xscale : 1.0f;
    float font_size = 14.0f * scale;

    /* Load fonts into the caller-supplied arena. */
    char path_reg[512], path_bold[512];
    snprintf(path_reg, sizeof(path_reg), "%s/JetBrainsMono-Regular.ttf", opts.font_dir);
    snprintf(path_bold, sizeof(path_bold), "%s/JetBrainsMono-Bold.ttf", opts.font_dir);

    long reg_size = 0, bold_size = 0;
    ReadErr rerr;

    void *reg_data = read_file_arena(a, path_reg, &reg_size, rerr);
    if (!reg_data) {
        teardown(window);
        return (rerr == ReadErr::Oom) ? UI_ERR_OOM : UI_ERR_FONT;
    }

    void *bold_data = read_file_arena(a, path_bold, &bold_size, rerr);
    if (!bold_data) {
        teardown(window);
        return (rerr == ReadErr::Oom) ? UI_ERR_OOM : UI_ERR_FONT;
    }

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(reg_data, (int)reg_size, font_size, &cfg);
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(bold_data, (int)bold_size, font_size, &cfg);

    apply_theme();
    ImGui::GetStyle().ScaleAllSizes(scale);

    Ui *ui = static_cast<Ui *>(arena_alloc(a, sizeof(Ui), alignof(Ui)));
    if (!ui) {
        teardown(window);
        return UI_ERR_OOM;
    }
    frame_stats_init(&ui->fs);
    ui->last_time = glfwGetTime();
    ui->online_since = 0.0;
    ui->prev_phase = CONN_DISCONNECTED;
    ui->log_split = 0.5f;
    ui->window = window;
    *out = ui;
    return UI_OK;
}

bool ui_frame(Ui *ui, const UiConnView *cv, ConnForm *cf, UiIntents *ci, const UiReconView *rv, RunConfig *rf,
              UiReconIntents *ri, const UiRunView *rrv, UiRunIntents *rri, KcForm *kf) {
    *ci = {};
    ci->select_host = -1;
    if (ri) {
        *ri = {};
        ri->pick_blueprint = -1;
        ri->pick_preset = -1;
        ri->pick_target = -1;
    }
    if (rri) *rri = {};
    /* Aliases for local use — existing code uses view/form/out. */
    const UiConnView *view = cv;
    ConnForm *form = cf;
    UiIntents *out = ci;
    double now = glfwGetTime();
    double dt = now - ui->last_time;
    ui->last_time = now;
    int fps = frame_stats_update(&ui->fs, dt);

    glfwPollEvents();

    if (glfwWindowShouldClose(ui->window)) return false;

    if (glfwGetKey(ui->window, GLFW_KEY_Q) == GLFW_PRESS &&
        (glfwGetKey(ui->window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(ui->window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS))
        return false;

    /* Track ONLINE transition for the grant stamp timer. */
    if (view->phase == CONN_ONLINE && ui->prev_phase != CONN_ONLINE) ui->online_since = glfwGetTime();
    if (view->phase != CONN_ONLINE) ui->online_since = 0.0;
    ui->prev_phase = view->phase;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    handle_global_keys(view, rrv, out, rri);

    /* ── Four-band geometry (computed once per frame) ──────────────────── */
    const ImGuiIO &io = ImGui::GetIO();
    const float avail_w = io.DisplaySize.x;
    const float avail_h = io.DisplaySize.y;
    const float g_pad = ImGui::GetStyle().FramePadding.y;
    const float g_hdr_h = ImGui::GetTextLineHeight() + g_pad * 4.0f;
    const float g_ftr_h = ImGui::GetTextLineHeight() + g_pad * 2.0f;
    const float g_cfg_h = 140.0f;
    const float g_log_y = g_hdr_h + g_cfg_h;
    const float g_log_h_raw = avail_h - g_log_y - g_ftr_h;
    const float g_log_h = g_log_h_raw > 0.0f ? g_log_h_raw : 0.0f;
    const float g_rcn_w = avail_w * 0.60f;
    const float g_ctl_w = avail_w - g_rcn_w;

    /* Clamp log_split so neither panel narrows below 96 px after window resize. */
    {
        const float safe_w = avail_w > 0.0f ? avail_w : 1.0f;
        const float min_r = 96.0f / safe_w;
        const float max_r = 1.0f - min_r;
        if (ui->log_split < min_r) ui->log_split = min_r;
        if (ui->log_split > max_r) ui->log_split = max_r;
    }
    const float g_build_w = avail_w * ui->log_split;
    const float g_live_x = g_build_w;
    const float g_live_w = avail_w - g_build_w;

    draw_overlay();

    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

    /* ── resting view: wordmark + identity ──────────────────────────── */
    {
        const char *wordmark = lex(LEX_WORDMARK);
        const char *identity = lex(LEX_IDENTITY);
        const ImVec2 wm_sz = ImGui::CalcTextSize(wordmark);
        const ImVec2 id_sz = ImGui::CalcTextSize(identity);
        const float gap = ImGui::GetTextLineHeightWithSpacing();
        const float block_h = wm_sz.y + gap + id_sz.y;

        const float off_y = center_offset(avail_h, block_h);
        const float wm_off_x = center_offset(avail_w, wm_sz.x);
        const float id_off_x = center_offset(avail_w, id_sz.x);

        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({avail_w, avail_h});
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::Begin("##resting_view", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();

        ImGui::SetCursorPos({wm_off_x, off_y});
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
        ImGui::TextUnformatted(wordmark);
        ImGui::PopStyleColor();

        ImGui::SetCursorPos({id_off_x, off_y + wm_sz.y + gap});
        ImGui::TextUnformatted(identity);

        ImGui::End();
    }

    /* ── connection bar (ONLINE / REACQUIRING) ──────────────────────── */
    bool bar_phase = (view->phase == CONN_ONLINE || view->phase == CONN_REACQUIRING);
    if (bar_phase) draw_conn_bar(view, ui->online_since, out);

    /* ── config band: recon (left) + run controls (right) ───────────── */
    if (bar_phase && !view->overlay_open && rv != nullptr && rf != nullptr && ri != nullptr)
        draw_recon_panel(rv, rf, ri, {0.0f, g_hdr_h}, {g_rcn_w, g_cfg_h});
    if (bar_phase && !view->overlay_open && rrv != nullptr && rri != nullptr)
        draw_run_controls(rrv, rri, {g_rcn_w, g_hdr_h}, {g_ctl_w, g_cfg_h});

    /* ── log panels: Build Log (left) + Live Feed (right) ───────────── */
    if (bar_phase && !view->overlay_open && rrv != nullptr && rri != nullptr) {
        draw_build_log(rrv, rri, {0.0f, g_log_y}, {g_build_w, g_log_h});
        draw_live_feed(rrv, rri, {g_live_x, g_log_y}, {g_live_w, g_log_h});

        /* Draggable splitter between the two log panels. */
        if (g_log_h > 0.0f && avail_w > 0.0f) {
            const float spl_w = 6.0f;
            const float spl_x = g_build_w - spl_w * 0.5f;
            ImGui::SetNextWindowPos({spl_x, g_log_y});
            ImGui::SetNextWindowSize({spl_w, g_log_h});
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
            ImGui::Begin("##log_splitter", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
            ImGui::PopStyleVar();

            float btn_h = ImGui::GetContentRegionAvail().y;
            if (btn_h <= 0.0f) btn_h = 1.0f;
            ImGui::InvisibleButton("##split_drag", {spl_w, btn_h});
            if (ImGui::IsItemActive()) {
                float delta = ImGui::GetIO().MouseDelta.x;
                ui->log_split += delta / avail_w;
                const float min_r = 96.0f / avail_w;
                const float max_r = 1.0f - min_r;
                if (ui->log_split < min_r) ui->log_split = min_r;
                if (ui->log_split > max_r) ui->log_split = max_r;
            }
            if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            ImGui::End();
        }
    }

    /* ── keychain passkey modal ─────────────────────────────────────────── */
    if (bar_phase && !view->overlay_open && rrv != nullptr && rri != nullptr && kf != nullptr)
        draw_kc_modal(rrv, kf, rri);

    /* ── BREACH overlay (DISCONNECTED / CONNECTING / AWAITING_HOSTKEY /
          SEVERED — SEVERED shows reason + BREACH to re-connect;
          overlay_open shows overlay over the bar for UPDATE) ──────── */
    bool overlay_phase = (view->phase == CONN_DISCONNECTED || view->phase == CONN_CONNECTING ||
                          view->phase == CONN_AWAITING_HOSTKEY || view->phase == CONN_SEVERED || view->overlay_open);
    if (overlay_phase) draw_breach_overlay(view, form, out);

    /* ── diagnostics footer ──────────────────────────────────────────── */
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s // %d FPS // %s", lex(LEX_FOOTER_NAME), fps, lex(LEX_FOOTER_ONLINE));

        ImGui::SetNextWindowPos({0.0f, avail_h - g_ftr_h});
        ImGui::SetNextWindowSize({avail_w, g_ftr_h});
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, C_BG);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{g_pad * 2.0f, g_pad});
        ImGui::Begin("##footer", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();

        ImGui::End();
    }

    ImGui::Render();

    int fb_w, fb_h;
    glfwGetFramebufferSize(ui->window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(C_BG.x, C_BG.y, C_BG.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(ui->window);

    return true;
}

void ui_shutdown(Ui *ui) {
    teardown(ui->window);
}

const char *ui_status_str(UiStatus st) {
    switch (st) {
        case UI_OK:
            return "ok";
        case UI_ERR_NO_DISPLAY:
            return "no display server reachable";
        case UI_ERR_GLFW:
            return "GLFW init or window or GL context failed";
        case UI_ERR_GL:
            return "OpenGL init failed";
        case UI_ERR_FONT:
            return "vendored font file missing or unreadable";
        case UI_ERR_OOM:
            return "arena exhausted during init";
        default:
            return "unknown error";
    }
}
