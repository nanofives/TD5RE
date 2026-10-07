"""[GEO LAND] Re-stamp BUILDINGS.JSON `landmark` from the RAW Overpass cache.

THE PROBLEM THIS SOLVES, measured on La Plata. geo_fetch's landmark rule is

    tourism or historic or (name and building in {cathedral, church, stadium,
                            museum, train_station, civic, public})

and it flags 10 of 2047 footprints, only 2 of them within 100 m of the route.
Walking the raw Overpass response for the 54 buildings that ARE within 100 m
finds nine more that any reasonable reading calls a landmark:

    office=government x4, government=administrative/ministry/legislative/yes,
    amenity=theatre, amenity=place_of_worship, amenity=police

NONE of those tags reach BUILDINGS.JSON -- geo_fetch keeps `building`,
`name`, heights and the roof tags and drops the rest -- so no amount of work
on the C reader can recover them. The permanent fix is geo_fetch's tag list;
this tool is the offline stand-in, because the raw responses are cached on
disk beside the place and re-fetching needs the network.

It rewrites `landmark` in place and adds `landmark_src` naming the tag that
decided it, so a promotion is attributable. Idempotent, and it never demotes:
anything geo_fetch already flagged stays flagged.

  python re/tools/geo_fixtures/land_relabel.py [--place la_plata] [--dry-run]
"""
import argparse
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
GEO = os.path.join(ROOT, "re", "assets", "geo")

# tag -> set of values that name a landmark, or True for "any value".
#
# 2026-10-07: this table moved INTO geo_fetch.py as LANDMARK_RULE, which is the
# permanent home the close-out asked for. The copy here is kept BYTE-IDENTICAL
# (asserted below) because this tool's remaining job is upgrading an OLD cache
# in place without the network, and it must agree with what a re-fetch would
# produce. Edit geo_fetch.py and mirror it here, never the other way round.
#
# Order is the ATTRIBUTION order only -- `landmark` is an OR over every row --
# so it names the most specific statement of what the structure IS.
RULE = {
    "building": {"cathedral", "church", "chapel", "basilica", "mosque",
                 "synagogue", "temple", "monastery", "shrine", "stadium",
                 "museum", "palace", "castle", "monument", "memorial",
                 "train_station", "courthouse", "townhall", "government",
                 "civic", "public", "theatre", "opera_house"},
    "government": True,
    "office": {"government", "diplomatic"},
    "amenity": {"place_of_worship", "theatre", "townhall", "courthouse",
                "arts_centre", "police", "fire_station", "embassy", "casino",
                "cinema", "conference_centre", "exhibition_centre",
                "monastery", "public_building"},
    "historic": True,
    "heritage": True,
    "man_made": {"tower", "lighthouse", "obelisk", "water_tower", "campanile"},
    "tourism": {"attraction", "museum", "gallery", "artwork", "viewpoint",
                "theme_park", "aquarium", "zoo", "monument"},
}

# Deliberately NOT promoted, and why: a hotel or a hostel carries tourism=*
# and is ordinary street frontage; geo_fetch's blanket `tourism or historic`
# is what put "UNICO Eco Hostel Boutique" in the La Plata landmark list.
# amenity=community_centre is out for the same reason -- La Plata has 14 of
# them and they are neighbourhood social clubs in ordinary shopfronts, not
# buildings anyone would drive past and recognise.
TOURISM_NOT = {"hotel", "hostel", "guest_house", "motel", "apartment",
               "chalet", "camp_site", "caravan_site", "information"}

# THE TWO TABLES MUST AGREE. This tool upgrades a cache in place and a re-fetch
# rebuilds it; if they diverge, the same place gets two different landmark sets
# depending on which path last touched it, and nothing would say so. Checked at
# import rather than documented, because a comment asking two files to stay in
# step is how they drift.
try:
    sys.path.insert(0, os.path.join(HERE, ".."))
    import geo_fetch as _gf                              # noqa: E402
except Exception as _exc:                                # noqa: BLE001
    print("warn: cannot import geo_fetch to cross-check the rule (%s)" % _exc,
          file=sys.stderr)
else:
    assert RULE == _gf.LANDMARK_RULE, (
        "land_relabel.RULE has drifted from geo_fetch.LANDMARK_RULE; edit "
        "geo_fetch.py and mirror it here")
    assert TOURISM_NOT == _gf.TOURISM_NOT_LANDMARK, (
        "land_relabel.TOURISM_NOT has drifted from "
        "geo_fetch.TOURISM_NOT_LANDMARK")


def near_route(place_dir, limit_m=100.0):
    """Set of building ids whose ring comes within limit_m of the route."""
    rp = os.path.join(place_dir, "ROUTE.JSON")
    bp = os.path.join(place_dir, "BUILDINGS.JSON")
    if not (os.path.isfile(rp) and os.path.isfile(bp)):
        return None
    route = json.load(open(rp, encoding="utf-8"))
    upm = route.get("units_per_metre") or 430.0
    pts = [(q["x"], q["z"]) for q in route["points"]]
    lim2 = (limit_m * upm) ** 2
    far2 = (400.0 * upm) ** 2
    out = set()
    for b in json.load(open(bp, encoding="utf-8"))["buildings"]:
        ring = b["points"]
        cx = sum(q["x"] for q in ring) / len(ring)
        cz = sum(q["z"] for q in ring) / len(ring)
        if min((cx - rx) ** 2 + (cz - rz) ** 2 for rx, rz in pts) > far2:
            continue
        hit = False
        for q in ring:
            for rx, rz in pts:
                if (q["x"] - rx) ** 2 + (q["z"] - rz) ** 2 <= lim2:
                    hit = True
                    break
            if hit:
                break
        if hit:
            out.add(b["id"])
    return out


def decide(tags):
    """(is_landmark, 'key=value') or (False, None)."""
    for key, allow in RULE.items():
        v = tags.get(key)
        if not v:
            continue
        if key == "tourism" and v in TOURISM_NOT:
            continue
        if allow is True or v in allow:
            return True, "%s=%s" % (key, v)
    return False, None


def load_raw(place_dir):
    """osm id -> tags, from every cached Overpass response beside the place."""
    out = {}
    for p in sorted(glob.glob(os.path.join(place_dir, "_cache", "*.json"))):
        try:
            d = json.load(open(p, encoding="utf-8"))
        except Exception:
            continue
        for e in d.get("elements") or []:
            t = e.get("tags")
            if t and e.get("id") is not None:
                out.setdefault(e["id"], {}).update(t)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--place", default="la_plata")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    pdir = os.path.join(GEO, a.place)
    bpath = os.path.join(pdir, "BUILDINGS.JSON")
    if not os.path.isfile(bpath):
        raise SystemExit("no BUILDINGS.JSON at " + bpath)
    raw = load_raw(pdir)
    if not raw:
        raise SystemExit("no raw Overpass cache under %s/_cache -- nothing to "
                         "relabel from" % pdir)

    doc = json.load(open(bpath, encoding="utf-8"))
    bs = doc["buildings"]
    near = near_route(pdir) or set()
    before = sum(1 for b in bs if b.get("landmark"))
    before_near = sum(1 for b in bs if b.get("landmark") and b["id"] in near)
    added = []
    for b in bs:
        t = raw.get(b.get("id"))
        if not t:
            continue
        ok, why = decide(t)
        if not ok:
            continue
        if not b.get("landmark"):
            added.append((b.get("name"), why))
        b["landmark"] = True
        b["landmark_src"] = why
    now = sum(1 for b in bs if b.get("landmark"))
    now_near = sum(1 for b in bs if b.get("landmark") and b["id"] in near)

    print("%s: %d footprints, landmark %d -> %d (+%d); "
          "WITHIN 100 m OF THE ROUTE %d of %d -> %d"
          % (a.place, len(bs), before, now, now - before,
             before_near, len(near), now_near))
    for name, why in added[:60]:
        print("  + %-14s %s" % (why, name or "(unnamed)"))
    if len(added) > 60:
        print("  ... and %d more" % (len(added) - 60))
    if a.dry_run:
        print("(dry run, BUILDINGS.JSON not written)")
        return 0
    json.dump(doc, open(bpath, "w", encoding="utf-8"), ensure_ascii=False)
    print("written " + bpath)
    return 0


if __name__ == "__main__":
    sys.exit(main())
