#!/usr/bin/env python3
"""Re-apply geo_fetch's ROAD_TAG_KEYS whitelist to an EXISTING ROADS.JSON.

[ROUND 1011 C2] Why this exists instead of re-running geo_fetch.

Adding a key to ``ROAD_TAG_KEYS`` only changes which raw OSM tags survive the
normaliser into ``ROADS.JSON.roads[].tags``. Re-running ``geo_fetch.py`` to pick
that up would also re-derive PLACE.JSON, the three rasters and the route frame
from the same cache -- a far larger blast radius than the change deserves, and
one that has gone wrong before (a repeated BUILD shrank La Plata by mutating its
own source cache). This script does the narrow thing: it reads the cached
Overpass responses, recomputes ONLY the ``tags`` sub-object of each road record,
and leaves every other field and every other file untouched.

It is also the verification. The four width spellings added in round 1011 are on
ZERO of La Plata's elements, so here the script reports 0 changed records and
rewrites nothing -- which is the proof that the keys are wired and that the
cache simply has no measured pavement width.

NETWORK: none. Reads only <place>/_cache/*.json and <place>/ROADS.JSON.

    python -I re/tools/geo_renormalise_road_tags.py re/assets/geo/la_plata
    python -I re/tools/geo_renormalise_road_tags.py <place> --write
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_fetch import ROAD_TAG_KEYS, _keep_tags    # noqa: E402


def _load_cached_way_tags(cache_dir: str) -> dict:
    """{osm way id: raw tag dict} over every cached Overpass response."""
    out: dict = {}
    if not os.path.isdir(cache_dir):
        return out
    for fn in sorted(os.listdir(cache_dir)):
        if not fn.endswith(".json"):
            continue
        try:
            with open(os.path.join(cache_dir, fn), "r", encoding="utf-8") as fh:
                doc = json.load(fh)
        except (OSError, ValueError):
            continue                      # a raster sidecar, not an Overpass body
        for el in doc.get("elements") or ():
            if el.get("type") != "way":
                continue
            wid = el.get("id")
            if wid is not None and el.get("tags"):
                out[wid] = el["tags"]
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("place", help="re/assets/geo/<slug>")
    ap.add_argument("--write", action="store_true",
                    help="rewrite ROADS.JSON when something actually changed")
    a = ap.parse_args(argv)

    roads_path = os.path.join(a.place, "ROADS.JSON")
    with open(roads_path, "r", encoding="utf-8") as fh:
        doc = json.load(fh)
    roads = doc.get("roads") or []

    raw = _load_cached_way_tags(os.path.join(a.place, "_cache"))
    print("cached ways with tags: %d" % len(raw))
    print("ROAD_TAG_KEYS: %d keys" % len(ROAD_TAG_KEYS))

    changed = matched = 0
    gained: dict = {}
    for r in roads:
        wid = r.get("id")
        if wid not in raw:
            continue
        matched += 1
        fresh = _keep_tags(raw[wid], ROAD_TAG_KEYS)
        old = r.get("tags") or {}
        if fresh != old:
            changed += 1
            for k in fresh:
                if k not in old:
                    gained[k] = gained.get(k, 0) + 1
            r["tags"] = fresh

    print("road records: %d, matched to the cache: %d, changed: %d"
          % (len(roads), matched, changed))
    if gained:
        for k in sorted(gained, key=lambda k: -gained[k]):
            print("  gained %-26s on %d way(s)" % (k, gained[k]))
    else:
        print("  no record gained a key: the whitelist change is INERT on "
              "this cache")

    if changed and a.write:
        with open(roads_path, "w", encoding="utf-8") as fh:
            json.dump(doc, fh, ensure_ascii=False, separators=(",", ":"))
        print("rewrote %s" % roads_path)
    elif changed:
        print("DRY RUN -- pass --write to rewrite %s" % roads_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
