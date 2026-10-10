# geo_fork_validate.ps1 -- round 1016 H: PER-FORK AI VALIDATION HARNESS.
#
# Races the geo route (La Plata Partido, slot 61 / level091) with a 6-AI field, N seeds IN
# PARALLEL, forcing every fork to see cars on BOTH arms, and judges every fork against
# an A/B baseline (the same seed with the real forks switched off). Output: a per-fork
# table with a PASS/WARN/FAIL verdict, and verify/out/fork_validate.json (the machine-
# readable allow/deny list, format in docs/plans/GEO_FORK_VALIDATION.md).
#
#   pwsh verify/geo_fork_validate.ps1                       # 3 seeds x (forks on + forks off)
#   pwsh verify/geo_fork_validate.ps1 -Seeds "11,22"        # chosen seeds
#   pwsh verify/geo_fork_validate.ps1 -Only 210             # build ONLY the fork whose F=210
#   pwsh verify/geo_fork_validate.ps1 -ForkEnv "TD5RE_GEO_PLAZA_STRETCH_MAX=3.0"
#                                                           # a candidate: extra env on the fork arm
#   pwsh verify/geo_fork_validate.ps1 -Baseline reuse       # reuse the last forks-off runs
#   pwsh verify/geo_fork_validate.ps1 -Analyze <outdir>     # re-judge saved runs, no races
#
# HOW THE RUNS DO NOT CLOBBER EACH OTHER. Every run gets its own directory
# verify/out/<tag>/<arm>_s<seed>/ holding a COPY of the exe (the game writes log/ next to
# the exe, and every asset path is relative to the working directory), a real re/assets/
# whose static children are NTFS junctions to the shared tree, a private levels/level091
# (the generator deletes and rewrites it) and a private geo place dir (hard links for
# the big rasters, a copy of _route/). Windows titles are distinct and every kill is by
# the PID this script started. Teardown removes the junctions with a reparse-aware
# delete (never Remove-Item -Recurse over a junction).
#
# Standing launch rules, all applied: --Windowed=1, --VSync=0, window sent to the back,
# RT off and minimum graphics, car damage OFF (damage ON freezes racers like a stall),
# audio off, no sockets and no network, every inherited TD5RE_* cleared first, td5re.ini
# never written (no ini is copied: the CLI carries every setting).
param(
    [string]$Seeds = "11,22,33",          # comma list, or a count ("3" = 11,22,33)
    [string]$Tag = "",                     # run-set name (default: timestamp)
    [string]$Exe = "td5re.exe",            # dev exe under the worktree root
    [string]$Only = "",                    # TD5RE_GEO_FORK_ALLOW: fork F list ("-1" = none)
    [string]$Deny = "",                    # TD5RE_GEO_FORK_DENY: fork F list
    [string]$ForkEnv = "",                 # "A=B,C=D" extra env for the FORK arm only
    [string]$CommonEnv = "",               # "A=B,C=D" extra env for BOTH arms
    [string]$Baseline = "run",             # run | reuse (previous baseline) | none
    [string]$BaselineFrom = "",            # run-set dir holding base_s<seed> to reuse
    [string]$Analyze = "",                 # re-judge an existing run-set dir, run nothing
    [string]$Verdicts = "",                # explicit output JSON (default verify/out/fork_validate.json)
    [int]$Jobs = 6,                        # max concurrent games
    [double]$FastForward = 8.0,            # TraceFastForward (1.0 = real time)
    [int]$MaxSimTicks = 5200,              # hard sim-tick cap per race
    [int]$StopSpan = 0,                    # end a race early when ALL cars passed this span_norm (0 = auto)
    [int]$StallSecs = 45,
    [int]$RaceOverSecs = 8,                # wall secs without a new sim tick in the trace = the race has ended
    [int]$StuckTicks = 900,                # sim ticks without advancing before a car stops holding the race open                  # end a race early when no car advanced for this many WALL secs
    [int]$GenWait = 900,                   # wall secs allowed for generate + race
    [int]$RaceSecs = 480,
    [string]$Place = "la_plata_partido",
    [int]$Track = 61,
    [int]$Opponents = 5,
    [string]$Difficulty = "auto",          # AI tier per seed: auto = 1,2,0,1,2,0.. (the seed alone changes only the fork choice)
    [int]$ForceMode = 1,                   # TD5RE_AI_BRANCH_FORCE_MODE (1 = alternate arms by slot)
    [string]$Modules = "track,driver,motion,progress,pose,rotation",
    [switch]$NoAnalyze,
    [string]$AnalyzeArgs = ""              # passed through to geo_fork_validate.py
)

$ErrorActionPreference = "Stop"
$wt = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$here = Join-Path $wt "verify"
$outRoot = Join-Path $here "out"
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
if ($Verdicts -eq "") { $Verdicts = Join-Path $outRoot "fork_validate.json" }

function Find-Python {
    foreach ($c in @("python", "py")) {
        $g = Get-Command $c -ErrorAction SilentlyContinue
        if ($g) { return $g.Source }
    }
    throw "python not found"
}

function Invoke-Analyzer([string]$dir) {
    $py = Find-Python
    $pyArgs = @((Join-Path $here "geo_fork_validate.py"), $dir, "--out", $Verdicts)
    if ($AnalyzeArgs -ne "") { $pyArgs += ($AnalyzeArgs -split ' ') }
    & $py @pyArgs | Out-Host
    return $LASTEXITCODE
}

if ($Analyze -ne "") {
    $d = if ([IO.Path]::IsPathRooted($Analyze)) { $Analyze } else { Join-Path $wt $Analyze }
    exit (Invoke-Analyzer $d)
}

# ---- seeds -------------------------------------------------------------------
$seedList = @()
if ($Seeds -match '^[1-9]$') {            # a single digit 1..9 = a COUNT (11,22,33,...)
    $seedList = @(11, 22, 33, 44, 55, 66, 77, 88, 99, 110, 121, 132)[0..([int]$Seeds - 1)]
} else {
    $seedList = $Seeds.Split(',') | ForEach-Object { [int]($_.Trim()) }
}
if ($Tag -eq "") { $Tag = "fv_" + (Get-Date -Format "yyyyMMdd_HHmmss") }
$setDir = Join-Path $outRoot $Tag
New-Item -ItemType Directory -Force -Path $setDir | Out-Null

# ---- assets: the shared tree the junctions point at ---------------------------
# A worktree has no 1.2 GB geo place of its own: use the main tree's, found through git.
function Find-AssetRoot {
    $cands = @((Join-Path $wt "re\assets"))
    $common = (& git -C $wt rev-parse --git-common-dir 2>$null)
    if ($common) { $cands += (Join-Path (Split-Path -Parent (Resolve-Path $common).Path) "re\assets") }
    foreach ($c in $cands) {
        if (Test-Path (Join-Path $c "geo\$Place\HEIGHT.R16")) { return $c }
    }
    throw "geo place '$Place' not found under any of: $($cands -join ', ')"
}
$assets = Find-AssetRoot
Write-Host "assets: $assets"

function New-Junction([string]$link, [string]$target) {
    New-Item -ItemType Junction -Path $link -Target $target | Out-Null
}

function Initialize-RunDir([string]$rd) {
    if (Test-Path $rd) { Remove-RunDir $rd }
    New-Item -ItemType Directory -Force -Path (Join-Path $rd "re\assets\levels") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $rd "re\assets\geo\$Place") | Out-Null
    Copy-Item (Join-Path $wt $Exe) (Join-Path $rd "td5re.exe") -Force
    $ra = Join-Path $rd "re\assets"
    foreach ($c in Get-ChildItem $assets -Force) {
        if ($c.Name -in @("levels", "geo")) { continue }
        if ($c.PSIsContainer) { New-Junction (Join-Path $ra $c.Name) $c.FullName }
        else { Copy-Item $c.FullName (Join-Path $ra $c.Name) }
    }
    $lvlGen = "level{0:D3}" -f ($Track + 30)
    foreach ($c in Get-ChildItem (Join-Path $assets "levels") -Force) {
        if ($c.Name -eq $lvlGen) { continue }          # generated, private
        if ($c.PSIsContainer) { New-Junction (Join-Path $ra "levels\$($c.Name)") $c.FullName }
        else { Copy-Item $c.FullName (Join-Path $ra "levels\$($c.Name)") }
    }
    # geo: only the selected place, never _tiles (that is the network tile cache).
    Set-Content -Path (Join-Path $ra "geo\SELECTED.TXT") -Value $Place -NoNewline
    $src = Join-Path $assets "geo\$Place"
    $dst = Join-Path $ra "geo\$Place"
    foreach ($c in Get-ChildItem $src -Force) {
        if ($c.Name -eq "_route") { Copy-Item $c.FullName (Join-Path $dst "_route") -Recurse }
        elseif ($c.PSIsContainer) { New-Junction (Join-Path $dst $c.Name) $c.FullName }
        else { New-Item -ItemType HardLink -Path (Join-Path $dst $c.Name) -Target $c.FullName | Out-Null }
    }
}

# Junction-safe tree delete: NEVER descends into a reparse point (a junction here points
# at the shared asset tree), deletes the link itself, then the real directories bottom-up.
function Remove-Tree([string]$p) {
    if (-not [IO.Directory]::Exists($p)) { return }
    foreach ($e in [IO.Directory]::GetFileSystemEntries($p)) {
        $attr = [IO.File]::GetAttributes($e)
        $isDir = ($attr -band [IO.FileAttributes]::Directory) -ne 0
        $isLink = ($attr -band [IO.FileAttributes]::ReparsePoint) -ne 0
        if ($isLink) {
            if ($isDir) { [IO.Directory]::Delete($e, $false) } else { [IO.File]::Delete($e) }
        } elseif ($isDir) { Remove-Tree $e }
        else {
            if ($attr -band [IO.FileAttributes]::ReadOnly) { [IO.File]::SetAttributes($e, [IO.FileAttributes]::Normal) }
            [IO.File]::Delete($e)
        }
    }
    [IO.Directory]::Delete($p, $false)
}
function Remove-RunDir([string]$rd) { Remove-Tree $rd }

# ---- one game launch ----------------------------------------------------------
$gfx = @("--Lighting=0", "--Quality=0", "--SunShadows=0", "--Reflections=0",
         "--WetRoads=0", "--StreetLights=0", "--CarLights=0", "--LegacyShadows=0",
         "--GIQuality=0", "--ShadowRays=0", "--ReflectionQuality=0",
         "--CarShadows=0", "--VFX=0", "--WorldBillboards=0", "--FoliageAA=0")

function Add-EnvList($e, [string]$list) {
    if ($list -eq "") { return }
    foreach ($kv in $list.Split(',')) {
        $p = $kv.Split('=', 2)
        if ($p.Count -eq 2 -and $p[0].Trim() -ne "") { $e[$p[0].Trim()] = $p[1] }
    }
}

try {
    Add-Type -Name W3 -Namespace N3 -MemberDefinition '
      [DllImport("user32.dll")] public static extern bool SetWindowPos(
        IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);' -ErrorAction Stop
} catch { }

function Get-Tier($run) {
    if ($Difficulty -ne "auto") { return [int]$Difficulty }
    $i = [array]::IndexOf($seedList, $run.seed)
    return @(1, 2, 0)[[Math]::Max(0, $i) % 3]
}

function Start-Run($run) {
    # Start-Process inherits THIS process's environment: clear every TD5RE_* first (knobs
    # persist across shell launches), set this run's, launch, then clear again. Launches are
    # sequential, so parallel runs cannot see each other's variables.
    Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $e = [ordered]@{}
    $e["TD5RE_GEO_PLACE"] = $Place
    $e["TD5RE_AUTOTRACK_REUSE"] = "0"
    $e["TD5RE_RT"] = "0"
    $e["TD5RE_WINDOW_TITLE"] = "TD5RE forkval $Tag $($run.name)"
    $e["TD5RE_RACE_SEED"] = "$($run.seed)"
    $e["TD5RE_GEO_FORK_DIAG"] = "1"
    $e["TD5RE_AI_BRANCH_FORCE_MODE"] = "$ForceMode"
    Add-EnvList $e $CommonEnv
    if ($run.arm -eq "base") {
        $e["TD5RE_GEO_REAL_FORKS"] = "0"
    } else {
        if ($Only -ne "") { $e["TD5RE_GEO_FORK_ALLOW"] = $Only }
        if ($Deny -ne "") { $e["TD5RE_GEO_FORK_DENY"] = $Deny }
        Add-EnvList $e $ForkEnv
    }
    foreach ($k in $e.Keys) { Set-Item "env:$k" $e[$k] }
    $a = @("--AutoRace=1", "--SkipIntro=1", "--DefaultTrack=$Track",
           "--DefaultOpponents=$Opponents", "--Traffic=0", "--Difficulty=$(Get-Tier $run)",
           "--PlayerIsAI=1", "--AutoThrottle=0", "--CarDamage=0",
           "--Windowed=1", "--VSync=0", "--SFXVolume=0", "--MusicVolume=0",
           "--RaceTrace=1", "--RaceTraceSlot=-1", "--RaceTraceMaxSimTicks=$MaxSimTicks",
           "--RaceTraceMaxFrames=4000000", "--TraceModules=$Modules",
           "--TraceStages=post_track,post_ai,post_physics,post_progress")
    if ($FastForward -gt 1.0) { $a += "--TraceFastForward=$FastForward" }
    $a += $gfx
    $p = Start-Process -FilePath (Join-Path $run.dir "td5re.exe") -ArgumentList $a `
            -WorkingDirectory $run.dir -PassThru `
            -RedirectStandardOutput (Join-Path $run.dir "stdout.txt") `
            -RedirectStandardError (Join-Path $run.dir "stderr.txt")
    Get-ChildItem env: | Where-Object { $_.Name -like 'TD5RE_*' } | ForEach-Object { Remove-Item "env:$($_.Name)" }
    $run.proc = $p
    $run.pid = $p.Id
    $run.t0 = Get-Date
    $run.pos = 0L
    $run.maxspan = @{}
    $run.lastprog = Get-Date
    $run.lasttick = 0
    $run.state = "running"
    Start-Sleep -Milliseconds 1500
    try {
        if ($p.MainWindowHandle -ne 0) { [void][N3.W3]::SetWindowPos($p.MainWindowHandle, [IntPtr]1, 0, 0, 0, 0, 0x0013) }
    } catch { }
    Write-Host ("[{0:HH:mm:ss}] started {1} pid={2}" -f (Get-Date), $run.name, $p.Id)
}

# Incremental read of the growing track trace: max span_norm per slot, last sim tick.
function Update-Progress($run) {
    $f = Join-Path $run.dir "log\race_trace_track.csv"
    if (-not (Test-Path $f)) { return }
    try {
        $fs = New-Object IO.FileStream($f, 'Open', 'Read', 'ReadWrite')
        if ($fs.Length -le $run.pos) { $fs.Dispose(); return }
        [void]$fs.Seek($run.pos, 'Begin')
        $sr = New-Object IO.StreamReader($fs)
        $txt = $sr.ReadToEnd()
        $nl = $txt.LastIndexOf("`n")
        if ($nl -lt 0) { $sr.Dispose(); return }
        $run.pos += [Text.Encoding]::UTF8.GetByteCount($txt.Substring(0, $nl + 1))
        $sr.Dispose()
        $moved = $false
        foreach ($ln in $txt.Substring(0, $nl).Split("`n")) {
            $c = $ln.Split(',')
            if ($c.Count -lt 8 -or $c[2] -ne "post_track") { continue }
            $slot = $c[3]; $norm = 0; $tick = 0
            if (-not [int]::TryParse($c[5], [ref]$norm)) { continue }
            [void][int]::TryParse($c[1], [ref]$tick)
            if ($tick -gt $run.lasttick) { $run.lasttick = $tick; $run.tickmoved = Get-Date }
            if (-not $run.maxspan.ContainsKey($slot) -or $norm -gt $run.maxspan[$slot]) {
                $run.maxspan[$slot] = $norm; $run.advtick[$slot] = $tick; $moved = $true
            }
        }
        if ($moved) { $run.lastprog = Get-Date }
    } catch { }
}

# The race.log is flushed as it fills: the strip's own fork lines and the registry finish are
# there long before the cars reach them. Returns 0 until they are.
function Get-AutoStop($run) {
    $f = Join-Path $run.dir "log\race.log"
    if (-not (Test-Path $f)) { return 0 }
    try {
        $fs = New-Object IO.FileStream($f, 'Open', 'Read', 'ReadWrite')
        $sr = New-Object IO.StreamReader($fs)
        $txt = $sr.ReadToEnd(); $sr.Dispose()
    } catch { return 0 }
    $fin = [regex]::Match($txt, 'registry finish span=(\d+)')
    if (-not $fin.Success) { return 0 }
    $rej = [regex]::Matches($txt, 'trackgen: fork \d+ \w+ F=\d+ len=\d+ corridor=\d+\.\.\d+ rejoin=(\d+)')
    $stop = [int]$fin.Groups[1].Value - 5
    if ($rej.Count -gt 0) {
        $mx = ($rej | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Maximum).Maximum + 40
        if ($mx -lt $stop) { $stop = $mx }
    }
    return $stop
}

function Stop-Run($run, [string]$why) {
    if ($run.state -ne "running") { return }
    $p = $run.proc
    Write-Host ("[{0:HH:mm:ss}] stopping {1} ({2}) tick={3}" -f (Get-Date), $run.name, $why, $run.lasttick)
    $run.why = $why
    try { if (-not $p.HasExited) { [void]$p.CloseMainWindow() } } catch { }
    for ($i = 0; $i -lt 40 -and -not $p.HasExited; $i++) { Start-Sleep -Milliseconds 500 }
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 1 }   # PID-scoped only
    $run.state = "done"
    $run.secs = [int]((Get-Date) - $run.t0).TotalSeconds
}

# ---- plan the runs --------------------------------------------------------------
$runs = @()
foreach ($s in $seedList) {
    $runs += [pscustomobject]@{ name = "fork_s$s"; arm = "fork"; seed = $s; dir = (Join-Path $setDir "fork_s$s"); state = "new"; why = ""; secs = 0; proc = $null; pid = 0; t0 = $null; pos = 0L; maxspan = @{}; lastprog = $null; lasttick = 0; stopat = 0; advtick = @{}; tickmoved = $null }
    if ($Baseline -ne "none") {
        $runs += [pscustomobject]@{ name = "base_s$s"; arm = "base"; seed = $s; dir = (Join-Path $setDir "base_s$s"); state = "new"; why = ""; secs = 0; proc = $null; pid = 0; t0 = $null; pos = 0L; maxspan = @{}; lastprog = $null; lasttick = 0; stopat = 0; advtick = @{}; tickmoved = $null }
    }
}
if ($Baseline -eq "reuse" -or $BaselineFrom -ne "") {
    # Reuse the forks-off runs of an earlier run-set: same exe revision of the track, same seed.
    $srcSet = $BaselineFrom
    if ($srcSet -eq "") {
        $srcSet = (Get-ChildItem $outRoot -Directory | Where-Object { $_.Name -ne $Tag -and (Test-Path (Join-Path $_.FullName "base_s$($seedList[0])\log")) } |
                   Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    } elseif (-not [IO.Path]::IsPathRooted($srcSet)) { $srcSet = Join-Path $wt $srcSet }
    if (-not $srcSet) { throw "no earlier run-set with baselines to reuse" }
    $keep2 = @()
    foreach ($r in $runs) {
        if ($r.arm -eq "base") {
            $o = Join-Path $srcSet "base_s$($r.seed)"
            if (-not (Test-Path (Join-Path $o "log\race_trace_track.csv"))) { throw "no baseline at $o" }
            # copy just what the analyzer reads
            New-Item -ItemType Directory -Force -Path (Join-Path $r.dir "log") | Out-Null
            Copy-Item (Join-Path $o "log\*") (Join-Path $r.dir "log") -Force
            foreach ($x in @("STRIP.DAT")) {
                $q = Join-Path $o $x
                if (Test-Path $q) { Copy-Item $q (Join-Path $r.dir $x) -Force }
            }
            $r.state = "done"; $r.why = "reused from $srcSet"
        }
        $keep2 += $r
    }
    $runs = $keep2
}

# ---- run them ---------------------------------------------------------------------
$wall0 = Get-Date
$todo = @($runs | Where-Object { $_.state -eq "new" })
Write-Host ("{0} runs ({1} seeds), {2} at a time, fast-forward {3}x, tag {4}" -f $todo.Count, $seedList.Count, $Jobs, $FastForward, $Tag)
foreach ($r in $todo) { Initialize-RunDir $r.dir }
Write-Host ("[{0:HH:mm:ss}] run dirs ready" -f (Get-Date))

$queue = [System.Collections.Queue]::new()
foreach ($r in $todo) { $queue.Enqueue($r) }
$active = @()
while ($queue.Count -gt 0 -or $active.Count -gt 0) {
    while ($queue.Count -gt 0 -and $active.Count -lt $Jobs) {
        $r = $queue.Dequeue(); Start-Run $r; $active += $r
    }
    Start-Sleep -Seconds 5
    foreach ($r in @($active)) {
        if ($r.state -ne "running") { continue }
        $age = ((Get-Date) - $r.t0).TotalSeconds
        if ($r.proc.HasExited) { $r.state = "done"; $r.why = "exited"; $r.secs = [int]$age; continue }
        Update-Progress $r
        $stopAt = $StopSpan
        if ($stopAt -le 0) {
            # auto: last built fork's rejoin + 40 spans, never past the registry finish
            if (-not $r.stopat -or $r.stopat -le 0) {
                if ($r.arm -eq "base") {
                    # a forks-off race stops where its forks-on twin does (same window judged)
                    $twin = $runs | Where-Object { $_.arm -eq "fork" -and $_.seed -eq $r.seed } | Select-Object -First 1
                    if ($twin -and $twin.stopat -gt 0) { $r.stopat = $twin.stopat }
                } else { $r.stopat = Get-AutoStop $r }
            }
            $stopAt = $r.stopat
        }
        if ($r.maxspan.Count -ge 1 -and $stopAt -gt 0) {
            # every car is either past the last fork or has not advanced for StuckTicks of SIM time
            # (a jammed car must not hold the other five hostage; it is measured as a jam)
            $pending = 0
            foreach ($k in $r.maxspan.Keys) {
                if ($r.maxspan[$k] -ge $stopAt) { continue }
                if (($r.lasttick - $r.advtick[$k]) -ge $StuckTicks) { continue }
                $pending++
            }
            if (($r.maxspan.Count -ge ($Opponents + 1)) -and $pending -eq 0) { Stop-Run $r "every car past span $stopAt or stuck $StuckTicks ticks"; continue }
        }
        # The sim itself stopped ticking: the race is over (finish/timeout), nothing more to measure.
        if ($r.lasttick -gt 0 -and $r.tickmoved -and ((Get-Date) - $r.tickmoved).TotalSeconds -gt $RaceOverSecs) { Stop-Run $r "race over (no sim tick for ${RaceOverSecs}s)"; continue }
        if ($r.lasttick -gt 0 -and ((Get-Date) - $r.lastprog).TotalSeconds -gt $StallSecs) { Stop-Run $r "no progress for ${StallSecs}s"; continue }
        if ($age -gt ($GenWait + $RaceSecs)) { Stop-Run $r "wall-clock cap"; continue }
    }
    $active = @($active | Where-Object { $_.state -eq "running" })
}
$wall = [int]((Get-Date) - $wall0).TotalSeconds
Write-Host ("[{0:HH:mm:ss}] all races done in {1} s wall" -f (Get-Date), $wall)

# keep the strip + models next to the traces (the analyzer reads the strip geometry)
foreach ($r in $runs) {
    $lvl = Join-Path $r.dir ("re\assets\levels\level{0:D3}" -f ($Track + 30))
    foreach ($x in @("STRIP.DAT", "MODELS.DAT")) {
        $q = Join-Path $lvl $x
        if (Test-Path $q) { Copy-Item $q (Join-Path $r.dir $x) -Force }
    }
}
$meta = [ordered]@{
    tag = $Tag; seeds = $seedList; only = $Only; deny = $Deny; fork_env = $ForkEnv; common_env = $CommonEnv
    fast_forward = $FastForward; max_sim_ticks = $MaxSimTicks; force_mode = $ForceMode
    place = $Place; track = $Track; wall_secs = $wall; exe = $Exe
    runs = @($runs | ForEach-Object { [ordered]@{ name = $_.name; arm = $_.arm; seed = $_.seed; secs = $_.secs; why = $_.why } })
}
$meta | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $setDir "meta.json")

# ---- teardown (junction-safe), then judge ---------------------------------------
foreach ($r in $runs) {
    # keep log/, STRIP.DAT; drop the asset tree (junctions) either way
    $ra = Join-Path $r.dir "re"
    if (Test-Path $ra) { Remove-RunDir $ra }
    $exe = Join-Path $r.dir "td5re.exe"
    if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }
}
$rc = 0
if (-not $NoAnalyze) { $rc = Invoke-Analyzer $setDir }
Write-Host ("run-set: {0}   wall: {1} s" -f $setDir, $wall)
exit $rc
