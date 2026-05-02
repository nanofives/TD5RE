"""
diff_race.py
============
Run the same race scenario in the original TD5_d3d.exe and the td5re source
port, capture race traces from both, and diff them with
tools/compare_race_trace.py.

Invoked by .claude/commands/diff-race.md. Can also be run directly:

  python tools/diff_race.py --car 0 --track 0
  python tools/diff_race.py --car 8 --track 12 --frames 300 \
      --fields world_x,world_y,world_z,vel_x,vel_y,vel_z

Trace window:
  Countdown = TD5_COUNTDOWN_INIT / TD5_COUNTDOWN_DECR = 0xA000 / 0x100 = 160 sim ticks.
  At 30 Hz that's 5.33 s, so 3 s of post-countdown racing = +90 ticks.
  Default cap = 300 frames (250 target + 50 frames of loading-fade margin).

The orchestrator (parallel-safe — never mutates td5_quickrace.ini):
  1. Forwards the scenario as first-class CLI flags to td5_quickrace.py
     (--track / --car / --game-type / --laps / --start-span-offset /
     --player-is-ai). The Frida hook receives the same values via
     script.post({type:'cfg', cfg}) so two parallel sessions can race
     different scenarios concurrently without colliding.
  2. Spawns the original via td5_quickrace.py --trace --trace-auto-exit and
     a per-session --csv-out so CSVs don't collide either.
  3. Runs td5re.exe from the repo root with --Key=N CLI overrides carrying
     the full scenario + trace knobs; waits for its trace to flush.
  4. Invokes compare_race_trace.py with any --fields / --float-tol passed in.

Neither td5re.ini nor td5_quickrace.ini is mutated — every knob rides on
the CLI on both sides. The td5re-side CLI overrides win over td5re.ini
(CLI > INI), so the user's working INI survives the diff run untouched.

CSV paths default to log/race_trace_original.csv + log/race_trace.csv
(legacy single-session). Pass --session-tag <tag> to suffix both with
the tag for parallel runs (log/race_trace_original_<tag>.csv etc.).
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
QUICKRACE_PY = REPO / "re" / "tools" / "quickrace" / "td5_quickrace.py"
COMPARE_PY = REPO / "tools" / "compare_race_trace.py"
TD5RE_EXE = REPO / "td5re.exe"
LOG_DIR = REPO / "log"

sys.path.insert(0, str(HERE))
import diff_profiles  # noqa: E402  (sibling module under tools/)


def _csv_path(name: str, tag: str | None) -> Path:
    """Per-session CSV path. ``name`` is the bare stem before the optional
    ``_<tag>`` suffix, e.g. _csv_path('race_trace_original', 'moscow-3') →
    log/race_trace_original_moscow-3.csv. ``tag=None`` keeps the legacy
    single-session filename for backwards compatibility with existing
    tooling that hardcodes log/race_trace*.csv."""
    if tag:
        return LOG_DIR / f"{name}_{tag}.csv"
    return LOG_DIR / f"{name}.csv"


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--car", type=int, default=0,
                    help="external car ID (see td5_quickrace.ini reference)")
    ap.add_argument("--track", type=int, default=0,
                    help="track schedule index")
    ap.add_argument("--game-type", type=int, default=0,
                    help="0=single race (default); see quickrace.ini for others")
    ap.add_argument("--laps", type=int, default=1,
                    help="circuit laps (capture only ~3 s so 1 is plenty)")
    ap.add_argument("--start-span-offset", type=int, default=0,
                    help="shift every actor's spawn span by this many units "
                         "along the track ring. Forwarded to the shared "
                         "quickrace INI's [race] start_span_offset. Both the "
                         "Frida hook on InitializeActorTrackPose and the port "
                         "spawn loop apply it. Default 0 = vanilla grid.")
    ap.add_argument("--seconds", type=float, default=15.0,
                    help="in-game race seconds to capture after countdown ends. "
                         "At 30 Hz sim rate this becomes sim_tick = seconds * 30 "
                         "on both binaries — the common engine-clock cap that "
                         "lets the comparator align rows deterministically.")
    ap.add_argument("--original-frames", type=int, default=3000,
                    help="render-frame safety ceiling for the Frida trace "
                         "(real cap is --seconds via sim_tick)")
    ap.add_argument("--port-frames", type=int, default=3000,
                    help="render-frame safety ceiling for td5re.exe "
                         "(real cap is --seconds via RaceTraceMaxSimTicks)")
    ap.add_argument("--port-wait", type=float, default=20.0,
                    help="hard cap seconds to wait for td5re.exe trace to flush")
    # comparator knobs — defaults are driven by --profile; pass
    # --modules/--stages/--fields explicitly to override the profile slice.
    ap.add_argument("--profile", default="core",
                    help="comma-separated profile names from tools/diff_profiles.py "
                         "(default: core). Each profile resolves to a "
                         "(modules, stages, fields) slice for one subsystem. "
                         "Multiple profiles compose. Run "
                         "`python tools/diff_profiles.py list` for the catalog.")
    ap.add_argument("--list-profiles", action="store_true",
                    help="print the profile catalog and exit (no race run).")
    ap.add_argument("--modules", default=None,
                    help="comma list of trace modules to capture + diff "
                         "(frame,pose,motion,track,controls,progress,view,calls). "
                         "Overrides the profile-resolved modules.")
    ap.add_argument("--stages", default=None,
                    help="comma list of stages to keep within each module CSV. "
                         "Overrides the profile-resolved stages.")
    ap.add_argument("--fields", default=None,
                    help="comma-separated non-key field allowlist applied to "
                         "every per-module compare. Overrides the "
                         "profile-resolved fields when set.")
    ap.add_argument("--dedupe", default="first", choices=("first", "error"),
                    help="comparator --dedupe mode (default first)")
    ap.add_argument("--float-tol", type=float, default=None,
                    help="per-field float tolerance for the comparator. Overrides "
                         "the profile-resolved tolerance. Falls back to 0.001.")
    ap.add_argument("--keep-traces", action="store_true",
                    help="do not rename the CSVs after the run")
    ap.add_argument("--hooks", default=None,
                    help="path to a hook-specs YAML/JSON file "
                         "(see re/trace-hooks/README.md). Frida attaches to "
                         "the listed functions; the port emits matching rows "
                         "via TD5_TRACE_CALL_ENTER/RET macros. Comparator will "
                         "diff the calls_trace CSVs in addition to the main "
                         "race_trace CSVs.")
    ap.add_argument("--session-tag", default=None,
                    help="suffix CSV filenames with this tag so multiple "
                         "diff_race sessions can run concurrently without "
                         "their traces colliding. Example: --session-tag "
                         "moscow-attempt-3 produces "
                         "log/race_trace_original_moscow-attempt-3.csv + "
                         "log/race_trace_moscow-attempt-3.csv. When omitted, "
                         "the legacy single-session paths are used.")
    ap.add_argument("--player-is-ai", dest="player_is_ai",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=True,
                    help="route slot 0 through AI dispatch on both sides "
                         "(true|false; default true). Set false to probe "
                         "the human-input path.")
    ap.add_argument("--attach-pid", dest="attach_pid", type=int, default=None,
                    help="attach to a manually-launched TD5_d3d.exe PID "
                         "instead of frida.spawn(). Use when frida.spawn "
                         "crashes (e.g., Win11 M2DX init regression). "
                         "Launch the game manually first, hold it on the "
                         "legal screen, then pass the PID.")
    ap.add_argument("--attach-name", dest="attach_name", default=None,
                    help="like --attach-pid but locate the PID by process "
                         "name (default: TD5_d3d.exe).")
    return ap.parse_args()


# ---------------------------------------------------------------------------
# CLI arg builders
# ---------------------------------------------------------------------------

def td5re_cli_args(args):
    """Build the --Key=N list that pins every td5re-side knob for one run.

    The port reads all scenario + trace knobs through the CLI overrides
    (see reference_td5re_cli_overrides.md), so we never touch td5re.ini.
    The defaults here mirror the faithful Frida-hook sequence on the
    original (ConfigureGameTypeFlags → InitializeRaceSeriesSchedule →
    InitializeFrontendDisplayModeState) and skip the wall-clock srand
    reseed when RaceTrace=1 so both binaries spawn with the same RNG.
    """
    sim_tick_cap = int(args.seconds * 30)
    return [
        # Scenario
        f"--DefaultCar={args.car}",
        f"--DefaultTrack={args.track}",
        f"--DefaultGameType={args.game_type}",
        f"--Laps={args.laps}",
        f"--StartSpanOffset={args.start_span_offset}",
        # Auto-race path (no frontend input)
        "--AutoRace=1",
        "--SkipIntro=1",
        # AI-drive slot 0 (default): matches the Frida side's UpdateRaceActors
        # slot[0].state=0 windowing so the diff focuses on physics/AI parity
        # rather than input divergence. Pass --player-is-ai false to probe
        # the human-input path on both binaries.
        f"--PlayerIsAI={1 if args.player_is_ai else 0}",
        # Trace knobs
        "--RaceTrace=1",
        # -1 = all slots; must match the Frida trace script's TRACE_SLOT=-1
        # default, otherwise the comparator keys miss every AI actor row.
        "--RaceTraceSlot=-1",
        f"--RaceTraceMaxFrames={args.port_frames}",
        # Sim-tick cap — stop writing rows once we have enough sim ticks to
        # cover the common capture window. sim_tick increments at 30 Hz
        # once the countdown ends, so N = seconds * 30 is "N in-game
        # seconds of race after GO". RaceTraceMaxFrames is a safety ceiling.
        f"--RaceTraceMaxSimTicks={sim_tick_cap}",
        "--AutoThrottle=1",
        # Fast-forward: speed multiplier for sim time vs wall clock. 4.0
        # means 4x sim throughput so the port reaches the same simulated
        # race window as the Frida-clamped original in ~25% the wall time.
        "--TraceFastForward=4.0",
        # Modular trace selection — only emit the per-module rows the
        # current profile asks for.
        f"--TraceModules={args.modules}",
        f"--TraceStages={args.stages}",
    ]


def quickrace_cli_args(args, paths):
    """Build the td5_quickrace.py CLI for the original binary side.

    Forwards scenario as first-class flags (--track / --car / --game-type /
    --laps / --start-span-offset / --player-is-ai), so the Frida-side INI
    is NEVER mutated. The hook reads these via script.post({type:'cfg'}).
    Per-module CSV destinations and the modules/stages mask are all passed
    via --modules/--stages/--module-csv-base; the launcher patches the
    Frida script's MODULE_PATHS / MODULE_MASK / STAGE_MASK constants in
    place before injection.
    """
    sim_tick_cap = int(args.seconds * 30)
    cmd = [
        sys.executable, "-u", str(QUICKRACE_PY),
        "--trace",
        "--trace-max-frames", str(args.original_frames),
        "--trace-max-sim-tick", str(sim_tick_cap),
        "--trace-auto-exit",
        # --no-ini hermetically seals this run from td5_quickrace.ini so a
        # parallel interactive session that left frontend_only=true (or any
        # other stale field) in the INI cannot redirect this run into the
        # main menu. We pass every scenario value explicitly below.
        "--no-ini",
        # Scenario (first-class flags — no INI read, no INI write)
        "--track", str(args.track),
        "--car", str(args.car),
        "--game-type", str(args.game_type),
        "--laps", str(args.laps),
        "--start-span-offset", str(args.start_span_offset),
        "--player-is-ai", "true" if args.player_is_ai else "false",
        "--frontend-only", "false",
        "--frontend-screen", "-1",
        # Modular trace selection. The launcher resolves per-module paths
        # under <base>_<module>.csv (e.g. log/race_trace_original_pose.csv)
        # and patches MODULE_PATHS in the Frida script.
        "--trace-modules", args.modules,
        "--trace-stages",  args.stages,
        "--module-csv-base", str(paths["base_original"]),
        # Calls trace stays in its own file with the legacy schema.
        "--calls-csv-out", str(paths["calls_original"]),
    ]
    if args.hooks:
        cmd += ["--hook-specs", str(args.hooks)]
    if args.attach_pid is not None:
        cmd += ["--attach-pid", str(args.attach_pid)]
    elif args.attach_name is not None:
        cmd += ["--attach-name", args.attach_name]
    return cmd


# Modules whose row schemas live in the main per-module CSV table (i.e. NOT
# the calls trace, which has its own separate file). Used by csv_paths +
# the comparator dispatcher.
SIM_MODULES = ("frame", "pose", "motion", "track",
               "controls", "progress", "view")


def csv_paths(args):
    """Resolve every CSV destination for this run, honoring --session-tag.

    Returns a dict keyed:
      base_original / base_port — directory + stem prefix; each side appends
                                  ``_<module>.csv``.
      original_<mod> / port_<mod>  for every module in SIM_MODULES.
      calls_original / calls_port  for the function-call trace.
    """
    base_orig = _csv_path("race_trace_original", args.session_tag).with_suffix("")
    base_port = _csv_path("race_trace",          args.session_tag).with_suffix("")
    out = {
        "base_original":  base_orig,
        "base_port":      base_port,
        "calls_original": _csv_path("calls_trace_original", args.session_tag),
        "calls_port":     _csv_path("calls_trace",          args.session_tag),
    }
    for mod in SIM_MODULES:
        out[f"original_{mod}"] = base_orig.parent / f"{base_orig.name}_{mod}.csv"
        out[f"port_{mod}"]     = base_port.parent / f"{base_port.name}_{mod}.csv"
    return out


# ---------------------------------------------------------------------------
# Run helpers
# ---------------------------------------------------------------------------

def _active_modules(args):
    """Return the modules selected for this run (sim modules only — calls
    is handled separately via --hooks)."""
    if not args.modules or args.modules in ("*", ""):
        return list(SIM_MODULES)
    asked = [m.strip() for m in args.modules.split(",") if m.strip() and m.strip() != "*"]
    return [m for m in asked if m in SIM_MODULES]


def run_original(args, paths):
    """Launch TD5_d3d.exe (detached) + attach Frida; wait for auto-exit.

    On Win11 `frida.spawn()` of TD5_d3d.exe trips an M2DX.dll NULL-deref at
    +0x144B because the spawned child inherits the Frida parent's audio
    session in a way M2DX init does not survive. The working recipe is to
    launch the game via Explorer-style `cmd /c start "" TD5_d3d.exe` (so it
    inherits the shell's audio session, not Frida's) and then attach by
    process name. This function does that automatically when the caller
    didn't pass --attach-pid / --attach-name. Pass either flag to override.
    """
    # Wipe stale per-module CSVs so we never silently compare a fresh run
    # against an old companion file from a previous diff session.
    for mod in _active_modules(args):
        p = paths[f"original_{mod}"]
        if p.exists():
            p.unlink()

    cmd = quickrace_cli_args(args, paths)
    print(f"[diff_race] running original: {' '.join(cmd)}")
    timeout = max(45, int(args.seconds * 4) + 20)
    proc = subprocess.run(cmd, cwd=str(REPO), timeout=timeout)
    if proc.returncode not in (0, None):
        print(f"[diff_race] quickrace launcher exit {proc.returncode}")
    # Verify at least one expected module CSV materialized — the Frida side
    # may have closed early on a race-end transition; we surface that here
    # rather than letting the comparator complain about missing files.
    found = [m for m in _active_modules(args) if paths[f"original_{m}"].exists()]
    if not found:
        sys.exit(f"[diff_race] no original trace files materialized "
                 f"(expected under {paths['base_original']}_*.csv)")
    for m in found:
        p = paths[f"original_{m}"]
        print(f"[diff_race] original {m}: {p} ({p.stat().st_size} bytes)")


def run_port(args, paths):
    """Spawn td5re.exe from repo root, wait for trace to flush, kill."""
    active = _active_modules(args)
    port_calls_csv = paths["calls_port"]

    # Wipe stale module CSVs so a previous run's data can't leak into
    # this comparison. Retry unlink a few times — the previous td5re may
    # still be flushing on slow disks.
    for _ in range(10):
        try:
            for mod in active:
                p = paths[f"port_{mod}"]
                if p.exists():
                    p.unlink()
            if port_calls_csv.exists():
                port_calls_csv.unlink()
            break
        except PermissionError:
            time.sleep(0.2)
    else:
        sys.exit(f"[diff_race] could not unlink stale port traces under "
                 f"{paths['base_port']}_*.csv; kill td5re.exe manually")

    cli_args = td5re_cli_args(args)
    print(f"[diff_race] td5re.exe CLI: {' '.join(cli_args)}")

    # Per-module env-var overrides for the port-side trace destinations.
    # td5_trace.c's module_open() reads TD5RE_TRACE_<MOD>_PATH (uppercase)
    # before falling back to log/race_trace_<mod>.csv. Calls trace uses
    # the legacy TD5RE_CALLS_TRACE_PATH.
    env_lines = []
    for mod in active:
        env_lines.append(
            f"$env:TD5RE_TRACE_{mod.upper()}_PATH = '{paths[f'port_{mod}']}'; "
        )
    env_lines.append(f"$env:TD5RE_CALLS_TRACE_PATH = '{port_calls_csv}'; ")

    ps_args = ",".join(f"'{a}'" for a in cli_args)
    ps = (
        "".join(env_lines) +
        "$p = Start-Process -FilePath "
        f"'{TD5RE_EXE}' -WorkingDirectory '{REPO}' "
        f"-ArgumentList {ps_args} "
        "-PassThru; "
        "Write-Output $p.Id"
    )
    out = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", ps],
        capture_output=True, text=True, timeout=30,
    )
    pid = out.stdout.strip().splitlines()[-1].strip() if out.stdout else ""
    if not pid.isdigit():
        sys.exit(f"[diff_race] failed to spawn td5re.exe: stdout={out.stdout} stderr={out.stderr}")
    pid_int = int(pid)
    print(f"[diff_race] spawned td5re.exe pid={pid_int}; waiting {args.port_wait:.0f}s for trace")

    # Stability poll on the largest active module CSV — if its size is
    # stable for ~2 s we treat the run as done. Larger files take the
    # longest to flush so they're the right end-of-write signal.
    def _stability_target():
        existing = [paths[f"port_{m}"] for m in active if paths[f"port_{m}"].exists()]
        if not existing:
            return None
        return max(existing, key=lambda p: p.stat().st_size)

    deadline = time.monotonic() + args.port_wait
    last_size = -1
    stable_ticks = 0
    while time.monotonic() < deadline:
        time.sleep(1.0)
        target = _stability_target()
        cur = target.stat().st_size if target else 0
        if cur == last_size and cur > 0:
            stable_ticks += 1
            if stable_ticks >= 2:
                break
        else:
            stable_ticks = 0
            last_size = cur

    subprocess.run(
        ["taskkill.exe", "/F", "/PID", str(pid_int)],
        capture_output=True, text=True,
    )
    found = [m for m in active if paths[f"port_{m}"].exists()]
    if not found:
        sys.exit(f"[diff_race] no port trace files materialized "
                 f"(expected under {paths['base_port']}_*.csv)")
    for m in found:
        p = paths[f"port_{m}"]
        print(f"[diff_race] port {m}: {p} ({p.stat().st_size} bytes)")


def resolve_profile(args):
    """Apply the --profile selection to the orchestrator knobs.

    Profile-resolved modules/stages/fields/float-tol fill in only when the
    user didn't pass an explicit override. Mutates ``args`` in place and
    returns the resolved profile dict for logging.
    """
    profile_names = diff_profiles.parse_profile_arg(args.profile)
    resolved = diff_profiles.resolve(profile_names)
    if args.modules is None:
        args.modules = resolved["modules"] or "*"
    if args.stages is None:
        args.stages = resolved["stages"] or "*"
    if args.fields is None:
        args.fields = resolved["fields"]
    if args.float_tol is None:
        args.float_tol = resolved["float_tol"] if resolved["float_tol"] is not None else 0.001
    return resolved


def run_compare(args, paths):
    """Run compare_race_trace.py once per active sim module. Each module's
    CSVs use a distinct narrow key schema (see diff_profiles.MODULE_KEYS)
    so the comparator is invoked with the right --key-fields per module.

    Returns a CompletedProcess stand-in whose returncode is the union of
    every per-module returncode (zero only if every module passed)."""
    overall_rc = 0
    for mod in _active_modules(args):
        orig = paths[f"original_{mod}"]
        port = paths[f"port_{mod}"]
        if not orig.exists() or not port.exists():
            print(f"[diff_race] {mod}: skipping — file missing "
                  f"(orig={orig.exists()} port={port.exists()})")
            overall_rc = overall_rc or 1
            continue
        key_fields = ",".join(diff_profiles.MODULE_KEYS[mod])
        cmd = [
            sys.executable, "-u", str(COMPARE_PY),
            str(orig), str(port),
            "--float-tol", str(args.float_tol),
            "--key-fields", key_fields,
        ]
        # Profile-resolved fields are a UNION across the active modules; the
        # comparator runs per-module with that module's narrow schema, so we
        # must intersect to drop fields that don't belong to this module.
        # Empty intersection ⇒ skip --fields entirely (compare every column
        # in this module's schema).
        if args.fields:
            wanted = [f for f in args.fields.split(",") if f]
            allowed = set(diff_profiles.MODULE_COLUMNS.get(mod, []))
            kept = [f for f in wanted if f in allowed]
            if kept:
                cmd += ["--fields", ",".join(kept)]
        if args.stages and args.stages not in ("*", ""):
            cmd += ["--stage", args.stages]
        if args.dedupe:
            cmd += ["--dedupe", args.dedupe]
        print(f"[diff_race] comparing {mod}: {' '.join(cmd)}")
        rc = subprocess.run(cmd, cwd=str(REPO)).returncode
        overall_rc = overall_rc or rc

    class _Result:
        pass
    r = _Result()
    r.returncode = overall_rc
    return r


def run_compare_calls(args, paths):
    """Run a second comparator pass on calls_trace_*.csv when --hooks is set.
    Uses a separate key schema (sim_tick, fn_name, call_idx) and diffs every
    arg_N column plus ret/has_ret. Float-tol is still applied in case args
    contain reinterpret-cast float bits that match closely enough."""
    if not args.hooks:
        return None
    orig_calls = paths["calls_original"]
    port_calls = paths["calls_port"]
    if not orig_calls.exists() or not port_calls.exists():
        print("[diff_race] calls_trace CSVs missing — skipping calls compare")
        print(f"  original: exists={orig_calls.exists()} ({orig_calls})")
        print(f"  port:     exists={port_calls.exists()} ({port_calls})")
        return None
    cmd = [
        sys.executable, "-u", str(COMPARE_PY),
        str(orig_calls), str(port_calls),
        "--key-fields", "sim_tick,fn_name,call_idx",
        "--float-tol", str(args.float_tol),
        "--dedupe", args.dedupe,
    ]
    print(f"[diff_race] comparing calls: {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=str(REPO))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    args = parse_args()

    if args.list_profiles:
        print(diff_profiles.list_profiles())
        return 0

    if not TD5RE_EXE.exists():
        sys.exit(f"td5re.exe missing at repo root: {TD5RE_EXE}")
    if not QUICKRACE_PY.exists() or not COMPARE_PY.exists():
        sys.exit("missing td5_quickrace.py or compare_race_trace.py")

    LOG_DIR.mkdir(exist_ok=True)

    try:
        resolved = resolve_profile(args)
    except ValueError as exc:
        sys.exit(f"[diff_race] {exc}")

    paths = csv_paths(args)

    print(f"[diff_race] scenario: car={args.car} track={args.track} "
          f"game_type={args.game_type} laps={args.laps} "
          f"start_span_offset={args.start_span_offset} "
          f"seconds={args.seconds} (sim_tick_cap={int(args.seconds * 30)}) "
          f"player_is_ai={args.player_is_ai}"
          + (f" tag={args.session_tag}" if args.session_tag else ""))
    print(f"[diff_race] profile={','.join(resolved['profiles'])} "
          f"modules={args.modules} stages={args.stages} "
          f"float_tol={args.float_tol} "
          f"fields={args.fields or '(all)'}")
    print(f"[diff_race] CSV paths:")
    for k, v in paths.items():
        print(f"  {k:>20s} = {v}")

    run_original(args, paths)
    run_port(args, paths)
    compare = run_compare(args, paths)
    calls_compare = run_compare_calls(args, paths)
    if calls_compare and calls_compare.returncode != 0 and compare.returncode == 0:
        # If the main compare passed but the calls compare found a
        # divergence, surface that as the overall failure.
        compare = calls_compare

    return compare.returncode if compare else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
