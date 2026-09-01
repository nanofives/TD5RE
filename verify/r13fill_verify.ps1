# [R13 FILL] verification harness. Generates the pinned auto-track once with a
# chosen TD5RE_R13_FILL value, then reports the element inventory, the guard
# reject tally, the r13fill exposed-rear dump and the level-file hashes.
#
#   pwsh -File r13fill_verify.ps1 -Fill 0 -Tag off
#   pwsh -File r13fill_verify.ps1 -Fill 1 -Tag on
param([string]$Fill = "1", [string]$Tag = "on", [int]$MaxWait = 240,
      [int]$Span = 1677, [int]$Pad = 40, [int]$Cam = 1677,
      [string]$Rear = "1")

$wt = Split-Path -Parent $MyInvocation.MyCommand.Path

# TD5RE_* vars persist across launches inside one PowerShell session.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_AUTOTRACK_SEED  = "20260901"
$env:TD5RE_R13_FILL        = $Fill
$env:TD5RE_R13_FILL_REAR   = $Rear
$env:TD5RE_R13_FILL_REPORT = "1"
$env:TD5RE_R13_FILL_SPAN   = "$Span"
$env:TD5RE_R13_FILL_PAD    = "$Pad"
$env:TD5RE_R13_FILL_CAM    = "$Cam"
$env:TD5RE_WINDOW_TITLE    = "TD5RE R13 FILL verify $Tag"

# level090 is CACHED: it only regenerates when the directory is absent.
$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
Remove-Item (Join-Path $wt "log\race.log*") -Force -ErrorAction SilentlyContinue

Set-Location $wt
$p = Start-Process -FilePath "$wt\td5re.exe" -WorkingDirectory $wt -PassThru `
     -ArgumentList "--AutoRace=1","--SkipIntro=1"
Write-Output "PID=$($p.Id) fill=$Fill tag=$Tag"

# Send to the back without activating, so a verification run never steals focus.
$api = @'
using System;using System.Runtime.InteropServices;
public class R13W {
  [DllImport("user32.dll")] public static extern int PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
'@
if (-not ("R13W" -as [type])) { Add-Type -TypeDefinition $api }
for ($i=0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500; $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
        [R13W]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null
        break
    }
}

# Track generation runs at level load; poll for the generated MODELS.DAT rather
# than sleeping a fixed time (the track needs ~110 s to build).
$models = Join-Path $lvl "MODELS.DAT"
$t0 = Get-Date
while (-not (Test-Path $models) -and ((Get-Date) - $t0).TotalSeconds -lt $MaxWait) {
    Start-Sleep -Seconds 2
    $p.Refresh(); if ($p.HasExited) { break }
}
Write-Output ("generated={0} after {1:N0}s" -f (Test-Path $models), ((Get-Date)-$t0).TotalSeconds)
Start-Sleep -Seconds 20   # let the level finish loading before asking it to quit

# race.log only flushes on CLEAN shutdown.
$p.Refresh()
if (-not $p.HasExited) {
    try { [R13W]::PostMessage($p.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null } catch {}
}
for ($i=0; $i -lt 60 -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1; $p.Refresh() }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }   # ONLY our PID

$dst = Join-Path $wt "r13fill_verify_$Tag.log"
Copy-Item (Join-Path $wt "log\race.log") $dst -Force
foreach ($f in @("MODELS.DAT","STRIP.DAT","LEVELINF.DAT")) {
    $q = Join-Path $lvl $f
    if (Test-Path $q) {
        $sha = [System.BitConverter]::ToString(
            [System.Security.Cryptography.SHA256]::Create().ComputeHash(
                [System.IO.File]::ReadAllBytes($q))).Replace("-","").Substring(0,16)
        Write-Output ("{0} {1} size={2} sha={3}" -f $Tag, $f, (Get-Item $q).Length, $sha)
    }
}
Write-Output "log=$dst"
