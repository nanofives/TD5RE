#!/usr/bin/env python3
"""
dump_tpages.py — Dump runtime-assembled texture pages from running TD5_d3d.exe.

Pages 0-2 already exist in re/assets/static/ (extracted from static.zip).
Pages 4 and 5 are assembled at runtime (HUD/effects sprites, wheel hubs) and
have no corresponding .dat files.  This script hooks UploadRaceTexturePage
(VA 0x40B590) to capture pixel data for every page uploaded, then saves files
as 256×256 BGRA32 — the format expected by td5_asset.c:load_static_r5g6b5_tpage.

Usage:
  1. Launch the original TD5_d3d.exe
  2. Navigate into a race (all pages are uploaded during race initialisation)
  3. In another terminal: python re/tools/dump_tpages.py [--pages 4,5]
  4. When "All target pages captured" prints, Ctrl+C to exit
  5. Files written to re/assets/static/tpage4.dat, tpage5.dat

Pixel format notes:
  formatMode 0  →  RGB24 (3 bpp, R-G-B byte order from DecodeArchiveImageToRgb24)
                   Converted here to BGRA32 ([B,G,R,0xFF] per pixel).
  formatMode 1/2 → ARGB32 stored as little-endian uint32_t = bytes [B,G,R,A].
                   Already BGRA32, saved as-is.
"""

import argparse
import os
import sys
import time

ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "assets", "static")

# UploadRaceTexturePage(data_ptr, page_index, format_mode, is_env_map)
# VA 0x40B590, image base 0x400000 → RVA 0xB590
UPLOAD_RVA = 0x40B590 - 0x400000   # = 0xB590

JS_TEMPLATE = r"""
'use strict';

var mods = Process.enumerateModules();
var base = null;
for (var i = 0; i < mods.length; i++) {
    if (mods[i].name.toLowerCase() === "td5_d3d.exe") {
        base = mods[i].base;
        break;
    }
}
if (base === null) { throw new Error("TD5_d3d.exe module not found"); }
var fnAddr = base.add(%(rva)d);

send("[dump_tpages] Hook at " + fnAddr + " (base " + base + ")");

var TARGET_PAGES = %(pages_js)s;
var captured    = new Set();
var PAGE_W = 256, PAGE_H = 256;

Interceptor.attach(fnAddr, {
    onEnter: function(args) {
        var dataPtr  = args[0];
        var pageIdx  = args[1].toInt32();
        var fmtMode  = args[2].toInt32();

        if (TARGET_PAGES !== null && !TARGET_PAGES.has(pageIdx)) return;
        if (captured.has(pageIdx)) return;

        var bpp  = (fmtMode === 0) ? 3 : 4;
        var size = PAGE_W * PAGE_H * bpp;

        if (dataPtr.isNull()) {
            send("[dump_tpages] page " + pageIdx + " null ptr");
            return;
        }

        send("[dump_tpages] page=" + pageIdx +
                    " fmt=" + fmtMode + " bpp=" + bpp + " ptr=" + dataPtr);

        var bytes = Memory.readByteArray(dataPtr, size);
        send({ page: pageIdx, bpp: bpp }, bytes);
        captured.add(pageIdx);
    }
});

send("[dump_tpages] Waiting — enter a race to trigger uploads...");
"""


def rgb24_to_bgra32(raw: bytes, npx: int) -> bytearray:
    """Convert packed RGB24 (R-G-B order) to BGRA32."""
    out = bytearray(npx * 4)
    for i in range(npx):
        r, g, b = raw[i*3], raw[i*3+1], raw[i*3+2]
        out[i*4+0] = b
        out[i*4+1] = g
        out[i*4+2] = r
        out[i*4+3] = 0xFF
    return out


class Dumper:
    def __init__(self, target_pages, out_dir, overwrite):
        self.target  = set(target_pages)
        self.out_dir = out_dir
        self.done    = set()
        self.overwrite = overwrite

    def on_message(self, message, data):
        if message.get("type") != "send":
            lvl = message.get("type", "?").upper()
            print(f"[frida:{lvl}]", message.get("description") or message)
            return

        info = message["payload"]
        # String payloads are log messages
        if isinstance(info, str):
            print(info)
            return

        page = info["page"]
        bpp  = info["bpp"]

        if page in self.done:
            return

        raw  = bytes(data)
        npx  = 256 * 256

        if bpp == 3:
            # RGB24 → BGRA32
            bgra = rgb24_to_bgra32(raw, npx)
        else:
            # Already BGRA32 (little-endian ARGB32)
            bgra = bytearray(raw)

        out_path = os.path.join(self.out_dir, f"tpage{page}.dat")
        if os.path.exists(out_path) and not self.overwrite:
            print(f"[dump_tpages] {out_path} exists — skipping (use --overwrite)")
        else:
            with open(out_path, "wb") as f:
                f.write(bgra)
            print(f"[dump_tpages] Saved tpage{page}.dat  ({len(bgra)} bytes, "
                  f"bpp={bpp})")

        self.done.add(page)
        if self.done >= self.target:
            print("[dump_tpages] All target pages captured!")


def main():
    ap = argparse.ArgumentParser(description="Dump runtime texture pages from TD5_d3d.exe")
    ap.add_argument("--pages", default="4,5",
                    help="Comma-separated page indices to capture (default: 4,5)")
    ap.add_argument("--all", action="store_true",
                    help="Capture every page UploadRaceTexturePage is called for")
    ap.add_argument("--process", default="TD5_d3d.exe",
                    help="Process name to attach to (default: TD5_d3d.exe)")
    ap.add_argument("--overwrite", action="store_true",
                    help="Overwrite existing .dat files")
    ap.add_argument("--out-dir", default=OUT_DIR,
                    help=f"Output directory (default: {OUT_DIR})")
    args = ap.parse_args()

    try:
        import frida
    except ImportError:
        sys.exit("frida not installed — run: pip install frida")

    if args.all:
        target_pages = list(range(32))
        pages_js = "null"   # null sentinel → hook everything
    else:
        target_pages = [int(x.strip()) for x in args.pages.split(",")]
        pages_js = "new Set([" + ",".join(str(p) for p in target_pages) + "])"

    os.makedirs(args.out_dir, exist_ok=True)

    js = JS_TEMPLATE % {"rva": UPLOAD_RVA, "pages_js": pages_js}

    dumper = Dumper(target_pages, args.out_dir, args.overwrite)

    print(f"[dump_tpages] Attaching to {args.process}...")
    try:
        session = frida.attach(args.process)
    except frida.ProcessNotFoundError:
        sys.exit(f"ERROR: {args.process} not running. Launch the game first.")

    script = session.create_script(js)
    script.on("message", dumper.on_message)
    script.load()

    print(f"[dump_tpages] Pages to capture: {target_pages}")
    print( "[dump_tpages] Enter a race in the game, then wait...")
    print( "              Press Ctrl+C when done.\n")

    try:
        while not (dumper.done >= dumper.target):
            time.sleep(0.3)
        time.sleep(0.5)   # flush any last messages
    except KeyboardInterrupt:
        pass
    finally:
        session.detach()

    missing = dumper.target - dumper.done
    if missing:
        print(f"\n[dump_tpages] Pages NOT captured: {sorted(missing)}")
        print("  → Did the game load a race while the script was running?")
        print("  → If page 4 is missing try --all to diagnose which pages ARE uploaded.")
    else:
        print(f"\n[dump_tpages] Done.  Files in: {args.out_dir}")


if __name__ == "__main__":
    main()
