# [R14 GENPERF] Generation wall-clock probe.
#
# Times GENERATION, not the process: t0 is the moment tg build_level creates
# re/assets/levels/level090 (_mkdir, first thing it does), t1 is the moment
# MODELS.DAT stops growing. App init, asset mount and window creation all land
# before t0, so the number is the generator's own cost and is comparable across
# builds that differ in start-up work.
#
# Nothing here waits for the race. A sibling round burned ~10 min per run
# polling for a RACE state that never arrived under this harness config.
#
#   -Exe    which binary to run (staged copies: r14genperf_base.exe = this
#           tree, r14genperf_r12.exe = the r12-integration build)
#   -Extra  knob hashtable, applied AFTER every TD5RE_* var is cleared
param([string]$Seed = "20260901", [hashtable]$Extra = @{}, [string]$Tag = "run",
      [string]$Exe = "r14genperf_base.exe", [int]$GenWait = 900,
      [int]$Port = 37163, [switch]$KeepLevel)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# TD5RE_* vars PERSIST across launches in one PowerShell session; an "isolated"
# A/B silently inherits every knob an earlier run set. Clear before EVERY launch.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED = $Seed
$env:TD5RE_CONTROL_PORT   = "$Port"
$env:TD5RE_WINDOW_TITLE   = "TD5RE R14 genperf $Tag seed $Seed"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

# level090 is CACHED -- it only regenerates when the DIRECTORY IS ABSENT.
$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$log = Join-Path $wt "log\race.log"
for ($t = 0; $t -lt 20 -and (Test-Path $log); $t++) {
    try { Remove-Item $log -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
}

$models = Join-Path $lvl "MODELS.DAT"
$sw = [Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList "--AutoRace=1","--SkipIntro=1","--Control=1" `
      -WorkingDirectory $wt -PassThru

# t0: the generator has entered build_level (it _mkdir's the level dir first).
$t0 = -1.0
while ($sw.Elapsed.TotalSeconds -lt $GenWait -and -not $p.HasExited) {
    if (Test-Path $lvl) { $t0 = $sw.Elapsed.TotalSeconds; break }
    Start-Sleep -Milliseconds 100
}
# t1: MODELS.DAT has stopped growing.
$t1 = -1.0; $last = -1; $stable = 0; $bytes = 0
while ($sw.Elapsed.TotalSeconds -lt $GenWait -and -not $p.HasExited) {
    Start-Sleep -Milliseconds 200
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) {
            $stable++
            if ($stable -eq 1) { $t1 = $sw.Elapsed.TotalSeconds }
            if ($stable -ge 5) { $bytes = $len; break }
        } else { $stable = 0; $t1 = -1.0 }
        $last = $len
    }
}
$gen = if ($t0 -ge 0 -and $t1 -ge 0) { [math]::Round($t1 - $t0, 1) } else { -1 }

# Clean shutdown so race.log flushes; kill ONLY our own PID as a fallback.
if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
Copy-Item $log (Join-Path $wt "log\race_r14genperf_$Tag.log") -Force -ErrorAction SilentlyContinue
Write-Host ("### {0}  exe={1}  seed={2}  GEN={3}s  (init={4}s)" -f `
            $Tag, $Exe, $Seed, $gen, [math]::Round($t0, 1))
foreach ($f in @("MODELS.DAT","STRIP.DAT","LEVELINF.DAT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0, 16)
        Write-Host ("  {0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $p2).Length, $h)
    }
}
if (-not $KeepLevel) { }
