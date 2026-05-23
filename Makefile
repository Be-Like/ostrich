CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra

BUILD   := build
SRC     := src
TESTS   := tests
INCLUDE := include

IMGUI_DIR := third_party/imgui
GLFW_DIR  := third_party/glfw

UNAME_S := $(shell uname -s)

# Pick up user-local headers/libs when present (e.g. Xcursor, Xrandr on Arch).
comma := ,
LOCAL_INC    := $(wildcard $(HOME)/.local/include)
LOCAL_LIB    := $(wildcard $(HOME)/.local/lib)
LOCAL_CFLAGS := $(if $(LOCAL_INC),-I$(LOCAL_INC))
LOCAL_LFLAGS := $(if $(LOCAL_LIB),-L$(LOCAL_LIB) -Wl$(comma)--disable-new-dtags$(comma)-rpath$(comma)$(LOCAL_LIB))

ifeq ($(UNAME_S),Linux)
GLFW_DEFS     := -D_GLFW_X11
PLATFORM_LIBS := $(LOCAL_LFLAGS) -lX11 -ldl -lpthread -lm -lGL \
    -lXcursor -lXi -lXinerama -lXrandr -lXfixes
GLFW_PLAT_SRC := \
    posix_time.c posix_thread.c posix_module.c posix_poll.c \
    x11_init.c x11_monitor.c x11_window.c xkb_unicode.c \
    glx_context.c linux_joystick.c
endif

ifeq ($(UNAME_S),Darwin)
GLFW_DEFS     := -D_GLFW_COCOA
PLATFORM_LIBS := \
    -framework Cocoa -framework IOKit \
    -framework CoreFoundation -framework OpenGL
GLFW_PLAT_SRC := \
    posix_thread.c posix_module.c macos_time.c \
    cocoa_init.m cocoa_joystick.m cocoa_monitor.m \
    cocoa_window.m nsgl_context.m
endif

GLFW_COMMON_SRC := \
    context.c init.c input.c monitor.c platform.c vulkan.c window.c \
    egl_context.c osmesa_context.c \
    null_init.c null_monitor.c null_window.c null_joystick.c

GLFW_ALL_SRC := $(GLFW_COMMON_SRC) $(GLFW_PLAT_SRC)
GLFW_C_SRC   := $(filter %.c,$(GLFW_ALL_SRC))
GLFW_M_SRC   := $(filter %.m,$(GLFW_ALL_SRC))
GLFW_OBJS    := $(patsubst %.c,$(BUILD)/glfw/%.o,$(GLFW_C_SRC)) \
                $(patsubst %.m,$(BUILD)/glfw/%.o,$(GLFW_M_SRC))

IMGUI_SRC         := imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp
IMGUI_BACKEND_SRC := imgui_impl_glfw.cpp imgui_impl_opengl3.cpp
IMGUI_OBJS        := $(patsubst %.cpp,$(BUILD)/imgui/%.o,$(IMGUI_SRC)) \
                     $(patsubst %.cpp,$(BUILD)/imgui_be/%.o,$(IMGUI_BACKEND_SRC))

UI_OBJS := $(BUILD)/ui/ui.o

.PHONY: all clean test

all: $(BUILD)/ostrich $(BUILD)/libglfw.a $(BUILD)/libui.a

# ── GLFW ──────────────────────────────────────────────────────────────
$(BUILD)/glfw/%.o: $(GLFW_DIR)/src/%.c | $(BUILD)/glfw
	$(CC) -c $(GLFW_DEFS) -I$(GLFW_DIR)/include -I$(GLFW_DIR)/src \
	    $(LOCAL_CFLAGS) -w -O2 -o $@ $<

$(BUILD)/glfw/%.o: $(GLFW_DIR)/src/%.m | $(BUILD)/glfw
	$(CC) -c $(GLFW_DEFS) -I$(GLFW_DIR)/include -I$(GLFW_DIR)/src \
	    $(LOCAL_CFLAGS) -w -O2 -o $@ $<

$(BUILD)/libglfw.a: $(GLFW_OBJS)
	ar rcs $@ $^

# ── ImGui ─────────────────────────────────────────────────────────────
$(BUILD)/imgui/%.o: $(IMGUI_DIR)/%.cpp | $(BUILD)/imgui
	$(CXX) -std=c++17 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
	    -I$(GLFW_DIR)/include -w -O2 -c $< -o $@

$(BUILD)/imgui_be/%.o: $(IMGUI_DIR)/backends/%.cpp | $(BUILD)/imgui_be
	$(CXX) -std=c++17 -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
	    -I$(GLFW_DIR)/include -w -O2 -c $< -o $@

# ── UI library (our code) ─────────────────────────────────────────────
$(BUILD)/ui/ui.o: $(SRC)/ui/ui.cpp | $(BUILD)/ui
	$(CXX) $(CXXFLAGS) -I$(INCLUDE) -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends \
	    -I$(GLFW_DIR)/include -c $< -o $@

$(BUILD)/libui.a: $(UI_OBJS) $(IMGUI_OBJS)
	ar rcs $@ $^

# ── Ostrich binary ────────────────────────────────────────────────────
$(BUILD)/app_main.o: $(SRC)/main.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_arena.o: $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_lexicon.o: $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_framestats.o: $(SRC)/framestats.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

APP_OBJS := $(BUILD)/app_main.o $(BUILD)/app_arena.o \
            $(BUILD)/app_lexicon.o $(BUILD)/app_framestats.o

$(BUILD)/ostrich: $(APP_OBJS) $(BUILD)/libui.a $(BUILD)/libglfw.a
	$(CXX) -o $@ $(APP_OBJS) \
	    $(BUILD)/libui.a $(BUILD)/libglfw.a $(PLATFORM_LIBS)

# ── Tests ─────────────────────────────────────────────────────────────
$(BUILD)/arena_test: $(TESTS)/arena_test.c $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

$(BUILD)/lexicon_test: $(TESTS)/lexicon_test.c $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

$(BUILD)/framestats_test: $(TESTS)/framestats_test.c $(SRC)/framestats.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

$(BUILD)/ui_test.o: $(TESTS)/ui_test.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/ui_arena.o: $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/ui_test: $(BUILD)/ui_test.o $(BUILD)/ui_arena.o \
                  $(BUILD)/libui.a $(BUILD)/libglfw.a
	$(CXX) -o $@ $(BUILD)/ui_test.o $(BUILD)/ui_arena.o \
	    $(BUILD)/libui.a $(BUILD)/libglfw.a $(PLATFORM_LIBS)

test: all $(BUILD)/arena_test $(BUILD)/lexicon_test \
      $(BUILD)/framestats_test $(BUILD)/ui_test
	./$(BUILD)/arena_test
	./$(BUILD)/lexicon_test
	./$(BUILD)/framestats_test
	./$(BUILD)/ui_test

# ── Build directories ─────────────────────────────────────────────────
$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/glfw: | $(BUILD)
	mkdir -p $(BUILD)/glfw

$(BUILD)/imgui: | $(BUILD)
	mkdir -p $(BUILD)/imgui

$(BUILD)/imgui_be: | $(BUILD)
	mkdir -p $(BUILD)/imgui_be

$(BUILD)/ui: | $(BUILD)
	mkdir -p $(BUILD)/ui

clean:
	rm -rf $(BUILD)
