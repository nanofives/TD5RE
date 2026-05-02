"""
diff_profiles.py
================
Named lenses for /diff-race. Each profile resolves to:

  - ``modules``  list[str]   — which per-module CSVs to capture + diff
  - ``stages``   list[str]   — which stage rows to keep within those CSVs
  - ``fields``   list[str]   — optional non-key column allowlist (within a
                                module's narrow schema). Empty = all cols.
  - ``float_tol`` float|None — comparator tolerance override

Modules and stages map 1:1 to the port's ``[Trace] Modules=`` /
``[Trace] Stages=`` knobs (see td5_trace.h). The orchestrator forwards them
as ``--TraceModules=...`` / ``--TraceStages=...`` to BOTH td5re.exe and the
Frida-side launcher, so each side only emits the rows the profile asks for.
The comparator then runs once per active module, joining on the module's
own narrow key schema.

Multiple profiles compose: their modules + stages + fields union (deduped,
order preserved).

Adding a profile = append a dict here. Keep ``fields`` lean — extra noise
columns hurt because the comparator's first-mismatch break (line 192 in
compare_race_trace.py) hides every field after the first divergent one.

Available trace dimensions:
  modules = frame | pose | motion | track | controls | progress | view | calls
  stages  = frame_begin | pre_physics | post_physics | post_ai | post_track |
            post_camera | post_progress | frame_end | pause_menu | countdown

Per-module column populations (see td5_trace.c file headers):
  - frame:    game_state, paused, pause_menu, fade_state, countdown_timer,
              sim_accum, sim_budget, frame_dt, instant_fps, viewport_count,
              split_mode, ticks_this_frame.
  - pose:     world_x/y/z, ang_roll/yaw/pitch, disp_roll/yaw/pitch.
  - motion:   vel_x/y/z, long_speed, lat_speed, front_slip, rear_slip.
  - track:    span_raw/norm/accum/high, track_contact, wheel_mask.
  - controls: slot_state, steering, engine, gear, vehicle_mode.
  - progress: race_pos, finish_time, accum_distance, pending_finish,
              metric_checkpoint, metric_mask, metric_norm_span,
              metric_timer, metric_display_pos, metric_speed_bonus,
              metric_top_speed.
  - view:     actor_slot, cam_world_x/y/z.
  - calls:    sim_tick, fn_name, call_idx, n_args, arg_0..arg_7, has_ret, ret.
"""

from __future__ import annotations

from typing import Dict, List, Optional, Sequence


# ---------------------------------------------------------------------------
# Profile catalog
# ---------------------------------------------------------------------------
#
# Each profile is keyed by its short name. Required keys:
#   description   one-line what-this-tests
#   modules       list[str] — per-module CSVs to diff
#   stages        list[str] — stage filter applied within those CSVs
# Optional keys:
#   fields        list[str] — narrow further to specific non-key columns
#   float_tol     float     — overrides --float-tol when set
#
# Order of insertion is preserved when --list-profiles prints the catalog.

PROFILES: Dict[str, dict] = {
    # -- High-level pulse -----------------------------------------------------
    "core": {
        "description": "Minimal baseline pulse - position + race progress per actor at end of tick. Default profile.",
        "modules": ["pose", "controls", "progress"],
        "stages":  ["post_progress"],
        "fields":  ["world_x", "world_y", "world_z",
                    "gear", "race_pos", "finish_time"],
    },

    # -- Physics --------------------------------------------------------------
    "physics": {
        "description": "Per-tick physics step output: position, velocity, body angles, slip, contact.",
        "modules": ["pose", "motion", "track"],
        "stages":  ["post_physics"],
        "fields":  ["world_x", "world_y", "world_z",
                    "vel_x", "vel_y", "vel_z",
                    "ang_roll", "ang_yaw", "ang_pitch",
                    "long_speed", "lat_speed",
                    "front_slip", "rear_slip",
                    "track_contact", "wheel_mask"],
    },
    "physics_full": {
        "description": "Physics + driver commands + render-side displacement (catch-all physics+AI lens).",
        "modules": ["pose", "motion", "track", "controls"],
        "stages":  ["pre_physics", "post_physics"],
    },
    "vehicle": {
        "description": "Drivetrain + ground-contact state.",
        "modules": ["controls", "motion", "track"],
        "stages":  ["post_physics"],
        "fields":  ["gear", "vehicle_mode", "track_contact", "wheel_mask",
                    "long_speed", "lat_speed"],
    },
    "steering": {
        "description": "Yaw response: steer command vs body yaw / display yaw / lateral speed.",
        "modules": ["controls", "pose", "motion"],
        "stages":  ["post_ai", "post_physics"],
        "fields":  ["steering", "ang_yaw", "disp_yaw", "lat_speed"],
    },

    # -- AI / track / progress -----------------------------------------------
    "ai": {
        "description": "AI command output: steer, engine, span position, race position.",
        "modules": ["controls", "track", "progress"],
        "stages":  ["post_ai"],
        "fields":  ["steering", "engine", "span_norm", "race_pos"],
    },
    "track": {
        "description": "Track-walker state: span (raw/norm/accum/high) + ground contact + wheel mask.",
        "modules": ["track"],
        "stages":  ["post_track"],
    },
    "progress": {
        "description": "Race progress / lap metrics / finish accounting.",
        "modules": ["progress"],
        "stages":  ["post_progress"],
    },
    "hud": {
        "description": "What the speedometer + minimap + timers read each tick.",
        "modules": ["progress", "controls", "motion"],
        "stages":  ["post_progress"],
        "fields":  ["gear", "race_pos", "long_speed",
                    "metric_checkpoint", "metric_norm_span", "metric_timer",
                    "metric_display_pos", "metric_top_speed",
                    "finish_time", "accum_distance"],
    },

    # -- One-off / inspection -------------------------------------------------
    "spawn": {
        "description": "Tick-0 spawn snapshot - position, heading, span - useful when grids drift before physics runs.",
        "modules": ["pose", "track"],
        "stages":  ["post_physics", "post_track"],
        "fields":  ["world_x", "world_y", "world_z", "ang_yaw", "span_norm"],
    },
    "slot_state": {
        "description": "Slot bookkeeping (state, etc.) — flag harness asymmetries explicitly.",
        "modules": ["controls"],
        "stages":  ["post_progress"],
        "fields":  ["slot_state"],
    },

    # -- Visuals --------------------------------------------------------------
    "view": {
        "description": "Camera world position per viewport.",
        "modules": ["view"],
        "stages":  ["post_camera", "frame_end"],
    },
    "display": {
        "description": "Render-time per-actor display angles (smoothed yaw/roll/pitch a frame later than body).",
        "modules": ["pose"],
        "stages":  ["frame_end"],
        "fields":  ["disp_roll", "disp_yaw", "disp_pitch"],
    },

    # -- Frame-level / frontend ----------------------------------------------
    "frontend": {
        "description": "Frontend / game-state FSM: game_state, paused, pause menu, fade, countdown.",
        "modules": ["frame"],
        "stages":  ["frame_begin", "frame_end"],
        "fields":  ["game_state", "paused", "pause_menu", "fade_state",
                    "countdown_timer"],
    },
    "viewport": {
        "description": "Split-screen layout: viewport count + split mode each frame.",
        "modules": ["frame"],
        "stages":  ["frame_begin"],
        "fields":  ["viewport_count", "split_mode"],
    },
}


# ---------------------------------------------------------------------------
# Resolution
# ---------------------------------------------------------------------------

def list_profiles() -> str:
    """Return a human-readable catalog of all registered profiles."""
    name_w = max(len(n) for n in PROFILES)
    lines = [f"{'profile':<{name_w}}  modules / stages / fields"]
    lines.append("-" * (name_w + 2 + 60))
    for name, prof in PROFILES.items():
        modules = ",".join(prof["modules"])
        stages  = ",".join(prof["stages"])
        fields  = ",".join(prof.get("fields", [])) or "(all)"
        lines.append(f"{name:<{name_w}}  modules={modules}")
        lines.append(f"{'':<{name_w}}  stages={stages}")
        lines.append(f"{'':<{name_w}}  fields={fields}")
        lines.append(f"{'':<{name_w}}  -> {prof['description']}")
    return "\n".join(lines)


def resolve(names: Sequence[str]) -> Dict[str, object]:
    """Resolve one or more profile names into a single comparator slice.

    Returns a dict with keys:
      ``profiles``  list[str]  — the input names
      ``modules``   str        — comma-joined module list (deduped, ordered)
      ``stages``    str        — comma-joined stage  list (deduped, ordered)
      ``fields``    str        — comma-joined field  allowlist; empty = all
      ``float_tol`` float|None — tightest explicit value, or None

    When multiple profiles are passed, modules / stages / fields union
    (first occurrence wins); float_tol picks the tightest.
    """
    if not names:
        names = ["core"]

    unknown = [n for n in names if n not in PROFILES]
    if unknown:
        available = ", ".join(PROFILES.keys())
        raise ValueError(
            f"unknown profile(s): {', '.join(unknown)}. "
            f"Available: {available}"
        )

    modules: List[str] = []
    stages:  List[str] = []
    fields:  List[str] = []
    float_tol: Optional[float] = None

    for name in names:
        prof = PROFILES[name]
        for m in prof["modules"]:
            if m not in modules:
                modules.append(m)
        for s in prof["stages"]:
            if s not in stages:
                stages.append(s)
        for f in prof.get("fields", []):
            if f not in fields:
                fields.append(f)
        if "float_tol" in prof and prof["float_tol"] is not None:
            ftol = float(prof["float_tol"])
            float_tol = ftol if float_tol is None else min(float_tol, ftol)

    return {
        "profiles":  list(names),
        "modules":   ",".join(modules),
        "stages":    ",".join(stages),
        "fields":    ",".join(fields),
        "float_tol": float_tol,
    }


def parse_profile_arg(arg: Optional[str]) -> List[str]:
    """Parse a comma-separated profile argument into a list of names."""
    if not arg:
        return ["core"]
    return [p.strip() for p in arg.split(",") if p.strip()]


# ---------------------------------------------------------------------------
# Per-module key schema
# ---------------------------------------------------------------------------
#
# Each per-module CSV uses its own key set for join purposes. The orchestrator
# reads MODULE_KEYS[mod] to pass --key-fields to compare_race_trace.py. Keep
# this in sync with td5_trace.c's emitter prefixes.

MODULE_KEYS: Dict[str, List[str]] = {
    "frame":    ["frame", "sim_tick", "stage"],
    "pose":     ["frame", "sim_tick", "stage", "slot"],
    "motion":   ["frame", "sim_tick", "stage", "slot"],
    "track":    ["frame", "sim_tick", "stage", "slot"],
    "controls": ["frame", "sim_tick", "stage", "slot"],
    "progress": ["frame", "sim_tick", "stage", "slot"],
    "view":     ["frame", "sim_tick", "stage", "view_index"],
    "calls":    ["sim_tick", "fn_name", "call_idx"],
}


# Non-key columns per module — used by the orchestrator to intersect a
# union ``--fields`` list against each module's narrow schema before
# invoking the comparator. Source-of-truth: td5_trace.c module headers.
MODULE_COLUMNS: Dict[str, List[str]] = {
    "frame": [
        "game_state", "paused", "pause_menu", "fade_state", "countdown_timer",
        "sim_accum", "sim_budget", "frame_dt", "instant_fps",
        "viewport_count", "split_mode", "ticks_this_frame",
    ],
    "pose": [
        "world_x", "world_y", "world_z",
        "ang_roll", "ang_yaw", "ang_pitch",
        "disp_roll", "disp_yaw", "disp_pitch",
    ],
    "motion": [
        "vel_x", "vel_y", "vel_z",
        "long_speed", "lat_speed",
        "front_slip", "rear_slip",
    ],
    "track": [
        "span_raw", "span_norm", "span_accum", "span_high",
        "track_contact", "wheel_mask",
    ],
    "controls": [
        "slot_state", "steering", "engine", "gear", "vehicle_mode",
    ],
    "progress": [
        "race_pos", "finish_time", "accum_distance", "pending_finish",
        "metric_checkpoint", "metric_mask", "metric_norm_span",
        "metric_timer", "metric_display_pos",
        "metric_speed_bonus", "metric_top_speed",
    ],
    "view": [
        "actor_slot", "cam_world_x", "cam_world_y", "cam_world_z",
    ],
    "calls": [
        "n_args", "arg_0", "arg_1", "arg_2", "arg_3",
        "arg_4", "arg_5", "arg_6", "arg_7", "has_ret", "ret",
    ],
}


# ---------------------------------------------------------------------------
# CLI for ad-hoc inspection: ``python -m tools.diff_profiles list``
# ---------------------------------------------------------------------------

def _main() -> int:
    import sys
    if len(sys.argv) >= 2 and sys.argv[1] == "list":
        print(list_profiles())
        return 0
    if len(sys.argv) >= 2 and sys.argv[1] == "resolve":
        names = parse_profile_arg(",".join(sys.argv[2:]))
        out = resolve(names)
        for k, v in out.items():
            print(f"{k}: {v}")
        return 0
    print("usage: python tools/diff_profiles.py [list | resolve <name>[,<name>]]")
    return 1


if __name__ == "__main__":
    raise SystemExit(_main())
