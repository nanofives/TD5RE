@echo off
setlocal
set HERE=%~dp0
set GCC=%HERE%..\td5mod\deps\mingw\mingw32\bin\gcc.exe

if not exist "%HERE%src\winmm.def" (
    echo [build] generating winmm.def from SysWOW64\winmm.dll...
    python "%HERE%generate_winmm_def.py" || exit /b 1
)

if not exist "%HERE%build" mkdir "%HERE%build"

echo [build] compiling winmm proxy...
"%GCC%" -m32 -shared -O2 -Wall ^
    -o "%HERE%build\winmm.dll" ^
    "%HERE%src\winmm_proxy.c" "%HERE%src\winmm.def" ^
    -Wl,--enable-stdcall-fixup ^
    -lkernel32
if errorlevel 1 (
    echo [build] winmm proxy FAILED
    exit /b 1
)

echo [build] compiling td5_windowed.asi...
"%GCC%" -m32 -shared -O2 -Wall ^
    -o "%HERE%build\td5_windowed.asi" ^
    "%HERE%src\td5_windowed.c" ^
    -lkernel32
if errorlevel 1 (
    echo [build] td5_windowed.asi FAILED
    exit /b 1
)

echo.
echo [build] outputs:
echo   %HERE%build\winmm.dll
echo   %HERE%build\td5_windowed.asi
echo.
echo Run install.bat to copy them (plus winmm_real.dll) into original\.
endlocal
