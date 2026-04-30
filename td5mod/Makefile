# TD5RE Makefile
# ==============
# Mirrors td5mod/src/td5re/build_standalone.bat + td5mod/ddraw_wrapper/build.bat
# without changing flags, output names, or output sizes.
#
# Targets:
#   make td5re      — build td5re.exe (requires wrapper to exist)
#   make wrapper    — build libddraw_wrapper.a + ddraw.dll
#   make all        — wrapper, then td5re
#   make clean      — remove build artifacts (both subprojects)
#   make rebuild    — clean + all
#
# Invoke from td5mod/ via mingw32-make. Windows-native make is required —
# the bundled toolchain ships td5mod/deps/mingw/mingw32/bin/mingw32-make.exe.
#
# This Makefile does not replace the .bat scripts; they remain authoritative
# for the legacy PowerShell + cmd.exe invocation path. The Makefile is for
# direct shell use and CI.

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------

MINGW_BIN := deps/mingw/mingw32/bin
GCC       := $(MINGW_BIN)/gcc.exe
AR        := $(MINGW_BIN)/ar.exe
WINDRES   := $(MINGW_BIN)/windres.exe
STRINGS   := $(MINGW_BIN)/strings.exe

# ---------------------------------------------------------------------------
# Layout
# ---------------------------------------------------------------------------

TD5RE_SRC     := src/td5re
TD5RE_BUILD   := $(TD5RE_SRC)/build
WRAPPER_SRC   := ddraw_wrapper/src
WRAPPER_BUILD := ddraw_wrapper/build
ZLIB_INC      := deps/mingw/mingw32/i686-w64-mingw32/include

PROJECT_ROOT  := ..

# ---------------------------------------------------------------------------
# td5re sources (mirrors build_standalone.bat TD5RE_SRCS list, in same order)
# ---------------------------------------------------------------------------

TD5RE_SRCS := \
    td5re.c td5_game.c td5_physics.c td5_track.c td5_ai.c td5_render.c \
    td5_frontend.c td5_hud.c td5_sound.c td5_input.c td5_asset.c \
    td5_inflate.c td5_save.c td5_net.c td5_camera.c td5_vfx.c td5_fmv.c \
    td5_trace.c td5_platform_win32.c main.c

TD5RE_OBJS := $(addprefix $(TD5RE_BUILD)/,$(TD5RE_SRCS:.c=.o))
# libtd5re.a contains every td5re object except main.o (linked separately)
TD5RE_LIB_OBJS := $(filter-out $(TD5RE_BUILD)/main.o,$(TD5RE_OBJS))

# ---------------------------------------------------------------------------
# Wrapper sources (mirrors ddraw_wrapper/build.bat)
# ---------------------------------------------------------------------------

WRAPPER_SRCS := \
    ddraw_main.c ddraw4.c surface4.c d3d3.c device3.c viewport3.c \
    texture2.c d3d11_backend.c png_loader.c

WRAPPER_OBJS := $(addprefix $(WRAPPER_BUILD)/,$(WRAPPER_SRCS:.c=.o))

# ---------------------------------------------------------------------------
# Flags (mirrors the .bat scripts exactly)
# ---------------------------------------------------------------------------

TD5RE_CFLAGS   := -c -O2 -Wall -Wextra -Wpedantic -DWIN32 -m32 \
                  -I$(TD5RE_SRC) -I$(WRAPPER_SRC) -I$(ZLIB_INC) \
                  -DTD5_INFLATE_USE_ZLIB

WRAPPER_CFLAGS := -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG

# png_loader.c additionally needs -Wno-unused-function per build.bat.
$(WRAPPER_BUILD)/png_loader.o: WRAPPER_CFLAGS += -Wno-unused-function

# Link command (mirrors build_standalone.bat link line)
LINK_LIBS := -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -lole32 \
             -lwinmm -ldinput8 -ldsound -ldxguid -lz -lws2_32 \
             -lmfplat -lmfreadwrite -lmfuuid -lole32

LINK_FLAGS := -m32 -mwindows -static \
              -Wl,--enable-stdcall-fixup \
              -Wl,--allow-multiple-definition

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.PHONY: all td5re wrapper clean rebuild dirs

all: wrapper td5re

dirs:
	@mkdir -p "$(TD5RE_BUILD)" "$(WRAPPER_BUILD)"

# --- td5re object files ---
$(TD5RE_BUILD)/%.o: $(TD5RE_SRC)/%.c | dirs
	"$(GCC)" $(TD5RE_CFLAGS) $< -o $@

# --- wrapper object files ---
$(WRAPPER_BUILD)/%.o: $(WRAPPER_SRC)/%.c | dirs
	"$(GCC)" $(WRAPPER_CFLAGS) $< -o $@

# --- wrapper static archive (consumed by td5re link) ---
$(WRAPPER_BUILD)/libddraw_wrapper.a: $(WRAPPER_OBJS)
	"$(AR)" rcs $@ $^

# --- wrapper DLL (legacy artifact for original/td5_ddraw setup) ---
$(WRAPPER_BUILD)/ddraw.dll: $(WRAPPER_OBJS)
	"$(GCC)" -shared -static -o $@ $^ ddraw_wrapper/ddraw.def \
	    -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -ldxguid \
	    -Wl,--enable-stdcall-fixup

wrapper: $(WRAPPER_BUILD)/libddraw_wrapper.a $(WRAPPER_BUILD)/ddraw.dll

# --- td5re static archive ---
$(TD5RE_BUILD)/libtd5re.a: $(TD5RE_LIB_OBJS)
	"$(AR)" rcs $@ $^

# --- td5re resource (icon) ---
$(TD5RE_BUILD)/td5re_res.o: $(TD5RE_SRC)/td5re.rc | dirs
	"$(WINDRES)" -F pe-i386 -i $< -o $@

# --- td5re executable ---
$(TD5RE_BUILD)/td5re.exe: $(TD5RE_BUILD)/main.o $(TD5RE_BUILD)/libtd5re.a \
                         $(TD5RE_BUILD)/td5re_res.o \
                         $(WRAPPER_BUILD)/libddraw_wrapper.a
	"$(GCC)" $(LINK_FLAGS) -o $@ \
	    $(TD5RE_BUILD)/main.o $(TD5RE_BUILD)/td5re_res.o \
	    -L$(TD5RE_BUILD) -Wl,--whole-archive -ltd5re -Wl,--no-whole-archive \
	    -L$(WRAPPER_BUILD) -lddraw_wrapper \
	    $(LINK_LIBS) \
	    -Wl,-Map=$(TD5RE_BUILD)/td5re.map

# --- deploy to repo root ---
$(PROJECT_ROOT)/td5re.exe: $(TD5RE_BUILD)/td5re.exe
	@cp -f "$<" "$@"

td5re: $(PROJECT_ROOT)/td5re.exe

# --- housekeeping ---
clean:
	@rm -rf "$(TD5RE_BUILD)" "$(WRAPPER_BUILD)"

rebuild: clean all
