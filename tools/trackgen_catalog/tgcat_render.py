"""tgcat_render.py -- offline textured thumbnail of a set of faces.

Orthographic, z-buffered, 2x supersampled, numpy. Each face is (page, [(pos, uv)
x3|x4]); quads are split into two triangles. Texture lookup is the page's 64x64
RGBA image with UV wrap (the generator tiles pages with UV > 1), alpha-tested
for colour-keyed pages (format 1, index 0 transparent) like the game does.
Lighting is a face-normal cosine plus ambient: enough to read the shape.

View choice:
  * every face in one plane (billboard tree / prop / sign)  -> face its front head on
  * otherwise the 3/4 view (of four) that shows the most area, front faces first
  * mostly horizontal faces (road, pavement, ground)        -> steep 3/4 view
  * anything else                                           -> 3/4 view
"""
from __future__ import annotations

import math

import numpy as np
from PIL import Image

BG = (24, 26, 30, 255)


def _normal(pts):
    p = np.asarray(pts, dtype=np.float64)
    n = np.zeros(3)
    for i in range(len(p)):
        a, b = p[i], p[(i + 1) % len(p)]
        n += [(a[1] - b[1]) * (a[2] + b[2]), (a[2] - b[2]) * (a[0] + b[0]),
              (a[0] - b[0]) * (a[1] + b[1])]
    ln = np.linalg.norm(n)
    return n / ln if ln > 1e-9 else np.array([0.0, 1.0, 0.0])


def _view_basis(yaw_deg, pitch_deg):
    y, p = math.radians(yaw_deg), math.radians(pitch_deg)
    fwd = np.array([math.sin(y) * math.cos(p), -math.sin(p), math.cos(y) * math.cos(p)])
    right = np.array([math.cos(y), 0.0, -math.sin(y)])
    up = np.cross(fwd, right)
    return right, up, fwd


def choose_view(faces):
    normals = [_normal([q for q, _ in f]) for _, f in faces]
    if not normals:
        return 35.0, 30.0
    n0 = normals[0]
    if all(abs(float(np.dot(n, n0))) > 0.995 for n in normals) and abs(n0[1]) < 0.5:
        # planar vertical piece: look straight at its FRONT. TD5 winds front
        # faces clockwise as seen from the front (D3D), so the Newell normal
        # points AWAY from a viewer who reads the texture the right way round.
        yaw = math.degrees(math.atan2(n0[0], n0[2]))
        return yaw, 0.0
    horiz = sum(1 for n in normals if abs(n[1]) > 0.8)
    pitch = 55.0 if horiz > 0.7 * len(normals) else 28.0
    # Of the four 3/4 views, keep the one that shows the most face area (a
    # row of walls seen edge-on reads as nothing).
    areas = []
    for _, f in faces:
        p = np.asarray([q for q, _ in f], dtype=np.float64)
        a = np.zeros(3)
        for i in range(1, len(p) - 1):
            a += np.cross(p[i] - p[0], p[i + 1] - p[0])
        areas.append(a * 0.5)
    best, best_yaw = -1.0, 35.0
    for yaw in (35.0, 125.0, 215.0, 305.0):
        fwd = _view_basis(yaw, pitch)[2]
        # visible area, plus a bonus for seeing faces from their front (see
        # above: front-facing <=> Newell area vector along the view direction)
        vis = sum(abs(float(np.dot(a, fwd))) + 0.5 * float(np.dot(a, fwd)) for a in areas)
        if vis > best + 1e-6:
            best, best_yaw = vis, yaw
    return best_yaw, pitch


def render(faces, textures, size=256, yaw=None, pitch=None, ss=2, bg=BG,
           highlight=None):
    """faces: [(page, [(pos, uv), ...])]; textures: {page: RGBA PIL image}.
    highlight: optional set of face indices drawn normally while the rest are
    greyed (context render)."""
    if yaw is None or pitch is None:
        yaw, pitch = choose_view(faces)
    right, up, fwd = _view_basis(yaw, pitch)
    W = size * ss
    tris = []
    for fi, (page, f) in enumerate(faces):
        pts = [np.asarray(p, dtype=np.float64) for p, _ in f]
        uvs = [uv for _, uv in f]
        n = _normal(pts)
        for (a, b, c) in ((0, 1, 2), (0, 2, 3))[: (1 if len(f) == 3 else 2)]:
            tris.append((fi, page, pts[a], pts[b], pts[c], uvs[a], uvs[b], uvs[c], n))
    if not tris:
        return Image.new("RGBA", (size, size), bg)
    allp = np.array([[t[2], t[3], t[4]] for t in tris]).reshape(-1, 3)
    sx, sy, sz = allp @ right, allp @ up, allp @ fwd
    x0, x1, y0, y1 = sx.min(), sx.max(), sy.min(), sy.max()
    span = max(x1 - x0, y1 - y0, 1e-6)
    scale = (W * 0.86) / span
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2

    color = np.zeros((W, W, 3), dtype=np.float32)
    color[:] = np.array(bg[:3], dtype=np.float32)
    depth = np.full((W, W), np.inf, dtype=np.float64)
    light = np.array([0.35, 0.85, 0.40])
    light /= np.linalg.norm(light)
    texcache = {}

    for (fi, page, a, b, c, ua, ub, uc, n) in tris:
        if page not in texcache:
            img = textures.get(page)
            texcache[page] = (np.asarray(img, dtype=np.float32) if img is not None
                              else None)
        tex = texcache[page]
        P = np.array([a, b, c])
        X = (P @ right - cx) * scale + W / 2
        Y = W / 2 - (P @ up - cy) * scale
        Z = P @ fwd
        xmin, xmax = int(max(0, math.floor(X.min()))), int(min(W - 1, math.ceil(X.max())))
        ymin, ymax = int(max(0, math.floor(Y.min()))), int(min(W - 1, math.ceil(Y.max())))
        if xmin > xmax or ymin > ymax:
            continue
        den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2])
        if abs(den) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(xmin, xmax + 1) + 0.5,
                             np.arange(ymin, ymax + 1) + 0.5)
        w0 = ((Y[1] - Y[2]) * (gx - X[2]) + (X[2] - X[1]) * (gy - Y[2])) / den
        w1 = ((Y[2] - Y[0]) * (gx - X[2]) + (X[0] - X[2]) * (gy - Y[2])) / den
        w2 = 1.0 - w0 - w1
        inside = (w0 >= -1e-6) & (w1 >= -1e-6) & (w2 >= -1e-6)
        if not inside.any():
            continue
        z = w0 * Z[0] + w1 * Z[1] + w2 * Z[2]
        sub = depth[ymin:ymax + 1, xmin:xmax + 1]
        vis = inside & (z < sub)
        if not vis.any():
            continue
        shade = 0.55 + 0.45 * abs(float(np.dot(n, light)))
        if tex is not None:
            u = w0 * ua[0] + w1 * ub[0] + w2 * uc[0]
            v = w0 * ua[1] + w1 * ub[1] + w2 * uc[1]
            tx = (np.floor((u % 1.0) * 64).astype(np.int64)) % 64
            ty = (np.floor((v % 1.0) * 64).astype(np.int64)) % 64
            texel = tex[ty, tx]
            vis = vis & (texel[..., 3] > 0)
            rgb = texel[..., :3] * shade
        else:
            rgb = np.broadcast_to(np.array([200, 60, 200], dtype=np.float32) * shade,
                                  gx.shape + (3,))
        if highlight is not None and fi not in highlight:
            g = rgb.mean(axis=-1, keepdims=True) * 0.45 + 40
            rgb = np.repeat(g, 3, axis=-1)
        sub[vis] = z[vis]
        csub = color[ymin:ymax + 1, xmin:xmax + 1]
        csub[vis] = rgb[vis]

    img = Image.fromarray(np.clip(color, 0, 255).astype(np.uint8), "RGB")
    if ss > 1:
        img = img.resize((size, size), Image.LANCZOS)
    return img.convert("RGBA")
