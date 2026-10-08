# stall160_sweep.ps1 -- race La Plata across several RACE SEEDS, traffic on and
# off, and report whether every car gets through the span 120..200 section.
#
#   pwsh verify/stall160_sweep.ps1 -Tag fix -Seeds 1,2,3,4,5 -RaceSecs 170
#
# One stall160_run.ps1 per (seed, traffic) pair, then stall160_report.py on each.
# The seed goes in via TD5RE_RACE_SEED (DEV-only): RaceTrace pins the per-race
# RNG at 0x1A2B3C4D, so without the override every "different seed" would be the
# same race.
param([string]$Tag = "sweep",
      [int[]]$Seeds = @(1, 2, 3, 4, 5),
      [int[]]$TrafficLevels = @(4, 0),
      [int]$RaceSecs = 170,
      [int]$GenWait = 60,
      [int]$PlayerIsAI = 1,
      [int]$CarDamage = 0,
      [int]$Goal = 200,
      [hashtable]$Extra = @{})

$wt  = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$run = Join-Path $wt "verify\stall160_run.ps1"
$rep = Join-Path $wt "verify\stall160_report.py"
$results = @()

foreach ($tfc in $TrafficLevels) {
    foreach ($sd in $Seeds) {
        $t = "{0}_s{1}_t{2}" -f $Tag, $sd, $tfc
        Write-Host "=== $t ==="
        & pwsh -NoProfile -File $run -Tag $t -Seed "$sd" -RaceSecs $RaceSecs `
               -GenWait $GenWait -Traffic $tfc -Opponents 5 `
               -PlayerIsAI $PlayerIsAI -CarDamage $CarDamage -Extra $Extra | Out-Host
        $out = & python $rep (Join-Path $wt "log\s160_$t") --hold 150 --goal $Goal 2>&1
        $out | Out-Host
        $verdict = ($out | Where-Object { $_ -match '^VERDICT' }) -join ''
        $results += [pscustomobject]@{ tag = $t; seed = $sd; traffic = $tfc; verdict = $verdict }
    }
}

Write-Host ""
Write-Host "===== SWEEP SUMMARY ($Tag) ====="
foreach ($r in $results) {
    Write-Host ("  seed {0,-10} traffic {1}  {2}" -f $r.seed, $r.traffic, $r.verdict)
}
