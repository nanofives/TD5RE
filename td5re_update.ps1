<#
.SYNOPSIS
    TD5RE LAN auto-updater.

.DESCRIPTION
    Downloads manifest.json from the release server, compares the SHA-256 of every
    file already on this machine, and downloads ONLY the files that are missing or
    have changed. Safe to run repeatedly.

    It never touches per-machine state:
        td5re_input.ini      (controller / input config)
        td5re_progress.ini   (career / save progress)
    Those are not in the manifest, so they are left exactly as they are.

.USAGE
    Double-click  update.bat   (recommended), or from PowerShell:

        powershell -ExecutionPolicy Bypass -File td5re_update.ps1

    The updater auto-falls-back to the Pi's static IP when the '.local' name does
    not resolve. This is what makes it work across a cascaded network (a second
    router hung off the main one): mDNS / '.local' multicast cannot cross a router
    boundary, but the Pi's IP is still reachable. To force a specific server, pass
    it explicitly (also accepts several, tried in order):

        powershell -ExecutionPolicy Bypass -File td5re_update.ps1 -BaseUrl http://192.168.68.112:8088

    Add -Prune to also delete local files under re\assets that no longer exist
    on the server (keeps the install an exact mirror).
#>
[CmdletBinding()]
param(
    [string[]]$BaseUrl,
    [string]$InstallRoot = $PSScriptRoot,
    [switch]$Prune
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # makes Invoke-WebRequest much faster

if (-not $InstallRoot) { $InstallRoot = (Get-Location).Path }

# Candidate servers, tried in order until one serves a valid manifest.
#   * the '.local' name works on a flat LAN (single subnet, mDNS resolves)
#   * the static IP is the fallback for a CASCADED network (a 2nd router hung off
#     the main one): mDNS multicast can't cross a router boundary, so '.local'
#     fails there, but the Pi's IP is still routable.
# An explicit -BaseUrl (one or more) overrides this list entirely.
if (-not $BaseUrl -or $BaseUrl.Count -eq 0) {
    $BaseUrl = @(
        'http://mariano-server.local:8088',
        'http://192.168.68.112:8088'
    )
}
$BaseUrl = @($BaseUrl | ForEach-Object { $_.TrimEnd('/') })

function Url-For([string]$rel) {
    # URL-encode each path segment (handles spaces / odd chars) but keep the slashes.
    $enc = ($rel -split '/' | ForEach-Object { [uri]::EscapeDataString($_) }) -join '/'
    return "$script:ResolvedBase/$enc"
}

Write-Host ""
Write-Host "TD5RE updater" -ForegroundColor Cyan
Write-Host ("  servers: {0}" -f ($BaseUrl -join ', '))
Write-Host "  target : $InstallRoot"

# --- 1. fetch manifest --------------------------------------------------------
# Try each candidate base URL until one serves a valid manifest. Download to a
# file and parse with explicit UTF-8 decoding: robust against a BOM or a missing
# charset on the server (Invoke-RestMethod silently returns the raw string in
# those cases, which would look like a 1-entry manifest). The first base that
# works becomes $script:ResolvedBase, so the file downloads below hit the same
# server we found the manifest on.
$script:ResolvedBase = $null
$manifest    = $null
$tmpManifest = Join-Path ([System.IO.Path]::GetTempPath()) ("td5re_manifest_" + [guid]::NewGuid().ToString("N") + ".json")
foreach ($base in $BaseUrl) {
    $manifestUrl = "$base/manifest.json"
    try {
        Invoke-WebRequest -Uri $manifestUrl -OutFile $tmpManifest -UseBasicParsing -TimeoutSec 30
        $candidate = (Get-Content -LiteralPath $tmpManifest -Raw -Encoding UTF8) | ConvertFrom-Json
        if (@($candidate.files).Count -ge 1 -and $candidate.files[0].path) {
            $script:ResolvedBase = $base
            $manifest = $candidate
            Write-Host ("  server : {0}" -f $base) -ForegroundColor Green
            break
        }
        Write-Host ("  ! {0} returned an invalid manifest, trying next ..." -f $manifestUrl) -ForegroundColor DarkYellow
    } catch {
        Write-Host ("  ! {0} unreachable ({1}), trying next ..." -f $manifestUrl, $_.Exception.Message) -ForegroundColor DarkYellow
    }
}
if (Test-Path -LiteralPath $tmpManifest) { Remove-Item -LiteralPath $tmpManifest -Force -ErrorAction SilentlyContinue }

if (-not $manifest) {
    Write-Host "ERROR: no server served a valid manifest. Tried:" -ForegroundColor Red
    foreach ($base in $BaseUrl) { Write-Host "       $base/manifest.json" -ForegroundColor Red }
    Write-Host "Tip: pass the Pi's current IP explicitly, e.g." -ForegroundColor Yellow
    Write-Host "     powershell -ExecutionPolicy Bypass -File td5re_update.ps1 -BaseUrl http://192.168.68.112:8088" -ForegroundColor Yellow
    exit 1
}

$files = @($manifest.files)
Write-Host ("  manifest: {0} files (generated {1})" -f $files.Count, $manifest.generated)

# --- 2. decide what to download ----------------------------------------------
$toGet = New-Object System.Collections.Generic.List[object]
$upToDate = 0
foreach ($f in $files) {
    $local = Join-Path $InstallRoot ($f.path -replace '/','\')
    $need  = $true
    if (Test-Path -LiteralPath $local) {
        $item = Get-Item -LiteralPath $local
        if ($item.Length -eq [int64]$f.size) {
            $h = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
            if ($h -ieq $f.sha256) { $need = $false }
        }
    }
    if ($need) { $toGet.Add($f) } else { $upToDate++ }
}

Write-Host ("  up-to-date: {0}   need update: {1}" -f $upToDate, $toGet.Count) -ForegroundColor Green

# --- 3. download changed / missing files -------------------------------------
$done = 0; $failed = 0; $i = 0
foreach ($f in $toGet) {
    $i++
    $local = Join-Path $InstallRoot ($f.path -replace '/','\')
    $dir   = Split-Path -Parent $local
    if ($dir -and -not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $tmp = "$local.dl_tmp"
    Write-Host ("  [{0}/{1}] {2}" -f $i, $toGet.Count, $f.path)
    try {
        Invoke-WebRequest -Uri (Url-For $f.path) -OutFile $tmp -UseBasicParsing -TimeoutSec 300
        $h = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
        if ($h -ine $f.sha256) {
            Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
            Write-Host "      ! hash mismatch after download, skipped" -ForegroundColor Red
            $failed++
            continue
        }
        Move-Item -LiteralPath $tmp -Destination $local -Force
        $done++
    } catch {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        Write-Host "      ! download failed: $($_.Exception.Message)" -ForegroundColor Red
        $failed++
    }
}

# --- 4. optional prune (mirror) ----------------------------------------------
$pruned = 0
if ($Prune) {
    $keep = New-Object System.Collections.Generic.HashSet[string]
    foreach ($f in $files) {
        if ($f.path -like 're/assets/*') {
            [void]$keep.Add( (Join-Path $InstallRoot ($f.path -replace '/','\')).ToLower() )
        }
    }
    $assetsDir = Join-Path $InstallRoot 're\assets'
    if (Test-Path -LiteralPath $assetsDir) {
        Get-ChildItem -LiteralPath $assetsDir -Recurse -File | ForEach-Object {
            if (-not $keep.Contains($_.FullName.ToLower())) {
                Remove-Item -LiteralPath $_.FullName -Force
                Write-Host ("  - pruned {0}" -f $_.FullName.Substring($InstallRoot.Length+1)) -ForegroundColor DarkYellow
                $pruned++
            }
        }
    }
}

# --- 5. summary ---------------------------------------------------------------
Write-Host ""
Write-Host ("Done. updated {0}, up-to-date {1}, failed {2}, pruned {3}." -f $done, $upToDate, $failed, $pruned) -ForegroundColor Cyan
if ($failed -gt 0) { exit 1 }
exit 0
