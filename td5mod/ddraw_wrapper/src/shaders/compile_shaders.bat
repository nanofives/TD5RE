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

REM --- G-buffer MRT variants (lighting rework P0) ---
%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_modulate_g_bytes.h /Vn g_ps_modulate_g ps_modulate_g.hlsl
if errorlevel 1 (echo FAILED: ps_modulate_g && exit /b 1)
echo   ps_modulate_g OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_modulate_alpha_g_bytes.h /Vn g_ps_modulate_alpha_g ps_modulate_alpha_g.hlsl
if errorlevel 1 (echo FAILED: ps_modulate_alpha_g && exit /b 1)
echo   ps_modulate_alpha_g OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_decal_bytes.h /Vn g_ps_decal ps_decal.hlsl
if errorlevel 1 (echo FAILED: ps_decal && exit /b 1)
echo   ps_decal OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_luminance_alpha_bytes.h /Vn g_ps_luminance_alpha ps_luminance_alpha.hlsl
if errorlevel 1 (echo FAILED: ps_luminance_alpha && exit /b 1)
echo   ps_luminance_alpha OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_composite_bytes.h /Vn g_ps_composite ps_composite.hlsl
if errorlevel 1 (echo FAILED: ps_composite && exit /b 1)
echo   ps_composite OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_light_bytes.h /Vn g_ps_light ps_light.hlsl
if errorlevel 1 (echo FAILED: ps_light && exit /b 1)
echo   ps_light OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_msdf_bytes.h /Vn g_ps_msdf ps_msdf.hlsl
if errorlevel 1 (echo FAILED: ps_msdf && exit /b 1)
echo   ps_msdf OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_roundrect_bytes.h /Vn g_ps_roundrect ps_roundrect.hlsl
if errorlevel 1 (echo FAILED: ps_roundrect && exit /b 1)
echo   ps_roundrect OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_arrow_bytes.h /Vn g_ps_arrow ps_arrow.hlsl
if errorlevel 1 (echo FAILED: ps_arrow && exit /b 1)
echo   ps_arrow OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_cursor_bytes.h /Vn g_ps_cursor ps_cursor.hlsl
if errorlevel 1 (echo FAILED: ps_cursor && exit /b 1)
echo   ps_cursor OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_gauge_bytes.h /Vn g_ps_gauge ps_gauge.hlsl
if errorlevel 1 (echo FAILED: ps_gauge && exit /b 1)
echo   ps_gauge OK

REM --- Procedural texture-free particle/VFX shaders (smoke, rain, decal, glow) ---
%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_fx_smoke_bytes.h /Vn g_ps_fx_smoke ps_fx_smoke.hlsl
if errorlevel 1 (echo FAILED: ps_fx_smoke && exit /b 1)
echo   ps_fx_smoke OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_fx_rain_bytes.h /Vn g_ps_fx_rain ps_fx_rain.hlsl
if errorlevel 1 (echo FAILED: ps_fx_rain && exit /b 1)
echo   ps_fx_rain OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_fx_decal_bytes.h /Vn g_ps_fx_decal ps_fx_decal.hlsl
if errorlevel 1 (echo FAILED: ps_fx_decal && exit /b 1)
echo   ps_fx_decal OK

%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_fx_glow_bytes.h /Vn g_ps_fx_glow ps_fx_glow.hlsl
if errorlevel 1 (echo FAILED: ps_fx_glow && exit /b 1)
echo   ps_fx_glow OK

echo All shaders compiled successfully.
