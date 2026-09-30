"""geo_selector.py -- the GEO TRACK map selector (Phase 4 of
docs/plans/GEO_TRACK_OSM_PLAN.md).

Serves a local page (re/tools/geo_selector/index.html, Leaflet over OSM tiles,
skinned to the TD5 frontend) and a small JSON API around the Phase 1 tools, so
a route is picked by clicking A and B and dragging waypoints instead of typing
coordinates into three scripts:

    python re/tools/geo_selector.py            # then open http://127.0.0.1:8765/

Run it from the repo root (the cache lives at re/assets/geo/, relative, same as
the game). It binds 127.0.0.1 ONLY: nothing outside this machine can reach it,
and no firewall rule is involved.

WHAT NEEDS THE NETWORK. Map tiles and place search are fetched by the BROWSER
(OSM tiles, Nominatim). FETCH AREA runs geo_fetch (Overpass + Terrarium, cached
on disk after the first time). Routing, conditioning and SEND TO GAME are local:
they read the cache and write files. So a place fetched once is editable offline
forever, map background aside.

API (all JSON):
    GET  /api/state                      places on disk + the game's selection
    POST /api/fetch  {name,lat,lon,radius}     geo_fetch a place (network)
    POST /api/route  {place, waypoints:[[lat,lon],...]}
         -> routed polyline + the conditioner's verdict, crossings as lat/lon
    POST /api/save   {place, waypoints}
         -> writes ROUTE_RAW.JSON + ROUTE.JSON, rebuilds the rasters in the
            route's frame (cached, no new downloads), selects it for the game

WHY THE SAVE REBUILDS THE RASTERS. The conditioner DECIDES the frame (rotation,
offset and origin at the route's own centroid) and the terrain must be in that
same frame or the road and the ground disagree with nothing raising an error
(geo_audit R8/R9). geo_fetch's --frame-from takes the rotation and offset but
keeps the place centre as the origin, so the save passes the ROUTE's origin
explicitly and widens the radius to still cover the original area.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from geo_common import (  # noqa: E402
    CACHE_ROOT,
    LocalProjection,
    TD5_TG_MAX_SPANS,
    place_dir,
    read_json,
    write_json,
)
import geo_condition  # noqa: E402
import geo_route  # noqa: E402

PAGE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "geo_selector")
SELECTED_NAME = "SELECTED.TXT"

# geo_fetch pulls in numpy/PIL; import lazily so the selector (and its
# self-test) still runs where only the routing half is needed.
_fetch_mod = None
_fetch_lock = threading.Lock()   # one Overpass job at a time, it rate-limits


def _geo_fetch():
    global _fetch_mod
    if _fetch_mod is None:
        import geo_fetch as m  # noqa: WPS433
        _fetch_mod = m
    return _fetch_mod


def _haversine_m(a: tuple[float, float], b: tuple[float, float]) -> float:
    la1, lo1, la2, lo2 = map(math.radians, (a[0], a[1], b[0], b[1]))
    h = (math.sin((la2 - la1) / 2) ** 2
         + math.cos(la1) * math.cos(la2) * math.sin((lo2 - lo1) / 2) ** 2)
    return 2 * 6371008.8 * math.asin(math.sqrt(h))


# ------------------------------------------------------------------ state ---

def list_places(root: str = CACHE_ROOT) -> list[dict]:
    out = []
    if not os.path.isdir(root):
        return out
    for slug in sorted(os.listdir(root)):
        d = os.path.join(root, slug)
        if slug.startswith(("_", ".")) or not os.path.isdir(d):
            continue
        pj = os.path.join(d, "PLACE.JSON")
        if not os.path.isfile(pj):
            continue
        p = read_json(pj)
        route = None
        rr = os.path.join(d, "ROUTE_RAW.JSON")
        if os.path.isfile(rr):
            raw = read_json(rr)
            route = {"waypoints": raw.get("waypoints", []),
                     "length_km": raw.get("length_km")}
        out.append({
            "slug": slug,
            "name": p.get("name", slug),
            "centre": p.get("centre"),
            "radius_m": p.get("radius_m"),
            "bbox": p.get("bbox"),
            "attribution": p.get("attribution", []),
            "has_route": os.path.isfile(os.path.join(d, "ROUTE.JSON")),
            "route": route,
        })
    return out


def read_selected(root: str = CACHE_ROOT) -> str:
    try:
        with open(os.path.join(root, SELECTED_NAME), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def write_selected(slug: str, root: str = CACHE_ROOT) -> None:
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, SELECTED_NAME), "w", encoding="utf-8") as f:
        f.write(slug)


# -------------------------------------------------------------- routing ---

_graph_cache: dict[str, tuple[float, tuple]] = {}


def _in_bbox(la: float, lo: float, bb: dict) -> bool:
    return bb["south"] <= la <= bb["north"] and bb["west"] <= lo <= bb["east"]


def _load_graph(slug: str, root: str):
    """The road graph is ~16k nodes; build it once per place and reuse it until
    ROADS.JSON changes (a save re-writes it in the new frame).

    ROUTING AREA IS PINNED. SEND TO GAME re-fetches a WIDER circle (the route's
    own origin plus the old radius, so the terrain covers everything). Routing
    over that wider graph found a different, shorter path for the same A/B
    (La Plata: 1492 -> 1200 spans), so touching a saved route silently changed
    it. The first save records the area the user actually routed in
    (PLACE.JSON "route_graph_bbox") and the graph is limited to roads with a
    point inside it from then on."""
    d = place_dir(slug, root)
    rp = os.path.join(d, "ROADS.JSON")
    mt = os.path.getmtime(rp)
    hit = _graph_cache.get(root + "|" + slug)
    if hit and hit[0] == mt:
        return hit[1]
    place = read_json(os.path.join(d, "PLACE.JSON"))
    roads = read_json(rp)["roads"]
    pr = place["projection"]
    proj = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
    proj.set_rotation(pr.get("rotation_rad", 0.0))
    proj.set_offset(pr.get("offset_x", 0.0), pr.get("offset_z", 0.0))
    bb = place.get("route_graph_bbox")
    if bb:
        keep = []
        for r in roads:
            for pt in r.get("points") or []:
                la, lo = proj.world_to_latlon(pt["x"], pt["z"])
                if _in_bbox(la, lo, bb):
                    keep.append(r)
                    break
        roads = keep
    loaded = (place, geo_route.RoadGraph(roads), proj)
    _graph_cache[root + "|" + slug] = (mt, loaded)
    return loaded


def route_and_condition(slug: str, waypoints: list[list[float]],
                        root: str = CACHE_ROOT) -> dict:
    if len(waypoints) < 2:
        return {"ok": False, "reasons": ["click A and B first"]}
    place, g, proj = _load_graph(slug, root)
    wps = [proj.to_world(float(la), float(lo)) for la, lo in waypoints]
    r = g.route(wps)
    if not r.get("ok"):
        return {"ok": False, "reasons": [r.get("reason", "routing failed")]}

    latlon = [proj.world_to_latlon(x, z) for x, z in r["points"]]
    cond = geo_condition.condition_route(latlon, r["lanes"])

    # Crossings are reported in the CONDITIONED frame; map them back to the map
    # so the page can put a marker where the user has to drag.
    marks = []
    if cond.get("projection") and cond.get("points"):
        pr = cond["projection"]
        cp = LocalProjection(pr["lat0"], pr["lon0"], pr["units_per_metre"])
        cp.set_rotation(pr.get("rotation_rad", 0.0))
        cp.set_offset(pr.get("offset_x", 0.0), pr.get("offset_z", 0.0))
        pts = cond["points"]
        for c in cond.get("crossings", []):
            a = pts[min(c.get("node_a", 0), len(pts) - 1)]
            la, lo = cp.world_to_latlon(a["x"], a["z"])
            marks.append({"lat": la, "lon": lo,
                          "span_gap": c.get("span_gap"),
                          "distance_m": (c.get("distance_units", 0.0)
                                         / pr["units_per_metre"])})
    upm = place["projection"]["units_per_metre"]
    return {
        "ok": bool(cond.get("ok")),
        "reasons": cond.get("reasons", []),
        "warnings": cond.get("warnings", []),
        "polyline": [[round(la, 7), round(lo, 7)] for la, lo in latlon],
        "street_names": r.get("street_names", []),
        "length_km": r["length_units"] / upm / 1000.0,
        "spans": cond.get("spans"),
        "span_cap": TD5_TG_MAX_SPANS,
        "direction": cond.get("direction"),
        "worst_turn_deg": (cond.get("curvature") or {}).get("worst_final_deg"),
        "monotone_pct": round(100.0 * (cond.get("final") or {}).get("monotone_frac", 0.0), 1),
        "crossings": marks,
        "_cond": cond,
        "_raw": {"latlon": latlon, "lanes": r["lanes"]},
    }


def save_route(slug: str, waypoints: list[list[float]], root: str = CACHE_ROOT,
               refetch: bool = True) -> dict:
    res = route_and_condition(slug, waypoints, root)
    if not res["ok"]:
        return {"ok": False, "reasons": res["reasons"] or ["route not usable"]}
    d = place_dir(slug, root)
    cond = res.pop("_cond")
    raw = res.pop("_raw")
    write_json(os.path.join(d, "ROUTE_RAW.JSON"), {
        "place": slug,
        "waypoints": waypoints,
        "length_km": res["length_km"],
        "street_names": res["street_names"],
        "points": [{"lat": round(la, 7), "lon": round(lo, 7), "lanes": ln}
                   for (la, lo), ln in zip(raw["latlon"], raw["lanes"])],
    })
    write_json(os.path.join(d, "ROUTE.JSON"), cond)

    if refetch:
        place = read_json(os.path.join(d, "PLACE.JSON"))
        # Pin the routing area to what the user routed in, the FIRST time only:
        # a later save must not widen it again (see _load_graph).
        graph_bbox = place.get("route_graph_bbox") or place.get("bbox")
        pr = cond["projection"]
        centre = (place["centre"]["lat"], place["centre"]["lon"])
        origin = (pr["lat0"], pr["lon0"])
        radius = float(place["radius_m"]) + _haversine_m(centre, origin) + 200.0
        with _fetch_lock:
            _geo_fetch().fetch_place(place["name"], origin[0], origin[1], radius,
                                     root, pr["units_per_metre"], None,
                                     rotation_rad=pr["rotation_rad"],
                                     offset_x=pr["offset_x"],
                                     offset_z=pr["offset_z"])
        if graph_bbox:
            pj = os.path.join(d, "PLACE.JSON")
            newp = read_json(pj)
            newp["route_graph_bbox"] = graph_bbox
            write_json(pj, newp)
        _graph_cache.pop(root + "|" + slug, None)

    write_selected(slug, root)
    return {"ok": True, "spans": cond["spans"], "length_km": res["length_km"],
            "selected": slug}


def fetch_area(name: str, lat: float, lon: float, radius: float,
               root: str = CACHE_ROOT) -> dict:
    if not name.strip():
        return {"ok": False, "reasons": ["give the place a name"]}
    radius = max(500.0, min(float(radius), 6000.0))   # Overpass-friendly box
    with _fetch_lock:
        p = _geo_fetch().fetch_place(name.strip(), float(lat), float(lon), radius, root)
    return {"ok": True, "slug": p["slug"], "name": p["name"]}


# ------------------------------------------------------------------ http ---

def handle_api(method: str, path: str, body: dict, root: str = CACHE_ROOT) -> tuple[int, dict]:
    """Pure dispatch, so the API is testable without opening a socket."""
    try:
        if method == "GET" and path == "/api/state":
            return 200, {"places": list_places(root), "selected": read_selected(root)}
        if method == "POST" and path == "/api/route":
            r = route_and_condition(body["place"], body.get("waypoints", []), root)
            r.pop("_cond", None)
            r.pop("_raw", None)
            return 200, r
        if method == "POST" and path == "/api/save":
            return 200, save_route(body["place"], body.get("waypoints", []), root)
        if method == "POST" and path == "/api/fetch":
            return 200, fetch_area(body.get("name", ""), body["lat"], body["lon"],
                                   body.get("radius", 2200), root)
        if method == "POST" and path == "/api/select":
            write_selected(body.get("place", ""), root)
            return 200, {"ok": True, "selected": body.get("place", "")}
        return 404, {"ok": False, "reasons": ["no such endpoint"]}
    except Exception as e:  # surfaced in the page, never a silent hang
        return 500, {"ok": False, "reasons": ["%s: %s" % (type(e).__name__, e)]}


class _Handler(BaseHTTPRequestHandler):
    root = CACHE_ROOT

    def _send(self, code: int, data: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code: int, obj: dict) -> None:
        self._send(code, json.dumps(obj).encode("utf-8"), "application/json")

    def do_GET(self):  # noqa: N802
        if self.path.startswith("/api/"):
            self._json(*handle_api("GET", self.path.split("?")[0], {}, self.root))
            return
        name = "index.html" if self.path in ("/", "/index.html") else None
        if not name:
            self._send(404, b"not found", "text/plain")
            return
        with open(os.path.join(PAGE_DIR, name), "rb") as f:
            self._send(200, f.read(), "text/html; charset=utf-8")

    def do_POST(self):  # noqa: N802
        n = int(self.headers.get("Content-Length") or 0)
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            self._json(400, {"ok": False, "reasons": ["bad JSON"]})
            return
        self._json(*handle_api("POST", self.path, body, self.root))

    def log_message(self, fmt, *args):  # quieter console
        sys.stderr.write("[selector] " + (fmt % args) + "\n")


# ------------------------------------------------------------- self-test ---

def _self_test() -> int:
    """Offline: a synthetic grid 'place' in a temp dir, routed, conditioned and
    saved (no re-fetch). Proves the API end to end without network or sockets."""
    tmp = tempfile.mkdtemp(prefix="geo_selector_")
    slug = "testgrid"
    d = place_dir(slug, tmp)
    os.makedirs(d)
    lat0, lon0, upm = -34.92, -57.95, 430.0
    proj = LocalProjection(lat0, lon0, upm)
    # 12x12 blocks of 110 m; avenues every 4th line are 4 lanes and primary.
    roads, rid, n, block = [], 1, 12, 110.0
    for k in range(n + 1):
        for horiz in (True, False):
            pts = []
            for s in range(n + 1):
                e = (s if horiz else k) * block - n * block / 2
                no = (k if horiz else s) * block - n * block / 2
                la, lo = proj.to_degrees(e, no)
                x, z = proj.to_world(la, lo)
                pts.append({"x": x, "z": z})
            av = (k % 4 == 0)
            roads.append({"id": rid, "name": "Calle %d" % rid,
                          "class": "primary" if av else "residential",
                          "lanes": 4 if av else 2, "oneway": False,
                          "points": pts})
            rid += 1
    write_json(os.path.join(d, "ROADS.JSON"), {"roads": roads})
    write_json(os.path.join(d, "PLACE.JSON"), {
        "name": "Test Grid", "slug": slug, "centre": {"lat": lat0, "lon": lon0},
        "radius_m": 900, "projection": proj.describe()})

    def at(e, no):
        return list(proj.to_degrees(e, no))

    fails = 0
    code, st = handle_api("GET", "/api/state", {}, tmp)
    ok = code == 200 and st["places"] and st["places"][0]["slug"] == slug
    print("state        :", "OK" if ok else "FAIL", len(st.get("places", [])), "place(s)")
    fails += not ok

    wps = [at(-600, -600), at(600, -200), at(-200, 600)]
    code, r = handle_api("POST", "/api/route", {"place": slug, "waypoints": wps}, tmp)
    ok = code == 200 and r.get("polyline") and r.get("spans")
    print("route        :", "OK" if ok else "FAIL",
          "%s spans, %.2f km, verdict %s, reasons %s"
          % (r.get("spans"), r.get("length_km", 0), r.get("ok"), r.get("reasons")))
    fails += not ok

    # Out and back down the same avenue: the conditioner must flag it.
    code, rb = handle_api("POST", "/api/route",
                          {"place": slug, "waypoints": [at(-660, 0), at(660, 0), at(-500, 0)]}, tmp)
    ok = code == 200 and not rb.get("ok") and rb.get("crossings")
    print("retrace flag :", "OK" if ok else "FAIL",
          "%d crossing marker(s), verdict %s" % (len(rb.get("crossings", [])), rb.get("ok")))
    fails += not ok

    if r.get("ok"):
        s = save_route(slug, wps, tmp, refetch=False)
        routed = read_json(os.path.join(d, "ROUTE.JSON"))
        ok = (s.get("ok") and read_selected(tmp) == slug
              and routed["points"][0]["x"] == 0 and routed["points"][0]["z"] == 0)
        print("save         :", "OK" if ok else "FAIL", s)
        fails += not ok
    else:
        print("save         : SKIPPED (route verdict not OK:", r.get("reasons"), ")")

    code, bad = handle_api("POST", "/api/nope", {}, tmp)
    ok = code == 404
    print("unknown path :", "OK" if ok else "FAIL")
    fails += not ok
    print("RESULT:", "OK" if not fails else "%d FAIL" % fails, "(temp dir %s)" % tmp)
    return 1 if fails else 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--root", default=CACHE_ROOT)
    ap.add_argument("--self-test", action="store_true",
                    help="offline API test on a synthetic grid; opens no socket")
    a = ap.parse_args(argv)
    if a.self_test:
        return _self_test()
    _Handler.root = a.root
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), _Handler)
    print("GEO TRACK selector on http://127.0.0.1:%d/  (cache %s)" % (a.port, a.root))
    print("Ctrl+C to stop.")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
