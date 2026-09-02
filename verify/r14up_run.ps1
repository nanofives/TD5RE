# [R14 OVERPASS item 3] Generate the auto-track once for one seed and copy out
# race.log, so the overpass-surround measurement is read from a log diff.
#
#   pwsh -File r14up_run.ps1 -Seed 20260901 -Tag base
param([string]$Seed = "20260901", [string]$Tag = "base",
      [int]$MaxWait = 400, [hashtable]$Knobs = @{})

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# TD5RE_* vars persist across launches inside one PowerShell session.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } |
    ForEach-Object { Remove-Item "env:$($_.Name)" }

$env:TD5RE_AUTOTRACK_SEED = $Seed
foreach ($k in $Knobs.Keys) { Set-Item "env:$k" $Knobs[$k] }
$env:TD5RE_WINDOW_TITLE = "TD5RE R14 UP $Tag"

# level090 is CACHED: it only regenerates when the directory is absent.
$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
Remove-Item (Join-Path $wt "log\race.log*") -Force -ErrorAction SilentlyContinue

Set-Location $wt
$p = Start-Process -FilePath "$wt\td5re.exe" -WorkingDirectory $wt -PassThru `
     -ArgumentList "--AutoRace=1","--SkipIntro=1"
Write-Output "PID=$($p.Id) seed=$Seed tag=$Tag"

$api = @'
using System;using System.Runtime.InteropServices;
public class R14W {
  [DllImport("user32.dll")] public static extern int PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int cx,int cy,uint f);
}
'@
if (-not ("R14W" -as [type])) { Add-Type -TypeDefinition $api }
for ($i=0; $i -lt 20; $i++) {
    Start-Sleep -Milliseconds 500; $p.Refresh()
    if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
        [R14W]::SetWindowPos($p.MainWindowHandle,[IntPtr]1,0,0,0,0,0x0013) | Out-Null
        break
    }
}

$models = Join-Path $lvl "MODELS.DAT"
$t0 = Get-Date
while (-not (Test-Path $models) -and ((Get-Date) - $t0).TotalSeconds -lt $MaxWait) {
    Start-Sleep -Seconds 2
    $p.Refresh(); if ($p.HasExited) { break }
}
Write-Output ("generated={0} after {1:N0}s" -f (Test-Path $models), ((Get-Date)-$t0).TotalSeconds)
Start-Sleep -Seconds 25

# race.log only flushes on CLEAN shutdown.
$p.Refresh()
if (-not $p.HasExited) {
    try { [R14W]::PostMessage($p.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null } catch {}
}
for ($i=0; $i -lt 90 -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1; $p.Refresh() }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }   # ONLY our PID

$dst = Join-Path $wt "r14up_$Tag.log"
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
