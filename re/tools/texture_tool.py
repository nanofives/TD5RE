#!/usr/bin/env python3
"""
texture_tool.py -- export/import Test Drive 5 TEXTURES.DAT (per-level track
texture set) to/from an editable source dir, for the pack-on-load retirement
(td5_assetsrc.c, Tier 3a).

TEXTURES.DAT layout (little-endian), per td5_asset.c load_race_texture_pages:
  u32 page_count
  u32 offsets[page_count]        -- byte offset from file start to each page
  each page at its offset:
    byte[3] pad | byte type | i32 pal_count | pal_count*3 BGR palette | 4096 idx
  (64x64, 1 byte/pixel palette index)

All shipping files are contiguous (offsets[0]=4+4*pc, pages back-to-back, no
gaps/dups), so rebuild is deterministic and byte-exact.

Source dir (textures.src/):
  textures.json  -- { page_count, pages:[{offset, pad_hex, type, palette_hex}] }
  indices.bin    -- page_count * 4096 raw index bytes (page i at i*4096)  [AUTHORITATIVE]
  pages/page_NNN.png -- decoded RGBA preview (edit surface; non-authoritative)

Runtime authoritative = indices.bin + palette_hex. Editing a PNG only takes
effect after `pack` re-quantizes it back into indices.bin (nearest palette
colour; palette preserved). Unedited pages stay byte-exact.

Usage:
  python texture_tool.py export <textures.dat> <out_dir>
  python texture_tool.py import <src_dir>      <out.dat>
  python texture_tool.py pack   <src_dir>            # PNG edits -> indices.bin
"""

import json
import os
import struct
import sys


def _parse(data: bytes):
    pc = struct.unpack_from("<I", data, 0)[0]
    offs = list(struct.unpack_from(f"<{pc}I", data, 4))
    pages = []
    indices = bytearray()
    for o in offs:
        pad = data[o:o + 3]
        ptype = data[o + 3]
        pal_count = struct.unpack_from("<i", data, o + 4)[0]
        pal = data[o + 8:o + 8 + pal_count * 3]
        idx = data[o + 8 + pal_count * 3:o + 8 + pal_count * 3 + 4096]
        pages.append({"offset": o, "pad_hex": pad.hex(),
                      "type": int(ptype), "palette_hex": pal.hex()})
        indices += idx
    return pc, pages, bytes(indices)


def export(dat_path: str, out_dir: str):
    data = open(dat_path, "rb").read()
    pc, pages, indices = _parse(data)
    os.makedirs(out_dir, exist_ok=True)

    with open(os.path.join(out_dir, "textures.json"), "w", encoding="utf-8") as fh:
        json.dump({"_format": "td5_textures", "_version": 1,
                   "page_count": pc, "pages": pages}, fh, indent=1)
        fh.write("\n")
    with open(os.path.join(out_dir, "indices.bin"), "wb") as fh:
        fh.write(indices)

    # PNG previews (non-authoritative). Indexed 'P' mode -> fast, no pixel loop.
    try:
        from PIL import Image
        pdir = os.path.join(out_dir, "pages")
        os.makedirs(pdir, exist_ok=True)
        for i, p in enumerate(pages):
            pal = bytes.fromhex(p["palette_hex"])         # BGR triplets
            idx = bytes(indices[i * 4096:(i + 1) * 4096])
            img = Image.frombytes("P", (64, 64), idx)
            rgb = bytearray()
            for c in range(len(pal) // 3):
                rgb += bytes((pal[c * 3 + 2], pal[c * 3 + 1], pal[c * 3 + 0]))
            if rgb:
                img.putpalette(list(rgb))
            img.save(os.path.join(pdir, f"page_{i:03d}.png"))
    except Exception as e:               # PNG previews are optional
        print(f"  (PNG previews skipped: {e})", file=sys.stderr)


def _load_src(src_dir: str):
    with open(os.path.join(src_dir, "textures.json"), encoding="utf-8") as fh:
        man = json.load(fh)
    indices = open(os.path.join(src_dir, "indices.bin"), "rb").read()
    return man, indices


def build(man: dict, indices: bytes) -> bytes:
    pc = man["page_count"]
    pages = man["pages"]
    # total size = max(page_end); files are contiguous so this == original size
    total = 4 + 4 * pc
    for p in pages:
        pal = bytes.fromhex(p["palette_hex"])
        end = p["offset"] + 8 + len(pal) + 4096
        total = max(total, end)
    out = bytearray(total)
    struct.pack_into("<I", out, 0, pc)
    for i, p in enumerate(pages):
        struct.pack_into("<I", out, 4 + 4 * i, p["offset"])
    for i, p in enumerate(pages):
        o = p["offset"]
        pal = bytes.fromhex(p["palette_hex"])
        out[o:o + 3] = bytes.fromhex(p["pad_hex"])
        out[o + 3] = p["type"] & 0xFF
        struct.pack_into("<i", out, o + 4, len(pal) // 3)
        out[o + 8:o + 8 + len(pal)] = pal
        out[o + 8 + len(pal):o + 8 + len(pal) + 4096] = indices[i * 4096:(i + 1) * 4096]
    return bytes(out)


def imp(src_dir: str, dat_path: str):
    man, indices = _load_src(src_dir)
    with open(dat_path, "wb") as fh:
        fh.write(build(man, indices))


def pack(src_dir: str):
    """Re-quantize edited pages/page_NNN.png back into indices.bin (nearest
    colour in the page's existing palette; palette unchanged)."""
    from PIL import Image
    man, indices = _load_src(src_dir)
    indices = bytearray(indices)
    pdir = os.path.join(src_dir, "pages")
    changed = 0
    for i, p in enumerate(man["pages"]):
        png = os.path.join(pdir, f"page_{i:03d}.png")
        if not os.path.exists(png):
            continue
        pal = bytes.fromhex(p["palette_hex"])
        ncol = len(pal) // 3
        if ncol == 0:
            continue
        img = Image.open(png).convert("RGBA").resize((64, 64))
        px = img.load()
        for y in range(64):
            for x in range(64):
                r, g, b, _ = px[x, y]
                best, bd = 0, 1 << 30
                for c in range(ncol):
                    pb, pg, pr = pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]
                    d = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2
                    if d < bd:
                        bd, best = d, c
                indices[i * 4096 + y * 64 + x] = best
        changed += 1
    with open(os.path.join(src_dir, "indices.bin"), "wb") as fh:
        fh.write(indices)
    print(f"packed {changed} edited page(s) into indices.bin")


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("export", "import", "pack"):
        print(__doc__)
        sys.exit(2)
    mode = sys.argv[1]
    if mode == "export":
        export(sys.argv[2], sys.argv[3])
    elif mode == "import":
        imp(sys.argv[2], sys.argv[3])
    else:
        pack(sys.argv[2])


if __name__ == "__main__":
    main()
