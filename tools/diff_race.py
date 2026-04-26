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

The orchestrator:
  1. Snapshots re/tools/quickrace/td5_quickrace.ini (Frida side only).
  2. Patches the quickrace INI with the scenario so td5_quickrace.py feeds
     the same values to the original binary's Frida hook.
  3. Runs the original via td5_quickrace.py --trace --trace-auto-exit.
  4. Runs td5re.exe from the repo root with --Key=N CLI overrides carrying
     the full scenario + trace knobs; waits for its trace to flush.
  5. Invokes compare_race_trace.py with any --fields / --float-tol passed in.
  6. Restores the quickrace INI snapshot and prints the comparator output.

td5re.ini is NOT mutated — every td5re-side knob rides on the CLI so the
user's working INI survives the diff run untouched. Since CLI > INI, any
key not passed on the command line still comes from the user's td5re.ini.

The CSVs land at log/race_trace_original.csv and log/race_trace.csv.
"""

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
QUICKRACE_INI = REPO / "re" / "tools" / "quickrace" / "td5_quickrace.ini"
QUICKRACE_PY = REPO / "re" / "tools" / "quickrace" / "td5_quickrace.py"
COMPARE_PY = REPO / "tools" / "compare_race_trace.py"
TD5RE_EXE = REPO / "td5re.exe"
LOG_DIR = REPO / "log"
ORIG_CSV = LOG_DIR / "race_trace_original.csv"
PORT_CSV = LOG_DIR / "race_trace.csv"
ORIG_CALLS_CSV = LOG_DIR / "calls_trace_original.csv"
PORT_CALLS_CSV = LOG_DIR / "calls_trace.csv"


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
    # comparator knobs — defaults chosen so the first run is readable.
    ap.add_argument("--stage", default="post_progress",
                    help="comparator --stage filter (default: post_progress, "
                         "emitted once per sim tick by both sides)")
    ap.add_argument("--kind", default="actor",
                    help="comparator --kind filter (default: actor)")
    ap.add_argument("--fields", default="world_x,world_y,world_z,vel_x,vel_y,vel_z,ang_yaw",
                    help="comma-separated field whitelist passed to compare_race_trace.py")
    ap.add_argument("--key-fields", default="sim_tick,stage,kind,id",
                    help="comparator --key-fields (default ignores render-frame drift)")
    ap.add_argument("--dedupe", default="first", choices=("first", "error"),
                    help="comparator --dedupe mode (default first)")
    ap.add_argument("--float-tol", type=float, default=0.001,
                    help="per-field float tolerance for the comparator")
    ap.add_argument("--keep-traces", action="store_true",
                    help="do not rename the CSVs after the run")
    ap.add_argument("--hooks", default=None,
                    help="path to a hook-specs YAML/JSON file "
                         "(see re/trace-hooks/README.md). Frida attaches to "
                         "the listed functions; the port emits matching rows "
                         "via TD5_TRACE_CALL_ENTER/RET macros. Comparator will "
                         "diff the calls_trace CSVs in addition to the main "
                         "race_trace CSVs.")
    return ap.parse_args()


# ---------------------------------------------------------------------------
# INI snapshot / patch helpers
# ---------------------------------------------------------------------------

def snapshot(path: Path) -> bytes:
    return path.read_bytes() if path.exists() else b""


def restore(path: Path, blob: bytes):
    if blob:
        path.write_bytes(blob)


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
        # AI-drive slot 0 on the port so its trace matches the Frida side,
        # which does the same via the quickrace hook's UpdateRaceActors
        # windowed slot[0].state=0 (see reference_frida_player_is_ai.md).
        # Both sides routing slot 0 through their AI paths keeps the diff
        # focused on physics/AI parity rather than input divergence.
        "--PlayerIsAI=1",
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
        # Fast-forward: inject N extra sim ticks per render frame so the
        # port reaches the same simulated race window as the Frida-clamped
        # original in seconds of wall-clock. 4 means ~5 sim ticks per frame.
        "--TraceFastForward=4",
    ]


def patch_quickrace_ini(args):
    """Write the Frida-side INI with the requested scenario.

    Only the original binary (via td5_quickrace.py) consumes this file now;
    td5re.exe stopped reading it on 2026-04-22. We edit only the keys
    td5_quickrace.py forwards to the Frida hook.
    """
    raw = QUICKRACE_INI.read_text(encoding="utf-8")

    def _set(pattern: str, repl: str, text: str) -> str:
        return re.sub(pattern, repl, text, count=1, flags=re.MULTILINE)

    raw = _set(r"^game_type\s*=.*$",     f"game_type = {args.game_type}", raw)
    raw = _set(r"^track\s*=.*$",         f"track = {args.track}", raw)
    raw = _set(r"^laps\s*=.*$",          f"laps = {args.laps}", raw)
    raw = _set(r"^car\s*=.*$",           f"car = {args.car}", raw)
    raw = _set(r"^start_span_offset\s*=.*$",
               f"start_span_offset = {args.start_span_offset}", raw)
    # AI-drive slot 0 on the Frida side too (mirrors the port's
    # --PlayerIsAI=1). The quickrace hook windows slot[0].state=0 inside
    # UpdateRaceActors so AI dispatch fires for slot 0 each tick.
    raw = _set(r"^player_is_ai\s*=.*$",  "player_is_ai = true", raw)
    QUICKRACE_INI.write_text(raw, encoding="utf-8")


# ---------------------------------------------------------------------------
# Run helpers
# ---------------------------------------------------------------------------

def run_original(args):
    """Spawn TD5_d3d.exe with quickrace + trace, wait for auto-exit."""
    if ORIG_CSV.exists():
        ORIG_CSV.unlink()
    if ORIG_CALLS_CSV.exists():
        ORIG_CALLS_CSV.unlink()
    sim_tick_cap = int(args.seconds * 30)
    cmd = [
        sys.executable, "-u", str(QUICKRACE_PY),
        "--trace",
        "--trace-max-frames", str(args.original_frames),
        "--trace-max-sim-tick", str(sim_tick_cap),
        "--trace-auto-exit",
    ]
    if args.hooks:
        cmd += ["--hook-specs", str(args.hooks)]
    print(f"[diff_race] running original: {' '.join(cmd)}")
    # Give the spawn + race a generous timeout proportional to the seconds arg.
    timeout = max(45, int(args.seconds * 4) + 20)
    proc = subprocess.run(cmd, cwd=str(REPO), timeout=timeout)
    if proc.returncode not in (0, None):
        print(f"[diff_race] quickrace launcher exit {proc.returncode}")
    if not ORIG_CSV.exists():
        sys.exit(f"[diff_race] original trace missing after run: {ORIG_CSV}")
    size = ORIG_CSV.stat().st_size
    print(f"[diff_race] original trace: {ORIG_CSV} ({size} bytes)")


def run_port(args):
    """Spawn td5re.exe from repo root, wait for trace to flush, kill."""
    # Kill any leftover td5re.exe from a previous crashed run before we try
    # to unlink the CSV it might still be holding open.
    subprocess.run(["taskkill.exe", "/F", "/IM", "td5re.exe"],
                   capture_output=True, text=True)
    # Retry unlink a few times in case the zombie process is still flushing.
    for _ in range(10):
        try:
            if PORT_CSV.exists():
                PORT_CSV.unlink()
            if PORT_CALLS_CSV.exists():
                PORT_CALLS_CSV.unlink()
            break
        except PermissionError:
            time.sleep(0.2)
    else:
        sys.exit(f"[diff_race] could not unlink {PORT_CSV}; kill td5re.exe manually")

    # Build the CLI override list and echo it so engine.log divergences are
    # traceable to exactly the flags that triggered the run.
    cli_args = td5re_cli_args(args)
    print(f"[diff_race] td5re.exe CLI: {' '.join(cli_args)}")

    # Start via PowerShell Start-Process so we get the PID back cleanly.
    # -ArgumentList forwards the CLI flags into lpCmdLine, which
    # td5_apply_cli_overrides() parses after td5re.ini is read.
    ps_args = ",".join(f"'{a}'" for a in cli_args)
    ps = (
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

    # Poll CSV growth as a readiness signal in addition to the fixed wait.
    deadline = time.monotonic() + args.port_wait
    last_size = -1
    stable_ticks = 0
    while time.monotonic() < deadline:
        time.sleep(1.0)
        cur = PORT_CSV.stat().st_size if PORT_CSV.exists() else 0
        if cur == last_size and cur > 0:
            stable_ticks += 1
            if stable_ticks >= 2:  # ~2 seconds of stability = trace done
                break
        else:
            stable_ticks = 0
            last_size = cur

    # Kill the game regardless.
    subprocess.run(
        ["taskkill.exe", "/F", "/PID", str(pid_int)],
        capture_output=True, text=True,
    )
    if not PORT_CSV.exists():
        sys.exit(f"[diff_race] port trace missing after run: {PORT_CSV}")
    size = PORT_CSV.stat().st_size
    print(f"[diff_race] port trace: {PORT_CSV} ({size} bytes)")


def run_compare(args):
    cmd = [
        sys.executable, "-u", str(COMPARE_PY),
        str(ORIG_CSV), str(PORT_CSV),
        "--float-tol", str(args.float_tol),
    ]
    if args.fields:
        cmd += ["--fields", args.fields]
    if args.stage:
        cmd += ["--stage", args.stage]
    if args.kind:
        cmd += ["--kind", args.kind]
    if args.key_fields:
        cmd += ["--key-fields", args.key_fields]
    if args.dedupe:
        cmd += ["--dedupe", args.dedupe]
    print(f"[diff_race] comparing: {' '.join(cmd)}")
    return subprocess.run(cmd, cwd=str(REPO))


def run_compare_calls(args):
    """Run a second comparator pass on calls_trace_*.csv when --hooks is set.
    Uses a separate key schema (sim_tick, fn_name, call_idx) and diffs every
    arg_N column plus ret/has_ret. Float-tol is still applied in case args
    contain reinterpret-cast float bits that match closely enough."""
    if not args.hooks:
        return None
    if not ORIG_CALLS_CSV.exists() or not PORT_CALLS_CSV.exists():
        print("[diff_race] calls_trace CSVs missing — skipping calls compare")
        print(f"  original: exists={ORIG_CALLS_CSV.exists()} ({ORIG_CALLS_CSV})")
        print(f"  port:     exists={PORT_CALLS_CSV.exists()} ({PORT_CALLS_CSV})")
        return None
    cmd = [
        sys.executable, "-u", str(COMPARE_PY),
        str(ORIG_CALLS_CSV), str(PORT_CALLS_CSV),
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

    if not TD5RE_EXE.exists():
        sys.exit(f"td5re.exe missing at repo root: {TD5RE_EXE}")
    if not QUICKRACE_PY.exists() or not COMPARE_PY.exists():
        sys.exit("missing td5_quickrace.py or compare_race_trace.py")

    LOG_DIR.mkdir(exist_ok=True)

    # Only the Frida-side INI needs snapshot/restore now — td5re reads its
    # scenario from CLI overrides so its INI is never touched.
    qr_snap = snapshot(QUICKRACE_INI)

    try:
        patch_quickrace_ini(args)

        print(f"[diff_race] scenario: car={args.car} track={args.track} "
              f"game_type={args.game_type} laps={args.laps} "
              f"start_span_offset={args.start_span_offset} "
              f"seconds={args.seconds} (sim_tick_cap={int(args.seconds * 30)})")

        run_original(args)
        run_port(args)
        compare = run_compare(args)
        calls_compare = run_compare_calls(args)
        if calls_compare and calls_compare.returncode != 0 and compare.returncode == 0:
            # If the main compare passed but the calls compare found a
            # divergence, surface that as the overall failure.
            compare = calls_compare

    finally:
        restore(QUICKRACE_INI, qr_snap)
        print("[diff_race] restored quickrace INI snapshot")

    return compare.returncode if compare else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
