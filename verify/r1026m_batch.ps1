# r1026m_batch.ps1 -- run the round-1026 M measurement matrix for ONE configuration (before / after).
#   pwsh verify/r1026m_batch.ps1 -Name base -Extra "TD5RE_V2V_GAPLOG=1|TD5RE_TRAFFIC_SAT=0"
#   pwsh verify/r1026m_batch.ps1 -Name sat  -Extra "TD5RE_V2V_GAPLOG=1"
param([string]$Name = "base", [string]$Extra = "TD5RE_V2V_GAPLOG=1", [string]$Exe = "td5re.exe", [int]$Ticks = 4000,
      [ValidateSet("full","short")][string]$Set = "full", [int]$Timeout = 420)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$runs = @(
  @{ t = 0;  m = "ai" }, @{ t = 0;  m = "throttle" },
  @{ t = 2;  m = "ai" }, @{ t = 5;  m = "ai" }, @{ t = 3;  m = "throttle" },
  @{ t = 61; m = "ai" }, @{ t = 61; m = "throttle" }
)
if ($Set -eq "short") { $runs = @(@{ t = 0; m = "ai" }, @{ t = 0; m = "throttle" }, @{ t = 2; m = "ai" }, @{ t = 3; m = "throttle" }, @{ t = 61; m = "throttle" }) }
foreach ($r in $runs) {
    $tag = "{0}_t{1}_{2}" -f $Name, $r.t, $r.m
    & pwsh -NoProfile -File (Join-Path $wt "verify\r1026m_gaprun.ps1") -Tag $tag -Track $r.t -Exe $Exe -Extra $Extra -Mode $r.m -Ticks $Ticks -Timeout $Timeout
}
Write-Host "batch $Name done"
