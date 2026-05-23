/* C++ internals for the ui module — hidden behind include/ui.h. */

#include "ui.h"
#include "arena.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

static const char *GLSL_VERSION = "#version 150";

struct Ui {
    GLFWwindow *window;
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

    ImGui::GetStyle().ScaleAllSizes(scale);

    Ui *ui = static_cast<Ui *>(arena_alloc(a, sizeof(Ui), alignof(Ui)));
    if (!ui) {
        teardown(window);
        return UI_ERR_OOM;
    }
    ui->window = window;
    *out = ui;
    return UI_OK;
}

bool ui_frame(Ui *ui) {
    glfwPollEvents();

    if (glfwWindowShouldClose(ui->window))
        return false;

    if (glfwGetKey(ui->window, GLFW_KEY_Q) == GLFW_PRESS &&
        (glfwGetKey(ui->window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
         glfwGetKey(ui->window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS))
        return false;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::DockSpaceOverViewport(0, nullptr,
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    ImGui::Render();

    int fb_w, fb_h;
    glfwGetFramebufferSize(ui->window, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);
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
