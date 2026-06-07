#!/usr/bin/env python3
"""
mesh_tool.py -- decode/rebuild Test Drive 5 PRR meshes (MODELS.DAT container +
HIMODEL.DAT single mesh) and convert to/from glTF (.glb) for Blender editing.
Part of the pack-on-load retirement, Phase 4b (the OFFLINE mesh-editing tool;
the .DAT itself is already retired byte-exact via models.bin/himodel.bin).

Pipeline:
    .dat  --decode-->  model  --export_glb-->  .glb   (edit in Blender)
    .glb  --import_glb--> model --build_dat-->  .dat'  (regenerate models.bin)

`model` mirrors what td5_track_parse_models_dat + the renderer consume:
  container: entry table (span-indexed) -> blocks -> sub-meshes (PRR)
  PRR mesh:  header(0x38) + commands(0x10 ea) + vertices(0x2C ea) + normals(0x10 ea)
  each command consumes tri*3 + quad*4 vertices SEQUENTIALLY (independent tris/quads).

Rebuild emits a canonical Format-A container (count @0, [offset,size] pairs @4,
blocks contiguous) that the existing C parser accepts. Mesh bytes (incl. the
runtime-written view/lighting slots) are preserved verbatim, so an unedited
round-trip is render-identical; `selfcheck` proves decode==decode(rebuild)
headless over the whole corpus.

Usage:
  python mesh_tool.py selfcheck <models.dat | himodel.dat>
  python mesh_tool.py selfcheck-all                 # sweep re/assets corpus
  python mesh_tool.py export <in.dat>  <out.glb>
  python mesh_tool.py import <in.glb>  <out.dat>
"""

import glob
import json
import os
import struct
import sys

U = lambda b, o: struct.unpack_from("<I", b, o)[0]
HDR = 0x38       # MeshResourceHeader
CMD = 0x10       # PrimitiveCommand
VTX = 0x2C       # MeshVertex
NRM = 0x10       # VertexNormal


# ---------------------------------------------------------------------------
# Decode .dat -> model
# ---------------------------------------------------------------------------

def _parse_entries(data):
    size = len(data)
    if size < 8:
        raise ValueError("too small")
    d0, d1 = U(data, 0), U(data, 4)
    if d0 > 0 and d0 < 10000 and d1 == 4 + d0 * 8 and 4 + d0 * 8 <= size:
        count, table = d0, 4
    elif (d0 > 0 and d0 < 10000 and 4 + d0 * 8 <= size and d1 >= 4 + d0 * 8
          and d1 < size and d1 + 4 <= size and 1 <= U(data, d1) <= 256):
        count, table = d0, 4
    elif d1 > 0 and (d1 & 7) == 0 and d1 <= size:
        count, table = d1 // 8, 0
    else:
        raise ValueError(f"cannot determine MODELS format d0={d0} d1={d1} size={size}")

    table_end = table + count * 8
    entries = []
    for i in range(count):
        tb = table + i * 8
        if tb + 8 > size:
            break
        f0, f1 = U(data, tb), U(data, tb + 4)

        def is_off(f):
            return (f >= table_end and f < size and f + 4 <= size
                    and 1 <= U(data, f) <= 256)
        if is_off(f0) and not is_off(f1):
            off, sz = f0, f1
        elif is_off(f1) and not is_off(f0):
            off, sz = f1, f0
        else:
            off, sz = f0, f1            # prefer f0 (matches C bugfix)
        entries.append((off, sz))
    return count, table, entries


def _parse_mesh(data, base):
    (render_type, texture_page_id, command_count, total_vertex_count,
     br, bcx, bcy, bcz, ox, oy, oz, reserved_28,
     coff, voff, noff) = struct.unpack_from("<hhii ffff fff IIII", data, base)
    mesh = {
        "render_type": render_type, "texture_page_id": texture_page_id,
        "bounding": [br, bcx, bcy, bcz], "origin": [ox, oy, oz],
        "reserved_28": reserved_28,
        "commands": [], "vertices": [], "normals": None,
    }
    cbase = base + coff
    for i in range(command_count):
        o = cbase + i * CMD
        dt, tp, r4, tri, quad = struct.unpack_from("<hhiHH", data, o)
        vptr = U(data, o + 0x0C)
        mesh["commands"].append({"dispatch_type": dt, "texture_page_id": tp,
                                 "reserved_04": r4, "tri": tri, "quad": quad,
                                 "vptr": vptr})
    vbase = base + voff
    for i in range(total_vertex_count):
        o = vbase + i * VTX
        px, py, pz = struct.unpack_from("<fff", data, o)
        vx, vy, vz = struct.unpack_from("<fff", data, o + 0x0C)
        light = U(data, o + 0x18)
        tu, tv, pu, pv = struct.unpack_from("<ffff", data, o + 0x1C)
        mesh["vertices"].append({"pos": [px, py, pz], "view": [vx, vy, vz],
                                 "light": light, "tex": [tu, tv], "proj": [pu, pv]})
    if noff != 0:
        nbase = base + noff
        norms = []
        for i in range(total_vertex_count):
            o = nbase + i * NRM
            nx, ny, nz = struct.unpack_from("<fff", data, o)
            vis = struct.unpack_from("<i", data, o + 0x0C)[0]
            norms.append({"n": [nx, ny, nz], "vis": vis})
        mesh["normals"] = norms
    return mesh


def decode(data, kind="models"):
    if kind == "himodel":
        # HIMODEL.DAT is a single PRR mesh at offset 0 (no container).
        # render_type 0x103 = TD5 expanded; 0x104 = TD6 indexed (transcoded at
        # load -> different vertex layout, not handled by this tool yet).
        rt = struct.unpack_from("<h", data, 0)[0]
        if rt == 0x104:
            return {"kind": "himodel", "mesh": None, "td6_indexed": True}
        return {"kind": "himodel", "mesh": _parse_mesh(data, 0)}

    size = len(data)
    count, table, entries = _parse_entries(data)
    meshes = []
    by_abs = {}            # absolute mesh offset -> index in meshes (dedup shared)
    out_entries = []

    for (off, sz) in entries:
        if off == 0 or off >= size:
            out_entries.append([])
            continue
        block_size = sz
        if block_size == 0 or off + block_size > size:
            # estimate to EOF (C does similar); clamp
            block_size = size - off
        sub_count = U(data, off)
        if sub_count == 0 or sub_count > 256 or block_size < 4 + sub_count * 4:
            out_entries.append([])
            continue
        ids = []
        for j in range(sub_count):
            moff = U(data, off + 4 + j * 4)
            if moff == 0:
                continue
            if moff < block_size and moff + HDR <= block_size:
                abs_off = off + moff
            elif moff < size and moff + HDR <= size:
                abs_off = moff                      # blob-relative
            else:
                continue
            cc = struct.unpack_from("<i", data, abs_off + 4)[0]
            vc = struct.unpack_from("<i", data, abs_off + 8)[0]
            if cc < 0 or cc > 4096 or vc < 0 or vc > 65536:
                continue
            if abs_off not in by_abs:
                by_abs[abs_off] = len(meshes)
                meshes.append(_parse_mesh(data, abs_off))
            ids.append(by_abs[abs_off])
        out_entries.append(ids)

    return {"kind": "models", "entry_count": count,
            "entries": out_entries, "meshes": meshes}


# ---------------------------------------------------------------------------
# Build model -> parser-valid Format-A .dat
# ---------------------------------------------------------------------------

def _pack_mesh(m):
    cmds = m["commands"]
    verts = m["vertices"]
    norms = m["normals"]
    coff = HDR
    voff = coff + len(cmds) * CMD
    noff = voff + len(verts) * VTX if norms is not None else 0
    out = bytearray()
    out += struct.pack("<hhii ffff fff IIII",
                       m["render_type"], m["texture_page_id"],
                       len(cmds), len(verts),
                       m["bounding"][0], m["bounding"][1], m["bounding"][2], m["bounding"][3],
                       m["origin"][0], m["origin"][1], m["origin"][2],
                       m["reserved_28"], coff, voff, noff)
    for c in cmds:
        out += struct.pack("<hhiHH", c["dispatch_type"], c["texture_page_id"],
                           c["reserved_04"], c["tri"], c["quad"])
        out += struct.pack("<I", c["vptr"])
    for v in verts:
        out += struct.pack("<fff", *v["pos"])
        out += struct.pack("<fff", *v["view"])
        out += struct.pack("<I", v["light"])
        out += struct.pack("<ffff", v["tex"][0], v["tex"][1], v["proj"][0], v["proj"][1])
    if norms is not None:
        for n in norms:
            out += struct.pack("<fff", *n["n"])
            out += struct.pack("<i", n["vis"])
    return bytes(out)


def build_dat(model):
    if model.get("kind") == "himodel":
        if model.get("mesh") is None:
            raise ValueError("TD6 0x104 indexed himodel not supported")
        return _pack_mesh(model["mesh"])
    count = model["entry_count"]
    entries = model["entries"]
    packed_meshes = [_pack_mesh(m) for m in model["meshes"]]

    table = 4 + count * 8
    blocks = []                 # (entry_index, block_bytes)
    # Layout: [count][ (off,sz) pairs ][ block_0 ][ block_1 ] ...
    block_bytes_list = []
    entry_off = [0] * count
    entry_sz = [0] * count
    cursor = table
    for i in range(count):
        ids = entries[i] if i < len(entries) else []
        if not ids:
            continue
        # block: [sub_count][mesh_offsets...][meshes...]
        sub = len(ids)
        header = bytearray(struct.pack("<I", sub))
        mesh_off_table = 4 + sub * 4
        body = bytearray()
        offs = []
        for mid in ids:
            offs.append(mesh_off_table + len(body))
            body += packed_meshes[mid]
        for o in offs:
            header += struct.pack("<I", o)
        block = bytes(header) + bytes(body)
        entry_off[i] = cursor
        entry_sz[i] = len(block)
        block_bytes_list.append(block)
        cursor += len(block)

    out = bytearray()
    out += struct.pack("<I", count)
    for i in range(count):
        out += struct.pack("<II", entry_off[i], entry_sz[i])
    for b in block_bytes_list:
        out += b
    return bytes(out)


# ---------------------------------------------------------------------------
# Equivalence (geometry / topology that the renderer consumes)
# ---------------------------------------------------------------------------

def _mesh_key(m):
    return (
        m["render_type"], m["texture_page_id"], m["reserved_28"],
        tuple(m["bounding"]), tuple(m["origin"]),
        tuple((c["dispatch_type"], c["texture_page_id"], c["reserved_04"],
               c["tri"], c["quad"]) for c in m["commands"]),
        tuple((tuple(v["pos"]), tuple(v["tex"]), tuple(v["proj"]), v["light"])
              for v in m["vertices"]),
        None if m["normals"] is None else
        tuple((tuple(n["n"]), n["vis"]) for n in m["normals"]),
    )


def model_equal(a, b):
    if a.get("kind") != b.get("kind"):
        return False, "kind differs"
    if a["kind"] == "himodel":
        if _mesh_key(a["mesh"]) != _mesh_key(b["mesh"]):
            return False, "mesh differs"
        return True, "ok"
    if a["entries"] != b["entries"]:
        return False, "entry->mesh mapping differs"
    if len(a["meshes"]) != len(b["meshes"]):
        return False, "mesh count differs"
    for i, (ma, mb) in enumerate(zip(a["meshes"], b["meshes"])):
        if _mesh_key(ma) != _mesh_key(mb):
            return False, f"mesh {i} differs"
    return True, "ok"


# ---------------------------------------------------------------------------
# glTF (.glb) export / import. Hand-rolled GLB (no pygltflib). Standard
# accessors (POSITION/TEXCOORD_0/TEXCOORD_1/NORMAL) so Blender can edit; PRR
# data (commands, per-vertex light, per-face visible_flag, header, container
# entry map) rides in extras for a faithful rebuild. view is dropped (the
# renderer recomputes it; not in the equivalence key).
# ---------------------------------------------------------------------------
import numpy as np

GLB_MAGIC = 0x46546C67
JSON_CHUNK = 0x4E4F534A
BIN_CHUNK = 0x004E4942
COMP_FLOAT = 5126
COMP_UINT = 5125
_NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}
_DTYPE = {5126: np.float32, 5125: np.uint32, 5123: np.uint16}


class _Glb:
    def __init__(self):
        self.bin = bytearray()
        self.bufferViews = []
        self.accessors = []

    def add(self, arr, comp, typ, minmax=False):
        while len(self.bin) % 4:
            self.bin.append(0)
        off = len(self.bin)
        self.bin += arr.tobytes()
        self.bufferViews.append({"buffer": 0, "byteOffset": off,
                                 "byteLength": int(arr.nbytes)})
        acc = {"bufferView": len(self.bufferViews) - 1, "componentType": comp,
               "count": int(arr.shape[0]), "type": typ}
        if minmax and arr.shape[0] > 0:
            acc["min"] = arr.min(axis=0).tolist()
            acc["max"] = arr.max(axis=0).tolist()
        self.accessors.append(acc)
        return len(self.accessors) - 1


def _indices_for_mesh(m):
    idx, cursor = [], 0
    for c in m["commands"]:
        tri, quad = c["tri"], c["quad"]
        for t in range(tri):
            b = cursor + t * 3
            idx += [b, b + 1, b + 2]
        qb = cursor + tri * 3
        for q in range(quad):
            b = qb + q * 4
            idx += [b, b + 1, b + 2, b, b + 2, b + 3]
        cursor += tri * 3 + quad * 4
    return idx


def _pack_glb(gltf, bin_data):
    js = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
    while len(js) % 4:
        js += b" "
    bd = bytearray(bin_data)
    while len(bd) % 4:
        bd.append(0)
    out = bytearray()
    out += struct.pack("<III", GLB_MAGIC, 2, 12 + 8 + len(js) + 8 + len(bd))
    out += struct.pack("<II", len(js), JSON_CHUNK) + js
    out += struct.pack("<II", len(bd), BIN_CHUNK) + bytes(bd)
    return bytes(out)


def _unpack_glb(data):
    magic, _ver, _total = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC:
        raise ValueError("not a GLB")
    off, js, bin_data = 12, None, b""
    while off < len(data):
        clen, ctype = struct.unpack_from("<II", data, off)
        off += 8
        chunk = data[off:off + clen]
        off += clen
        if ctype == JSON_CHUNK:
            js = json.loads(chunk.decode("utf-8"))
        elif ctype == BIN_CHUNK:
            bin_data = bytes(chunk)
    return js, bin_data


def _read_acc(gltf, bin_data, idx):
    acc = gltf["accessors"][idx]
    bv = gltf["bufferViews"][acc["bufferView"]]
    nc = _NCOMP[acc["type"]]
    arr = np.frombuffer(bin_data, dtype=_DTYPE[acc["componentType"]],
                        count=acc["count"] * nc, offset=bv.get("byteOffset", 0))
    return arr.reshape(acc["count"], nc) if nc > 1 else arr


def export_glb(model):
    gb = _Glb()
    meshes = [model["mesh"]] if model["kind"] == "himodel" else model["meshes"]
    gltf_meshes = []
    for m in meshes:
        verts = m["vertices"]
        n = len(verts)
        pos = np.array([v["pos"] for v in verts], np.float32).reshape(-1, 3) if n else np.zeros((0, 3), np.float32)
        tex = np.array([v["tex"] for v in verts], np.float32).reshape(-1, 2) if n else np.zeros((0, 2), np.float32)
        proj = np.array([v["proj"] for v in verts], np.float32).reshape(-1, 2) if n else np.zeros((0, 2), np.float32)
        attrs = {"POSITION": gb.add(pos, COMP_FLOAT, "VEC3", minmax=True),
                 "TEXCOORD_0": gb.add(tex, COMP_FLOAT, "VEC2"),
                 "TEXCOORD_1": gb.add(proj, COMP_FLOAT, "VEC2")}
        if m["normals"] is not None:
            nrm = np.array([nd["n"] for nd in m["normals"]], np.float32).reshape(-1, 3) if n else np.zeros((0, 3), np.float32)
            attrs["NORMAL"] = gb.add(nrm, COMP_FLOAT, "VEC3")
        prim = {"attributes": attrs, "mode": 4}
        idx = _indices_for_mesh(m)
        if idx:
            prim["indices"] = gb.add(np.array(idx, np.uint32), COMP_UINT, "SCALAR")
        prr = {"render_type": m["render_type"], "texture_page_id": m["texture_page_id"],
               "reserved_28": m["reserved_28"], "bounding": m["bounding"],
               "origin": m["origin"],
               "commands": [[c["dispatch_type"], c["texture_page_id"], c["reserved_04"],
                             c["tri"], c["quad"], c["vptr"]] for c in m["commands"]],
               "light": [v["light"] for v in verts],
               "vis": None if m["normals"] is None else [nd["vis"] for nd in m["normals"]]}
        gltf_meshes.append({"primitives": [prim], "extras": {"prr": prr}})

    td5 = {"kind": model["kind"]}
    if model["kind"] == "models":
        td5["entry_count"] = model["entry_count"]
        td5["entries"] = model["entries"]
    gltf = {"asset": {"version": "2.0", "generator": "td5_mesh_tool",
                      "extras": {"td5": td5}},
            "buffers": [{"byteLength": len(gb.bin)}],
            "bufferViews": gb.bufferViews, "accessors": gb.accessors,
            "meshes": gltf_meshes,
            "nodes": [{"mesh": i} for i in range(len(gltf_meshes))],
            "scenes": [{"nodes": list(range(len(gltf_meshes)))}], "scene": 0}
    return _pack_glb(gltf, bytes(gb.bin))


def import_glb(data):
    gltf, bin_data = _unpack_glb(data)
    td5 = gltf["asset"]["extras"]["td5"]
    meshes = []
    for gm in gltf["meshes"]:
        prim = gm["primitives"][0]
        attrs = prim["attributes"]
        prr = gm["extras"]["prr"]
        pos = _read_acc(gltf, bin_data, attrs["POSITION"])
        tex = _read_acc(gltf, bin_data, attrs["TEXCOORD_0"])
        proj = _read_acc(gltf, bin_data, attrs["TEXCOORD_1"])
        light, vis = prr["light"], prr["vis"]
        n = pos.shape[0]
        verts = [{"pos": [float(pos[i, 0]), float(pos[i, 1]), float(pos[i, 2])],
                  "view": [0.0, 0.0, 0.0],
                  "light": int(light[i]) if i < len(light) else 0,
                  "tex": [float(tex[i, 0]), float(tex[i, 1])],
                  "proj": [float(proj[i, 0]), float(proj[i, 1])]} for i in range(n)]
        normals = None
        if "NORMAL" in attrs:
            nrm = _read_acc(gltf, bin_data, attrs["NORMAL"])
            normals = [{"n": [float(nrm[i, 0]), float(nrm[i, 1]), float(nrm[i, 2])],
                        "vis": int(vis[i]) if (vis and i < len(vis)) else 0}
                       for i in range(n)]
        meshes.append({"render_type": prr["render_type"],
                       "texture_page_id": prr["texture_page_id"],
                       "reserved_28": prr["reserved_28"], "bounding": prr["bounding"],
                       "origin": prr["origin"],
                       "commands": [{"dispatch_type": c[0], "texture_page_id": c[1],
                                     "reserved_04": c[2], "tri": c[3], "quad": c[4],
                                     "vptr": c[5]} for c in prr["commands"]],
                       "vertices": verts, "normals": normals})
    if td5["kind"] == "himodel":
        return {"kind": "himodel", "mesh": meshes[0]}
    return {"kind": "models", "entry_count": td5["entry_count"],
            "entries": td5["entries"], "meshes": meshes}


def selfcheck_glb(path):
    """decode -> glb -> import -> rebuild -> decode  must equal decode(original)."""
    kind = "himodel" if "himodel" in os.path.basename(path).lower() else "models"
    data = open(path, "rb").read()
    m0 = decode(data, kind)
    label = f"{os.path.basename(os.path.dirname(path))}/{os.path.basename(path)}"
    if kind == "himodel" and m0.get("mesh") is None:
        print(f"SKIP  {label}  (TD6 0x104 indexed)")
        return True
    glb = export_glb(m0)
    m1 = import_glb(glb)
    m2 = decode(build_dat(m1), kind)
    ok, why = model_equal(m0, m2)
    print(f"{'PASS' if ok else 'FAIL'}  {label}  glb={len(glb)}B"
          + ("" if ok else f"  [{why}]"))
    return ok


def selfcheck(path):
    kind = "himodel" if "himodel" in os.path.basename(path).lower() else "models"
    data = open(path, "rb").read()
    m0 = decode(data, kind)
    label = f"{os.path.basename(os.path.dirname(path))}/{os.path.basename(path)}"

    if kind == "himodel" and m0.get("mesh") is None:
        print(f"SKIP  {label}  (TD6 0x104 indexed himodel — not yet in glTF tool)")
        return True

    rebuilt = build_dat(m0)
    m1 = decode(rebuilt, kind)
    ok, why = model_equal(m0, m1)
    if kind == "himodel":
        nmesh, nverts, nent = 1, len(m0["mesh"]["vertices"]), "-"
    else:
        nmesh = len(m0["meshes"])
        nverts = sum(len(m["vertices"]) for m in m0["meshes"])
        nent = m0["entry_count"]
    tag = "PASS" if ok else "FAIL"
    print(f"{tag}  {label} entries={nent} meshes={nmesh} verts={nverts}"
          + ("" if ok else f"  [{why}]"))
    return ok


# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    mode = sys.argv[1]
    def corpus(root):
        return (sorted(glob.glob(f"{root}/levels/level*/models.dat"))
                + sorted(glob.glob(f"{root}/cars/*/himodel.dat")))

    if mode in ("selfcheck", "selfcheck-glb"):
        fn = selfcheck if mode == "selfcheck" else selfcheck_glb
        sys.exit(0 if fn(sys.argv[2]) else 1)
    elif mode in ("selfcheck-all", "selfcheck-glb-all"):
        fn = selfcheck if mode == "selfcheck-all" else selfcheck_glb
        root = sys.argv[2] if len(sys.argv) > 2 else "re/assets"
        files = corpus(root)
        bad = sum(0 if fn(f) else 1 for f in files)
        print(f"\nmesh {mode}: {len(files)} checked, {bad} failed")
        sys.exit(1 if bad else 0)
    elif mode == "export":
        src, dst = sys.argv[2], sys.argv[3]
        kind = "himodel" if "himodel" in os.path.basename(src).lower() else "models"
        m = decode(open(src, "rb").read(), kind)
        if kind == "himodel" and m.get("mesh") is None:
            print("TD6 0x104 indexed himodel not supported yet", file=sys.stderr)
            sys.exit(2)
        open(dst, "wb").write(export_glb(m))
        print(f"exported {dst}")
    elif mode == "import":
        src, dst = sys.argv[2], sys.argv[3]
        open(dst, "wb").write(build_dat(import_glb(open(src, "rb").read())))
        print(f"imported {dst}")
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
