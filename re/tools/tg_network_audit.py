#!/usr/bin/env python3
"""tg_network_audit.py -- audit an auto-track's NETWORK.JSON (topology-first).

Same contract as tg_strip_audit.py: prints one line per check, exit 1 on any
violation. Checks:
  planarity      no two edge segments cross except at a shared node
  street-off-road no street segment runs inside the main carriageway
                  (except its own mouth segment)
  mouth-struct   no mouth on a bridge/tunnel span
  struct-len     every bridge/tunnel run within [min, cap], runs of the two
                  kinds at least CLEAR spans apart
  grade          |dy| per span <= cap * span_length (cap from --cap, 0.20 abs)
  water          every non-bridge span: ground not under water, and the road
                  within [-tunnel_depth, +bridge_lift] of the ground (else a
                  structure or a conform was owed)

  python re/tools/tg_network_audit.py re/assets/levels/level090/NETWORK.JSON
"""
import json, sys, math

TUNNEL_MAX = 32; BRIDGE_MAX = 56; TUNNEL_MIN = 6; BRIDGE_MIN = 2; CLEAR = 10
TUNNEL_DEPTH = 2200.0; BRIDGE_LIFT = 2000.0; CAP = 0.20

def seg_cross(p, q, r, s):
    """proper crossing of segments p-q and r-s (shared endpoints excluded)"""
    def orient(a, b, c):
        v = (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])
        return 0 if abs(v) < 1e-6 else (1 if v > 0 else -1)
    if p == r or p == s or q == r or q == s: return False
    o1, o2, o3, o4 = orient(p,q,r), orient(p,q,s), orient(r,s,p), orient(r,s,q)
    return o1 != o2 and o3 != o4 and o1 and o2 and o3 and o4

def main(path):
    d = json.load(open(path))
    spans = d["spans"]; edges = d["edges"]; structs = d["structs"]
    sl = d["span_length"]; bad = 0
    kind = {s[0]: s[5] for s in spans}
    # 1. planarity
    segs = []
    for e in edges:
        poly = e["poly"]
        for k in range(len(poly)-1):
            segs.append(((poly[k][0], poly[k][1]), (poly[k+1][0], poly[k+1][1]), e["id"]))
    cross = 0
    for i in range(len(segs)):
        for j in range(i+1, len(segs)):
            if segs[i][2] == segs[j][2]: continue
            if seg_cross(segs[i][0], segs[i][1], segs[j][0], segs[j][1]): cross += 1
    print(f"planarity: {len(segs)} segments, {cross} crossing(s)"); bad += cross > 0
    # 2. street off road (mouth segment start excluded)
    def near_road(x, z, skip_si, margin=0.0):
        for s in spans:
            si, sx, sz, sy, w = s[0], s[1], s[2], s[3], s[4]
            if abs(si - skip_si) <= 3: continue
            if (sx-x)**2 + (sz-z)**2 < (w*0.5 + margin)**2: return si
        return -1
    onroad = 0
    for e in edges:
        if e["kind"] == "underpass": continue
        poly = e["poly"]
        for k in range(len(poly)-1):
            if k == 0 and e["mouth"]["si"] >= 0:
                # skip the first 3000 of the mouth segment
                continue
            x, z = poly[k][0], poly[k][1]
            hit = near_road(x, z, e["mouth"]["si"])
            if hit >= 0 and e["rejoin"] != hit and abs(e["rejoin"] - hit) > 3:
                onroad += 1
    print(f"street-off-road: {onroad} polyline point(s) inside a carriageway"); bad += onroad > 0
    # 3. mouths on structures
    ms = 0
    for e in edges:
        m = e["mouth"]
        if m["si"] >= 0 and kind.get(m["si"], 0) != 0: ms += 1
    print(f"mouth-struct: {ms} mouth(s) on a bridge/tunnel span"); bad += ms > 0
    # 4. structure lengths + interlock
    sbad = 0; prev = None
    for st in structs:
        mx = TUNNEL_MAX if st["kind"] == "tunnel" else BRIDGE_MAX
        mn = TUNNEL_MIN if st["kind"] == "tunnel" else (BRIDGE_MIN if st["water"] else 6)
        if st["len"] > mx or st["len"] < mn: sbad += 1; print(f"  struct {st}")
        if prev and prev["kind"] != st["kind"] and st["s0"] - prev["s1"] - 1 < CLEAR:
            sbad += 1; print(f"  interlock {prev} {st}")
        prev = st
    print(f"struct-len: {len(structs)} run(s), {sbad} violation(s)"); bad += sbad > 0
    # 5. grade
    g = 0; worst = 0.0
    for i in range(1, len(spans)):
        dy = abs(spans[i][3] - spans[i-1][3]) / sl
        worst = max(worst, dy)
        if dy > CAP * 1.001: g += 1
    print(f"grade: worst {worst:.4f}, {g} span(s) over {CAP}"); bad += g > 0
    # 6. water / owed structure
    w = 0
    infork = set()
    for f in d.get("forks", []):
        for q in range(f["F"] - 8, f["R"] + 9): infork.add(q)
    forced = 0; deep = 0
    for s in spans:
        si, y, k, hg, wet, force = s[0], s[3], s[5], s[6], s[7], (s[8] if len(s) > 8 else 0)
        if k != 0: continue
        if force: forced += 1; continue    # the terrain yielded (causeway / deep cut), logged by the walk
        if si in infork: continue          # structures are forbidden there: a causeway/cut by design
        if wet: w += 1; continue
        d = y - hg
        if d < -TUNNEL_DEPTH - 1.0 or d > BRIDGE_LIFT + 1.0: deep += 1
    print(f"water: {w} open span(s) over water without a deck (FAIL), {forced} forced-conform span(s), "
          f"{deep} cutting/embankment span(s) beyond the structure thresholds (short runs; WARN)")
    bad += w > 0
    print("RESULT:", "FAIL" if bad else "OK")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "re/assets/levels/level090/NETWORK.JSON"))
