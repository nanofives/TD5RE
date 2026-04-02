@echo off
setlocal

REM Change to script directory so relative paths work
cd /d "%~dp0"

set GCC=..\..\deps\mingw\mingw32\bin\gcc.exe
set SRCDIR=src
set OUTDIR=build
set DEF=ddraw.def
set OUT=ddraw.dll

echo === D3D11 Wrapper Build ===

if not exist %OUTDIR% mkdir %OUTDIR%

REM --- Compile HLSL shaders ---
echo Compiling HLSL shaders...
pushd "%~dp0src\shaders"
call "%~dp0src\shaders\compile_shaders.bat"
popd
if errorlevel 1 goto :fail

echo Compiling ddraw_main.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\ddraw_main.c   -o %OUTDIR%\ddraw_main.o
if errorlevel 1 goto :fail

echo Compiling ddraw4.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\ddraw4.c       -o %OUTDIR%\ddraw4.o
if errorlevel 1 goto :fail

echo Compiling surface4.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\surface4.c     -o %OUTDIR%\surface4.o
if errorlevel 1 goto :fail

echo Compiling d3d3.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\d3d3.c         -o %OUTDIR%\d3d3.o
if errorlevel 1 goto :fail

echo Compiling device3.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\device3.c      -o %OUTDIR%\device3.o
if errorlevel 1 goto :fail

echo Compiling viewport3.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\viewport3.c    -o %OUTDIR%\viewport3.o
if errorlevel 1 goto :fail

echo Compiling texture2.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\texture2.c     -o %OUTDIR%\texture2.o
if errorlevel 1 goto :fail

echo Compiling d3d11_backend.c...
"%GCC%" -c -O2 -Wall -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\d3d11_backend.c -o %OUTDIR%\d3d11_backend.o
if errorlevel 1 goto :fail

echo Compiling png_loader.c...
"%GCC%" -c -O2 -Wall -Wno-unused-function -DWIN32 -DWRAPPER_DEBUG %SRCDIR%\png_loader.c -o %OUTDIR%\png_loader.o
if errorlevel 1 goto :fail

echo Linking %OUT%...
"%GCC%" -shared -static -o %OUTDIR%\%OUT% ^
    %OUTDIR%\ddraw_main.o ^
    %OUTDIR%\ddraw4.o ^
    %OUTDIR%\surface4.o ^
    %OUTDIR%\d3d3.o ^
    %OUTDIR%\device3.o ^
    %OUTDIR%\viewport3.o ^
    %OUTDIR%\texture2.o ^
    %OUTDIR%\d3d11_backend.o ^
    %OUTDIR%\png_loader.o ^
    %DEF% ^
    -ld3d11 -ldxgi -lkernel32 -luser32 -lgdi32 -luuid -ldxguid ^
    -Wl,--enable-stdcall-fixup
if errorlevel 1 goto :fail

echo Deploying to game directory...
copy /Y %OUTDIR%\%OUT% ..\..\%OUT% >nul

echo.
echo === BUILD OK: %OUTDIR%\%OUT% (deployed) ===
goto :done

:fail
echo.
echo === BUILD FAILED ===
exit /b 1

:done
endlocal
