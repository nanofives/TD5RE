#!/usr/bin/env python3
"""fix_td6_type11_links.py — correct the TD6 backward-junction (span type 11)
`link_prev` ATTRIBUTE in the editable strip.json / stripb.json source data.

WHY (RE'd in TD6.exe, 2026-06-12): TD5's route walker reads a type-11 (backward)
junction's branch target from `link_prev` (+0x0A), but TD6's exporter writes the
target of BOTH junction kinds into `link_next` (+0x08) and leaves `link_prev` as a
per-section GARBAGE constant (e.g. London 41050 / 10262, Paris/Rome/etc. their own).
`convert_td6_tracks.py` passes the 24-byte span record through unchanged, so the
editable strip.json inherits the garbage; the engine then patches it in RAM at load
(td5_track.c TD6 backward-junction fix). This tool bakes the SAME fix into the source
of truth so the data is self-consistent and no longer relies on the runtime hack.

THE FIX (identical guard to td5_track.c:1782): for every span_type==11, if link_prev
read as a SIGNED int16 is out of [0, span_count) while link_next is in range, set
link_prev = link_next. Native TD5 tracks (valid link_prev) are left untouched, so this
is a no-op there — but it only runs on the TD6 asset levels listed below anyway.

Span JSON layout: [type, b1, b2, lane, vl, vr, link_next(6), link_prev(7), ox, oy, oz].
Run from repo root. Writes BOTH the main re/assets and the AI-worktree copy.
"""
import os, json, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# TD6 asset levels (re/assets/levels/levelNNN): the 5 P2P + 6 circuit migrations.
TD6_ASSET_LEVELS = [7, 8, 9, 10, 11, 12, 18, 19, 20, 21, 22]
ASSET_ROOTS = [
    os.path.join(ROOT, "re", "assets", "levels"),
    os.path.join(ROOT, ".claude", "worktrees", "fix-1780934761-153592-28290",
                 "re", "assets", "levels"),
]


def s16(v):
    """interpret a JSON-stored u16 link field as the engine's signed int16."""
    return v - 65536 if v >= 32768 else v


def fix_strip_json(path):
    if not os.path.exists(path):
        return None
    sj = json.load(open(path))
    spans = sj.get("spans")
    if not spans:
        return None
    n = len(spans)
    patched = 0
    for s in spans:
        # type-9 (SENTINEL_START) and type-11 (backward junction) are the span
        # types the walker FOLLOWS via link_prev. TD6 stores the junction target
        # in link_NEXT for both (RE'd + geometry-verified: link_next is co-located
        # with the span for all 56 London type-9/11 spans, while link_prev is a
        # per-section garbage constant on 30 of them). So set link_prev = link_next
        # UNCONDITIONALLY (not just when link_prev is out of range — the in-range
        # garbage like London span 744 link_prev=2070 was the residual teleport).
        if s[0] not in (9, 11):
            continue
        nxt = s16(s[6])
        if 0 <= nxt < n and s[7] != s[6]:   # link_next valid & differs from link_prev
            s[7] = s[6]                      # link_prev <- link_next (store same u16)
            patched += 1
    if patched:
        # compact form matches the dat-retirement writer (no spurious whitespace)
        json.dump(sj, open(path, "w"), separators=(",", ":"))
    return patched


def main():
    levels = TD6_ASSET_LEVELS if len(sys.argv) < 2 else [int(a) for a in sys.argv[1:]]
    grand = 0
    for root in ASSET_ROOTS:
        tag = "main" if root.endswith(os.path.join("re", "assets", "levels")) else "worktree"
        print(f"=== {tag}: {root} ===")
        if not os.path.isdir(root):
            print("  (root missing, skip)")
            continue
        for lvl in levels:
            d = os.path.join(root, f"level{lvl:03d}")
            line = f"  level{lvl:03d}:"
            for fn in ("strip.json", "stripb.json"):
                r = fix_strip_json(os.path.join(d, fn))
                if r is None:
                    line += f" {fn}=--"
                else:
                    line += f" {fn}={r}"
                    grand += r
            print(line)
    print(f"\nTOTAL type-11 link_prev fields corrected: {grand}")


if __name__ == "__main__":
    main()
