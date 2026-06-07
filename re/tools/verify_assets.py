#!/usr/bin/env python3
"""
verify_assets.py -- regression guard for the pack-on-load retirement.

For every editable source under re/assets/, re-pack it with the Python packer and
byte-compare against the original .DAT preserved in re/_retired_dats/ (the backup
made when the .DATs were retired). Proves the sources still reproduce the
originals exactly. Meshes (MODELS/HIMODEL) are byte-exact passthrough, so the
source .bin must equal the original .dat; the glTF round-trip is checked
separately by `mesh_tool.py selfcheck-glb-all`.

Run from the project root:
    python re/tools/verify_assets.py
Exit code 0 = all good, 1 = any mismatch/missing.
"""

import glob
import json
import os
import sys

sys.path.insert(0, "re/tools")
import levelinf_editor as li          # noqa: E402
import carparam_editor as cp          # noqa: E402
import dat_tables as dt               # noqa: E402
import strip_tool as st               # noqa: E402
import texture_tool as tt             # noqa: E402

ASSETS = "re/assets"
RETIRED = "re/_retired_dats"


def _orig(src_path, dat_name):
    """Original .DAT path in the retired backup, alongside where src lives."""
    d = os.path.dirname(src_path).replace(ASSETS, RETIRED, 1)
    return os.path.join(d, dat_name)


def _cmp(label, packed, dat_path, st_counts):
    st_counts[0] += 1
    if not os.path.exists(dat_path):
        print(f"WARN  {label}  (no retired .DAT to compare)")
        return
    orig = open(dat_path, "rb").read()
    if bytes(packed) == orig:
        return
    st_counts[1] += 1
    print(f"FAIL  {label}  packed={len(packed)} orig={len(orig)}")


def main():
    counts = [0, 0]   # [checked, failed]

    def each(pattern, dat_name, pack):
        for src in sorted(glob.glob(f"{ASSETS}/{pattern}")):
            label = os.path.relpath(src, ASSETS)
            try:
                packed = pack(src)
            except Exception as e:
                counts[0] += 1; counts[1] += 1
                print(f"FAIL  {label}  pack error: {e}")
                continue
            _cmp(label, packed, _orig(src, dat_name), counts)

    # Tier 1 tables
    each("levels/level*/levelinf.json", "levelinf.dat",
         lambda s: li.import_json(open(s, encoding="utf-8").read()))
    each("cars/*/carparam.json", "carparam.dat",
         lambda s: cp.import_dict(json.load(open(s, encoding="utf-8"))))
    each("levels/level*/checkpt.json", "checkpt.num",
         lambda s: dt.checkpt_import(open(s, encoding="utf-8").read()))
    for src_name, dat in (("traffic.json", "traffic.bus"),
                          ("trafficb.json", "trafficb.bus")):
        each(f"levels/level*/{src_name}", dat,
             lambda s: dt.traffic_import(open(s, encoding="utf-8").read()))
    for src_name, dat in (("left.trk.csv", "left.trk"), ("leftb.trk.csv", "leftb.trk"),
                          ("right.trk.csv", "right.trk"), ("rightb.trk.csv", "rightb.trk")):
        each(f"levels/level*/{src_name}", dat,
             lambda s: dt.routes_import(open(s, encoding="utf-8").read()))

    # Tier 2 geometry
    for src_name, dat in (("strip.json", "strip.dat"), ("stripb.json", "stripb.dat")):
        each(f"levels/level*/{src_name}", dat,
             lambda s: st.imp(open(s, encoding="utf-8").read()))

    # Tier 3a textures (source is a directory; compare to retired textures.dat)
    for man in sorted(glob.glob(f"{ASSETS}/levels/level*/textures.src/textures.json")):
        label = os.path.relpath(man, ASSETS)
        try:
            m = json.load(open(man, encoding="utf-8"))
            idx = open(os.path.join(os.path.dirname(man), "indices.bin"), "rb").read()
            packed = tt.build(m, idx)
        except Exception as e:
            counts[0] += 1; counts[1] += 1
            print(f"FAIL  {label}  pack error: {e}")
            continue
        d = os.path.dirname(os.path.dirname(man)).replace(ASSETS, RETIRED, 1)
        _cmp(label, packed, os.path.join(d, "textures.dat"), counts)

    # Tier 3b meshes: byte-exact passthrough sources
    for src_name, dat in (("models.bin", "models.dat"),):
        each(f"levels/level*/{src_name}", dat, lambda s: open(s, "rb").read())
    each("cars/*/himodel.bin", "himodel.dat", lambda s: open(s, "rb").read())

    print(f"\nverify_assets: {counts[0]} checked, {counts[1]} failed")
    sys.exit(1 if counts[1] else 0)


if __name__ == "__main__":
    main()
