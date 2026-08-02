@echo off
setlocal

set FXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x86\fxc.exe"
set OPTS=/nologo /O2

REM DXR shader compiler (DXIL). The x64 dxc from the same Win10 SDK; ships
REM dxil.dll alongside so lib_6_3 blobs are signed (unsigned DXIL is rejected by
REM CreateStateObject). If you relocate the SDK, drop a dxc release into
REM ..\..\tools\dxc\ and repoint DXC here.
set DXC="C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\dxc.exe"

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

REM --- Screen-space ray-marched sun shadows (lighting rework P2) ---
%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_shadow_bytes.h /Vn g_ps_shadow ps_shadow.hlsl
if errorlevel 1 (echo FAILED: ps_shadow && exit /b 1)
echo   ps_shadow OK

REM --- Screen-space reflections (lighting rework P3) ---
%FXC% %OPTS% /T ps_4_0 /E main /Fh ps_ssr_bytes.h /Vn g_ps_ssr ps_ssr.hlsl
if errorlevel 1 (echo FAILED: ps_ssr && exit /b 1)
echo   ps_ssr OK

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

REM ===========================================================================
REM [D3D12 port] SM 5.0 variants for the D3D12 backend (DXBC <= SM5.1; no DXC/
REM DXIL needed for raster parity). SEPARATE arrays (g_<name>_50 in
REM <name>_bytes_50.h) so the D3D11 SM4.0 shaders above stay byte-for-byte
REM untouched. Compiled from the SAME HLSL -- SM4->5 is a no-op transform
REM (verified: identical instruction counts). The D3D12 backend includes the
REM _50 headers; the D3D11 backend ignores them.
REM ===========================================================================
for %%S in (vs_pretransformed vs_fullscreen) do (
    %FXC% %OPTS% /T vs_5_0 /E main /Fh %%S_bytes_50.h /Vn g_%%S_50 %%S.hlsl
    if errorlevel 1 (echo FAILED: %%S ^(sm5^) && exit /b 1)
)
for %%S in (ps_modulate ps_modulate_alpha ps_modulate_g ps_modulate_alpha_g ps_modulate_shadowed ps_modulate_alpha_shadowed ps_decal ps_luminance_alpha ps_composite ps_light ps_shadow ps_ssr ps_shadow_rt ps_light_rt ps_ssr_rt ps_msdf ps_roundrect ps_arrow ps_cursor ps_gauge ps_fx_smoke ps_fx_rain ps_fx_decal ps_fx_glow) do (
    %FXC% %OPTS% /T ps_5_0 /E main /Fh %%S_bytes_50.h /Vn g_%%S_50 %%S.hlsl
    if errorlevel 1 (echo FAILED: %%S ^(sm5^) && exit /b 1)
)
echo   SM5.0 variants OK

REM ===========================================================================
REM [RT lighting] DXR shader library -> DXIL. ONE lib_6_3 blob containing every
REM ray-tracing entry point (rgen_smoke in Phase 0; rgen_debug/shadow/refl +
REM miss/chit/anyhit added by later phases), emitted as a BYTE array header
REM (g_rt_pipeline) that d3d12_dxr.c #includes. dxc requires <windows.h> types;
REM the header is pure data (no MinGW linking implications).
REM ===========================================================================
%DXC% -nologo -T lib_6_3 -Fh rt_pipeline_bytes.h -Vn g_rt_pipeline rt_pipeline.hlsl
if errorlevel 1 (echo FAILED: rt_pipeline ^(dxil lib_6_3^) && exit /b 1)
echo   rt_pipeline (DXIL lib_6_3) OK

echo All shaders compiled successfully.
