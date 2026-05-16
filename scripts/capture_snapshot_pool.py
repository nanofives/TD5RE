#!/usr/bin/env python3
"""
capture_snapshot_pool.py -- Parallel snapshot capture across the Frida pool.

Spawns up to N TD5_d3d.exe processes (one per frida_pool/slot* dir), attaches
a Frida session to each, injects frida_state_snapshot.js with a per-capture
output path, and waits for all captures to complete. The result is up to N
state_snapshot_<scenario>.bin files in tools/frida_csv/, captured concurrently.

Scenario list is the SCENARIOS table below (track, car, ai_mode, output_stem
tuples). Edit there to add new scenarios.

Usage:
  python scripts/capture_snapshot_pool.py             # capture all scenarios
  python scripts/capture_snapshot_pool.py --list      # show scenarios
  python scripts/capture_snapshot_pool.py --only honolulu_ai,edinburgh_ai
  python scripts/capture_snapshot_pool.py --parallel 4   # at most 4 concurrent
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

import frida

PROJECT_ROOT = Path("C:/Users/maria/Desktop/Proyectos/TD5RE")
POOL_SCRIPT  = PROJECT_ROOT / "scripts" / "frida_pool.sh"
SNAP_SCRIPT  = PROJECT_ROOT / "tools"   / "frida_state_snapshot.js"
QUICKRACE_DIR = PROJECT_ROOT / "re" / "tools" / "quickrace"
QUICKRACE_HOOK = QUICKRACE_DIR / "td5_quickrace_hook.js"
SNAPSHOT_OUT_DIR = PROJECT_ROOT / "tools" / "frida_csv"

POOL_DIR = PROJECT_ROOT / "frida_pool"

# ---- Scenario catalog --------------------------------------------------------
@dataclass
class Scenario:
    name: str
    track: int
    car: int
    player_is_ai: bool
    laps: int = 3
    direction: int = 0
    opponent_car: int = 0   # used for drag mode
    game_type: int = 0      # 0=race, 1=cup, 3=drag, etc.

SCENARIOS = [
    Scenario("honolulu_ai_viper",   8, 0, True),
    Scenario("honolulu_hum_viper",  8, 0, False),
    Scenario("edinburgh_ai_viper",  1, 0, True),
    Scenario("edinburgh_ai_sky",    1, 4, True),
    Scenario("moscow_ai_viper",     4, 0, True),
    Scenario("moscow_ai_gtr",       4, 19, True),
    Scenario("jarash_ai_viper",     2, 0, True),
    Scenario("athens_ai_viper",     3, 0, True),
    Scenario("athens_ai_xkr",       3, 7, True),
    Scenario("cuba_ai_viper",       5, 0, True),
    Scenario("hkong_ai_viper",      6, 0, True),
    Scenario("nyc_ai_viper",        7, 0, True),
    Scenario("honolulu_ai_xkr",     8, 7, True),
    Scenario("honolulu_ai_gtr",     8, 19, True),
    Scenario("edinburgh_hum_viper", 1, 0, False),
    Scenario("moscow_hum_viper",    4, 0, False),
    Scenario("sydney_ai_viper",     2, 0, True),
    Scenario("blueridge_ai_viper",  3, 0, True),
]

# ---- Pool helpers (inline Python to avoid bash-from-Python quirks) ----------
_pool_lock = threading.Lock()

# Serialize the spawn-and-detect-PID phase. Without this, parallel threads
# both snapshot the existing-PID set, both spawn, then both see each other's
# new PIDs and can claim the same one.
_spawn_lock = threading.Lock()

# Launch semaphore. Capacity controls how many TD5_d3d.exe processes may be
# running concurrently. Default 1 = sequential captures (avoids focus-pause
# starvation since TD5_d3d.exe halts its sim loop when not foreground; only
# the focused window actually ticks).
LAUNCH_CAPACITY = 1
_launch_sema: Optional[threading.Semaphore] = None
def _get_launch_sema():
    global _launch_sema
    if _launch_sema is None:
        _launch_sema = threading.Semaphore(LAUNCH_CAPACITY)
    return _launch_sema

# Per-capture hard timeout. Catches the case where a launched game never
# reaches the snapshot-complete state -- e.g., menu navigation hook didn't
# fire, the window lost focus, M2DX threw a Frida-attach race, etc. Without
# this, one stuck capture blocks the entire queue forever.
CAPTURE_HARD_TIMEOUT_S = 90.0

# Window focus helpers (ctypes -- no extra deps).
import ctypes
import ctypes.wintypes as wt
_user32 = ctypes.windll.user32
_kernel32 = ctypes.windll.kernel32

def focus_pid_window(pid: int, timeout_s: float = 5.0) -> bool:
    """Bring the first visible top-level window owned by `pid` to the
    foreground. TD5_d3d.exe pauses its race-frame loop when not foreground
    (focus-pause), so we MUST do this before the snapshot will tick."""
    WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    found = {"hwnd": None}
    def enum(hwnd, _lparam):
        owner = wt.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and _user32.IsWindowVisible(hwnd):
            found["hwnd"] = hwnd
            return False
        return True
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        _user32.EnumWindows(WNDENUMPROC(enum), 0)
        if found["hwnd"]:
            break
        time.sleep(0.05)
    if not found["hwnd"]:
        return False
    _user32.ShowWindow(found["hwnd"], 9)         # SW_RESTORE
    _user32.SetForegroundWindow(found["hwnd"])
    return True

def pool_acquire() -> Optional[Path]:
    """Return first unlocked slot dir; create its .lock file. None if full."""
    with _pool_lock:
        if not POOL_DIR.exists():
            print(f"[pool] not initialized; run scripts/frida_pool.sh init",
                  file=sys.stderr)
            return None
        slots = sorted(p for p in POOL_DIR.iterdir()
                       if p.is_dir() and p.name.startswith("slot"))
        for d in slots:
            n = d.name.replace("slot", "")
            lock = POOL_DIR / f"slot{n}.lock"
            if not lock.exists():
                lock.touch()
                return d
        return None

def pool_release(slot_dir: Path):
    n = slot_dir.name.replace("slot", "")
    lock = POOL_DIR / f"slot{n}.lock"
    with _pool_lock:
        if lock.exists():
            lock.unlink()

def pool_cleanup():
    """Clear all locks before a fresh capture run."""
    if not POOL_DIR.exists(): return
    for f in POOL_DIR.glob("*.lock"):
        try: f.unlink()
        except: pass

# ---- Frida launch ------------------------------------------------------------
def make_quickrace_cfg(s: Scenario) -> dict:
    return {
        "game_type":  s.game_type,
        "track":      s.track,
        "direction":  s.direction,
        "laps":       s.laps,
        "car":        s.car,
        "paint":      0,
        "transmission": 0,
        "start_span_offset": 0,
        "opponent_car": s.opponent_car,
        "verbose":      False,
        "trace_track_load": False,
        "player_is_ai": s.player_is_ai,
        "seed_crt":     False,
        "crt_seed":     439041101,
        "frontend_screen": -1,
        "frontend_only": False,
    }

def read_script_with_overrides(out_path: Path, max_frames: int = 200) -> str:
    """Read frida_state_snapshot.js and override OUTPUT_PATH + MAX_FRAMES.
    The path is emitted with forward slashes to avoid escape pitfalls -- Frida's
    File() accepts both on Windows. Forward-slash paths also need no escaping
    when embedded in a JS string literal."""
    src = SNAP_SCRIPT.read_text()
    posix_path = str(out_path).replace("\\", "/")
    src = re.sub(
        r'var OUTPUT_PATH = "[^"]*";',
        f'var OUTPUT_PATH = "{posix_path}";',
        src, count=1
    )
    src = re.sub(
        r'var MAX_FRAMES\s*=\s*\d+;',
        f'var MAX_FRAMES  = {max_frames};',
        src, count=1
    )
    return src

@dataclass
class CaptureResult:
    scenario: str
    slot_dir: Path
    out_path: Path
    success: bool
    frames: int
    error: Optional[str] = None
    elapsed_s: float = 0.0

def capture_one(s: Scenario, slot_dir: Path, max_frames: int = 200,
                spawn_delay_s: float = 0.0) -> CaptureResult:
    """Launch one TD5_d3d.exe from slot_dir, attach Frida, capture, kill.

    The actual game-running phase is guarded by `_launch_sema` (default cap=1)
    so only one TD5_d3d.exe runs at a time -- a hard requirement because the
    game pauses its frame loop when its window isn't foreground. Worker
    threads queue up here; the pre-launch work (slot acquire, INI parse,
    etc.) happens in parallel."""
    t0 = time.monotonic()
    out_path = SNAPSHOT_OUT_DIR / f"state_snapshot_{s.name}.bin"
    SNAPSHOT_OUT_DIR.mkdir(parents=True, exist_ok=True)
    if out_path.exists():
        out_path.unlink()

    sema = _get_launch_sema()
    # spawn_delay_s was a stagger hack for the no-semaphore design; keep the
    # parameter accepted but ignore it -- semaphore-serialized launches don't
    # race each other.
    _ = spawn_delay_s

    # Block here until the launch slot is free. Each scenario hits its own
    # CAPTURE_HARD_TIMEOUT_S only AFTER it acquires; queueing time is unbounded.
    acquired = sema.acquire(timeout=600.0)  # 10 min cap on total queue wait
    if not acquired:
        return CaptureResult(s.name, slot_dir, out_path, False, 0,
                             error="queue wait > 600s -- pipeline jammed",
                             elapsed_s=time.monotonic() - t0)
    try:
        return _capture_via_quickrace(s, out_path, max_frames, t0)
    finally:
        sema.release()

QUICKRACE_PY = PROJECT_ROOT / "re" / "tools" / "quickrace" / "td5_quickrace.py"

def _capture_via_quickrace(s, out_path, max_frames, t0):
    """Drive a single capture by shelling out to td5_quickrace.py with a
    per-scenario snapshot script written to a tempfile. This piggy-backs on
    the proven detached-launch + poll-attach + focus-window infrastructure
    that already works for the user's run_state_replay.py --capture-orig flow.
    All captures use original/ as cwd (so frida_pool slots are unused in this
    sequential mode -- they remain available for the future Frida-focus-pause
    nop work that would enable real parallelism)."""
    # Write a per-scenario snapshot script with our OUTPUT_PATH baked in.
    snap_src = read_script_with_overrides(out_path, max_frames)
    import tempfile
    with tempfile.NamedTemporaryFile(mode="w", suffix=".js", delete=False,
                                      encoding="utf-8") as f:
        f.write(snap_src)
        snap_path = f.name

    try:
        cmd = [
            sys.executable, str(QUICKRACE_PY),
            "--track", str(s.track),
            "--car",   str(s.car),
            "--player-is-ai", "true" if s.player_is_ai else "false",
            "--game-type", str(s.game_type),
            "--laps", str(s.laps),
            "--direction", str(s.direction),
            "--extra-script", snap_path,
            "--trace-auto-exit",
            "--max-runtime", str(int(CAPTURE_HARD_TIMEOUT_S)),
        ]
        if s.opponent_car:
            cmd += ["--opponent-car", str(s.opponent_car)]
        # Run quickrace as a Popen so we can early-kill when the snapshot
        # file stops growing (= snapshot script finished writing). Without
        # this we always wait the full --max-runtime cap.
        proc = subprocess.Popen(cmd, cwd=str(PROJECT_ROOT),
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True)
        EXPECTED_BYTES = 32 + max_frames * 7272   # full file size at cap
        STABLE_S = 3.0                            # require N s of no growth
        last_size = -1
        last_growth_t = time.monotonic()
        deadline = time.monotonic() + CAPTURE_HARD_TIMEOUT_S + 30
        while proc.poll() is None and time.monotonic() < deadline:
            time.sleep(0.5)
            try:
                cur = out_path.stat().st_size if out_path.exists() else 0
            except Exception:
                cur = 0
            if cur != last_size:
                last_size = cur
                last_growth_t = time.monotonic()
            # Early exit when we're at full capacity AND stable for STABLE_S
            if cur >= EXPECTED_BYTES and (time.monotonic() - last_growth_t) > 0.5:
                # kill TD5_d3d.exe + the quickrace subprocess
                subprocess.run(["cmd", "/c", "taskkill", "/F", "/IM",
                                "TD5_d3d.exe"], capture_output=True)
                try: proc.terminate()
                except: pass
                break
            # Also early-exit if file has been stable for STABLE_S (capture
            # finished but file is below expected because game ended early).
            if cur > 1024 and (time.monotonic() - last_growth_t) > STABLE_S:
                subprocess.run(["cmd", "/c", "taskkill", "/F", "/IM",
                                "TD5_d3d.exe"], capture_output=True)
                try: proc.terminate()
                except: pass
                break
        try:
            proc.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            proc.kill()
        elapsed = time.monotonic() - t0
        if proc.returncode and proc.returncode != 0 and proc.returncode is not None:
            # Don't fail just on non-zero rc -- our early-kill produces non-zero
            pass
        if not out_path.exists() or out_path.stat().st_size < 1024:
            return CaptureResult(s.name, Path("original"), out_path, False, 0,
                                 error="output missing/empty",
                                 elapsed_s=elapsed)
        # Frame count = (filesize - header) / record_size
        size = out_path.stat().st_size
        frames = max(0, (size - 32) // 7272)
        return CaptureResult(s.name, Path("original"), out_path, True,
                             frames, elapsed_s=elapsed)
    except subprocess.TimeoutExpired:
        return CaptureResult(s.name, Path("original"), out_path, False, 0,
                             error=f"quickrace subprocess timed out",
                             elapsed_s=time.monotonic() - t0)
    finally:
        try: os.unlink(snap_path)
        except: pass

def _capture_one_inner(s, slot_dir, out_path, max_frames, t0):
    # Snapshot existing PIDs, spawn, claim the new PID -- all under the
    # spawn lock so two parallel threads can't race and claim each other's
    # (even with cap=1 today, keep the lock so cap can be raised safely).
    device = frida.get_local_device()
    new_pid = None
    with _spawn_lock:
        existing_pids = {p.pid for p in device.enumerate_processes()
                         if p.name.lower() == "td5_d3d.exe"}
        subprocess.Popen(
            ["cmd", "/c", "start", "", "TD5_d3d.exe"],
            cwd=str(slot_dir), close_fds=True,
        )
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            cur = {p.pid for p in device.enumerate_processes()
                   if p.name.lower() == "td5_d3d.exe"}
            new = cur - existing_pids
            if new:
                new_pid = sorted(new)[0]
                break
            time.sleep(0.05)
    if not new_pid:
        return CaptureResult(s.name, slot_dir, out_path, False, 0,
                             error="failed to detect spawned PID",
                             elapsed_s=time.monotonic() - t0)

    # Bring the new game window to the foreground so its race-frame loop
    # actually ticks. TD5_d3d.exe pauses when not foreground.
    focused = focus_pid_window(new_pid, timeout_s=5.0)
    print(f"[{s.name}] window focus: {'OK' if focused else 'FAILED'} (pid={new_pid})")

    # Attach + load scripts
    try:
        session = device.attach(new_pid)
    except frida.ProcessNotRespondingError as e:
        # The process is still booting; retry a few times.
        for _ in range(20):
            time.sleep(0.05)
            try:
                session = device.attach(new_pid)
                break
            except frida.ProcessNotRespondingError:
                continue
        else:
            try: os.kill(new_pid, 9)
            except Exception: pass
            return CaptureResult(s.name, slot_dir, out_path, False, 0,
                                 error=f"attach kept failing: {e}",
                                 elapsed_s=time.monotonic() - t0)

    # Inject quickrace hook (track/car config) + snapshot script.
    # quickrace_hook.js exposes recv('cfg', cb) to receive its config -- send
    # it after script.load() (the hook handles dispatch before the first tick).
    quickrace_src = QUICKRACE_HOOK.read_text()
    cfg = make_quickrace_cfg(s)
    snapshot_src = read_script_with_overrides(out_path, max_frames)

    captured_event = threading.Event()
    captured_frames = [0]
    last_error = [None]

    def on_message(msg, _data):
        try:
            if msg["type"] == "send":
                payload = msg.get("payload", {})
                if isinstance(payload, dict):
                    if payload.get("type") == "snapshot-complete":
                        captured_frames[0] = int(payload.get("frames", 0))
                        captured_event.set()
        except Exception:
            pass

    try:
        # Quickrace hook first (sets up race-launch automation).
        # It exposes recv('cfg', cb) to receive its config.
        # Capture hook output so we can see if it's stuck somewhere.
        # The hook uses console.log -- in Frida that comes through as
        # {type: 'log', level: ..., payload: ...} OR as send({...}).
        def qr_on_message(msg, _data):
            mtype = msg.get("type")
            if mtype == "log":
                lvl = msg.get("level", "info")
                p   = msg.get("payload", "")
                print(f"[{s.name}/qr {lvl}] {p}")
            elif mtype == "send":
                p = msg.get("payload")
                print(f"[{s.name}/qr send] {p}")
            elif mtype == "error":
                print(f"[{s.name}/qr ERROR] {msg.get('description', msg)}")
            else:
                print(f"[{s.name}/qr {mtype}] {msg}")
        qr_script = session.create_script(quickrace_src)
        qr_script.on("message", qr_on_message)
        qr_script.load()
        qr_script.post({"type": "cfg", "cfg": cfg})

        # Snapshot script (parallel slot's own output path baked in).
        snap_script = session.create_script(snapshot_src)
        snap_script.on("message", on_message)
        snap_script.load()

        # Wait for snapshot-complete with the configured per-capture deadline.
        if not captured_event.wait(timeout=CAPTURE_HARD_TIMEOUT_S):
            last_error[0] = (f"snapshot did not complete within "
                             f"{CAPTURE_HARD_TIMEOUT_S:.0f}s")
    except Exception as e:
        last_error[0] = f"scripting error: {e}"
    finally:
        try: session.detach()
        except Exception: pass
        try: os.kill(new_pid, 9)
        except Exception: pass

    elapsed = time.monotonic() - t0
    if last_error[0]:
        return CaptureResult(s.name, slot_dir, out_path, False,
                             captured_frames[0], error=last_error[0],
                             elapsed_s=elapsed)
    if not out_path.exists() or out_path.stat().st_size < 1024:
        return CaptureResult(s.name, slot_dir, out_path, False,
                             captured_frames[0],
                             error=f"output missing/empty: {out_path}",
                             elapsed_s=elapsed)
    return CaptureResult(s.name, slot_dir, out_path, True,
                         captured_frames[0], elapsed_s=elapsed)

def repr_js_obj(d: dict) -> str:
    """Produce a JS object literal from a Python dict (very simple types)."""
    parts = []
    for k, v in d.items():
        if isinstance(v, bool):
            parts.append(f'"{k}": {"true" if v else "false"}')
        elif isinstance(v, (int, float)):
            parts.append(f'"{k}": {v}')
        else:
            parts.append(f'"{k}": "{v}"')
    return "{" + ", ".join(parts) + "}"

# ---- Parallel driver ---------------------------------------------------------
def run_parallel(scenarios: list[Scenario], max_parallel: int, max_frames: int):
    results: list[CaptureResult] = []
    queue = list(enumerate(scenarios))
    lock = threading.Lock()
    in_flight: list[threading.Thread] = []

    def worker(idx: int, s: Scenario):
        slot_dir = pool_acquire()
        if not slot_dir:
            with lock:
                results.append(CaptureResult(s.name, Path("?"), Path("?"),
                                              False, 0,
                                              error="no slot available"))
            return
        try:
            # Stagger spawns to avoid DirectSound init race when many
            # processes boot simultaneously.
            delay = idx * 0.5  # 500ms stagger
            r = capture_one(s, slot_dir, max_frames=max_frames,
                            spawn_delay_s=delay)
        finally:
            pool_release(slot_dir)
        with lock:
            results.append(r)
            status = "OK" if r.success else f"FAIL ({r.error})"
            print(f"[capture] {s.name:30s}  {status}  frames={r.frames}  "
                  f"{r.elapsed_s:.1f}s  slot={slot_dir.name}")

    # Reset all locks before starting
    pool_cleanup()

    # Launch in batches of max_parallel
    next_idx = 0
    while next_idx < len(queue) or in_flight:
        # Reap finished threads
        in_flight = [t for t in in_flight if t.is_alive()]
        # Launch new threads up to max_parallel
        while next_idx < len(queue) and len(in_flight) < max_parallel:
            idx, s = queue[next_idx]
            t = threading.Thread(target=worker, args=(idx, s),
                                 name=f"cap_{s.name}")
            t.start()
            in_flight.append(t)
            next_idx += 1
        time.sleep(0.2)

    for t in in_flight:
        t.join()
    return results

# ---- Main --------------------------------------------------------------------
def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--list", action="store_true", help="List scenarios")
    ap.add_argument("--only", help="Comma-separated scenario names")
    ap.add_argument("--parallel", type=int, default=4,
                    help="Worker thread count (default 4). Workers queue up "
                         "at the launch semaphore.")
    ap.add_argument("--launch-capacity", type=int, default=1,
                    help="Max concurrent TD5_d3d.exe processes (default 1). "
                         "Keep at 1 unless the focus-pause check has been "
                         "Frida-disabled -- only the foreground game ticks.")
    ap.add_argument("--capture-timeout", type=float, default=90.0,
                    help="Per-capture hard timeout in seconds (default 90).")
    ap.add_argument("--max-frames", type=int, default=200,
                    help="Frames per capture (default 200)")
    args = ap.parse_args(argv)

    if args.list:
        for s in SCENARIOS:
            print(f"  {s.name:30s}  track={s.track:>2}  car={s.car:>2}  "
                  f"ai={s.player_is_ai}")
        return 0

    scenarios = SCENARIOS
    if args.only:
        wanted = set(args.only.split(","))
        scenarios = [s for s in SCENARIOS if s.name in wanted]
        if not scenarios:
            print(f"No matching scenarios in --only={args.only}", file=sys.stderr)
            return 2

    # Apply CLI overrides for the global tunables.
    global LAUNCH_CAPACITY, CAPTURE_HARD_TIMEOUT_S, _launch_sema
    LAUNCH_CAPACITY = max(1, int(args.launch_capacity))
    CAPTURE_HARD_TIMEOUT_S = float(args.capture_timeout)
    _launch_sema = threading.Semaphore(LAUNCH_CAPACITY)
    print(f"Launch capacity={LAUNCH_CAPACITY}, per-capture timeout="
          f"{CAPTURE_HARD_TIMEOUT_S:.0f}s, workers={args.parallel}")

    print(f"Capturing {len(scenarios)} scenario(s) with up to "
          f"{args.parallel} concurrent processes...")
    t0 = time.monotonic()
    results = run_parallel(scenarios, args.parallel, args.max_frames)
    elapsed = time.monotonic() - t0

    ok = sum(1 for r in results if r.success)
    fail = len(results) - ok
    print(f"\nDone in {elapsed:.1f}s: {ok} OK, {fail} FAIL")
    return 0 if fail == 0 else 1

if __name__ == "__main__":
    raise SystemExit(main())
