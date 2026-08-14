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

REM --- Keep the D3D11 wrapper static lib fresh -------------------------------
REM build_standalone links a PREBUILT -lddraw_wrapper and never compiles the
REM wrapper itself. If ddraw_wrapper\src changed since libddraw_wrapper.a was
REM built, td5re's link fails with "undefined reference" (e.g. S01's
REM Backend_SetExclusiveFullscreen). Rebuild the wrapper when its source is newer
REM than the lib (or the lib is missing) so it can't go stale. [2026-06-04]
REM ABSOLUTE path (via %~dp0) so build.bat's own %~dp0 resolves correctly when we
REM call it — a RELATIVE call leaves build.bat's later %~dp0 wrong after its cd.
set WRAPDIR=%~dp0..\..\ddraw_wrapper
powershell -NoProfile -File "%~dp0..\..\..\scripts\wrapper_stale_check.ps1" -WrapperDir "%WRAPDIR%"
if errorlevel 1 goto :rebuild_wrapper
goto :wrapper_ok
:rebuild_wrapper
echo Wrapper lib stale or missing -- rebuilding ddraw_wrapper...
call "%WRAPDIR%\build.bat"
if errorlevel 1 goto :wrapper_failed
cd /d "%~dp0"
goto :wrapper_ok
:wrapper_failed
echo.
echo build_all: WRAPPER build FAILED.
exit /b 1
:wrapper_ok
echo.

REM Tee the DEV compile diagnostics so the structure lint can ratchet the warning
REM count LOCALLY, not just in CI. DEV (not RELEASE) because the CI job that owns
REM the baseline builds dev — RELEASE's extra -DTD5RE_RELEASE compiles modules out
REM and would undercount. Cleared before the release build so the log stays
REM single-variant. [2026-08-14]
set "TD5RE_BUILD_LOG=%~dp0..\..\..\build_warnings.log"
break > "%TD5RE_BUILD_LOG%"

call "%~dp0build_standalone.bat" dev
if errorlevel 1 (
    echo.
    echo build_all: DEV build FAILED -- stopping before release build.
    exit /b 1
)

set "TD5RE_BUILD_LOG="

echo.
call "%~dp0build_standalone.bat" release
if errorlevel 1 (
    echo.
    echo build_all: RELEASE build FAILED.
    exit /b 1
)

REM --- Code map refresh (navigation indexes; see CLAUDE.md "Fast navigation")
REM codemap\*.tsv is gitignored and regenerated here so it never goes stale.
REM Runs BEFORE the lint so a lint failure still leaves the indexes fresh.
where python >nul 2>&1
if not errorlevel 1 (
    python "%~dp0..\..\..\scripts\gen_codemap.py"
)

REM --- Structure lint (FAILS the build on regressions, same as CI) ----------
REM Ratchets: extern-in-.c, td5_game.h includer set, per-flag warning counts vs
REM the DEV build log tee'd above. See scripts/lint_structure.ps1 +
REM scripts/lint_structure_baseline.json.
REM
REM [2026-08-14] Was -ReportOnly AND passed no -BuildLog, so the warning ratchet
REM was never even evaluated locally — a regression was invisible until a push,
REM and CI stayed red for 10 days (84 -> 93) before anyone noticed. It now fails
REM here, where the fix is cheap. Set TD5RE_LINT_SOFT=1 to downgrade to a warning
REM for mid-experiment builds you do not intend to commit.
set LINTRC=0
where pwsh >nul 2>&1
if not errorlevel 1 (
    echo.
    if defined TD5RE_LINT_SOFT (
        pwsh -NoProfile -File "%~dp0..\..\..\scripts\lint_structure.ps1" -ReportOnly -BuildLog "%~dp0..\..\..\build_warnings.log"
    ) else (
        pwsh -NoProfile -File "%~dp0..\..\..\scripts\lint_structure.ps1" -BuildLog "%~dp0..\..\..\build_warnings.log"
        if errorlevel 1 set LINTRC=1
    )
)
if "%LINTRC%"=="1" goto :lint_failed

echo.
echo ############################################################
echo #  build_all OK
echo #  td5re.exe + td5re_release.exe deployed to project root
echo ############################################################
endlocal
exit /b 0

:lint_failed
echo.
echo ############################################################
echo #  build_all: STRUCTURE LINT FAILED
echo #  The exes built and deployed, but the ratchet regressed --
echo #  see the STRUCTURE REGRESSIONS lines above. CI will reject
echo #  this. Fix it, or re-baseline if the change is intentional:
echo #    pwsh scripts\lint_structure.ps1 -UpdateBaseline -BuildLog build_warnings.log
echo #  Override for a throwaway build: set TD5RE_LINT_SOFT=1
echo ############################################################
endlocal
exit /b 1
