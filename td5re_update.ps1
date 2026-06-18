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

    If the .local name does not resolve on this machine, pass the Pi's LAN IP:

        powershell -ExecutionPolicy Bypass -File td5re_update.ps1 -BaseUrl http://192.168.0.50:8088

    Add -Prune to also delete local files under re\assets that no longer exist
    on the server (keeps the install an exact mirror).
#>
[CmdletBinding()]
param(
    [string]$BaseUrl     = "http://mariano-server.local:8088",
    [string]$InstallRoot = $PSScriptRoot,
    [switch]$Prune
)

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # makes Invoke-WebRequest much faster

if (-not $InstallRoot) { $InstallRoot = (Get-Location).Path }
$BaseUrl = $BaseUrl.TrimEnd('/')

function Url-For([string]$rel) {
    # URL-encode each path segment (handles spaces / odd chars) but keep the slashes.
    $enc = ($rel -split '/' | ForEach-Object { [uri]::EscapeDataString($_) }) -join '/'
    return "$BaseUrl/$enc"
}

Write-Host ""
Write-Host "TD5RE updater" -ForegroundColor Cyan
Write-Host "  server : $BaseUrl"
Write-Host "  target : $InstallRoot"

# --- 1. fetch manifest --------------------------------------------------------
# Download to a file and parse with explicit UTF-8 decoding: robust against a
# BOM or a missing charset on the server (Invoke-RestMethod silently returns the
# raw string in those cases, which would look like a 1-entry manifest).
$manifestUrl = "$BaseUrl/manifest.json"
$tmpManifest = Join-Path ([System.IO.Path]::GetTempPath()) ("td5re_manifest_" + [guid]::NewGuid().ToString("N") + ".json")
try {
    Invoke-WebRequest -Uri $manifestUrl -OutFile $tmpManifest -UseBasicParsing -TimeoutSec 30
    $manifest = (Get-Content -LiteralPath $tmpManifest -Raw -Encoding UTF8) | ConvertFrom-Json
} catch {
    Write-Host "ERROR: cannot fetch/parse $manifestUrl" -ForegroundColor Red
    Write-Host "       $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "Tip: if '.local' does not resolve here, re-run with the Pi's IP, e.g." -ForegroundColor Yellow
    Write-Host "     powershell -ExecutionPolicy Bypass -File td5re_update.ps1 -BaseUrl http://192.168.0.50:8088" -ForegroundColor Yellow
    exit 1
} finally {
    if (Test-Path -LiteralPath $tmpManifest) { Remove-Item -LiteralPath $tmpManifest -Force -ErrorAction SilentlyContinue }
}

$files = @($manifest.files)
if ($files.Count -lt 1 -or -not $files[0].path) {
    Write-Host "ERROR: manifest at $manifestUrl looks invalid (parsed $($files.Count) entries)." -ForegroundColor Red
    exit 1
}
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
