@echo off
setlocal enabledelayedexpansion

REM Change to script directory so relative paths work
cd /d "%~dp0"

REM [2026-06-04] Toolchain is at td5mod\deps. From this script's dir
REM (td5mod\ddraw_wrapper) that is ONE level up, not two — the old ..\..\deps
REM pointed at a nonexistent TD5RE\deps and broke every wrapper rebuild.
REM
REM x86_64 ONLY since the i686 retirement (2026-07-30) -- mirrors
REM build_standalone.bat: one arch, plain output dir, -m64 lives in
REM wrapper_cflags.txt alongside the other shared flags.
if /I "%TD5RE_ARCH%"=="i686" (
    echo ERROR: i686 was retired 2026-07-30 -- toolchain parked in _archive\.
    exit /b 1
)
set TOOLPREFIX=..\deps\mingw64\mingw64\bin

set GCC=%TOOLPREFIX%\gcc.exe
set AR=%TOOLPREFIX%\ar.exe
set SRCDIR=src
set OUTDIR=build

REM [D3D12 port P0.6] Backend selector. Arg 1 (or TD5RE_BACKEND) picks the GPU
REM backend: d3d11 (default) or d3d12. The backend is baked into the archive
REM filename (libddraw_wrapper_<backend>.a) so a stale copy fails to LINK, not
REM to run. The d3d11 path is UNCHANGED (also still emits the plain
REM libddraw_wrapper.a that build_standalone.bat + CI link by default), so this
REM does not touch the default build or the CI workflow. For d3d12 the shared
REM COM files (wrapper_srcs.txt) are compiled MINUS the d3d11_backend_*.c files,
REM PLUS wrapper_srcs_d3d12.txt.
set "BACKEND=%~1"
if "%BACKEND%"=="" set "BACKEND=%TD5RE_BACKEND%"
if "%BACKEND%"=="" set "BACKEND=d3d11"
if /I not "%BACKEND%"=="d3d11" if /I not "%BACKEND%"=="d3d12" (
    echo ERROR: unknown backend "%BACKEND%" ^(expected d3d11 or d3d12^)
    goto :fail
)

echo === Wrapper Build [backend=%BACKEND%] ===

if not exist %OUTDIR% mkdir %OUTDIR%

REM --- Compile HLSL shaders ---
echo Compiling HLSL shaders...
pushd "%~dp0src\shaders"
call "%~dp0src\shaders\compile_shaders.bat"
popd
if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Module list + flags -- SINGLE SOURCE OF TRUTH: wrapper_srcs.txt /
REM wrapper_cflags.txt (one entry per line, # comments; same pattern as
REM src/td5re/srcs.txt + cflags.txt). td5mod\Makefile, build.yml and
REM release.yml read the SAME files, so this list/flag set cannot drift.
REM png_loader.c additionally needs -Wno-unused-function (kept here, not in
REM the shared flags file, since it's a single-file exception).
REM ---------------------------------------------------------------------------
set "WRAPPER_SRCS="
for /f "usebackq eol=# delims=" %%L in ("%~dp0wrapper_srcs.txt") do (
    set "SL=%%L"
    REM For the d3d12 build, drop the d3d11 backend engine files (d3d11_backend_*.c).
    if /I "%BACKEND%"=="d3d12" if "!SL:~0,13!"=="d3d11_backend" set "SL="
    if defined SL set "WRAPPER_SRCS=!WRAPPER_SRCS! !SL!"
)
REM Append the d3d12 backend engine files for the d3d12 build.
if /I "%BACKEND%"=="d3d12" (
    if not exist "%~dp0wrapper_srcs_d3d12.txt" (
        echo ERROR: d3d12 backend requires wrapper_srcs_d3d12.txt ^(not yet created^)
        goto :fail
    )
    for /f "usebackq eol=# delims=" %%L in ("%~dp0wrapper_srcs_d3d12.txt") do set "WRAPPER_SRCS=!WRAPPER_SRCS! %%L"
)
if not defined WRAPPER_SRCS (
    echo ERROR: wrapper_srcs.txt missing or empty at %~dp0wrapper_srcs.txt
    goto :fail
)

set "WRAPPER_CFLAGS="
for /f "usebackq eol=# delims=" %%L in ("%~dp0wrapper_cflags.txt") do set "WRAPPER_CFLAGS=!WRAPPER_CFLAGS! %%L"
if not defined WRAPPER_CFLAGS (
    echo ERROR: wrapper_cflags.txt missing or empty at %~dp0wrapper_cflags.txt
    goto :fail
)
set ARCHIVE_OBJS=
for %%F in (!WRAPPER_SRCS!) do (
    echo Compiling %%F...
    if /I "%%~nF"=="png_loader" (
        "%GCC%" !WRAPPER_CFLAGS! -Wno-unused-function %SRCDIR%\%%F -o %OUTDIR%\%%~nF.o
    ) else (
        "%GCC%" !WRAPPER_CFLAGS! %SRCDIR%\%%F -o %OUTDIR%\%%~nF.o
    )
    if errorlevel 1 goto :fail
    set "ARCHIVE_OBJS=!ARCHIVE_OBJS! %OUTDIR%\%%~nF.o"
)

REM --- Static archive for the source-port link ---
REM [2026-06-04] build_standalone.bat links the wrapper as a PREBUILT static lib
REM (-L build -lddraw_wrapper); it does NOT compile the wrapper itself. So this
REM .a MUST be (re)produced here whenever the wrapper objects change, or td5re's
REM link fails with "undefined reference" (e.g. S01's Backend_SetExclusiveFullscreen).
echo Creating libddraw_wrapper_%BACKEND%.a...
if exist %OUTDIR%\libddraw_wrapper_%BACKEND%.a del %OUTDIR%\libddraw_wrapper_%BACKEND%.a
"%AR%" rcs %OUTDIR%\libddraw_wrapper_%BACKEND%.a !ARCHIVE_OBJS!
if errorlevel 1 goto :fail
REM Default (d3d11) also emits the plain libddraw_wrapper.a that the default
REM build_standalone.bat + CI link path expects -- keeps them unchanged.
if /I "%BACKEND%"=="d3d11" copy /y %OUTDIR%\libddraw_wrapper_d3d11.a %OUTDIR%\libddraw_wrapper.a >nul

echo.
echo === BUILD OK: %OUTDIR%\libddraw_wrapper_%BACKEND%.a ===
goto :done

:fail
echo.
echo === BUILD FAILED ===
endlocal
exit /b 1

:done
endlocal
