#!/usr/bin/env python3
"""
frida_differential_capture.py -- Simultaneous user-driven Frida differential capture
across original/TD5_d3d.exe and td5re.exe for the SAME scenario.

Closes the long-tail RE for:
  #7  chassis launches  -> --target chassis-launch  (Edinburgh AI Viper)
  #8  cascade / yaw-spin -> --target cascade        (Moscow AI Viper)

The hook scripts (tools/frida_chassis_probe.js, tools/frida_cascade_probe.js)
emit two CSVs per run -- one per binary -- with identical schemas, keyed by
(binary, slot, sub_tick). The diff is "first sub_tick where any field
diverges" and "which actor / RS field diverged first".

------------------------------------------------------------------------------
Why this exists (and what static analysis missed)
------------------------------------------------------------------------------

Multiple prior agents verified td5_physics.c's RefreshVehicleWheelContactFrames
and td5_ai.c's FindActorTrackOffsetPeer are byte-faithful with the original at
the C-statement level. Yet the port still produces different runtime values
in the same function. The only remaining explanations are:
    * upstream value (an actor field) diverges before this fn is entered
    * call order differs between binaries
    * a missing global write at this function's call site
    * floating-point precision compounds across iterations into a control-flow
      boundary

Static analysis can't catch any of these.  This tool runs both binaries on
identically-configured scenarios and captures state at the failing tick to
isolate the first divergent runtime field.

------------------------------------------------------------------------------
Architectural notes
------------------------------------------------------------------------------

* The script CANNOT spawn both binaries truly simultaneously (Windows
  desktop, single-foreground game loop).  It launches them sequentially
  and overlays the resulting CSVs by (slot, sub_tick).  Determinism
  comes from the quickrace hook seeding the CRT RNG identically for
  both runs.
* Frida attaches AFTER spawn via the same poll-attach pattern used in
  scripts/capture_snapshot_pool.py (per
  memory/reference_m2dx_crash_2026-04-29.md).
* Original-binary configuration uses td5_quickrace.py (Frida-injected
  config); the port uses td5re.ini patching (AutoRace=1).
* Output goes to log/diff_<target>_<timestamp>.csv with rows interleaved
  by (binary, slot, sub_tick).  A small Python merger combines the two
  per-binary CSVs into the final differential output.

Usage:
    python scripts/frida_differential_capture.py --target chassis-launch
    python scripts/frida_differential_capture.py --target cascade --duration 45
    python scripts/frida_differential_capture.py --target chassis-launch --dry-run

The user MUST run this from an interactive desktop session (foreground
windowing required).  Frida runs against the in-process binary -- no
frida-server install required, just the `frida` Python package.
"""

from __future__ import annotations

import argparse
import configparser
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Tuple

PROJECT_ROOT = Path(__file__).resolve().parent.parent
ORIGINAL_DIR = PROJECT_ROOT / "original"
ORIG_EXE     = ORIGINAL_DIR / "TD5_d3d.exe"
PORT_EXE     = PROJECT_ROOT / "td5re.exe"
PORT_INI     = PROJECT_ROOT / "td5re.ini"
LOG_DIR      = PROJECT_ROOT / "log"

# Hook scripts live in tools/ (gitignore exceptions added).
CHASSIS_JS = PROJECT_ROOT / "tools" / "frida_chassis_probe.js"
CASCADE_JS = PROJECT_ROOT / "tools" / "frida_cascade_probe.js"

# Quickrace dispatcher used for orig (it spawns + injects + hooks).
QUICKRACE_PY  = PROJECT_ROOT / "re" / "tools" / "quickrace" / "td5_quickrace.py"
QUICKRACE_HOOK = PROJECT_ROOT / "re" / "tools" / "quickrace" / "td5_quickrace_hook.js"


# ----------------------------------------------------------------------------
# Targets
# ----------------------------------------------------------------------------

@dataclass
class Target:
    name: str
    description: str
    track: int                # quickrace track id
    car: int                  # 0 = Viper
    player_is_ai: bool        # always True for these targets
    game_type: int            # 0 = race
    hook_script: Path
    # CSV header is contributed by the hook script itself (line 1 of each
    # per-binary CSV); we just record what the comparator expects to find.
    csv_header: str


TARGETS = {
    "chassis-launch": Target(
        name="chassis-launch",
        description=(
            "Edinburgh AI Viper. Hooks RefreshVehicleWheelContactFrames "
            "@ 0x00403720 onEnter/onLeave for all 6 racer slots. Captures "
            "actor pose + per-wheel ground_y + wheel_contact_bitmask. "
            "Diagnoses #7 chassis launches (todo_edinburgh_chassis_launch_*)."
        ),
        track=1, car=0, player_is_ai=True, game_type=0,
        hook_script=CHASSIS_JS,
        csv_header="binary,slot,sub_tick,world_x,world_y,world_z,vx,vy,vz,"
                   "ground_FL,ground_FR,ground_RL,ground_RR,wcb,scf",
    ),
    "cascade": Target(
        name="cascade",
        description=(
            "Moscow AI Viper. Hooks FindActorTrackOffsetPeer @ 0x004337E0 "
            "onEnter/onLeave for all 6 racer slots. Captures RS bias, "
            "span_norm, FWD_TRACK_COMP, ACTIVE_LO/HI, RIGHT_BOUNDARY_A/B, "
            "g_lateral_avoidance_direction, and the returned peer_slot. "
            "Diagnoses #8 cascade saturation (reference_steering_cascade_*)."
        ),
        track=0, car=0, player_is_ai=True, game_type=0,
        hook_script=CASCADE_JS,
        csv_header="binary,self_slot,sub_tick,route_ptr,bias,span_norm,"
                   "fwd_track,active_lo,active_hi,right_a,right_b,lat_dir,"
                   "peer_returned",
    ),
}


# ----------------------------------------------------------------------------
# Output path helpers
# ----------------------------------------------------------------------------

def make_output_paths(target_name: str, stamp: str) -> Tuple[Path, Path, Path]:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    orig_csv = LOG_DIR / f"diff_{target_name}_orig_{stamp}.csv"
    port_csv = LOG_DIR / f"diff_{target_name}_port_{stamp}.csv"
    merged   = LOG_DIR / f"diff_{target_name}_{stamp}.csv"
    return orig_csv, port_csv, merged


# ----------------------------------------------------------------------------
# Port (td5re.exe) launch -- td5re.ini patching + Frida attach
# ----------------------------------------------------------------------------

def patch_port_ini(target: Target) -> str:
    """Configure td5re.ini for the target scenario; returns the original
    INI text so we can restore on exit."""
    original_text = PORT_INI.read_text(encoding="utf-8")
    cp = configparser.ConfigParser(strict=False); cp.optionxform = str
    cp.read(PORT_INI, encoding="utf-8")
    for sect in ("Game", "GameOptions", "Trace", "Logging"):
        if not cp.has_section(sect):
            cp.add_section(sect)
    cp["Game"]["DefaultTrack"]    = str(target.track)
    cp["Game"]["DefaultCar"]      = str(target.car)
    cp["Game"]["DefaultGameType"] = str(target.game_type)
    cp["Game"]["AutoRace"]        = "1"
    cp["Game"]["SkipIntro"]       = "1"
    cp["GameOptions"]["PlayerIsAI"] = "1" if target.player_is_ai else "0"
    cp["Trace"]["StateReplayMode"]      = "off"
    cp["Trace"]["RaceTrace"]            = "0"
    cp["Trace"]["WholeState"]           = "0"
    cp["Trace"]["RaceTraceMaxSimTicks"] = "0"
    cp["Logging"]["Enabled"] = "1"
    with PORT_INI.open("w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=True)
    return original_text


def restore_port_ini(original_text: str):
    PORT_INI.write_text(original_text, encoding="utf-8")


def read_hook_with_overrides(hook_path: Path, binary_label: str,
                             out_csv: Path) -> str:
    """Inline-edit the JS so OUTPUT_PATH and BINARY_LABEL are correct for
    this run.  The hooks expose those as top-of-file `var` declarations."""
    src = hook_path.read_text(encoding="utf-8")
    posix_out = str(out_csv).replace("\\", "/")
    src = re.sub(
        r'var OUTPUT_PATH\s*=\s*"[^"]*";',
        f'var OUTPUT_PATH = "{posix_out}";',
        src, count=1
    )
    src = re.sub(
        r'var BINARY_LABEL\s*=\s*"[^"]*";',
        f'var BINARY_LABEL = "{binary_label}";',
        src, count=1
    )
    return src


def focus_pid_window(pid: int, timeout_s: float = 5.0) -> bool:
    """Bring the first visible top-level window owned by `pid` to the
    foreground. Both TD5_d3d.exe and td5re.exe pause their loops when not
    foreground, so this MUST succeed for the snapshot to tick."""
    import ctypes, ctypes.wintypes as wt
    u32 = ctypes.windll.user32
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    found = {"hwnd": None}
    def enum(hwnd, _):
        o = wt.DWORD()
        u32.GetWindowThreadProcessId(hwnd, ctypes.byref(o))
        if o.value == pid and u32.IsWindowVisible(hwnd):
            found["hwnd"] = hwnd; return False
        return True
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        u32.EnumWindows(WNDENUMPROC(enum), 0)
        if found["hwnd"]: break
        time.sleep(0.05)
    if not found["hwnd"]: return False
    u32.ShowWindow(found["hwnd"], 9)         # SW_RESTORE
    u32.SetForegroundWindow(found["hwnd"])
    return True


def capture_port(target: Target, duration_s: float, out_csv: Path,
                 dry_run: bool) -> bool:
    """Launch td5re.exe with AutoRace=1, attach Frida, run hook, capture."""
    print(f"[port] preparing td5re.exe capture -> {out_csv}")
    if dry_run:
        print(f"[port] DRY-RUN -- would patch INI to track={target.track} "
              f"car={target.car} ai={target.player_is_ai} game_type={target.game_type}")
        print(f"[port] DRY-RUN -- would spawn {PORT_EXE} from cwd={PROJECT_ROOT}")
        print(f"[port] DRY-RUN -- would inject {target.hook_script}")
        print(f"[port] DRY-RUN -- would run for ~{duration_s}s, then taskkill td5re.exe")
        return True

    original_ini = patch_port_ini(target)
    if out_csv.exists():
        out_csv.unlink()

    # Frida is required at runtime; gate on import here so --dry-run works
    # in environments without it installed.
    import frida

    proc = None
    session = None
    script = None
    try:
        # Snapshot existing PIDs so we can claim the spawned one.
        device = frida.get_local_device()
        existing = {p.pid for p in device.enumerate_processes()
                    if p.name.lower() == "td5re.exe"}
        proc = subprocess.Popen(
            ["cmd", "/c", "start", "", str(PORT_EXE)],
            cwd=str(PROJECT_ROOT), close_fds=True,
        )
        # Detect spawned PID
        new_pid = None
        deadline = time.monotonic() + 10.0
        while time.monotonic() < deadline:
            cur = {p.pid for p in device.enumerate_processes()
                   if p.name.lower() == "td5re.exe"}
            new = cur - existing
            if new:
                new_pid = sorted(new)[0]; break
            time.sleep(0.05)
        if not new_pid:
            print("[port] ERROR: failed to detect spawned td5re.exe pid")
            return False
        print(f"[port] spawned pid={new_pid}; focusing window")
        focus_pid_window(new_pid, timeout_s=5.0)

        # Attach (with brief retry for boot race).
        for _ in range(30):
            try: session = device.attach(new_pid); break
            except (frida.ProcessNotRespondingError, frida.ServerNotRunningError):
                time.sleep(0.1)
        else:
            print("[port] ERROR: failed to attach to td5re.exe")
            return False

        script_src = read_hook_with_overrides(target.hook_script, "port", out_csv)
        script = session.create_script(script_src)
        script.on("message", lambda msg, _: print(f"[port:js] {msg}"))
        script.load()
        print(f"[port] hook loaded; capturing for {duration_s}s")
        time.sleep(duration_s)
    finally:
        try:
            if script: script.unload()
        except Exception: pass
        try:
            if session: session.detach()
        except Exception: pass
        # Always taskkill the game (Popen.terminate() doesn't always reach
        # the actual TD5 process spawned via cmd /c start).
        subprocess.run(["taskkill", "/F", "/IM", "td5re.exe"],
                       capture_output=True, text=True)
        restore_port_ini(original_ini)

    ok = out_csv.exists() and out_csv.stat().st_size > 64
    if ok:
        print(f"[port] OK  ({out_csv.stat().st_size} bytes)")
    else:
        print(f"[port] FAIL: output missing or empty")
    return ok


# ----------------------------------------------------------------------------
# Original (TD5_d3d.exe) launch -- piggy-back on td5_quickrace.py
# ----------------------------------------------------------------------------

def capture_orig(target: Target, duration_s: float, out_csv: Path,
                 dry_run: bool) -> bool:
    """Launch original/TD5_d3d.exe via td5_quickrace.py with the same
    scenario, injecting the same hook script."""
    print(f"[orig] preparing TD5_d3d.exe capture -> {out_csv}")
    if out_csv.exists():
        out_csv.unlink()

    # Build the per-run hook variant (BINARY_LABEL=orig + OUTPUT_PATH).
    hook_src = read_hook_with_overrides(target.hook_script, "orig", out_csv)
    if dry_run:
        # Write the patched script to a tempfile and report the planned cmd,
        # but don't spawn anything.
        with tempfile.NamedTemporaryFile(mode="w", suffix=".js", delete=False,
                                          encoding="utf-8") as f:
            f.write(hook_src); tmp_path = f.name
        cmd = [
            sys.executable, str(QUICKRACE_PY),
            "--track", str(target.track),
            "--car",   str(target.car),
            "--player-is-ai", "true" if target.player_is_ai else "false",
            "--game-type", str(target.game_type),
            "--laps", "3", "--direction", "0",
            "--extra-script", tmp_path,
            "--trace-auto-exit",
            "--max-runtime", str(int(duration_s + 30)),
        ]
        print(f"[orig] DRY-RUN -- patched JS at {tmp_path}")
        print(f"[orig] DRY-RUN -- would run: {' '.join(cmd)}")
        # Cleanup the tempfile (real run leaves it for quickrace to read).
        try: os.unlink(tmp_path)
        except: pass
        return True

    with tempfile.NamedTemporaryFile(mode="w", suffix=".js", delete=False,
                                      encoding="utf-8") as f:
        f.write(hook_src); tmp_path = f.name
    try:
        cmd = [
            sys.executable, str(QUICKRACE_PY),
            "--track", str(target.track),
            "--car",   str(target.car),
            "--player-is-ai", "true" if target.player_is_ai else "false",
            "--game-type", str(target.game_type),
            "--laps", "3", "--direction", "0",
            "--extra-script", tmp_path,
            "--trace-auto-exit",
            "--max-runtime", str(int(duration_s + 30)),
        ]
        print(f"[orig] launching quickrace with hook injection")
        proc = subprocess.Popen(cmd, cwd=str(PROJECT_ROOT),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        # Wait for capture to finish (capped by duration + boot grace).
        try:
            stdout, _ = proc.communicate(timeout=duration_s + 60)
        except subprocess.TimeoutExpired:
            print("[orig] quickrace ran past timeout; killing")
            proc.kill()
            subprocess.run(["taskkill", "/F", "/IM", "TD5_d3d.exe"],
                           capture_output=True, text=True)
            stdout = ""
        if stdout:
            # Echo orig output verbatim (helpful for debugging).
            for line in stdout.splitlines():
                print(f"[orig:qr] {line}")
    finally:
        try: os.unlink(tmp_path)
        except: pass
        # Belt-and-braces: kill any straggling original processes.
        subprocess.run(["taskkill", "/F", "/IM", "TD5_d3d.exe"],
                       capture_output=True, text=True)

    ok = out_csv.exists() and out_csv.stat().st_size > 64
    if ok:
        print(f"[orig] OK  ({out_csv.stat().st_size} bytes)")
    else:
        print(f"[orig] FAIL: output missing or empty")
    return ok


# ----------------------------------------------------------------------------
# Merge -- interleave by (slot, sub_tick); orig row first, then port row
# ----------------------------------------------------------------------------

def merge_csvs(orig_csv: Path, port_csv: Path, merged: Path,
               target: Target) -> bool:
    """Combine the two per-binary CSVs into one differential CSV. Output
    rows are sorted by (slot, sub_tick); within each (slot, sub_tick) the
    orig row appears first, then the port row. Header is emitted once
    from the first file that exists with a non-empty header line."""
    import csv

    def load(path: Path):
        if not path.exists() or path.stat().st_size <= 0:
            return None, []
        with path.open("r", newline="", encoding="utf-8") as f:
            rdr = csv.reader(f)
            try: hdr = next(rdr)
            except StopIteration: return None, []
            rows = list(rdr)
        return hdr, rows

    orig_hdr, orig_rows = load(orig_csv)
    port_hdr, port_rows = load(port_csv)
    if orig_hdr is None and port_hdr is None:
        print("[merge] ERROR: neither orig nor port CSV is present/non-empty")
        return False
    hdr = orig_hdr or port_hdr
    rows = []
    rows.extend(orig_rows)
    rows.extend(port_rows)

    # Index of the slot/self_slot column varies per target schema but is
    # column index 1 in both (binary, slot/self_slot, sub_tick, ...).
    def keyfn(r):
        try:
            return (int(r[1]), int(r[2]), 0 if r[0] == "orig" else 1)
        except (ValueError, IndexError):
            return (-1, -1, 0)
    rows.sort(key=keyfn)

    with merged.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(hdr)
        for r in rows:
            w.writerow(r)
    print(f"[merge] wrote {merged}  ({len(rows)} rows)")
    return True


# ----------------------------------------------------------------------------
# Driver
# ----------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Frida differential capture across orig + port",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--target", required=True, choices=sorted(TARGETS.keys()),
                    help="Capture target (chassis-launch or cascade).")
    ap.add_argument("--duration", type=float, default=30.0,
                    help="Wall-clock seconds of capture per binary (default 30).")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print spawn commands without launching anything.")
    ap.add_argument("--skip-orig", action="store_true",
                    help="Capture port only (orig CSV must already exist).")
    ap.add_argument("--skip-port", action="store_true",
                    help="Capture orig only (port CSV must already exist).")
    args = ap.parse_args()

    target = TARGETS[args.target]
    print(f"=== Frida Differential Capture ===")
    print(f"Target:    {target.name}")
    print(f"  {target.description}")
    print(f"Duration:  {args.duration}s per binary")
    print(f"Orig exe:  {ORIG_EXE}")
    print(f"Port exe:  {PORT_EXE}")
    print(f"Hook js:   {target.hook_script}")
    print()

    # Sanity-check binaries (skip in dry-run).
    if not args.dry_run:
        missing = []
        if not args.skip_orig and not ORIG_EXE.exists():
            missing.append(str(ORIG_EXE))
        if not args.skip_port and not PORT_EXE.exists():
            missing.append(str(PORT_EXE))
        if not target.hook_script.exists():
            missing.append(str(target.hook_script))
        if missing:
            for m in missing:
                print(f"ERROR: required file missing: {m}", file=sys.stderr)
            return 2

    stamp = time.strftime("%Y%m%d-%H%M%S")
    orig_csv, port_csv, merged = make_output_paths(target.name, stamp)

    ok_orig = True
    ok_port = True
    if not args.skip_orig:
        ok_orig = capture_orig(target, args.duration, orig_csv, args.dry_run)
    if not args.skip_port:
        ok_port = capture_port(target, args.duration, port_csv, args.dry_run)

    print()
    if args.dry_run:
        print(f"DRY-RUN complete.  Real run would emit:")
        print(f"  {orig_csv}")
        print(f"  {port_csv}")
        print(f"  {merged}  (merged)")
        return 0

    if not (ok_orig and ok_port):
        print("WARNING: one or both captures failed; merged CSV may be partial.")
    merge_csvs(orig_csv, port_csv, merged, target)
    print()
    print(f"=== Outputs ===")
    print(f"  orig:   {orig_csv}")
    print(f"  port:   {port_csv}")
    print(f"  merged: {merged}")
    return 0 if (ok_orig and ok_port) else 1


if __name__ == "__main__":
    sys.exit(main())
