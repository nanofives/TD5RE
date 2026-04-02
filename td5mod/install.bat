@echo off
echo === TD5 Mod Install ===
echo.

set GAME_DIR=%~dp0..

REM Check if ASI loader exists
if not exist "%GAME_DIR%\dinput.dll" (
    echo WARNING: dinput.dll (ASI Loader) not found in game directory
    echo Download from: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases
    echo Get the x86 dinput.dll version and place it in: %GAME_DIR%
    echo.
)

REM Create scripts directory
if not exist "%GAME_DIR%\scripts" mkdir "%GAME_DIR%\scripts"

REM Copy ASI and INI
if exist "build\scripts\td5_mod.asi" (
    copy /y "build\scripts\td5_mod.asi" "%GAME_DIR%\scripts\"
    echo Copied td5_mod.asi
) else (
    echo ERROR: build\scripts\td5_mod.asi not found. Run build.bat first.
    pause
    exit /b 1
)

if not exist "%GAME_DIR%\scripts\td5_mod.ini" (
    copy /y "dist\scripts\td5_mod.ini" "%GAME_DIR%\scripts\"
    echo Copied td5_mod.ini
) else (
    echo td5_mod.ini already exists, skipping (edit manually if needed)
)

echo.
echo === Install complete! ===
echo Launch TD5_d3d.exe normally to use the mod.
echo Edit scripts\td5_mod.ini to configure features.
echo.
pause
