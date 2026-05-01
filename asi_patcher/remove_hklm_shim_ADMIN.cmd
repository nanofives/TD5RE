@echo off
REM Right-click -> Run as administrator.
REM Removes the HKLM AppCompat Layer assigned to original\TD5_d3d.exe.
REM Backup is written next to this script.

setlocal
set TARGET=C:\Users\maria\Desktop\Proyectos\TD5RE\original\TD5_d3d.exe
set KEY=Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
set BACKUP=%~dp0hklm_shim_backup.txt

net session >nul 2>&1
if errorlevel 1 (
    echo ERROR: Not running as administrator.
    echo Right-click this file and choose "Run as administrator".
    pause
    exit /b 1
)

echo === BEFORE === > "%BACKUP%"
reg query "HKLM\%KEY%" /v "%TARGET%" >> "%BACKUP%" 2>&1
echo. >> "%BACKUP%"

echo === DELETING === >> "%BACKUP%"
reg delete "HKLM\%KEY%" /v "%TARGET%" /f >> "%BACKUP%" 2>&1

echo === AFTER === >> "%BACKUP%"
reg query "HKLM\%KEY%" /v "%TARGET%" >> "%BACKUP%" 2>&1

type "%BACKUP%"
echo.
echo Done. Backup at %BACKUP%.
pause
endlocal
