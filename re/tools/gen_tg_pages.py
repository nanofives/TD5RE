#!/usr/bin/env python3
"""gen_tg_pages.py -- MANIFEST-DRIVEN texture-page importer for the auto-track.

Every earlier import (gen_trackgen_city_tex.py, _r7city, _r8var, _r5flora,
_r11signs, gen_tg_furniture_tex.py) is a one-off script with the page list
hard-coded in Python and a hand-copied ALREADY set so it does not mine a page
twice. Adding a set of shipped pages to the generator therefore meant writing
a new script. This tool replaces that step with a JSON manifest:

    {
      "header":  "td5_tg_real_tex_<name>.h",     # written to td5mod/src/td5re
      "prefix":  "k_real_<name>",               # C symbol prefix
      "title":   "one line for the header comment",
      "sets": {
        "low":   [["level002", 61, "San Francisco: tan stone facade"], ...],
        "rail":  [["level014", 276, "Sydney: W-beam armco"]]
      }
    }

and emits, for every set NAME, exactly the symbol shape the generator already
consumes for td5_tg_real_tex_r8var.h (tg_emit_r8var_*_pages):

    static const int           <prefix>_<name><i>_paln;          colours
    static const unsigned char <prefix>_<name><i>_pal[3*paln];   BGR palette
    static const unsigned char <prefix>_<name><i>_idx[4096];     64x64 indices
    static const int           <prefix>_<name>_count;
    static const int           <prefix>_<name>_paln[count];
    static const unsigned char *const <prefix>_<name>_pal[count];
    static const unsigned char *const <prefix>_<name>_idx[count];

so a new set plugs into tg_emit_real_page(pages, pal[v], paln[v], idx[v], keyed)
with no per-set glue beyond the page-slot reservation in td5_trackgen.c.

Source of every page: <levels>/<level>/textures.src/textures.json (palette_hex,
BGR) + indices.bin (4096 bytes per page). Pages are copied VERBATIM, never
resampled, so the art is exactly what the shipped track shows.

The ALREADY-MINED registry is no longer hand-copied: --list-mined scans every
td5mod/src/td5re/td5_tg_*.h for its "/* levelNNN page NNN -> ... */" source
comments (the base td5_tg_real_tex.h is all level014 and writes "/* page NNN
-> ... */"), and the importer refuses a page that is already in any header
unless --allow-dup is given. That keeps the existing elements intact: this
tool never rewrites an existing header, it only adds new ones.

A pick from the in-game free-cam geometry picker (dev build, F-cam, left
click) copies a line like

    level023 L23 e29 s0 p451+320+321 pos 30312,1441,-122016 r20444 v476 c29

to the clipboard; --from-pick reads one or more of those (file or stdin) and
appends every page they name to a manifest set, so "I like that wall" ->
manifest entry is one paste. The pre-2026-09-06 JSON pick payload
({"track":"level014","level":14,"page":276,...}) is still accepted, so older
notes keep working. Several picks may share one line.

Usage:
    python re/tools/gen_tg_pages.py --list-mined [--levels DIR]
    python re/tools/gen_tg_pages.py MANIFEST.json [--levels DIR] [--allow-dup]
                                    [--dry-run]
    python re/tools/gen_tg_pages.py --from-pick MANIFEST.json SET [PICK.json]
            (appends the pick(s) to MANIFEST's SET; reads stdin if no file)

Assets root: --levels, else $TD5RE_ASSETS_ROOT/levels, else re/assets/levels.
"""

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SRC_DIR = os.path.join(ROOT, "td5mod", "src", "td5re")
TEXELS = 64 * 64

_MINED_RE = re.compile(r"^/\*\s*(level\d{3})\s+page\s+(\d+)\s*->", re.M)
_BASE_RE = re.compile(r"^/\*\s*page\s+(\d+)\s*->", re.M)


def levels_dir(arg):
    if arg:
        return arg
    root = os.environ.get("TD5RE_ASSETS_ROOT")
    if root:
        return os.path.join(root, "levels")
    return os.path.join(ROOT, "re", "assets", "levels")


def scan_mined():
    """(level, page) -> header file, for every page already baked into a
    td5_tg_*.h header."""
    mined = {}
    for h in sorted(glob.glob(os.path.join(SRC_DIR, "td5_tg_*.h"))):
        text = open(h, encoding="utf-8", errors="replace").read()
        name = os.path.basename(h)
        for lvl, pg in _MINED_RE.findall(text):
            mined.setdefault((lvl, int(pg)), name)
        if name == "td5_tg_real_tex.h":
            # Base header: every page is level014 (Sydney), comment lacks it.
            for pg in _BASE_RE.findall(text):
                mined.setdefault(("level014", int(pg)), name)
    return mined


def read_page(levels, level, num):
    src = os.path.join(levels, level, "textures.src")
    meta_p = os.path.join(src, "textures.json")
    if not os.path.exists(meta_p):
        sys.exit("%s: no textures.src/textures.json (level not extracted?)" % level)
    meta = json.load(open(meta_p))
    count = meta.get("page_count", len(meta.get("pages", [])))
    if num < 0 or num >= count:
        sys.exit("%s has no page %d (page_count %d)" % (level, num, count))
    pal = bytes.fromhex(meta["pages"][num]["palette_hex"])
    with open(os.path.join(src, "indices.bin"), "rb") as f:
        f.seek(num * TEXELS)
        idx = f.read(TEXELS)
    if len(idx) != TEXELS:
        sys.exit("%s page %d: short index block" % (level, num))
    if max(idx) >= len(pal) // 3:
        sys.exit("%s page %d: index %d past palette (%d colours)"
                 % (level, num, max(idx), len(pal) // 3))
    ptype = meta["pages"][num].get("type", 0)
    return pal, idx, ptype


def emit_array(out, decl, data, per_line=32):
    out.write("static const unsigned char %s[%d] = {" % (decl, len(data)))
    for i, v in enumerate(data):
        out.write(("\n" if i % per_line == 0 else "") + "%d," % v)
    out.write("};\n")


def emit_set(out, levels, prefix, name, entries):
    for i, ent in enumerate(entries):
        level, num, why = ent[0], int(ent[1]), (ent[2] if len(ent) > 2 else "")
        pal, idx, ptype = read_page(levels, level, num)
        key = idx.count(0) * 100.0 / TEXELS
        out.write("/* %s page %03d -> %s %d: %s (%d colours, key %.1f%%, type %d) */\n"
                  % (level, num, name.upper(), i, why, len(pal) // 3, key, ptype))
        out.write("static const int %s_%s%d_paln = %d;\n"
                  % (prefix, name, i, len(pal) // 3))
        emit_array(out, "%s_%s%d_pal" % (prefix, name, i), pal)
        emit_array(out, "%s_%s%d_idx" % (prefix, name, i), idx)
    n = len(entries)
    out.write("static const int %s_%s_count = %d;\n" % (prefix, name, n))
    out.write("static const int %s_%s_paln[%d] = { %s };\n"
              % (prefix, name, max(n, 1),
                 ", ".join("%s_%s%d_paln" % (prefix, name, i) for i in range(n)) or "0"))
    for kind in ("pal", "idx"):
        out.write("static const unsigned char *const %s_%s_%s[%d] = { %s };\n"
                  % (prefix, name, kind, max(n, 1),
                     ", ".join("%s_%s%d_%s" % (prefix, name, i, kind)
                               for i in range(n)) or "0"))
    out.write("\n")


def build(manifest_path, levels, allow_dup, dry_run):
    m = json.load(open(manifest_path))
    header = m["header"]
    prefix = m.get("prefix", "k_real_" + os.path.splitext(header)[0].split("_")[-1])
    guard = re.sub(r"[^A-Za-z0-9]", "_", header).upper()
    out_path = os.path.join(SRC_DIR, header)
    mined = scan_mined()

    if os.path.exists(out_path) and not m.get("regenerate", False):
        sys.exit("%s exists. Existing headers are never rewritten (keeps shipped "
                 "elements intact); pick a new header name, or set "
                 "\"regenerate\": true in the manifest if this header was "
                 "produced by this tool." % header)

    problems = []
    seen = set()
    for name, entries in m["sets"].items():
        for ent in entries:
            key = (ent[0], int(ent[1]))
            if key in seen:
                problems.append("%s page %d listed twice in the manifest" % key)
            seen.add(key)
            if key in mined and mined[key] != header and not allow_dup:
                problems.append("%s page %d already mined by %s" % (key + (mined[key],)))
    if problems:
        sys.exit("REFUSING:\n  " + "\n  ".join(problems) +
                 "\n(use --allow-dup to import a page a second time on purpose)")

    if dry_run:
        for name, entries in m["sets"].items():
            for ent in entries:
                pal, idx, ptype = read_page(levels, ent[0], int(ent[1]))
                print("%-10s %s page %03d  %3d colours  key %5.1f%%  type %d  %s"
                      % (name, ent[0], int(ent[1]), len(pal) // 3,
                         idx.count(0) * 100.0 / TEXELS, ptype,
                         ent[2] if len(ent) > 2 else ""))
        print("dry run: %s not written" % header)
        return

    with open(out_path, "w", newline="\n") as out:
        out.write("/* %s\n" % m.get("title", "Shipped texture pages for the auto-track."))
        out.write(" * Palettes are BGR and indices 64x64, copied verbatim from each\n"
                  " * level's textures.src. GENERATED by re/tools/gen_tg_pages.py from\n"
                  " * %s, do not edit -- edit the manifest and re-run. */\n"
                  % os.path.relpath(manifest_path, ROOT).replace("\\", "/"))
        out.write("#ifndef %s\n#define %s\n\n" % (guard, guard))
        for name, entries in m["sets"].items():
            emit_set(out, levels, prefix, name, entries)
        out.write("#endif /* %s */\n" % guard)
    print("wrote", out_path, os.path.getsize(out_path), "bytes;",
          sum(len(e) for e in m["sets"].values()), "pages in",
          len(m["sets"]), "set(s)")


# The picker emitted a JSON blob until 2026-09-06 and a compact one-liner after
# (td5_pick.c:525-558). Both are accepted: pasted history and old notes still
# work, and a fresh clipboard paste works, which is the whole point of the flag.
#
#   level023 L23 e29 s0 p451+320+321 pos 30312,1441,-122016 r20444 v476 c29
#   AUTO e12 s3 guardrail p107:RAIL+44 pos 1,2,3 r900 v48 c2
#
# `L<n>` is absent on the AUTO track and a mesh-kind word is present there
# instead; page ids may carry a ":NAME" suffix on AUTO. `c` is the COMMAND
# count, not a face count, and the page list is capped at 6 by the picker, so a
# busy mesh silently reports only its first six distinct pages.
PICK_LINE = re.compile(
    r"(?P<track>level\d{3}|AUTO)"
    r"(?:\s+L(?P<lvl>\d+))?"
    r"\s+e(?P<entry>\d+)"
    r"\s+s(?P<slot>\d+)"
    r"(?:\s+(?P<kind>[A-Za-z_][A-Za-z0-9_]*))?"
    r"\s+p(?P<pages>\d+(?::[A-Za-z0-9_]+)?(?:\+\d+(?::[A-Za-z0-9_]+)?)*)"
    r"\s+pos\s+(?P<pos>-?\d+,\s*-?\d+,\s*-?\d+)"
    r"\s+r(?P<r>-?\d+)\s+v(?P<v>\d+)\s+c(?P<c>\d+)")


def parse_picks(text):
    """Parse either pick format into the dict shape the manifest writer wants."""
    picks = []
    for chunk in re.findall(r"\{[^{}]*\}", text):
        try:
            picks.append(json.loads(chunk))
        except ValueError:
            pass
    for m in PICK_LINE.finditer(text):
        picks.append({
            "track": m.group("track"),
            "level": int(m.group("lvl")) if m.group("lvl") else None,
            "entry": int(m.group("entry")), "slot": int(m.group("slot")),
            "kind": m.group("kind") or "pick",
            "pages": [int(p.split(":")[0]) for p in m.group("pages").split("+")],
            "pos": m.group("pos"), "r": int(m.group("r")),
            "verts": int(m.group("v")), "cmds": int(m.group("c")),
        })
    return picks


def from_pick(manifest_path, set_name, pick_path):
    text = open(pick_path).read() if pick_path else sys.stdin.read()
    picks = parse_picks(text)
    if not picks:
        sys.exit("no picks found (expected a picker line such as "
                 "'level023 L23 e29 s0 p451+320 pos 1,2,3 r10 v4 c1', "
                 "or a legacy {...} JSON pick)")
    m = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {
        "header": "td5_tg_real_tex_%s.h" % os.path.splitext(
            os.path.basename(manifest_path))[0],
        "sets": {}}
    entries = m.setdefault("sets", {}).setdefault(set_name, [])
    added = 0
    for p in picks:
        track = p.get("track")
        if not (isinstance(track, str) and track.startswith("level")):
            print("skip pick from %r (not a shipped level; the AUTO track's pages "
                  "are already generator pages)" % track)
            continue
        for pg in p.get("pages") or [p.get("page")]:
            if pg is None:
                continue
            ent = [track, int(pg), "%s (picked at %s)"
                   % (p.get("kind", "pick"), p.get("pos", ""))]
            if any(e[0] == ent[0] and int(e[1]) == ent[1] for e in entries):
                continue
            entries.append(ent)
            added += 1
    with open(manifest_path, "w", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    print("appended %d page(s) to %s set %r" % (added, manifest_path, set_name))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("manifest", nargs="?")
    ap.add_argument("--levels", default=None)
    ap.add_argument("--list-mined", action="store_true")
    ap.add_argument("--allow-dup", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--from-pick", nargs="+", metavar=("MANIFEST SET", "PICK"))
    args = ap.parse_args()

    if args.list_mined:
        mined = scan_mined()
        by_level = {}
        for (lvl, pg), h in sorted(mined.items()):
            by_level.setdefault(lvl, []).append((pg, h))
        for lvl, pages in sorted(by_level.items()):
            print("%s: %d pages" % (lvl, len(pages)))
            for pg, h in pages:
                print("    page %03d  %s" % (pg, h))
        print("total mined pages: %d" % len(mined))
        return
    if args.from_pick:
        if len(args.from_pick) < 2:
            ap.error("--from-pick needs MANIFEST SET [PICK.json]")
        from_pick(args.from_pick[0], args.from_pick[1],
                  args.from_pick[2] if len(args.from_pick) > 2 else None)
        return
    if not args.manifest:
        ap.error("manifest required (or --list-mined / --from-pick)")
    build(args.manifest, levels_dir(args.levels), args.allow_dup, args.dry_run)


if __name__ == "__main__":
    main()
