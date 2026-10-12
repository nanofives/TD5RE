#!/usr/bin/env python3
"""[R1021 C item 12] Rail / fence census of a generated level: how many guardrail meshes (kind rail)
and kerb / arm fence meshes (page 46 FENCE, any kind) does it carry?
  python verify/r1021c_rail_census.py LEVELDIR [--json out]"""
import collections, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geo_bld_audit as A
def main():
    b, meshes = A.load_level(sys.argv[1])
    rail = [m for m in meshes if m["kind"] == "rail"]
    fence = collections.Counter(); fence_q = collections.Counter()
    for m in meshes:
        for pg, t, q in m["cmds"]:
            if pg == 46:
                fence[m["kind"]] += 1; fence_q[m["kind"]] += q + t
    rq = sum(q + t for m in rail for pg, t, q in m["cmds"])
    out = {"guardrail_meshes": len(rail), "guardrail_quads": rq,
           "fence_meshes": dict(fence), "fence_meshes_total": sum(fence.values()),
           "fence_quads_total": sum(fence_q.values())}
    print(json.dumps(out))
    if len(sys.argv) > 3 and sys.argv[2] == "--json": json.dump(out, open(sys.argv[3], "w"))
main()
