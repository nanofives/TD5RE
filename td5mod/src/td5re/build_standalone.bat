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
REM Architecture selection (x64 Stage 3)
REM
REM Selected by the TD5RE_ARCH environment variable, NOT an argument: this
REM script's contract is "any unrecognised first argument means dev" (the /fix
REM and /end skills pass a PID), so an arch argument could not be told apart
REM from that and would silently mis-build.
REM
REM   set TD5RE_ARCH=x86_64  &  build_standalone.bat      -> 64-bit
REM   (unset, or anything else)                           -> 32-bit, the default
REM
REM i686 remains the SHIPPING target. The 64-bit path exists because DXR is
REM unavailable to 32-bit processes (measured; see docs/plans/X64_DXR_ROADMAP.md).
REM Object dirs are arch-suffixed so the two never share a stale .o cache --
REM the same reason DEV and RELEASE are already separated.
REM ---------------------------------------------------------------------------
set ARCH=i686
if /I "%TD5RE_ARCH%"=="x86_64" set ARCH=x86_64
if /I "%TD5RE_ARCH%"=="x64" set ARCH=x86_64

if "%ARCH%"=="x86_64" goto :arch_x64

REM --- i686 (default, shipping) ---
set TOOLPREFIX=..\..\deps\mingw\mingw32\bin
set ZLIB_INC=..\..\deps\mingw\mingw32\i686-w64-mingw32\include
set WINDRES_TARGET=pe-i386
set ARCHLDFLAG=-m32
set BUILDSUFFIX=
goto :arch_done

:arch_x64
REM --- x86_64 (retarget) ---
REM No ZLIB_INC: this build has no zlib dependency (see cflags_x86_64.txt), but
REM the -I is harmless and kept pointing at the i686 headers because zlib's two
REM headers are arch-neutral -- so a build that DOES opt back into zlib still
REM finds them.
set TOOLPREFIX=..\..\deps\mingw64\mingw64\bin
set ZLIB_INC=..\..\deps\mingw\mingw32\i686-w64-mingw32\include
set WINDRES_TARGET=pe-x86-64
set ARCHLDFLAG=-m64
set BUILDSUFFIX=_x64

:arch_done
set GCC=%TOOLPREFIX%\gcc.exe
set AR=%TOOLPREFIX%\ar.exe
set WINDRES=%TOOLPREFIX%\windres.exe
set SRCDIR=.
set WRAPPER_SRCDIR=..\..\ddraw_wrapper\src
REM Arch-matched wrapper archive: ddraw_wrapper\build.bat uses the SAME suffix.
REM Linking the i686 .a into a 64-bit exe fails with a bare "cannot find
REM -lddraw_wrapper", which reads like a missing build rather than an arch
REM mismatch -- so the paths are kept deliberately parallel.
set WRAPPER_BUILDDIR=..\..\ddraw_wrapper\build%BUILDSUFFIX%
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
REM Only the path-dependent -I include dirs are appended here.
set "CFLAGS_COMMON="
for /f "usebackq eol=# delims=" %%L in ("%~dp0cflags.txt") do set "CFLAGS_COMMON=!CFLAGS_COMMON! %%L"
if not defined CFLAGS_COMMON (
    echo ERROR: cflags.txt missing or empty at %~dp0cflags.txt
    exit /b 1
)
REM Arch-specific flags (-m32/-m64 and friends) live in cflags_<arch>.txt and
REM are read IN ADDITION to the arch-neutral cflags.txt above. Missing file =
REM hard error: silently building for the wrong architecture is the exact
REM failure this split exists to prevent.
if not exist "%~dp0cflags_%ARCH%.txt" (
    echo ERROR: cflags_%ARCH%.txt missing at %~dp0cflags_%ARCH%.txt
    exit /b 1
)
set "CFLAGS_ARCH="
for /f "usebackq eol=# delims=" %%L in ("%~dp0cflags_%ARCH%.txt") do set "CFLAGS_ARCH=!CFLAGS_ARCH! %%L"
if not defined CFLAGS_ARCH (
    echo ERROR: cflags_%ARCH%.txt contains no flags
    exit /b 1
)
set CFLAGS_BASE=!CFLAGS_COMMON! !CFLAGS_ARCH! -I%SRCDIR% -I%WRAPPER_SRCDIR% -I%ZLIB_INC%

REM System link libraries -- SINGLE SOURCE OF TRUTH: link_libs.txt (shared with
REM build.yml, release.yml and td5mod\Makefile so the lib list cannot drift).
set "LINK_LIBS="
for /f "usebackq eol=# delims=" %%L in ("%~dp0link_libs.txt") do set "LINK_LIBS=!LINK_LIBS! %%L"
if not defined LINK_LIBS (
    echo ERROR: link_libs.txt missing or empty at %~dp0link_libs.txt
    exit /b 1
)
REM Arch-specific libs. Unlike cflags_<arch>.txt this may legitimately be EMPTY
REM (x86_64 needs no extra libs), so only the FILE is required, not its content.
if not exist "%~dp0link_libs_%ARCH%.txt" (
    echo ERROR: link_libs_%ARCH%.txt missing at %~dp0link_libs_%ARCH%.txt
    exit /b 1
)
for /f "usebackq eol=# delims=" %%L in ("%~dp0link_libs_%ARCH%.txt") do set "LINK_LIBS=!LINK_LIBS! %%L"

REM Per-variant configuration (goto-based, NOT parenthesized blocks, so comments
REM containing parentheses cannot corrupt the batch parser).
if /I "%VARIANT%"=="release" goto :cfg_release

REM --- DEV: full debug affordances ---
set BUILDDIR=build!BUILDSUFFIX!
REM Arch suffix on the EXE too, not just the object dir: the x64 build must not
REM overwrite the shipping 32-bit td5re.exe in the project root. The suffix is
REM empty for i686, so the shipping name is unchanged.
set EXE=td5re!BUILDSUFFIX!.exe
set MAPFILE=td5re!BUILDSUFFIX!.map
set TD5RE_SRCS=!TD5RE_SRCS_COMMON!
set CFLAGS=!CFLAGS_BASE!
set EXTRA_LDFLAGS=
goto :cfg_done

:cfg_release
REM --- RELEASE: define TD5RE_RELEASE so dev affordances (trace knobs, debug
REM     overlays, net selftest) compile out / hard-disable, then strip the
REM     symbol table. ---
set BUILDDIR=build_release!BUILDSUFFIX!
set EXE=td5re_release!BUILDSUFFIX!.exe
set MAPFILE=td5re_release!BUILDSUFFIX!.map
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
    -L%WRAPPER_BUILDDIR% -lddraw_wrapper ^
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
