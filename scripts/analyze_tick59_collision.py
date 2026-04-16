"""
Tick-59 collision investigation script.

Examines ticks 55-65 in both original and port traces,
checks AI proximity to player at tick 58-59, and
reports differences in actor positions and player state.
"""

import csv
import math
import sys
from collections import defaultdict

PORT_TRACE    = "log/race_trace.csv"
ORIGINAL_TRACE = "log/race_trace_original.csv"

TICK_LO, TICK_HI = 55, 65
PROXIMITY_TICKS = (58, 59)
PROX_THRESHOLD = 500   # world units (pre-fp-shift; ~1.95 m at 1/256)

def load_actor_rows(path):
    """Return dict: tick -> actor_id -> row_dict (last frame_end or frame_begin row per tick)."""
    data = defaultdict(dict)  # tick -> id -> row
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "actor":
                continue
            tick = int(row["sim_tick"])
            stage = row["stage"]
            actor_id = int(row["id"])
            # Prefer frame_end rows (post-physics) over frame_begin
            existing = data[tick].get(actor_id)
            if existing is None:
                data[tick][actor_id] = row
            elif stage == "frame_end" and existing["stage"] != "frame_end":
                data[tick][actor_id] = row
    return data

def fmt_row(row):
    wx  = int(row["world_x"])
    wz  = int(row["world_z"])
    vx  = int(row["vel_x"])
    vz  = int(row["vel_z"])
    tc  = row.get("track_contact","?")
    wm  = row.get("wheel_mask","?")
    stage = row["stage"]
    return (f"  wx={wx:>12}  wz={wz:>12}  "
            f"vel_x={vx:>6}  vel_z={vz:>6}  "
            f"track_contact={tc}  wheel_mask={wm}  [{stage}]")

def distance_xz(rowA, rowB):
    dx = int(rowA["world_x"]) - int(rowB["world_x"])
    dz = int(rowA["world_z"]) - int(rowB["world_z"])
    return math.sqrt(dx*dx + dz*dz)

def print_section(title):
    print()
    print("=" * 72)
    print(f"  {title}")
    print("=" * 72)

def analyze(label, data):
    print_section(f"{label}: ticks {TICK_LO}-{TICK_HI}, all actors")
    for tick in range(TICK_LO, TICK_HI + 1):
        tick_data = data.get(tick, {})
        if not tick_data:
            print(f"  tick {tick:3d}: NO DATA")
            continue
        print(f"\n  tick {tick:3d}:")
        for aid in sorted(tick_data.keys()):
            row = tick_data[aid]
            print(f"    actor {aid}:" + fmt_row(row))

def proximity_check(label, data):
    print_section(f"{label}: AI-to-player proximity at ticks {PROXIMITY_TICKS}")
    for tick in PROXIMITY_TICKS:
        tick_data = data.get(tick, {})
        player = tick_data.get(0)
        if player is None:
            print(f"  tick {tick:3d}: player (id=0) not found")
            continue
        print(f"\n  tick {tick:3d}  player pos: wx={player['world_x']}  wz={player['world_z']}")
        for aid, row in sorted(tick_data.items()):
            if aid == 0:
                continue
            dist = distance_xz(row, player)
            flag = " <<< CLOSE" if dist < PROX_THRESHOLD else ""
            print(f"    actor {aid}: dist={dist:8.1f}{flag}  "
                  f"wx={row['world_x']}  wz={row['world_z']}")

def compare_ai_positions(orig_data, port_data):
    print_section("AI position delta: original vs port, ticks 55-65")
    for tick in range(TICK_LO, TICK_HI + 1):
        orig_tick = orig_data.get(tick, {})
        port_tick = port_data.get(tick, {})
        # Compare every AI actor (id 1-5)
        diffs = []
        for aid in range(1, 6):
            o = orig_tick.get(aid)
            p = port_tick.get(aid)
            if o is None or p is None:
                diffs.append(f"    actor {aid}: MISSING (orig={'present' if o else 'absent'}, port={'present' if p else 'absent'})")
                continue
            dx = int(o["world_x"]) - int(p["world_x"])
            dz = int(o["world_z"]) - int(p["world_z"])
            dvx = int(o["vel_x"]) - int(p["vel_x"])
            dvz = int(o["vel_z"]) - int(p["vel_z"])
            dist = math.sqrt(dx*dx + dz*dz)
            if dist > 100 or abs(dvx) > 10 or abs(dvz) > 10:
                diffs.append(f"    actor {aid}: pos_delta=({dx},{dz}) dist={dist:.0f}  vel_delta=({dvx},{dvz})")
        if diffs:
            print(f"\n  tick {tick:3d}:")
            for d in diffs:
                print(d)
        else:
            print(f"  tick {tick:3d}: all AI actors within tolerance")

def player_state_around_tick59(label, data):
    print_section(f"{label}: player (id=0) track_contact & wheel_mask, ticks 57-62")
    for tick in range(57, 63):
        tick_data = data.get(tick, {})
        player = tick_data.get(0)
        if player is None:
            print(f"  tick {tick:3d}: NO DATA")
            continue
        vx  = int(player["vel_x"])
        vz  = int(player["vel_z"])
        tc  = player.get("track_contact","?")
        wm  = player.get("wheel_mask","?")
        wx  = int(player["world_x"])
        wz  = int(player["world_z"])
        print(f"  tick {tick:3d}: vel=({vx:>6},{vz:>6})  "
              f"track_contact={tc}  wheel_mask={wm}  "
              f"wx={wx}  wz={wz}  [{player['stage']}]")

def main():
    print("Loading traces...")
    orig = load_actor_rows(ORIGINAL_TRACE)
    port = load_actor_rows(PORT_TRACE)
    print(f"  Original: {sum(len(v) for v in orig.values())} actor rows across {len(orig)} ticks")
    print(f"  Port:     {sum(len(v) for v in port.values())} actor rows across {len(port)} ticks")

    # 1. All actors ticks 55-65 in original
    analyze("ORIGINAL", orig)

    # 2. All actors ticks 55-65 in port
    analyze("PORT", port)

    # 3. AI proximity in original at ticks 58-59
    proximity_check("ORIGINAL", orig)

    # 4. AI proximity in port at ticks 58-59
    proximity_check("PORT", port)

    # 5. Compare AI positions between traces
    compare_ai_positions(orig, port)

    # 6. Player state in original around tick 59
    player_state_around_tick59("ORIGINAL", orig)

    # 7. Player state in port around tick 59
    player_state_around_tick59("PORT", port)

    # 8. Velocity spike summary
    print_section("Velocity spike summary: original actor 0, ticks 57-62")
    for tick in range(57, 63):
        o = orig.get(tick, {}).get(0)
        p = port.get(tick, {}).get(0)
        ov = (int(o["vel_x"]), int(o["vel_z"])) if o else ("?","?")
        pv = (int(p["vel_x"]), int(p["vel_z"])) if p else ("?","?")
        orig_speed = math.sqrt(ov[0]**2 + ov[1]**2) if o else 0
        port_speed = math.sqrt(pv[0]**2 + pv[1]**2) if p else 0
        print(f"  tick {tick:3d}: orig vel=({str(ov[0]):>6},{str(ov[1]):>6}) speed={orig_speed:7.1f}  |  "
              f"port vel=({str(pv[0]):>6},{str(pv[1]):>6}) speed={port_speed:7.1f}")

def load_frame_rows(path):
    """Return dict: tick -> list of frame row dicts."""
    data = defaultdict(list)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "frame":
                continue
            tick = int(row["sim_tick"])
            data[tick].append(row)
    return data

def frame_state_around_tick59(label, path):
    print_section(f"{label}: frame rows ticks 56-62 (game_state, paused, countdown, sim_accum)")
    data = load_frame_rows(path)
    for tick in range(56, 63):
        rows = data.get(tick, [])
        for row in rows:
            print(f"  tick {tick:3d} [{row['stage']:15s}]  "
                  f"game_state={row['game_state']}  paused={row['paused']}  "
                  f"countdown={row['countdown_timer']}  sim_accum={row['sim_accum']}  "
                  f"sim_budget={row['sim_budget']}  ticks_this_frame={row['ticks_this_frame']}")

def post_progress_ticks_this_frame(label, path):
    print_section(f"{label}: post_progress ticks_this_frame ticks 55-65")
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "frame" or row["stage"] != "post_progress":
                continue
            t = int(row["sim_tick"])
            if 55 <= t <= 65:
                print(f"  tick {t:3d}: ticks_this_frame={row['ticks_this_frame']}  "
                      f"sim_accum={row['sim_accum']}  sim_budget={row['sim_budget']}")

def stages_present_per_tick(label, path):
    print_section(f"{label}: which stages appear at each tick 55-65")
    tick_stages = defaultdict(set)
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "frame":
                continue
            t = int(row["sim_tick"])
            if 55 <= t <= 65:
                tick_stages[t].add(row["stage"])
    for t in range(55, 66):
        stages = sorted(tick_stages.get(t, set()))
        print(f"  tick {t:3d}: {stages}")

def actor_row_stages(label, path, tick_lo, tick_hi, actor_id):
    """Print all actor rows for a given actor/tick range, grouped by tick+stage."""
    print_section(f"{label}: actor {actor_id} all stages, ticks {tick_lo}-{tick_hi}")
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row["kind"] != "actor":
                continue
            if int(row["id"]) != actor_id:
                continue
            t = int(row["sim_tick"])
            if tick_lo <= t <= tick_hi:
                vx = int(row["vel_x"])
                vz = int(row["vel_z"])
                if vx != 0 or vz != 0:
                    print(f"  tick {t:3d} [{row['stage']:15s}]  vel=({vx:>6},{vz:>6})  "
                          f"wx={row['world_x']}  wz={row['world_z']}")


if __name__ == "__main__":
    post_progress_ticks_this_frame("ORIGINAL", ORIGINAL_TRACE)
    post_progress_ticks_this_frame("PORT", PORT_TRACE)
    stages_present_per_tick("ORIGINAL", ORIGINAL_TRACE)
    stages_present_per_tick("PORT", PORT_TRACE)
    # Focus on non-zero velocity actor rows in original
    actor_row_stages("ORIGINAL (non-zero vel only)", ORIGINAL_TRACE, 55, 65, 0)
