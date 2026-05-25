CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra

BUILD   := build
SRC     := src
TESTS   := tests
INCLUDE := include

IMGUI_DIR   := third_party/imgui
GLFW_DIR    := third_party/glfw
LIBSSH2_DIR := third_party/libssh2
JSMN_DIR    := third_party/jsmn

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
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null)
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null)
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
# Homebrew prefix fallback when pkg-config misses it
BREW_PREFIX    := $(shell brew --prefix openssl 2>/dev/null)
OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null || \
                      (test -n "$(BREW_PREFIX)" && echo "-I$(BREW_PREFIX)/include"))
OPENSSL_LIBS   := $(shell pkg-config --libs   openssl 2>/dev/null || \
                      (test -n "$(BREW_PREFIX)" && echo "-L$(BREW_PREFIX)/lib -lssl -lcrypto"))
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

# libssh2 sources: OpenSSL backend only; exclude libgcrypt, mbedtls, os400qc3, wincng
LIBSSH2_SRC := \
    agent.c bcrypt_pbkdf.c blowfish.c chacha.c channel.c \
    cipher-chachapoly.c comp.c crypt.c global.c hostkey.c \
    keepalive.c kex.c knownhost.c mac.c misc.c openssl.c \
    packet.c pem.c poly1305.c publickey.c scp.c session.c \
    sftp.c transport.c userauth.c userauth_kbd_packet.c version.c
LIBSSH2_OBJS := $(patsubst %.c,$(BUILD)/libssh2/%.o,$(LIBSSH2_SRC))
LIBSSH2_CFLAGS := \
    -DHAVE_CONFIG_H -DLIBSSH2_OPENSSL \
    -Ithird_party \
    -I$(LIBSSH2_DIR)/include -I$(LIBSSH2_DIR)/src \
    $(LOCAL_CFLAGS) $(OPENSSL_CFLAGS)

# libssh (our wrapper over libssh2)
SSH_SRC  := ssh.c
SSH_OBJS := $(patsubst %.c,$(BUILD)/ssh/%.o,$(SSH_SRC))
SSH_CFLAGS := -I$(LIBSSH2_DIR)/include $(LOCAL_CFLAGS) $(OPENSSL_CFLAGS)

# connstate (pure lifecycle core)
CONNSTATE_SRC  := connstate.c
CONNSTATE_OBJS := $(patsubst %.c,$(BUILD)/connstate/%.o,$(CONNSTATE_SRC))

# store (saved connections)
STORE_SRC  := store.c
STORE_OBJS := $(patsubst %.c,$(BUILD)/store/%.o,$(STORE_SRC))

# session (worker thread + SPSC rings)
SESSION_SRC  := session.c
SESSION_OBJS := $(patsubst %.c,$(BUILD)/session/%.o,$(SESSION_SRC))

# discovery (pure functional core: command builders, curation, readiness)
DISCOVERY_SRC  := discovery.c
DISCOVERY_OBJS := $(patsubst %.c,$(BUILD)/discovery/%.o,$(DISCOVERY_SRC))

.PHONY: all clean test debug ssh_version_smoke ssh_smoke session_smoke discovery_smoke

all: $(BUILD)/ostrich $(BUILD)/libglfw.a $(BUILD)/libui.a \
     $(BUILD)/liblibssh2.a $(BUILD)/libssh.a $(BUILD)/libconnstate.a \
     $(BUILD)/libstore.a $(BUILD)/libsession.a $(BUILD)/libdiscovery.a

# ── GLFW ──────────────────────────────────────────────────────────────
$(BUILD)/glfw/%.o: $(GLFW_DIR)/src/%.c | $(BUILD)/glfw
	$(CC) -c $(GLFW_DEFS) -I$(GLFW_DIR)/include -I$(GLFW_DIR)/src \
	    $(LOCAL_CFLAGS) -w -O2 -o $@ $<

$(BUILD)/glfw/%.o: $(GLFW_DIR)/src/%.m | $(BUILD)/glfw
	$(CC) -c $(GLFW_DEFS) -I$(GLFW_DIR)/include -I$(GLFW_DIR)/src \
	    $(LOCAL_CFLAGS) -w -O2 -o $@ $<

$(BUILD)/libglfw.a: $(GLFW_OBJS)
	ar rcs $@ $^

# ── libssh2 ───────────────────────────────────────────────────────────
$(BUILD)/libssh2/%.o: $(LIBSSH2_DIR)/src/%.c | $(BUILD)/libssh2
	$(CC) -c $(LIBSSH2_CFLAGS) -w -O2 -o $@ $<

$(BUILD)/liblibssh2.a: $(LIBSSH2_OBJS)
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

$(BUILD)/app_app.o: $(SRC)/app/app.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_form.o: $(SRC)/app/app_form.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_arena.o: $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_lexicon.o: $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_framestats.o: $(SRC)/framestats.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/app_spsc_ring.o: $(SRC)/spsc_ring.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

APP_OBJS := $(BUILD)/app_main.o $(BUILD)/app_app.o $(BUILD)/app_form.o \
            $(BUILD)/app_arena.o $(BUILD)/app_lexicon.o \
            $(BUILD)/app_framestats.o $(BUILD)/app_spsc_ring.o

$(BUILD)/ostrich: $(APP_OBJS) $(BUILD)/libui.a $(BUILD)/libglfw.a \
                  $(BUILD)/libsession.a $(BUILD)/libssh.a \
                  $(BUILD)/libconnstate.a $(BUILD)/libstore.a \
                  $(BUILD)/libdiscovery.a $(BUILD)/liblibssh2.a
	$(CXX) -o $@ $(APP_OBJS) \
	    $(BUILD)/libui.a $(BUILD)/libglfw.a \
	    $(BUILD)/libsession.a $(BUILD)/libssh.a \
	    $(BUILD)/libconnstate.a $(BUILD)/libstore.a \
	    $(BUILD)/libdiscovery.a $(BUILD)/liblibssh2.a \
	    $(OPENSSL_LIBS) $(PLATFORM_LIBS)

# ── ssh library (our libssh2 wrapper) ────────────────────────────────
$(BUILD)/ssh/%.o: $(SRC)/ssh/%.c | $(BUILD)/ssh
	$(CC) $(CFLAGS) -I$(INCLUDE) $(SSH_CFLAGS) -c $< -o $@

$(BUILD)/libssh.a: $(SSH_OBJS)
	ar rcs $@ $^

# ── connstate library (pure lifecycle core) ───────────────────────────
$(BUILD)/connstate/%.o: $(SRC)/connstate/%.c | $(BUILD)/connstate
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/libconnstate.a: $(CONNSTATE_OBJS)
	ar rcs $@ $^

# ── store library (saved connections) ────────────────────────────────
$(BUILD)/store/%.o: $(SRC)/store/%.c | $(BUILD)/store
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/libstore.a: $(STORE_OBJS)
	ar rcs $@ $^

# ── session library (worker thread + rings) ───────────────────────────
$(BUILD)/session/%.o: $(SRC)/session/%.c | $(BUILD)/session
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/libsession.a: $(SESSION_OBJS)
	ar rcs $@ $^

# ── discovery library (pure functional core) ──────────────────────────
$(BUILD)/discovery/%.o: $(SRC)/discovery/%.c | $(BUILD)/discovery
	$(CC) $(CFLAGS) -I$(INCLUDE) -I$(JSMN_DIR) -c $< -o $@

$(BUILD)/libdiscovery.a: $(DISCOVERY_OBJS)
	ar rcs $@ $^

# ── Dev smokes (not part of make test) ───────────────────────────────
ssh_version_smoke: $(BUILD)/ssh_version_smoke
	./$(BUILD)/ssh_version_smoke

$(BUILD)/ssh_version_smoke: tools/ssh_version_smoke.c $(BUILD)/liblibssh2.a | $(BUILD)
	$(CC) $(CFLAGS) $(LOCAL_CFLAGS) $(OPENSSL_CFLAGS) \
	    -I$(LIBSSH2_DIR)/include \
	    -o $@ $< $(BUILD)/liblibssh2.a $(OPENSSL_LIBS)

ssh_smoke: $(BUILD)/ssh_smoke

$(BUILD)/ssh_smoke: tools/ssh_smoke.c $(BUILD)/libssh.a $(BUILD)/liblibssh2.a \
                    $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) $(SSH_CFLAGS) \
	    -o $@ tools/ssh_smoke.c $(SRC)/arena.c \
	    $(BUILD)/libssh.a $(BUILD)/liblibssh2.a $(OPENSSL_LIBS)

session_smoke: $(BUILD)/session_smoke

$(BUILD)/session_smoke: tools/session_smoke.c $(BUILD)/libsession.a \
                        $(BUILD)/libssh.a $(BUILD)/libconnstate.a \
                        $(BUILD)/liblibssh2.a \
                        $(SRC)/arena.c $(SRC)/spsc_ring.c $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) $(SSH_CFLAGS) \
	    -o $@ tools/session_smoke.c \
	    $(SRC)/arena.c $(SRC)/spsc_ring.c $(SRC)/lexicon.c \
	    $(BUILD)/libsession.a $(BUILD)/libssh.a $(BUILD)/libconnstate.a \
	    $(BUILD)/liblibssh2.a $(OPENSSL_LIBS) -lpthread -lm

discovery_smoke: $(BUILD)/discovery_smoke

$(BUILD)/discovery_smoke: tools/discovery_smoke.c $(BUILD)/libsession.a \
                          $(BUILD)/libssh.a $(BUILD)/libconnstate.a \
                          $(BUILD)/libdiscovery.a $(BUILD)/liblibssh2.a \
                          $(SRC)/arena.c $(SRC)/spsc_ring.c $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) $(SSH_CFLAGS) \
	    -o $@ tools/discovery_smoke.c \
	    $(SRC)/arena.c $(SRC)/spsc_ring.c $(SRC)/lexicon.c \
	    $(BUILD)/libsession.a $(BUILD)/libssh.a $(BUILD)/libconnstate.a \
	    $(BUILD)/libdiscovery.a $(BUILD)/liblibssh2.a \
	    $(OPENSSL_LIBS) -lpthread -lm

# ── Tests ─────────────────────────────────────────────────────────────
$(BUILD)/app_test: $(TESTS)/app_test.c $(BUILD)/app_form.o $(BUILD)/app_arena.o \
                   $(BUILD)/libconnstate.a | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $(TESTS)/app_test.c \
	    $(BUILD)/app_form.o $(BUILD)/app_arena.o \
	    $(SRC)/lexicon.c $(BUILD)/libconnstate.a -lm

$(BUILD)/connstate_test: $(TESTS)/connstate_test.c $(BUILD)/libconnstate.a \
                         $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $(TESTS)/connstate_test.c \
	    $(SRC)/lexicon.c $(BUILD)/libconnstate.a -lm

$(BUILD)/store_test: $(TESTS)/store_test.c $(BUILD)/libstore.a \
                     $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $(TESTS)/store_test.c \
	    $(SRC)/arena.c $(BUILD)/libstore.a

$(BUILD)/spsc_ring_test: $(TESTS)/spsc_ring_test.c $(SRC)/spsc_ring.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $^

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

$(BUILD)/ui_lexicon.o: $(SRC)/lexicon.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/ui_framestats.o: $(SRC)/framestats.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -c $< -o $@

$(BUILD)/ui_test: $(BUILD)/ui_test.o $(BUILD)/ui_arena.o \
                  $(BUILD)/ui_lexicon.o $(BUILD)/ui_framestats.o \
                  $(BUILD)/libui.a $(BUILD)/libglfw.a
	$(CXX) -o $@ $(BUILD)/ui_test.o $(BUILD)/ui_arena.o \
	    $(BUILD)/ui_lexicon.o $(BUILD)/ui_framestats.o \
	    $(BUILD)/libui.a $(BUILD)/libglfw.a $(PLATFORM_LIBS)

$(BUILD)/discovery_test: $(TESTS)/discovery_test.c $(BUILD)/libdiscovery.a \
                         $(SRC)/arena.c | $(BUILD)
	$(CC) $(CFLAGS) -I$(INCLUDE) -o $@ $(TESTS)/discovery_test.c \
	    $(SRC)/arena.c $(BUILD)/libdiscovery.a

$(BUILD)/log_test: $(TESTS)/log_test.c $(SRC)/log.c | $(BUILD)
	$(CC) $(CFLAGS) -DOSTRICH_DEBUG -I$(INCLUDE) -o $@ \
	    $(TESTS)/log_test.c $(SRC)/log.c

test: all $(BUILD)/app_test $(BUILD)/connstate_test $(BUILD)/spsc_ring_test \
      $(BUILD)/arena_test $(BUILD)/lexicon_test $(BUILD)/framestats_test \
      $(BUILD)/ui_test $(BUILD)/store_test $(BUILD)/discovery_test \
      $(BUILD)/log_test
	./$(BUILD)/app_test
	./$(BUILD)/connstate_test
	./$(BUILD)/spsc_ring_test
	./$(BUILD)/arena_test
	./$(BUILD)/lexicon_test
	./$(BUILD)/framestats_test
	./$(BUILD)/ui_test
	./$(BUILD)/store_test
	./$(BUILD)/discovery_test
	./$(BUILD)/log_test

debug:
	$(MAKE) CFLAGS="$(CFLAGS) -DOSTRICH_DEBUG -g -O0" all

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

$(BUILD)/libssh2: | $(BUILD)
	mkdir -p $(BUILD)/libssh2

$(BUILD)/ssh: | $(BUILD)
	mkdir -p $(BUILD)/ssh

$(BUILD)/connstate: | $(BUILD)
	mkdir -p $(BUILD)/connstate

$(BUILD)/store: | $(BUILD)
	mkdir -p $(BUILD)/store

$(BUILD)/session: | $(BUILD)
	mkdir -p $(BUILD)/session

$(BUILD)/discovery: | $(BUILD)
	mkdir -p $(BUILD)/discovery

clean:
	rm -rf $(BUILD)
