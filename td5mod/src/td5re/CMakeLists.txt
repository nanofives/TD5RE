cmake_minimum_required(VERSION 3.15)
project(td5re C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Force 32-bit build (matching original binary)
if(MSVC)
    # Use -A Win32 on cmake command line
elseif(MINGW)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -m32")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -m32")
endif()

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------

set(TD5RE_HEADERS
    td5re.h
    td5_types.h
    td5_platform.h
    td5_game.h
    td5_physics.h
    td5_track.h
    td5_ai.h
    td5_render.h
    td5_frontend.h
    td5_hud.h
    td5_sound.h
    td5_input.h
    td5_asset.h
    td5_inflate.h
    td5_save.h
    td5_net.h
    td5_camera.h
    td5_vfx.h
    td5_fmv.h
    td5_trace.h
)

set(TD5RE_SOURCES
    td5re.c
    td5_game.c
    td5_physics.c
    td5_track.c
    td5_ai.c
    td5_render.c
    td5_frontend.c
    td5_hud.c
    td5_sound.c
    td5_input.c
    td5_asset.c
    td5_inflate.c
    td5_save.c
    td5_net.c
    td5_camera.c
    td5_vfx.c
    td5_fmv.c
    td5_trace.c
    td5_platform_win32.c
)

# ---------------------------------------------------------------------------
# Static library (td5re_core)
#
# All game logic compiled as a static lib. This can be linked into:
#   1. A standalone executable (td5re.exe)
#   2. The ASI mod DLL (td5_mod.asi) for incremental replacement
# ---------------------------------------------------------------------------

add_library(td5re_core STATIC ${TD5RE_SOURCES} ${TD5RE_HEADERS})

target_include_directories(td5re_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(td5re_core PUBLIC
    ws2_32
)

# Strict warnings
if(MSVC)
    target_compile_options(td5re_core PRIVATE /W4)
else()
    target_compile_options(td5re_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# Optional: Standalone executable (future)
#
# Uncomment when platform layer implementations are ready.
# ---------------------------------------------------------------------------

# add_executable(td5re main.c)
# target_link_libraries(td5re PRIVATE td5re_core)
# if(WIN32)
#     target_link_libraries(td5re PRIVATE kernel32 user32 gdi32)
# endif()

# ---------------------------------------------------------------------------
# Optional: ASI mod DLL build (integrates with existing td5mod)
#
# This builds td5re_core as a library that td5_mod.c can link against,
# allowing incremental function replacement.
# ---------------------------------------------------------------------------

# To integrate with the parent td5mod CMakeLists.txt:
#   add_subdirectory(src/td5re)
#   target_link_libraries(td5mod PRIVATE td5re_core)
