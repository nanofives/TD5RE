# [R13 JUNCTION] one-shot generator probe: generate the track for one seed with
# the opt-in reports on, then quit CLEANLY so race.log flushes.
#   -Seed <n>  -Extra @{ KNOB = "0" }  -Tag <name>
param([string]$Seed = "20260901", [hashtable]$Extra = @{}, [string]$Tag = "run",
      [int]$Wait = 150)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)  # repo/worktree root
Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
$env:TD5RE_AUTOTRACK_SEED   = $Seed
$env:TD5RE_R13_JUNC_REPORT  = "1"
$env:TD5RE_R13_JUNC_SPAN    = "709"
$env:TD5RE_CONTROL_PORT     = "37131"
$env:TD5RE_WINDOW_TITLE     = "TD5RE R13 junc probe $Tag seed $Seed"
foreach ($k in $Extra.Keys) { Set-Item "env:$k" $Extra[$k] }

$lvl = Join-Path $wt "re\assets\levels\level090"
if (Test-Path $lvl) { Remove-Item $lvl -Recurse -Force }
$log = Join-Path $wt "log\race.log"
# A previous run's process can still hold race.log for a moment after it exits,
# so retry rather than leaving the old log in place and comparing two runs' output.
for ($t = 0; $t -lt 20 -and (Test-Path $log); $t++) {
    try { Remove-Item $log -Force -ErrorAction Stop } catch { Start-Sleep -Milliseconds 500 }
}

$p = Start-Process -FilePath "$wt\td5re.exe" `
      -ArgumentList "--AutoRace=1","--SkipIntro=1","--Control=1" `
      -WorkingDirectory $wt -PassThru
Write-Host "pid=$($p.Id)"

# Poll the control socket until the race is up, then quit cleanly.
Add-Type -AssemblyName System.Net.Sockets 2>$null
$u = New-Object System.Net.Sockets.UdpClient
$u.Client.ReceiveTimeout = 1500
$ep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Loopback, 37131)
$ok = $false
for ($i = 0; $i -lt $Wait; $i++) {
    Start-Sleep -Seconds 1
    if ($p.HasExited) { break }
    try {
        $b = [Text.Encoding]::ASCII.GetBytes("get_state")
        [void]$u.Send($b, $b.Length, $ep)
        $rep = New-Object System.Net.IPEndPoint([System.Net.IPAddress]::Any, 0)
        $r = [Text.Encoding]::ASCII.GetString($u.Receive([ref]$rep))
        if ($r -match "RACE") { $ok = $true; break }
    } catch { }
}
Write-Host "race_reached=$ok after ${i}s"
if (-not $p.HasExited) {
    try { $b = [Text.Encoding]::ASCII.GetBytes("quit"); [void]$u.Send($b, $b.Length, $ep) } catch { }
    for ($j = 0; $j -lt 25 -and -not $p.HasExited; $j++) { Start-Sleep -Seconds 1 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
$u.Close()
Copy-Item $log (Join-Path $wt "log\race_r13_$Tag.log") -Force -ErrorAction SilentlyContinue
foreach ($f in @("MODELS.DAT","STRIP.DAT","LEVELINF.DAT","ROUTES.DAT")) {
    $p2 = Join-Path $lvl $f
    if (Test-Path $p2) {
        $h = (Get-FileHash $p2 -Algorithm SHA256).Hash.Substring(0,16)
        Write-Host ("{0,-13} {1,10} bytes  {2}" -f $f, (Get-Item $p2).Length, $h)
    }
}
Write-Host "saved log\race_r13_$Tag.log"
