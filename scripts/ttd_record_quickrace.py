"""
ttd_record_quickrace.py -- Record an original TD5_d3d.exe run under TTD,
driven by td5_quickrace.py's Frida hooks so the game reaches a race
automatically. Records the first ~1 second of racing plus countdown buildup.

REQUIRES ADMINISTRATIVE PRIVILEGES (TTD instrumentation needs SeDebug).
Run from an elevated PowerShell:
    python scripts/ttd_record_quickrace.py --scenario moscow_ai_viper

Outputs a .run trace file to traces/ (size ~50-200 MB per recording).

Process flow:
    1. TTD.exe spawns TD5_d3d.exe (from original/ so the DDrawCompat shim loads)
    2. Python polls for the TD5_d3d.exe PID
    3. Frida attaches, injects td5_quickrace_hook.js with the scenario cfg
    4. Quickrace forces race entry (skip intro/legal/menus)
    5. Sleep wall_time seconds (process slow under TTD, real time)
    6. taskkill TD5_d3d.exe -- TTD finalizes the trace on exit
    7. Verify trace file size + report
"""
import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import frida

PROJECT_ROOT = Path(r"C:\Users\maria\Desktop\Proyectos\TD5RE")
TTD_EXE      = PROJECT_ROOT / "tools" / "windbg-dlls" / "ttd" / "TTD.exe"
TD5_DIR      = PROJECT_ROOT / "original"
TD5_EXE      = TD5_DIR / "TD5_d3d.exe"
TRACES_DIR   = PROJECT_ROOT / "traces"
QR_HOOK      = PROJECT_ROOT / "re" / "tools" / "quickrace" / "td5_quickrace_hook.js"

# Scenario presets (name -> cfg dict). Same shape as quickrace_hook expects.
#
# Track IDs are the ORIGINAL game's g_selectedScheduleIndex values, NOT the
# port's DefaultTrack. Confirmed against td5_frontend.c:636-641 comment:
#   "Unlocked slots 0-7: Moscow, Edinburgh, Sydney, Blue Ridge, Jarash,
#    Newcastle, Maui, Courmayeur. Locked 8-15: Honolulu, Tokyo, Keswick,
#    San Francisco, Bern, Kyoto, Washington, Munich."
SCENARIOS = {
    "moscow_ai_viper": dict(
        track=0, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
    "edinburgh_ai_viper": dict(
        track=1, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
    "sydney_ai_viper": dict(
        track=2, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
    "blueridge_ai_viper": dict(
        track=3, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
    "jarash_ai_viper": dict(
        track=4, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
    "honolulu_ai_viper": dict(
        track=8, car=0, game_type=0, direction=0, laps=3,
        player_is_ai=True, opponent_car=0,
    ),
}


def build_cfg(s_cfg):
    """Fill out the quickrace cfg dict matching td5_quickrace_hook expectations."""
    return {
        "game_type":  s_cfg["game_type"],
        "track":      s_cfg["track"],
        "direction":  s_cfg["direction"],
        "laps":       s_cfg["laps"],
        "car":        s_cfg["car"],
        "paint":      0,
        "transmission": 0,
        "start_span_offset": 0,
        "opponent_car": s_cfg["opponent_car"],
        "verbose":      False,
        "trace_track_load": False,
        "player_is_ai": s_cfg["player_is_ai"],
        "seed_crt":     True,
        "crt_seed":     0x1A2B3C4D,
        "frontend_screen": -1,
        "frontend_only": False,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenario", required=True,
                    choices=sorted(SCENARIOS.keys()))
    ap.add_argument("--wall-time", type=float, default=180.0,
                    help="Wall-clock seconds to record after spawn. "
                         "Default 180s. Covers: ~5s boot + intro skip + ~30s "
                         "countdown under TTD slowdown + ~30s of race (= ~1s "
                         "real race time at 30x TTD overhead) + buffer.")
    ap.add_argument("--out", default=None,
                    help="Output .run path (default: traces/<scenario>_orig_<timestamp>.run)")
    args = ap.parse_args()

    # Resolve paths
    if not TTD_EXE.exists():
        print(f"ERROR: TTD.exe not at {TTD_EXE}", file=sys.stderr); return 1
    if not TD5_EXE.exists():
        print(f"ERROR: TD5_d3d.exe not at {TD5_EXE}", file=sys.stderr); return 1
    if not QR_HOOK.exists():
        print(f"ERROR: quickrace hook not at {QR_HOOK}", file=sys.stderr); return 1

    TRACES_DIR.mkdir(parents=True, exist_ok=True)
    if args.out:
        out_path = Path(args.out)
    else:
        stamp = time.strftime("%Y%m%d-%H%M%S")
        out_path = TRACES_DIR / f"{args.scenario}_orig_{stamp}.run"

    cfg = build_cfg(SCENARIOS[args.scenario])
    print(f"=== TTD recorder ===")
    print(f"Scenario:  {args.scenario}  (track={cfg['track']} car={cfg['car']} ai={cfg['player_is_ai']})")
    print(f"TTD:       {TTD_EXE}")
    print(f"Target:    {TD5_EXE}")
    print(f"CWD:       {TD5_DIR}")
    print(f"Trace out: {out_path}")
    print(f"Wall time: {args.wall_time}s")
    print()

    # Snapshot existing TD5_d3d.exe PIDs so we can find our new one
    device = frida.get_local_device()
    existing = {p.pid for p in device.enumerate_processes()
                if p.name.lower() == "td5_d3d.exe"}
    if existing:
        print(f"Note: {len(existing)} TD5_d3d.exe already running -- will skip those PIDs")

    # Launch TTD in background
    print("[ttd] spawning...")
    ttd_proc = subprocess.Popen(
        [str(TTD_EXE), "-accepteula", "-out", str(out_path),
         "-launch", str(TD5_EXE)],
        cwd=str(TD5_DIR),
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )

    # Poll for the new TD5_d3d.exe PID
    new_pid = None
    deadline = time.monotonic() + 60.0
    while time.monotonic() < deadline:
        cur = {p.pid for p in device.enumerate_processes()
               if p.name.lower() == "td5_d3d.exe"}
        new = cur - existing
        if new:
            new_pid = sorted(new)[0]
            break
        time.sleep(0.2)
        if ttd_proc.poll() is not None:
            out, _ = ttd_proc.communicate(timeout=2)
            print(f"[ttd] exited prematurely. Output:\n{out[:500]}")
            return 2
    if not new_pid:
        ttd_proc.terminate()
        print("ERROR: TD5_d3d.exe never spawned under TTD", file=sys.stderr)
        return 3

    print(f"[ttd] TD5_d3d.exe pid={new_pid}, attaching Frida...")

    # Attach Frida
    try:
        session = frida.attach(new_pid)
    except Exception as e:
        print(f"ERROR: Frida attach failed: {e}", file=sys.stderr)
        subprocess.run(["taskkill", "/F", "/IM", "TD5_d3d.exe"], capture_output=True)
        return 4

    # Inject quickrace hook
    qr_src = QR_HOOK.read_text()
    def qr_msg(msg, _data):
        if msg.get("type") == "send":
            p = msg.get("payload")
            if isinstance(p, dict) and p.get("kind") == "log":
                print(f"[qr] {p.get('msg', p)}")
        elif msg.get("type") == "error":
            print(f"[qr ERR] {msg.get('description', msg)}")

    script = session.create_script(qr_src)
    script.on("message", qr_msg)
    script.load()
    script.post({"type": "cfg", "cfg": cfg})
    print(f"[qr] cfg posted")

    # Focus the window so the game's race-frame loop actually ticks
    try:
        import ctypes, ctypes.wintypes as wt
        u32 = ctypes.windll.user32
        WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
        found = {"h": None}
        def enum(h, _):
            o = wt.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(o))
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
            print(f"[win] focused HWND {found['h']}")
        else:
            print("[win] WARNING: window not found to focus")
    except Exception as e:
        print(f"[win] focus failed: {e}")

    # Sleep for the recording duration
    print(f"[rec] recording for {args.wall_time}s ...")
    time.sleep(args.wall_time)

    # Stop the game; TTD finalizes the trace
    print("[rec] killing TD5_d3d.exe")
    subprocess.run(["taskkill", "/F", "/PID", str(new_pid)],
                   capture_output=True, text=True)
    try: session.detach()
    except Exception: pass

    # Wait for TTD to finalize and exit
    try:
        ttd_out, _ = ttd_proc.communicate(timeout=60)
    except subprocess.TimeoutExpired:
        ttd_proc.kill()
        ttd_out = ""

    if out_path.exists():
        size_mb = out_path.stat().st_size / (1024 * 1024)
        idx_path = out_path.with_suffix(".idx")
        idx_size_mb = (idx_path.stat().st_size / (1024 * 1024)) if idx_path.exists() else 0
        print()
        print(f"=== Trace saved ===")
        print(f"  {out_path}  ({size_mb:.1f} MB)")
        if idx_path.exists():
            print(f"  {idx_path}  ({idx_size_mb:.1f} MB)")
        print()
        print(f"Replay:")
        print(f"  via cdbX86: cdbX86.exe -z \"{out_path}\"")
        print(f"  via MCP:    load_dump \"{out_path}\"")
        return 0
    else:
        print(f"ERROR: trace not written to {out_path}", file=sys.stderr)
        print(f"TTD output:\n{ttd_out[:1500]}", file=sys.stderr)
        return 5


if __name__ == "__main__":
    sys.exit(main())
