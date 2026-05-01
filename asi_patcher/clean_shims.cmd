@echo off
setlocal
set TARGET=C:\Users\maria\Desktop\Proyectos\TD5RE\original\TD5_d3d.exe
set KEY_LAYERS=Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers
set KEY_STORE=Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store
set KEY_PERSIST=Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Persisted

echo Before:
reg query "HKCU\%KEY_LAYERS%" /v "%TARGET%" 2>&1

echo Clearing HKCU Layers, Store, Persisted entries for %TARGET%:
reg delete "HKCU\%KEY_LAYERS%"  /v "%TARGET%" /f 2>&1
reg delete "HKCU\%KEY_STORE%"   /v "%TARGET%" /f 2>&1
reg delete "HKCU\%KEY_PERSIST%" /v "%TARGET%" /f 2>&1

echo After:
reg query "HKCU\%KEY_LAYERS%" /v "%TARGET%" 2>&1
endlocal
