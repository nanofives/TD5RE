#!/usr/bin/env python3
"""
td5_car_studio.py -- local web GUI for authoring TD5RE custom cars.

Starts a localhost server and opens a browser "Car Studio": a real 3D viewport
(three.js) that loads YOUR glTF/OBJ, overlays the carparam wheel positions as
gizmos so you can place wheels visually, previews the skin on the body, lets you
edit every live physics/stat field, and builds the drop-in car with one click
(driving re/tools/td5_car_import.py). The produced folder
(re/assets/cars/custom_<name>/) is auto-enumerated by the engine — no rebuild.

Usage:
  python re/tools/td5_car_studio.py [--port 8765] [--cars-dir re/assets/cars]
                                    [--no-browser] [--revendor]

three.js: vendored into car_studio/vendor/ on first run (cached, offline after).
If the download fails (offline first run) it falls back to a CDN importmap, which
needs internet. Re-run with --revendor to refresh the local copy.
"""
import argparse
import base64
import io
import json
import os
import posixpath
import re
import sys
import threading
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))   # re/tools/car_studio/
STUDIO_DIR = HERE                                    # index.html / car_studio.js live here
VENDOR_DIR = os.path.join(HERE, "vendor")            # three.js downloaded here on first run
sys.path.insert(0, HERE)
import td5_car_import as importer  # noqa: E402  (the build engine, in this folder)
import td5_car_physics_ref as physics_ref  # noqa: E402  (fleet hints/ranges/presets)

THREE_VER = "0.160.0"
CDN = f"https://unpkg.com/three@{THREE_VER}"
ENTRY_ADDONS = ["loaders/GLTFLoader.js", "loaders/OBJLoader.js", "controls/OrbitControls.js",
                "exporters/GLTFExporter.js"]

# Filled from CLI in main().
CARS_DIR = "re/assets/cars"
USE_LOCAL_VENDOR = False


# ---------------------------------------------------------------------------
# three.js vendoring (download core + addon import closure; CDN fallback)
# ---------------------------------------------------------------------------
def _fetch(url, timeout=25):
    req = urllib.request.Request(url, headers={"User-Agent": "td5-car-studio"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def ensure_vendor(revendor=False):
    global USE_LOCAL_VENDOR
    three_path = os.path.join(VENDOR_DIR, "three.module.js")
    jsm_dir = os.path.join(VENDOR_DIR, "jsm")
    have_addons = all(os.path.isfile(os.path.join(jsm_dir, a)) for a in ENTRY_ADDONS)
    if not revendor and os.path.isfile(three_path) and have_addons:
        USE_LOCAL_VENDOR = True
        return True
    try:
        os.makedirs(jsm_dir, exist_ok=True)
        print(f"  vendoring three.js r{THREE_VER} -> {os.path.relpath(VENDOR_DIR)} ...")
        with open(three_path, "wb") as f:
            f.write(_fetch(f"{CDN}/build/three.module.js"))
        seen, queue, n = set(), list(ENTRY_ADDONS), 0
        while queue and n < 200:
            rel = queue.pop()
            if rel in seen:
                continue
            seen.add(rel)
            n += 1
            src = _fetch(f"{CDN}/examples/jsm/{rel}").decode("utf-8")
            dst = os.path.join(jsm_dir, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "w", encoding="utf-8") as f:
                f.write(src)
            # Follow this file's imports so the local copy is self-contained.
            for spec in re.findall(r"""from\s+['"]([^'"]+)['"]""", src):
                if spec == "three":
                    continue  # importmap resolves bare 'three'
                if spec.startswith("three/addons/"):
                    queue.append(spec[len("three/addons/"):])
                elif spec.startswith("./") or spec.startswith("../"):
                    queue.append(posixpath.normpath(posixpath.join(posixpath.dirname(rel), spec)))
        USE_LOCAL_VENDOR = True
        print(f"  vendored {n} addon file(s) + three core.")
        return True
    except Exception as e:
        print(f"  three.js download failed ({e}); using CDN importmap (needs internet).")
        USE_LOCAL_VENDOR = False
        return False


def importmap_json():
    if USE_LOCAL_VENDOR:
        return json.dumps({"imports": {
            "three": "./vendor/three.module.js",
            "three/addons/": "./vendor/jsm/"}})
    return json.dumps({"imports": {
        "three": f"{CDN}/build/three.module.js",
        "three/addons/": f"{CDN}/examples/jsm/"}})


# ---------------------------------------------------------------------------
# API helpers
# ---------------------------------------------------------------------------
def list_donors():
    out = []
    if not os.path.isdir(CARS_DIR):
        return out
    for code in sorted(os.listdir(CARS_DIR)):
        d = os.path.join(CARS_DIR, code)
        if not os.path.isdir(d):
            continue
        if not os.path.isfile(os.path.join(d, "carparam.json")):
            continue
        name = code
        nfo = os.path.join(d, "config.nfo")
        if os.path.isfile(nfo):
            try:
                first = open(nfo, encoding="utf-8", errors="ignore").readline().strip()
                if first:
                    name = first.replace("_", " ")
            except Exception:
                pass
        out.append({"code": code, "name": name, "custom": code.startswith("custom_")})
    return out


def load_carparam(donor):
    p = os.path.join(CARS_DIR, donor, "carparam.json")
    if not os.path.isfile(p):
        return None
    return json.load(open(p, encoding="utf-8"))


def _b64_to_bytes(s):
    if not s:
        return None
    if "," in s and s.strip().startswith("data:"):
        s = s.split(",", 1)[1]
    return base64.b64decode(s)


def do_build(req):
    """req = parsed JSON from POST /api/build. Returns (ok, result_dict)."""
    import contextlib
    import tempfile

    code = req.get("code") or ""
    if not code.startswith("custom_"):
        code = "custom_" + (code or "car")
    if "." in code:
        return False, {"error": "code must not contain '.'"}
    model_b64 = req.get("model_b64")
    if not model_b64:
        return False, {"error": "no model supplied"}

    tmp = tempfile.mkdtemp(prefix="td5studio_")
    try:
        model_name = req.get("model_name") or "model.glb"
        ext = os.path.splitext(model_name)[1].lower() or ".glb"
        if ext not in (".glb", ".gltf", ".obj"):
            ext = ".glb"
        model_path = os.path.join(tmp, "model" + ext)
        with open(model_path, "wb") as f:
            f.write(_b64_to_bytes(model_b64))

        skin_path = None
        if req.get("skin_b64"):
            skin_path = os.path.join(tmp, "skin.png")
            with open(skin_path, "wb") as f:
                f.write(_b64_to_bytes(req["skin_b64"]))

        carparam_path = None
        if isinstance(req.get("carparam"), dict):
            carparam_path = os.path.join(tmp, "carparam.json")
            with open(carparam_path, "w", encoding="utf-8") as f:
                json.dump(req["carparam"], f, indent=2)

        args = SimpleNamespace(
            model=model_path, name=req.get("name") or code, short=req.get("short"),
            code=code, out_dir=CARS_DIR, donor=req.get("donor") or "vip",
            skin=skin_path, carpic=None, carparam_json=carparam_path, stats=None,
            scale=req.get("scale") or "auto", up=req.get("up") or "y",
            forward=req.get("forward") or "-z", flip_v=bool(req.get("flip_v")),
            dry_run=False)

        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = importer.cmd_import(args)
        log = buf.getvalue()
        car_out = os.path.join(CARS_DIR, code)

        if rc != 0:
            return False, {"error": "import failed", "log": log}

        # Viewport-rendered showroom thumbnail overrides the placeholder.
        if req.get("carpic_b64"):
            png = _b64_to_bytes(req["carpic_b64"])
            for i in range(4):
                with open(os.path.join(car_out, f"carpic{i}.png"), "wb") as f:
                    f.write(png)

        # Validate the result.
        dbuf = io.StringIO()
        with contextlib.redirect_stdout(dbuf):
            importer.cmd_doctor(SimpleNamespace(car_dir=car_out))
        return True, {"ok": True, "code": code, "dir": car_out,
                      "log": log, "doctor": dbuf.getvalue()}
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# HTTP handler
# ---------------------------------------------------------------------------
class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass  # quiet

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body).encode("utf-8")
        elif isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, path, ctype):
        if not os.path.isfile(path):
            self._send(404, {"error": "not found"})
            return
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        q = {}
        if "?" in self.path:
            from urllib.parse import parse_qs
            q = {k: v[0] for k, v in parse_qs(self.path.split("?", 1)[1]).items()}

        if p in ("/", "/index.html"):
            html = open(os.path.join(STUDIO_DIR, "index.html"), encoding="utf-8").read()
            html = html.replace("__IMPORTMAP__", importmap_json())
            self._send(200, html, "text/html; charset=utf-8")
        elif p == "/car_studio.js":
            self._serve_file(os.path.join(STUDIO_DIR, "car_studio.js"),
                             "text/javascript; charset=utf-8")
        elif p.startswith("/vendor/"):
            safe = posixpath.normpath(p[len("/vendor/"):]).lstrip("/.")
            ctype = "text/javascript" if safe.endswith(".js") else "application/octet-stream"
            self._serve_file(os.path.join(VENDOR_DIR, *safe.split("/")), ctype)
        elif p == "/api/donors":
            self._send(200, list_donors())
        elif p == "/api/carparam":
            cp = load_carparam(q.get("donor", "vip"))
            self._send(200 if cp else 404, cp or {"error": "no carparam.json"})
        elif p == "/api/reference":
            # Fleet physics reference: effect hints + per-field min/median/max +
            # exemplar cars + archetype presets (td5_car_physics_ref).
            try:
                self._send(200, physics_ref.build_reference(CARS_DIR))
            except Exception as e:
                self._send(500, {"error": str(e)})
        else:
            self._send(404, {"error": "not found"})

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
                import traceback
                self._send(500, {"error": str(e), "trace": traceback.format_exc()})
        elif self.path == "/api/doctor":
            import contextlib
            d = os.path.join(CARS_DIR, req.get("code", ""))
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                importer.cmd_doctor(SimpleNamespace(car_dir=d))
            self._send(200, {"doctor": buf.getvalue()})
        else:
            self._send(404, {"error": "not found"})


def main():
    global CARS_DIR
    ap = argparse.ArgumentParser(description="TD5RE Car Studio (web GUI)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--cars-dir", default="re/assets/cars")
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--revendor", action="store_true", help="re-download three.js")
    args = ap.parse_args()
    CARS_DIR = args.cars_dir

    if not os.path.isdir(STUDIO_DIR):
        print(f"ERROR: {STUDIO_DIR} missing (car_studio/ UI files).", file=sys.stderr)
        return 2
    ensure_vendor(args.revendor)

    url = f"http://localhost:{args.port}/"
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"TD5 Car Studio -> {url}  (cars: {CARS_DIR})  Ctrl+C to stop")
    if not args.no_browser:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
