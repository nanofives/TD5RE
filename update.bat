@echo off
REM TD5RE auto-updater launcher. Double-click this to sync the game from the LAN server.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0td5re_update.ps1" %*
echo.
pause
