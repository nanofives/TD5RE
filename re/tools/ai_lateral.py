#!/usr/bin/env python3
"""ai_lateral.py — where does the traced actor drive laterally vs the road?

Joins pose(post_physics) world_x/z with track(post_track) span_raw for the slot,
projects the position onto each span's lateral axis -> lane index (0..lanes),
and reports the distribution. Road = center lanes [sh, lanes-sh); grass shoulders
= outer sh each side (sh = round(lanes*0.25)). Tells us if the AI hugs the left
grass (lane<sh), right grass (lane>lanes-sh), or stays on the road.

Usage: python re/tools/ai_lateral.py <log_dir> <STRIP.DAT> [slot]
"""
import csv, os, sys, struct, math, statistics

LOG = sys.argv[1] if len(sys.argv) > 1 else "log"
STRIP = sys.argv[2] if len(sys.argv) > 2 else "re/assets/levels/level007/STRIP.DAT"
SLOT = int(sys.argv[3]) if len(sys.argv) > 3 else 1
SHOULDER_FRAC = 0.25

d = open(STRIP, "rb").read()
span_off, ring, vtx_off, vtx_cnt, total = struct.unpack_from("<5I", d, 0)
def span(i):
    o = span_off + i*24
    lv, rv = struct.unpack_from("<HH", d, o+4); lanes = d[o+3] & 0xF
    ox, oy, oz = struct.unpack_from("<iii", d, o+12)
    return lv, rv, lanes, ox, oz
def vtx(vi):
    x, _, z = struct.unpack_from("<hhh", d, vtx_off + vi*6); return x, z

def load(mod, stage):
    p = os.path.join(LOG, f"race_trace_{mod}.csv")
    out = {}
    if not os.path.exists(p): return out
    for r in csv.DictReader(open(p)):
        if r.get("stage") == stage and int(r.get("slot", 0)) == SLOT:
            out[int(r["sim_tick"])] = r
    return out
pose = load("pose", "post_physics")
track = load("track", "post_track")
ticks = sorted(set(pose) & set(track))
print(f"slot {SLOT}: {len(ticks)} joined ticks")

lanes_hist = {}
lane_vals = []
on_grass = 0; total_n = 0
for t in ticks:
    sp = int(track[t]["span_raw"])
    if sp < 0 or sp >= ring: continue
    lv, rv, lanes, ox, oz = span(sp)
    if lanes < 1: continue
    wx = int(pose[t]["world_x"]) >> 8; wz = int(pose[t]["world_z"]) >> 8
    l0x, l0z = vtx(lv); lLx, lLz = vtx(lv + lanes)        # near edge: lane 0 .. lane `lanes`
    ax = lLx - l0x; az = lLz - l0z
    den = ax*ax + az*az
    if den == 0: continue
    rx = wx - ox - l0x; rz = wz - oz - l0z
    frac = (rx*ax + rz*az) / den          # 0..1 across width
    lane_pos = frac * lanes
    lane_vals.append(lane_pos)
    sh = 0 if lanes <= 2 else max(1, round(lanes * SHOULDER_FRAC))
    total_n += 1
    if lane_pos < sh or lane_pos > (lanes - sh): on_grass += 1
    b = round(lane_pos)
    lanes_hist[b] = lanes_hist.get(b, 0) + 1

if lane_vals:
    print(f"lane position (0=left rail .. lanes=right rail): mean={statistics.mean(lane_vals):.2f} "
          f"median={statistics.median(lane_vals):.2f} min={min(lane_vals):.2f} max={max(lane_vals):.2f}")
    print(f"on grass (outer 25% each side): {on_grass}/{total_n} = {100*on_grass/total_n:.0f}%")
    print("lane occupancy histogram (rounded lane index):", dict(sorted(lanes_hist.items())))
    print("NOTE: most spans are 8 lanes -> road=lanes 2..6, grass=0,1 (left) & 6,7 (right)")
