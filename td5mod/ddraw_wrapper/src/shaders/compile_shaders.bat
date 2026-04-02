@echo off
setlocal

set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe"
set OPTS=/nologo /O2

echo Compiling shaders...

%FXC% %OPTS% /T vs_4_0 /E main /Fh vs_pretransformed_bytes.h /Vn g_vs_pretransformed vs_pretransformed.hlsl
if errorlevel 1 (echo FAILED: vs_pretransformed && exit /b 1)
echo   vs_pretransformed OK

%FXC% %OPTS% /T vs_4_0 /E main /Fh vs_fullscreen_bytes.h /Vn g_vs_fullscreen vs_fullscreen.hlsl
if errorlevel 1 (echo FAILED: vs_fullscreen && exit /b 1)
echo   vs_fullscreen OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_modulate_bytes.h /Vn g_ps_modulate ps_modulate.hlsl
if errorlevel 1 (echo FAILED: ps_modulate && exit /b 1)
echo   ps_modulate OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_modulate_alpha_bytes.h /Vn g_ps_modulate_alpha ps_modulate_alpha.hlsl
if errorlevel 1 (echo FAILED: ps_modulate_alpha && exit /b 1)
echo   ps_modulate_alpha OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_decal_bytes.h /Vn g_ps_decal ps_decal.hlsl
if errorlevel 1 (echo FAILED: ps_decal && exit /b 1)
echo   ps_decal OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_luminance_alpha_bytes.h /Vn g_ps_luminance_alpha ps_luminance_alpha.hlsl
if errorlevel 1 (echo FAILED: ps_luminance_alpha && exit /b 1)
echo   ps_luminance_alpha OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_composite_bytes.h /Vn g_ps_composite ps_composite.hlsl
if errorlevel 1 (echo FAILED: ps_composite && exit /b 1)
echo   ps_composite OK

echo All shaders compiled successfully.
