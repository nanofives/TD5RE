# [R14 JUNCTION] FAST generator probe.
#
# The R13 probe polled the control socket until the race came up and only then
# quit. On this branch get_state never answered RACE (race_reached=False on
# every run), so each run burned the full 400-iteration poll -- ~10 minutes of
# waiting for an event that never arrives -- on top of generation.
#
# Nothing in this round needs the race. tg_r14_junc_report runs inside
# tg_emit_models, i.e. DURING generation, so the numbers exist the moment
# MODELS.DAT is written. This waits for that file to appear and stop growing,
# then quits CLEANLY (race.log only flushes on a clean shutdown) and falls back
# to killing ONLY the PID it launched.
#
#   -Exe   which binary to run (default td5re.exe; pass r14junc_baseline.exe to
#          re-measure the pre-rebuild build without rebuilding it back)
#   -Extra knob hashtable, applied after every TD5RE_* var is cleared
param([string]$Seed = "20260901", [hashtable]$Extra = @{}, [string]$Tag = "run",
      [string]$Exe = "td5re.exe", [int]$GenWait = 600, [int]$Port = 37141)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

# TD5RE_* vars PERSIST across launches in one PowerShell session; an "isolated"
# A/B silently inherits every knob an earlier run set. Clear before EVERY launch.
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED  = $Seed
$env:TD5RE_R13_JUNC_REPORT = "1"
$env:TD5RE_R13_JUNC_SPAN   = "1109"     # the complaint span this round
$env:TD5RE_CONTROL_PORT    = "$Port"
$env:TD5RE_WINDOW_TITLE    = "TD5RE R14 junc $Tag seed $Seed"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

# level090 is CACHED -- it only regenerates when the DIRECTORY IS ABSENT.
$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$log = Join-Path $wt "log\race.log"
for ($t = 0; $t -lt 20 -and (Test-Path $log); $t++) {
    try { Remove-Item $log -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
}

$p = Start-Process -FilePath (Join-Path $wt $Exe) `
      -ArgumentList "--AutoRace=1","--SkipIntro=1","--Control=1" `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id) exe=$Exe seed=$Seed tag=$Tag"

# Wait for MODELS.DAT to appear and go quiet (generation done), not for a race.
$models = Join-Path $lvl "MODELS.DAT"
$last = -1; $stable = 0; $done = $false
for ($i = 0; $i -lt $GenWait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    if (Test-Path $models) {
        $len = (Get-Item $models).Length
        if ($len -gt 0 -and $len -eq $last) { $stable++ } else { $stable = 0 }
        $last = $len
        if ($stable -ge 6) { $done = $true; break }
    }
}
Write-Host "generated=$done after ${i}s models=$last"

# Clean shutdown so race.log flushes; kill ONLY our own PID as a fallback.
if (-not $p.HasExited) {
    $u = New-Object System.Net.Sockets.UdpClient
    $ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, $Port)
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    $u.Close()
    for ($j = 0; $j -lt 30 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2 }
}
Copy-Item $log (Join-Path $wt "log\race_r14_$Tag.log") -Force -ErrorAction SilentlyContinue
Write-Host "### $Tag seed=$Seed"
$out = Join-Path $wt "log\race_r14_$Tag.log"
if (Test-Path $out) {
    Select-String -Path $out -Pattern "r13junc SUMMARY|r14junc CONTINUATIONS" |
        ForEach-Object { Write-Host ("  " + $_.Line) }
} else { Write-Host "  NO LOG" }
foreach ($f in @("MODELS.DAT","STRIP.DAT","LEVELINF.DAT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0,16)
        Write-Host ("  {0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $p2).Length, $h)
    }
}
