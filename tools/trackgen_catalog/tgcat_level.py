"""tgcat_level.py -- everything the catalogue learns from a BUILT level.

Inputs (all written by the generator into re/assets/levels/level<NN>/):
  MODELS.DAT    the scenery meshes, per span-entry blocks of sub-mesh slots
                (decoded with re/tools/mesh_tool.py, the byte-exact decoder)
  MESHTAG.BIN   per-(entry, slot) emitter kind TG_GK_* (dev builds only,
                td5_tg_guard.c tg_meshtag_write)
  TEXTURES.DAT  the page atlas: per page a palette + 64x64 indices
  GENSTAMP.TXT  seed / spec / exe identity of the build

Products:
  pages()            [{slot, fmt, paln, pal(bytes), idx(bytes), image(RGBA PIL)}]
  slots()            iterate (entry, slot, kind, mesh) with the ORIGINAL slot
                     number kept (mesh_tool.decode drops empty slots, which
                     would shift the MESHTAG lookup)
  shipped_index()    hash of every page of every SHIPPED level's textures.src,
                     to tell a verbatim copy of real art from a procedural page
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "re", "tools"))
import mesh_tool as mt  # noqa: E402

U = lambda b, o: struct.unpack_from("<I", b, o)[0]  # noqa: E731

MESHTAG_MAGIC = 0x4741544D
MESHTAG_NONE = 0xFF


def read_genstamp(level_dir):
    p = os.path.join(level_dir, "GENSTAMP.TXT")
    if not os.path.exists(p):
        return {}
    txt = open(p, encoding="utf-8", errors="replace").read().strip()
    out = {"raw": txt}
    for tok in txt.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def read_meshtag(level_dir):
    p = os.path.join(level_dir, "MESHTAG.BIN")
    if not os.path.exists(p):
        return None
    data = open(p, "rb").read()
    magic, ver, seed, nent, stride = struct.unpack_from("<5I", data, 0)
    if magic != MESHTAG_MAGIC:
        return None
    return {"seed": seed, "nentries": nent, "stride": stride, "tags": data[20:]}


def tag_of(mtag, entry, slot):
    if not mtag or entry >= mtag["nentries"] or slot >= mtag["stride"]:
        return MESHTAG_NONE
    return mtag["tags"][entry * mtag["stride"] + slot]


def slots(level_dir):
    """Yield (entry, slot, mesh_dict) for every sub-mesh in MODELS.DAT, with the
    on-disk slot index (the MESHTAG key)."""
    data = open(os.path.join(level_dir, "MODELS.DAT"), "rb").read()
    size = len(data)
    count, table, entries = mt._parse_entries(data)
    cache = {}
    for e, (off, sz) in enumerate(entries):
        if off == 0 or off >= size:
            continue
        block_size = sz if (sz and off + sz <= size) else size - off
        sub = U(data, off)
        if sub == 0 or sub > 256 or block_size < 4 + sub * 4:
            continue
        for j in range(sub):
            moff = U(data, off + 4 + j * 4)
            if moff == 0:
                continue
            if moff < block_size and moff + mt.HDR <= block_size:
                a = off + moff
            elif moff < size and moff + mt.HDR <= size:
                a = moff
            else:
                continue
            cc = struct.unpack_from("<i", data, a + 4)[0]
            vc = struct.unpack_from("<i", data, a + 8)[0]
            if cc < 0 or cc > 4096 or vc < 0 or vc > 65536:
                continue
            if a not in cache:
                cache[a] = mt._parse_mesh(data, a)
            yield e, j, cache[a]


def faces(mesh):
    """Yield (page, [(pos, uv), ...]) per triangle/quad, mirroring the
    renderer's command walk (each command eats tri*3 + quad*4 vertices)."""
    verts, cur = mesh["vertices"], 0
    for c in mesh["commands"]:
        page = c["texture_page_id"]
        for _ in range(c["tri"]):
            if cur + 3 > len(verts):
                return
            yield page, [(verts[k]["pos"], verts[k]["tex"]) for k in range(cur, cur + 3)]
            cur += 3
        for _ in range(c["quad"]):
            if cur + 4 > len(verts):
                return
            yield page, [(verts[k]["pos"], verts[k]["tex"]) for k in range(cur, cur + 4)]
            cur += 4


# ---------------------------------------------------------------------------
# TEXTURES.DAT
# ---------------------------------------------------------------------------

def _rgba(pal, paln, idx, fmt):
    """Palette bytes are B,G,R (the original's colour order)."""
    lut = []
    for i in range(256):
        if i < paln:
            b, g, r = pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]
            a = 0 if (fmt == 1 and i == 0) else 255
            lut.append((r, g, b, a))
        else:
            lut.append((255, 0, 255, 255))
    img = Image.new("RGBA", (64, 64))
    img.putdata([lut[v] for v in idx])
    return img


def pages(level_dir):
    data = open(os.path.join(level_dir, "TEXTURES.DAT"), "rb").read()
    n = U(data, 0)
    out = []
    for i in range(n):
        off = U(data, 4 + i * 4)
        if off + 8 > len(data):
            continue
        fmt = data[off + 3]
        paln = U(data, off + 4)
        if paln == 0 or paln > 256:
            continue
        pal = data[off + 8: off + 8 + paln * 3]
        idx = data[off + 8 + paln * 3: off + 8 + paln * 3 + 4096]
        out.append({"slot": i, "fmt": fmt, "paln": paln, "pal": pal, "idx": idx,
                    "image": _rgba(pal, paln, idx, fmt)})
    return out


def page_keys(pal, paln, idx):
    """exact = same palette + same indices (a verbatim copy);
    shape = same indices, any palette (a recolour / tint of a real page)."""
    exact = hashlib.sha1(bytes(pal[:paln * 3]) + bytes(idx)).hexdigest()
    shape = hashlib.sha1(bytes(idx)).hexdigest()
    return exact, shape


def shipped_index(levels_root, skip=("level090", "level091")):
    """{exact_hash: [(level, page)]}, {shape_hash: [(level, page)]} over every
    shipped / converted level's textures.src. Cached next to this file."""
    cache = os.path.join(HERE, "site", "_shipped_index.json")
    sig = []
    for lv in sorted(os.listdir(levels_root)):
        j = os.path.join(levels_root, lv, "textures.src", "textures.json")
        if lv in skip or not os.path.exists(j):
            continue
        sig.append(f"{lv}:{int(os.path.getmtime(j))}")
    sig = "|".join(sig)
    if os.path.exists(cache):
        try:
            c = json.load(open(cache))
            if c.get("sig") == sig:
                return c["exact"], c["shape"]
        except Exception:
            pass
    exact, shape = {}, {}
    for lv in sorted(os.listdir(levels_root)):
        d = os.path.join(levels_root, lv, "textures.src")
        j = os.path.join(d, "textures.json")
        if lv in skip or not os.path.exists(j):
            continue
        meta = json.load(open(j))
        ib = os.path.join(d, "indices.bin")
        if not os.path.exists(ib):
            continue
        ind = open(ib, "rb").read()
        for p, pg in enumerate(meta.get("pages", [])):
            pal = bytes.fromhex(pg.get("palette_hex", ""))
            idx = ind[p * 4096:(p + 1) * 4096]
            if len(idx) != 4096 or not pal:
                continue
            e, s = page_keys(pal, len(pal) // 3, idx)
            exact.setdefault(e, []).append([lv, p])
            shape.setdefault(s, []).append([lv, p])
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    json.dump({"sig": sig, "exact": exact, "shape": shape}, open(cache, "w"))
    return exact, shape


def _sig16(rgb64):
    """16x16 mean-colour signature of a 64x64x3 array, float32 0..255."""
    import numpy as np
    a = np.asarray(rgb64, dtype=np.float32).reshape(16, 4, 16, 4, 3)
    return a.mean(axis=(1, 3)).reshape(-1)


def shipped_signatures(levels_root, skip=("level090", "level091")):
    """(names[(level, page)], sigs ndarray[N, 768], std[N]) of every shipped
    page, for the approximate match. Cached as .npz next to the site."""
    import numpy as np
    cache = os.path.join(HERE, "site", "_shipped_sigs.npz")
    if os.path.exists(cache):
        z = np.load(cache, allow_pickle=False)
        return [tuple(x) for x in z["names"].tolist()], z["sigs"], z["std"]
    names, sigs, std = [], [], []
    for lv in sorted(os.listdir(levels_root)):
        d = os.path.join(levels_root, lv, "textures.src")
        j = os.path.join(d, "textures.json")
        ib = os.path.join(d, "indices.bin")
        if lv in skip or not os.path.exists(j) or not os.path.exists(ib):
            continue
        meta = json.load(open(j))
        ind = open(ib, "rb").read()
        for p, pg in enumerate(meta.get("pages", [])):
            pal = bytes.fromhex(pg.get("palette_hex", ""))
            idx = ind[p * 4096:(p + 1) * 4096]
            if len(idx) != 4096 or len(pal) < 3:
                continue
            lut = np.zeros((256, 3), dtype=np.float32)
            n = len(pal) // 3
            arr = np.frombuffer(pal[:n * 3], dtype=np.uint8).reshape(n, 3)[:, ::-1]
            lut[:n] = arr
            rgb = lut[np.frombuffer(idx, dtype=np.uint8)].reshape(64, 64, 3)
            names.append((lv, str(p)))
            sigs.append(_sig16(rgb))
            std.append(float(rgb.std()))
    sigs = np.stack(sigs).astype(np.float32)
    std = np.asarray(std, dtype=np.float32)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    np.savez(cache, names=np.asarray(names), sigs=sigs, std=std)
    return names, sigs, std


def approx_match(image, names, sigs, std, max_err=2.5, min_std=12.0):
    """Closest shipped page to a generated page by mean absolute difference of
    the 16x16 colour signature. Returns (level, page, err) or None. Flat pages
    (std < min_std) never match: a grey asphalt is not evidence of a copy."""
    import numpy as np
    rgb = np.asarray(image.convert("RGB"), dtype=np.float32)
    if rgb.std() < min_std:
        return None
    s = _sig16(rgb)
    err = np.abs(sigs - s).mean(axis=1)
    err[std < min_std] = 1e9
    k = int(err.argmin())
    if err[k] <= max_err:
        return names[k][0], int(names[k][1]), float(err[k])
    return None


def level_names(levels_root):
    """level dir -> human track name from levelinf.json when present."""
    out = {}
    for lv in sorted(os.listdir(levels_root)):
        j = os.path.join(levels_root, lv, "levelinf.json")
        if os.path.exists(j):
            try:
                d = json.load(open(j))
                nm = d.get("name") or d.get("track_name") or d.get("title")
                if nm:
                    out[lv] = nm
            except Exception:
                pass
    return out
