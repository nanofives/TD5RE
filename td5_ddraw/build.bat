@echo off
setlocal
set HERE=%~dp0
set GCC=%HERE%..\td5mod\deps\mingw\mingw32\bin\gcc.exe

if not exist "%HERE%src\ddraw.def" (
    echo [build] generating ddraw.def from SysWOW64\ddraw.dll...
    python "%HERE%generate_def.py" || exit /b 1
)
if not exist "%HERE%build" mkdir "%HERE%build"

echo [build] compiling td5_ddraw.dll...
"%GCC%" -m32 -shared -O2 -Wall -DCINTERFACE ^
    -o "%HERE%build\ddraw.dll" ^
    "%HERE%src\ddraw_proxy.c" ^
    "%HERE%src\idd_wrap.c" ^
    "%HERE%src\ddraw.def" ^
    -Wl,--enable-stdcall-fixup ^
    -lkernel32 -luser32 -lgdi32 -lole32 -ldxguid -luuid
if errorlevel 1 (
    echo [build] FAILED
    exit /b 1
)

echo.
echo [build] output: %HERE%build\ddraw.dll
echo Run install.bat to copy into original\.
endlocal
