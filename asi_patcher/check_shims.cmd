@echo off
set KEY=Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
set TARGET=C:\Users\maria\Desktop\Proyectos\TD5RE\original\TD5_d3d.exe
echo HKCU:
reg query "HKCU\%KEY%" /v "%TARGET%" 2>&1
echo.
echo HKLM:
reg query "HKLM\%KEY%" /v "%TARGET%" 2>&1
