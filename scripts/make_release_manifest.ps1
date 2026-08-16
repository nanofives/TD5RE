<#
.SYNOPSIS
    Generate manifest.json for the TD5RE release (dev machine, run before publishing).

.DESCRIPTION
    Walks the playable release set and writes a manifest of {path, size, sha256}:
        td5re_release.exe, td5re_release.ini, td5re_update.ps1, update.bat
        td5re.exe, td5re.ini.default   (DEV build — see note below)
        pending_to_test.csv   (copied here from td5mod/src/td5re/, the tracked
                               source of truth, so release machines get the list)
        re/assets/**  (everything, recursive)

    Deliberately EXCLUDES per-machine state so the updater never clobbers it:
        td5re_input.ini, td5re_progress.ini, and the live dev ini (td5re.ini).

    [2026-08-16] The DEV exe is now published alongside the release one, so a LAN
    machine can run the debug affordances the release build compiles out (trace
    knobs, F12 collision overlay, selftest, the live-control socket). Its LIVE ini
    stays excluded — that is per-machine state; we ship td5re.ini.default instead
    so a fresh machine has a template without its own config being overwritten.

.USAGE
    powershell -ExecutionPolicy Bypass -File tools\make_release_manifest.ps1
#>
[CmdletBinding()]
param(
    [string]$Root,
    [string]$OutFile
)
$ErrorActionPreference = 'Stop'
if (-not $Root) {
    $here = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $Root = (Resolve-Path (Join-Path $here "..")).Path
}
if (-not $OutFile) { $OutFile = Join-Path $Root "manifest.json" }
$rootFull = (Resolve-Path $Root).Path.TrimEnd('\')

$topLevel = @('td5re_release.exe','td5re_release.ini','td5re_update.ps1','update.bat','pending_to_test.csv','td5re.exe','td5re.ini.default')
$entries  = New-Object System.Collections.Generic.List[object]

# Stage the pending-to-test checklist next to the exe from its tracked source of
# truth (td5mod/src/td5re/pending_to_test.csv) so it ships in the manifest. The
# root copy is a publish artifact (gitignored); the DEV runtime prefers the
# source-tree copy, so this never hijacks dev reads/writes.
$pendingSrc = Join-Path $Root 'td5mod\src\td5re\pending_to_test.csv'
$pendingDst = Join-Path $Root 'pending_to_test.csv'
if (Test-Path -LiteralPath $pendingSrc) {
    Copy-Item -LiteralPath $pendingSrc -Destination $pendingDst -Force
} else {
    Write-Warning "missing tracked pending_to_test.csv at $pendingSrc"
}

# [2026-08-15] Seed the shipping td5re_release.ini from its tracked template.
# The live ini is gitignored runtime state (main.c persists options back into
# it), so on a fresh clone / CI checkout it does not exist yet -- but it IS a
# top-level release artifact above, and packaging must not depend on someone
# having launched the release exe first. Seed only when missing, so a curated
# local config is never overwritten during a publish.
$relIniSrc = Join-Path $Root 'td5re_release.ini.default'
$relIniDst = Join-Path $Root 'td5re_release.ini'
if (-not (Test-Path -LiteralPath $relIniDst)) {
    if (Test-Path -LiteralPath $relIniSrc) {
        Copy-Item -LiteralPath $relIniSrc -Destination $relIniDst
        Write-Host "seeded td5re_release.ini from td5re_release.ini.default"
    } else {
        Write-Warning "missing both td5re_release.ini and its .default template at $Root"
    }
}

# SHA256 via .NET rather than Get-FileHash: this box's Windows PowerShell 5.1
# ships a partially-broken Microsoft.PowerShell.Utility that is missing
# Get-FileHash (and Import-PowerShellDataFile) — other Utility cmdlets work, but
# those two are absent, so `Get-FileHash` fails with CommandNotFoundException and
# the whole publish aborts. [System.Security.Cryptography.SHA256] is present on
# every PowerShell (5.1 and 7), so this is portable and dependency-free.
function Get-Sha256Hex([string]$full) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $fs = [System.IO.File]::OpenRead($full)
        try { $bytes = $sha.ComputeHash($fs) } finally { $fs.Dispose() }
    } finally { $sha.Dispose() }
    -join ($bytes | ForEach-Object { $_.ToString('x2') })
}

function Add-Entry([string]$full, [string]$rel) {
    $h   = Get-Sha256Hex $full
    $len = (Get-Item -LiteralPath $full).Length
    $entries.Add([ordered]@{ path = ($rel -replace '\\','/'); size = $len; sha256 = $h })
}

foreach ($t in $topLevel) {
    $p = Join-Path $Root $t
    if (Test-Path -LiteralPath $p) { Add-Entry $p $t } else { Write-Warning "missing: $t" }
}

$assetsRoot = Join-Path $Root 're\assets'
if (Test-Path -LiteralPath $assetsRoot) {
    Get-ChildItem -LiteralPath $assetsRoot -Recurse -File | ForEach-Object {
        Add-Entry $_.FullName ($_.FullName.Substring($rootFull.Length + 1))
    }
} else {
    Write-Warning "missing re\assets directory"
}

$manifest = [ordered]@{
    generated = (Get-Date).ToString('o')
    count     = $entries.Count
    files     = $entries
}
$json = $manifest | ConvertTo-Json -Depth 5
# Write UTF-8 WITHOUT a BOM. Windows PowerShell 5.1's `Set-Content -Encoding UTF8`
# prepends a BOM, which makes ConvertFrom-Json fail on the client.
[System.IO.File]::WriteAllText($OutFile, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("Wrote {0} ({1} files)" -f $OutFile, $entries.Count) -ForegroundColor Green
