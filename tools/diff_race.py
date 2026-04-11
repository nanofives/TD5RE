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
  1. Snapshots td5re.ini and re/tools/quickrace/td5_quickrace.ini.
  2. Patches both for a deterministic, skip-frontend, auto-throttle run.
  3. Runs the original via td5_quickrace.py --trace --trace-auto-exit.
  4. Runs td5re.exe from the repo root; waits for its trace to flush.
  5. Invokes compare_race_trace.py with any --fields / --float-tol passed in.
  6. Restores the INI snapshots and prints the comparator output.

The CSVs land at log/race_trace_original.csv and log/race_trace.csv.
"""

import argparse
import configparser
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
TD5RE_INI = REPO / "td5re.ini"
QUICKRACE_INI = REPO / "re" / "tools" / "quickrace" / "td5_quickrace.ini"
QUICKRACE_PY = REPO / "re" / "tools" / "quickrace" / "td5_quickrace.py"
COMPARE_PY = REPO / "tools" / "compare_race_trace.py"
TD5RE_EXE = REPO / "td5re.exe"
LOG_DIR = REPO / "log"
ORIG_CSV = LOG_DIR / "race_trace_original.csv"
PORT_CSV = LOG_DIR / "race_trace.csv"


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
    return ap.parse_args()


# ---------------------------------------------------------------------------
# INI snapshot / patch helpers
# ---------------------------------------------------------------------------

def snapshot(path: Path) -> bytes:
    return path.read_bytes() if path.exists() else b""


def restore(path: Path, blob: bytes):
    if blob:
        path.write_bytes(blob)


def patch_td5re_ini(args):
    """Ensure td5re.ini has the trace + auto-race knobs we need for one run."""
    cp = configparser.ConfigParser()
    cp.optionxform = str  # preserve case
    cp.read(TD5RE_INI, encoding="utf-8")
    if not cp.has_section("Trace"):
        cp.add_section("Trace")
    cp.set("Trace", "RaceTrace", "1")
    # -1 = all slots; must match the Frida trace script's TRACE_SLOT = -1
    # default, otherwise the comparator keys miss every AI actor row.
    cp.set("Trace", "RaceTraceSlot", "-1")
    cp.set("Trace", "RaceTraceMaxFrames", str(args.port_frames))
    cp.set("Trace", "AutoThrottle", "1")
    # Fast-forward: inject N extra sim ticks per render frame so the port
    # reaches the same simulated race window as the Frida-clamped original
    # in a few seconds of wall clock. 4 means ~5 sim ticks per frame total.
    cp.set("Trace", "TraceFastForward", "4")
    # Sim-tick cap — stop writing rows once we have enough sim ticks to
    # cover the common capture window. sim_tick increments at 30 Hz once
    # the countdown ends, so N = seconds * 30 is "N in-game seconds of
    # race after GO". The render-frame cap above is just a safety ceiling.
    cp.set("Trace", "RaceTraceMaxSimTicks", str(int(args.seconds * 30)))

    if not cp.has_section("Game"):
        cp.add_section("Game")
    cp.set("Game", "SkipIntro", "1")
    with TD5RE_INI.open("w", encoding="utf-8") as f:
        cp.write(f)


def patch_quickrace_ini(args):
    """Write the shared INI with skip_frontend=true and the requested scenario."""
    raw = QUICKRACE_INI.read_text(encoding="utf-8")

    def _set(pattern: str, repl: str, text: str) -> str:
        return re.sub(pattern, repl, text, count=1, flags=re.MULTILINE)

    raw = _set(r"^skip_frontend\s*=.*$", "skip_frontend = true", raw)
    raw = _set(r"^game_type\s*=.*$",     f"game_type = {args.game_type}", raw)
    raw = _set(r"^track\s*=.*$",         f"track = {args.track}", raw)
    raw = _set(r"^laps\s*=.*$",          f"laps = {args.laps}", raw)
    raw = _set(r"^car\s*=.*$",           f"car = {args.car}", raw)
    QUICKRACE_INI.write_text(raw, encoding="utf-8")


# ---------------------------------------------------------------------------
# Run helpers
# ---------------------------------------------------------------------------

def run_original(args):
    """Spawn TD5_d3d.exe with quickrace + trace, wait for auto-exit."""
    if ORIG_CSV.exists():
        ORIG_CSV.unlink()
    sim_tick_cap = int(args.seconds * 30)
    cmd = [
        sys.executable, "-u", str(QUICKRACE_PY),
        "--trace",
        "--trace-max-frames", str(args.original_frames),
        "--trace-max-sim-tick", str(sim_tick_cap),
        "--trace-auto-exit",
    ]
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
            break
        except PermissionError:
            time.sleep(0.2)
    else:
        sys.exit(f"[diff_race] could not unlink {PORT_CSV}; kill td5re.exe manually")

    # Sanity-check that the td5re.ini patch actually landed on disk.
    cp = configparser.ConfigParser()
    cp.optionxform = str
    cp.read(TD5RE_INI, encoding="utf-8")
    mf = cp.get("Trace", "RaceTraceMaxFrames", fallback="<missing>")
    ar = cp.get("Game", "AutoRace", fallback="<missing>")
    at = cp.get("Trace", "AutoThrottle", fallback="<missing>")
    print(f"[diff_race] td5re.ini on disk: RaceTraceMaxFrames={mf} "
          f"AutoThrottle={at} AutoRace={ar}")

    # Start via PowerShell Start-Process so we get the PID back cleanly.
    ps = (
        "$p = Start-Process -FilePath "
        f"'{TD5RE_EXE}' -WorkingDirectory '{REPO}' -PassThru; "
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

    td5re_snap = snapshot(TD5RE_INI)
    qr_snap = snapshot(QUICKRACE_INI)

    try:
        patch_td5re_ini(args)
        patch_quickrace_ini(args)

        print(f"[diff_race] scenario: car={args.car} track={args.track} "
              f"game_type={args.game_type} laps={args.laps} "
              f"seconds={args.seconds} (sim_tick_cap={int(args.seconds * 30)})")

        run_original(args)
        run_port(args)
        compare = run_compare(args)

    finally:
        restore(TD5RE_INI, td5re_snap)
        restore(QUICKRACE_INI, qr_snap)
        print("[diff_race] restored INI snapshots")

    return compare.returncode if compare else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
