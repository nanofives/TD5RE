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
# This Makefile does not replace the .bat scripts; they remain authoritative.
# It shares the module list with them via src/td5re/srcs.txt. Builds the DEV
# variant only (no -DTD5RE_RELEASE); use build_standalone.bat for RELEASE.

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
# td5re sources -- SINGLE SOURCE OF TRUTH: src/td5re/srcs.txt (one module per
# line), shared with build_standalone.bat + the CI workflows so the list cannot
# drift. Requires GNU Make >= 4.0 for $(file). $(filter %.c,...) drops any blank
# or comment tokens.
# ---------------------------------------------------------------------------

TD5RE_SRCS := $(filter %.c,$(file < $(TD5RE_SRC)/srcs.txt))

# Objects live flat in build/ (basename), matching the .bat / CI layout;
# deps/cjson/cJSON.c -> build/cJSON.o via the explicit rule below.
TD5RE_OBJS := $(addprefix $(TD5RE_BUILD)/,$(notdir $(TD5RE_SRCS:.c=.o)))
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

# Common flags -- SINGLE SOURCE OF TRUTH: src/td5re/cflags.txt (shared with
# build_standalone.bat + the CI workflows). $(filter -%,...) keeps only flag
# tokens, dropping comment words and blanks. Path-dependent -I dirs appended.
TD5RE_CFLAGS   := $(filter -%,$(file < $(TD5RE_SRC)/cflags.txt)) \
                  -I$(TD5RE_SRC) -I$(WRAPPER_SRC) -I$(ZLIB_INC)

WRAPPER_CFLAGS := -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG -m32

# png_loader.c additionally needs -Wno-unused-function per build.bat.
$(WRAPPER_BUILD)/png_loader.o: WRAPPER_CFLAGS += -Wno-unused-function

# System link libs -- SINGLE SOURCE OF TRUTH: src/td5re/link_libs.txt
# (shared with build_standalone.bat + the CI workflows).
LINK_LIBS := $(filter -l%,$(file < $(TD5RE_SRC)/link_libs.txt))

LINK_FLAGS := -m32 -mwindows -static \
              -Wl,--enable-stdcall-fixup \
              -Wl,--allow-multiple-definition

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------

.PHONY: all td5re wrapper clean rebuild dirs verify-assets

all: wrapper td5re

# Pack-on-load regression guard: re-pack every editable source and byte-compare
# against the retired originals, then check the glTF mesh round-trip. Run from
# the project root (where re/assets + re/_retired_dats live).
verify-assets:
	cd "$(PROJECT_ROOT)" && python re/tools/verify_assets.py
	cd "$(PROJECT_ROOT)" && python re/tools/mesh_tool.py selfcheck-glb-all re/_retired_dats

dirs:
	@mkdir -p "$(TD5RE_BUILD)" "$(WRAPPER_BUILD)"

# --- td5re object files ---
$(TD5RE_BUILD)/%.o: $(TD5RE_SRC)/%.c | dirs
	"$(GCC)" $(TD5RE_CFLAGS) $< -o $@

# --- vendored cJSON (lives in deps/cjson/) ---
$(TD5RE_BUILD)/cJSON.o: $(TD5RE_SRC)/deps/cjson/cJSON.c | dirs
	"$(GCC)" $(TD5RE_CFLAGS) $< -o $@

# --- wrapper object files ---
$(WRAPPER_BUILD)/%.o: $(WRAPPER_SRC)/%.c | dirs
	"$(GCC)" $(WRAPPER_CFLAGS) $< -o $@

# --- wrapper static archive (consumed by td5re link) ---
$(WRAPPER_BUILD)/libddraw_wrapper.a: $(WRAPPER_OBJS)
	"$(AR)" rcs $@ $^

# --- wrapper DLL (legacy artifact from the retired DLL-proxy setup) ---
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
