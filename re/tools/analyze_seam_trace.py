#!/usr/bin/env python3
"""analyze_seam_trace.py — locate the TD6-track teleport / stuck / airborne in a
port-only race trace (slot 0).

Joins track (post_track) with pose (post_physics) on sim_tick.
Reports, with correct thresholds:
  - global max per-tick world dx/dy/dz (real teleport = >>1000 units/tick)
  - every RUN of wheel_mask==0x0F with length>=3 (the recovery trigger), w/ span+pos
  - the seam wrap (span_raw high->low) windows
  - "stuck" runs (|long_speed|<8000 for >=6 ticks) with span+pos

Usage: python re/tools/analyze_seam_trace.py [log_dir]
"""
import csv, os, sys

LOG = sys.argv[1] if len(sys.argv) > 1 else "log"

def load(mod, stage, slot=0):
    path = os.path.join(LOG, f"race_trace_{mod}.csv")
    rows = {}
    if not os.path.exists(path):
        return rows
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if r.get("stage") != stage or int(r.get("slot", 0)) != slot:
                continue
            rows[int(r["sim_tick"])] = r
    return rows

track = load("track", "post_track")
pose  = load("pose",  "post_physics")
mot   = load("motion","post_physics")
ticks = sorted(set(track) & set(pose))

def gi(d, t, k, default=0):
    r = d.get(t)
    if not r or r.get(k) in (None, ""): return default
    try: return int(r[k])
    except ValueError: return default

rec = []
prev = None
for t in ticks:
    r = dict(t=t,
        wx=gi(pose,t,"world_x"), wy=gi(pose,t,"world_y"), wz=gi(pose,t,"world_z"),
        sr=gi(track,t,"span_raw"), sn=gi(track,t,"span_norm"),
        sa=gi(track,t,"span_accum"), sh=gi(track,t,"span_high"),
        tc=gi(track,t,"track_contact"), wm=gi(track,t,"wheel_mask"),
        ls=gi(mot,t,"long_speed"))
    r["dx"]=r["wx"]-prev["wx"] if prev else 0
    r["dy"]=r["wy"]-prev["wy"] if prev else 0
    r["dz"]=r["wz"]-prev["wz"] if prev else 0
    r["dsr"]=r["sr"]-prev["sr"] if prev else 0
    rec.append(r); prev=r

print(f"ticks={len(rec)}  span_raw[min={min(r['sr'] for r in rec)},max={max(r['sr'] for r in rec)}]")

# 1) global extremes (raw fp -> >>8 world units)
def topn(key, n=5):
    return sorted(rec, key=lambda r: abs(r[key]), reverse=True)[:n]
print("\n=== largest per-tick position deltas (world units) ===")
for key in ("dx","dy","dz"):
    print(f"  {key}: " + ", ".join(f"t{r['t']}={r[key]>>8 if r[key]>=0 else -((-r[key])>>8)}(span{r['sr']})" for r in topn(key)))

def dump(lo, hi, mark=None):
    print("    t    span dsr  s_nrm   world_x  world_y  world_z   dy  contact mask  lspd")
    for r in rec[max(0,lo):min(len(rec),hi)]:
        f=""
        if r["wm"]==0x0f: f+="A"
        if abs(r["dx"])>>8>1000 or abs(r["dz"])>>8>1000 or abs(r["dy"])>>8>1000: f+="T"
        dyv = r["dy"]>>8 if r["dy"]>=0 else -((-r["dy"])>>8)
        print(f"  {r['t']:5d} {r['sr']:5d} {r['dsr']:3d} {r['sn']:6d}  "
              f"{r['wx']>>8:8d} {r['wy']>>8:8d} {r['wz']>>8:8d} {dyv:4d}  "
              f"{r['tc']:6d} {r['wm']:#05x} {r['ls']:6d}  {f}")

# 2) sustained airborne runs (wm==0x0f, len>=3)
print("\n=== sustained airborne runs (wheel_mask==0x0F, >=3 ticks) ===")
i=0; runs=[]
while i < len(rec):
    if rec[i]["wm"]==0x0f:
        j=i
        while j<len(rec) and rec[j]["wm"]==0x0f: j+=1
        if j-i>=3: runs.append((i,j))
        i=j
    else: i+=1
print(f"  {len(runs)} runs")
for (i,j) in runs[:8]:
    print(f"\n  -- airborne run ticks {rec[i]['t']}..{rec[j-1]['t']} (len {j-i}) span={rec[i]['sr']} --")
    dump(i-4, j+4)

# 3) seam wraps (span drops from >300 to <60 within a few ticks)
print("\n=== seam wraps (span_raw high->low) ===")
seam=[]
for k in range(1,len(rec)):
    if rec[k-1]["sr"]>300 and rec[k]["sr"]<80:
        seam.append(k)
print(f"  {len(seam)} wraps at ticks {[rec[k]['t'] for k in seam]}")
for k in seam[:4]:
    print(f"\n  -- wrap @ tick {rec[k]['t']} ({rec[k-1]['sr']}->{rec[k]['sr']}) --")
    dump(k-6, k+10)

# 4) stuck runs (|lspd|<8000 for >=6 ticks)
print("\n=== stuck runs (|long_speed|<8000, >=6 ticks) ===")
i=0; stuck=[]
while i < len(rec):
    if abs(rec[i]["ls"])<8000:
        j=i
        while j<len(rec) and abs(rec[j]["ls"])<8000: j+=1
        if j-i>=6: stuck.append((i,j))
        i=j
    else: i+=1
print(f"  {len(stuck)} runs")
for (i,j) in stuck[:6]:
    print(f"  stuck ticks {rec[i]['t']}..{rec[j-1]['t']} (len {j-i}) span={rec[i]['sr']} "
          f"pos=({rec[i]['wx']>>8},{rec[i]['wy']>>8},{rec[i]['wz']>>8})")
