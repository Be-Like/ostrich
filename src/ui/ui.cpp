/* C++ internals for the ui module — hidden behind include/ui.h. */

#include "ui.h"
#include "arena.h"
#include "framestats.h"
#include "lexicon.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

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
static const ImVec4 C_BG          = {0.027f, 0.035f, 0.051f, 1.0f}; /* #07090d */
static const ImVec4 C_PANEL       = {0.051f, 0.067f, 0.090f, 1.0f}; /* #0d1117 */
static const ImVec4 C_CYAN_DIM    = {0.055f, 0.353f, 0.388f, 1.0f}; /* #0e5a63 */
static const ImVec4 C_MAGENTA_DIM = {0.431f, 0.078f, 0.341f, 1.0f}; /* #6e1457 */
static const ImVec4 C_CYAN        = {0.000f, 0.941f, 1.000f, 1.0f}; /* #00f0ff */
static const ImVec4 C_MAGENTA     = {1.000f, 0.169f, 0.839f, 1.0f}; /* #ff2bd6 */
/* semantic — defined, reserved for meaning */
static const ImVec4 C_OK   = {0.098f, 1.000f, 0.478f, 1.0f}; /* #19ff7a */
static const ImVec4 C_FAIL = {1.000f, 0.231f, 0.314f, 1.0f}; /* #ff3b50 */
static const ImVec4 C_BUSY = {1.000f, 0.690f, 0.000f, 1.0f}; /* #ffb000 */
static const ImVec4 C_TEXT = {0.784f, 0.816f, 0.847f, 1.0f}; /* #c8d0d8 */
static const ImVec4 C_NONE = {0.000f, 0.000f, 0.000f, 0.0f}; /* transparent */

static void apply_theme(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImVec4     *c = s.Colors;

    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 0.0f;
    s.FrameRounding     = 0.0f;
    s.PopupRounding     = 0.0f;
    s.ScrollbarRounding = 2.0f;
    s.GrabRounding      = 2.0f;
    s.TabRounding       = 0.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;

    c[ImGuiCol_Text]                      = C_TEXT;
    c[ImGuiCol_TextDisabled]              = {C_TEXT.x * 0.4f, C_TEXT.y * 0.4f, C_TEXT.z * 0.4f, 1.0f};
    c[ImGuiCol_WindowBg]                  = C_PANEL;
    c[ImGuiCol_ChildBg]                   = C_BG;
    c[ImGuiCol_PopupBg]                   = C_PANEL;
    c[ImGuiCol_Border]                    = C_CYAN_DIM;
    c[ImGuiCol_BorderShadow]              = C_NONE;
    c[ImGuiCol_FrameBg]                   = C_BG;
    c[ImGuiCol_FrameBgHovered]            = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.4f};
    c[ImGuiCol_FrameBgActive]             = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.7f};
    c[ImGuiCol_TitleBg]                   = C_BG;
    c[ImGuiCol_TitleBgActive]             = C_PANEL;
    c[ImGuiCol_TitleBgCollapsed]          = C_BG;
    c[ImGuiCol_MenuBarBg]                 = C_BG;
    c[ImGuiCol_ScrollbarBg]               = C_BG;
    c[ImGuiCol_ScrollbarGrab]             = C_CYAN_DIM;
    c[ImGuiCol_ScrollbarGrabHovered]      = C_CYAN;
    c[ImGuiCol_ScrollbarGrabActive]       = C_CYAN;
    c[ImGuiCol_CheckMark]                 = C_CYAN;
    c[ImGuiCol_CheckboxSelectedBg]        = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.3f};
    c[ImGuiCol_SliderGrab]                = C_CYAN_DIM;
    c[ImGuiCol_SliderGrabActive]          = C_CYAN;
    c[ImGuiCol_Button]                    = C_MAGENTA_DIM;
    c[ImGuiCol_ButtonHovered]             = C_MAGENTA;
    c[ImGuiCol_ButtonActive]              = C_MAGENTA;
    c[ImGuiCol_Header]                    = C_PANEL;
    c[ImGuiCol_HeaderHovered]             = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.5f};
    c[ImGuiCol_HeaderActive]              = C_CYAN_DIM;
    c[ImGuiCol_Separator]                 = C_CYAN_DIM;
    c[ImGuiCol_SeparatorHovered]          = C_CYAN;
    c[ImGuiCol_SeparatorActive]           = C_CYAN;
    c[ImGuiCol_ResizeGrip]                = {C_MAGENTA_DIM.x, C_MAGENTA_DIM.y, C_MAGENTA_DIM.z, 0.4f};
    c[ImGuiCol_ResizeGripHovered]         = C_MAGENTA;
    c[ImGuiCol_ResizeGripActive]          = C_MAGENTA;
    c[ImGuiCol_InputTextCursor]           = C_CYAN;
    c[ImGuiCol_Tab]                       = C_BG;
    c[ImGuiCol_TabHovered]                = C_CYAN_DIM;
    c[ImGuiCol_TabSelected]               = C_PANEL;
    c[ImGuiCol_TabSelectedOverline]       = C_CYAN;
    c[ImGuiCol_TabDimmed]                 = C_BG;
    c[ImGuiCol_TabDimmedSelected]         = {C_PANEL.x, C_PANEL.y, C_PANEL.z, 0.7f};
    c[ImGuiCol_TabDimmedSelectedOverline] = C_NONE;
    c[ImGuiCol_DockingPreview]            = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.3f};
    c[ImGuiCol_DockingEmptyBg]            = C_BG;
    c[ImGuiCol_PlotLines]                 = C_CYAN;
    c[ImGuiCol_PlotLinesHovered]          = C_CYAN;
    c[ImGuiCol_PlotHistogram]             = C_CYAN_DIM;
    c[ImGuiCol_PlotHistogramHovered]      = C_CYAN;
    c[ImGuiCol_TableHeaderBg]             = C_PANEL;
    c[ImGuiCol_TableBorderStrong]         = C_CYAN_DIM;
    c[ImGuiCol_TableBorderLight]          = {C_CYAN_DIM.x, C_CYAN_DIM.y, C_CYAN_DIM.z, 0.3f};
    c[ImGuiCol_TableRowBg]                = C_NONE;
    c[ImGuiCol_TableRowBgAlt]             = {C_PANEL.x, C_PANEL.y, C_PANEL.z, 0.5f};
    c[ImGuiCol_TextLink]                  = C_CYAN;
    c[ImGuiCol_TextSelectedBg]            = {C_MAGENTA_DIM.x, C_MAGENTA_DIM.y, C_MAGENTA_DIM.z, 0.5f};
    c[ImGuiCol_TreeLines]                 = C_CYAN_DIM;
    c[ImGuiCol_DragDropTarget]            = C_CYAN;
    c[ImGuiCol_DragDropTargetBg]          = {C_CYAN.x, C_CYAN.y, C_CYAN.z, 0.15f};
    c[ImGuiCol_UnsavedMarker]             = C_CYAN;
    c[ImGuiCol_NavCursor]                 = C_CYAN;
    c[ImGuiCol_NavWindowingHighlight]     = C_CYAN;
    c[ImGuiCol_NavWindowingDimBg]         = {0.0f, 0.0f, 0.0f, 0.6f};
    c[ImGuiCol_ModalWindowDimBg]          = {0.0f, 0.0f, 0.0f, 0.6f};
}

struct Ui {
    GLFWwindow *window;
    FrameStats  fs;
    double      last_time;
    double      online_since; /* glfwGetTime() when phase last became ONLINE; 0 = not online */
    ConnPhase   prev_phase;
};

static bool has_display(void) {
#ifdef __linux__
    return getenv("DISPLAY") != nullptr || getenv("WAYLAND_DISPLAY") != nullptr;
#else
    return true;
#endif
}

enum class ReadErr { Ok, NotFound, Oom };

static void *read_file_arena(Arena *a, const char *path, long *out_size,
                              ReadErr &err) {
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
    ImDrawList   *dl = ImGui::GetBackgroundDrawList();
    const ImGuiIO &io = ImGui::GetIO();
    const float   w = io.DisplaySize.x;
    const float   h = io.DisplaySize.y;

    /* scanlines: faint dark stripe on every other row */
    const ImU32 scan = IM_COL32(0, 0, 0, 8);
    for (float y = 0.0f; y < h; y += 2.0f)
        dl->AddRectFilled({0.0f, y}, {w, y + 1.0f}, scan);

    /* vignette: four gradient fills fading inward from each edge */
    const ImU32 edge  = IM_COL32(0, 0, 0, 110);
    const ImU32 clear = IM_COL32(0, 0, 0, 0);
    const float vw    = w * 0.42f;
    const float vh    = h * 0.42f;

    dl->AddRectFilledMultiColor({0.0f, 0.0f}, {vw, h}, edge, clear, clear, edge);    /* left  */
    dl->AddRectFilledMultiColor({w - vw, 0.0f}, {w, h}, clear, edge, edge, clear);   /* right */
    dl->AddRectFilledMultiColor({0.0f, 0.0f}, {w, vh}, edge, edge, clear, clear);    /* top   */
    dl->AddRectFilledMultiColor({0.0f, h - vh}, {w, h}, clear, clear, edge, edge);   /* bottom */
}

/* ── BREACH overlay ─────────────────────────────────────────────────── */
static void draw_breach_overlay(const UiConnView *view, ConnForm *form,
                                UiIntents *out) {
    const ImGuiIO &io        = ImGui::GetIO();
    const float    cx        = io.DisplaySize.x * 0.5f;
    const float    cy        = io.DisplaySize.y * 0.5f;
    bool           connecting = (view->phase == CONN_CONNECTING ||
                                 view->phase == CONN_AWAITING_HOSTKEY);

    ImGui::SetNextWindowPos({cx, cy}, ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({460.0f, 0.0f}, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_Border, C_CYAN);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 12.0f});
    ImGui::Begin("##breach_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    bool first_show    = ImGui::IsWindowAppearing();
    bool passkey_enter = false;

    /* Header */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN);
    ImGui::TextUnformatted(lex(LEX_CONN_UPLINK));
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::Spacing();

    /* Form fields */
    ImGuiInputTextFlags ro = connecting ? ImGuiInputTextFlags_ReadOnly : 0;
    const float         lw = 60.0f; /* label column width */

    /* HOST */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_HOST));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1.0f);
    if (first_show && !connecting) ImGui::SetKeyboardFocusHere();
    bool host_enter = ImGui::InputText("##host", form->host, sizeof(form->host),
                                       ro | ImGuiInputTextFlags_EnterReturnsTrue);

    /* PORT */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_PORT));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(90.0f);
    bool port_enter =
        ImGui::InputText("##port", form->port, sizeof(form->port),
                         ro | ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_CharsDecimal);

    /* USER */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_USER));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    ImGui::SetNextItemWidth(-1.0f);
    bool user_enter = ImGui::InputText("##user", form->user, sizeof(form->user),
                                       ro | ImGuiInputTextFlags_EnterReturnsTrue);

    /* AUTH toggle: SSH-AGENT or PASSKEY */
    ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
    ImGui::TextUnformatted(lex(LEX_CONN_FIELD_AUTH));
    ImGui::PopStyleColor();
    ImGui::SameLine(lw);
    {
        const char *auth_label = (form->auth == SSH_AUTH_PASSWORD)
            ? lex(LEX_CONN_AUTH_PASSKEY)
            : lex(LEX_CONN_AUTH_AGENT);
        if (connecting) {
            ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
            ImGui::TextUnformatted(auth_label);
            ImGui::PopStyleColor();
        } else {
            ImGui::SetNextItemWidth(120.0f);
            if (ImGui::BeginCombo("##auth", auth_label)) {
                if (ImGui::Selectable(lex(LEX_CONN_AUTH_AGENT),
                                      form->auth == SSH_AUTH_AGENT))
                    form->auth = SSH_AUTH_AGENT;
                if (ImGui::Selectable(lex(LEX_CONN_AUTH_PASSKEY),
                                      form->auth == SSH_AUTH_PASSWORD))
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
        passkey_enter = ImGui::InputText(
            "##passkey", form->passkey, sizeof(form->passkey),
            ro | ImGuiInputTextFlags_Password |
                ImGuiInputTextFlags_EnterReturnsTrue);
    }

    /* REMEMBER PASSKEY checkbox — only visible when auth == SSH_AUTH_PASSWORD */
    if (form->auth == SSH_AUTH_PASSWORD) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_CYAN_DIM);
        ImGui::TextUnformatted(lex(LEX_CONN_REMEMBER_PASSKEY));
        ImGui::PopStyleColor();
        ImGui::SameLine(lw);
        if (connecting) ImGui::BeginDisabled();
        ImGui::Checkbox("##remember", &form->remember);
        if (connecting) ImGui::EndDisabled();
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
        int max_vis  = (view->known_count < 4) ? view->known_count : 4;
        float list_h = ImGui::GetTextLineHeightWithSpacing() * (float)max_vis;
        ImGui::BeginChild("##kh", {0.0f, list_h}, false);
        for (int i = 0; i < view->known_count; ++i) {
            const Conn *c   = &view->known_hosts[i];
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
                    (view->fingerprint && view->fingerprint[0])
                        ? view->fingerprint : "");
        ImGui::PopStyleColor();
    } else if (view->show_mismatch) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_FAIL);
        ImGui::TextUnformatted(lex(LEX_CONN_ERR_HOSTKEY_MISMATCH));
        if (view->fingerprint && view->fingerprint[0])
            ImGui::Text("  %s", view->fingerprint);
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
        ImGui::PushStyleColor(ImGuiCol_Button,
                              {C_OK.x * 0.25f, C_OK.y * 0.25f, C_OK.z * 0.25f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              {C_OK.x * 0.45f, C_OK.y * 0.45f, C_OK.z * 0.45f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_OK);
        if (ImGui::Button(lex(LEX_CONN_TRUST)))
            out->trust = true;
        ImGui::PopStyleColor(3);

        ImGui::SameLine();

        ImGui::PushStyleColor(ImGuiCol_Button,
                              {C_FAIL.x * 0.25f, C_FAIL.y * 0.25f, C_FAIL.z * 0.25f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              {C_FAIL.x * 0.45f, C_FAIL.y * 0.45f, C_FAIL.z * 0.45f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, C_FAIL);
        if (ImGui::Button(lex(LEX_CONN_DECLINE)))
            out->decline = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            out->decline = true;
        ImGui::PopStyleColor(3);
    } else if (connecting) {
        if (ImGui::Button(lex(LEX_CONN_ABORT)))
            out->abort = true;
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            out->abort = true;
    } else {
        bool valid = (form->host[0] != '\0' && form->user[0] != '\0' &&
                      (form->auth == SSH_AUTH_AGENT ||
                       form->passkey[0] != '\0'));
        bool enter = host_enter || port_enter || user_enter || passkey_enter;

        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button(lex(LEX_CONN_BREACH)) || (valid && enter))
            out->breach = true;
        if (!valid) ImGui::EndDisabled();

        ImGui::SameLine();

        if (!valid) ImGui::BeginDisabled();
        if (ImGui::Button(lex(LEX_CONN_SAVE)))
            out->save = true;
        if (!valid) ImGui::EndDisabled();
    }

    ImGui::End();
}

/* ── connection bar ─────────────────────────────────────────────────── */
static void draw_conn_bar(const UiConnView *view, double online_since) {
    const ImGuiIO &io     = ImGui::GetIO();
    const float    avail_w = io.DisplaySize.x;
    const float    pad    = ImGui::GetStyle().FramePadding.y;
    const float    bar_h  = ImGui::GetTextLineHeight() + pad * 2.0f;

    ImGui::SetNextWindowPos({0.0f, 0.0f});
    ImGui::SetNextWindowSize({avail_w, bar_h});
    ImGui::SetNextWindowBgAlpha(1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, C_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{pad * 2.0f, pad});
    ImGui::Begin("##conn_bar", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    /* user@host */
    if (view->user_host && view->user_host[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_TEXT);
        ImGui::TextUnformatted(view->user_host);
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }

    /* Grant stamp fades after 2.5 s; then steady * ONLINE with slow pulse */
    double now     = glfwGetTime();
    double elapsed = (online_since > 0.0) ? now - online_since : 9999.0;

    if (elapsed < 2.5) {
        ImGui::PushStyleColor(ImGuiCol_Text, C_OK);
        ImGui::TextUnformatted(lex(LEX_CONN_ACCESS_GRANTED));
        ImGui::PopStyleColor();
    } else {
        /* Pulse the * dot between ~40 % and 100 % brightness */
        float pulse = 0.7f + 0.3f * (float)sin(now * 1.8);
        ImVec4 c_pulse = {C_OK.x, C_OK.y, C_OK.z, pulse};
        ImGui::PushStyleColor(ImGuiCol_Text, c_pulse);
        ImGui::TextUnformatted(lex(LEX_CONN_ONLINE));
        ImGui::PopStyleColor();
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

    if (!has_display())
        return UI_ERR_NO_DISPLAY;

    if (!glfwInit())
        return UI_ERR_GLFW;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    if (opts.headless)
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

    GLFWwindow *window = glfwCreateWindow(
        opts.width, opts.height,
        opts.title ? opts.title : "ostrich",
        nullptr, nullptr);
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
    snprintf(path_reg,  sizeof(path_reg),  "%s/JetBrainsMono-Regular.ttf",
             opts.font_dir);
    snprintf(path_bold, sizeof(path_bold), "%s/JetBrainsMono-Bold.ttf",
             opts.font_dir);

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
    io.Fonts->AddFontFromMemoryTTF(reg_data,  (int)reg_size,  font_size, &cfg);
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
    ui->last_time    = glfwGetTime();
    ui->online_since = 0.0;
    ui->prev_phase   = CONN_DISCONNECTED;
    ui->window       = window;
    *out = ui;
    return UI_OK;
}

bool ui_frame(Ui *ui, const UiConnView *view, ConnForm *form, UiIntents *out) {
    *out = {};
    out->select_host = -1;
    double now = glfwGetTime();
    double dt  = now - ui->last_time;
    ui->last_time = now;
    int fps = frame_stats_update(&ui->fs, dt);

    glfwPollEvents();

    if (glfwWindowShouldClose(ui->window))
        return false;

    if (glfwGetKey(ui->window, GLFW_KEY_Q) == GLFW_PRESS &&
        (glfwGetKey(ui->window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(ui->window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS))
        return false;

    /* Track ONLINE transition for the grant stamp timer. */
    if (view->phase == CONN_ONLINE && ui->prev_phase != CONN_ONLINE)
        ui->online_since = glfwGetTime();
    if (view->phase != CONN_ONLINE)
        ui->online_since = 0.0;
    ui->prev_phase = view->phase;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_overlay();

    ImGui::DockSpaceOverViewport(0, nullptr,
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    /* ── resting view: wordmark + identity ──────────────────────────── */
    {
        const ImGuiIO &io      = ImGui::GetIO();
        const float    avail_w = io.DisplaySize.x;
        const float    avail_h = io.DisplaySize.y;

        const char  *wordmark = lex(LEX_WORDMARK);
        const char  *identity = lex(LEX_IDENTITY);
        const ImVec2 wm_sz    = ImGui::CalcTextSize(wordmark);
        const ImVec2 id_sz    = ImGui::CalcTextSize(identity);
        const float  gap      = ImGui::GetTextLineHeightWithSpacing();
        const float  block_h  = wm_sz.y + gap + id_sz.y;

        const float off_y    = center_offset(avail_h, block_h);
        const float wm_off_x = center_offset(avail_w, wm_sz.x);
        const float id_off_x = center_offset(avail_w, id_sz.x);

        ImGui::SetNextWindowPos({0.0f, 0.0f});
        ImGui::SetNextWindowSize({avail_w, avail_h});
        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
        ImGui::Begin("##resting_view", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
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

    /* ── connection bar (ONLINE / REACQUIRING / SEVERED) ────────────── */
    bool bar_phase = (view->phase == CONN_ONLINE ||
                      view->phase == CONN_REACQUIRING ||
                      view->phase == CONN_SEVERED);
    if (bar_phase)
        draw_conn_bar(view, ui->online_since);

    /* ── BREACH overlay (DISCONNECTED / CONNECTING / AWAITING_HOSTKEY) ─ */
    bool overlay_phase = (view->phase == CONN_DISCONNECTED ||
                          view->phase == CONN_CONNECTING ||
                          view->phase == CONN_AWAITING_HOSTKEY);
    if (overlay_phase)
        draw_breach_overlay(view, form, out);

    /* ── diagnostics footer ──────────────────────────────────────────── */
    {
        const ImGuiIO &io      = ImGui::GetIO();
        const float    avail_w = io.DisplaySize.x;
        const float    avail_h = io.DisplaySize.y;
        const float    pad     = ImGui::GetStyle().FramePadding.y;
        const float    foot_h  = ImGui::GetTextLineHeight() + pad * 2.0f;

        char buf[128];
        snprintf(buf, sizeof(buf), "%s // %d FPS // %s",
                 lex(LEX_FOOTER_NAME), fps, lex(LEX_FOOTER_ONLINE));

        ImGui::SetNextWindowPos({0.0f, avail_h - foot_h});
        ImGui::SetNextWindowSize({avail_w, foot_h});
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, C_BG);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2{pad * 2.0f, pad});
        ImGui::Begin("##footer", nullptr,
                     ImGuiWindowFlags_NoDecoration |
                         ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
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
    case UI_OK:             return "ok";
    case UI_ERR_NO_DISPLAY: return "no display server reachable";
    case UI_ERR_GLFW:       return "GLFW init or window or GL context failed";
    case UI_ERR_GL:         return "OpenGL init failed";
    case UI_ERR_FONT:       return "vendored font file missing or unreadable";
    case UI_ERR_OOM:        return "arena exhausted during init";
    default:                return "unknown error";
    }
}
