@echo off
setlocal
set HERE=%~dp0
set DEST=%HERE%..\original

if not exist "%HERE%build\ddraw.dll" (
    echo [install] build\ddraw.dll missing. Run build.bat first.
    exit /b 1
)
if not exist "%DEST%\TD5_d3d.exe" (
    echo [install] %DEST%\TD5_d3d.exe not found.
    exit /b 1
)

REM Move aside any pre-existing ddraw.dll that isn't ours (e.g. DDrawCompat).
if exist "%DEST%\ddraw.dll" (
    findstr /m "td5_ddraw" "%DEST%\ddraw.dll" >nul 2>&1
    if errorlevel 1 (
        if not exist "%DEST%\ddraw.dll.prev" (
            move "%DEST%\ddraw.dll" "%DEST%\ddraw.dll.prev" >nul
            echo [install] saved prior ddraw.dll as ddraw.dll.prev
        ) else (
            del "%DEST%\ddraw.dll" >nul
        )
    )
)

copy /Y "%HERE%build\ddraw.dll" "%DEST%\ddraw.dll" >nul || exit /b 1
copy /Y "C:\Windows\SysWOW64\ddraw.dll" "%DEST%\ddraw_real.dll" >nul || exit /b 1

echo [install] installed:
echo   %DEST%\ddraw.dll       (td5_ddraw proxy)
echo   %DEST%\ddraw_real.dll  (forwarder target = system ddraw)
echo Log: %DEST%\td5_ddraw.log
endlocal
