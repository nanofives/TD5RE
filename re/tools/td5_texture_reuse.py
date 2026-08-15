#!/usr/bin/env python3
"""
td5_texture_reuse.py -- reuse existing TD5 track texture pages when authoring a
custom level. Copies a page (index bytes + inline palette + type) from one
level's editable texture pool into another's, so generated geometry can point
its `texture_page_id` at real game art instead of the flat asphalt page.

Why this works with NO engine recompile: each page in `textures.src/` is
self-contained -- it carries its OWN inline palette (see td5_assetsrc.c
td5_src_encode_textures) -- so a page id is nothing but an index into the
per-level `pages[]` array. Cross-track reuse = copy the page's 4096 index bytes
+ palette + type into the target pool, append it as a new page, and reference
the new index. There is no global palette and no palette-sharing constraint.

The one packing invariant (enforced here): the C encoder honors each page's
manifest `offset` and sizes TEXTURES.DAT from max(offset + 8 + palette + 4096).
Adding a page grows the `4 + 4*page_count` offset header, so EVERY offset must
be recomputed contiguously. `_recompute_offsets` does that (matches the layout
td5_trackgen.write_road_textures emits: page 0 at offset 8 when page_count==1).

A "level dir" here is a `levelNNN/` directory; its pool lives in
`<level_dir>/textures.src/` (textures.json + indices.bin + pages/page_NNN.png).
If a source level has only a packed `TEXTURES.DAT` and no `textures.src/`, the
pool is read straight from the .DAT via texture_tool.

Usage (CLI, for quick manual reuse):
  python td5_texture_reuse.py list   <level_dir>
  python td5_texture_reuse.py import <target_level_dir> <source_level_dir> <page> [<page> ...]
"""
from __future__ import annotations

import json
import os
import struct
import sys

PAGE_IDX_BYTES = 4096          # 64 x 64, 1 byte/pixel palette index


def _pool_dir(level_dir: str) -> str:
    return os.path.join(level_dir, "textures.src")


def _find_textures_dat(level_dir: str):
    for name in os.listdir(level_dir):
        if name.lower() == "textures.dat":
            return os.path.join(level_dir, name)
    return None


def _load_pool(level_dir: str):
    """Return (manifest_dict, indices_bytearray) for a level's texture pool.

    Prefers the editable `textures.src/`; falls back to parsing a packed
    `TEXTURES.DAT` (read-only source levels) via texture_tool.
    """
    src = _pool_dir(level_dir)
    jpath = os.path.join(src, "textures.json")
    ipath = os.path.join(src, "indices.bin")
    if os.path.isfile(jpath) and os.path.isfile(ipath):
        with open(jpath, encoding="utf-8") as fh:
            man = json.load(fh)
        with open(ipath, "rb") as fh:
            indices = bytearray(fh.read())
        return man, indices

    dat = _find_textures_dat(level_dir)
    if dat:
        import texture_tool
        pc, pages, indices = texture_tool._parse(open(dat, "rb").read())
        man = {"_format": "td5_textures", "_version": 1,
               "page_count": pc, "pages": pages}
        return man, bytearray(indices)

    raise FileNotFoundError(
        "no texture pool in %s (need textures.src/ or TEXTURES.DAT)" % level_dir)


def _recompute_offsets(pages):
    """Lay pages contiguously after the `4 + 4*page_count` offset header, in
    array order. Mutates each page's `offset` in place; returns total bytes."""
    pc = len(pages)
    cursor = 4 + 4 * pc
    for p in pages:
        p["offset"] = cursor
        pal_n = len(bytes.fromhex(p.get("palette_hex", "")))
        cursor += 8 + pal_n + PAGE_IDX_BYTES
    return cursor


def _write_page_png(pool_dir: str, page_index: int, palette_hex: str, idx: bytes):
    """Best-effort RGBA preview (non-authoritative). Silently skips without PIL."""
    try:
        from PIL import Image
    except Exception:
        return
    pal = bytes.fromhex(palette_hex)                      # BGR triplets
    img = Image.frombytes("P", (64, 64), bytes(idx))
    rgb = bytearray()
    for c in range(len(pal) // 3):
        rgb += bytes((pal[c * 3 + 2], pal[c * 3 + 1], pal[c * 3 + 0]))
    if rgb:
        img.putpalette(list(rgb))
    pdir = os.path.join(pool_dir, "pages")
    os.makedirs(pdir, exist_ok=True)
    img.save(os.path.join(pdir, "page_%03d.png" % page_index))


def _save_pool(level_dir: str, man: dict, indices: bytearray, *, png_from=None):
    """Write textures.json + indices.bin back to `<level_dir>/textures.src/`.
    `png_from` is an optional {page_index: (palette_hex, idx_bytes)} map of pages
    to (re)render a PNG preview for."""
    src = _pool_dir(level_dir)
    os.makedirs(src, exist_ok=True)
    man["page_count"] = len(man["pages"])
    with open(os.path.join(src, "textures.json"), "w", encoding="utf-8") as fh:
        json.dump(man, fh, indent=2)
        fh.write("\n")
    with open(os.path.join(src, "indices.bin"), "wb") as fh:
        fh.write(bytes(indices))
    for pi, (pal_hex, idx) in (png_from or {}).items():
        _write_page_png(src, pi, pal_hex, idx)


def list_pages(level_dir: str):
    """Summarize a level's texture pages: [{id, type, palette_len, size}]."""
    man, _ = _load_pool(level_dir)
    out = []
    for i, p in enumerate(man["pages"]):
        out.append({"id": i, "type": int(p.get("type", 0)),
                    "palette_len": len(bytes.fromhex(p.get("palette_hex", ""))) // 3,
                    "size": "64x64"})
    return out


def import_texture_page(target_level_dir: str, source_level_dir: str,
                        source_page: int) -> int:
    """Copy one page from `source_level_dir` into `target_level_dir`'s pool.
    Returns the new page id (index) in the target. Re-packs on next game load
    from the updated textures.src -- no explicit pack step needed."""
    return import_texture_pages(target_level_dir, source_level_dir, [source_page])[0]


def import_texture_pages(target_level_dir: str, source_level_dir: str,
                         pages) -> list:
    """Copy several source pages into the target pool. Returns the list of new
    page ids in the target (order matches `pages`)."""
    src_man, src_idx = _load_pool(source_level_dir)
    tgt_man, tgt_idx = _load_pool(target_level_dir)
    src_pages = src_man["pages"]
    new_ids = []
    png_from = {}
    for sp in pages:
        sp = int(sp)
        if sp < 0 or sp >= len(src_pages):
            raise IndexError("source page %d out of range (0..%d)"
                             % (sp, len(src_pages) - 1))
        srcp = src_pages[sp]
        new_id = len(tgt_man["pages"])
        page_bytes = bytes(src_idx[sp * PAGE_IDX_BYTES:(sp + 1) * PAGE_IDX_BYTES])
        if len(page_bytes) != PAGE_IDX_BYTES:
            raise ValueError("source indices.bin truncated for page %d" % sp)
        tgt_man["pages"].append({
            "offset": 0,                       # fixed up by _recompute_offsets
            "pad_hex": srcp.get("pad_hex", "000000"),
            "type": int(srcp.get("type", 0)),
            "palette_hex": srcp.get("palette_hex", ""),
        })
        tgt_idx += page_bytes
        png_from[new_id] = (srcp.get("palette_hex", ""), page_bytes)
        new_ids.append(new_id)
    _recompute_offsets(tgt_man["pages"])
    _save_pool(target_level_dir, tgt_man, tgt_idx, png_from=png_from)
    return new_ids


# ---------------------------------------------------------------------------
def _cli():
    if len(sys.argv) >= 3 and sys.argv[1] == "list":
        for p in list_pages(sys.argv[2]):
            print("page %3d  type=%d  palette=%d colours" %
                  (p["id"], p["type"], p["palette_len"]))
        return 0
    if len(sys.argv) >= 5 and sys.argv[1] == "import":
        target, source = sys.argv[2], sys.argv[3]
        ids = import_texture_pages(target, source, [int(x) for x in sys.argv[4:]])
        print("imported source pages %s -> target ids %s" %
              (sys.argv[4:], ids))
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    sys.exit(_cli())
