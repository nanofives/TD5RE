# geo_r1016k_batch.ps1 -- round 1016 K: N AI races of the La Plata partido route IN PARALLEL.
#
#   pwsh verify/geo_r1016k_batch.ps1 -Tag after1 -Exe td5re.exe
#   pwsh verify/geo_r1016k_batch.ps1 -Tag base -Exe td5re_base.exe -Seeds 11,22,33
#   pwsh verify/geo_r1016k_batch.ps1 -Tag stress -Extra "TD5RE_AI_BRANCH_FORCE_ALL=1"
#
# geo_realfork_run.ps1 writes log/ and regenerates re/assets/levels/level091 in the
# directory it lives in, so parallel runs need one directory each. RunRoot holds
# _r1016k_run1 .. runN (a copy of re/assets + horns + inputscripts + verify/*.ps1; make
# them with the mkrun snippet in docs/plans/GEO_REAL_FORKS.md "Round 1016 K"). Each run
# ends as soon as its track trace has been silent for 40 s (-IdleStop), about 3 minutes.
# Output: DataDir/rf_<Tag>_s<seed>_*  (+ rf_<Tag>_STRIP.DAT) and, if python is on the
# path, the geo_r1016k_report.py summary of every seed.
param([string]$Tag = "run",
      [string]$Exe = "td5re.exe",
      [string]$Seeds = "11,22,33",     # comma list (a [int[]] does not survive pwsh -File)
      [string]$Extra = "",
      [string]$RunRoot = "",
      [string]$DataDir = "",
      [string]$GeoPlace = "la_plata_partido",
      [int]$MaxWaitSec = 900,
      [int]$Traffic = 0,
      [int]$Track = 61,                 # 60 + GeoPlace none = the synthetic auto track
      [string]$Modules = "track,driver,motion,progress,pose")

$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ($RunRoot -eq "") { $RunRoot = Split-Path -Parent $wt }
if ($DataDir -eq "") { $DataDir = Join-Path $RunRoot "_r1016k_data" }
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null

$SeedList = @($Seeds.Split(",") | ForEach-Object { [int]$_.Trim() })
$procs = @()
for ($i = 0; $i -lt $SeedList.Count; $i++) {
    $rd = Join-Path $RunRoot ("_r1016k_run{0}" -f ($i + 1))
    if (-not (Test-Path $rd)) { throw "missing run dir $rd" }
    Copy-Item (Join-Path $wt $Exe) (Join-Path $rd $Exe) -Force
    Copy-Item (Join-Path $wt "verify\geo_realfork_run.ps1") (Join-Path $rd "verify\geo_realfork_run.ps1") -Force
    $s = $SeedList[$i]
    $a = @("-NoProfile", "-File", (Join-Path $rd "verify\geo_realfork_run.ps1"),
           "-Tag", "${Tag}_s$s", "-Exe", $Exe, "-Seed", "$s", "-GeoPlace", $GeoPlace,
           "-GenWait", "180", "-RaceSecs", "600", "-Modules", $Modules, "-IdleStop", "40",
           "-Traffic", "$Traffic", "-Track", "$Track")
    if ($Extra -ne "") { $a += @("-Extra", $Extra) }
    $out = Join-Path $rd "harness_$Tag.txt"
    $procs += Start-Process -FilePath pwsh -ArgumentList $a -WorkingDirectory $rd `
        -RedirectStandardOutput $out -WindowStyle Hidden -PassThru
}
$t0 = Get-Date
foreach ($p in $procs) {
    $left = $MaxWaitSec - ((Get-Date) - $t0).TotalSeconds
    if ($left -lt 1) { $left = 1 }
    [void]$p.WaitForExit([int]($left * 1000))
}
Write-Host ("batch {0} done in {1:N0} s" -f $Tag, ((Get-Date) - $t0).TotalSeconds)
for ($i = 0; $i -lt $SeedList.Count; $i++) {
    $rd = Join-Path $RunRoot ("_r1016k_run{0}" -f ($i + 1))
    $s = $SeedList[$i]
    Get-ChildItem (Join-Path $rd "log") -Filter "rf_${Tag}_s${s}_*" -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Item $_.FullName $DataDir -Force }
    $strip = Join-Path $rd "re\assets\levels\level091\STRIP.DAT"
    if (Test-Path $strip) { Copy-Item $strip (Join-Path $DataDir "rf_${Tag}_s${s}_STRIP.DAT") -Force }
    Copy-Item (Join-Path $rd "harness_$Tag.txt") (Join-Path $DataDir "rf_${Tag}_s${s}_harness.txt") -Force -ErrorAction SilentlyContinue
}
