#!/usr/bin/env python3
"""
td5_car_physics_ref.py -- physics-authoring reference for TD5RE custom cars.

Turns "what number do I put here?" into an answerable question by scanning the
stock fleet's carparam.json files and pairing each live field with:
  * a plain-language EFFECT hint (what it does + which way each direction pushes),
  * the fleet min / median / max with the exemplar cars at the extremes,
  * every stock car's value (for the Studio's per-field bar + compare panel),
  * archetype PRESETS (median of a sensible subset: Exotic / Muscle / Lightweight
    / Heavy / Balanced) you can drop in as a baseline and then tune.

Consumed by td5_car_studio.py (/api/reference) and td5_car_import.py (`stats`).
Pure stdlib + the carparam.json files; no game build needed.
"""
import glob
import json
import os
import statistics

# ---------------------------------------------------------------------------
# EFFECT HINTS — curated from re/TD5_MODDING_GUIDE.md §2.2 + the carparam field
# descriptions. `more` = what INCREASING the value does. Wheel positions are
# placed visually in the Studio (gizmos), so they carry no fleet bar / preset.
# ---------------------------------------------------------------------------
EFFECT_HINTS = {
    "top_speed_limit":         {"effect": "Hard top-speed cap.",                      "more": "higher top speed"},
    "drive_torque_multiplier": {"effect": "Base engine torque → acceleration.",       "more": "quicker acceleration"},
    "max_rpm":                 {"effect": "Redline / torque cutoff.",                 "more": "revs higher before cutoff"},
    "drag_coefficient":        {"effect": "Aerodynamic drag.",                        "more": "more drag — lower top speed"},
    "brake_force":             {"effect": "Braking deceleration.",                    "more": "stronger brakes, shorter stops"},
    "engine_brake_force":      {"effect": "Engine braking off-throttle.",             "more": "slows harder when you lift"},
    "lateral_slip_stiffness":  {"effect": "Cornering grip / slip resistance.",        "more": "more grip — sticks in turns"},
    "handbrake_grip_modifier": {"effect": "Rear grip while handbraking.",             "more": "more rear grip — harder to slide"},
    "collision_mass":          {"effect": "Vehicle mass for impacts.",               "more": "heavier — shrugs off hits"},
    "vehicle_inertia":         {"effect": "Yaw moment of inertia.",                  "more": "slower to rotate — stable, less darty"},
    "half_wheelbase":          {"effect": "CG-to-axle distance (length).",           "more": "longer — more stable, wider turns"},
    "front_weight_dist":       {"effect": "Front axle weight share.",                "more": "more front bias — pushes/understeers"},
    "rear_weight_dist":        {"effect": "Rear axle weight share.",                 "more": "more rear bias — traction/oversteer"},
    "suspension_spring_rate":  {"effect": "Spring stiffness.",                       "more": "stiffer — responsive but skittish"},
    "suspension_damping":      {"effect": "Shock damping.",                          "more": "firmer — settles faster, less bounce"},
    "suspension_travel_limit": {"effect": "Max suspension travel.",                 "more": "more travel — soft/soft-roader"},
    "suspension_feedback":     {"effect": "Cross-axis suspension coupling. (advanced)", "more": "more roll/pitch coupling"},
    "suspension_response_factor": {"effect": "Suspension velocity feedback. (advanced)", "more": "snappier suspension response"},
    "suspension_height_ref":   {"effect": "Ride-height reference for wheel contact.", "more": "sits higher off the ground"},
    "envelope_reference_y":    {"effect": "Collision envelope vertical reference. (advanced)", "more": "raises the collision body"},
    "damping_low_speed":       {"effect": "Velocity decay at low speed.",           "more": "more low-speed drag"},
    "damping_high_speed":      {"effect": "Velocity decay at high speed.",          "more": "more high-speed drag"},
    "drivetrain_type":         {"effect": "Driven wheels: 1=RWD, 2=FWD, 3=AWD.",    "more": "(category, not a slider)"},
    # arrays (advanced; shown as hints only, no single-number bar)
    "torque_curve":            {"effect": "Engine torque at 16 RPM points.",        "more": "fatter curve = more power across the band"},
    "gear_ratio_table":        {"effect": "R/N/1st–6th gear ratios.",               "more": "taller gears = more top speed, less punch"},
    "upshift_rpm_table":       {"effect": "Auto-box upshift RPM per gear.",         "more": "shifts up later (revs out further)"},
    "downshift_rpm_table":     {"effect": "Auto-box downshift RPM per gear.",       "more": "holds lower gears longer"},
    "wheel_pos_FL": {"effect": "Front-left wheel mount (placed via the 3D gizmos).", "more": ""},
    "wheel_pos_FR": {"effect": "Front-right wheel mount (placed via the 3D gizmos).", "more": ""},
    "wheel_pos_RL": {"effect": "Rear-left wheel mount (placed via the 3D gizmos).",  "more": ""},
    "wheel_pos_RR": {"effect": "Rear-right wheel mount (placed via the 3D gizmos).", "more": ""},
}

# Fields a preset should set (everything live EXCEPT the mesh-specific wheels).
_WHEEL_FIELDS = {"wheel_pos_FL", "wheel_pos_FR", "wheel_pos_RL", "wheel_pos_RR"}


def car_display_name(cars_dir, code):
    nfo = os.path.join(cars_dir, code, "config.nfo")
    if os.path.isfile(nfo):
        try:
            first = open(nfo, encoding="utf-8", errors="ignore").readline().strip()
            if first:
                return first.replace("_", " ")
        except Exception:
            pass
    return code


def _load_fleet(cars_dir):
    """Return {code: carparam_dict} for every stock (non-custom) car."""
    fleet = {}
    for cp in sorted(glob.glob(os.path.join(cars_dir, "*", "carparam.json"))):
        code = os.path.basename(os.path.dirname(cp))
        if code.startswith("custom_"):
            continue
        try:
            fleet[code] = json.load(open(cp, encoding="utf-8"))
        except Exception:
            pass
    return fleet


# Archetype membership predicates, evaluated per car against fleet percentiles.
# Each is (name, predicate(features) -> bool). A car may inform several presets;
# "Balanced" is always the straight fleet median. Transparent on purpose.
def _percentile(sorted_vals, q):
    if not sorted_vals:
        return 0
    i = max(0, min(len(sorted_vals) - 1, int(round(q * (len(sorted_vals) - 1)))))
    return sorted_vals[i]


def build_reference(cars_dir):
    fleet = _load_fleet(cars_dir)
    if not fleet:
        return {"fields": {}, "cars": [], "presets": {}, "preset_members": {}, "error": "no cars found"}

    # Collect live fields across the fleet.
    scalar_vals = {}   # field -> {code: number}
    array_vals = {}    # field -> {code: [..]}
    meta = {}          # field -> {type, description}
    for code, d in fleet.items():
        for k, v in d.items():
            if not (isinstance(v, dict) and v.get("kind") == "live"):
                continue
            meta.setdefault(k, {"type": v.get("type", ""), "description": v.get("description", "")})
            val = v.get("value")
            if isinstance(val, (int, float)):
                scalar_vals.setdefault(k, {})[code] = val
            elif isinstance(val, list):
                array_vals.setdefault(k, {})[code] = val

    fields = {}
    for k, percar in scalar_vals.items():
        nums = sorted(percar.values())
        lo_code = min(percar, key=percar.get)
        hi_code = max(percar, key=percar.get)
        fields[k] = {
            "kind": "scalar",
            "min": nums[0], "median": statistics.median(nums), "max": nums[-1],
            "min_car": {"code": lo_code, "name": car_display_name(cars_dir, lo_code), "value": percar[lo_code]},
            "max_car": {"code": hi_code, "name": car_display_name(cars_dir, hi_code), "value": percar[hi_code]},
            "values": percar,
            "hint": EFFECT_HINTS.get(k, {"effect": "", "more": ""}),
            "type": meta[k]["type"], "description": meta[k]["description"],
        }
    for k, percar in array_vals.items():
        fields[k] = {
            "kind": "array", "values": percar,
            "hint": EFFECT_HINTS.get(k, {"effect": "", "more": ""}),
            "type": meta[k]["type"], "description": meta[k]["description"],
        }

    # --- Archetype presets -------------------------------------------------
    def pcar(field, code):
        return scalar_vals.get(field, {}).get(code)

    # Classify on fields that actually vary across the fleet. collision_mass is a
    # near-constant default (mostly 16) so it can't separate cars; vehicle_inertia
    # is the clean agility/heft axis. Use STRICT vs-median splits for Agile/Heavy
    # so the median cluster doesn't fall into both.
    tops = sorted(scalar_vals.get("top_speed_limit", {}).values())
    torqs = sorted(scalar_vals.get("drive_torque_multiplier", {}).values())
    grips = sorted(scalar_vals.get("lateral_slip_stiffness", {}).values())
    inertias = sorted(scalar_vals.get("vehicle_inertia", {}).values())
    top_66 = _percentile(tops, 0.66)
    torq_66 = _percentile(torqs, 0.66)
    grip_md = statistics.median(grips) if grips else 0
    inertia_md = statistics.median(inertias) if inertias else 0

    archetypes = {
        "Exotic": lambda c: (pcar("top_speed_limit", c) or 0) >= top_66 and (pcar("lateral_slip_stiffness", c) or 0) >= grip_md,
        "Muscle": lambda c: pcar("drivetrain_type", c) == 1 and (pcar("drive_torque_multiplier", c) or 0) >= torq_66,
        "Agile":  lambda c: (pcar("vehicle_inertia", c) if pcar("vehicle_inertia", c) is not None else inertia_md) < inertia_md,
        "Heavy":  lambda c: (pcar("vehicle_inertia", c) or inertia_md) > inertia_md,
    }
    all_live = [f for f in fields if f not in _WHEEL_FIELDS]
    presets, members = {}, {}

    def median_over(codes):
        out = {}
        for f in all_live:
            if fields[f]["kind"] == "scalar":
                vals = [scalar_vals[f][c] for c in codes if c in scalar_vals.get(f, {})]
                if vals:
                    out[f] = int(round(statistics.median(vals)))
            else:  # array: element-wise median
                lists = [array_vals[f][c] for c in codes if c in array_vals.get(f, {})]
                if lists:
                    n = min(len(x) for x in lists)
                    out[f] = [int(round(statistics.median([x[i] for x in lists]))) for i in range(n)]
        return out

    for name, pred in archetypes.items():
        codes = [c for c in fleet if pred(c)]
        members[name] = sorted(codes)
        presets[name] = median_over(codes) if codes else median_over(list(fleet))
    members["Balanced"] = sorted(fleet)
    presets["Balanced"] = median_over(list(fleet))

    return {
        "fields": fields,
        "cars": [{"code": c, "name": car_display_name(cars_dir, c)} for c in sorted(fleet)],
        "presets": presets,
        "preset_members": {k: [{"code": c, "name": car_display_name(cars_dir, c)} for c in v]
                           for k, v in members.items()},
    }


# ---------------------------------------------------------------------------
# Text / markdown rendering (the `stats` CLI)
# ---------------------------------------------------------------------------
def render_table(ref, markdown=False):
    rows = []
    order = [k for k in EFFECT_HINTS if k in ref["fields"] and ref["fields"][k]["kind"] == "scalar"]
    if markdown:
        out = ["| Field | Effect (↑ = more) | Fleet min | median | max | low / high car |",
               "|---|---|--:|--:|--:|---|"]
        for k in order:
            f = ref["fields"][k]
            out.append(f"| `{k}` | {f['hint']['effect']} — ↑ {f['hint']['more']} | "
                       f"{f['min']} | {f['median']:.0f} | {f['max']} | "
                       f"{f['min_car']['name']} / {f['max_car']['name']} |")
        return "\n".join(out)
    for k in order:
        f = ref["fields"][k]
        rows.append(f"{k:26s} {f['min']:>7} {f['median']:>7.0f} {f['max']:>7}   "
                    f"(+) {f['hint']['more']}  [{f['min_car']['name']} -> {f['max_car']['name']}]")
    head = f"{'field':26s} {'min':>7} {'median':>7} {'max':>7}   effect of increasing"
    return head + "\n" + "\n".join(rows)


if __name__ == "__main__":
    import sys
    try:
        sys.stdout.reconfigure(encoding="utf-8")   # markdown ↑ etc. on Win consoles
    except Exception:
        pass
    cars_dir = sys.argv[1] if len(sys.argv) > 1 else "re/assets/cars"
    ref = build_reference(cars_dir)
    print(render_table(ref, markdown=("--markdown" in sys.argv)))
    print(f"\npresets: {', '.join(f'{k}({len(v)})' for k, v in ref['preset_members'].items())}")
