@echo off
setlocal

echo === TD5 Mod Framework Build ===
echo.

REM Set up MinGW from local deps
set MINGW_DIR=%~dp0deps\mingw\mingw32\bin
set PATH=%MINGW_DIR%;%PATH%
set MINHOOK_DIR=%~dp0deps\minhook-1.3.4

REM Verify compiler
where gcc >nul 2>&1
if errorlevel 1 (
    echo ERROR: gcc not found. Run setup first or check deps\mingw\
    pause
    exit /b 1
)

echo [1/3] Compiling MinHook...
if not exist build mkdir build

gcc -c -O2 -Wall -DWIN32 -I"%MINHOOK_DIR%\include" -I"%MINHOOK_DIR%\src" "%MINHOOK_DIR%\src\hook.c" -o build\hook.o
if errorlevel 1 goto fail
gcc -c -O2 -Wall -DWIN32 -I"%MINHOOK_DIR%\include" -I"%MINHOOK_DIR%\src" "%MINHOOK_DIR%\src\buffer.c" -o build\buffer.o
if errorlevel 1 goto fail
gcc -c -O2 -Wall -DWIN32 -I"%MINHOOK_DIR%\include" -I"%MINHOOK_DIR%\src" "%MINHOOK_DIR%\src\trampoline.c" -o build\trampoline.o
if errorlevel 1 goto fail
gcc -c -O2 -Wall -DWIN32 -I"%MINHOOK_DIR%\include" -I"%MINHOOK_DIR%\src\hde" "%MINHOOK_DIR%\src\hde\hde32.c" -o build\hde32.o

echo [2/5] Compiling td5_mod...
gcc -c -O2 -Wall -DWIN32 ^
    -I"%MINHOOK_DIR%\include" ^
    -Isrc ^
    src\td5_mod.c ^
    -o build\td5_mod.o

if errorlevel 1 goto fail

echo [3/5] Compiling td5_tuning...
gcc -c -O2 -Wall -DWIN32 ^
    -I"%MINHOOK_DIR%\include" ^
    -Isrc ^
    src\td5_tuning.c ^
    -o build\td5_tuning.o

if errorlevel 1 goto fail

echo [4/6] Compiling td5_asset_dump...
gcc -c -O2 -Wall -DWIN32 ^
    -Isrc ^
    src\td5_asset_dump.c ^
    -o build\td5_asset_dump.o

if errorlevel 1 goto fail

echo [5/6] Compiling td5_png_replace...
gcc -c -O2 -Wall -Wno-unused-function -DWIN32 ^
    -Isrc ^
    -I.\ddraw_wrapper\src ^
    src\td5_png_replace.c ^
    -o build\td5_png_replace.o

if errorlevel 1 goto fail

echo [6/6] Linking td5_mod.asi...
if not exist build\scripts mkdir build\scripts
gcc -shared -static -o build\scripts\td5_mod.asi ^
    build\td5_mod.o build\td5_tuning.o build\td5_asset_dump.o build\td5_png_replace.o build\hook.o build\buffer.o build\trampoline.o build\hde32.o ^
    -lkernel32 -luser32 -lgdi32 -s

if errorlevel 1 goto fail

echo.
echo === Build successful! ===
for %%F in (build\scripts\td5_mod.asi) do echo Output: %%~fF (%%~zF bytes)
echo.
echo Run install.bat to copy to game directory.
echo.
pause
exit /b 0

:fail
echo.
echo === BUILD FAILED ===
pause
exit /b 1
