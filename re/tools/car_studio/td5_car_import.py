#!/usr/bin/env python3
"""
td5_car_import.py -- import a custom car (any glTF/OBJ model + textures) into a
drop-in TD5RE car the engine auto-enumerates in the SELECT CAR screen.

This is the AUTHORING side of the custom-car pipeline. The engine side
(td5_customcar.c) scans re/assets/cars/custom_*/ at startup and registers every
folder this tool produces as an extra car slot (index 76+), so a converted car
appears in-game with NO source edit.

What it writes into  re/assets/cars/<code>/  (the loose editable-tree the port's
car loader resolves -- td5_asset.c build_extracted_asset_path):

  himodel.bin    the car body MESH, in TD5 native PRR format (render_type 0x103,
                 de-indexed float triangles, page-7 body). The loader maps
                 himodel.dat -> himodel.bin (td5_assetsrc passthrough) and uses
                 it as-is (no TD6 transcode). Built via mesh_tool._pack_mesh.
  carparam.json  physics (wheels, suspension, torque, gears, mass). Copied from a
                 DONOR car; the assetsrc layer re-encodes it to the 268-byte
                 carparam.dat at load. Wheels stay sane because the mesh is
                 scaled to the donor's length.
  carskin0..3.png  body texture (your image, normalized to RGBA PNG). The engine
                 decodes PNG -> BGRA itself, so "engine-readable texture" just
                 means a correctly-sized RGBA PNG -- which is what `texture` and
                 the --skin path produce.
  carhub0..3.png   wheel-hub sprite sheet (64x64 2x2), copied from the donor --
                 wheels render as sprites positioned by carparam, NOT mesh.
  carpic0..3.png   SELECT-CAR showroom thumbnail (placeholder built from the skin).
  config.nfo     17-line spec sheet: line0 display name, line1 short name, then
                 stats. Spaces -> '_' (the frontend converts back).
  drive/rev/horn/reverb.wav  engine audio, copied from the donor.

Mesh format (locked from re/tools/mesh_tool.py + a native-car survey):
  * world units: a TD5 car is ~1500 long (Z) x ~650 wide (X) x ~350 tall (Y),
    origin at the geometric centre, Y-up. We centre + uniform-scale the import
    to a target length (default 1500) so it sits right and the donor wheels fit.
  * himodel: header(0x38) + commands(0x10) + vertices(0x2C) + normals(0x10).
    One command per <=65535 triangles, dispatch_type 0 (independent tris),
    texture_page_id 7 (-> carskin at load), light 0xFFFFFFFF, normal vis 0.

Usage:
  # Import a model into a drop-in custom car:
  python re/tools/td5_car_import.py import \
      --model mycar.glb --name "My Cool Car" [--skin body.png] \
      [--code custom_mycar] [--donor vip] [--out-dir re/assets/cars] \
      [--scale auto|FLOAT] [--forward +z|-z|+x|-x] [--up y|z] [--flip-v]

  # Just convert ANY image into an engine-readable texture PNG:
  python re/tools/td5_car_import.py texture in.jpg out.png \
      [--size 256] [--square] [--colorkey none|black|blue88|red|cyan]

  # Validate a produced car (round-trips himodel.bin through mesh_tool):
  python re/tools/td5_car_import.py verify re/assets/cars/custom_mycar
"""
import argparse
import base64
import json
import os
import shutil
import struct
import sys

import numpy as np

# mesh_tool owns the exact PRR byte layout (_pack_mesh / decode / model_equal).
# It is the shared RE codec and lives one level up in re/tools/; add both this
# folder and the parent so `import mesh_tool` resolves whether this file is run
# from re/tools/car_studio/ (its home) or copied elsewhere alongside mesh_tool.
_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)
sys.path.insert(0, os.path.dirname(_HERE))   # re/tools/ (shared mesh_tool codec)
import mesh_tool  # noqa: E402

try:
    from PIL import Image
except ImportError:
    Image = None

# ---------------------------------------------------------------------------
# Native-car reference constants (surveyed from re/assets/cars/*/himodel.bin)
# ---------------------------------------------------------------------------
TARGET_LENGTH = 1500.0      # world units along the length axis (Z) after scale
MAX_TRIS_PER_CMD = 65535    # triangle_count is uint16 in TD5_PrimitiveCmd
BODY_PAGE = 7               # himodel cmd texture_page_id the loader maps -> carskin
LIGHT_FULL = 0xFFFFFFFF     # per-vertex lighting slot on native cars
RENDER_TYPE_NATIVE = 0x103  # TD5 expanded mesh, used as-is (0x104 = TD6, transcoded)


# ===========================================================================
# Model readers: glTF/GLB and OBJ -> (positions[N,3], uvs[N,2]|None,
#                                       normals[N,3]|None, tri_indices[M,3])
# Everything is merged into ONE indexed mesh in model-local space; node/world
# transforms are baked. Only triangle primitives are kept.
# ===========================================================================

def _load_obj(path):
    """Minimal OBJ reader. Triangulates n-gon faces (fan). Builds a unique
    vertex per (v,vt,vn) corner combo so UVs/normals stay per-corner."""
    positions, texcoords, normals = [], [], []
    verts, uvs, norms, tris = [], [], [], []
    combo_cache = {}

    def emit(tok):
        # OBJ face token "v/vt/vn" (vt/vn optional), 1-based, negatives allowed.
        key = tok
        idx = combo_cache.get(key)
        if idx is not None:
            return idx
        parts = (tok.split("/") + ["", ""])[:3]
        vi = int(parts[0])
        vi = vi - 1 if vi > 0 else len(positions) + vi
        verts.append(positions[vi])
        if parts[1]:
            ti = int(parts[1]); ti = ti - 1 if ti > 0 else len(texcoords) + ti
            uvs.append(texcoords[ti])
        else:
            uvs.append((0.0, 0.0))
        if parts[2]:
            ni = int(parts[2]); ni = ni - 1 if ni > 0 else len(normals) + ni
            norms.append(normals[ni])
        else:
            norms.append(None)
        idx = len(verts) - 1
        combo_cache[key] = idx
        return idx

    have_uv = have_norm = False
    with open(path, "r", errors="ignore") as f:
        for line in f:
            if line.startswith("v "):
                positions.append(tuple(float(x) for x in line.split()[1:4]))
            elif line.startswith("vt "):
                c = [float(x) for x in line.split()[1:3]]
                texcoords.append((c[0], c[1] if len(c) > 1 else 0.0)); have_uv = True
            elif line.startswith("vn "):
                normals.append(tuple(float(x) for x in line.split()[1:4])); have_norm = True
            elif line.startswith("f "):
                toks = line.split()[1:]
                corners = [emit(t) for t in toks]
                for k in range(1, len(corners) - 1):       # fan triangulate
                    tris.append((corners[0], corners[k], corners[k + 1]))
    P = np.array(verts, np.float32).reshape(-1, 3)
    UV = np.array(uvs, np.float32).reshape(-1, 2) if have_uv else None
    if have_norm and all(n is not None for n in norms):
        N = np.array(norms, np.float32).reshape(-1, 3)
    else:
        N = None
    T = np.array(tris, np.uint32).reshape(-1, 3)
    return P, UV, N, T


# --- glTF / GLB ------------------------------------------------------------
_GLB_MAGIC = 0x46546C67
_CT = {5120: ("b", 1), 5121: ("B", 1), 5122: ("h", 2), 5123: ("H", 2),
       5125: ("I", 4), 5126: ("f", 4)}
_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def _glb_chunks(data):
    magic, _ver, _total = struct.unpack_from("<III", data, 0)
    if magic != _GLB_MAGIC:
        return None, None
    off, js, bin_blob = 12, None, b""
    while off < len(data):
        clen, ctype = struct.unpack_from("<II", data, off); off += 8
        chunk = data[off:off + clen]; off += clen
        if ctype == 0x4E4F534A:      # JSON
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == 0x004E4942:    # BIN
            bin_blob = bytes(chunk)
    return js, bin_blob


def _gltf_buffers(gltf, base_dir, glb_bin):
    out = []
    for b in gltf.get("buffers", []):
        uri = b.get("uri")
        if uri is None:
            out.append(glb_bin)
        elif uri.startswith("data:"):
            out.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            from urllib.parse import unquote
            with open(os.path.join(base_dir, unquote(uri)), "rb") as f:
                out.append(f.read())
    return out


def _read_accessor(gltf, buffers, idx):
    acc = gltf["accessors"][idx]
    ct, csz = _CT[acc["componentType"]]
    nc = _NC[acc["type"]]
    count = acc["count"]
    bv = gltf["bufferViews"][acc["bufferView"]]
    buf = buffers[bv.get("buffer", 0)]
    base = bv.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = bv.get("byteStride", csz * nc)
    arr = np.empty((count, nc), np.float64)
    for i in range(count):
        off = base + i * stride
        arr[i] = struct.unpack_from("<" + ct * nc, buf, off)
    return arr if nc > 1 else arr[:, 0]


def _node_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], np.float64).reshape(4, 4).T  # glTF col-major
    M = np.eye(4)
    if "scale" in node:
        S = np.diag(node["scale"] + [1.0]); M = S @ M
    if "rotation" in node:
        x, y, z, w = node["rotation"]
        R = np.array([
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1]], np.float64)
        M = R @ M
    if "translation" in node:
        T = np.eye(4); T[:3, 3] = node["translation"]; M = T @ M
    return M


def _load_gltf(path):
    data = open(path, "rb").read()
    base_dir = os.path.dirname(os.path.abspath(path))
    if data[:4] == b"glTF":
        gltf, glb_bin = _glb_chunks(data)
    else:
        gltf, glb_bin = json.loads(data.decode("utf-8")), b""
    if gltf is None:
        raise ValueError("not a glTF/GLB file")
    for m in gltf.get("meshes", []):
        for p in m.get("primitives", []):
            if "extensions" in p and "KHR_draco_mesh_compression" in p["extensions"]:
                raise ValueError("Draco-compressed glTF unsupported — re-export uncompressed")
    buffers = _gltf_buffers(gltf, base_dir, glb_bin)

    # Walk the scene graph so node transforms are baked into world space.
    P_all, UV_all, N_all, T_all = [], [], [], []
    base_v = 0
    have_uv_any = have_n_any = True
    nodes = gltf.get("nodes", [])

    def walk(ni, parent):
        nonlocal base_v, have_uv_any, have_n_any
        node = nodes[ni]
        M = parent @ _node_matrix(node)
        if "mesh" in node:
            mesh = gltf["meshes"][node["mesh"]]
            for prim in mesh.get("primitives", []):
                if prim.get("mode", 4) != 4:          # only triangles
                    continue
                attrs = prim["attributes"]
                if "POSITION" not in attrs:
                    continue
                pos = _read_accessor(gltf, buffers, attrs["POSITION"])
                n = pos.shape[0]
                ph = np.concatenate([pos, np.ones((n, 1))], axis=1)
                pos = (ph @ M.T)[:, :3]
                P_all.append(pos)
                if "TEXCOORD_0" in attrs:
                    UV_all.append(_read_accessor(gltf, buffers, attrs["TEXCOORD_0"]))
                else:
                    UV_all.append(np.zeros((n, 2))); have_uv_any = False
                if "NORMAL" in attrs:
                    nrm = _read_accessor(gltf, buffers, attrs["NORMAL"])
                    R = M[:3, :3]
                    nrm = nrm @ np.linalg.inv(R).T
                    N_all.append(nrm)
                else:
                    N_all.append(np.zeros((n, 3))); have_n_any = False
                if "indices" in prim:
                    idx = _read_accessor(gltf, buffers, prim["indices"]).astype(np.int64)
                else:
                    idx = np.arange(n, dtype=np.int64)
                tri = idx.reshape(-1, 3) + base_v
                T_all.append(tri)
                base_v += n
        for c in node.get("children", []):
            walk(c, M)

    scene = gltf.get("scene", 0)
    roots = gltf.get("scenes", [{}])[scene].get("nodes", list(range(len(nodes))))
    for r in roots:
        walk(r, np.eye(4))

    if not P_all:
        raise ValueError("no triangle geometry found in glTF")
    P = np.concatenate(P_all).astype(np.float32)
    UV = np.concatenate(UV_all).astype(np.float32) if have_uv_any else None
    N = np.concatenate(N_all).astype(np.float32) if have_n_any else None
    T = np.concatenate(T_all).astype(np.uint32)
    return P, UV, N, T


def _extract_gltf_base_color(path):
    """Best-effort: pull the first material's base-color texture out of a glTF
    as a PIL image (for use as the skin when --skin is absent). Returns None if
    no embedded texture or PIL missing."""
    if Image is None:
        return None
    try:
        data = open(path, "rb").read()
        base_dir = os.path.dirname(os.path.abspath(path))
        if data[:4] == b"glTF":
            gltf, glb_bin = _glb_chunks(data)
        else:
            gltf, glb_bin = json.loads(data.decode("utf-8")), b""
        images = gltf.get("images", [])
        if not images:
            return None
        # Prefer a material baseColorTexture; else just the first image.
        img_idx = 0
        for mat in gltf.get("materials", []):
            bct = mat.get("pbrMetallicRoughness", {}).get("baseColorTexture")
            if bct is not None:
                tex = gltf["textures"][bct["index"]]
                img_idx = tex.get("source", 0)
                break
        img = images[img_idx]
        import io
        if "uri" in img and img["uri"].startswith("data:"):
            raw = base64.b64decode(img["uri"].split(",", 1)[1])
        elif "uri" in img:
            from urllib.parse import unquote
            raw = open(os.path.join(base_dir, unquote(img["uri"])), "rb").read()
        else:
            buffers = _gltf_buffers(gltf, base_dir, glb_bin)
            bv = gltf["bufferViews"][img["bufferView"]]
            o = bv.get("byteOffset", 0)
            raw = buffers[bv.get("buffer", 0)][o:o + bv["byteLength"]]
        return Image.open(io.BytesIO(raw)).convert("RGBA")
    except Exception as e:
        print(f"  (note: could not extract embedded glTF texture: {e})")
        return None


def load_model(path):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".obj":
        return _load_obj(path)
    if ext in (".gltf", ".glb"):
        return _load_gltf(path)
    raise ValueError(f"unsupported model format '{ext}' (use .glb/.gltf/.obj)")


# ===========================================================================
# Geometry -> TD5 himodel model dict
# ===========================================================================

def _axis_remap(P, N, up, forward):
    """Map the source model axes onto game space: X=width, Y=up, Z=length.
    glTF/OBJ are Y-up; the only ambiguity is which way the car faces, hence
    --forward. We rebuild an (right, up, fwd) basis and project onto
    game (X=right, Y=up, Z=fwd)."""
    axes = {"+x": np.array([1, 0, 0.]), "-x": np.array([-1, 0, 0.]),
            "+y": np.array([0, 1, 0.]), "-y": np.array([0, -1, 0.]),
            "+z": np.array([0, 0, 1.]), "-z": np.array([0, 0, -1.])}
    up_v = axes["+y"] if up == "y" else axes["+z"]
    fwd_v = axes[forward]
    right_v = np.cross(up_v, fwd_v)
    if np.linalg.norm(right_v) < 1e-6:
        right_v = axes["+x"]
    right_v = right_v / (np.linalg.norm(right_v) or 1)
    basis = np.stack([right_v, up_v, fwd_v], axis=0)   # rows map src->game XYZ
    P2 = P @ basis.T
    N2 = N @ basis.T if N is not None else None
    return P2.astype(np.float32), (N2.astype(np.float32) if N2 is not None else None)


def build_himodel_model(P, UV, N, T, scale="auto", up="y", forward="-z",
                        flip_v=False):
    """Return (model_dict, stats) ready for mesh_tool.build_dat. De-indexes T
    into independent triangles, centres + scales to TARGET_LENGTH, computes
    missing normals."""
    P, N = _axis_remap(P, N, up, forward)

    # Centre on the geometric centre of the bounding box (native cars are
    # symmetric about the origin).
    lo, hi = P.min(axis=0), P.max(axis=0)
    centre = (lo + hi) * 0.5
    P = P - centre

    # Uniform scale so the longest extent (the car length, Z) hits the target.
    span = (hi - lo)
    if scale == "auto":
        length = max(span[2], 1e-6)
        s = TARGET_LENGTH / length
    else:
        s = float(scale)
    P = P * s

    # De-index into independent triangle corners (the native expanded form).
    corners = T.reshape(-1)
    vpos = P[corners]
    vuv = UV[corners] if UV is not None else np.zeros((len(corners), 2), np.float32)
    if flip_v:
        vuv = vuv.copy(); vuv[:, 1] = 1.0 - vuv[:, 1]

    if N is not None:
        vnrm = N[corners]
    else:
        # Per-face normal, replicated to its 3 corners.
        tri_p = vpos.reshape(-1, 3, 3)
        fn = np.cross(tri_p[:, 1] - tri_p[:, 0], tri_p[:, 2] - tri_p[:, 0])
        ln = np.linalg.norm(fn, axis=1, keepdims=True)
        fn = fn / np.where(ln == 0, 1, ln)
        vnrm = np.repeat(fn, 3, axis=0)

    ntri = len(corners) // 3
    bound_r = float(np.linalg.norm(vpos, axis=1).max()) if len(vpos) else 0.0

    # One command per <=65535 triangles (uint16 triangle_count).
    commands, vptr_cursor = [], 0
    remaining = ntri
    while remaining > 0:
        t = min(remaining, MAX_TRIS_PER_CMD)
        commands.append({"dispatch_type": 0, "texture_page_id": BODY_PAGE,
                         "reserved_04": 0, "tri": t, "quad": 0, "vptr": 0})
        remaining -= t

    vertices = [{"pos": [float(vpos[i, 0]), float(vpos[i, 1]), float(vpos[i, 2])],
                 "view": [0.0, 0.0, 0.0], "light": LIGHT_FULL,
                 "tex": [float(vuv[i, 0]), float(vuv[i, 1])], "proj": [0.0, 0.0]}
                for i in range(len(vpos))]
    normals = [{"n": [float(vnrm[i, 0]), float(vnrm[i, 1]), float(vnrm[i, 2])],
                "vis": 0} for i in range(len(vnrm))]

    model = {"kind": "himodel", "mesh": {
        "render_type": RENDER_TYPE_NATIVE, "texture_page_id": 0, "reserved_28": 0,
        "bounding": [bound_r, 0.0, 0.0, 0.0], "origin": [0.0, 0.0, 0.0],
        "commands": commands, "vertices": vertices, "normals": normals}}
    stats = dict(tris=ntri, verts=len(vpos), cmds=len(commands), scale=s,
                 bound_r=bound_r,
                 dims=(float(span[0] * s), float(span[1] * s), float(span[2] * s)))
    return model, stats


# ===========================================================================
# Textures
# ===========================================================================
_KEYS = {"blue88": (0, 0, 88), "black": (0, 0, 0), "red": (248, 0, 0),
         "cyan": (0, 248, 248)}


def normalize_texture(src, out_path, size=None, square=False, colorkey="none"):
    """Convert any image into an engine-readable RGBA PNG. The engine decodes
    PNG->BGRA itself, so this just guarantees RGBA + sane dimensions (+ optional
    colour-key alpha). `src` may be a path or a PIL.Image."""
    if Image is None:
        raise RuntimeError("Pillow (PIL) is required for texture conversion: pip install pillow")
    img = src if isinstance(src, Image.Image) else Image.open(src)
    img = img.convert("RGBA")
    if size:
        if square:
            img = img.resize((size, size), Image.LANCZOS)
        else:
            w, h = img.size
            sc = size / max(w, h)
            if sc < 1.0:
                img = img.resize((max(1, int(w * sc)), max(1, int(h * sc))), Image.LANCZOS)
    if colorkey != "none":
        kr, kg, kb = _KEYS[colorkey]
        a = np.array(img)
        if colorkey == "black":
            m = (a[:, :, 0] < 8) & (a[:, :, 1] < 8) & (a[:, :, 2] < 8)
        elif colorkey == "blue88":
            m = (a[:, :, 0] < 8) & (a[:, :, 1] < 8) & (a[:, :, 2] >= 80) & (a[:, :, 2] <= 96)
        elif colorkey == "red":
            m = (a[:, :, 0] >= 248) & (a[:, :, 1] < 8) & (a[:, :, 2] < 8)
        else:  # cyan
            m = (a[:, :, 1] > 240) & (a[:, :, 2] > 240)
        a[m, 3] = 0
        img = Image.fromarray(a)
    img.save(out_path, "PNG")
    return img.size


def _make_carpic(skin_img, out_path, w=408, h=280, name=""):
    """Placeholder showroom thumbnail: the skin fit onto a BLUE88-keyed frame
    (carpic uses the BLUE88 colour key) so the SELECT-CAR gallery isn't broken."""
    if Image is None:
        return
    from PIL import ImageDraw
    pic = Image.new("RGBA", (w, h), (0, 0, 88, 255))   # BLUE88 background
    s = skin_img.convert("RGBA")
    sw, sh = s.size
    sc = min((w - 24) / sw, (h - 48) / sh)
    s = s.resize((max(1, int(sw * sc)), max(1, int(sh * sc))), Image.LANCZOS)
    pic.paste(s, ((w - s.size[0]) // 2, (h - s.size[1]) // 2 - 8), s)
    try:
        d = ImageDraw.Draw(pic)
        d.text((10, h - 22), (name or "CUSTOM")[:28], fill=(255, 255, 255, 255))
    except Exception:
        pass
    pic.save(out_path, "PNG")


# ===========================================================================
# config.nfo
# ===========================================================================
def write_config_nfo(out_path, display, short, stats=None):
    st = stats or {}
    us = lambda s: str(s).replace(" ", "_")
    lines = [
        us(display), us(short),
        st.get("layout", "A"), str(st.get("gears", "6")), st.get("price", "N/A"),
        us(st.get("tire_f", "N/A")), us(st.get("tire_r", "N/A")),
        str(st.get("top", "0")), str(st.get("accel", "0.0")), str(st.get("brake", "0")),
        str(st.get("quarter", "0.0")), st.get("engine", "A"),
        st.get("compression", "N/A"), st.get("displacement", "N/A"),
        st.get("lateral", "0.0G"), us(st.get("torque", "N/A")), us(st.get("hp", "N/A")),
    ]
    with open(out_path, "w", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")


# ===========================================================================
# import command
# ===========================================================================
def cmd_import(args):
    if Image is None:
        print("ERROR: Pillow (PIL) required: pip install pillow", file=sys.stderr)
        return 2

    code = args.code
    if not code:
        slug = "".join(c.lower() if c.isalnum() else "_" for c in args.name).strip("_")
        slug = "_".join(p for p in slug.split("_") if p)[:24] or "car"
        code = f"custom_{slug}"
    if not code.startswith("custom_"):
        code = "custom_" + code
    if "." in code:
        print("ERROR: --code must not contain '.'", file=sys.stderr)
        return 2

    donor_dir = args.donor if os.path.isdir(args.donor) else \
        os.path.join(args.out_dir, args.donor)
    if not os.path.isdir(donor_dir):
        print(f"ERROR: donor dir not found: {donor_dir}", file=sys.stderr)
        return 2

    print(f"Importing '{args.model}' -> car '{code}' (donor {os.path.basename(donor_dir)})")
    P, UV, N, T = load_model(args.model)
    print(f"  model: {len(P)} verts, {len(T)} tris, "
          f"uv={'yes' if UV is not None else 'NO (flat)'}, "
          f"normals={'yes' if N is not None else 'computed'}")

    model, st = build_himodel_model(P, UV, N, T, scale=args.scale, up=args.up,
                                    forward=args.forward, flip_v=args.flip_v)
    print(f"  himodel: {st['tris']} tris in {st['cmds']} cmd(s), scale x{st['scale']:.4f}, "
          f"dims WxHxL={st['dims'][0]:.0f}x{st['dims'][1]:.0f}x{st['dims'][2]:.0f}, "
          f"radius {st['bound_r']:.0f}")
    if st["tris"] > 6000:
        print(f"  WARNING: {st['tris']} tris is high for the software renderer "
              f"(native cars ~200-450); expect a frame-rate hit. Consider decimating.")

    himodel_bytes = mesh_tool.build_dat(model)
    # Round-trip sanity: decode what we just packed and confirm it parses.
    rt = mesh_tool.decode(himodel_bytes, "himodel")["mesh"]
    assert len(rt["vertices"]) == st["verts"], "himodel round-trip vertex mismatch"

    car_out = os.path.join(args.out_dir, code)
    if args.dry_run:
        print(f"  [dry-run] would write {car_out}/ ({len(himodel_bytes)} B himodel)")
        return 0
    os.makedirs(car_out, exist_ok=True)

    with open(os.path.join(car_out, "himodel.bin"), "wb") as f:
        f.write(himodel_bytes)

    # Skin: --skin > embedded glTF base-color > neutral grey.
    if args.skin:
        skin_src = args.skin
    else:
        emb = _extract_gltf_base_color(args.model) if args.model.lower().endswith((".glb", ".gltf")) else None
        skin_src = emb if emb is not None else Image.new("RGBA", (256, 256), (150, 150, 155, 255))
        if emb is not None:
            print("  skin: using embedded glTF base-color texture")
        else:
            print("  skin: no texture supplied -> neutral grey placeholder (use --skin)")
    skin_img = skin_src if isinstance(skin_src, Image.Image) else Image.open(skin_src)
    skin_img = skin_img.convert("RGBA")
    if max(skin_img.size) > 512:
        sc = 512 / max(skin_img.size)
        skin_img = skin_img.resize((int(skin_img.size[0] * sc), int(skin_img.size[1] * sc)), Image.LANCZOS)
    for i in range(4):
        skin_img.save(os.path.join(car_out, f"carskin{i}.png"), "PNG")

    # Hub sprite: copy donor (wheels render as sprites, not mesh). Stub if absent.
    donor_hub = os.path.join(donor_dir, "carhub0.png")
    for i in range(4):
        dst = os.path.join(car_out, f"carhub{i}.png")
        if os.path.isfile(donor_hub):
            shutil.copyfile(donor_hub, dst)
        else:
            Image.new("RGBA", (64, 64), (40, 40, 40, 255)).save(dst, "PNG")

    # Showroom thumbnails (placeholder).
    if args.carpic and os.path.isfile(args.carpic):
        cp = Image.open(args.carpic).convert("RGBA")
        for i in range(4):
            cp.save(os.path.join(car_out, f"carpic{i}.png"), "PNG")
    else:
        for i in range(4):
            _make_carpic(skin_img, os.path.join(car_out, f"carpic{i}.png"), name=args.name)

    # Physics: an explicit --carparam-json (e.g. edited in the Car Studio) wins;
    # otherwise copy the donor's carparam.json (assetsrc re-encodes -> carparam.dat).
    donor_param = os.path.join(donor_dir, "carparam.json")
    if args.carparam_json and os.path.isfile(args.carparam_json):
        shutil.copyfile(args.carparam_json, os.path.join(car_out, "carparam.json"))
        print(f"  carparam: using supplied {os.path.basename(args.carparam_json)}")
    elif os.path.isfile(donor_param):
        shutil.copyfile(donor_param, os.path.join(car_out, "carparam.json"))
    elif os.path.isfile(os.path.join(donor_dir, "carparam.dat")):
        shutil.copyfile(os.path.join(donor_dir, "carparam.dat"),
                        os.path.join(car_out, "carparam.dat"))
    else:
        print(f"  WARNING: donor has no carparam.json/.dat — car will use engine defaults")

    # Engine audio: copy donor wavs.
    for w in ("drive.wav", "rev.wav", "horn.wav", "reverb.wav"):
        src = os.path.join(donor_dir, w)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(car_out, w))

    # Spec sheet.
    stats = None
    if args.stats and os.path.isfile(args.stats):
        stats = json.load(open(args.stats))
    write_config_nfo(os.path.join(car_out, "config.nfo"), args.name,
                     args.short or args.name, stats)

    print(f"\nDONE -> {car_out}")
    print(f"  Launch td5re.exe; '{args.name}' auto-enumerates in SELECT CAR "
          f"(Quick Race). No rebuild needed once td5_customcar is in the engine.")
    return 0


# ===========================================================================
# texture command (standalone converter)
# ===========================================================================
def cmd_texture(args):
    size = args.size if args.size > 0 else None
    sz = normalize_texture(args.src, args.out, size=size, square=args.square,
                           colorkey=args.colorkey)
    print(f"wrote {args.out} {sz[0]}x{sz[1]} RGBA (colorkey={args.colorkey})")
    return 0


# ===========================================================================
# verify command
# ===========================================================================
def cmd_verify(args):
    d = args.car_dir
    hb = os.path.join(d, "himodel.bin")
    if not os.path.isfile(hb):
        hb = os.path.join(d, "himodel.dat")
    if not os.path.isfile(hb):
        print(f"FAIL: no himodel.bin/.dat in {d}", file=sys.stderr)
        return 1
    data = open(hb, "rb").read()
    rt = struct.unpack_from("<h", data, 0)[0]
    m = mesh_tool.decode(data, "himodel")
    if m.get("mesh") is None:
        print(f"{d}: render_type 0x{rt:x} (TD6 indexed)")
        return 0
    mesh = m["mesh"]
    # rebuild->decode->equal round-trip
    m2 = mesh_tool.decode(mesh_tool.build_dat(m), "himodel")
    ok, why = mesh_tool.model_equal(m, m2)
    nv = len(mesh["vertices"])
    nt = sum(c["tri"] for c in mesh["commands"])
    pages = sorted({c["texture_page_id"] for c in mesh["commands"]})
    have = [f for f in ("carskin0.png", "carparam.json", "carparam.dat",
                        "carhub0.png", "config.nfo") if os.path.isfile(os.path.join(d, f))]
    print(f"{'PASS' if ok else 'FAIL'}  {os.path.basename(d)}  rt=0x{rt:x} "
          f"verts={nv} tris={nt} cmd_pages={pages} files={have}"
          + ("" if ok else f"  [{why}]"))
    return 0 if ok else 1


# ===========================================================================
# new command (scaffold a custom car by cloning a donor)
# ===========================================================================
def cmd_new(args):
    code = args.code if args.code.startswith("custom_") else "custom_" + args.code
    if "." in code:
        print("ERROR: --code must not contain '.'", file=sys.stderr)
        return 2
    donor_dir = args.donor if os.path.isdir(args.donor) else os.path.join(args.out_dir, args.donor)
    if not os.path.isdir(donor_dir):
        print(f"ERROR: donor dir not found: {donor_dir}", file=sys.stderr)
        return 2
    car_out = os.path.join(args.out_dir, code)
    os.makedirs(car_out, exist_ok=True)
    # Clone the donor's runtime files so the new car is immediately drivable; the
    # author then swaps mesh/skin/stats via the Studio or `import`.
    for f in os.listdir(donor_dir):
        if f.startswith(".") or f.endswith((".dat", ".log")):
            continue
        src = os.path.join(donor_dir, f)
        if os.path.isfile(src):
            shutil.copyfile(src, os.path.join(car_out, f))
    name = args.name or code.replace("custom_", "").replace("_", " ").upper()
    write_config_nfo(os.path.join(car_out, "config.nfo"), name, name)
    print(f"scaffolded {car_out} (clone of {os.path.basename(donor_dir)}) as '{name}'")
    print("  edit it in the Car Studio, or: td5_car_import.py import --model ... "
          f"--code {code}")
    return 0


# ===========================================================================
# doctor command (validate a car folder)
# ===========================================================================
def cmd_doctor(args):
    d = args.car_dir
    warns, errs = [], []

    def need(f):
        if not os.path.isfile(os.path.join(d, f)):
            errs.append(f"missing {f}")

    hb = os.path.join(d, "himodel.bin")
    if not os.path.isfile(hb):
        hb = os.path.join(d, "himodel.dat")
    if not os.path.isfile(hb):
        errs.append("missing himodel.bin/.dat")
        mesh = None
    else:
        try:
            data = open(hb, "rb").read()
            rt = struct.unpack_from("<h", data, 0)[0]
            m = mesh_tool.decode(data, "himodel")
            mesh = m.get("mesh")
            if mesh is None:
                print(f"  note: render_type 0x{rt:x} (TD6 indexed) — not validated further")
        except Exception as e:
            errs.append(f"himodel parse failed: {e}")
            mesh = None

    for f in ("carskin0.png", "carhub0.png", "config.nfo"):
        need(f)
    if not (os.path.isfile(os.path.join(d, "carparam.json")) or
            os.path.isfile(os.path.join(d, "carparam.dat"))):
        errs.append("missing carparam.json/.dat")

    if mesh is not None:
        ntri = sum(c["tri"] for c in mesh["commands"])
        nq = sum(c["quad"] for c in mesh["commands"])
        if ntri + nq == 0:
            errs.append("mesh has no triangles")
        if ntri > 6000:
            warns.append(f"{ntri} triangles — high for the software renderer (native ~200-450)")
        verts = mesh["vertices"]
        if verts:
            import math
            xs = [v["pos"][0] for v in verts]; ys = [v["pos"][1] for v in verts]; zs = [v["pos"][2] for v in verts]
            dx, dy, dz = max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)
            if not (200 < dz < 6000):
                warns.append(f"length (Z) {dz:.0f} unusual — native cars ~1500 (re-import with --scale)")
            pages = sorted({c["texture_page_id"] for c in mesh["commands"]})
            if pages and 7 not in pages and 8 not in pages:
                warns.append(f"command texture pages {pages} — body usually page 7")
            # Wheels inside the body envelope?
            cpj = os.path.join(d, "carparam.json")
            if os.path.isfile(cpj):
                try:
                    cp = json.load(open(cpj))
                    for w in ("wheel_pos_FL", "wheel_pos_FR", "wheel_pos_RL", "wheel_pos_RR"):
                        if w in cp and isinstance(cp[w], dict):
                            x, y, z = cp[w]["value"][:3]
                            if abs(x) > dx * 0.75 or abs(z) > dz * 0.6:
                                warns.append(f"{w}=({x},{y},{z}) sits outside the body bbox "
                                             f"(WxL {dx:.0f}x{dz:.0f}) — wheels may float")
                except Exception:
                    pass

    label = os.path.basename(os.path.abspath(d))
    for e in errs:
        print(f"  FAIL {label}: {e}")
    for w in warns:
        print(f"  WARN {label}: {w}")
    if not errs and not warns:
        print(f"  OK   {label}: all checks passed")
    elif not errs:
        print(f"  OK   {label}: drivable ({len(warns)} warning(s))")
    return 1 if errs else 0


# ===========================================================================
# stats command (fleet physics reference)
# ===========================================================================
def cmd_stats(args):
    import td5_car_physics_ref as ref_mod
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    ref = ref_mod.build_reference(args.cars_dir)
    if ref.get("error"):
        print(ref["error"], file=sys.stderr)
        return 1
    print(ref_mod.render_table(ref, markdown=args.markdown))
    if not args.markdown:
        print("\npresets (median of each archetype's stock cars; apply as a baseline, then tune):")
        print("  " + ", ".join(f"{k}({len(v)})" for k, v in ref["preset_members"].items()))
    return 0


def main():
    ap = argparse.ArgumentParser(description="Import custom cars / convert textures for TD5RE")
    sub = ap.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("import", help="import a glTF/OBJ model into a drop-in custom car")
    pi.add_argument("--model", required=True, help="source .glb/.gltf/.obj")
    pi.add_argument("--name", required=True, help="display name (SELECT CAR title)")
    pi.add_argument("--short", default=None, help="short name (default: --name)")
    pi.add_argument("--code", default=None, help="folder name (default: custom_<slug>)")
    pi.add_argument("--out-dir", default="re/assets/cars", help="cars root")
    pi.add_argument("--donor", default="vip", help="donor car (carparam/sounds/hub)")
    pi.add_argument("--skin", default=None, help="body texture image (any format)")
    pi.add_argument("--carpic", default=None, help="showroom thumbnail image")
    pi.add_argument("--carparam-json", default=None,
                    help="use this carparam.json verbatim (else copy donor's)")
    pi.add_argument("--stats", default=None, help="JSON of config.nfo stat fields")
    pi.add_argument("--scale", default="auto", help="'auto' (fit length) or a float")
    pi.add_argument("--up", default="y", choices=["y", "z"], help="model up axis")
    pi.add_argument("--forward", default="-z", choices=["+z", "-z", "+x", "-x"],
                    help="model forward axis (flip if the car faces backward)")
    pi.add_argument("--flip-v", action="store_true", help="flip texture V (if upside-down)")
    pi.add_argument("--dry-run", action="store_true")
    pi.set_defaults(func=cmd_import)

    pt = sub.add_parser("texture", help="convert any image to an engine-readable PNG")
    pt.add_argument("src")
    pt.add_argument("out")
    pt.add_argument("--size", type=int, default=0, help="max dimension (0 = keep)")
    pt.add_argument("--square", action="store_true", help="force size x size")
    pt.add_argument("--colorkey", default="none",
                    choices=["none", "black", "blue88", "red", "cyan"])
    pt.set_defaults(func=cmd_texture)

    pv = sub.add_parser("verify", help="validate a produced car folder")
    pv.add_argument("car_dir")
    pv.set_defaults(func=cmd_verify)

    pn = sub.add_parser("new", help="scaffold a custom car by cloning a donor")
    pn.add_argument("--code", required=True, help="folder name (custom_<x>)")
    pn.add_argument("--name", default=None, help="display name")
    pn.add_argument("--donor", default="vip", help="donor car to clone")
    pn.add_argument("--out-dir", default="re/assets/cars", help="cars root")
    pn.set_defaults(func=cmd_new)

    pd = sub.add_parser("doctor", help="validate a car folder (mesh/files/wheels/dims)")
    pd.add_argument("car_dir")
    pd.set_defaults(func=cmd_doctor)

    ps = sub.add_parser("stats", help="fleet physics reference: each field's effect + min/median/max + presets")
    ps.add_argument("--cars-dir", default="re/assets/cars", help="cars root to scan")
    ps.add_argument("--markdown", action="store_true", help="emit a markdown table (e.g. for docs)")
    ps.set_defaults(func=cmd_stats)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
