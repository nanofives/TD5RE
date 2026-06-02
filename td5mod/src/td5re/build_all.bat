@echo off
setlocal
cd /d "%~dp0"

REM === TD5RE build_all ===
REM Builds BOTH source-port executables and deploys them to the project root:
REM   td5re.exe          -> DEV     (full RE instrumentation, F12 overlay, trace harness)
REM   td5re_release.exe  -> RELEASE (instrumentation stripped, no dev affordances)
REM
REM The two binaries coexist so you can keep iterating with td5re.exe while
REM shipping/perf-checking td5re_release.exe. Separate object dirs (build\ vs
REM build_release\) mean the differing -D flags never share a stale .o cache.

echo ############################################################
echo #  TD5RE build_all
echo #  DEV (td5re.exe) + RELEASE (td5re_release.exe)
echo ############################################################
echo.

call "%~dp0build_standalone.bat" dev
if errorlevel 1 (
    echo.
    echo build_all: DEV build FAILED -- stopping before release build.
    exit /b 1
)

echo.
call "%~dp0build_standalone.bat" release
if errorlevel 1 (
    echo.
    echo build_all: RELEASE build FAILED.
    exit /b 1
)

echo.
echo ############################################################
echo #  build_all OK
echo #  td5re.exe + td5re_release.exe deployed to project root
echo ############################################################
endlocal
