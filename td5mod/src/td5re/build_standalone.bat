@echo off
setlocal enabledelayedexpansion

REM === TD5RE Standalone Build ===
REM Compiles all td5re modules + wrapper backend into the source-port exe
REM and copies the result to the project root.
REM
REM Usage:
REM   build_standalone.bat            -> DEV build    (td5re.exe, full RE instrumentation)
REM   build_standalone.bat dev        -> DEV build    (same as no arg)
REM   build_standalone.bat release    -> RELEASE build (td5re_release.exe, instrumentation stripped)
REM
REM Any other first argument (e.g. a PID passed by the /fix and /end skills) is
REM treated as DEV, so the historic "build_standalone.bat <pid>" call still works.
REM
REM DEV and RELEASE use separate object dirs (build\ vs build_release\) so their
REM differing -D flags never share a stale .o cache. Both deploy to the project
REM root. Build BOTH at once with build_all.bat.

cd /d "%~dp0"

REM ---------------------------------------------------------------------------
REM Variant selection
REM ---------------------------------------------------------------------------
set VARIANT=dev
if /I "%~1"=="release" set VARIANT=release

set GCC=..\..\deps\mingw\mingw32\bin\gcc.exe
set AR=..\..\deps\mingw\mingw32\bin\ar.exe
set WINDRES=..\..\deps\mingw\mingw32\bin\windres.exe
set STRINGS=..\..\deps\mingw\mingw32\bin\strings.exe
set SRCDIR=.
set WRAPPER_SRCDIR=..\..\ddraw_wrapper\src
set WRAPPER_BUILDDIR=..\..\ddraw_wrapper\build
set ZLIB_INC=..\..\deps\mingw\mingw32\i686-w64-mingw32\include
set PROJECT_ROOT=..\..\..

REM ---------------------------------------------------------------------------
REM Module list (shared by DEV and RELEASE). td5_trace.c (the CSV race-trace
REM harness) is inert at runtime unless [Trace] knobs enable it; the release
REM build additionally hard-disables every trace knob in main.c.
REM ---------------------------------------------------------------------------
set TD5RE_SRCS_COMMON=td5re.c td5_game.c td5_physics.c td5_track.c td5_track_registry.c td5_ai.c td5_render.c td5_frontend.c td5_fe_net.c td5_fe_race.c td5_fe_menu.c td5_frontend_button_cache.c td5_font.c td5_hud.c td5_sound.c td5_input.c td5_asset.c td5_assetsrc.c td5_customcar.c deps\cjson\cJSON.c td5_inflate.c td5_save.c td5_pending.c td5_net.c td5_upnp.c td5_camera.c td5_vfx.c td5_arcade.c td5_damage.c td5_tutorial.c td5_fmv.c td5_benchmark.c td5_trace.c td5_trig_lut_data.c td5_profile.c td5_jobs.c td5_rcmd.c td5_platform_win32.c td5_msvc_rand.c main.c

REM Shared compiler flags for every variant.
set CFLAGS_BASE=-c -O2 -fwrapv -Wall -Wextra -Wpedantic -DWIN32 -m32 -I%SRCDIR% -I%WRAPPER_SRCDIR% -I%ZLIB_INC% -DTD5_INFLATE_USE_ZLIB

REM Per-variant configuration (goto-based, NOT parenthesized blocks, so comments
REM containing parentheses cannot corrupt the batch parser).
if /I "%VARIANT%"=="release" goto :cfg_release

REM --- DEV: full debug affordances ---
set BUILDDIR=build
set EXE=td5re.exe
set MAPFILE=td5re.map
set TD5RE_SRCS=!TD5RE_SRCS_COMMON!
set CFLAGS=!CFLAGS_BASE!
set EXTRA_LDFLAGS=
goto :cfg_done

:cfg_release
REM --- RELEASE: define TD5RE_RELEASE so dev affordances (trace knobs, debug
REM     overlays, net selftest) compile out / hard-disable, then strip the
REM     symbol table. ---
set BUILDDIR=build_release
set EXE=td5re_release.exe
set MAPFILE=td5re_release.map
set TD5RE_SRCS=!TD5RE_SRCS_COMMON!
REM Strip the symbol table (-s). We intentionally do NOT use
REM -ffunction-sections/--gc-sections: per-function section padding bloated the
REM binary by ~1 MB while reclaiming only small dead code, so the net was larger.
set CFLAGS=!CFLAGS_BASE! -DTD5RE_RELEASE -DNDEBUG
set EXTRA_LDFLAGS=-s

:cfg_done

REM Verify compiler
"%GCC%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: gcc not found at %GCC%
    exit /b 1
)

if not exist !BUILDDIR! mkdir !BUILDDIR!

echo === TD5RE Standalone Build [!VARIANT!] -^> !EXE! ===
echo.

REM ---------------------------------------------------------------------------
REM Compile td5re modules
REM ---------------------------------------------------------------------------
set FAIL=0
for %%F in (!TD5RE_SRCS!) do (
    echo Compiling %%~nF.c...
    "%GCC%" !CFLAGS! %SRCDIR%\%%F -o !BUILDDIR!\%%~nF.o
    if errorlevel 1 (
        echo FAILED: %%F
        set FAIL=1
        goto :check_fail
    )
)

:check_fail
if !FAIL!==1 goto :fail

REM ---------------------------------------------------------------------------
REM Compile resource file (icon). This is shared by DEV and RELEASE -- both link
REM !RESOBJ! below, so the TD5 app icon (td5re.rc -> td5re.ico, multi-size
REM 16/32/48/256 under resource id 1) is embedded in BOTH td5re.exe and
REM td5re_release.exe. A missing/failed resource is treated as FATAL: shipping a
REM release exe with the generic default Windows icon is a regression, so we stop
REM the build rather than silently drop the icon.
REM ---------------------------------------------------------------------------
echo Compiling td5re.rc...
REM The .ico is referenced by td5re.rc via a relative path; if it is absent at
REM build time (e.g. not checked out / not git-tracked on another machine),
REM windres can still succeed but embed an empty/placeholder icon, shipping an
REM exe that falls back to the generic default Windows icon on other computers.
REM Catch that FATALLY here rather than silently shipping no icon.
if not exist %SRCDIR%\td5re.ico (
    echo ERROR: td5re.ico missing -- app icon would be absent, aborting build
    goto :fail
)
del !BUILDDIR!\td5re_res.o 2>nul
REM --include-dir %SRCDIR% makes the relative "td5re.ico" reference in td5re.rc
REM resolve deterministically regardless of windres' working directory.
"%WINDRES%" -F pe-i386 --include-dir %SRCDIR% -i %SRCDIR%\td5re.rc -o !BUILDDIR!\td5re_res.o
if errorlevel 1 (
    echo ERROR: windres failed -- app icon would be missing, aborting build
    goto :fail
)
if not exist !BUILDDIR!\td5re_res.o (
    echo ERROR: windres produced no resource object, aborting build
    goto :fail
)
set "RESOBJ=!BUILDDIR!\td5re_res.o"

REM ---------------------------------------------------------------------------
REM Build static archive from all td5re .o files (excluding main.o).
REM Delete any stale archive first: "ar rcs" only adds/replaces members and
REM never removes them, so a previously-archived td5re_res.o would linger and
REM collide with the directly-linked icon (.rsrc duplicate-leaf merge failure).
REM ---------------------------------------------------------------------------
echo Creating libtd5re.a...
if exist !BUILDDIR!\libtd5re.a del !BUILDDIR!\libtd5re.a
set ARCHIVE_OBJS=
for %%F in (!TD5RE_SRCS!) do (
    if /I NOT "%%~nF"=="main" (
        set "ARCHIVE_OBJS=!ARCHIVE_OBJS! !BUILDDIR!\%%~nF.o"
    )
)
"%AR%" rcs !BUILDDIR!\libtd5re.a !ARCHIVE_OBJS!
if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Link the exe. main.o + libtd5re.a + libddraw_wrapper.a + system libs.
REM --whole-archive for libtd5re.a includes ALL modules (prevents static-library
REM symbol pruning of function-pointer-table-only modules). RELEASE additionally
REM passes --gc-sections (paired with -ffunction-sections) so dead per-function
REM sections are still removed, plus -s to strip the symbol table.
REM ---------------------------------------------------------------------------
echo Linking !EXE!...
"%GCC%" -m32 -mwindows -static -o !BUILDDIR!\!EXE! ^
    !BUILDDIR!\main.o !RESOBJ! ^
    -L!BUILDDIR! -Wl,--whole-archive -ltd5re -Wl,--no-whole-archive ^
    -L%WRAPPER_BUILDDIR% -lddraw_wrapper ^
    -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -lole32 -lshell32 ^
    -lwinmm -ldinput8 -ldsound -ldxguid -lz -lws2_32 -lmfplat -lmfreadwrite -lmfuuid -lole32 ^
    -Wl,-Map=!BUILDDIR!\!MAPFILE! ^
    -Wl,--enable-stdcall-fixup ^
    -Wl,--allow-multiple-definition ^
    !EXTRA_LDFLAGS!

if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Deploy to project root
REM ---------------------------------------------------------------------------
echo Deploying to project root...
copy /Y !BUILDDIR!\!EXE! %PROJECT_ROOT%\!EXE! >nul
if errorlevel 1 goto :fail

echo.
for %%F in (%PROJECT_ROOT%\!EXE!) do echo === BUILD OK [!VARIANT!]: %%~fF (%%~zF bytes) ===
REM Explicit success exit so stray errorlevels never leak through `call` to
REM build_all and read as a failure.
endlocal
exit /b 0

:fail
echo.
echo === BUILD FAILED [!VARIANT!] ===
endlocal
exit /b 1
