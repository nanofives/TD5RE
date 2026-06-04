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
REM Source split: COMMON modules are in every build; PILOT modules are the RE
REM trace instrumentation, compiled into DEV only and excluded from RELEASE.
REM The 3 td5_trace*.c front-end modules stay in COMMON but are inert at
REM runtime -- the release build hard-disables every trace knob in main.c.
REM ---------------------------------------------------------------------------
set TD5RE_SRCS_COMMON=td5re.c td5_game.c td5_physics.c td5_track.c td5_ai.c td5_render.c td5_frontend.c td5_frontend_button_cache.c td5_hud.c td5_sound.c td5_input.c td5_asset.c td5_inflate.c td5_save.c td5_net.c td5_upnp.c td5_camera.c td5_vfx.c td5_fmv.c td5_benchmark.c td5_trace.c td5_trace_whole_state.c td5_trace_replay.c td5_trig_lut_data.c td5_profile.c td5_platform_win32.c td5_msvc_rand.c main.c
set TD5RE_SRCS_PILOT=td5_pilot_trace.c td5_pilot_trace_0040A720.c td5_pilot_trace_trig.c td5_pilot_trace_00403A20.c td5_pilot_trace_00403D90.c td5_pilot_trace_00404030.c td5_pilot_trace_00404EC0.c td5_pilot_trace_004057F0.c td5_pilot_trace_00405B40.c td5_pilot_trace_00405D70.c td5_pilot_trace_00405E80.c td5_pilot_trace_004063A0.c td5_pilot_trace_00406650.c td5_pilot_trace_00406980.c td5_pilot_trace_00409150.c td5_pilot_trace_0042EB10.c td5_pilot_trace_0042EBF0.c td5_pilot_trace_0042F030.c td5_pilot_trace_00432D60.c td5_pilot_trace_00432E60.c td5_pilot_trace_004340C0.c td5_pilot_trace_00434350.c td5_pilot_trace_00434FE0.c td5_pilot_trace_00436A70.c td5_pilot_trace_004370A0.c td5_pilot_trace_004440F0.c td5_pilot_trace_pool15_spline.c td5_pilot_trace_traffic.c td5_pilot_trace_v2v_contact.c td5_pilot_trace_v2v.c

REM Shared compiler flags for every variant.
set CFLAGS_BASE=-c -O2 -fwrapv -Wall -Wextra -Wpedantic -DWIN32 -m32 -I%SRCDIR% -I%WRAPPER_SRCDIR% -I%ZLIB_INC% -DTD5_INFLATE_USE_ZLIB

REM Per-variant configuration (goto-based, NOT parenthesized blocks, so comments
REM containing parentheses cannot corrupt the batch parser).
if /I "%VARIANT%"=="release" goto :cfg_release

REM --- DEV: full RE instrumentation, byte-identical to the historic build ---
set BUILDDIR=build
set EXE=td5re.exe
set MAPFILE=td5re.map
set TD5RE_SRCS=!TD5RE_SRCS_COMMON! !TD5RE_SRCS_PILOT!
set CFLAGS=!CFLAGS_BASE! -DTD5_PILOT_TRACE_0040A720 -DTD5_PILOT_TRACE_00403D90 -DTD5_PILOT_TRACE_00409150 -DTD5_PILOT_TRACE_00434350 -DTD5_PILOT_TRACE_004370A0 -DTD5_PILOT_TRACE_TRAFFIC
set EXTRA_LDFLAGS=
goto :cfg_done

:cfg_release
REM --- RELEASE: strip the trace instrumentation. No pilot modules, no
REM     -DTD5_PILOT_TRACE_* hot-path hooks, define TD5RE_RELEASE so the few
REM     ungated trace call sites and dev affordances compile out, and let the
REM     linker GC the now-dead functions, then strip the symbol table. ---
set BUILDDIR=build_release
set EXE=td5re_release.exe
set MAPFILE=td5re_release.map
REM COMMON modules + the release stub TU. The stub supplies empty definitions
REM for the always-on pilot-trace leaf emitters whose .c modules are excluded.
set TD5RE_SRCS=!TD5RE_SRCS_COMMON! td5_release_stubs.c
REM Strip the symbol table (-s). We intentionally do NOT use
REM -ffunction-sections/--gc-sections: per-function section padding bloated the
REM binary by ~1 MB while reclaiming only the small dead F12/HUD/stub code, so
REM the net was larger. The real strip comes from excluding the 30 pilot .c
REM modules + dropping the -DTD5_PILOT_TRACE_* hot hooks + NDEBUG.
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
REM Compile resource file (icon)
REM ---------------------------------------------------------------------------
echo Compiling td5re.rc...
"%WINDRES%" -F pe-i386 -i %SRCDIR%\td5re.rc -o !BUILDDIR!\td5re_res.o
if errorlevel 1 (
    echo WARNING: windres failed, icon will not be embedded
    set "RESOBJ="
) else (
    set "RESOBJ=!BUILDDIR!\td5re_res.o"
)

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
    -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -lole32 ^
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

REM ---------------------------------------------------------------------------
REM Verify build: trace instrumentation strings must be PRESENT in the dev
REM build and ABSENT in the release build. "log/port/pool" is the OUT_PATH
REM literal compiled only into the pilot-trace modules.
REM ---------------------------------------------------------------------------
echo.
echo === Verifying build artifact ===
"%STRINGS%" %PROJECT_ROOT%\!EXE! | findstr /C:"log/port/pool" >nul 2>&1
set HAS_INSTR=!errorlevel!
if /I "%VARIANT%"=="release" goto :verify_release

REM DEV: instrumentation should be present.
if !HAS_INSTR!==0 (
    echo OK: dev !EXE! contains expected instrumentation strings.
) else (
    echo WARNING: dev !EXE! does NOT contain expected instrumentation strings.
    echo The build may be stale or the linker may have stripped the code.
)
goto :verify_done

:verify_release
REM RELEASE: instrumentation must be absent.
if !HAS_INSTR!==0 (
    echo ERROR: release !EXE! STILL contains pilot-trace instrumentation strings.
    echo The strip is incomplete -- a trace module linked in. Investigate before shipping.
    goto :fail
)
echo OK: release !EXE! is clean -- no pilot-trace instrumentation strings.

:verify_done
echo.
for %%F in (%PROJECT_ROOT%\!EXE!) do echo === BUILD OK [!VARIANT!]: %%~fF (%%~zF bytes) ===
REM Explicit success exit — the verify findstr above leaves errorlevel=1 when the
REM release build is clean (string not found), which would otherwise leak through
REM `call` to build_all and read as a failure. Force 0 here on the success path.
endlocal
exit /b 0

:fail
echo.
echo === BUILD FAILED [!VARIANT!] ===
endlocal
exit /b 1
