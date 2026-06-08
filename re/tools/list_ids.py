#!/usr/bin/env python3
"""list_ids.py — dynamic Track + Car name<->ID reference for TD5RE.

Regenerates the canonical "reference a track or car by NAME instead of a numeric
ID" table by reading the live sources of truth in the repo, so it stays correct
whenever you add a new track or car:

  TRACKS  td5mod/src/td5re/td5_frontend.c
            s_track_display_names[]            (name index -> city name)
            s_track_schedule_to_name_index[]   (DefaultTrack -> name index)
          td5mod/src/td5re/td5_asset.c
            k_schedule_to_pool[] / k_pool_to_zip[]  (DefaultTrack -> levelNNN)
          re/assets/levels/levelNNN/LEVELINF.DAT
            DWORD[0]  (1 = circuit, 0 = point-to-point)

  CARS    td5mod/src/td5re/td5_asset.c
            s_car_zip_paths[]                  (DefaultCar -> cars/<code>.zip)
          re/assets/cars/<code>/config.nfo
            line 1 = internal name, line 2 = display name
          re/assets/cars/                      (every available archive code,
                                                including ones only reachable via
                                                [Game] PlayerCarArchive=<code>)

Usage:
    python re/tools/list_ids.py                 # print markdown table to stdout
    python re/tools/list_ids.py --md  > re/track_car_ids.md
    python re/tools/list_ids.py --json > re/track_car_ids.json
    python re/tools/list_ids.py --name moscow   # fuzzy lookup by name (track+car)

The numbers printed are exactly what you put in td5re.ini / on the CLI:
    [Game] DefaultTrack = N      (--DefaultTrack=N)
    [Game] DefaultCar   = N      (--DefaultCar=N)
    [Game] PlayerCarArchive = <code>   (slot-0 car override, any archive code)
"""
import os
import re
import sys
import json
import struct

# --- locate the project root (this file lives at <root>/re/tools/list_ids.py) ---
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
FRONTEND_C = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_frontend.c")
ASSET_C    = os.path.join(ROOT, "td5mod", "src", "td5re", "td5_asset.c")
LEVELS_DIR = os.path.join(ROOT, "re", "assets", "levels")
CARS_DIR   = os.path.join(ROOT, "re", "assets", "cars")


def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def _array_body(src, decl_regex):
    """Return the text between the first '{' and its matching '};' after a decl."""
    m = re.search(decl_regex, src)
    if not m:
        return None
    start = src.index("{", m.end() - 1)
    depth = 0
    for i in range(start, len(src)):
        c = src[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return src[start + 1:i]
    return None


# Require the declaration to be a brace initializer ("= {") so we don't match
# the array name where it appears inside a comment or an expression (e.g.
# "s_track_schedule_to_name_index[19]==0" in a comment matched "...]=" before).
def _decl(name):
    return r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{"


def parse_str_array(src, name):
    body = _array_body(src, _decl(name))
    if body is None:
        return []
    return re.findall(r'"((?:[^"\\]|\\.)*)"', body)


def parse_int_array(src, name):
    body = _array_body(src, _decl(name))
    if body is None:
        return []
    # strip /* ... */ comments so commented numbers aren't picked up
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    return [int(x) for x in re.findall(r"-?\d+", body)]


def parse_car_roster(src):
    """s_car_zip_paths[]: 'cars/<code>.zip', /* <idx> - <NAME> */  -> ordered list."""
    body = _array_body(src, _decl("s_car_zip_paths"))
    if body is None:
        return []
    out = []
    # each entry: "cars/xxx.zip",  /* 0  - NAME */
    for m in re.finditer(r'"cars/([^"]+?)\.zip"\s*,?\s*(?:/\*\s*(\d+)\s*-\s*([^*]*?)\s*\*/)?', body):
        code = m.group(1)
        idx = int(m.group(2)) if m.group(2) else len(out)
        comment_name = (m.group(3) or "").strip()
        out.append((idx, code, comment_name))
    return out


def read_levelinf_circuit(level_number):
    """Return 'circuit' / 'P2P' / '?' by reading LEVELINF.DAT DWORD[0]."""
    p = os.path.join(LEVELS_DIR, "level%03d" % level_number, "LEVELINF.DAT")
    if not os.path.isfile(p):
        return "?"
    try:
        with open(p, "rb") as f:
            data = f.read(4)
        if len(data) < 4:
            return "?"
        flag = struct.unpack("<i", data)[0]
        return "circuit" if flag == 1 else "P2P"
    except OSError:
        return "?"


def _clean(name):
    """TD5/TD6 config.nfo names use '_' for spaces; normalize for display."""
    return name.replace("_", " ").strip() if name else name


def read_config_nfo(code):
    """Return (display_name, internal_name) from re/assets/cars/<code>/config.nfo.
    config.nfo line 1 = internal name, line 2 = display name (with '_' for spaces)."""
    p = os.path.join(CARS_DIR, code, "config.nfo")
    if not os.path.isfile(p):
        return (None, None)
    try:
        lines = [l.rstrip("\r\n") for l in _read(p).splitlines()]
    except OSError:
        return (None, None)
    internal = lines[0].strip() if len(lines) > 0 else None
    display = lines[1].strip() if len(lines) > 1 else None
    return (_clean(display) or _clean(internal), internal)


def build_tracks():
    fe = _read(FRONTEND_C)
    asset = _read(ASSET_C)
    names = parse_str_array(fe, "s_track_display_names")
    sched_to_name = parse_int_array(fe, "s_track_schedule_to_name_index")
    sched_to_pool = parse_int_array(asset, "k_schedule_to_pool")
    pool_to_zip   = parse_int_array(asset, "k_pool_to_zip")

    tracks = []
    for ti in range(len(sched_to_name)):
        name_idx = sched_to_name[ti]
        name = names[name_idx] if 0 <= name_idx < len(names) else "TRACK %d" % ti
        # DefaultTrack -> level number (drag strip 19 is the special level 30).
        if ti < len(sched_to_pool):
            pool = sched_to_pool[ti]
            level = pool_to_zip[pool] if 0 <= pool < len(pool_to_zip) else None
        elif "DRAG" in name.upper():
            level = 30
        else:
            level = None
        kind = read_levelinf_circuit(level) if level is not None else "?"
        tracks.append({"id": ti, "name": name, "level": level, "kind": kind})
    return tracks


def build_cars():
    asset = _read(ASSET_C)
    roster = parse_car_roster(asset)             # [(idx, code, comment_name), ...]
    by_code = {}
    cars = []
    for idx, code, comment in roster:
        disp, internal = read_config_nfo(code)
        # Prefer the hand-written roster comment ("'97 CAMARO") over config.nfo's
        # underscore form; fall back to the cleaned config.nfo display, then code.
        name = comment or disp or code.upper()
        by_code[code] = idx
        cars.append({"id": idx, "code": code, "name": name,
                     "internal": internal, "roster_comment": comment})
    # Any extra archive codes present on disk but NOT in the indexed roster are
    # still usable via [Game] PlayerCarArchive=<code> (TD6 / bonus cars).
    extras = []
    if os.path.isdir(CARS_DIR):
        for code in sorted(os.listdir(CARS_DIR)):
            d = os.path.join(CARS_DIR, code)
            if not os.path.isdir(d) or code in by_code:
                continue
            disp, internal = read_config_nfo(code)
            extras.append({"id": None, "code": code,
                           "name": disp or code.upper(), "internal": internal})
    return cars, extras


def fuzzy(query, tracks, cars, extras):
    q = query.strip().lower()
    hits = []
    for t in tracks:
        if q in t["name"].lower():
            hits.append("TRACK  DefaultTrack=%-2d  %-28s %s  level%s" %
                        (t["id"], t["name"], t["kind"],
                         ("%03d" % t["level"]) if t["level"] is not None else "?"))
    for c in cars:
        if q in c["name"].lower() or q == c["code"].lower():
            hits.append("CAR    DefaultCar=%-3d code=%-4s %s" %
                        (c["id"], c["code"], c["name"]))
    for c in extras:
        if q in c["name"].lower() or q == c["code"].lower():
            hits.append("CAR    PlayerCarArchive=%-4s %s  (bonus; no DefaultCar index)" %
                        (c["code"], c["name"]))
    return hits


def render_md(tracks, cars, extras):
    out = []
    out.append("# TD5RE - Track & Car ID reference\n")
    out.append("_Generated by `re/tools/list_ids.py` - re-run after adding a track or car._\n")
    out.append("\n## Tracks - `[Game] DefaultTrack=N` / `--DefaultTrack=N`\n")
    out.append("| ID | Name | Kind | Level |")
    out.append("|---:|------|------|-------|")
    for t in tracks:
        lvl = ("level%03d" % t["level"]) if t["level"] is not None else "?"
        out.append("| %d | %s | %s | %s |" % (t["id"], t["name"], t["kind"], lvl))
    out.append("\n> Kind is read live from `LEVELINF.DAT` DWORD[0] (1=circuit, 0=P2P). "
               "Note the quickrace.ini circuit/P2P labels are inverted; this table is authoritative.\n")
    out.append("\n## Cars - `[Game] DefaultCar=N` / `--DefaultCar=N`\n")
    out.append("| ID | Code | Name |")
    out.append("|---:|------|------|")
    for c in cars:
        out.append("| %d | %s | %s |" % (c["id"], c["code"], c["name"]))
    if extras:
        out.append("\n## Bonus cars - `[Game] PlayerCarArchive=<code>` only (no DefaultCar index)\n")
        out.append("| Code | Name |")
        out.append("|------|------|")
        for c in extras:
            out.append("| %s | %s |" % (c["code"], c["name"]))
    out.append("")
    return "\n".join(out)


def main(argv):
    tracks = build_tracks()
    cars, extras = build_cars()

    if "--json" in argv:
        print(json.dumps({"tracks": tracks, "cars": cars, "bonus_cars": extras}, indent=2))
        return 0
    if "--name" in argv:
        i = argv.index("--name")
        query = argv[i + 1] if i + 1 < len(argv) else ""
        hits = fuzzy(query, tracks, cars, extras)
        print("\n".join(hits) if hits else "no match for %r" % query)
        return 0
    # default + --md both print markdown
    print(render_md(tracks, cars, extras))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
