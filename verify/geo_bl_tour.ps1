# geo_bl_tour.ps1 -- thin wrapper over verify/geo_tour.ps1 (round 1019 T). The per-round tour scripts
# booted the game once per pose list, regenerated the level every time and waited out the scenery
# stream; geo_tour.ps1 captures every pose in one launch and reuses the built level.
# Same parameters as before, same output (log/tour_<Tag>_<name>.png).
# The block-loop harness car setup is kept: 5 AI opponents, AutoThrottle 0, branch force mode 2, race seed 11.
#
#   pwsh verify/geo_bl_tour.ps1 -Tag after -PosesFile poses.txt
#   pwsh verify/geo_bl_tour.ps1 -Tag before -PosesFile poses.txt -Env "TD5RE_GEO_FORK_MERGE=0"
param([string]$Tag = "run",
      [string]$Exe = "td5re.exe",
      [Parameter(Mandatory = $true)][string]$PosesFile,
      [string]$Env = "",            # "NAME=VALUE|NAME=VALUE"
      [string]$GeoPlace = "",
      [int]$Track = 61,
      [int]$Dwell = 6,
      [int]$GenWait = 900,
      [int]$RaceSecs = 240)

$x = @{}
$x["TD5RE_AI_BRANCH_FORCE_MODE"] = "2"; $x["TD5RE_RACE_SEED"] = "11"
if ($Env -ne "") {
    foreach ($kv in $Env.Split('|')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { $x[$p[0].Trim()] = $p[1] }
    }
}
& (Join-Path $PSScriptRoot "geo_tour.ps1") -Tag $Tag -Exe $Exe -Poses $PosesFile -Extra $x `
    -GeoPlace $GeoPlace -Track $Track -Dwell $Dwell -Timeout ($GenWait + $RaceSecs) -Opponents 5 -AutoThrottle 0
exit $LASTEXITCODE
