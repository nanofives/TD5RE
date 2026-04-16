@echo off
setlocal enabledelayedexpansion

REM === TD5RE Standalone Build ===
REM Compiles all td5re modules + wrapper backend into td5re.exe
REM and copies the result to the project root.

cd /d "%~dp0"

set GCC=..\..\deps\mingw\mingw32\bin\gcc.exe
set AR=..\..\deps\mingw\mingw32\bin\ar.exe
set WINDRES=..\..\deps\mingw\mingw32\bin\windres.exe
set SRCDIR=.
set WRAPPER_SRCDIR=..\..\ddraw_wrapper\src
set WRAPPER_BUILDDIR=..\..\ddraw_wrapper\build
set ZLIB_INC=..\..\deps\mingw\mingw32\i686-w64-mingw32\include
set BUILDDIR=build
set PROJECT_ROOT=..\..\..

REM Verify compiler
"%GCC%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: gcc not found at %GCC%
    pause
    exit /b 1
)

if not exist %BUILDDIR% mkdir %BUILDDIR%

REM ---------------------------------------------------------------------------
REM Compile td5re modules
REM ---------------------------------------------------------------------------

set CFLAGS=-c -O2 -Wall -Wextra -Wpedantic -DWIN32 -m32 -I%SRCDIR% -I%WRAPPER_SRCDIR% -I%ZLIB_INC% -DTD5_INFLATE_USE_ZLIB
set TD5RE_SRCS=td5re.c td5_game.c td5_physics.c td5_track.c td5_ai.c td5_render.c td5_frontend.c td5_hud.c td5_sound.c td5_input.c td5_asset.c td5_inflate.c td5_save.c td5_net.c td5_camera.c td5_vfx.c td5_fmv.c td5_trace.c td5_platform_win32.c main.c

echo === TD5RE Standalone Build ===
echo.

set FAIL=0
for %%F in (%TD5RE_SRCS%) do (
    echo Compiling %%~nF.c...
    "%GCC%" %CFLAGS% %SRCDIR%\%%F -o %BUILDDIR%\%%~nF.o
    if errorlevel 1 (
        echo FAILED: %%F
        set FAIL=1
        goto :check_fail
    )
)

:check_fail
if %FAIL%==1 goto :fail

REM ---------------------------------------------------------------------------
REM Build static archive from all td5re .o files (excluding main.o)
REM ---------------------------------------------------------------------------

REM ---------------------------------------------------------------------------
REM Compile resource file (icon)
REM ---------------------------------------------------------------------------

echo Compiling td5re.rc...
"%WINDRES%" -F pe-i386 -i %SRCDIR%\td5re.rc -o %BUILDDIR%\td5re_res.o
if errorlevel 1 (
    echo WARNING: windres failed, icon will not be embedded
    set "RESOBJ="
) else (
    set "RESOBJ=%BUILDDIR%\td5re_res.o"
)

echo Creating libtd5re.a...
set ARCHIVE_OBJS=
for %%F in (%TD5RE_SRCS%) do (
    if /I NOT "%%~nF"=="main" (
        set "ARCHIVE_OBJS=!ARCHIVE_OBJS! %BUILDDIR%\%%~nF.o"
    )
)
"%AR%" rcs %BUILDDIR%\libtd5re.a %ARCHIVE_OBJS%
if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Link td5re.exe
REM
REM We link main.o + libtd5re.a + libddraw_wrapper.a + system libs.
REM Using --whole-archive for libtd5re.a ensures ALL modules are included,
REM preventing the static-library symbol pruning problem.
REM ---------------------------------------------------------------------------

echo Linking td5re.exe...
"%GCC%" -m32 -mwindows -static -o %BUILDDIR%\td5re.exe ^
    %BUILDDIR%\main.o %RESOBJ% ^
    -L%BUILDDIR% -Wl,--whole-archive -ltd5re -Wl,--no-whole-archive ^
    -L%WRAPPER_BUILDDIR% -lddraw_wrapper ^
    -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -lole32 ^
    -lwinmm -ldinput8 -ldsound -ldxguid -lz -lws2_32 -lmfplat -lmfreadwrite -lmfuuid -lole32 ^
    -Wl,-Map=%BUILDDIR%\td5re.map ^
    -Wl,--enable-stdcall-fixup ^
    -Wl,--allow-multiple-definition

if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Deploy to project root
REM ---------------------------------------------------------------------------

echo Deploying to project root...
copy /Y %BUILDDIR%\td5re.exe %PROJECT_ROOT%\td5re.exe >nul
if errorlevel 1 goto :fail

REM ---------------------------------------------------------------------------
REM Verify build: check that key instrumentation strings are present
REM ---------------------------------------------------------------------------

echo.
echo === Verifying build artifact ===
..\..\deps\mingw\mingw32\bin\strings.exe %PROJECT_ROOT%\td5re.exe | findstr "append=" >nul 2>&1
if errorlevel 1 (
    echo WARNING: td5re.exe does NOT contain expected instrumentation strings
    echo The build may be stale or the linker may have stripped the code.
) else (
    echo OK: td5re.exe contains expected instrumentation strings
)

echo.
for %%F in (%PROJECT_ROOT%\td5re.exe) do echo === BUILD OK: %%~fF (%%~zF bytes) ===
goto :done

:fail
echo.
echo === BUILD FAILED ===
pause
exit /b 1

:done
endlocal
