# stall160_sweep.ps1 -- race La Plata across several race CONFIGURATIONS,
# traffic on and off, and report whether every car gets through the
# span 120..200 section.
#
#   pwsh verify/stall160_sweep.ps1 -Tag fix `
#        -Configs "1001,1,5;2002,0,5;3003,2,5;4004,1,3;5005,0,3" `
#        -TrafficLevels "4,0" -RaceSecs 150
#
# -Configs is a semicolon-separated list of "raceSeed,difficulty,opponents".
#
# WHY NOT SEED ALONE: TD5RE_RACE_SEED is measurably INERT for this question.
# Two La Plata races at seeds 0x1A2B3C4D and 1001 produced 8400 identical
# per-tick span samples over the first 1400 ticks -- the SmartAI per-car skill
# and persona are hashed from (slot, difficulty tier), NOT from the race seed,
# so the field drives the same race whatever the seed is. The seed is still
# passed (it is what the round asked for, and it does move the traffic/branch
# RNG later on), but DIFFICULTY and OPPONENT COUNT are what actually give five
# different races, so they are swept too.
#
# NOTE: every parameter is a STRING. `pwsh -File` stringifies every argument,
# so [int[]] and [hashtable] parameters fail with a cast error instead of
# being parsed.
param([string]$Tag = "sweep",
      [string]$Configs = "1001,1,5;2002,0,5;3003,2,5;4004,1,3;5005,0,3",
      [string]$TrafficLevels = "4,0",
      [int]$RaceSecs = 150,
      [int]$GenWait = 45,
      [int]$PlayerIsAI = 1,
      [int]$CarDamage = 0,
      [int]$Goal = 200,
      [int]$MaxSimTicks = 4000,   # fixed sim-tick budget per run (see stall160_run.ps1)
      [int]$MinTicks = 3000,      # below this a run is INCONCLUSIVE, not FAIL
      [string]$Extra = "")        # "NAME=VALUE,NAME=VALUE", see stall160_run.ps1

$wt  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$run = Join-Path $wt "verify\stall160_run.ps1"
$rep = Join-Path $wt "verify\stall160_report.py"
$results = @()

$trafficList = $TrafficLevels.Split(',') | ForEach-Object { [int]$_.Trim() }
$configList  = $Configs.Split(';') | Where-Object { $_.Trim() -ne "" } | ForEach-Object {
    $p = $_.Split(',')
    [pscustomobject]@{ seed = $p[0].Trim(); diff = [int]$p[1].Trim(); opp = [int]$p[2].Trim() }
}

foreach ($tfc in $trafficList) {
    foreach ($c in $configList) {
        $t = "{0}_s{1}d{2}o{3}_t{4}" -f $Tag, $c.seed, $c.diff, $c.opp, $tfc
        Write-Host "=== $t ==="
        & pwsh -NoProfile -File $run -Tag $t -Seed $c.seed -RaceSecs $RaceSecs `
               -GenWait $GenWait -Traffic $tfc -Opponents $c.opp -Difficulty $c.diff `
               -MaxSimTicks $MaxSimTicks `
               -PlayerIsAI $PlayerIsAI -CarDamage $CarDamage -Extra "$Extra" | Out-Host
        $out = & python $rep (Join-Path $wt "log\s160_$t") --hold 150 --goal $Goal `
                      --min-ticks $MinTicks 2>&1
        $out | Out-Host
        $verdict = ($out | Where-Object { $_ -match '^VERDICT' }) -join ''
        $results += [pscustomobject]@{ tag = $t; cfg = $c; traffic = $tfc; verdict = $verdict }
    }
}

Write-Host ""
Write-Host "===== SWEEP SUMMARY ($Tag) ====="
foreach ($r in $results) {
    Write-Host ("  seed {0,-6} diff {1} opp {2} traffic {3}  {4}" `
                -f $r.cfg.seed, $r.cfg.diff, $r.cfg.opp, $r.traffic, $r.verdict)
}
