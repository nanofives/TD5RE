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
    "race": ["game_type", "track", "direction", "laps", "start_span_offset"],
    "car":  ["car", "paint", "transmission"],
    "drag": ["opponent_car"],
}
BOOL_FIELDS = {"launcher": ["verbose", "trace_track_load"]}


def load_ini(path):
    cp = configparser.ConfigParser()
    if not os.path.exists(path):
        sys.exit(f"ini not found: {path}")
    cp.read(path)

    out = {
        "game_type": 0, "track": 0, "direction": 0, "laps": 3,
        "car": 0, "paint": 0, "transmission": 0,
        "start_span_offset": 0,
        "opponent_car": 0,
        "verbose": False,
        "trace_track_load": False,
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
        if field in ("verbose", "trace_track_load"):
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


def patch_trace_script(source: str, max_frames: int, max_sim_tick: int,
                       hook_specs: list | None = None) -> str:
    """Rewrite TRACE_MAX_FRAMES / TRACE_MAX_SIM_TICK constants in the trace script.

    If hook_specs is provided, also injects the JSON-encoded spec array as the
    script's HOOK_SPECS global so the Frida side can install custom Interceptor
    hooks via its installHookSpecs() helper.
    """
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
    if hook_specs:
        # Use Python json encoder to produce valid JS array literal.
        import json
        spec_literal = json.dumps(hook_specs)
        source = re.sub(
            r"var\s+HOOK_SPECS\s*=\s*\[\s*\];",
            f"var HOOK_SPECS = {spec_literal};",
            source,
            count=1,
        )
    return source


def load_hook_specs(path: str) -> list:
    """Load hook spec YAML/JSON file. YAML preferred; fallback to JSON.

    Spec shape:
      hooks:
        - name: some_function
          original_rva: 0x4457E0
          args: 3                # number of int32 args to capture (0..8)
          capture_return: true   # emit retval column
          # port_symbol, comments, etc. are informational for humans
    Returns the normalized list of hook dicts (keys: name, original_rva,
    args, capture_return).
    """
    with open(path, "r", encoding="utf-8") as f:
        raw = f.read()
    try:
        import yaml  # type: ignore
        doc = yaml.safe_load(raw)
    except ImportError:
        import json
        doc = json.loads(raw)
    if not isinstance(doc, dict):
        raise ValueError(f"{path}: expected a mapping at the top level")
    hooks = doc.get("hooks") or []
    if not isinstance(hooks, list):
        raise ValueError(f"{path}: `hooks` must be a list")
    out = []
    for i, h in enumerate(hooks):
        name = h.get("name")
        rva  = h.get("original_rva")
        if not name or rva is None:
            raise ValueError(f"{path}: hook[{i}] requires name + original_rva")
        # Accept int, hex string, or int-literal string.
        if isinstance(rva, str):
            rva = int(rva, 0)
        out.append({
            "name": str(name),
            "original_rva": int(rva),
            "args": int(h.get("args", 0)),
            "capture_return": bool(h.get("capture_return", False)),
        })
    return out


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
    ap.add_argument("--hook-specs", default=None,
                    help="path to a YAML/JSON hook-specs file (see "
                         "re/trace-hooks/README.md); injected into the trace "
                         "script as HOOK_SPECS so Frida attaches per-fn hooks")
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
        hook_specs = None
        if args.hook_specs:
            if not os.path.exists(args.hook_specs):
                sys.exit(f"--hook-specs file missing: {args.hook_specs}")
            hook_specs = load_hook_specs(args.hook_specs)
            print(f"[launcher] loaded {len(hook_specs)} hook spec(s) from "
                  f"{args.hook_specs}")
        trace_source = patch_trace_script(
            trace_source, args.trace_max_frames, args.trace_max_sim_tick,
            hook_specs=hook_specs)
        print(f"[launcher] injecting trace script "
              f"(max_frames={args.trace_max_frames} "
              f"max_sim_tick={args.trace_max_sim_tick}"
              f"{' hooks=' + str(len(hook_specs)) if hook_specs else ''})")
        trace_script = session.create_script(trace_source)
        trace_script.on("message", on_message)
        trace_script.load()

    if args.no_resume:
        print("[launcher] --no-resume set, process still suspended")
    else:
        frida.resume(pid)
        print("[launcher] resumed; Ctrl+C to detach")
        # TD5 pauses its race-frame loop when the window isn't in the
        # foreground (focus-loss pause). Bring the game window to the front
        # so the message pump + race tick actually advance. Without this the
        # race stays at sim_tick=0 paused=1 forever when launched via Frida.
        try:
            import ctypes, ctypes.wintypes as wt
            user32 = ctypes.windll.user32
            EnumWindows = user32.EnumWindows
            GetWindowThreadProcessId = user32.GetWindowThreadProcessId
            SetForegroundWindow = user32.SetForegroundWindow
            ShowWindow = user32.ShowWindow
            WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
            found = {"hwnd": None}
            def enum(hwnd, lparam):
                owner_pid = wt.DWORD()
                GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
                if owner_pid.value == pid and user32.IsWindowVisible(hwnd):
                    found["hwnd"] = hwnd
                    return False
                return True
            for _ in range(30):  # up to ~3s of polling
                EnumWindows(WNDENUMPROC(enum), 0)
                if found["hwnd"]:
                    break
                time.sleep(0.1)
            if found["hwnd"]:
                ShowWindow(found["hwnd"], 9)  # SW_RESTORE
                SetForegroundWindow(found["hwnd"])
                print(f"[launcher] focused HWND {found['hwnd']}")
            else:
                print("[launcher] warning: could not locate TD5 window to focus")
        except Exception as exc:
            print(f"[launcher] focus attempt failed: {exc}")

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
