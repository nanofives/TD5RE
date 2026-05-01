@echo off
setlocal
set HERE=%~dp0
set DEST=%HERE%..\original

if not exist "%HERE%build\winmm.dll" (
    echo [install] winmm.dll not built. Run build.bat first.
    exit /b 1
)
if not exist "%HERE%build\td5_windowed.asi" (
    echo [install] td5_windowed.asi not built. Run build.bat first.
    exit /b 1
)
if not exist "%DEST%\TD5_d3d.exe" (
    echo [install] %DEST%\TD5_d3d.exe not found.
    exit /b 1
)

copy /Y "%HERE%build\winmm.dll" "%DEST%\winmm.dll" >nul || exit /b 1
copy /Y "%HERE%build\td5_windowed.asi" "%DEST%\td5_windowed.asi" >nul || exit /b 1
copy /Y "C:\Windows\SysWOW64\winmm.dll" "%DEST%\winmm_real.dll" >nul || exit /b 1

echo [install] installed to %DEST%\:
echo   winmm.dll          (ASI proxy)
echo   winmm_real.dll     (forwarder target = system winmm)
echo   td5_windowed.asi   (patcher plugin)
echo.
echo Launch original\TD5_d3d.exe normally.
echo Patch log: original\td5_windowed.log
endlocal
