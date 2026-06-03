"""
ttd_record_port.py -- Record td5re.exe (the port) under TTD for a scenario.

REQUIRES ADMINISTRATIVE PRIVILEGES (TTD instrumentation needs SeDebug).
Run from an elevated PowerShell:
    python scripts/ttd_record_port.py --scenario moscow_ai_viper

Unlike the orig recorder, the port doesn't need Frida + quickrace: td5re.exe
respects td5re.ini's AutoRace=1 and DefaultTrack/DefaultCar/PlayerIsAI flags
to skip menus and start the race automatically. So this script just:

    1. Saves current td5re.ini to a temp backup
    2. Writes scenario-specific td5re.ini overrides (track, car, AI, replay-off)
    3. Launches td5re.exe under TTD
    4. Sleeps wall-time seconds (boot + countdown + 1s race under TTD)
    5. Kills td5re.exe -- TTD finalizes the trace on exit
    6. Restores the original td5re.ini

Output: traces/<scenario>_port_<timestamp>.run (+ .idx)
"""
import argparse
import configparser
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROJECT_ROOT = Path(r"C:\Users\maria\Desktop\Proyectos\TD5RE")
TTD_EXE      = PROJECT_ROOT / "tools" / "windbg-dlls" / "ttd" / "TTD.exe"
TD5RE_EXE    = PROJECT_ROOT / "td5re.exe"
TD5RE_INI    = PROJECT_ROOT / "td5re.ini"
TRACES_DIR   = PROJECT_ROOT / "traces"

# Track IDs aligned with the original game's g_selectedScheduleIndex
# (matches scripts/ttd_record_quickrace.py SCENARIOS).
SCENARIOS = {
    "moscow_ai_viper":    dict(track=0, car=0,  player_is_ai=1, game_type=0),
    "edinburgh_ai_viper": dict(track=1, car=0,  player_is_ai=1, game_type=0),
    "sydney_ai_viper":    dict(track=2, car=0,  player_is_ai=1, game_type=0),
    "blueridge_ai_viper": dict(track=3, car=0,  player_is_ai=1, game_type=0),
    "jarash_ai_viper":    dict(track=4, car=0,  player_is_ai=1, game_type=0),
    "honolulu_ai_viper":  dict(track=8, car=0,  player_is_ai=1, game_type=0),
}


def patch_ini(scenario_cfg):
    """Write td5re.ini with scenario-specific values. Returns the saved
    original-content string for restore."""
    original_text = TD5RE_INI.read_text(encoding="utf-8")

    cp = configparser.ConfigParser(strict=False)
    cp.optionxform = str  # preserve case
    cp.read(TD5RE_INI, encoding="utf-8")

    if not cp.has_section("Game"): cp.add_section("Game")
    if not cp.has_section("GameOptions"): cp.add_section("GameOptions")
    if not cp.has_section("Trace"): cp.add_section("Trace")
    if not cp.has_section("Logging"): cp.add_section("Logging")

    cp["Game"]["DefaultTrack"]   = str(scenario_cfg["track"])
    cp["Game"]["DefaultCar"]     = str(scenario_cfg["car"])
    cp["Game"]["DefaultGameType"] = str(scenario_cfg["game_type"])
    cp["Game"]["AutoRace"]       = "1"
    cp["Game"]["SkipIntro"]      = "1"

    cp["GameOptions"]["PlayerIsAI"] = str(scenario_cfg["player_is_ai"])

    # Replay-off -- we want a pristine port run, no state injection
    cp["Trace"]["StateReplayMode"]     = "off"
    cp["Trace"]["RaceTrace"]           = "0"
    cp["Trace"]["WholeState"]          = "0"
    cp["Trace"]["RaceTraceMaxSimTicks"] = "0"  # no auto-quit -- we'll kill via wall-time

    cp["Logging"]["Enabled"] = "1"

    with TD5RE_INI.open("w", encoding="utf-8") as f:
        cp.write(f, space_around_delimiters=True)

    return original_text


def restore_ini(original_text):
    TD5RE_INI.write_text(original_text, encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", required=True, choices=sorted(SCENARIOS.keys()))
    ap.add_argument("--wall-time", type=float, default=150.0,
                    help="Wall-clock seconds to record after spawn. "
                         "Default 150s. Same as orig recorder.")
    ap.add_argument("--out", default=None,
                    help="Output .run path (default: traces/<scenario>_port_<timestamp>.run)")
    args = ap.parse_args()

    # Resolve paths
    if not TTD_EXE.exists():
        print(f"ERROR: TTD.exe not at {TTD_EXE}", file=sys.stderr); return 1
    if not TD5RE_EXE.exists():
        print(f"ERROR: td5re.exe not at {TD5RE_EXE} -- build first", file=sys.stderr); return 1
    if not TD5RE_INI.exists():
        print(f"ERROR: td5re.ini not at {TD5RE_INI}", file=sys.stderr); return 1

    TRACES_DIR.mkdir(parents=True, exist_ok=True)
    if args.out:
        out_path = Path(args.out)
    else:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out_path = TRACES_DIR / f"{args.scenario}_port_{stamp}.run"

    scenario_cfg = SCENARIOS[args.scenario]
    print(f"=== TTD port recorder ===")
    print(f"Scenario:  {args.scenario}  (track={scenario_cfg['track']} car={scenario_cfg['car']} ai={scenario_cfg['player_is_ai']})")
    print(f"Target:    {TD5RE_EXE}")
    print(f"Trace out: {out_path}")
    print(f"Wall time: {args.wall_time}s")
    print()

    # Patch INI
    print("[ini] patching td5re.ini for scenario")
    original_ini = patch_ini(scenario_cfg)

    try:
        # Snapshot existing td5re.exe pids so we identify (and later kill) only
        # the instance TTD launches — never a parallel session's.
        pre_pids = set()
        try:
            import frida as _frida
            pre_pids = {p.pid for p in _frida.get_local_device().enumerate_processes()
                        if p.name.lower() == "td5re.exe"}
        except Exception:
            pass
        td5re_pid = None

        # Launch TTD; it spawns td5re.exe and records
        print("[ttd] spawning td5re.exe under TTD")
        ttd_proc = subprocess.Popen(
            [str(TTD_EXE), "-accepteula", "-out", str(out_path),
             "-launch", str(TD5RE_EXE)],
            cwd=str(PROJECT_ROOT),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1,
        )

        # Wait for td5re.exe to spawn (so we can focus its window)
        time.sleep(3.0)  # boot time

        # Focus the window
        try:
            import frida
            import ctypes, ctypes.wintypes as wt
            device = frida.get_local_device()
            cur = [p.pid for p in device.enumerate_processes()
                   if p.name.lower() == "td5re.exe"]
            new = [pid for pid in cur if pid not in pre_pids]
            if new:
                new_pid = sorted(new)[-1]  # our TTD-launched instance
                td5re_pid = new_pid
                u32 = ctypes.windll.user32
                WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
                found = {"h": None}
                def enum(h, _):
                    o = wt.DWORD()
                    u32.GetWindowThreadProcessId(h, ctypes.byref(o))
                    if o.value == new_pid and u32.IsWindowVisible(h):
                        found["h"] = h; return False
                    return True
                d = time.monotonic() + 5.0
                while time.monotonic() < d:
                    u32.EnumWindows(WNDENUMPROC(enum), 0)
                    if found["h"]: break
                    time.sleep(0.1)
                if found["h"]:
                    u32.ShowWindow(found["h"], 9)
                    u32.SetForegroundWindow(found["h"])
                    print(f"[win] focused HWND {found['h']} (pid={new_pid})")
                else:
                    print(f"[win] window not found (pid={new_pid})")
            else:
                print("[win] td5re.exe not in process list")
        except Exception as e:
            print(f"[win] focus skipped: {e}")

        # Sleep for the rest of the recording duration
        remaining = max(1.0, args.wall_time - 3.0)
        print(f"[rec] recording for {remaining:.0f}s more...")
        time.sleep(remaining)

        # Stop OUR game so TTD finalizes the trace. Kill only the pid we
        # launched — never taskkill /IM (that would stop other sessions' games
        # and corrupt their captures).
        if td5re_pid:
            print(f"[rec] killing td5re.exe pid={td5re_pid}")
            subprocess.run(["taskkill", "/F", "/PID", str(td5re_pid)],
                           capture_output=True, text=True)
        else:
            print("[rec] WARN: our td5re pid unknown; terminating the TTD launcher instead")
            ttd_proc.terminate()

        # Wait for TTD to finalize and exit
        try:
            ttd_out, _ = ttd_proc.communicate(timeout=120)
        except subprocess.TimeoutExpired:
            ttd_proc.kill()
            ttd_out = ""

    finally:
        # Restore INI (always, even on Ctrl-C or error)
        print("[ini] restoring td5re.ini")
        restore_ini(original_ini)

    if out_path.exists():
        size_mb = out_path.stat().st_size / (1024 * 1024)
        idx_path = out_path.with_suffix(".idx")
        idx_size_mb = (idx_path.stat().st_size / (1024 * 1024)) if idx_path.exists() else 0
        print()
        print(f"=== Trace saved ===")
        print(f"  {out_path}  ({size_mb:.1f} MB)")
        if idx_path.exists():
            print(f"  {idx_path}  ({idx_size_mb:.1f} MB)")
        return 0
    else:
        print(f"ERROR: trace not written to {out_path}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    sys.exit(main())
