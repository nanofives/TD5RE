# wrapper_stale_check.ps1
#
# Exit 0 if td5mod/ddraw_wrapper's prebuilt libddraw_wrapper.a is up to date,
# exit 1 if it's missing or older than any wrapper source file (.c/.h/.hlsl).
# Extracted from build_all.bat's inline one-liner [2026-06-04] for readability;
# same check, unchanged behavior.
#
# Usage: pwsh -NoProfile -File scripts\wrapper_stale_check.ps1 -WrapperDir <path>

param(
    [Parameter(Mandatory = $true)]
    [string]$WrapperDir
)

$lib = Join-Path $WrapperDir 'build\libddraw_wrapper.a'
if (-not (Test-Path $lib)) {
    exit 1
}

$newestSource = Get-ChildItem (Join-Path $WrapperDir 'src') -Recurse -Include *.c, *.h, *.hlsl -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1

if ($newestSource -and $newestSource.LastWriteTime -gt (Get-Item $lib).LastWriteTime) {
    exit 1
}

exit 0
