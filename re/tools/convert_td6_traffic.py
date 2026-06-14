#!/usr/bin/env python3
"""convert_td6_traffic.py — import the REAL per-city TD6 traffic car models.

TD6's `Test Drive 6/traffic.zip` holds CITY-specific traffic vehicles:
  lo0..lo4 + lopol  (London)   pa* (Paris)   ro* (Rome)   ny* (New York)
  hk* (Hong Kong)   — 6 cars each (5 civilian + 1 police), each a standalone
  indexed (render-type 0x04) PRR mesh + a 256x256 .tga skin.

The source port's traffic loader (td5_asset_load_traffic_model) loads
`model<N>.prr` + `skin<N>.png` from re/assets/traffic/ and expects TD5-native
de-indexed (0x03) meshes. The 6-row TD5 model table only covers the original
TD5 cities, so migrated TD6 city tracks borrow track-0's (Moscow's) cars.

This tool de-indexes each city PRR (reusing convert_td6_tracks._deindex_mesh,
the SAME 0x04->0x03 conversion used for track meshes) and writes them as the
high model indices the C side maps each TD6 city to:

  London=31..36  Paris=37..42  Rome=43..48  NewYork=49..54  HongKong=55..60

so the existing index-based loader picks them up with no new load path.
Run from repo root.
"""
import os, sys, struct, io, zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "re", "tools"))
import convert_td6_tracks as C
from PIL import Image

TRAFFIC_ZIP = os.path.join(ROOT, "Test Drive 6", "traffic.zip")
OUT_DIR     = os.path.join(ROOT, "re", "assets", "traffic")

# city prefix -> (base model index, list of 6 source names in pool order).
# Pool order mirrors s_traffic_model_table rows: [a,b,c,POLICE-slot uses idx3,d,e];
# we keep civilian 0..4 then police so slot_in_pool maps 1:1 to {0,1,2,pol,3,4}-ish.
# Simpler: just lay them 0,1,2,3,4,pol — the C override maps slot_in_pool directly.
CITIES = {
    "lo": (31, ["lo0", "lo1", "lo2", "lo3", "lo4", "lopol"]),  # London
    "pa": (37, ["pa0", "pa1", "pa2", "pa3", "pa4", "papol"]),  # Paris
    "ro": (43, ["ro0", "ro1", "ro2", "ro3", "ro4", "ropol"]),  # Rome
    "ny": (49, ["ny0", "ny1", "ny2", "ny3", "ny4", "nypol"]),  # New York
    "hk": (55, ["hk0", "hk1", "hk2", "hk3", "hk4", "hkpol"]),  # Hong Kong
}


def deindex_prr(blob):
    """De-index a standalone TD6 0x04 car mesh (at offset 0) into a single TD5
    0x03 mesh. Returns mesh bytes, or None. Cars are small (no spatial split),
    so _deindex_mesh yields one sub-mesh; if it yields more we pack them into a
    TD5 display-list block (sub_count header) the renderer accepts."""
    mblist, info = C._deindex_mesh(blob, 0)
    if mblist is None:
        return None, info
    mblist = [m for m in mblist if m is not None]
    if not mblist:
        return None, "empty"
    if len(mblist) == 1:
        return mblist[0], "1 mesh"
    # multi sub-mesh -> display-list block [sub_count][offs..][data..]
    sub = len(mblist)
    hdr_sz = 4 + sub * 4
    data = bytearray()
    offs = []
    for mb in mblist:
        offs.append(hdr_sz + len(data))
        data += mb
    blk = bytearray()
    blk += struct.pack("<I", sub)
    for o in offs:
        blk += struct.pack("<I", o)
    blk += data
    return bytes(blk), f"{sub} submeshes"


def main():
    cities = sys.argv[1:] or list(CITIES.keys())
    z = zipfile.ZipFile(TRAFFIC_ZIP)
    names = {n.lower(): n for n in z.namelist()}
    os.makedirs(OUT_DIR, exist_ok=True)
    grand = 0
    for city in cities:
        base, srcs = CITIES[city]
        print(f"=== {city} (base model {base}) ===")
        for i, src in enumerate(srcs):
            idx = base + i
            prr = names.get(src + ".prr")
            tga = names.get(src + ".tga")
            if not prr or not tga:
                print(f"  {src}: MISSING prr/tga in traffic.zip — skip")
                continue
            mesh, info = deindex_prr(z.read(prr))
            if mesh is None:
                print(f"  {src} -> model{idx}: de-index FAILED ({info})")
                continue
            with open(os.path.join(OUT_DIR, f"model{idx}.prr"), "wb") as fh:
                fh.write(mesh)
            # skin: TGA -> PNG (drop a garbage all-zero alpha so it isn't
            # premultiplied to black, same guard as the texture pipeline).
            im = Image.open(io.BytesIO(z.read(tga)))
            if im.mode == "RGBA" and im.getchannel("A").getextrema()[1] == 0:
                im = im.convert("RGB")
            im.convert("RGB").save(os.path.join(OUT_DIR, f"skin{idx}.png"))
            print(f"  {src} -> model{idx}.prr ({len(mesh)} B, {info}) + skin{idx}.png {im.size}")
            grand += 1
    print(f"\nDONE: {grand} city traffic models written to {OUT_DIR}")


if __name__ == "__main__":
    main()
