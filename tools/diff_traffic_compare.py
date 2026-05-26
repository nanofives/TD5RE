#!/usr/bin/env python3
"""diff_traffic_compare.py — Analyze paired traffic captures from
tools/frida_traffic_compare.js.

Inputs:
  --port  <port_csv>   (required)   live port capture
  --orig  <orig_csv>   (optional)   live orig capture, schema-matched
  --orig-pool8 <csv>   (optional)   historical orig pool8_traffic CSV
                                    (different schema — auto-mapped)

Output: textual summary to stdout. Sections:
  1. PORT SPAWN — per-slot spawn position, direction (yaw), lane, span
  2. PORT MOTION — per-slot speed/yaw/lane progression over time
  3. ORIG REFERENCE — what orig spawn SHOULD write (from Ghidra decomp)
  4. ORIG POOL8 — per-tick physics envelope (lin_vel range, ang_vel range,
                   long_spd range, yaw_accum range) for sanity comparison
  5. DELTAS — per-slot, signed deltas (port vs orig) where comparable

Notes:
  * All position fields are in 24.8 fixed-point (divide by 256 for world).
  * euler_yaw is angle * 0x100 with direction baked in (+0x80000 = reverse).
  * sub_lane is uint8 lane number.
"""

import csv, argparse, sys
from collections import defaultdict
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent


def load_compare_csv(path):
    """Load CSV emitted by frida_traffic_compare.js."""
    rows = []
    with open(path, "r", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            try:
                r["sim_tick"] = int(r["sim_tick"])
                r["slot"]     = int(r["slot"])
                for k in ("span_raw","span_norm","span_acc","sub_lane",
                          "pos_x","pos_y","pos_z",
                          "lin_x","lin_y","lin_z",
                          "euler_yaw","ang_yaw",
                          "long_spd","steer","enc_steer"):
                    r[k] = int(r[k])
                rows.append(r)
            except (ValueError, KeyError):
                continue
    return rows


def load_pool8_csv(path):
    """Load tools/pool8_traffic.csv (orig historical) — schema:
    sim_tick,paused,slot,which_fn,actor_addr,in_*,arg_*,out_*

    We use the IN (entry) snapshot since it's the actor state at that tick.
    Filter to which_fn=friction (one row per slot per tick — susp is redundant).
    """
    rows = []
    with open(path, "r", newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r.get("which_fn") != "friction":
                continue
            try:
                rows.append({
                    "sim_tick": int(r["sim_tick"]),
                    "slot":     int(r["slot"]),
                    "pos_x":    int(r["in_world_pos_x"]),
                    "pos_z":    int(r["in_world_pos_z"]),
                    "lin_x":    int(r["in_lin_vel_x"]),
                    "lin_z":    int(r["in_lin_vel_z"]),
                    "ang_yaw":  int(r["in_ang_vel_yaw"]),
                    "euler_yaw": int(r["in_yaw_accum"]),
                    "long_spd": int(r["in_long_speed"]),
                    "steer":    int(r["in_steering_cmd"]),
                    "enc_steer": int(r["in_encounter_steer"]),
                })
            except (ValueError, KeyError):
                continue
    return rows


def fp_to_world(x):
    return x / 256.0


def yaw_to_deg(y):
    # euler_yaw = angle12 * 256, full circle 0x1000 * 256 = 0x100000
    # mod 0x100000 then scale to degrees
    a = (y >> 8) & 0xFFF
    return a * 360.0 / 4096.0


def yaw_direction(y):
    """Return 'fwd' or 'rev' based on whether the +0x80000 (half-circle) bit
    is set in the spawn-baked heading."""
    return "rev" if (y & 0x80000) else "fwd"


def fmt_pos(p):
    return f"{fp_to_world(p):,.1f}"


def stats(values):
    if not values: return (0, 0, 0, 0)
    return (min(values), max(values), sum(values)/len(values), len(values))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="Port traffic_compare CSV")
    ap.add_argument("--orig", help="Orig traffic_compare CSV (same schema)")
    ap.add_argument("--orig-pool8", help="Orig pool8_traffic.csv (different schema)")
    args = ap.parse_args()

    port_rows = load_compare_csv(args.port)
    print(f"[loaded] port rows: {len(port_rows)}")

    orig_rows = []
    if args.orig and Path(args.orig).exists():
        orig_rows = load_compare_csv(args.orig)
        print(f"[loaded] orig rows: {len(orig_rows)}")

    pool8 = []
    if args.orig_pool8 and Path(args.orig_pool8).exists():
        pool8 = load_pool8_csv(args.orig_pool8)
        print(f"[loaded] orig pool8 rows: {len(pool8)} (friction-only)")

    # ============================================================
    # 1. PORT SPAWN snapshot (first init_leave per slot)
    # ============================================================
    print()
    print("=" * 78)
    print("1. PORT SPAWN POSITIONS  (from init_leave at race start)")
    print("=" * 78)
    print(f"{'slot':>4} {'span':>5} {'lane':>4}  {'pos_x':>12} {'pos_z':>12} "
          f"{'yaw_deg':>8} {'dir':>3}  {'lin_vx':>8} {'lin_vz':>8}")
    spawn = {}
    for r in port_rows:
        if r["event"] == "init_leave" and r["slot"] not in spawn:
            spawn[r["slot"]] = r
    for slot in sorted(spawn):
        r = spawn[slot]
        print(f"{slot:>4} {r['span_raw']:>5} {r['sub_lane']:>4}  "
              f"{fmt_pos(r['pos_x']):>12} {fmt_pos(r['pos_z']):>12} "
              f"{yaw_to_deg(r['euler_yaw']):>8.1f} {yaw_direction(r['euler_yaw']):>3}  "
              f"{r['lin_x']:>8} {r['lin_z']:>8}")

    # ============================================================
    # 2. PORT MOTION (per-slot tick stats)
    # ============================================================
    print()
    print("=" * 78)
    print("2. PORT MOTION (per-slot from tick events — friction call per tick)")
    print("=" * 78)
    per_slot = defaultdict(list)
    for r in port_rows:
        if r["event"] == "tick":
            per_slot[r["slot"]].append(r)
    print(f"{'slot':>4} {'ticks':>6}  "
          f"{'lin_vx range':>22} {'lin_vz range':>22}  "
          f"{'long_spd range':>22} {'yaw_deg final':>14}")
    for slot in sorted(per_slot):
        rs = per_slot[slot]
        if not rs: continue
        vx_lo, vx_hi, _, _ = stats([r["lin_x"] for r in rs])
        vz_lo, vz_hi, _, _ = stats([r["lin_z"] for r in rs])
        ls_lo, ls_hi, _, _ = stats([r["long_spd"] for r in rs])
        final_yaw = rs[-1]["euler_yaw"]
        print(f"{slot:>4} {len(rs):>6}  "
              f"[{vx_lo:>10},{vx_hi:>10}] [{vz_lo:>10},{vz_hi:>10}]  "
              f"[{ls_lo:>10},{ls_hi:>10}] {yaw_to_deg(final_yaw):>14.1f}")

    # ============================================================
    # 3. PORT RECYCLE rate
    # ============================================================
    print()
    print("=" * 78)
    print("3. PORT RECYCLE EVENTS")
    print("=" * 78)
    rec_per_slot = defaultdict(int)
    pre_pos = {}
    post_pos = {}
    recycle_calls = 0
    last_block_pos = {}
    for r in port_rows:
        if r["event"] == "recycle_leave":
            # Every recycle_leave dumps all 6 slots — count one event per
            # 6 consecutive rows (use sim_tick boundary).
            slot = r["slot"]
            if slot == 6:
                recycle_calls += 1
            prev = last_block_pos.get(slot)
            cur = (r["pos_x"], r["pos_z"], r["span_raw"])
            if prev is not None and prev != cur:
                rec_per_slot[slot] += 1
            last_block_pos[slot] = cur
    print(f"recycle_leave function calls: {recycle_calls}")
    print(f"slots that actually moved on recycle (state delta between events):")
    for slot in sorted(rec_per_slot):
        print(f"  slot {slot}: {rec_per_slot[slot]} state changes")

    # ============================================================
    # 4. ORIG REFERENCE (from Ghidra decomp of 0x00435940)
    # ============================================================
    print()
    print("=" * 78)
    print("4. ORIG SPAWN REFERENCE  (Ghidra decomp 0x00435940)")
    print("=" * 78)
    print("""
At spawn (InitializeTrafficActorsFromQueue), orig writes per traffic slot:
  actor+0x80  track_span_raw       <- queue[0] (ushort)
  actor+0x8C  track_sub_lane_index <- queue[3] (byte; or queue[3] - (segment&0xF) for REMAP)
  actor+0x1F4 euler_accum.yaw      <- (AngleFromVector12(...)+0x800)*0x100
                                       + (0x80000 if (queue[2] & 1))  ; direction flip
  actor+0x1FC world_pos.x          <- InitActorTrackSegmentPlacement output
  actor+0x204 world_pos.z          <- "
  actor+0x200 world_pos.y          <- (set from track vertex Y; usually >0)

Lin/ang velocities are zeroed by ResetVehicleActorState; long_spd=0; steer=0.

Direction bit interpretation:
  queue[2] & 1 == 0  -> spawn driving "forward" along track (chase direction)
  queue[2] & 1 == 1  -> spawn driving "reverse" (oncoming, opposite of player)
""")

    # ============================================================
    # 5. ORIG POOL8 envelope (per-tick physics ranges)
    # ============================================================
    if pool8:
        print("=" * 78)
        print("5. ORIG POOL8 PER-TICK ENVELOPE  (historical reference)")
        print("=" * 78)
        per_slot_o = defaultdict(list)
        for r in pool8:
            per_slot_o[r["slot"]].append(r)
        print(f"{'slot':>4} {'ticks':>6}  "
              f"{'lin_vx range':>22} {'lin_vz range':>22}  "
              f"{'long_spd range':>22} {'ang_yaw range':>14}")
        for slot in sorted(per_slot_o):
            rs = per_slot_o[slot]
            vx_lo, vx_hi, _, _ = stats([r["lin_x"] for r in rs])
            vz_lo, vz_hi, _, _ = stats([r["lin_z"] for r in rs])
            ls_lo, ls_hi, _, _ = stats([r["long_spd"] for r in rs])
            ay_lo, ay_hi, _, _ = stats([r["ang_yaw"] for r in rs])
            print(f"{slot:>4} {len(rs):>6}  "
                  f"[{vx_lo:>10},{vx_hi:>10}] [{vz_lo:>10},{vz_hi:>10}]  "
                  f"[{ls_lo:>10},{ls_hi:>10}] [{ay_lo:>5},{ay_hi:>5}]")

    # ============================================================
    # 6. DELTAS — only if orig same-schema CSV is present
    # ============================================================
    if orig_rows:
        print()
        print("=" * 78)
        print("6. PORT vs ORIG DELTAS (same-schema rows; matched by tick+slot)")
        print("=" * 78)
        port_idx = {(r["sim_tick"], r["slot"]): r for r in port_rows
                    if r["event"] == "tick"}
        orig_idx = {(r["sim_tick"], r["slot"]): r for r in orig_rows
                    if r["event"] == "tick"}
        keys = sorted(set(port_idx) & set(orig_idx))
        print(f"matched ticks: {len(keys)}")
        if keys:
            field_deltas = defaultdict(list)
            for k in keys:
                p, o = port_idx[k], orig_idx[k]
                for fld in ("pos_x","pos_z","lin_x","lin_z","euler_yaw",
                            "long_spd","span_raw","sub_lane"):
                    field_deltas[fld].append(p[fld] - o[fld])
            for fld, ds in sorted(field_deltas.items()):
                if not ds: continue
                lo, hi, avg, _ = stats(ds)
                print(f"  {fld:<12} delta range [{lo:>12},{hi:>12}] mean={avg:>10.1f}")

    # ============================================================
    # 7. PORT POOL8 cross-check (port vs orig pool8 envelope)
    # ============================================================
    if pool8 and per_slot:
        print()
        print("=" * 78)
        print("7. PORT vs ORIG-POOL8 ENVELOPE COMPARISON  (per-slot)")
        print("=" * 78)
        per_slot_o = defaultdict(list)
        for r in pool8:
            per_slot_o[r["slot"]].append(r)
        for slot in sorted(set(per_slot) & set(per_slot_o)):
            p_rs = per_slot[slot]
            o_rs = per_slot_o[slot]
            for fld_label, p_f, o_f in [
                ("long_spd", "long_spd", "long_spd"),
                ("lin_vx",   "lin_x",    "lin_x"),
                ("lin_vz",   "lin_z",    "lin_z"),
                ("ang_yaw",  "ang_yaw",  "ang_yaw"),
            ]:
                p_lo, p_hi, p_avg, _ = stats([r[p_f] for r in p_rs])
                o_lo, o_hi, o_avg, _ = stats([r[o_f] for r in o_rs])
                diff = p_avg - o_avg
                pct  = (diff / max(abs(o_avg), 1)) * 100.0
                print(f"  slot {slot}  {fld_label:<10}  "
                      f"port_avg={p_avg:>10.1f}  orig_avg={o_avg:>10.1f}  "
                      f"diff={diff:>+10.1f}  ({pct:>+7.1f}%)")
            print()


if __name__ == "__main__":
    main()
