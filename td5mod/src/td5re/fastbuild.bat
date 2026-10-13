@echo off
setlocal enabledelayedexpansion

REM === TD5RE fast incremental build ===
REM Recompiles ONLY the modules you name, reuses every other .o already in the
REM build dir, then archives + links the FULL srcs.txt module list. ~9 s for a
REM one-module edit versus ~2.5 min for a full build_standalone.bat.
REM
REM Usage:
REM   fastbuild.bat td5_tg_city.c                   -> DEV, one module
REM   fastbuild.bat td5_geo.c td5_tg_road.c         -> DEV, several modules
REM   fastbuild.bat release td5_tg_city.c           -> RELEASE build_release\
REM   set FASTSRCS=a.c b.c  &&  fastbuild.bat       -> legacy env form
REM
REM NO HEADER DEPENDENCY TRACKING. After editing any .h, run the full
REM build_standalone.bat -- a fast build would silently keep .o files compiled
REM against the OLD header and link a mixed exe. Same rule after changing
REM srcs.txt or cflags.txt.
REM
REM This file deliberately contains NO toolchain, flag, module-list or link
REM logic: it sets TD5RE_FASTSRCS and calls build_standalone.bat, which resolves
REM srcs.txt / cflags.txt / link_libs.txt exactly as it always does. That is why
REM it cannot drift from the real build the way a forked copy of the script did
REM (_archive\round1024\fastbuild_r1023.bat was such a copy).

cd /d "%~dp0"

set "VARIANT="
set "LIST="

:argloop
if "%~1"=="" goto :argdone
if /I "%~1"=="release" (
    set "VARIANT=release"
) else (
    set "LIST=!LIST! %~1"
)
shift
goto :argloop
:argdone

REM Legacy form used by the round-1024 briefs: FASTSRCS in the environment.
if not defined LIST if defined FASTSRCS set "LIST=%FASTSRCS%"

if not defined LIST (
    echo ERROR: no modules given.
    echo   usage: fastbuild.bat [release] ^<module.c^> [module.c ...]
    echo   or:    set FASTSRCS=a.c b.c ^&^& fastbuild.bat
    exit /b 1
)

REM Catch a typo'd / moved module here rather than inside the compile loop.
for %%F in (!LIST!) do (
    if not exist "%%F" (
        echo ERROR: no such module in %~dp0 : %%F
        exit /b 1
    )
)

set "TD5RE_FASTSRCS=!LIST!"
call "%~dp0build_standalone.bat" %VARIANT%
exit /b %errorlevel%
