@echo off
setlocal enabledelayedexpansion

REM Change to script directory so relative paths work
cd /d "%~dp0"

REM [2026-06-04] Toolchain is at td5mod\deps\mingw. From this script's dir
REM (td5mod\ddraw_wrapper) that is ONE level up, not two — the old ..\..\deps
REM pointed at a nonexistent TD5RE\deps and broke every wrapper rebuild.
REM ---------------------------------------------------------------------------
REM Architecture selection (x64 Stage 3) -- mirrors build_standalone.bat exactly:
REM chosen by the TD5RE_ARCH env var, output dir arch-suffixed so the 64-bit
REM archive can never overwrite the shipping 32-bit libddraw_wrapper.a.
REM
REM   set TD5RE_ARCH=x86_64  &  build.bat   -> build_x64\libddraw_wrapper.a
REM   (unset)                               -> build\libddraw_wrapper.a
REM
REM wrapper_cflags.txt is already arch-NEUTRAL (it carries no -m32; the i686
REM toolchain defaults to 32-bit and the Makefile/CI append -m32 themselves), so
REM only the toolchain and output dir change here.
REM ---------------------------------------------------------------------------
set ARCH=i686
if /I "%TD5RE_ARCH%"=="x86_64" set ARCH=x86_64
if /I "%TD5RE_ARCH%"=="x64" set ARCH=x86_64

if "%ARCH%"=="x86_64" goto :arch_x64
set TOOLPREFIX=..\deps\mingw\mingw32\bin
set ARCHCFLAG=-m32
set BUILDSUFFIX=
goto :arch_done
:arch_x64
set TOOLPREFIX=..\deps\mingw64\mingw64\bin
set ARCHCFLAG=-m64
set BUILDSUFFIX=_x64
:arch_done

set GCC=%TOOLPREFIX%\gcc.exe
set AR=%TOOLPREFIX%\ar.exe
set SRCDIR=src
set OUTDIR=build%BUILDSUFFIX%

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
REM Pin the target explicitly rather than relying on the toolchain default, so a
REM mis-set TOOLPREFIX fails loudly instead of silently producing the wrong arch.
set "WRAPPER_CFLAGS=!WRAPPER_CFLAGS! %ARCHCFLAG%"

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
