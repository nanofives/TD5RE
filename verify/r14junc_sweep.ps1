# [R14 JUNCTION] sweep: run the r13junc probe over a seed list x a knob set and
# print just the SUMMARY line of each run, so one command answers "did this move".
param([string[]]$Seeds = @("20260901","99991","777"),
      [string]$Tag = "sweep",
      [hashtable]$Extra = @{},
      [int]$Wait = 400)
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
foreach ($s in $Seeds) {
    $t = "$Tag`_$s"
    & "$wt\verify\r13junc_probe.ps1" -Seed $s -Tag $t -Extra $Extra -Wait $Wait | Out-Null
    $log = Join-Path $wt "log\race_r13_$t.log"
    if (Test-Path $log) {
        $sum = Select-String -Path $log -Pattern "r13junc SUMMARY|r14junc SUMMARY" |
               ForEach-Object { $_.Line }
        Write-Host "### $Tag seed=$s"
        if ($sum) { $sum | ForEach-Object { Write-Host $_ } } else { Write-Host "  NO SUMMARY" }
    } else { Write-Host "### $Tag seed=$s  NO LOG" }
}
