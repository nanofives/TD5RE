"""
frida_main_menu_capture.py
==========================
Launches the original TD5_d3d.exe windowed (via asi_patcher recipe),
attaches Frida, injects frida_main_menu_hook.js, and records every
frontend rendering primitive that fires while the Main Menu screen
is on display. After ~CAPTURE_SECONDS it kills the game.

Output:
  re/tools/frida_main_menu_capture.log     -- raw `send()` JSONL
  re/tools/frida_main_menu_capture.txt     -- human summary

Usage:
  python re/tools/frida_main_menu_capture.py [--seconds 8]
"""

import argparse
import json
import os
import subprocess
import sys
import time

import frida

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
GAME_DIR = os.path.join(REPO, "original")
GAME_EXE = os.path.join(GAME_DIR, "TD5_d3d.exe")
HOOK_JS  = os.path.join(HERE, "frida_main_menu_hook.js")

LOG_PATH  = os.path.join(HERE, "frida_main_menu_capture.log")
SUMM_PATH = os.path.join(HERE, "frida_main_menu_capture.txt")


def launch_game():
    """Mirror asi_patcher/run_no_shims.ps1: __COMPAT_LAYER=RunAsInvoker,
    Start-Process from original/. Returns subprocess.Popen handle."""
    env = os.environ.copy()
    env["__COMPAT_LAYER"] = "RunAsInvoker"
    return subprocess.Popen(
        [GAME_EXE],
        cwd=GAME_DIR,
        env=env,
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP,
    )


def attach_pid_with_retry(pid, deadline_s=15.0, sleep=0.1):
    t0 = time.time()
    last_err = None
    while time.time() - t0 < deadline_s:
        try:
            return frida.attach(pid)
        except Exception as e:
            last_err = e
            time.sleep(sleep)
    raise RuntimeError(f"could not attach to pid {pid}: {last_err}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=8.0)
    args = ap.parse_args()

    if not os.path.exists(GAME_EXE):
        sys.exit(f"missing: {GAME_EXE}")
    if not os.path.exists(HOOK_JS):
        sys.exit(f"missing: {HOOK_JS}")

    log = open(LOG_PATH, "w", encoding="utf-8", buffering=1)

    def w(s):
        print(s, flush=True)
        log.write(s + "\n")

    w(f"[+] launching {GAME_EXE}")
    proc = launch_game()
    pid = proc.pid
    w(f"[+] pid={pid}")

    session = attach_pid_with_retry(pid)
    w(f"[+] attached")

    with open(HOOK_JS, "r", encoding="utf-8") as f:
        script_src = f.read()
    script = session.create_script(script_src)

    frames = []
    calls = []
    errs = []

    def on_message(msg, _data):
        if msg["type"] == "send":
            p = msg["payload"]
            kind = p.get("kind")
            log.write("MSG " + json.dumps(p) + "\n")
            if kind == "frame":
                frames.append(p)
            elif kind == "call":
                calls.append(p)
            elif kind == "err":
                errs.append(p)
                w(f"[hook-err] {p}")
            elif kind == "ready":
                w(f"[+] hooks loaded base={p.get('base')}")
        elif msg["type"] == "error":
            w(f"[script-err] {msg.get('description','')}")

    script.on("message", on_message)
    script.load()
    w(f"[+] script loaded; capturing for {args.seconds}s")

    t0 = time.time()
    while time.time() - t0 < args.seconds:
        if proc.poll() is not None:
            w("[!] game exited early")
            break
        time.sleep(0.05)

    try: session.detach()
    except Exception: pass

    try: proc.terminate()
    except Exception: pass
    try: proc.wait(2)
    except Exception:
        try: proc.kill()
        except Exception: pass

    # ---------- Summarise ----------
    summary = []
    summary.append(f"frames captured : {len(frames)}")
    summary.append(f"call records    : {len(calls)}")
    summary.append(f"errs            : {len(errs)}")

    # Locate the longest run of frames where screenIdx looks stable
    state_runs = []
    cur = None
    for f in frames:
        key = f.get("screen", "?")
        if cur is None or cur["key"] != key:
            cur = {"key": key, "n": 0, "first": f["frame"]}
            state_runs.append(cur)
        cur["n"] += 1
    summary.append("")
    summary.append("Screen runs (screen) -> frames:")
    for r in state_runs[:25]:
        summary.append(f"  {r['key']!s:30}  {r['n']:5d} frames  starting frame {r['first']}")

    # Per-call totals across all captured frames
    totals = {}
    for f in frames:
        for k, v in f.get("counts", {}).items():
            totals[k] = totals.get(k, 0) + v
    summary.append("")
    summary.append("Per-call totals across capture window:")
    for name, n in sorted(totals.items(), key=lambda kv: -kv[1]):
        summary.append(f"  {n:6d}  {name}")

    # State-stable summary: per-frame mean call count for each primitive
    if frames:
        mm = [r for r in state_runs if r["key"] == "MainMenu"]
        if mm:
            chosen = max(mm, key=lambda r: r["n"])
        else:
            chosen = max(state_runs, key=lambda r: r["n"])
        summary.append("")
        summary.append(f"Main-menu run: {chosen['key']} length={chosen['n']} frames")
        agg = {}
        for f in frames:
            if f.get("screen", "?") != chosen["key"]:
                continue
            for k, v in f.get("counts", {}).items():
                agg[k] = agg.get(k, 0) + v
        for name, n in sorted(agg.items(), key=lambda kv: -kv[1]):
            per = n / float(chosen["n"])
            summary.append(f"  {n:6d}  ({per:6.2f}/frame)  {name}")

    # First 60 string draws and first 30 button creations -- the meat
    summary.append("")
    summary.append("First 60 string-rendering calls (with strings):")
    sn = 0
    for c in calls:
        if sn >= 60: break
        if c["name"].startswith("Draw") or c["name"] == "MeasureOrDrawFrontendFontString" \
                or c["name"] == "CreateMenuStringLabelSurface":
            summary.append(f"  f{c['frame']:>4} {c.get('screen','?'):<14} {c['name']:<40} {c}")
            sn += 1

    summary.append("")
    summary.append("First 30 CreateFrontendDisplayModeButton calls:")
    sn = 0
    for c in calls:
        if sn >= 30: break
        if c["name"].startswith("CreateFrontendDisplayMode") or c["name"] == "CreateFrontendMenuRectEntry":
            summary.append(f"  f{c['frame']:>4} {c.get('screen','?'):<14} {c}")
            sn += 1

    summary.append("")
    summary.append("First 30 sprite-blit dest rects (in Main Menu):")
    sn = 0
    for c in calls:
        if sn >= 30: break
        if c["name"] != "QueueFrontendSpriteBlit": continue
        if c.get("screen") != "MainMenu": continue
        summary.append(f"  f{c['frame']:>4} dst=({c.get('dx')},{c.get('dy')})-({c.get('dx2')},{c.get('dy2')}) src=({c.get('sx')},{c.get('sy')}) surf={c.get('surf')}")
        sn += 1
    summary.append("")
    summary.append("First 30 fill-rect calls (during MainMenu):")
    sn = 0
    for c in calls:
        if sn >= 30: break
        if c["name"] not in ("FillPrimaryFrontendRect","FillSurfaceRectWithColor","ClearBackbufferWithColor"):
            continue
        if c.get("screen") != "MainMenu": continue
        summary.append(f"  f{c['frame']:>4} {c['name']:<32} color=0x{(c.get('color') or 0)&0xffffffff:08x} rect=({c.get('x')},{c.get('y')},{c.get('w')},{c.get('h')})")
        sn += 1

    text = "\n".join(summary)
    with open(SUMM_PATH, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print()
    try:
        print(text)
    except UnicodeEncodeError:
        sys.stdout.buffer.write(text.encode("utf-8", "replace"))
        print()
    log.close()
    print(f"\n[+] log : {LOG_PATH}")
    print(f"[+] summary: {SUMM_PATH}")


if __name__ == "__main__":
    main()
