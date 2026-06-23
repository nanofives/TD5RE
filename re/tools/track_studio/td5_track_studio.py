#!/usr/bin/env python3
"""TD5RE Track Studio -- web GUI to import and edit custom tracks.

Sibling of the Car Studio (re/tools/car_studio/). A self-contained stdlib HTTP
server + three.js viewport for authoring the neutral centerline spec that
td5_trackgen.py turns into a drivable TD5 level:

  - import an existing track (any levelNNN/) back into an editable centerline,
  - or start from a built-in sample / blank,
  - drag/add/delete centerline nodes, set per-node lanes/width/surface,
  - add branches and checkpoints, set params (circuit/P2P, weather, fog, traffic),
  - Build -> writes re/assets/levels/levelNNN/ + registers it in custom_tracks.json
    (via td5_trackgen.build_track), so it's drivable with no recompile.

Run:  python re/tools/track_studio/td5_track_studio.py [--port 8766] [--no-browser]
"""

import argparse
import importlib.util
import io
import json
import os
import posixpath
import re
import sys
import threading
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

STUDIO_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(STUDIO_DIR)                       # re/tools
REPO_ROOT = os.path.dirname(os.path.dirname(TOOLS_DIR))       # .../TD5RE
VENDOR_DIR = os.path.join(STUDIO_DIR, "vendor")

ASSETS_DIR = os.path.join(REPO_ROOT, "re", "assets")
THREE_VER = "0.160.0"
CDN = f"https://unpkg.com/three@{THREE_VER}"
ENTRY_ADDONS = ["controls/OrbitControls.js", "loaders/GLTFLoader.js"]  # GLTFLoader for env geometry
USE_LOCAL_VENDOR = False


# --------------------------------------------------------------------------
# Load the converter module (re/tools/td5_trackgen.py) -- the build engine.
# --------------------------------------------------------------------------
def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


trackgen = _load_module("td5_trackgen", os.path.join(TOOLS_DIR, "td5_trackgen.py"))
try:
    mesh_tool = _load_module("mesh_tool", os.path.join(TOOLS_DIR, "mesh_tool.py"))  # needs numpy
except Exception as _e:  # noqa
    mesh_tool = None
_glb_cache = {}   # level -> glb bytes (env geometry; built once)


# --------------------------------------------------------------------------
# three.js vendoring (mirrors car_studio: download on first run, CDN fallback).
# --------------------------------------------------------------------------
def _fetch(url):
    import urllib.request
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read()


def ensure_vendor(revendor=False):
    global USE_LOCAL_VENDOR
    three_path = os.path.join(VENDOR_DIR, "three.module.js")
    jsm_dir = os.path.join(VENDOR_DIR, "jsm")
    have = all(os.path.isfile(os.path.join(jsm_dir, a)) for a in ENTRY_ADDONS)
    if not revendor and os.path.isfile(three_path) and have:
        USE_LOCAL_VENDOR = True
        return True
    try:
        os.makedirs(jsm_dir, exist_ok=True)
        print(f"  vendoring three.js r{THREE_VER} -> {os.path.relpath(VENDOR_DIR)} ...")
        with open(three_path, "wb") as f:
            f.write(_fetch(f"{CDN}/build/three.module.js"))
        seen, queue, n = set(), list(ENTRY_ADDONS), 0
        while queue and n < 100:
            rel = queue.pop()
            if rel in seen:
                continue
            seen.add(rel); n += 1
            src = _fetch(f"{CDN}/examples/jsm/{rel}").decode("utf-8")
            dst = os.path.join(jsm_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="utf-8") as f:
                f.write(src)
            for ref in re.findall(r"""from\s+['"]([^'"]+)['"]""", src):
                if ref == "three":
                    continue
                if ref.startswith("three/addons/"):
                    queue.append(ref[len("three/addons/"):])
                elif ref.startswith("./") or ref.startswith("../"):
                    queue.append(posixpath.normpath(posixpath.join(posixpath.dirname(rel), ref)))
        USE_LOCAL_VENDOR = True
        print(f"  vendored {n} addon file(s) + three core.")
        return True
    except Exception as e:
        print(f"  three.js download failed ({e}); using CDN importmap (needs internet).")
        USE_LOCAL_VENDOR = False
        return False


def importmap_json():
    if USE_LOCAL_VENDOR:
        return json.dumps({"imports": {"three": "./vendor/three.module.js",
                                       "three/addons/": "./vendor/jsm/"}})
    return json.dumps({"imports": {"three": f"{CDN}/build/three.module.js",
                                   "three/addons/": f"{CDN}/examples/jsm/"}})


# --------------------------------------------------------------------------
# API helpers
# --------------------------------------------------------------------------
def _levels_dir():
    return trackgen.levels_dir(ASSETS_DIR)


# level number -> display name, mirroring the engine tables
# (td5_frontend.c s_track_display_names + the schedule maps; td5_asset.c
# k_td6_menu_slots). Lets the import list show "MOSCOW, RUSSIA" not "LEVEL 23".
def _level_names():
    names = ["DRAG STRIP", "MONTEGO BAY, JAMAICA", "HOUSE OF BEZ, ENGLAND",
             "NEWCASTLE, ENGLAND", "MAUI, HAWAII, USA", "COURMAYEUR, ITALY",
             "JARASH, JORDAN", "CHEDDAR CHEESE, ENGLAND", "MOSCOW, RUSSIA",
             "BLUE RIDGE PARKWAY, NC, USA", "EDINBURGH, SCOTLAND", "TOKYO, JAPAN",
             "SYDNEY, AUSTRALIA", "HONOLULU, HAWAII, USA", "MUNICH, GERMANY",
             "WASHINGTON, DC, USA", "KYOTO, JAPAN", "BERN, SWITZERLAND",
             "SAN FRANCISCO, CA, USA", "KESWICK, ENGLAND",
             "CHAMPIONSHIP CUP", "ERA CUP", "CHALLENGE CUP", "PITBULL CUP",
             "MASTERS CUP", "ULTIMATE CUP",
             "PELTON RACEWAY", "IRELAND", "LAKE TAHOE, USA", "CAPE HATTERAS, USA",
             "SWITZERLAND", "EGYPT", "PARIS, FRANCE", "NEW YORK, USA", "ROME, ITALY",
             "HONG KONG, CHINA", "LONDON, ENGLAND"]
    sched_name = [8, 10, 12, 9, 6, 3, 4, 5, 13, 11, 19, 18, 17, 16, 15, 14, 7, 1, 2]
    sched_pool = [11, 9, 7, 10, 13, 16, 15, 14, 6, 8, 0, 1, 2, 3, 4, 5, 12, 18, 17]
    pool_zip = [1, 2, 3, 4, 5, 6, 13, 14, 15, 16, 17, 23, 25, 26, 27, 28, 29, 37, 39]
    m = {}
    for s in range(19):
        m[pool_zip[sched_pool[s]]] = names[sched_name[s]]
    for lvl, ni in {7: 26, 18: 27, 19: 28, 20: 29, 21: 30, 22: 31,
                    8: 32, 9: 33, 10: 34, 11: 35, 12: 36}.items():
        m[lvl] = names[ni]
    m[30] = "DRAG STRIP"
    return m


def list_tracks():
    """Custom tracks (from the manifest) + every levelNNN/ available to import."""
    manifest = trackgen.load_manifest(ASSETS_DIR)
    custom_by_level = {int(t["level"]): t for t in manifest.get("tracks", [])}
    names = _level_names()
    levels = []
    ld = _levels_dir()
    if os.path.isdir(ld):
        for name in sorted(os.listdir(ld)):
            if not (name.startswith("level") and name[5:8].isdigit()):
                continue
            num = int(name[5:8])
            strip = os.path.join(ld, name, "strip.json")
            if not os.path.isfile(strip):
                continue
            entry = custom_by_level.get(num)
            disp = entry["name"] if entry else names.get(num, "LEVEL %d" % num)
            levels.append({"level": num, "name": disp, "custom": bool(entry),
                           "slot": entry["slot"] if entry else None})
    return {"custom": manifest.get("tracks", []), "levels": levels,
            "slot_base": trackgen.CUSTOM_SLOT_BASE}


def do_import(level, name=None):
    level = int(level)
    manifest = trackgen.load_manifest(ASSETS_DIR)
    custom = next((t for t in manifest.get("tracks", []) if int(t["level"]) == level), None)
    if not name:
        name = (custom["name"] if custom else None) or _level_names().get(level)
    spec, warnings = trackgen.extract_track(ASSETS_DIR, level, name=name, decimate_to=110)
    if custom:                       # so a rebuild overwrites the same slot/level
        spec["_slot"] = int(custom["slot"]); spec["_level"] = level
    return {"ok": True, "spec": spec, "warnings": warnings}


def _level_dir(level):
    return os.path.join(_levels_dir(), "level%03d" % int(level))


def list_assets(level):
    """Texture pages, skybox images, and whether env geometry exists for a level
    -- backs the 'from selected track' loaders."""
    ld = _level_dir(level)
    texdir = os.path.join(ld, "textures")
    textures = sorted(f for f in os.listdir(texdir)
                      if f.lower().endswith((".png", ".tga"))) if os.path.isdir(texdir) else []
    sky = [f for f in ("forwsky.png", "backsky.png") if os.path.isfile(os.path.join(ld, f))]
    return {"textures": textures, "skybox": sky,
            "has_models": os.path.isfile(os.path.join(ld, "models.bin")) and mesh_tool is not None}


def serve_asset(level, name):
    """(bytes, content_type) for a level texture/skybox PNG, or None."""
    safe = os.path.basename(name or "")
    ld = _level_dir(level)
    for cand in (os.path.join(ld, "textures", safe), os.path.join(ld, safe)):
        if os.path.isfile(cand):
            ct = "image/png" if safe.lower().endswith(".png") else "application/octet-stream"
            with open(cand, "rb") as f:
                return f.read(), ct
    return None


def build_model_glb(level):
    """Decode the level's models.bin (env geometry) and export a GLB (cached)."""
    if mesh_tool is None:
        raise RuntimeError("mesh_tool/numpy unavailable")
    level = int(level)
    if level in _glb_cache:
        return _glb_cache[level]
    path = os.path.join(_level_dir(level), "models.bin")
    if not os.path.isfile(path):
        raise FileNotFoundError("no models.bin for level %d" % level)
    with open(path, "rb") as f:
        glb = mesh_tool.export_glb(mesh_tool.decode(f.read(), "models"))
    _glb_cache[level] = glb
    return glb


def do_sample(kind):
    spec = trackgen.sample_spec(kind)
    # return the editable raw form (nodes carry resolved width/lanes already)
    return {"ok": True, "spec": spec, "warnings": []}


def do_build(req):
    spec = req.get("spec")
    if not isinstance(spec, dict):
        return False, {"error": "missing 'spec'"}
    slot = req.get("slot", spec.get("_slot"))
    level = req.get("level", spec.get("_level"))
    buf = io.StringIO()
    import contextlib
    with contextlib.redirect_stderr(buf):
        res = trackgen.build_track(spec, assets_root=ASSETS_DIR, slot=slot, level=level)
    res["log"] = buf.getvalue()
    res["drive"] = ("td5re.exe --AutoRace=1 --SkipIntro=1 --DefaultTrack=%d"
                    % res["slot"])
    return True, res


# --------------------------------------------------------------------------
# HTTP server
# --------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if not isinstance(body, (bytes, bytearray)):
            if ctype.startswith("application/json"):
                body = json.dumps(body).encode("utf-8")
            else:
                body = str(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, path, ctype):
        if not os.path.isfile(path):
            self._send(404, {"error": "not found: %s" % os.path.basename(path)})
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        q = {}
        if "?" in self.path:
            from urllib.parse import parse_qs
            q = {k: v[0] for k, v in parse_qs(self.path.split("?", 1)[1]).items()}
        try:
            if p in ("/", "/index.html"):
                html = open(os.path.join(STUDIO_DIR, "index.html"), encoding="utf-8").read()
                html = html.replace("__IMPORTMAP__", importmap_json())
                self._send(200, html, "text/html; charset=utf-8")
            elif p == "/track_studio.js":
                self._serve_file(os.path.join(STUDIO_DIR, "track_studio.js"),
                                 "text/javascript; charset=utf-8")
            elif p.startswith("/vendor/"):
                safe = posixpath.normpath(p[len("/vendor/"):]).lstrip("/.")
                ctype = "text/javascript" if safe.endswith(".js") else "application/octet-stream"
                self._serve_file(os.path.join(VENDOR_DIR, *safe.split("/")), ctype)
            elif p == "/api/tracks":
                self._send(200, list_tracks())
            elif p == "/api/import":
                self._send(200, do_import(q.get("level"), q.get("name")))
            elif p == "/api/sample":
                self._send(200, do_sample(q.get("kind", "oval")))
            elif p == "/api/assets":
                self._send(200, list_assets(q.get("level")))
            elif p == "/api/asset":
                a = serve_asset(q.get("level"), q.get("name"))
                if a:
                    self._send(200, a[0], a[1])
                else:
                    self._send(404, {"error": "asset not found"})
            elif p == "/api/model":
                self._send(200, build_model_glb(q.get("level")), "model/gltf-binary")
            else:
                self._send(404, {"error": "not found"})
        except Exception as e:
            self._send(500, {"error": str(e), "trace": traceback.format_exc()})

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n) if n else b"{}"
        try:
            req = json.loads(raw.decode("utf-8"))
        except Exception as e:
            self._send(400, {"error": f"bad JSON: {e}"})
            return
        if self.path == "/api/build":
            try:
                ok, res = do_build(req)
                self._send(200 if ok else 400, res)
            except Exception as e:
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        else:
            self._send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description="TD5RE Track Studio (web GUI)")
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--revendor", action="store_true", help="re-download three.js")
    args = ap.parse_args()

    if not os.path.isdir(STUDIO_DIR):
        print(f"ERROR: {STUDIO_DIR} missing.", file=sys.stderr)
        return 2
    ensure_vendor(args.revendor)

    url = f"http://localhost:{args.port}/"
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"TD5 Track Studio -> {url}  (assets: {os.path.relpath(ASSETS_DIR)})  Ctrl+C to stop")
    if not args.no_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
