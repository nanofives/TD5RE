@echo off
setlocal
REM TD6 compatibility stubs — satisfy WMAUDSDK.DLL's dead 1999 import chain
REM (DRMClien.DLL, strmdll.dll) so TD6.exe can load on modern Windows.
REM wmaudsdk_stub builds to wmaudsdk_stub.dll and is NOT deployed by default;
REM rename over the real WMAUDSDK.DLL only if the loader stubs are not enough.

cd /d "%~dp0"
set GCC=..\deps\mingw\mingw32\bin\gcc.exe
set OUTDIR=build
set GAMEDIR=..\..\Test Drive 6

if not exist %OUTDIR% mkdir %OUTDIR%

echo Building DRMClien.DLL stub...
"%GCC%" -shared -static -O2 -Wall -o %OUTDIR%\DRMClien.DLL drmclien_stub.c drmclien.def -nostdlib -lkernel32 -Wl,--entry,0 -Wl,--enable-stdcall-fixup
if errorlevel 1 goto :fail

echo Building strmdll.dll stub...
"%GCC%" -shared -static -O2 -Wall -o %OUTDIR%\strmdll.dll strmdll_stub.c strmdll.def -nostdlib -lkernel32 -Wl,--entry,0 -Wl,--enable-stdcall-fixup
if errorlevel 1 goto :fail

echo Building wmaudsdk_stub.dll (fallback, not deployed)...
"%GCC%" -shared -static -O2 -Wall -o %OUTDIR%\wmaudsdk_stub.dll wmaudsdk_stub.c wmaudsdk.def -nostdlib -lkernel32 -Wl,--entry,0 -Wl,--enable-stdcall-fixup
if errorlevel 1 goto :fail

echo Deploying loader stubs to "%GAMEDIR%"...
copy /Y %OUTDIR%\DRMClien.DLL "%GAMEDIR%\DRMClien.DLL" >nul
copy /Y %OUTDIR%\strmdll.dll "%GAMEDIR%\strmdll.dll" >nul

echo === BUILD OK ===
goto :done

:fail
echo === BUILD FAILED ===
exit /b 1

:done
endlocal
