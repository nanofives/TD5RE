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
    "race":     ["game_type", "track", "direction", "laps", "start_span_offset"],
    "car":      ["car", "paint", "transmission"],
    "drag":     ["opponent_car"],
    "frontend": ["frontend_screen"],
}
BOOL_FIELDS = {
    "launcher":  ["verbose", "trace_track_load", "player_is_ai"],
    "frontend":  ["frontend_only"],
}


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
        "frontend_screen": -1,
        "frontend_only": False,
        "verbose": False,
        "trace_track_load": False,
        "player_is_ai": False,
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
        if field in ("verbose", "trace_track_load", "player_is_ai", "frontend_only"):
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
                       hook_specs: list | None = None,
                       csv_out: str | None = None,
                       brake_csv_out: str | None = None,
                       contacts_csv_out: str | None = None,
                       calls_csv_out: str | None = None,
                       trace_modules: str | None = None,
                       trace_stages:  str | None = None,
                       module_csv_base: str | None = None) -> str:
    """Rewrite TRACE_MAX_FRAMES / TRACE_MAX_SIM_TICK constants in the trace script.

    If hook_specs is provided, also injects the JSON-encoded spec array as the
    script's HOOK_SPECS global so the Frida side can install custom Interceptor
    hooks via its installHookSpecs() helper.

    If any of the *_csv_out paths are provided, rewrites the matching hardcoded
    OUTPUT_PATH / BRAKE_OUTPUT_PATH / CONTACTS_OUTPUT_PATH / CALLS_OUTPUT_PATH
    constants so parallel sessions can write to per-session CSVs without
    stomping on each other's output. Paths are JSON-quoted so backslashes
    and other Windows path characters land in the JS string correctly.
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

    # Per-session CSV path rewrites. The trace script declares each constant
    # as `var <NAME> = "<absolute path>";` near the top; we match the var
    # binding and replace the literal with a JSON-quoted alternative.
    import json as _json
    csv_subs = [
        (csv_out,          "OUTPUT_PATH"),
        (brake_csv_out,    "BRAKE_OUTPUT_PATH"),
        (contacts_csv_out, "CONTACTS_OUTPUT_PATH"),
        (calls_csv_out,    "CALLS_OUTPUT_PATH"),
    ]
    for new_path, var_name in csv_subs:
        if not new_path:
            continue
        quoted = _json.dumps(new_path)  # JSON-quoted JS string literal
        pat = r"var\s+" + var_name + r"\s*=\s*\"[^\"]*\";"
        # re.sub interprets backslashes in the replacement as backreferences;
        # use a lambda to pass the replacement verbatim so JSON-escaped
        # backslashes survive into the JS source.
        replacement = f"var {var_name} = {quoted};"
        source = re.sub(pat, lambda _m: replacement, source, count=1)

    # Modular trace selection. The Frida side declares MODULE_MASK_CSV /
    # STAGE_MASK_CSV / MODULE_PATHS at the top of the script; we rewrite
    # those literals so the original-side trace mirrors the port's
    # --TraceModules / --TraceStages / per-module CSV destinations.
    if trace_modules:
        quoted = _json.dumps(trace_modules)
        source = re.sub(
            r"var\s+MODULE_MASK_CSV\s*=\s*\"[^\"]*\";",
            lambda _m: f"var MODULE_MASK_CSV = {quoted};",
            source, count=1,
        )
    if trace_stages:
        quoted = _json.dumps(trace_stages)
        source = re.sub(
            r"var\s+STAGE_MASK_CSV\s*=\s*\"[^\"]*\";",
            lambda _m: f"var STAGE_MASK_CSV = {quoted};",
            source, count=1,
        )
    if module_csv_base:
        # The Frida side reads MODULE_CSV_BASE and constructs
        # `<base>_<module>.csv` per module at init time. JSON-quoting
        # preserves Windows backslashes through re.sub.
        quoted = _json.dumps(module_csv_base)
        source = re.sub(
            r"var\s+MODULE_CSV_BASE\s*=\s*\"[^\"]*\";",
            lambda _m: f"var MODULE_CSV_BASE = {quoted};",
            source, count=1,
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
    ap.add_argument("--no-ini", action="store_true",
                    help="skip td5_quickrace.ini entirely; use only built-in "
                         "defaults + explicit CLI flags. Use this for "
                         "orchestrated runs (diff_race.py, /fix, automated "
                         "probes) so the launcher is hermetically sealed "
                         "from INI contamination by parallel interactive "
                         "sessions. The INI's `frontend_only=true` from a "
                         "stale interactive session was the previous source "
                         "of 'launches into main menu' surprises.")
    ap.add_argument("--set", dest="overrides", action="append", default=[],
                    help="override e.g. race.track=5 (repeatable)")
    ap.add_argument("--exe", default=None, help="override launcher.exe")
    ap.add_argument("--no-resume", action="store_true",
                    help="leave process suspended after injection")
    ap.add_argument("--attach-pid", dest="attach_pid", type=int, default=None,
                    help="attach to an already-running TD5_d3d.exe by PID "
                         "instead of frida.spawn(). Use when the spawn path "
                         "crashes (e.g., Win11 M2DX init regression) — "
                         "launch the game manually first, then pass its PID. "
                         "Skips frida.resume() and does NOT kill the process "
                         "on exit.")
    ap.add_argument("--attach-name", dest="attach_name", default=None,
                    help="like --attach-pid but locate the PID by process "
                         "name (default: TD5_d3d.exe). Picks the first match.")
    ap.add_argument("--spawn", action="store_true",
                    help="force the historical frida.spawn() launch path. "
                         "BROKEN on Win11 — trips an M2DX.dll NULL-deref at "
                         "+0x144B during DirectSound init (see memory: "
                         "reference_m2dx_crash_2026-04-29.md). The default "
                         "is now attach-mode: if the target isn't running, "
                         "the launcher does `cmd /c start \"\" <exe>` and "
                         "polls until the PID appears. Use --spawn only on "
                         "non-Win11 boxes or when explicitly diagnosing "
                         "spawn-path behavior.")
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
    ap.add_argument("--max-runtime", dest="max_runtime", type=int, default=180,
                    help="hard wall-clock cap on this Frida session, in "
                         "seconds. When the cap is hit the launcher detaches "
                         "and (in spawn mode) kills TD5_d3d.exe — same as "
                         "--trace-auto-exit, but works even when --trace is "
                         "off or the trace script never signals. Default: "
                         "180s. Pass 0 to run indefinitely (only do this for "
                         "interactive debugging — orchestrators MUST pass a "
                         "positive value so /fix and /diff-race can never "
                         "leak a hung TD5_d3d.exe). The cap is enforced on "
                         "top of --trace-auto-exit; whichever fires first "
                         "wins.")
    ap.add_argument("--hook-specs", default=None,
                    help="path to a YAML/JSON hook-specs file (see "
                         "re/trace-hooks/README.md); injected into the trace "
                         "script as HOOK_SPECS so Frida attaches per-fn hooks")
    ap.add_argument("--extra-script", action="append", default=[],
                    help="additional Frida JS script(s) to inject into the "
                         "same session (repeatable). Loaded after the hook + "
                         "trace scripts. Use for ad-hoc probes.")

    # First-class scenario flags. These bypass the INI entirely so parallel
    # sessions can run different scenarios without colliding on the shared
    # td5_quickrace.ini file. Precedence (low → high): INI defaults → --set
    # overrides → these named flags. Any flag left at None falls through.
    sc = ap.add_argument_group("scenario (overrides INI without mutating it)")
    sc.add_argument("--track", type=int, default=None,
                    help="track schedule index (overrides INI [race].track)")
    sc.add_argument("--car", type=int, default=None,
                    help="car id (overrides INI [car].car)")
    sc.add_argument("--game-type", dest="game_type", type=int, default=None,
                    help="game type 0..n (overrides INI [race].game_type)")
    sc.add_argument("--laps", type=int, default=None,
                    help="circuit laps (overrides INI [race].laps)")
    sc.add_argument("--direction", type=int, default=None,
                    help="track direction 0|1 (overrides INI [race].direction)")
    sc.add_argument("--start-span-offset", dest="start_span_offset",
                    type=int, default=None,
                    help="grid spawn span offset (overrides INI [race].start_span_offset)")
    sc.add_argument("--paint", type=int, default=None,
                    help="car paint index (overrides INI [car].paint)")
    sc.add_argument("--transmission", type=int, default=None,
                    help="transmission 0|1 (overrides INI [car].transmission)")
    sc.add_argument("--opponent-car", dest="opponent_car",
                    type=int, default=None,
                    help="opponent car id (overrides INI [opponent].car)")
    sc.add_argument("--player-is-ai", dest="player_is_ai",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=None,
                    help="route slot 0 through AI dispatch (true|false; "
                         "overrides INI [launcher].player_is_ai)")
    sc.add_argument("--frontend-only", dest="frontend_only",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=None,
                    help="bypass intro/legal but let the menu run (true|false)")
    sc.add_argument("--frontend-screen", dest="frontend_screen",
                    type=int, default=None,
                    help="jump to frontend screen N instead of launching a race")
    sc.add_argument("--seed-crt", dest="seed_crt",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=None,
                    help="call _srand(crt_seed) before InitializeRaceSeriesSchedule")
    sc.add_argument("--crt-seed", dest="crt_seed",
                    type=lambda s: int(s, 0), default=None,
                    help="CRT seed value when --seed-crt is true (default 0x1A2B3C4D)")
    sc.add_argument("--verbose", dest="verbose",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=None,
                    help="enable hook verbose logging (true|false)")
    sc.add_argument("--trace-track-load", dest="trace_track_load",
                    type=lambda v: v.lower() in ("1", "true", "yes", "on"),
                    default=None,
                    help="log LoadTrackRuntimeData + spawn pose calls (true|false)")

    # Per-session output path overrides — let parallel sessions write to
    # different CSVs without stomping on each other's results. When set,
    # patch_trace_script() rewrites the matching OUTPUT_PATH constant in the
    # trace script before injection.
    co = ap.add_argument_group("per-session output paths (parallel-safe)")
    co.add_argument("--csv-out", dest="csv_out", default=None,
                    help="absolute path for the main race trace CSV "
                         "(default: log/race_trace_original.csv)")
    co.add_argument("--brake-csv-out", dest="brake_csv_out", default=None,
                    help="absolute path for race_trace_brake_original.csv")
    co.add_argument("--contacts-csv-out", dest="contacts_csv_out", default=None,
                    help="absolute path for race_trace_contacts_original.csv")
    co.add_argument("--calls-csv-out", dest="calls_csv_out", default=None,
                    help="absolute path for calls_trace_original.csv")
    co.add_argument("--trace-modules", dest="trace_modules", default=None,
                    help="comma list of trace modules to capture "
                         "(frame,pose,motion,track,controls,progress,view,calls). "
                         "Mirrors the port's --TraceModules so both sides emit "
                         "the same per-module CSVs.")
    co.add_argument("--trace-stages", dest="trace_stages", default=None,
                    help="comma list of stage rows to keep within each module CSV. "
                         "Mirrors the port's --TraceStages.")
    co.add_argument("--module-csv-base", dest="module_csv_base", default=None,
                    help="path stem for per-module original-side CSVs. The "
                         "Frida script appends '_<module>.csv' at runtime.")

    args = ap.parse_args()

    if args.no_ini:
        # Built-in defaults only — same shape as load_ini's `out` dict.
        cfg = {
            "game_type": 0, "track": 0, "direction": 0, "laps": 3,
            "car": 0, "paint": 0, "transmission": 0,
            "start_span_offset": 0,
            "opponent_car": 0,
            "frontend_screen": -1,
            "frontend_only": False,
            "verbose": False,
            "trace_track_load": False,
            "player_is_ai": False,
        }
        ini_exe = "TD5_d3d.exe"
        print("[launcher] --no-ini: skipping td5_quickrace.ini, using built-in defaults")
    else:
        cfg, ini_exe = load_ini(args.ini)
    apply_overrides(cfg, args.overrides)
    # First-class scenario flags win over INI + --set; None = unset (fall through).
    for k in ("track", "car", "game_type", "laps", "direction",
              "start_span_offset", "paint", "transmission", "opponent_car",
              "player_is_ai", "frontend_only", "frontend_screen",
              "seed_crt", "crt_seed", "verbose", "trace_track_load"):
        v = getattr(args, k, None)
        if v is not None:
            cfg[k] = v
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

    # Default to auto-spawn mode (launch detached, then poll for the new PID)
    # because frida.spawn() is broken on Win11 — the spawned child's audio
    # session inheritance trips M2DX.dll's DirectSound init at +0x144B.
    # Pass --spawn to force the historical frida.spawn() path.
    #
    # CRITICAL: in default mode we ALWAYS spawn a fresh process, even if a
    # TD5_d3d.exe is already running. Earlier behavior was "attach to the
    # first match systemwide", which silently hijacked another /fix or
    # /diff-race session's process — injecting cross-session Frida scripts
    # routinely crashed the victim. Snapshot the existing PIDs before
    # spawning and pick only the new one. To explicitly attach to an
    # already-running process, pass --attach-pid or --attach-name.
    user_attach = args.attach_pid is not None or args.attach_name is not None
    auto_spawn_default = not args.spawn and not user_attach
    if auto_spawn_default:
        args.attach_name = exe_name
    attach_mode = args.attach_pid is not None or args.attach_name is not None
    # Tracks whether WE spawned this PID (so we can safely kill on cleanup).
    # User-attached processes (manual --attach-pid / --attach-name match) are
    # never killed — they belong to the user.
    we_own_pid = False
    if attach_mode:
        if args.attach_pid is not None:
            pid = args.attach_pid
        else:
            target_name = (args.attach_name or "TD5_d3d.exe").lower()
            # frida 16+ removed the module-level enumerate_processes(); the
            # local device is the right entry point on Windows.
            device = frida.get_local_device()
            def _find_pid():
                return [p for p in device.enumerate_processes()
                        if p.name.lower() == target_name]
            existing = _find_pid()
            existing_pids = {p.pid for p in existing}
            if auto_spawn_default:
                # Default path: always spawn fresh so concurrent sessions stay
                # isolated. Other sessions' PIDs in `existing_pids` are left
                # strictly alone — we filter them out of the post-spawn match.
                if existing_pids:
                    print(f"[launcher] {len(existing_pids)} {target_name!r} "
                          f"process(es) already running (pids="
                          f"{sorted(existing_pids)}); leaving them untouched "
                          f"and spawning our own")
                print(f"[launcher] auto-spawning {exe_name} from {ORIGINAL_DIR}")
                subprocess.run(
                    ["cmd", "/c", "start", "", exe_name],
                    cwd=ORIGINAL_DIR, check=True,
                )
                t0 = time.monotonic()
                deadline = t0 + 10.0
                new_matches = []
                while time.monotonic() < deadline:
                    new_matches = [p for p in _find_pid()
                                   if p.pid not in existing_pids]
                    if new_matches:
                        break
                    time.sleep(0.01)
                else:
                    sys.exit(f"[launcher] auto-spawned {target_name!r} did "
                             f"not appear within 10s (existing pids "
                             f"{sorted(existing_pids)} ignored)")
                pid = new_matches[0].pid
                we_own_pid = True
                if len(new_matches) > 1:
                    # Two sessions racing the spawn at the same instant could
                    # both see each other's new PID. Pick the lowest-PID one
                    # deterministically and warn — both sessions will end up
                    # with their own freshly-spawned process.
                    print(f"[launcher] warning: {len(new_matches)} new "
                          f"{target_name!r} processes appeared during spawn "
                          f"window; attaching to lowest pid={pid}")
                print(f"[launcher] auto-spawned new pid in "
                      f"{(time.monotonic() - t0) * 1000:.0f} ms: {pid}")
            else:
                # User passed --attach-name explicitly — attach to a running
                # process. If none exists, auto-launch as a convenience and
                # take ownership of the spawned PID.
                if not existing:
                    print(f"[launcher] no running {target_name!r}; "
                          f"detached-launching {exe_name} from {ORIGINAL_DIR}")
                    subprocess.run(
                        ["cmd", "/c", "start", "", exe_name],
                        cwd=ORIGINAL_DIR, check=True,
                    )
                    t0 = time.monotonic()
                    deadline = t0 + 10.0
                    while time.monotonic() < deadline:
                        existing = _find_pid()
                        if existing:
                            break
                        time.sleep(0.01)
                    else:
                        sys.exit(f"[launcher] {target_name!r} did not appear "
                                 f"within 10s of detached launch")
                    we_own_pid = True
                    print(f"[launcher] caught new pid in "
                          f"{(time.monotonic() - t0) * 1000:.0f} ms")
                pid = existing[0].pid
                if len(existing) > 1:
                    print(f"[launcher] warning: {len(existing)} processes "
                          f"match {target_name!r}; attaching to first "
                          f"(pid={pid}) — pass --attach-pid to disambiguate")
        print(f"[launcher] attaching to pid: {pid}")
    else:
        print(f"[launcher] spawning: {exe_path}")
        pid = frida.spawn(exe_path, cwd=ORIGINAL_DIR)
        we_own_pid = True
        print(f"[launcher] pid: {pid}")

    session = frida.attach(pid)
    script = session.create_script(js_source)
    script.on("message", on_message)
    script.load()

    # When --trace is active we align the Frida harness's CRT _holdrand with
    # the port's deterministic seed so the AI-car selection in
    # InitializeRaceSeriesSchedule draws from the matching sequence. Without
    # this the Frida capture's AI-car set diverges from the port's even when
    # td5re.ini [Trace] RaceTraceEnabled=1 is set. See:
    #   todo_ai_spawn_world_y_divergence.md (Issue 9 follow-up)
    if args.trace:
        cfg.setdefault("seed_crt", True)
        cfg.setdefault("crt_seed", 0x1A2B3C4D)

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
            hook_specs=hook_specs,
            csv_out=args.csv_out,
            brake_csv_out=args.brake_csv_out,
            contacts_csv_out=args.contacts_csv_out,
            calls_csv_out=args.calls_csv_out,
            trace_modules=args.trace_modules,
            trace_stages=args.trace_stages,
            module_csv_base=args.module_csv_base)
        print(f"[launcher] injecting trace script "
              f"(max_frames={args.trace_max_frames} "
              f"max_sim_tick={args.trace_max_sim_tick}"
              f"{' hooks=' + str(len(hook_specs)) if hook_specs else ''})")
        trace_script = session.create_script(trace_source)
        trace_script.on("message", on_message)
        trace_script.load()

    extra_scripts = []
    for extra_path in args.extra_script:
        if not os.path.exists(extra_path):
            sys.exit(f"--extra-script not found: {extra_path}")
        with open(extra_path, "r", encoding="utf-8") as f:
            extra_source = f.read()
        print(f"[launcher] injecting extra script: {extra_path}")
        es = session.create_script(extra_source)
        es.on("message", on_message)
        es.load()
        extra_scripts.append(es)

    if attach_mode:
        print("[launcher] attached to running process; skipping resume")
    elif args.no_resume:
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

    deadline = None
    if args.max_runtime and args.max_runtime > 0:
        deadline = time.monotonic() + args.max_runtime
        print(f"[launcher] max-runtime cap: {args.max_runtime}s "
              f"(deadline = monotonic+{args.max_runtime})")
    elif args.max_runtime == 0:
        print("[launcher] max-runtime cap DISABLED (--max-runtime 0); "
              "session will run until --trace-auto-exit fires or Ctrl-C")

    timed_out = False
    try:
        while True:
            time.sleep(0.5)
            if args.trace_auto_exit and g_auto_close:
                if we_own_pid:
                    print("[launcher] trace auto-close; detaching and killing target")
                else:
                    print("[launcher] trace auto-close; detaching (process kept alive — owned by user)")
                break
            if deadline is not None and time.monotonic() >= deadline:
                timed_out = True
                if we_own_pid:
                    print(f"[launcher] max-runtime ({args.max_runtime}s) hit; "
                          f"detaching and killing target")
                else:
                    print(f"[launcher] max-runtime ({args.max_runtime}s) hit; "
                          f"detaching (process kept alive — owned by user)")
                break
    except KeyboardInterrupt:
        print("\n[launcher] detaching")
    finally:
        try:
            session.detach()
        except Exception:
            pass
        # Kill only PIDs we spawned ourselves (we_own_pid). User-attached
        # processes (manual --attach-pid / --attach-name onto a running
        # process) belong to the user and are left alone, even on timeout.
        # This ensures no parallel session's TD5_d3d.exe is ever killed.
        if (args.trace_auto_exit or timed_out) and we_own_pid:
            try:
                frida.kill(pid)
            except Exception:
                pass


if __name__ == "__main__":
    main()
