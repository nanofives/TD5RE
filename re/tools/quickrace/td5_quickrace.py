"""
td5_quickrace.py
================
Launches original/TD5_d3d.exe with the quick-race Frida hook, reading
customization from td5_quickrace.ini next to this script.

Reads:   re/tools/quickrace/td5_quickrace.ini
Spawns:  original/TD5_d3d.exe (or whatever launcher.exe is set to)
Injects: td5_quickrace_hook.js

The original windowed-mode patches must already be applied (see
re/patches/patch_windowed_*.py) and the td5re wrapper DDraw.dll must be
moved aside to DDraw.dll.td5re_wrapper.

Usage:
  python re/tools/quickrace/td5_quickrace.py
  python re/tools/quickrace/td5_quickrace.py --ini custom.ini
  python re/tools/quickrace/td5_quickrace.py --set race.track=5 car.car=12

Ctrl+C detaches and leaves the game running.
"""

import argparse
import configparser
import os
import re
import subprocess
import sys
import time

import frida

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_INI = os.path.join(HERE, "td5_quickrace.ini")
HOOK_JS = os.path.join(HERE, "td5_quickrace_hook.js")
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ORIGINAL_DIR = os.path.join(REPO_ROOT, "original")
TRACE_JS = os.path.join(REPO_ROOT, "tools", "frida_race_trace.js")


INT_FIELDS = {
    "race": ["game_type", "track", "direction", "laps"],
    "car":  ["car", "paint", "transmission"],
}
BOOL_FIELDS = {"launcher": ["verbose"]}


def load_ini(path):
    cp = configparser.ConfigParser()
    if not os.path.exists(path):
        sys.exit(f"ini not found: {path}")
    cp.read(path)

    out = {
        "game_type": 0, "track": 0, "direction": 0, "laps": 3,
        "car": 0, "paint": 0, "transmission": 0,
        "verbose": False,
    }
    for section, fields in INT_FIELDS.items():
        if cp.has_section(section):
            for f in fields:
                if cp.has_option(section, f):
                    out[f] = cp.getint(section, f)
    for section, fields in BOOL_FIELDS.items():
        if cp.has_section(section):
            for f in fields:
                if cp.has_option(section, f):
                    out[f] = cp.getboolean(section, f)

    exe = "TD5_d3d.exe"
    if cp.has_option("launcher", "exe"):
        exe = cp.get("launcher", "exe")
    return out, exe


def apply_overrides(cfg, overrides):
    # overrides are ["race.track=5", "car.paint=2", ...]
    for o in overrides:
        if "=" not in o:
            sys.exit(f"bad override (expected section.key=value): {o}")
        key, val = o.split("=", 1)
        if "." in key:
            _section, field = key.split(".", 1)
        else:
            field = key
        if field in ("verbose",):
            cfg[field] = val.lower() in ("1", "true", "yes", "on")
        else:
            try:
                cfg[field] = int(val, 0)
            except ValueError:
                sys.exit(f"non-integer override for {field}: {val}")


g_auto_close = False


def on_message(msg, data):
    if msg["type"] == "send":
        payload = msg.get("payload") or {}
        if isinstance(payload, dict):
            kind = payload.get("kind", "?")
            if kind == "log":
                print(f"[hook] {payload.get('msg', '')}")
                return
            if payload.get("type") == "auto-close":
                print("[trace] auto-close received")
                globals()["g_auto_close"] = True
                return
            print(f"[hook/{kind}] {payload}")
        else:
            print(f"[trace] {payload}")
    elif msg["type"] == "error":
        print(f"[hook/error] {msg.get('stack') or msg}")


def patch_trace_script(source: str, max_frames: int, max_sim_tick: int) -> str:
    """Rewrite TRACE_MAX_FRAMES / TRACE_MAX_SIM_TICK constants in the trace script."""
    source = re.sub(
        r"var\s+TRACE_MAX_FRAMES\s*=\s*\d+;",
        f"var TRACE_MAX_FRAMES   = {max_frames};",
        source,
        count=1,
    )
    source = re.sub(
        r"var\s+TRACE_MAX_SIM_TICK\s*=\s*\d+;",
        f"var TRACE_MAX_SIM_TICK = {max_sim_tick};",
        source,
        count=1,
    )
    return source


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ini", default=DEFAULT_INI, help="ini path")
    ap.add_argument("--set", dest="overrides", action="append", default=[],
                    help="override e.g. race.track=5 (repeatable)")
    ap.add_argument("--exe", default=None, help="override launcher.exe")
    ap.add_argument("--no-resume", action="store_true",
                    help="leave process suspended after injection")
    ap.add_argument("--trace", action="store_true",
                    help="also inject tools/frida_race_trace.js into the same "
                         "Frida session; writes log/race_trace_original.csv")
    ap.add_argument("--trace-max-frames", type=int, default=300,
                    help="override TRACE_MAX_FRAMES in the trace script "
                         "(safety ceiling on render frames; 0 = unlimited)")
    ap.add_argument("--trace-max-sim-tick", type=int, default=0,
                    help="override TRACE_MAX_SIM_TICK in the trace script "
                         "(engine-clock cap; 450 = 15 s post-countdown at 30 Hz)")
    ap.add_argument("--trace-auto-exit", action="store_true",
                    help="exit this launcher when the trace script reports "
                         "an auto-close message (used by diff_race.py)")
    args = ap.parse_args()

    cfg, ini_exe = load_ini(args.ini)
    apply_overrides(cfg, args.overrides)
    exe_name = args.exe or ini_exe

    exe_path = os.path.join(ORIGINAL_DIR, exe_name)
    if not os.path.exists(exe_path):
        sys.exit(f"exe not found: {exe_path}")
    if not os.path.exists(HOOK_JS):
        sys.exit(f"hook script missing: {HOOK_JS}")

    with open(HOOK_JS, "r", encoding="utf-8") as f:
        js_source = f.read()

    print(f"[launcher] ini: {args.ini}")
    print(f"[launcher] cfg: {cfg}")
    print(f"[launcher] spawning: {exe_path}")

    pid = frida.spawn(exe_path, cwd=ORIGINAL_DIR)
    print(f"[launcher] pid: {pid}")

    session = frida.attach(pid)
    script = session.create_script(js_source)
    script.on("message", on_message)
    script.load()

    script.post({"type": "cfg", "cfg": cfg})

    trace_script = None
    if args.trace:
        if not os.path.exists(TRACE_JS):
            sys.exit(f"--trace set but script missing: {TRACE_JS}")
        with open(TRACE_JS, "r", encoding="utf-8") as f:
            trace_source = f.read()
        trace_source = patch_trace_script(
            trace_source, args.trace_max_frames, args.trace_max_sim_tick)
        print(f"[launcher] injecting trace script "
              f"(max_frames={args.trace_max_frames} "
              f"max_sim_tick={args.trace_max_sim_tick})")
        trace_script = session.create_script(trace_source)
        trace_script.on("message", on_message)
        trace_script.load()

    if args.no_resume:
        print("[launcher] --no-resume set, process still suspended")
    else:
        frida.resume(pid)
        print("[launcher] resumed; Ctrl+C to detach")

    try:
        while True:
            time.sleep(0.5)
            if args.trace_auto_exit and g_auto_close:
                print("[launcher] trace auto-close; detaching and killing target")
                break
    except KeyboardInterrupt:
        print("\n[launcher] detaching")
    finally:
        try:
            session.detach()
        except Exception:
            pass
        if args.trace_auto_exit:
            try:
                frida.kill(pid)
            except Exception:
                pass


if __name__ == "__main__":
    main()
