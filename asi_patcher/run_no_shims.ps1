# Launch TD5 with the AppCompat shim engine disabled entirely.
# __COMPAT_LAYER=RunAsInvoker is the documented escape hatch that overrides
# all per-EXE shim registry entries for the process we spawn.
$env:__COMPAT_LAYER = "RunAsInvoker"
Push-Location 'C:\Users\maria\Desktop\Proyectos\TD5RE\original'
Start-Process -FilePath '.\TD5_d3d.exe' -UseNewEnvironment:$false
Pop-Location
Write-Host "launched (shims disabled via __COMPAT_LAYER=RunAsInvoker)"
