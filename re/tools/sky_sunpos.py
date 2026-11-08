#!/usr/bin/env python3
"""Sun placement vs the skybox (round UC, item 7).

The sun disc (td5_render_draw_sun_disc) and, with RT on, the scene sun of the
shadow / SSR / car-sun passes all use ONE direction: s_sky_sun_dir, probed from
the sky panorama at load (td5_render_effects.c sky_probe_classify +
sky_probe_resolve_dir). That probe looks for the brightest region in the top 55%
of the image and does not know what the texels show, so on several tracks the
sun lands on mountains, buildings or ground painted into the panorama.

This tool:
  1. replicates the C probe exactly (class + old direction), per sky image;
  2. maps any direction to the texel the dome draws there (ray / flat-triangle
     hit on re/assets/static/sky.prr, barycentric UV == what the rasteriser
     interpolates);
  3. labels every texel sky / not-sky with a per-column SKYLINE: the panorama is
     sky above one boundary and backdrop (ground, mountains, buildings, water)
     below it, so a dynamic programme picks the boundary row per column that
     best separates "looks like the sky seed at the top" from "does not",
     with a smoothness cost and wrap-around (the panorama tiles horizontally);
  4. for every SUNNY sky whose old sun footprint (disc + glare radius + margin)
     touches non-sky, picks the nearest direction (great-circle) whose whole
     footprint is sky, inside a believable elevation band;
  5. writes the C table td5mod/src/td5re/td5_sky_sunpos.h (keyed by a hash of
     the decoded pixels, so the auto track's borrowed FORWSKY and the free-roam
     host skies, which are byte copies of shipped panoramas, match too) and a
     contact sheet with the mask, the old sun and the new sun drawn on.

Usage:
  python re/tools/sky_sunpos.py                    # table + sheet + report
  python re/tools/sky_sunpos.py --sheet out.png --csv out.csv --no-table
"""
import argparse, glob, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
DOME = os.path.join(ROOT, 're', 'assets', 'static', 'sky.prr')
LEVELS = os.path.join(ROOT, 're', 'assets', 'levels')
TABLE = os.path.join(ROOT, 'td5mod', 'src', 'td5re', 'td5_sky_sunpos.h')

# ---- C probe constants (td5_render_effects.c, keep in lock-step) ----------
PROBE_HORIZON = 0.55
PROBE_AREA = 0.20
PROBE_SAT = 0.18
PROBE_BLUE = 0.55
PROBE_RATIO = 1.8
PROBE_PEAK = 200.0
NIGHT_THR = 80.0
MIN_ELEV = 0.15            # TD5RE_SUN_MIN_ELEV default (sin of the elevation)

# ---- sun footprint (td5_render_draw_sun_disc defaults) --------------------
DISC_TAN = 0.03            # TD5RE_SUN_DISC_SIZE: core half-size = tan(radius)
GLARE_MUL = 2.6            # outer glare quad = 2.6 x core
CLEAR_DEG = math.degrees(math.atan(DISC_TAN * GLARE_MUL)) + 1.0   # glare + 1 deg slack
# believable elevation band for a MOVED sun (degrees above the dome horizon)
EL_MIN, EL_MAX = 10.0, 40.0

# Eye-validated corrections from the contact sheet (row = texel row of the 256-row
# panorama). The classifier is tuned to err CONSERVATIVE, but a few SUNNY skies need
# a hand: `floor` = every row above it is sky (cloud banks the colour model calls
# backdrop), `cap` = every row from it down is backdrop (snow the colour model
# calls cloud). Rows read off sunpos_sheet_sunny with a few rows of margin.
OVERRIDES = {
    'level004/forwsky.png': dict(floor=138),   # cumulus bank down to the hill top (row ~144)
    'level010/forwsky.png': dict(floor=136),   # cloud sheet down to the ridge (row ~142)
    'level011/forwsky.png': dict(floor=120),   # cumulus down to the peak (row ~126) / towers (~143)
    'level020/forwsky.png': dict(floor=134),   # clouds down to the sea horizon (row ~139)
    'level028/forwsky.png': dict(floor=138),   # clouds down to the tree line (row ~144)
    'level025/forwsky.png': dict(cap=138),     # snow ridge (top row ~143) reads as cloud
    'level027/forwsky.png': dict(cap=124),     # dark summit (top row ~128) reads as sky
}

SKYLINE_COLS = 64          # skyline samples stored per sky in the C table


# ============================ dome ==========================================
def load_dome():
    b = open(DOME, 'rb').read()
    nv = struct.unpack_from('<i', b, 8)[0]
    vo = struct.unpack_from('<I', b, 0x30)[0]
    pos = np.zeros((nv, 3)); uv = np.zeros((nv, 2))
    for i in range(nv):
        f = struct.unpack_from('<6fI4f', b, vo + i * 44)
        pos[i] = f[0:3]; uv[i] = f[7:9]
    return pos, uv


def dirs_to_uv(dirs, pos, uv):
    """dirs: (N,3) dome-model space (+Y up). Returns (N,2) UV, NaN where no hit."""
    tri = pos.reshape(-1, 3, 3); tuv = uv.reshape(-1, 3, 2)
    v0, e1, e2 = tri[:, 0], tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0]
    out = np.full((len(dirs), 2), np.nan)
    best = np.full(len(dirs), np.inf)
    for c0 in range(0, len(dirs), 2048):
        d = dirs[c0:c0 + 2048][:, None, :]                    # (n,1,3)
        p = np.cross(d, e2[None])                               # (n,T,3)
        det = np.einsum('ntk,tk->nt', p, e1)
        ok = np.abs(det) > 1e-9
        inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
        s = -v0[None]                                           # ray origin 0
        u = np.einsum('ntk,tk->nt', p, -v0) * inv
        q = np.cross(np.broadcast_to(s, p.shape), e1[None])
        v = np.einsum('ntk,nk->nt', q, d[:, 0]) * inv
        t = np.einsum('ntk,tk->nt', q, e2) * inv
        hit = ok & (u >= -1e-6) & (v >= -1e-6) & (u + v <= 1 + 1e-6) & (t > 0)
        t = np.where(hit, t, np.inf)
        k = np.argmin(t, axis=1); tb = t[np.arange(len(k)), k]
        has = np.isfinite(tb)
        uu = u[np.arange(len(k)), k]; vv = v[np.arange(len(k)), k]
        w0 = 1 - uu - vv
        res = (w0[:, None] * tuv[k, 0] + uu[:, None] * tuv[k, 1] + vv[:, None] * tuv[k, 2])
        out[c0:c0 + 2048][has] = res[has]
        best[c0:c0 + 2048] = tb
    return out


def azel_to_dir(az, el):
    """az: atan2(x, z) degrees; el degrees; dome model space +Y up."""
    a = np.radians(az); e = np.radians(el)
    return np.stack([np.cos(e) * np.sin(a), np.sin(e), np.cos(e) * np.cos(a)], axis=-1)


def dir_to_azel(d):
    d = np.asarray(d, float); d = d / np.linalg.norm(d)
    return math.degrees(math.atan2(d[0], d[2])) % 360.0, math.degrees(math.asin(d[1]))


# ============================ C probe replica ===============================
def c_probe(rgb, pos, uv):
    """Exact replica of sky_probe_classify + sky_probe_resolve_dir. Returns
    (cls, sun_uv, dome_dir_up or None, rgb)."""
    h, w, _ = rgb.shape
    md = max(w, h); f = 1
    while md // f > 128: f += 1
    dw, dh = max(1, w // f), max(1, h // f)
    a = rgb[:dh * f, :dw * f].astype(np.float64).reshape(dh, f, dw, f, 3).mean(axis=(1, 3))
    cr, cg, cb = a[..., 0], a[..., 1], a[..., 2]
    L = (cr + cg + cb) / 3.0
    Lmean = L.mean()
    band = max(1, int(dh * PROBE_HORIZON))
    Lb = L[:band]; Lpeak = Lb.max()
    mx = np.maximum(np.maximum(cr, cg), cb)[:band]; mn = np.minimum(np.minimum(cr, cg), cb)[:band]
    sat = np.where(mx > 1.0, (mx - mn) / np.where(mx > 1.0, mx, 1), 0).mean()
    blue = ((cb >= cr) & (cb >= cg))[:band].mean()
    pk = Lb >= 0.90 * Lpeak
    ys, xs = np.nonzero(pk)
    wts = Lb[pk]
    su = (xs * wts).sum() / wts.sum() / max(1, dw - 1)
    sv = (ys * wts).sum() / wts.sum() / max(1, dh - 1)
    srgb = (cr[:band][pk].mean(), cg[:band][pk].mean(), cb[:band][pk].mean())
    area = pk.sum() / float(band * dw)
    ratio = Lpeak / max(Lmean, 1.0)
    tight = ratio >= PROBE_RATIO and Lpeak >= PROBE_PEAK and area < PROBE_AREA
    clear_blue = blue >= PROBE_BLUE and sat >= PROBE_SAT and Lmean >= NIGHT_THR
    if Lmean < NIGHT_THR: cls = 'NIGHT'
    elif tight or clear_blue: cls = 'SUNNY'
    else: cls = 'OVERCAST'
    if cls != 'SUNNY':
        return cls, (su, sv), None, srgb
    # nearest dome vertex by UV, first-wins on ties (C: strict <)
    d2 = (uv[:, 0] - np.float32(su)) ** 2 + (uv[:, 1] - np.float32(sv)) ** 2
    bi = int(np.argmin(d2))
    p = pos[bi]; m = np.linalg.norm(p)
    dx, dy, dz = p[0] / m, -p[1] / m, p[2] / m          # position space (+Y down)
    if dy > -MIN_ELEV:
        hl = math.hypot(dx, dz)
        if hl < 1e-4: dx, dz, hl = 0.0, -1.0, 1.0
        hz = math.sqrt(1 - MIN_ELEV * MIN_ELEV)
        dx, dz, dy = dx / hl * hz, dz / hl * hz, -MIN_ELEV
    return cls, (su, sv), np.array([dx, -dy, dz]), srgb   # back to +Y up


# ============================ sky / not-sky =================================
def box3(a, r):
    k = 2 * r + 1
    p = np.pad(np.pad(a, ((0, 0), (r, r)), mode='wrap'), ((r, r), (0, 0)), mode='edge')  # panorama wraps in x only
    c = np.cumsum(np.cumsum(p, 0), 1)
    c = np.pad(c, ((1, 0), (1, 0)))
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def kmeans(pts, k, iters=15, seed=1):
    rng = np.random.default_rng(seed)
    k = min(k, len(pts))
    cent = pts[rng.choice(len(pts), k, replace=False)].copy()
    for _ in range(iters):
        lab = ((pts[:, None] - cent[None]) ** 2).sum(-1).argmin(1)
        for j in range(k):
            if (lab == j).any(): cent[j] = pts[lab == j].mean(0)
    return cent


def texel_features(rgb):
    x = rgb.astype(np.float64)
    L = x.mean(axis=2)
    o1 = (x[..., 0] - x[..., 1]); o2 = (x[..., 0] + x[..., 1]) / 2 - x[..., 2]
    gy = np.abs(np.diff(L, axis=0, append=L[-1:]))
    gx = np.abs(np.diff(L, axis=1, append=L[:, :1]))
    tex = box3(gx + gy, 1)
    # luma, two opponent-colour axes (hue weighs as much as brightness), texture
    return np.stack([L, 1.5 * o1, 1.5 * o2, TEX_W * tex], axis=-1)


TEX_W = 2.5
EDGE_BETA = 6.0
FILL_STEP = 6.0
FILL_ROW_FRAC = 0.5
FRONT_BIAS = 0.62


def groundness(rgb, horizon_row):
    """0..1 per texel: relative likelihood of BACKDROP vs SKY, plus a per-row
    EDGE strength used by the skyline DP.
    Sky model  = k-means of the top 10% rows (the dome draws them at 42-48 deg
                 elevation: sky in every shipped panorama).
    Backdrop   = k-means of the rows below the dome horizon (ground, water,
                 reflections), minus every texel the sky model already explains
                 (cloud reflections, a sky-coloured sea): a panorama with no
                 backdrop content then has no backdrop model and stays sky.
    g = d_sky / (d_sky + d_backdrop)."""
    f = texel_features(rgb)
    h, w, _ = f.shape
    top = f[:max(4, h // 10)].reshape(-1, 4)
    sky = kmeans(top, 6)
    d_top = np.sqrt(((top[:, None] - sky[None]) ** 2).sum(-1)).min(-1)
    tol = np.percentile(d_top, 90) + 4.0
    bot = f[min(h - 2, horizon_row + 3):].reshape(-1, 4)
    d_bot = np.sqrt(((bot[:, None] - sky[None]) ** 2).sum(-1)).min(-1)
    bot = bot[d_bot > tol]
    ds = np.sqrt(((f[:, :, None, :] - sky[None, None]) ** 2).sum(-1)).min(-1)
    if len(bot) < 0.05 * w * (h - horizon_row):
        g = np.clip((ds - tol) / (2.0 * tol), 0, 1) * 0.6     # nothing to compare to: lean sky
    else:
        gnd = kmeans(bot, 6)
        dg = np.sqrt(((f[:, :, None, :] - gnd[None, None]) ** 2).sum(-1)).min(-1)
        g = ds / (ds + dg + 8.0)
    # skyline edge: colour change between the 3 rows above and the 3 rows below
    c = f[..., :3]
    cs = np.vstack([np.zeros((1, w, 3)), np.cumsum(c, 0)])
    edge = np.zeros((h + 1, w))
    for y in range(3, h - 2):
        up = (cs[y] - cs[y - 3]) / 3.0; dn = (cs[y + 3] - cs[y]) / 3.0
        edge[y] = np.sqrt(((up - dn) ** 2).sum(-1))
    edge = np.clip(edge / (np.percentile(edge, 99) + 1e-6), 0, 1)
    return np.clip(g, 0, 1), edge


def sky_fill(rgb, horizon_row, step_thr=None, tex_thr=None):
    """Region-grow the sky down from the top row: a texel joins when its colour
    (3x3-blurred) is within step_thr of the neighbour it is reached from and its
    texture is low. Sky gradients (sunsets, haze) pass in small steps; a ridge,
    a roofline or a tree line is a jump and stops the fill. Returns 1 = reached."""
    from collections import deque
    f = texel_features(rgb)
    h, w, _ = f.shape
    c = np.stack([box3(f[..., k], 1) for k in range(3)], -1)
    tex = f[..., 3] / TEX_W
    t_top = np.percentile(tex[:max(4, h // 10)], 75)
    if tex_thr is None: tex_thr = t_top * 1.6 + 5.0
    if step_thr is None: step_thr = FILL_STEP
    R = np.zeros((h, w), bool)
    q = deque()
    for x in range(w):
        R[0, x] = True; q.append((0, x))
    while q:
        y, x = q.popleft()
        for dy, dx in ((1, 0), (0, 1), (0, -1), (-1, 0)):
            ny, nx = y + dy, (x + dx) % w
            if ny < 0 or ny >= horizon_row or R[ny, nx]: continue
            if tex[ny, nx] > tex_thr: continue
            if np.sqrt(((c[ny, nx] - c[y, x]) ** 2).sum()) > step_thr: continue
            R[ny, nx] = True; q.append((ny, nx))
    # trust the fill only down to the first row where it covers less than
    # FILL_ROW_FRAC of the row: a skyline blocks most of a row, a leak through a
    # gap (a low pass between two peaks, a seam) does not
    frac = R.mean(1)
    below = np.nonzero(frac < FILL_ROW_FRAC)[0]
    front = int(below[0]) if len(below) else horizon_row
    R[front:] = False
    return R, front


def skyline_dp(g, horizon_row, edge=None, reached=None, floor=0, cap=None, lam=0.6, beta=EDGE_BETA):
    """Per-column boundary s(x): rows < s are sky. Minimises
    sum_{y<s} g + sum_{y>=s} (1-g) + lam*|s(x)-s(x-1)|, wrap-around in x.
    Rows >= horizon_row are forced backdrop (below the geometric horizon)."""
    h, w = g.shape
    gg = g.copy(); gg[horizon_row:] = 1.0
    above = np.vstack([np.zeros((1, w)), np.cumsum(gg, 0)])          # (h+1,w): sum g rows < s
    below = np.vstack([np.cumsum((1 - gg)[::-1], 0)[::-1], np.zeros((1, w))])  # sum (1-g) rows >= s
    unary = above + below                                              # (h+1, w)
    if edge is not None:
        unary = unary - beta * edge
    unary[horizon_row + 1:] += 1e6
    if reached is not None:
        # every texel the sky fill reached is sky: the boundary sits below the
        # lowest reached texel of the column (capped at the horizon row)
        low = np.where(reached.any(0), h - 1 - np.argmax(reached[::-1], 0), -1)
        low = np.minimum(low, (horizon_row if cap is None else min(cap, horizon_row)) - 1)
        rows = np.arange(h + 1)[:, None]
        unary = unary + 1e6 * (rows <= low[None, :])
    if floor:
        unary[:floor] += 1e6                       # rows < floor are sky
    if cap is not None:
        unary[cap + 1:] += 1e6                     # rows >= cap are backdrop                                     # s <= horizon_row
    W = 3 * w                                                          # unroll the wrap
    U = np.tile(unary, (1, 3))
    S = np.arange(h + 1)
    pen = lam * np.abs(S[:, None] - S[None, :])
    cost = U[:, 0].copy(); back = np.zeros((h + 1, W), int)
    for xx in range(1, W):
        tot = cost[None, :] + pen                                       # [s_new, s_old]
        back[:, xx] = tot.argmin(1)
        cost = tot[S, back[:, xx]] + U[:, xx]
    s = np.zeros(W, int); s[-1] = int(cost.argmin())
    for xx in range(W - 1, 0, -1):
        s[xx - 1] = back[s[xx], xx]
    return s[w:2 * w]


# ============================ placement =====================================
def footprint_dirs(d, radius_deg, n=16):
    """Center + two rings (radius/2, radius) around unit dir d."""
    d = d / np.linalg.norm(d)
    a = np.array([0, 1, 0]) if abs(d[1]) < 0.9 else np.array([1, 0, 0])
    t1 = np.cross(d, a); t1 /= np.linalg.norm(t1); t2 = np.cross(d, t1)
    out = [d]
    for r in (radius_deg / 2, radius_deg):
        tr = math.tan(math.radians(r))
        for k in range(n):
            th = 2 * math.pi * k / n
            v = d + tr * (math.cos(th) * t1 + math.sin(th) * t2)
            out.append(v / np.linalg.norm(v))
    return np.array(out)


def is_sky_uv(uvs, skyline, h, w):
    """uvs (N,2): True where the texel is above the skyline (sky)."""
    ok = np.isfinite(uvs[:, 0])
    u = np.where(ok, uvs[:, 0], 0); v = np.where(ok, uvs[:, 1], 1)
    x = np.clip((u * w).astype(int) % w, 0, w - 1)
    y = np.clip((v * h).astype(int), 0, h - 1)
    return ok & (y < skyline[x])


def fnv1a_bgr(rgb):
    """FNV-1a 32 over the decoded pixels as B,G,R bytes per pixel (alpha ignored),
    row-major, the order the C side walks its BGRA32 buffer."""
    b = np.ascontiguousarray(rgb[..., ::-1]).astype(np.uint8).tobytes()
    h = 0x811C9DC5
    # vectorised FNV is awkward; 196608 bytes per sky is fine in a loop
    for byte in b:
        h = ((h ^ byte) * 0x01000193) & 0xFFFFFFFF
    return h


def find_skies():
    out = []
    for d in sorted(glob.glob(os.path.join(LEVELS, 'level*'))):
        for n in os.listdir(d):
            if n.lower() in ('forwsky.png', 'backsky.png'):
                out.append(os.path.join(d, n))
    return out


def draw_sheet(tiles, out, pos, uv, hzv):
    S = 2; cols = 4
    TW, TH = 256 * S, 256 * S + 18
    sheet = Image.new('RGB', (cols * TW, max(1, (len(tiles) + cols - 1) // cols) * TH), (30, 30, 30))
    dr = ImageDraw.Draw(sheet)
    for i, (rel, rgb, sl, cls, old, new, oc) in enumerate(tiles):
        h, w, _ = rgb.shape
        im = rgb.astype(np.float64).copy()
        mask = np.arange(h)[:, None] >= sl[None, :]
        im[mask] = im[mask] * 0.45 + np.array([255, 0, 160]) * 0.55
        im = Image.fromarray(im.astype(np.uint8)).resize((w * S, h * S), Image.NEAREST)
        x0 = (i % cols) * TW; y0 = (i // cols) * TH
        sheet.paste(im, (x0, y0 + 18))
        hr = int(hzv * h * S)
        dr.line([(x0, y0 + 18 + hr), (x0 + TW - 1, y0 + 18 + hr)], fill=(255, 255, 255))
        for d, col in ((old, (255, 255, 0)), (new, (0, 255, 0))):
            if d is None: continue
            fp = dirs_to_uv(footprint_dirs(d, CLEAR_DEG, 24), pos, uv)
            for (uu, vv) in fp[1:]:
                if np.isfinite(uu):
                    px = x0 + (uu % 1.0) * w * S; py = y0 + 18 + vv * h * S
                    dr.rectangle([px - 1, py - 1, px + 1, py + 1], fill=col)
            uu, vv = fp[0]
            px = x0 + (uu % 1.0) * w * S; py = y0 + 18 + vv * h * S
            dr.ellipse([px - 4, py - 4, px + 4, py + 4], outline=col, width=2)
        tag = cls + ('' if oc is None else (' OK' if oc else (' -> MOVED' if new is not None else ' NO CLEAR SKY')))
        dr.text((x0 + 4, y0 + 3), rel.replace('re/assets/levels/', '') + '  ' + tag, fill=(255, 255, 0))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    sheet.save(out)
    print('sheet:', out)
    per = 3 * TH; base, ext = os.path.splitext(out)
    for pi in range((sheet.size[1] + per - 1) // per):
        sheet.crop((0, pi * per, sheet.size[0], min(sheet.size[1], (pi + 1) * per))).save('%s_p%d%s' % (base, pi, ext))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--sheet', default=os.path.join(ROOT, '_archive', 'roam', 'UC', 'sunpos_sheet.png'))
    ap.add_argument('--csv', default=os.path.join(ROOT, '_archive', 'roam', 'UC', 'sunpos.csv'))
    ap.add_argument('--no-table', action='store_true')
    ap.add_argument('--only', default='')
    args = ap.parse_args()

    pos, uv = load_dome()
    # candidate grid in dome space
    AZ = np.arange(0, 360, 0.5); EL = np.arange(EL_MIN, EL_MAX + 1e-6, 0.5)
    ga, ge = np.meshgrid(AZ, EL)
    cand = azel_to_dir(ga.ravel(), ge.ravel())
    cand_fp = [footprint_dirs(c, CLEAR_DEG, 12) for c in cand]
    nfp = len(cand_fp[0])
    cache = os.path.join(ROOT, '_archive', 'roam', 'UC', 'cand_fp_uv_%.3f_%g_%g.npy' % (CLEAR_DEG, EL_MIN, EL_MAX))
    if os.path.exists(cache):
        cand_fp_uv = np.load(cache)
    else:
        cand_fp_uv = dirs_to_uv(np.concatenate(cand_fp), pos, uv).reshape(len(cand), nfp, 2)
        os.makedirs(os.path.dirname(cache), exist_ok=True); np.save(cache, cand_fp_uv)
    # horizon row: the dome texel row at elevation 0
    hz_uv = dirs_to_uv(azel_to_dir(np.array([120.0]), np.array([0.0])), pos, uv)[0]

    rows = []; entries = []; tiles = []
    for path in find_skies():
        rel = os.path.relpath(path, ROOT).replace(os.sep, '/')
        if args.only and args.only not in rel: continue
        rgb = np.asarray(Image.open(path).convert('RGB'))
        h, w, _ = rgb.shape
        cls, suv, old, srgb = c_probe(rgb, pos, uv)
        hrow = int(hz_uv[1] * h)
        g, edge = groundness(rgb, hrow)
        R, front = sky_fill(rgb, hrow)
        # below the trusted fill frontier, an unreached texel leans backdrop
        lean = np.zeros_like(g); lean[front:] = FRONT_BIAS
        g = np.maximum(g, lean * (~R))
        ov = OVERRIDES.get(rel.replace('re/assets/levels/', '').lower(), {})
        sl = skyline_dp(g, hrow, edge, reached=R, floor=ov.get('floor', 0), cap=ov.get('cap'))
        # the game stores the skyline at SKYLINE_COLS columns, min over each group
        # (conservative); judge placement on exactly that, so tool and game agree
        grp = w // SKYLINE_COLS
        slc = sl[:grp * SKYLINE_COLS].reshape(SKYLINE_COLS, grp).min(1)
        sl = np.repeat(slc, grp)
        hsh = fnv1a_bgr(rgb)
        new = None; status = cls
        old_clear = None
        if old is not None:
            fp_uv = dirs_to_uv(footprint_dirs(old, CLEAR_DEG, 12), pos, uv)
            old_clear = bool(is_sky_uv(fp_uv, sl, h, w).all())
            if old_clear:
                status = 'SUNNY ok'
            else:
                okc = is_sky_uv(cand_fp_uv.reshape(-1, 2), sl, h, w).reshape(len(cand), nfp).all(1)
                if okc.any():
                    dots = cand @ (old / np.linalg.norm(old))
                    dots[~okc] = -2
                    new = cand[int(np.argmax(dots))]
                    status = 'SUNNY moved'
                else:
                    status = 'SUNNY no-clear-sky'
        oaz, oel = dir_to_azel(old) if old is not None else (float('nan'),) * 2
        naz, nel = dir_to_azel(new) if new is not None else (float('nan'),) * 2
        moved = (math.degrees(math.acos(np.clip(np.dot(old / np.linalg.norm(old), new), -1, 1)))
                 if new is not None else 0.0)
        rows.append((rel, cls, hsh, oaz, oel, old_clear, naz, nel, moved, status))
        print('%-40s %-8s %08X old az=%6.1f el=%5.1f clear=%s new az=%6.1f el=%5.1f moved=%5.1f %s'
              % (rel, cls, hsh, oaz, oel, old_clear, naz, nel, moved, status))
        entries.append((rel, hsh, cls, old, new, slc, h))
        tiles.append((rel, rgb, sl, cls, old, new, old_clear))

    # ---- contact sheets: every sky, then the SUNNY ones only -----------------
    hzrow = hz_uv[1]
    draw_sheet(tiles, args.sheet, pos, uv, hzrow)
    base, ext = os.path.splitext(args.sheet)
    draw_sheet([t for t in tiles if t[3] == 'SUNNY'], base + '_sunny' + ext, pos, uv, hzrow)
    with open(args.csv, 'w') as f:
        f.write('sky,class,hash,old_az,old_el,old_clear,new_az,new_el,moved_deg,status\n')
        for r in rows:
            f.write('%s,%s,%08X,%.2f,%.2f,%s,%.2f,%.2f,%.2f,%s\n' % r)
    sunny = [r for r in rows if r[1] == 'SUNNY']
    print('SUNNY skies: %d, sun over non-sky before: %d, moved: %d, no clear sky: %d'
          % (len(sunny), sum(1 for r in sunny if not r[5]), sum(1 for r in sunny if r[9] == 'SUNNY moved'),
             sum(1 for r in sunny if r[9] == 'SUNNY no-clear-sky')))
    if not args.no_table and not args.only:
        write_table(entries)


def write_table(entries):
    L = []
    L.append('/* td5_sky_sunpos.h -- GENERATED by re/tools/sky_sunpos.py. Do not hand-edit:')
    L.append(' * rerun `python re/tools/sky_sunpos.py` after changing a sky panorama or the')
    L.append(' * classifier. Included ONLY by td5_render_effects.c (round UC, item 7).')
    L.append(' *')
    L.append(' * One row per shipped sky panorama (FORWSKY / BACKSKY of every level), keyed by')
    L.append(' * FNV-1a 32 of the decoded pixels as B,G,R bytes (alpha ignored), so a byte copy of a')
    L.append(' * panorama (the auto track borrows one, a free-roam geo host loads one) matches too.')
    L.append(' *   sun_set  : 1 = the probed sun sat over non-sky, use sun_dir instead')
    L.append(' *   sun_dir  : dome model space (+Y UP, X/Z = world X/Z), unit')
    L.append(' *   skyline  : per 1/%d of the texture width, the first texel row (0..h) that is NOT' % SKYLINE_COLS)
    L.append(' *              sky (minimum over the group = conservative). Rows above it are sky.')
    L.append(' * Clearance used for the placement: %.2f deg (sun glare radius + 1 deg). */' % CLEAR_DEG)
    L.append('#ifndef TD5_SKY_SUNPOS_H')
    L.append('#define TD5_SKY_SUNPOS_H')
    L.append('#include <stdint.h>')
    L.append('')
    L.append('#define TD5_SKY_SUNPOS_COLS %d' % SKYLINE_COLS)
    L.append('#define TD5_SKY_SUNPOS_CLEAR_DEG %.3ff' % CLEAR_DEG)
    L.append('')
    L.append('typedef struct TD5_SkySunPos {')
    L.append('    uint32_t    hash;')
    L.append('    const char *name;')
    L.append('    int         sun_set;')
    L.append('    float       sun_dir[3];')
    L.append('    uint8_t     skyline[TD5_SKY_SUNPOS_COLS];')
    L.append('} TD5_SkySunPos;')
    L.append('')
    L.append('static const TD5_SkySunPos k_sky_sunpos[] = {')
    for rel, hsh, cls, old, new, slc, h in entries:
        nm = rel.replace('re/assets/levels/', '')
        d = new if new is not None else np.zeros(3)
        sl = ','.join(str(min(255, int(v))) for v in slc)
        L.append('    { 0x%08Xu, "%s", %d, { %.5ff, %.5ff, %.5ff },  /* %s */' %
                 (hsh, nm, 1 if new is not None else 0, d[0], d[1], d[2], cls))
        L.append('      { %s } },' % sl)
    L.append('};')
    L.append('')
    L.append('#endif /* TD5_SKY_SUNPOS_H */')
    with open(TABLE, 'w', newline='\n') as f:
        f.write('\n'.join(L) + '\n')
    print('table:', TABLE, len(entries), 'rows')


if __name__ == '__main__':
    main()
