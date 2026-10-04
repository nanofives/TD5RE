#!/usr/bin/env python3
"""
car_mesh_uv.py  [TD5 CAR PAINT GEOMETRY 2026-10-04]

Geometry side of the TD5 car-paint bake: read a car's himodel.bin and answer, per
SKIN TEXEL, two questions the image alone cannot:

    covered(x,y)   does any BODY face actually sample this texel?
    pos(x,y)       which MODEL-SPACE point does this texel sit on?

plus the authored hardpoints from carparam.json (wheel centres, lamp centres),
so a texel can be tested against real car geometry instead of against its colour.

WHY THIS EXISTS. bake_td5_car_paint.py classifies paint from the four factory
skins, which is the right primary evidence -- only the paint changes between
carskin0..3. But its clean-up stage (binary closing, fill-holes, "grow into dark
varying neighbours") is purely topological: it cannot tell a 1-texel specular
pinhole it SHOULD close from a headlight lens it should not, so it leaks across
part boundaries. Mariano, 2026-10-04: "wheels, part of the windows, headlights
still wrong". Geometry is the missing discriminator.

WHAT THE MESH DOES AND DOES NOT CARRY (measured, not assumed):

  * himodel.bin is a PRR mesh -- header(0x38) + commands(0x10) + vertices(0x2C) +
    normals(0x10). Layout authority: re/tools/mesh_tool.py, mirrored by
    TD5_MeshHeader / TD5_PrimitiveCmd / TD5_MeshVertex in td5_types.h.
  * There is NO per-face material flag. Measured over the shipped cars: every car
    is exactly TWO commands -- page 7 (body skin; ss1 ships 36) and page 8
    (hub/chassis underside, retargeted to the chassis sprite at load,
    td5_asset.c:4340). Vertex `lighting` is the constant 0xFFFFFFFF and
    proj_u/proj_v are all zero, so neither carries a glass/lamp bit.
    => the mesh cannot NAME a part. It can only say WHERE a texel is.
  * carparam.json does carry authored hardpoints, and those name parts:
      wheel_pos_FL/FR/RL/RR  (tuning +0x40..+0x58)  -- 4 wheel centres
      tuning +0x60 / +0x70   -- two symmetric point PAIRS

    The +0x60 pair is CONFIRMED to be the taillights: td5_render_effects.c:1322
    "Hardpoints: car_config+0x60 (left), car_config+0x68 (right)", the port's
    brake-light draw, corresponding to RenderVehicleTaillightQuads @ 0x004011C0.
    The +0x70 pair is read as the HEADLIGHTS here, which is [INFERRED], not
    confirmed: carparam.json labels both ranges traffic_alt_wheel_A/B because
    ComputeVehicleSuspensionEnvelope overwrites them for traffic slots (>= 6), so
    the shipped bytes are only meaningful for player/AI cars. The inference rests
    on measurement over all 27 paint-baked cars: in every one, both ranges hold a
    mirrored (+x,-x) pair at mid height sitting at the longitudinal EXTREME --
    |z| within 12% of the mesh bbox -- with +0x60 at the tail and +0x70 at the
    nose. A traffic wheel template would not be mirrored at the nose and tail at
    lamp height. `lamp_points()` re-checks that shape per car and drops the pair
    when it does not hold, so a car with genuinely different bytes degrades to
    "no lamp hardpoint" rather than vetoing the wrong texels.

Scale note: hardpoints are int16 in the SAME model space as the mesh vertices
(jag wheels x=+-256 against a mesh bbox of x=+-328), so no conversion is needed.

Usage as a tool (prints a per-car geometry summary; writes nothing):
    python car_mesh_uv.py            # every car with a himodel.bin
    python car_mesh_uv.py jag gto
"""
import os, sys, json, glob, struct
import numpy as np

CARS_DIR = os.path.join(os.path.dirname(__file__), "..", "assets", "cars")

HDR = 0x38      # PRR header
CMD = 0x10      # primitive command
VTX = 0x2C      # vertex
NRM = 0x10      # normal

MESH_RT_TD5 = 0x103     # native expanded-vertex mesh (TD6 ships 0x104 indexed)
HUB_PAGE    = 8         # TD5_CAR_MESH_HUB_ID -- hub/chassis underside

# Barycentric slack, copied from the runtime rasteriser (td5_asset.c:3841) so the
# offline coverage map and the in-game pattern map claim the same edge texels.
BARY_SLACK = -0.02


class CarMesh(object):
    """Decoded himodel.bin. `cmds` is [(page, tri_count, quad_count)]."""

    def __init__(self, path):
        d = open(path, "rb").read()
        if len(d) < HDR:
            raise ValueError("himodel too small (%d bytes)" % len(d))
        self.render_type, _tp, ncmd, nvtx = struct.unpack_from("<hhii", d, 0)
        coff, voff, noff = struct.unpack_from("<III", d, 0x2C)
        if ncmd <= 0 or nvtx <= 0:
            raise ValueError("himodel has no geometry")
        self.cmds = []
        for c in range(ncmd):
            _dt, pg, _r4, tri, quad = struct.unpack_from("<hhiHH", d, coff + c * CMD)
            self.cmds.append((pg, tri, quad))
        if voff + nvtx * VTX > len(d):
            raise ValueError("vertex block runs past EOF")
        # pos(3) + uv(2); view/lighting/proj are runtime-only or constant.
        raw = np.frombuffer(d, np.uint8, count=nvtx * VTX, offset=voff)
        raw = raw.reshape(nvtx, VTX)
        self.pos = raw[:, 0x00:0x0C].copy().view(np.float32).reshape(nvtx, 3).astype(np.float64)
        self.uv = raw[:, 0x1C:0x24].copy().view(np.float32).reshape(nvtx, 2).astype(np.float64)
        self.nrm = None
        if noff and noff + nvtx * NRM <= len(d):
            nr = np.frombuffer(d, np.uint8, count=nvtx * NRM, offset=noff).reshape(nvtx, NRM)
            self.nrm = nr[:, 0:0x0C].copy().view(np.float32).reshape(nvtx, 3).astype(np.float64)
        # Sum of per-command vertex use must equal the header count, or the
        # sequential walk below is reading the wrong vertices for every command
        # after the first. This is the one invariant that silently produces a
        # plausible-looking but wrong map, so it is checked, not assumed.
        used = sum(t * 3 + q * 4 for (_p, t, q) in self.cmds)
        if used != nvtx:
            raise ValueError("command vertex sum %d != header count %d" % (used, nvtx))

    def triangles(self, body_only=True):
        """(page, i0, i1, i2) per triangle, using the port's quad fan split
        (td5_asset.c:3812-3817) so offline and runtime tessellate identically."""
        cur = 0
        for (pg, tri, quad) in self.cmds:
            base = cur
            cur += tri * 3 + quad * 4
            if body_only and pg == HUB_PAGE:
                continue
            for t in range(tri):
                a = base + t * 3
                yield pg, a, a + 1, a + 2
            for q in range(quad):
                a = base + tri * 3 + q * 4
                yield pg, a, a + 1, a + 2
                yield pg, a, a + 2, a + 3

    def bbox(self, body_only=True):
        idx = []
        for _pg, i0, i1, i2 in self.triangles(body_only):
            idx += [i0, i1, i2]
        p = self.pos[np.unique(idx)]
        return p.min(0), p.max(0)


def rasterise(mesh, sw, sh, body_only=True):
    """Per-texel coverage + model-space position.

    Returns (covered HxW bool, pos HxWx3 float32).

    Overlapping UV charts resolve last-writer-wins, matching the runtime map
    (td5_asset.c:3848-3852, which also just overwrites). Measured over the
    shipped cars the body charts do not overlap, so the rule is not load-bearing.
    """
    cov = np.zeros((sh, sw), bool)
    pos = np.zeros((sh, sw, 3), np.float32)
    U = mesh.uv
    P = mesh.pos
    for _pg, i0, i1, i2 in mesh.triangles(body_only):
        ax, ay = U[i0, 0] * sw, U[i0, 1] * sh
        bx, by = U[i1, 0] * sw, U[i1, 1] * sh
        cx, cy = U[i2, 0] * sw, U[i2, 1] * sh
        den = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
        if abs(den) < 1e-6:
            continue                                  # degenerate in UV space
        x0 = max(int(np.floor(min(ax, bx, cx))) - 1, 0)
        x1 = min(int(np.ceil(max(ax, bx, cx))) + 1, sw - 1)
        y0 = max(int(np.floor(min(ay, by, cy))) - 1, 0)
        y1 = min(int(np.ceil(max(ay, by, cy))) + 1, sh - 1)
        if x1 < x0 or y1 < y0:
            continue
        xs = np.arange(x0, x1 + 1) + 0.5
        ys = np.arange(y0, y1 + 1) + 0.5
        px, py = np.meshgrid(xs, ys)
        w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / den
        w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / den
        w2 = 1.0 - w0 - w1
        m = (w0 >= BARY_SLACK) & (w1 >= BARY_SLACK) & (w2 >= BARY_SLACK)
        if not m.any():
            continue
        q = (w0[..., None] * P[i0] + w1[..., None] * P[i1] + w2[..., None] * P[i2])
        sub_p = pos[y0:y1 + 1, x0:x1 + 1]
        sub_c = cov[y0:y1 + 1, x0:x1 + 1]
        sub_p[m] = q[m]
        sub_c[m] = True
    return cov, pos


def _pair(v):
    """Two int16[4] points packed in one sint16[8] value -> [(x,y,z),(x,y,z)]."""
    if not v or len(v) < 7:
        return None
    return [tuple(float(t) for t in v[0:3]), tuple(float(t) for t in v[4:7])]


def load_hardpoints(car_dir, mesh):
    """Authored wheel + lamp centres in model space, from carparam.json.

    Returns dict(wheels=[(x,y,z)x4], lamps_rear=[..x2], lamps_front=[..x2]),
    with any entry that fails its shape check dropped (empty list), never
    guessed. See the module docstring for why the lamp ranges need a check.
    """
    out = {"wheels": [], "lamps_rear": [], "lamps_front": []}
    p = os.path.join(car_dir, "carparam.json")
    if not os.path.exists(p):
        return out
    try:
        j = json.load(open(p, "r", encoding="utf-8"))
    except Exception:
        return out

    def val(key):
        e = j.get(key)
        return e.get("value") if isinstance(e, dict) else None

    for k in ("FL", "FR", "RL", "RR"):
        v = val("wheel_pos_" + k)
        if v and len(v) >= 3:
            out["wheels"].append(tuple(float(t) for t in v[0:3]))
    if len(out["wheels"]) != 4:
        out["wheels"] = []

    lo, hi = mesh.bbox(body_only=True)
    zspan = float(hi[2] - lo[2])
    if zspan < 1.0:
        return out
    # A lamp pair must be MIRRORED in x and sit at the longitudinal extreme.
    # 0.12 of the bbox span: measured over the 27 paint-baked cars the real
    # pairs all land within 0.08, and nothing else in the file does.
    for key, slot, want_tail in (("traffic_alt_wheel_A", "lamps_rear", True),
                                 ("traffic_alt_wheel_B", "lamps_front", False)):
        pr = _pair(val(key))
        if not pr:
            continue
        (x0, _y0, z0), (x1, _y1, z1) = pr
        mirrored = abs(x0 + x1) <= 0.20 * max(abs(x0), abs(x1), 1.0) and abs(x0) > 1.0
        at_end = all(
            (abs(z - lo[2]) <= 0.12 * zspan) if want_tail else (abs(z - hi[2]) <= 0.12 * zspan)
            for z in (z0, z1))
        if mirrored and at_end:
            out[slot] = pr
    return out


def distance_to(pos, cov, points):
    """Per-texel distance to the NEAREST of `points` (model space).

    Uncovered texels get +inf: they have no model-space position, so any
    distance computed there is meaningless and must never pass a radius test.
    """
    d = np.full(pos.shape[:2], np.inf, np.float64)
    if not points:
        return d
    for p in points:
        dd = np.linalg.norm(pos - np.asarray(p, np.float32), axis=-1)
        d = np.minimum(d, dd)
    d[~cov] = np.inf
    return d


def load_car_geometry(code, cars_dir=None):
    """Everything the baker needs, or None when the car has no usable TD5 mesh."""
    cars_dir = cars_dir or CARS_DIR
    d = os.path.join(cars_dir, code)
    hm = os.path.join(d, "himodel.bin")
    if not os.path.exists(hm):
        return None
    # Check the render type BEFORE decoding: a TD6 indexed mesh (0x104) is a
    # different record layout, so CarMesh would raise on it. Those cars ship a
    # hand-made carmask and the baker skips them -- silence, not a warning.
    try:
        with open(hm, "rb") as f:
            rt = struct.unpack("<h", f.read(2))[0]
    except Exception:
        return None
    if rt != MESH_RT_TD5:
        return None
    try:
        mesh = CarMesh(hm)
    except Exception as e:
        print("  %s: himodel unusable (%s) -> no geometry gate" % (code, e))
        return None
    return dict(mesh=mesh, dir=d, hard=load_hardpoints(d, mesh))


def texel_geometry(geo, sw, sh):
    """Rasterise `geo`'s mesh into a sw x sh skin and attach hardpoint distances."""
    cov, pos = rasterise(geo["mesh"], sw, sh, body_only=True)
    h = geo["hard"]
    return dict(
        covered=cov,
        pos=pos,
        wheel_d=distance_to(pos, cov, h["wheels"]),
        lamp_d=distance_to(pos, cov, h["lamps_rear"] + h["lamps_front"]),
        n_wheel=len(h["wheels"]),
        n_lamp=len(h["lamps_rear"]) + len(h["lamps_front"]),
    )


def main():
    codes = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not codes:
        codes = sorted(os.path.basename(p) for p in glob.glob(os.path.join(CARS_DIR, "*"))
                       if os.path.isdir(p))
    print("%-5s %-9s %6s %6s %6s  %s" % ("car", "skin", "cov%", "wheels", "lamps", "bbox(x,y,z)"))
    for c in codes:
        geo = load_car_geometry(c)
        if not geo:
            continue
        import PIL.Image as Image
        sp = os.path.join(geo["dir"], "carskin0.png")
        if not os.path.exists(sp):
            continue
        sw, sh = Image.open(sp).size
        tg = texel_geometry(geo, sw, sh)
        lo, hi = geo["mesh"].bbox()
        print("%-5s %-9s %6.1f %6d %6d  (%.0f..%.0f, %.0f..%.0f, %.0f..%.0f)" % (
            c, "%dx%d" % (sw, sh), tg["covered"].mean() * 100, tg["n_wheel"], tg["n_lamp"],
            lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))


if __name__ == "__main__":
    main()
