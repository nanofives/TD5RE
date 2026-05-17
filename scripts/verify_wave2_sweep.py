"""
verify_wave2_sweep.py -- One-shot regression sweep across canonical scenarios
post Round 3 Wave 1 + Wave 2 commits.

For each scenario:
  1. Patch td5re.ini DefaultTrack / DefaultCar / PlayerIsAI
  2. Run td5re.exe with StateReplayMode=both, replaying against the
     matching tools/frida_csv/state_snapshot_<scenario>.bin
  3. Diff port_state_snapshot.bin vs the scenario snapshot
  4. Report sub_tick=0/1/racing field counts + delta vs Round 2 baseline
"""
import os
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(r"C:\Users\maria\Desktop\Proyectos\TD5RE")
TD5RE_EXE  = PROJECT_ROOT / "td5re.exe"
TD5RE_INI  = PROJECT_ROOT / "td5re.ini"
PORT_SNAP  = PROJECT_ROOT / "log" / "port_state_snapshot.bin"

# Reuse run_state_replay's line-preserving ini_patch (configparser rewrites
# in a format td5re.exe doesn't parse correctly).
sys.path.insert(0, str(PROJECT_ROOT / "tools"))
from run_state_replay import ini_patch, ini_restore, BASELINE_BOTH

# (name, track, car, player_is_ai, round3_sub1_baseline)
# Track IDs match the ORIGINAL game's g_selectedScheduleIndex (td5_frontend.c:636).
# Baselines refreshed post-30d1ab9 (actual-Moscow/Jarash track-ID corrections).
SCENARIOS = [
    ("honolulu_ai_viper",        8, 0, 1, 146),
    ("edinburgh_ai_viper",       1, 0, 1, 228),
    ("sydney_ai_viper",          2, 0, 1, None),  # new in Round 3 (P1)
    ("blueridge_ai_viper",       3, 0, 1, None),  # new in Round 3 (P1)
    ("honolulu_hum_viper",       8, 0, 0, 216),
    ("actual_moscow_ai_viper",   0, 0, 1, 128),   # NEW: real Moscow track=0, post-2a24dfa unblock
    ("jarash_ai_viper",          4, 0, 1, 145),   # was mislabeled as "moscow" pre-30d1ab9
]


def build_overrides(track, car, player_is_ai):
    """Build BASELINE_BOTH-shape dict with scenario overrides."""
    out = dict(BASELINE_BOTH)
    out["Game.DefaultTrack"] = str(track)
    out["Game.DefaultCar"]   = str(car)
    out["GameOptions.PlayerIsAI"] = str(player_is_ai)
    return out


def _parse_diff_stdout(stdout):
    rows = {}
    for line in stdout.splitlines():
        # Lines like: "       0         0      3       370  actor[0] +0x120 rotation_matrix"
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit() and parts[1].isdigit():
            sub = int(parts[0])
            if sub in (0, 1, 3, 10, 121, 160, 179):
                try:
                    count = int(parts[3])
                    rows[sub] = count
                except ValueError:
                    pass
    return rows


def run_capture_diff(scenario_name, track, car, player_is_ai, unfiltered=False):
    """Run td5re.exe with replay-injection of the scenario, diff vs scenario.

    When unfiltered=True, also report unfiltered diff counts (EXPECTED_DIVERGENT
    bypass). Returns (rows, rows_unfiltered_or_None, err_or_None).
    """
    bin_path = PROJECT_ROOT / "tools" / "frida_csv" / f"state_snapshot_{scenario_name}.bin"
    if not bin_path.exists():
        return None, None, f"missing scenario .bin: {bin_path.name}"

    env = dict(os.environ)
    env["TD5RE_STATE_REPLAY_PATH"] = str(bin_path)

    # Delete prior port snapshot
    if PORT_SNAP.exists(): PORT_SNAP.unlink()

    # Patch INI (line-preserving)
    originals = ini_patch(TD5RE_INI, build_overrides(track, car, player_is_ai))

    # Launch minimized so repeated sweep runs don't yank focus.
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 7  # SW_SHOWMINNOACTIVE

    t0 = time.monotonic()
    try:
        try:
            subprocess.call([str(TD5RE_EXE)], cwd=str(PROJECT_ROOT), env=env,
                            timeout=90, startupinfo=si)
        except subprocess.TimeoutExpired:
            # Some scenarios on certain tracks may not auto-quit cleanly;
            # kill it and proceed -- the snapshot may still be valid.
            subprocess.run(["taskkill", "/F", "/IM", "td5re.exe"],
                           capture_output=True)
    finally:
        ini_restore(TD5RE_INI, originals)
    elapsed = time.monotonic() - t0

    if not PORT_SNAP.exists():
        return None, None, f"port snapshot not produced after {elapsed:.1f}s"

    # Diff (filtered)
    diff_proc = subprocess.run(
        [sys.executable, str(PROJECT_ROOT / "tools" / "diff_replay_frames.py"),
         str(PORT_SNAP), str(bin_path)],
        cwd=str(PROJECT_ROOT), capture_output=True, text=True, timeout=60,
    )
    rows = _parse_diff_stdout(diff_proc.stdout)

    rows_unf = None
    if unfiltered:
        diff_proc_u = subprocess.run(
            [sys.executable, str(PROJECT_ROOT / "tools" / "diff_replay_frames.py"),
             "--unfiltered", str(PORT_SNAP), str(bin_path)],
            cwd=str(PROJECT_ROOT), capture_output=True, text=True, timeout=60,
        )
        rows_unf = _parse_diff_stdout(diff_proc_u.stdout)
    return rows, rows_unf, None


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--unfiltered", action="store_true",
                    help="Also report unfiltered (EXPECTED_DIVERGENT bypass) counts.")
    ap.add_argument("--scenario", default=None,
                    help="Run only the named scenario (else: all).")
    args = ap.parse_args()

    if args.unfiltered:
        print(f"{'scenario':<24} "
              f"{'sub0':>6} {'sub1':>6} {'sub3':>6} {'sub10':>6} {'sub121':>7} "
              f"{'race160':>8} {'race179':>8} {'R2 sub1':>8}  {'d R2':>6}  | "
              f"{'u0':>6} {'u1':>6} {'u3':>6} {'u10':>6} {'u121':>7} "
              f"{'u160':>6} {'u179':>6}")
    else:
        print(f"{'scenario':<24} {'sub0':>6} {'sub1':>6} {'sub3':>6} "
              f"{'sub10':>6} {'sub121':>7} {'race160':>8} {'race179':>8} "
              f"{'R2 sub1':>8}  {'d R2':>6}")
    print("-" * (180 if args.unfiltered else 100))
    for name, track, car, ai, baseline in SCENARIOS:
        if args.scenario and name != args.scenario:
            continue
        rows, rows_unf, err = run_capture_diff(
            name, track, car, ai, unfiltered=args.unfiltered)
        if err:
            print(f"{name:<24} ERROR: {err}")
            continue
        sub0  = rows.get(0,  -1)
        sub1  = rows.get(1,  -1)
        sub3  = rows.get(3,  -1)
        sub10 = rows.get(10, -1)
        sub121 = rows.get(121, -1)
        r160  = rows.get(160, -1)
        r179  = rows.get(179, -1)
        delta = (sub1 - baseline) if baseline is not None and sub1 >= 0 else None
        delta_str = f"{delta:+d}" if delta is not None else "  N/A"
        baseline_str = str(baseline) if baseline is not None else "   --"
        if args.unfiltered and rows_unf is not None:
            u0   = rows_unf.get(0,  -1)
            u1   = rows_unf.get(1,  -1)
            u3   = rows_unf.get(3,  -1)
            u10  = rows_unf.get(10, -1)
            u121 = rows_unf.get(121, -1)
            u160 = rows_unf.get(160, -1)
            u179 = rows_unf.get(179, -1)
            print(f"{name:<24} {sub0:>6} {sub1:>6} {sub3:>6} {sub10:>6} "
                  f"{sub121:>7} {r160:>8} {r179:>8} {baseline_str:>8}  "
                  f"{delta_str:>6}  | {u0:>6} {u1:>6} {u3:>6} {u10:>6} "
                  f"{u121:>7} {u160:>6} {u179:>6}")
        else:
            print(f"{name:<24} {sub0:>6} {sub1:>6} {sub3:>6} {sub10:>6} "
                  f"{sub121:>7} {r160:>8} {r179:>8} {baseline_str:>8}  "
                  f"{delta_str:>6}")


if __name__ == "__main__":
    main()
