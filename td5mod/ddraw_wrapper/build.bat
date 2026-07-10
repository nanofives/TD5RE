@echo off
setlocal enabledelayedexpansion

REM Change to script directory so relative paths work
cd /d "%~dp0"

REM [2026-06-04] Toolchain is at td5mod\deps\mingw. From this script's dir
REM (td5mod\ddraw_wrapper) that is ONE level up, not two — the old ..\..\deps
REM pointed at a nonexistent TD5RE\deps and broke every wrapper rebuild.
set GCC=..\deps\mingw\mingw32\bin\gcc.exe
set AR=..\deps\mingw\mingw32\bin\ar.exe
set SRCDIR=src
set OUTDIR=build

echo === D3D11 Wrapper Build ===

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
for /f "usebackq eol=# delims=" %%L in ("%~dp0wrapper_srcs.txt") do set "WRAPPER_SRCS=!WRAPPER_SRCS! %%L"
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
echo Creating libddraw_wrapper.a...
if exist %OUTDIR%\libddraw_wrapper.a del %OUTDIR%\libddraw_wrapper.a
"%AR%" rcs %OUTDIR%\libddraw_wrapper.a !ARCHIVE_OBJS!
if errorlevel 1 goto :fail

echo.
echo === BUILD OK: %OUTDIR%\libddraw_wrapper.a ===
goto :done

:fail
echo.
echo === BUILD FAILED ===
endlocal
exit /b 1

:done
endlocal
