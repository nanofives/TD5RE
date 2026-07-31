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

REM ---------------------------------------------------------------------------
REM GPU backend: D3D12 (the D3D11 backend was retired at the 2026-07-31 cutover).
REM Links the prebuilt libddraw_wrapper.a (built from wrapper_srcs.txt, which now
REM lists d3d12_backend.c) + -ld3d12 (from link_libs.txt).
REM ---------------------------------------------------------------------------
set "WRAP_LIB=ddraw_wrapper"

REM ---------------------------------------------------------------------------
REM Architecture: x86_64 ONLY (i686 retired 2026-07-30)
REM
REM The port shipped 32-bit until the x64 retarget (branch x64-stage1-sse2)
REM reached full parity: 67/67 modules, suite green, all 8 golden checks
REM bit-identical to the last i686 build. DXR requires a 64-bit process
REM (measured; see docs/plans/X64_DXR_ROADMAP.md), so x86_64 is now the ONE
REM shipping target and produces the plain exe names (td5re.exe /
REM td5re_release.exe). The i686 toolchain is parked in _archive\ and the last
REM 32-bit-buildable tree is tagged in git history.
REM
REM TD5RE_ARCH is no longer honoured -- fail loudly if someone asks for i686
REM rather than silently building 64-bit under a 32-bit label.
REM ---------------------------------------------------------------------------
if /I "%TD5RE_ARCH%"=="i686" (
    echo ERROR: i686 was retired 2026-07-30 -- toolchain parked in _archive\,
    echo        last 32-bit tree available in git history.
    exit /b 1
)
set TOOLPREFIX=..\..\deps\mingw64\mingw64\bin
set WINDRES_TARGET=pe-x86-64
set ARCHLDFLAG=-m64
set GCC=%TOOLPREFIX%\gcc.exe
set AR=%TOOLPREFIX%\ar.exe
set WINDRES=%TOOLPREFIX%\windres.exe
set SRCDIR=.
set WRAPPER_SRCDIR=..\..\ddraw_wrapper\src
set WRAPPER_BUILDDIR=..\..\ddraw_wrapper\build
set PROJECT_ROOT=..\..\..

REM ---------------------------------------------------------------------------
REM Module list (shared by DEV and RELEASE). SINGLE SOURCE OF TRUTH: srcs.txt
REM (one module per line, POSIX '/' paths). build.yml, release.yml and
REM td5mod\Makefile read the SAME file, so the list can no longer drift between
REM build paths -- add/remove modules in srcs.txt, never hand-edit a list.
REM td5_trace.c (the CSV race-trace harness) is inert at runtime unless [Trace]
REM knobs enable it; the release build hard-disables every trace knob in main.c.
REM ---------------------------------------------------------------------------
set "TD5RE_SRCS_COMMON="
for /f "usebackq eol=# delims=" %%L in ("%~dp0srcs.txt") do set "TD5RE_SRCS_COMMON=!TD5RE_SRCS_COMMON! %%L"
if not defined TD5RE_SRCS_COMMON (
    echo ERROR: srcs.txt missing or empty at %~dp0srcs.txt
    exit /b 1
)

REM Shared compiler flags -- SINGLE SOURCE OF TRUTH: cflags.txt (same pattern
REM as srcs.txt; build.yml, release.yml and td5mod\Makefile read the SAME file).
REM Arch flags (-m64) live there too since the i686 retirement collapsed the
REM cflags/cflags_<arch> split back into one file. Only the path-dependent -I
REM include dirs are appended here.
set "CFLAGS_COMMON="
for /f "usebackq eol=# delims=" %%L in ("%~dp0cflags.txt") do set "CFLAGS_COMMON=!CFLAGS_COMMON! %%L"
if not defined CFLAGS_COMMON (
    echo ERROR: cflags.txt missing or empty at %~dp0cflags.txt
    exit /b 1
)
set CFLAGS_BASE=!CFLAGS_COMMON! -I%SRCDIR% -I%WRAPPER_SRCDIR%

REM System link libraries -- SINGLE SOURCE OF TRUTH: link_libs.txt (shared with
REM build.yml, release.yml and td5mod\Makefile so the lib list cannot drift).
set "LINK_LIBS="
for /f "usebackq eol=# delims=" %%L in ("%~dp0link_libs.txt") do set "LINK_LIBS=!LINK_LIBS! %%L"
if not defined LINK_LIBS (
    echo ERROR: link_libs.txt missing or empty at %~dp0link_libs.txt
    exit /b 1
)

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
"%WINDRES%" -F %WINDRES_TARGET% --include-dir %SRCDIR% -i %SRCDIR%\td5re.rc -o !BUILDDIR!\td5re_res.o
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
REM passes -s to strip the symbol table (EXTRA_LDFLAGS). We deliberately do NOT
REM use -ffunction-sections/--gc-sections -- see the variant config above: it
REM bloated the binary by ~1 MB while reclaiming only small dead code.
REM ---------------------------------------------------------------------------
echo Linking !EXE!...
"%GCC%" %ARCHLDFLAG% -mwindows -static -o !BUILDDIR!\!EXE! ^
    !BUILDDIR!\main.o !RESOBJ! ^
    -L!BUILDDIR! -Wl,--whole-archive -ltd5re -Wl,--no-whole-archive ^
    -L%WRAPPER_BUILDDIR% -l!WRAP_LIB! ^
    !LINK_LIBS! ^
    -Wl,-Map=!BUILDDIR!\!MAPFILE! ^
    -Wl,--enable-stdcall-fixup ^
    -Wl,--allow-multiple-definition ^
    !EXTRA_LDFLAGS!

if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Deploy to project root
REM ---------------------------------------------------------------------------
echo Deploying to project root...
REM Force a CLEAN overwrite of the root exe. A running instance memory-maps and
REM LOCKS the exe, so copy fails and the root stays STALE -- delete first so a
REM stale exe can never be silently kept, and fail LOUDLY (not a quiet skip).
if exist %PROJECT_ROOT%\!EXE! del /Q %PROJECT_ROOT%\!EXE! >nul 2>&1
copy /Y !BUILDDIR!\!EXE! %PROJECT_ROOT%\!EXE! >nul
if errorlevel 1 (
    echo.
    echo *** DEPLOY FAILED: cannot write %PROJECT_ROOT%\!EXE!
    echo *** The game is almost certainly STILL RUNNING and locking the exe.
    echo *** Close td5re.exe / td5re_release.exe and rebuild.
    goto :fail
)
REM Sanity: confirm the root exe now matches the freshly-linked build output.
for %%A in (!BUILDDIR!\!EXE!) do set "SRCSZ=%%~zA"
for %%B in (%PROJECT_ROOT%\!EXE!) do set "DSTSZ=%%~zB"
if not "!SRCSZ!"=="!DSTSZ!" (
    echo *** DEPLOY MISMATCH: root exe size !DSTSZ! != build !SRCSZ! -- stale/partial copy.
    goto :fail
)

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
