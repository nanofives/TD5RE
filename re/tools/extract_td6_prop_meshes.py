#!/usr/bin/env python3
"""extract_td6_prop_meshes.py — import the REAL VISIBLE breakable street
furniture for migrated TD6 city tracks.

RE finding (2026-06-16): TD6 props are a TWO-layer system.
  * level<N>.tcl / Level<N>b.tcl = INVISIBLE collision footprints (16-byte recs).
    extract_td6_props.py already copies these as LEVEL.TCL / LEVELB.TCL.
  * level.mov / Levelb.mov        = the VISIBLE furniture instances (24-byte recs)
    drawn as COL_NN.prr MESHES. THIS is what the player sees and smashes; it was
    never ported, so the source port wrongly rendered the .tcl footprints.

MOV record (24 bytes), London level004 verified:
  [0]    serial id (1..N, 0 = end)
  [4]    model byte: low nibble &0x0F = COL_<idx> mesh (0..11); high nibble flags
  [12:16] worldX s32 (24.8)   [16:20] worldY s32 (24.8)   [20:24] worldZ s32 (24.8)
  [18]   orientation byte (12-bit angle = byte<<4 in 0x1000 units; here byte*16)
The COL_NN.prr meshes are standalone render-type 0x04 indexed meshes (same family
as the TD6 traffic cars), so we de-index them with convert_td6_tracks._deindex_mesh
into TD5-native 0x03 meshes the renderer already understands.

Outputs into re/assets/levels/level<td5>/:
  LEVEL.MOV, LEVELB.MOV       raw 24-byte furniture tables (port parses them)
  COL_00.prr .. COL_NN.prr    de-indexed furniture meshes
Run from repo root.
"""
import os, sys, struct, zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "re", "tools"))
import convert_td6_tracks as C
import zlib

TD6_LEVELS = os.path.join(ROOT, "Test Drive 6", "LEVELS")
TD6_STATIC = os.path.join(ROOT, "Test Drive 6", "static.zip")
ASSETS     = os.path.join(ROOT, "re", "assets", "levels")
PROPS_DIR  = os.path.join(ROOT, "re", "assets", "props")

# The 12 furniture prototype textures (TD6 static.zip) -> td6_<name>.png. The COL
# meshes' command texture-slot is mapped to one of these via the per-level subset
# table (London row @0x0049be58 = BENCH/REDTAPE/1BOLLARD/K1BOX/1CRATE).
FURNITURE_TGAS = {
    "1bin.tga": "1bin", "1bollard.tga": "1bollard", "1crate.tga": "1crate",
    "1phone.tga": "1phone", "1worky.tga": "1worky", "bins.tga": "bins",
    "Canopy.tga": "canopy", "Chair.tga": "chair", "Redtape.tga": "redtape",
    "rick1.tga": "rick1", "Bench.tga": "bench", "K1box.tga": "k1box",
}

def _png_write(path, w, h, rows):
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    raw = b"".join(b"\x00" + r for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))

def convert_furniture_textures():
    """SOLID (opaque) PNGs: the COL meshes have full-face UVs, so the cutout
    sprites' transparent margin is filled with each texture's own average colour
    to avoid see-through holes. Vertical flip (TGA bottom-up) + BGRA->RGBA."""
    if not os.path.exists(TD6_STATIC):
        print("static.zip MISSING — skip furniture textures"); return
    os.makedirs(PROPS_DIR, exist_ok=True)
    z = zipfile.ZipFile(TD6_STATIC)
    names = {n.lower(): n for n in z.namelist()}
    for src, name in FURNITURE_TGAS.items():
        ent = names.get(src.lower())
        if not ent:
            continue
        raw = z.read(ent)
        idlen = raw[0]; w, h = struct.unpack("<HH", raw[12:16]); desc = raw[17]
        botup = (desc & 0x20) == 0
        px = raw[18 + idlen: 18 + idlen + w * h * 4]
        so = [0, 0, 0, 0]
        for i in range(w * h):
            if px[i*4+3] > 40:
                so[0] += px[i*4+2]; so[1] += px[i*4+1]; so[2] += px[i*4]; so[3] += 1
        fr, fg, fb = (round(so[0]/max(1, so[3])), round(so[1]/max(1, so[3])), round(so[2]/max(1, so[3])))
        rows = []
        for sy in range(h):
            y = (h - 1 - sy) if botup else sy
            row = bytearray()
            for x in range(w):
                i = (y*w + x) * 4
                b, g, r, a = px[i], px[i+1], px[i+2], px[i+3]
                if a < 40: r, g, b = fr, fg, fb
                row += bytes((r, g, b, 255))
            rows.append(bytes(row))
        _png_write(os.path.join(PROPS_DIR, f"td6_{name}.png"), w, h, rows)
    print(f"furniture textures -> {PROPS_DIR}/td6_*.png ({len(FURNITURE_TGAS)})")

# (TD6 level number, port td5 level dir number, name) — mirror extract_td6_props
PAIRS = [
    (4, 12, "London"), (0, 8, "Paris"), (1, 9, "NewYork"), (2, 10, "Rome"),
    (3, 11, "HongKong"), (10, 7, "Pelton"), (11, 18, "Ireland"),
    (15, 19, "Tahoe"), (16, 20, "CapeHatteras"), (17, 21, "Switzerland"),
    (18, 22, "Egypt"),
]

def find_entry(z, *cands):
    names = {n.lower(): n for n in z.namelist()}
    for c in cands:
        if c.lower() in names:
            return names[c.lower()]
    return None

def deindex_prr(blob):
    """COL mesh -> single de-indexed 0x03 mesh (or a sub_count display block)."""
    mblist, info = C._deindex_mesh(blob, 0)
    if mblist is None:
        return None, info
    mblist = [m for m in mblist if m is not None]
    if not mblist:
        return None, "empty"
    if len(mblist) == 1:
        return mblist[0], "1 mesh"
    sub = len(mblist); hdr = 4 + sub * 4
    data = bytearray(); offs = []
    for mb in mblist:
        offs.append(hdr + len(data)); data += mb
    blk = bytearray(struct.pack("<I", sub))
    for o in offs: blk += struct.pack("<I", o)
    blk += data
    return bytes(blk), f"{sub} submeshes"

def mov_summary(raw):
    from collections import Counter
    models = Counter(); n = 0
    for i in range(len(raw) // 24):
        if raw[i*24] == 0:
            # serial 0 = terminator (only the trailing record)
            if i == len(raw)//24 - 1: break
        models[raw[i*24+4] & 0x0F] += 1; n += 1
    return n, dict(sorted(models.items()))

def main():
    only = sys.argv[1:] or None
    convert_furniture_textures()
    for td6, td5, name in PAIRS:
        if only and name not in only and str(td5) not in only:
            continue
        zp = os.path.join(TD6_LEVELS, f"level{td6:03d}.zip")
        od = os.path.join(ASSETS, f"level{td5:03d}")
        if not os.path.exists(zp) or not os.path.isdir(od):
            print(f"{name:14s} level{td6:03d}->{td5:03d}: zip/dir MISSING"); continue
        z = zipfile.ZipFile(zp)
        # --- MOV furniture tables (raw passthrough) ---
        for cands, dst in ((("level.mov",), "LEVEL.MOV"),
                           (("Levelb.mov", "levelb.mov"), "LEVELB.MOV")):
            e = find_entry(z, *cands)
            if not e:
                print(f"{name:14s} {dst}: not in zip"); continue
            raw = z.read(e)
            n, hist = mov_summary(raw)
            with open(os.path.join(od, dst), "wb") as f:
                f.write(raw)
            print(f"{name:14s} {dst}: {len(raw)}B  {n} props  models={hist}")
        # --- COL_NN.prr meshes (de-indexed) + PROPMESH.BIN render blob ---
        ncol = 0
        col_meshes = []   # (radius, [(x,y,z,u,v,col)...]) for the render blob
        for idx in range(16):
            e = find_entry(z, f"COL_{idx:02d}.prr", f"col_{idx:02d}.prr")
            if not e:
                continue
            mesh, info = deindex_prr(z.read(e))
            if mesh is None:
                print(f"{name:14s} COL_{idx:02d}: de-index FAILED ({info})"); continue
            with open(os.path.join(od, f"COL_{idx:02d}.prr"), "wb") as f:
                f.write(mesh)
            col_meshes.append(parse_deindexed_mesh(mesh))
            ncol += 1
            print(f"{name:14s} COL_{idx:02d}.prr -> {len(mesh)}B ({info})")
        # PROPMESH.BIN = 'PMS2' magic, mesh_count, per-mesh [u32 vcount, f32 radius],
        # then tri-list verts [3xf32 pos, 2xf32 uv, u32 col]. Consumed by
        # td5_render_load_td6_prop_meshes / render_td6_props.
        if col_meshes:
            blob = bytearray(struct.pack("<4sI", b"PMS2", len(col_meshes)))
            for radius, verts in col_meshes:
                blob += struct.pack("<If", len(verts), radius)
            for radius, verts in col_meshes:
                for x, y, zz, u, v, col in verts:
                    blob += struct.pack("<fffffI", x, y, zz, u, v, col)
            with open(os.path.join(od, "PROPMESH.BIN"), "wb") as f:
                f.write(blob)
            print(f"{name:14s} PROPMESH.BIN -> {len(blob)}B  verts={[len(v) for _,v in col_meshes]}")
        print(f"{name:14s} == {ncol} COL meshes ==")

# de-indexed TD5 0x03 mesh: 0x38 header (MESH_HDR), then tri-list 44B verts
# (pos@0, baked-grey ARGB@0x18, uv@0x1C). Extract pos+uv+col for the render blob.
_PMESH_HDR = "<BBHii fffffff IIII"
def parse_deindexed_mesh(b):
    h = struct.unpack_from(_PMESH_HDR, b, 0)
    vcnt = h[4]; radius = h[5]; out_verts = h[14]
    verts = []
    for i in range(vcnt):
        vo = out_verts + i * 44
        x, y, zz = struct.unpack_from("<fff", b, vo)
        col = struct.unpack_from("<I", b, vo + 24)[0]
        u, v = struct.unpack_from("<ff", b, vo + 28)
        verts.append((x, y, zz, u, v, col))
    return radius, verts

if __name__ == "__main__":
    main()
