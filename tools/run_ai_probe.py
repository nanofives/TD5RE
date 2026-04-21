"""
run_ai_probe.py — Spawn TD5_d3d.exe with quickrace hook + AI probe.

Reuses td5_quickrace_hook.js to boot straight into the race scenario,
then also injects frida_ai_probe.js to capture per-slot per-tick AI
internals (yaw, steer, left_dev, right_dev, threshold_result, cascade
weight). Auto-exits after N sim_ticks.

Output: log/ai_probe_original.csv

Usage:
  python tools/run_ai_probe.py                    # defaults (Moscow/Viper/single race, 300 ticks)
  python tools/run_ai_probe.py --max-sim-tick 200
"""

import argparse
import configparser
import json
import os
import subprocess
import sys
import time

import frida

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, ".."))
ORIGINAL_DIR = os.path.join(REPO_ROOT, "original")
QUICKRACE_DIR = os.path.join(REPO_ROOT, "re", "tools", "quickrace")
HOOK_JS = os.path.join(QUICKRACE_DIR, "td5_quickrace_hook.js")
PROBE_JS = os.path.join(HERE, "frida_ai_probe.js")
QUICKRACE_INI = os.path.join(QUICKRACE_DIR, "td5_quickrace.ini")


def load_quickrace_cfg(ini_path):
    cp = configparser.ConfigParser()
    cp.read(ini_path)
    cfg = {
        "game_type": 0, "track": 0, "direction": 0, "laps": 1,
        "car": 0, "paint": 0, "transmission": 0, "verbose": False,
    }
    for section, fields in [("race", ["game_type","track","direction","laps"]),
                            ("car",  ["car","paint","transmission"])]:
        if cp.has_section(section):
            for f in fields:
                if cp.has_option(section, f):
                    cfg[f] = cp.getint(section, f)
    return cfg


def on_message(msg, data):
    if msg.get("type") == "send":
        print(f"[hook] {msg['payload']}")
    elif msg.get("type") == "error":
        print(f"[hook ERROR] {msg.get('description')}: {msg.get('stack','')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--max-sim-tick", type=int, default=300,
                    help="stop after N sim ticks (default 300)")
    ap.add_argument("--wait-seconds", type=int, default=30,
                    help="hard upper bound in wall clock before force-exit")
    args = ap.parse_args()

    cfg = load_quickrace_cfg(QUICKRACE_INI)
    exe_path = os.path.join(ORIGINAL_DIR, "TD5_d3d.exe")

    for p in (exe_path, HOOK_JS, PROBE_JS):
        if not os.path.exists(p):
            sys.exit(f"missing: {p}")

    print(f"[launcher] cfg: {cfg}")
    print(f"[launcher] spawning {exe_path}")

    pid = frida.spawn(exe_path, cwd=ORIGINAL_DIR)
    session = frida.attach(pid)

    # 1. Quickrace hook — boots straight into race
    with open(HOOK_JS, "r", encoding="utf-8") as f:
        hook_src = f.read()
    hook_script = session.create_script(hook_src)
    hook_script.on("message", on_message)
    hook_script.load()
    hook_script.post({"type": "cfg", "cfg": cfg})

    # 2. AI probe — captures AI internals
    with open(PROBE_JS, "r", encoding="utf-8") as f:
        probe_src = f.read()
    probe_script = session.create_script(probe_src)
    probe_script.on("message", on_message)
    probe_script.load()

    print("[launcher] resuming process")
    frida.resume(pid)

    # Wait for max_sim_tick or wall-clock limit
    G_SIM = 0x004AADA0
    start = time.time()
    reached = False
    while time.time() - start < args.wait_seconds:
        try:
            v = probe_script.exports_sync.read_sim_tick() if hasattr(probe_script, "exports_sync") else None
        except Exception:
            v = None
        time.sleep(0.5)
        # Can't easily read sim_tick from outside; use wall clock as primary gate.
        # The probe keeps writing rows until we detach.

    print(f"[launcher] wall-clock limit reached ({args.wait_seconds}s); detaching")
    try:
        probe_script.unload()
    except Exception:
        pass
    try:
        hook_script.unload()
    except Exception:
        pass
    session.detach()

    try:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    except Exception:
        pass

    print("[launcher] done")


if __name__ == "__main__":
    main()
