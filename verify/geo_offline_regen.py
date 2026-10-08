#!/usr/bin/env python3
"""Rebuild a geo place from its own _cache with the network HARD-BLOCKED.

Round 1008. The round-1007 L1 re-fetch (the OSM tag round) was lost when
td5_geo_route_commit corrupted the place cache in place, but the 263 HTTP
fragments under <place>/_cache/ survived, and L1 measured a full rebuild at
12 cache hits / 0 misses. So the data can be regenerated with no Overpass
request, no terrarium tile and no COG range -- which this script PROVES rather
than asserts, by replacing urllib.request.urlopen with a counter that raises.

Both geo_fetch and geo_cog reach the network through `urllib.request.urlopen`,
so one patch covers every path. A single call is a FAILURE: it means something
was not in the cache and the rebuild is not the offline rebuild it claims to be.

Usage (from the repo root):

    python verify/geo_offline_regen.py --place-json <PLACE.JSON to copy params
                                         from> --frame-from <ROUTE.JSON>

Every fetch parameter is read from the reference PLACE.JSON rather than typed
in, so the rebuild cannot silently differ from the place it is reproducing.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.request


class NetworkBlocked(RuntimeError):
    pass


_ATTEMPTS: list[str] = []


def _blocked_urlopen(req, *a, **kw):
    url = getattr(req, "full_url", None) or str(req)
    _ATTEMPTS.append(url)
    raise NetworkBlocked("OFFLINE REBUILD: outbound request refused -> %s" % url)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--place-json", required=True,
                    help="reference PLACE.JSON; every fetch parameter is read "
                         "from it")
    ap.add_argument("--frame-from", required=True,
                    help="conditioned ROUTE.JSON whose frame the rebuild uses")
    ap.add_argument("--root", default=os.path.join("re", "assets", "geo"))
    ap.add_argument("--levels-estimator", choices=("v1", "v2"), default="v2")
    a = ap.parse_args(argv)

    with open(a.place_json, encoding="utf-8") as f:
        ref = json.load(f)

    name = ref["name"]
    lat = ref["centre"]["lat"]
    lon = ref["centre"]["lon"]
    radius = ref["radius_m"]
    upm = ref["projection"]["units_per_metre"]
    smooth = ref["layers"]["height"].get("lowpass_m", 200.0)
    src = ref.get("sources", {})
    land_cover = src.get("land_cover", "osm")
    canopy = bool(src.get("canopy", False))

    print("offline rebuild of %r" % name)
    print("  lat/lon        %.14f, %.14f" % (lat, lon))
    print("  radius_m       %.6f" % radius)
    print("  units/metre    %.1f" % upm)
    print("  dem-smooth-m   %.1f" % smooth)
    print("  land-cover     %s" % land_cover)
    print("  canopy         %s" % ("on" if canopy else "off"))
    print("  levels-est     %s" % a.levels_estimator)
    print("  frame-from     %s" % a.frame_from)
    print("  root           %s" % a.root)

    # The block goes on BEFORE geo_fetch is imported, so nothing can capture a
    # reference to the real urlopen at import time and route around it.
    urllib.request.urlopen = _blocked_urlopen

    sys.path.insert(0, os.path.join("re", "tools"))
    import geo_fetch                                     # noqa: E402

    argv2 = [
        "--name", name,
        "--lat", repr(lat),
        "--lon", repr(lon),
        "--radius", repr(radius),
        "--root", a.root,
        "--units-per-metre", repr(upm),
        "--dem-smooth-m", repr(smooth),
        "--frame-from", a.frame_from,
        "--land-cover", land_cover,
        "--canopy", "on" if canopy else "off",
        "--levels-estimator", a.levels_estimator,
        # The IGN WFS coverage probe is NOT cached and its answer is not stored,
        # so on a rebuild it is one outbound request for nothing -- and with the
        # block on, one fatal one.
        "--skip-dem-probe",
    ]
    print("\ngeo_fetch %s\n" % " ".join(argv2))

    t0 = time.time()
    try:
        rc = geo_fetch.main(argv2)
    except NetworkBlocked as exc:
        print("\nFAILED: %s" % exc)
        print("attempts: %d" % len(_ATTEMPTS))
        for u in _ATTEMPTS:
            print("  %s" % u)
        return 2
    dt = time.time() - t0

    print("\n=== OFFLINE REBUILD RESULT ===")
    print("geo_fetch rc      %s" % rc)
    print("elapsed           %.1f s" % dt)
    print("network requests  %d   <-- must be 0" % len(_ATTEMPTS))
    for u in _ATTEMPTS:
        print("  attempted: %s" % u)
    return 0 if (rc == 0 and not _ATTEMPTS) else 1


if __name__ == "__main__":
    sys.exit(main())
