/**
 * td5_tg_city.c -- auto-track CITY: bend-fold authority, facade walls, turn continuation, pavement geometry, run-end census, side-street occupancy, street furniture
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

double tg_r14_keep(void)
{
    if (!td5_env_flag_on("TD5RE_R14_KEEP_ON")) return TD5_TG_R13_KEEP;
    return (double)td5_env_float("TD5RE_R14_KEEP", TD5_TG_R14_KEEP,
                                 -1.0f, 0.95f);
}

double tg_r13_fold_reach(const TG_NodeList *nl, int si, double side,
                                double ang)
{
    double nlx, nly, nlz, nrx, nry, nrz, flx, fly, flz, frx, fry, frz;
    double nux, nuz, fux, fuz, ex, ez, fx2, fz2, ix, iz, ilen, slope, len;
    const double c = cos(ang), s = sin(ang);
    double rn_x, rn_z, rf_x, rf_z;

    if (!nl || si < 0 || si + 1 >= nl->count) return 1e30;
    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) return 1e30;
    nux /= len; nuz /= len;
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) return 1e30;
    fux /= len; fuz /= len;
    if (side > 0.0) { ex = nlx; ez = nlz; fx2 = flx; fz2 = flz; }
    else { ex = nrx; ez = nrz; fx2 = frx; fz2 = frz;
           nux = -nux; nuz = -nuz; fux = -fux; fuz = -fuz; }
    rn_x = nux * c - nuz * s; rn_z = nux * s + nuz * c;
    rf_x = fux * c - fuz * s; rf_z = fux * s + fuz * c;

    ix = fx2 - ex; iz = fz2 - ez;
    ilen = sqrt(ix * ix + iz * iz);
    if (ilen < 1e-6) return 1e30;
    slope = ((rf_x - rn_x) * (ix / ilen) + (rf_z - rn_z) * (iz / ilen)) / ilen;
    if (slope > -1e-12) return 1e30;                  /* parallel or diverging */
    return (tg_r14_keep() - 1.0) / slope;
}

double tg_r13_fold_cap(const TG_NodeList *nl, int si, double side,
                              double ang, double want, const char *knob)
{
    double lim;
    if (!td5_env_flag_on("TD5RE_R13_FOLD")) return want;
    if (knob && !td5_env_flag_on(knob)) return want;
    lim = tg_r13_fold_reach(nl, si, side, ang);
    if (lim < TD5_TG_R13_FLOOR) lim = TD5_TG_R13_FLOOR;
    return want > lim ? lim : want;
}

/* One road mesh for span si, laterally offset by shift_near..shift_far AND
 * width-scaled wscale_near..wscale_far across the span. Appended to blk.
 *
 * The width TAPERS across the span rather than being one number for the whole
 * of it (2026-08-27): a branch corridor widens continuously (tg_branch_wscale),
 * and a constant-per-span width turns that taper back into the staircase the
 * strip rows no longer have -- the mesh would then disagree with the surface
 * you collide with. tg_emit_road_quad below passes the same value twice, so the
 * plain road and the fixed-width halves are unchanged. Returns 0 on OOM.
 *
 * ITEMS 7 & 11 (2026-08-28): the across-road U is `u_scale * wscale`, NOT a flat
 * lane count anchored edge-to-edge. Root cause of "textures moving on branches"
 * and "lane markers shift when adding lanes": the old code set the right-edge U
 * to a per-span integer lane count while the physical width tapered
 * continuously (tg_branch_wscale) and stepped the lane count independently. That
 * made the world-space tile size vary along a corridor (the asphalt "moved") and
 * jump at each lane-count change (the lane paint "shifted"), because on the main
 * ring width and lane count are both constant so the bug never showed. Tying U
 * to wscale makes the tile size (physical_width / U = width/u_scale) CONSTANT in
 * world units along the whole corridor and continuous across span joins: with a
 * per-corridor-constant u_scale, adjacent spans agree at their shared node
 * because U there depends only on the shared wscale, not on either span's lane
 * count. Constant-width callers (main road wscale 1.0, fork half wscale 0.5)
 * pass u_scale = lanes/wscale via tg_emit_road_quad, so their U is byte-identical
 * to before. `u_scale` = the U reached at the right edge when wscale == 1.0. */
int tg_emit_road_quad_taper(const TG_NodeList *nl, int si, double u_scale,
                                   double shift_near, double shift_far,
                                   double wscale_near, double wscale_far,
                                   int page, TG_Buf *blk)
{
    double px[TD5_TG_ROAD_SUBDIV * 4], py[TD5_TG_ROAD_SUBDIV * 4];
    double pz[TD5_TG_ROAD_SUBDIV * 4], uu[TD5_TG_ROAD_SUBDIV * 4];
    double vv[TD5_TG_ROAD_SUBDIV * 4];
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int k, i, n = 0;

    for (k = 0; k < TD5_TG_ROAD_SUBDIV; k++) {
        double f0 = (double)k / (double)TD5_TG_ROAD_SUBDIV;
        double f1 = (double)(k + 1) / (double)TD5_TG_ROAD_SUBDIV;
        double nlx, nly, nlz, nrx, nry, nrz;
        double flx, fly, flz, frx, fry, frz;
        double s0v = shift_near + (shift_far - shift_near) * f0;
        double s1v = shift_near + (shift_far - shift_near) * f1;
        double w0v = wscale_near + (wscale_far - wscale_near) * f0;
        double w1v = wscale_near + (wscale_far - wscale_near) * f1;
        tg_road_edge(nl, si, f0, s0v, w0v, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
        tg_road_edge(nl, si, f1, s1v, w1v, &flx, &fly, &flz, &frx, &fry, &frz);
        /* Right-edge U tracks the PHYSICAL width at each subdiv end (u_scale *
         * wscale), so the lane paint keeps a constant world-space pitch along a
         * tapering/widening corridor instead of stretching and jumping. */
        {
        /* [ROAD UV WIDTH FIX 2026-08-31] U must track the PHYSICAL width, which
         * is (interpolated node width) * wscale -- see tg_road_edge. The old
         * `u_scale * wscale` only saw wscale, and since u_scale is lanes/wscale
         * the two cancel: ur was ALWAYS `lanes`, whatever the road did. So a
         * dual-lane taper, which widens via node->width and leaves wscale at
         * 1.0, stretched the texture further on every span while U stayed put.
         *
         * MEASURED (LAB seed 777, PCT_DUAL=25, the reported spans): width runs
         * 6000 -> 10500 across spans 105-111 (+750/span, +75% total) while
         * ur stayed 4.0000 and du stayed 0.00000. The lane pitch therefore
         * changed span to span and the markings mismatched at every boundary.
         *
         * Scaling by nw/nw_ref makes the tile size constant in world space.
         * nw_ref is the corridor's base width, lanes * LANE_WIDTH, recovered as
         * (u_scale * wscale) * LANE_WIDTH since that product IS the lane count.
         * Constant-width road: nw == nw_ref, ratio 1, byte-identical to before.
         * Fork carriageways (wscale 0.5, node width unchanged): ratio 1, also
         * unchanged. Only a genuine width taper moves. TD5RE_ROAD_UV_WIDTH=0
         * restores the old behaviour for A/B. */
        double ur0 = u_scale * w0v;
        double ur1 = u_scale * w1v;
        if (td5_env_flag_on("TD5RE_ROAD_UV_WIDTH")) {
            const TG_Node *rna = &nl->v[si];
            const TG_Node *rnb = &nl->v[si + 1];
            const double nw0    = rna->width + (rnb->width - rna->width) * f0;
            const double nw1    = rna->width + (rnb->width - rna->width) * f1;
            /* [R17 ROADMARK items 1&2] The carriageway's width in LANES is its
             * physical width over one lane width, i.e. (node width * wscale) /
             * LANE_WIDTH, and U must equal exactly that so one texture tile is
             * one lane at a constant world pitch. The nw/nw_ref form below did
             * that correctly ONLY for a full-width road: there nw_ref =
             * LANE_WIDTH*(u_scale*wscale_near) reduces to the road's own width
             * and the ratio collapses to nw/LANE_WIDTH. But on a width-SCALED
             * carriageway -- a fork's MAIN HALF or a BRANCH CORRIDOR, where the
             * centreline node keeps the FULL road width and `wscale` carries the
             * fraction -- nw is the full width while nw_ref is only the half
             * carriageway's width, so the ratio came out ~2 and, worse, keyed on
             * the per-span `wscale_near`. That DOUBLED the lane-line density on a
             * fork carriageway (the "lane markings not consistent when the branch
             * is finished" report at a rejoin, where a double-density main half
             * meets the normal full road) and made it JUMP at every corridor span
             * whose wscale differed from its neighbour's (the "lane markings not
             * continuous when the road widens" report), because adjacent spans
             * divided by different wscale_near references.
             *
             * nw*wscale/LANE_WIDTH has neither fault: it is BYTE-IDENTICAL on the
             * main ring (wscale == 1 -> nw/LANE_WIDTH, exactly the old ratio
             * form's result there, including the dual-lane width taper this block
             * was written for) and continuous through a fork/corridor taper.
             * TD5RE_R17_ROADMARK_UV=0 restores the old nw/nw_ref ratio for A/B. */
            if (td5_env_flag_on("TD5RE_R17_ROADMARK_UV")
                && TD5_TG_LANE_WIDTH > 0) {
                ur0 = nw0 * w0v / (double)TD5_TG_LANE_WIDTH;
                ur1 = nw1 * w1v / (double)TD5_TG_LANE_WIDTH;
            } else {
                const double nw_ref =
                    (double)TD5_TG_LANE_WIDTH * (u_scale * wscale_near);
                if (nw_ref > 1e-6) { ur0 *= nw0 / nw_ref; ur1 *= nw1 / nw_ref; }
            }
        }
#ifndef TD5RE_RELEASE
        /* [ROAD UV DIAG 2026-08-31] TD5RE_ROAD_UV_DIAG=1 dumps the per-subdiv U
         * span of each road quad. Reproduced under control on LAB v5 (seed 777,
         * PCT_DUAL=25) at spans 105-110; three earlier lab tracks with
         * PCT_DUAL=0 were clean, which points at the WIDTH TAPER rather than
         * curvature or grade.
         *
         * The quad carries U = (0, ur0, ur1, 0). When ur0 != ur1 the UV
         * rectangle is a TRAPEZOID, and a 2-triangle split cannot represent that
         * mapping: the UV gradient is discontinuous across the shared diagonal,
         * which reads as a zigzag along the road. du = ur1-ur0 is the size of
         * that mismatch, so a nonzero du on exactly the reported spans confirms
         * the mechanism, and a zero du there kills it. */
        if (getenv("TD5RE_ROAD_UV_DIAG") && k == 0) {
            /* du proved ZERO on the reported spans, so the trapezoid idea is
             * dead. Dump the span's other properties instead and let the diff
             * against clean neighbours say what is actually different. */
            const TG_Node *na = &nl->v[si];
            const TG_Node *nb = &nl->v[(si + 1 < nl->count) ? si + 1 : si];
            double dot = na->tx * nb->tx + na->tz * nb->tz;
            double dy  = nb->y - na->y;
            if (dot >  1.0) dot =  1.0;
            if (dot < -1.0) dot = -1.0;
            TD5_LOG_I(LOG_TAG,
                "roadspan si=%d page=%d w=%.0f turn=%.2fdeg dy=%+.0f y=%.0f "
                "ur=%.4f V=%.1f..%.1f",
                si, tg_road_page(si), na->width,
                acos(dot) * 180.0 / 3.14159265358979323846, dy, na->y,
                ur0, (double)si, (double)si + 1.0);
        }
#endif
        /* Quad loop: near-left, near-right, far-right, far-left. */
        px[n]=nlx; py[n]=nly; pz[n]=nlz; uu[n]=0.0; vv[n]=si+f0; n++;
        px[n]=nrx; py[n]=nry; pz[n]=nrz; uu[n]=ur0; vv[n]=si+f0; n++;
        px[n]=frx; py[n]=fry; pz[n]=frz; uu[n]=ur1; vv[n]=si+f1; n++;
        px[n]=flx; py[n]=fly; pz[n]=flz; uu[n]=0.0; vv[n]=si+f1; n++;
        }
    }

    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;   /* NaN/<=0 is rejected by the culler */

    /* --- mesh record (0x38) --- */
    tg_put_u16(blk, 259);            /* 0x00 render_type (nothing reads it) */
    tg_put_u16(blk, 0);              /* 0x02 billboard tag: 0 = opaque */
    tg_put_u32(blk, 1);              /* 0x04 command_count */
    tg_put_u32(blk, (unsigned)n);    /* 0x08 total_vertex_count */
    tg_put_f32(blk, radius);         /* 0x0C bounding_radius */
    tg_put_f32(blk, cx);             /* 0x10 bounding_center */
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0);            /* 0x1C origin: 0 for opaque geometry */
    tg_put_f32(blk, 0.0);
    tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);              /* 0x28 reserved */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);                       /* commands */
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);     /* vertices */
    tg_put_u32(blk, 0);              /* 0x34 normals: NULL is allowed */

    /* --- one command: quads only, sequential vertex cursor --- */
    tg_put_u16(blk, 0);              /* dispatch_type 0 = TRISTRIP */
    tg_put_u16(blk, (unsigned)page); /* texture_page_id: the SAMPLED page */
    tg_put_u32(blk, 0);              /* reserved */
    tg_put_u16(blk, 0);                             /* triangle_count */
    tg_put_u16(blk, TD5_TG_ROAD_SUBDIV);            /* quad_count */
    tg_put_u32(blk, 0);              /* vertex_data_ptr 0 = sequential */

    /* --- de-indexed vertices, 44 B each --- */
    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0);        /* view xyz: filled at runtime */
        tg_put_f32(blk, 0.0);
        tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);/* lighting ARGB: full bright */
        tg_put_f32(blk, uu[i]);      /* UVs are normalised floats; >1 tiles */
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0);        /* proj_u/proj_v: runtime */
        tg_put_f32(blk, 0.0);
    }
    /* Every drivable quad lands here -- the plain road, the fork half-road and
     * the branch corridor all route through this one writer. */
    tg_acct(TG_ACCT_ROAD, si);
    return !blk->oom;
}

/* Constant-width road mesh: the taper emitter with one width for both ends.
 * Kept as its own name so every existing caller is untouched. */
int tg_emit_road_quad(const TG_NodeList *nl, int si, int lanes,
                             double shift_near, double shift_far, double wscale,
                             int page, TG_Buf *blk)
{
    /* Constant width, so u_scale*wscale must reproduce the flat `lanes` the old
     * code wrote at the right edge. wscale is 0.5 or 1.0 here (exact in binary),
     * so lanes/wscale then *wscale is byte-identical to the old integer U. */
    const double u_scale = (wscale > 0.0) ? (double)lanes / wscale : (double)lanes;
    return tg_emit_road_quad_taper(nl, si, u_scale, shift_near, shift_far,
                                   wscale, wscale, page, blk);
}

/* Plain main-road mesh for span si (no lateral offset). */
int tg_emit_road_mesh(const TG_NodeList *nl, int si, int lanes,
                             TG_Buf *blk)
{
    return tg_emit_road_quad(nl, si, lanes, 0.0, 0.0, 1.0, tg_road_page(si), blk);
}

/* Six quads. Corner sign triples per face, then which half-extents span the
 * face (for UV scaling). Winding is effectively free -- clip_and_submit_polygon
 * culls by screen area, and scenery is submitted CULL_NONE. */
static const signed char k_box_corner[6][4][3] = {
    {{-1, 1,-1},{ 1, 1,-1},{ 1, 1, 1},{-1, 1, 1}},   /* +Y top    */
    {{-1,-1, 1},{ 1,-1, 1},{ 1,-1,-1},{-1,-1,-1}},   /* -Y bottom */
    {{-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1}},   /* -Z        */
    {{ 1,-1, 1},{-1,-1, 1},{-1, 1, 1},{ 1, 1, 1}},   /* +Z        */
    {{-1,-1, 1},{-1,-1,-1},{-1, 1,-1},{-1, 1, 1}},   /* -X        */
    {{ 1,-1,-1},{ 1,-1, 1},{ 1, 1, 1},{ 1, 1,-1}}    /* +X        */
};

/* Half-extent axis pairs giving each face's (u,v) size: 0=x 1=y 2=z. */
static const signed char k_box_uv_axis[6][2] = {
    {0,2},{0,2},{0,1},{0,1},{2,1},{2,1}
};

/* A box (building / bridge pier / tunnel wall) centred at c with half-extents
 * h, textured from `page`, tiling every `tile` world units. */
int tg_emit_box_mesh(TG_Buf *blk, double cx, double cy, double cz,
                            double hx, double hy, double hz,
                            double fx, double fz, int page, double tile,
                            unsigned int color)
{
    /* Box frame: fwd = (fx,fz) along the road, right = (fz,-fx), up = +Y.
     * An axis-aligned box is fine for a building but wrong for anything that
     * must FOLLOW a curving road (tunnel walls, bridge decks), so hz runs
     * along the road and hx across it. */
    const double rx = fz, rz = -fx;
    const double hv[3] = { hx, hy, hz };
    double radius = sqrt(hx*hx + hy*hy + hz*hz);
    int f, i;

    if (!(radius > 0.0)) radius = 1.0;
    if (tile <= 0.0) tile = 1500.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, 6 * 4);                /* 6 quads, de-indexed */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);                    /* triangle_count */
    tg_put_u16(blk, 6);                    /* quad_count */
    tg_put_u32(blk, 0);

    for (f = 0; f < 6; f++) {
        double ua = 2.0 * hv[(int)k_box_uv_axis[f][0]] / tile;
        double vb = 2.0 * hv[(int)k_box_uv_axis[f][1]] / tile;
        for (i = 0; i < 4; i++) {
            const double sx = k_box_corner[f][i][0];
            const double sy = k_box_corner[f][i][1];
            const double sz = k_box_corner[f][i][2];
            tg_put_f32(blk, cx + rx * (sx * hx) + fx * (sz * hz));
            tg_put_f32(blk, cy +       sy * hy);
            tg_put_f32(blk, cz + rz * (sx * hx) + fz * (sz * hz));
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
            tg_put_u32(blk, color);
            /* Corner order walks the quad loop, so (0,0)(u,0)(u,v)(0,v). */
            tg_put_f32(blk, (i == 1 || i == 2) ? ua : 0.0);
            tg_put_f32(blk, (i >= 2) ? vb : 0.0);
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        }
    }
    return !blk->oom;
}

int tg_emit_billboard_mesh(TG_Buf *blk, double wx, double wy, double wz,
                                  double half_w, double height, int page, int tag)
{
    double radius = sqrt(half_w * half_w + height * height);
    /* A HALF page is drawn as TWO quads meeting at the billboard's vertical
     * axis, the second with u reversed -- mirror-and-duplicate, which is how
     * the shipped level placed the pair. Whole pages stay one quad. */
    const int nq = tg_tree_page_is_half(page) ? 2 : 1;
    int q, i;
    static const double ly[4] = {  0.0,  0.0,  1.0,  1.0 };

    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, (unsigned)tag);        /* 1 = camera-facing, 2 = additive */
    tg_put_u32(blk, 1);                    /* one command */
    tg_put_u32(blk, (unsigned)(4 * nq));   /* one quad, or two when mirrored */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, wx);                   /* bounding centre stays world */
    tg_put_f32(blk, wy + height * 0.5);
    tg_put_f32(blk, wz);
    tg_put_f32(blk, wx * 256.0);           /* origin is 24.8 for billboards */
    tg_put_f32(blk, wy * 256.0);
    tg_put_f32(blk, wz * 256.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    tg_put_u16(blk, 0);                    /* dispatch_type 0 */
    tg_put_u16(blk, (unsigned)page);
    tg_put_u32(blk, 0);
    tg_put_u16(blk, 0);
    tg_put_u16(blk, (unsigned)nq);         /* quad_count */
    tg_put_u32(blk, 0);

    for (q = 0; q < nq; q++) {
        /* Whole page: one quad spanning -half_w..+half_w, u 0..1.
         * Half page: the image's own axis is its u=1 edge (measured: every
         * half page's foliage is FLUSH at column 63 with a keyed left margin),
         * so quad 0 runs -half_w..0 with u 0..1 and quad 1 runs 0..+half_w
         * with u 1..0 -- the seam is the axis and the halves match exactly. */
        const double x0 = (nq == 1 || q == 0) ? -half_w : 0.0;
        const double x1 = (nq == 1 || q == 1) ?  half_w : 0.0;
        const double u0 = (q == 1) ? 1.0 : 0.0;
        const double u1 = (q == 1) ? 0.0 : 1.0;
        for (i = 0; i < 4; i++) {
            /* local: x across, y up. Quad loop order is near-bottom,
             * far-bottom, far-top, near-top, so 1 and 2 take the far edge. */
            tg_put_f32(blk, (i == 1 || i == 2) ? x1 : x0);
            tg_put_f32(blk, ly[i] * height);
            tg_put_f32(blk, 0.0);
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
            tg_put_u32(blk, 0xFFFFFFFFu);
            tg_put_f32(blk, (i == 1 || i == 2) ? u1 : u0);
            tg_put_f32(blk, (i >= 2) ? 0.0 : 1.0);   /* v flipped: base at v=1 */
            tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        }
    }
    return !blk->oom;
}

/* Write ONE opaque quad-list mesh in the 0x38 format, split into up to `nseg`
 * COMMANDS -- each command samples its own page over its own run of quads, in
 * vertex order. That is how one building can carry a shop page on the ground
 * floor and a wall page above without a second mesh (which would corrupt the
 * one-mesh-per-building offset accounting in tg_emit_models). */
/* [GEOMLIB] Write one PREFAB instance: lifted shipped geometry, placed at
 * (ox,oy,oz) and yawed by (ca,sa).
 *
 * Separate from tg_write_quad_mesh for three reasons, each of which it cannot
 * do: these meshes carry TRIANGLES as well as quads (the Moscow set pieces are
 * 264 tris and 592 quads, so a quad-only writer would drop a third of the
 * faces); they carry BAKED per-vertex ARGB that must survive (48 distinct
 * values across the set, dominant 0xFFA0A0A0, only 390 of 3084 actually white,
 * so the hardcoded 0xFFFFFFFF would brighten most of the geometry); and their
 * commands name a LOCAL page index that has to be rebased.
 *
 * Vertex layout in `v` is x,y,z,u,v; `cmd` is (page_local, tri, quad) triples.
 * Vertices are consumed sequentially, TRIS BEFORE QUADS within a command --
 * the MODELS.DAT rule the whole shipped corpus obeys without exception. */
int tg_write_prefab_mesh(TG_Buf *blk, const float *v, const unsigned int *light,
                         int nv, const unsigned short *cmd, int ncmd,
                         int page_base, double ox, double oy, double oz,
                         double ca, double sa)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s, need = 0;

    if (!v || !light || !cmd || nv <= 0 || ncmd <= 0) return 1;
    /* The cursor must account for every vertex, exactly as the reader assumes.
     * A short or long command list would slice the block at the wrong offsets
     * and texture the wrong faces, silently. */
    for (s = 0; s < ncmd; s++) need += cmd[s * 3 + 1] * 3 + cmd[s * 3 + 2] * 4;
    if (need != nv) return 0;

    for (i = 0; i < nv; i++) {
        const double lx = v[i * 5 + 0], lz = v[i * 5 + 2];
        cx += ox + lx * ca - lz * sa;
        cy += oy + v[i * 5 + 1];
        cz += oz + lx * sa + lz * ca;
    }
    cx /= nv; cy /= nv; cz /= nv;
    for (i = 0; i < nv; i++) {
        const double lx = v[i * 5 + 0], lz = v[i * 5 + 2];
        const double wx = ox + lx * ca - lz * sa;
        const double wy = oy + v[i * 5 + 1];
        const double wz = oz + lx * sa + lz * ca;
        const double dx = wx - cx, dy = wy - cy, dz = wz - cz;
        const double d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, (unsigned)ncmd);
    tg_put_u32(blk, (unsigned)nv);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + ncmd * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < ncmd; s++) {
        tg_put_u16(blk, 0);                                    /* dispatch 0 */
        tg_put_u16(blk, (unsigned)(page_base + cmd[s * 3 + 0]));
        tg_put_u32(blk, 0);
        tg_put_u16(blk, cmd[s * 3 + 1]);                       /* triangles */
        tg_put_u16(blk, cmd[s * 3 + 2]);                       /* quads     */
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < nv; i++) {
        const double lx = v[i * 5 + 0], lz = v[i * 5 + 2];
        tg_put_f32(blk, ox + lx * ca - lz * sa);
        tg_put_f32(blk, oy + v[i * 5 + 1]);
        tg_put_f32(blk, oz + lx * sa + lz * ca);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, light[i]);         /* baked ARGB, NOT forced to white */
        tg_put_f32(blk, v[i * 5 + 3]);
        tg_put_f32(blk, v[i * 5 + 4]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

int tg_write_quad_mesh(TG_Buf *blk, const double *px, const double *py,
                              const double *pz, const double *uu, const double *vv,
                              int n, const int *seg_page, const int *seg_nq,
                              int nseg)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s;

    if (n <= 0 || nseg <= 0) return 1;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, (unsigned)nseg);       /* command_count */
    tg_put_u32(blk, (unsigned)n);          /* total de-indexed vertices */
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + nseg * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < nseg; s++) {
        tg_put_u16(blk, 0);                    /* dispatch_type 0 */
        tg_put_u16(blk, (unsigned)seg_page[s]);
        tg_put_u32(blk, 0);
        tg_put_u16(blk, 0);                    /* triangle_count */
        tg_put_u16(blk, (unsigned)seg_nq[s]);  /* quad_count */
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, 0xFFFFFFFFu);
        tg_put_f32(blk, uu[i]);
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* [R5 STRUCT item 9] Same as tg_write_quad_mesh but with a PER-VERTEX colour,
 * so one mesh can carry both the DIM tunnel walls and a BRIGHT portal lintel
 * (the swept tunnel keeps everything in a single mesh per span so the caller's
 * offset bookkeeping records exactly one model). */
int tg_write_quad_mesh_col(TG_Buf *blk, const double *px, const double *py,
                                  const double *pz, const double *uu, const double *vv,
                                  const unsigned int *col, int n,
                                  const int *seg_page, const int *seg_nq, int nseg)
{
    double cx = 0.0, cy = 0.0, cz = 0.0, radius = 0.0;
    int i, s;

    if (n <= 0 || nseg <= 0) return 1;
    for (i = 0; i < n; i++) { cx += px[i]; cy += py[i]; cz += pz[i]; }
    cx /= n; cy /= n; cz /= n;
    for (i = 0; i < n; i++) {
        double dx = px[i]-cx, dy = py[i]-cy, dz = pz[i]-cz;
        double d = sqrt(dx*dx + dy*dy + dz*dz);
        if (d > radius) radius = d;
    }
    if (!(radius > 0.0)) radius = 1.0;

    tg_put_u16(blk, 259);
    tg_put_u16(blk, 0);                    /* opaque, not a billboard */
    tg_put_u32(blk, (unsigned)nseg);
    tg_put_u32(blk, (unsigned)n);
    tg_put_f32(blk, radius);
    tg_put_f32(blk, cx);
    tg_put_f32(blk, cy);
    tg_put_f32(blk, cz);
    tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    tg_put_u32(blk, 0);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE);
    tg_put_u32(blk, TD5_TG_MESH_DISK_SIZE + nseg * TD5_TG_CMD_SIZE);
    tg_put_u32(blk, 0);

    for (s = 0; s < nseg; s++) {
        tg_put_u16(blk, 0);
        tg_put_u16(blk, (unsigned)seg_page[s]);
        tg_put_u32(blk, 0);
        tg_put_u16(blk, 0);
        tg_put_u16(blk, (unsigned)seg_nq[s]);
        tg_put_u32(blk, 0);
    }

    for (i = 0; i < n; i++) {
        tg_put_f32(blk, px[i]);
        tg_put_f32(blk, py[i]);
        tg_put_f32(blk, pz[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
        tg_put_u32(blk, col[i]);
        tg_put_f32(blk, uu[i]);
        tg_put_f32(blk, vv[i]);
        tg_put_f32(blk, 0.0); tg_put_f32(blk, 0.0);
    }
    return !blk->oom;
}

/* Push floors [r0,r1) of a cols x `rows` grid onto the vertex arrays. `base` is
 * the lower-near corner; `across` and `up` are the FULL edge vectors (up spans
 * all `rows` floors). Each cell maps the whole page (UV 0..1, v=1 at the base),
 * so no image is cut mid-cell. Splitting the row range lets the ground floor go
 * on a shop page and the floors above on the wall page. */
void tg_facade_push_grid(double bx, double by, double bz,
                                double ax, double ay, double az,
                                double ux, double uy, double uz,
                                int cols, int rows, int r0, int r1,
                                double *px, double *py, double *pz,
                                double *uu, double *vv, int *pn)
{
    const double e0 = TD5_TG_FACADE_UV_INSET, e1 = 1.0 - TD5_TG_FACADE_UV_INSET;
    int c, r, n = *pn;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (r0 < 0) r0 = 0;
    if (r1 > rows) r1 = rows;
    for (r = r0; r < r1; r++) {
        for (c = 0; c < cols; c++) {
            double c0 = (double)c / cols, c1 = (double)(c + 1) / cols;
            double r0f = (double)r / rows, r1f = (double)(r + 1) / rows;
            double rr0 = r0f, rr1 = r1f;
            if (n + 4 > TD5_TG_FACADE_MAXQUAD * 4) { *pn = n; return; }
            /* quad loop: near-bottom, far-bottom, far-top, near-top */
            px[n]=bx+ax*c0+ux*rr0; py[n]=by+ay*c0+uy*rr0; pz[n]=bz+az*c0+uz*rr0; uu[n]=e0; vv[n]=e1; n++;
            px[n]=bx+ax*c1+ux*rr0; py[n]=by+ay*c1+uy*rr0; pz[n]=bz+az*c1+uz*rr0; uu[n]=e1; vv[n]=e1; n++;
            px[n]=bx+ax*c1+ux*rr1; py[n]=by+ay*c1+uy*rr1; pz[n]=bz+az*c1+uz*rr1; uu[n]=e1; vv[n]=e0; n++;
            px[n]=bx+ax*c0+ux*rr1; py[n]=by+ay*c0+uy*rr1; pz[n]=bz+az*c0+uz*rr1; uu[n]=e0; vv[n]=e0; n++;
        }
    }
    *pn = n;
}

/* Resolve span si on one side to its superblock, its phase within it, that
 * block's side street (start phase + length) and whether it is an avenue. */
void tg_facade_block(int si, int left, unsigned int *block,
                            unsigned int *phase, unsigned int *gs,
                            unsigned int *gl, int *avenue)
{
    const unsigned int ab = (unsigned)si / TD5_TG_FACADE_PERIOD;
    const int av = (int)(((ab * 2654435761u) >> 28) % (unsigned)TD5_TG_AVENUE_IN
                         == 0u);
    const unsigned int s = (unsigned)si + ((!av && left) ? 777u : 0u);
    const unsigned int blk = s / TD5_TG_FACADE_PERIOD;
    const unsigned int h = blk * 2654435761u;

    *block  = blk;
    *phase  = s % TD5_TG_FACADE_PERIOD;
    *gs     = 2u + ((h >> 27) % 11u);            /* run before the street */
    /* An avenue is wider than a street, and that is the whole point of the
     * distinction: 6..8 spans (9000..12000 raw, six to eight lanes) against the
     * 2..4 of a minor street. */
    *gl     = av ? (6u + ((h >> 23) % 3u)) : (2u + ((h >> 23) % 3u));
    *avenue = av;
}

signed char s_turn_side[TD5_TG_MAX_SPANS];   /* 0 none, +1 left, -1 right */

float       s_turn_skew[TD5_TG_MAX_SPANS];   /* normal -> incoming heading */

/* [R11 CROSS item 16] |sin| of the heading change across +/-TD5_TG_R8_TURN_BASE
 * spans, for EVERY span. This is not a new curvature test: it is the value the
 * continuation loop below already computes and then throws away, recorded
 * BEFORE that loop's dedup (TURN_GAP) and frontage gate so it stays a plain
 * measurement of the ROAD rather than an answer about street openings. The
 * crossing thinner reads it through tg_turn_bend; s_turn_side / s_turn_skew
 * keep their existing meaning ("a continuation opens here"), which is a
 * different question and must not be confused with this one. */
static float       s_turn_bend[TD5_TG_MAX_SPANS];

/* [R14 item 4] Continuation census: candidates that cleared the bend and `dot`
 * tests, how many the skew ceiling then refused, and the steepest lean seen.
 * Read-only bookkeeping for tg_r14_junc_report -- nothing places on them. */
int    s_r14_turn_cand;

int    s_r14_turn_refused;

double s_r14_turn_worst;

/* Continuations actually OPENED, which is NOT cand - refused. A refusal takes
 * the `continue` above without advancing `last`, so the TD5_TG_R8_TURN_GAP
 * dedup no longer suppresses the spans just after it and they re-enter as
 * fresh candidates: on seed 20260901 the candidate pool itself moves 95 -> 123
 * when the ceiling is switched on. The frontage gate below can also still
 * reject a candidate that cleared the ceiling. So the only honest measure of
 * "how many junctions did this remove" is counted where the map is written. */
int    s_r14_turn_opened;

void tg_turn_map_build(const TG_NodeList *nl, int nspans)
{
    const int k = TD5_TG_R8_TURN_BASE;
    int si, last = -TD5_TG_R8_TURN_GAP;

    memset(s_turn_side, 0, sizeof(s_turn_side));
    memset(s_turn_skew, 0, sizeof(s_turn_skew));
    memset(s_turn_bend, 0, sizeof(s_turn_bend));
    s_r14_turn_cand = 0;
    s_r14_turn_refused = 0;
    s_r14_turn_worst = 0.0;
    s_r14_turn_opened = 0;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    /* [R11 CROSS item 16] Bend magnitude first, unconditionally -- it is filled
     * even with TD5RE_R8_CROSS_TURN=0, because how bent the road is does not
     * depend on whether turn continuations are enabled. */
    for (si = k; si < nspans - k; si++) {
        const TG_Node *a = &nl->v[si - k], *c = &nl->v[si + k];
        s_turn_bend[si] = (float)fabs(a->tx * c->tz - a->tz * c->tx);
    }

    if (!td5_env_flag_on("TD5RE_R8_CROSS_TURN")) return;

    for (si = k; si < nspans - k; si++) {
        const TG_Node *a = &nl->v[si - k], *c = &nl->v[si + k];
        double cross, sg, ux, uz, dot, det;
        if (si - last < TD5_TG_R8_TURN_GAP) continue;
        cross = a->tx * c->tz - a->tz * c->tx;
        /* Same number, read from the map instead of recomputed, so the
         * continuation decision is bit-identical to before. */
        if ((double)s_turn_bend[si] < TD5_TG_R8_TURN_SIN) continue;
        /* A LEFT turn rotates the tangent toward the left normal (tz,-tx), which
         * makes this cross product NEGATIVE; the continuation belongs on the
         * OUTSIDE of the bend, so left turn -> right side and vice versa. */
        sg = (cross < 0.0) ? -1.0 : 1.0;
        /* Outward unit on that side, and the signed angle from it to the
         * incoming heading. Reject a bend too shallow for the continuation to
         * actually leave the road (the street would graze the kerb). */
        ux = nl->v[si].tz * sg; uz = -nl->v[si].tx * sg;
        dot = ux * a->tx + uz * a->tz;
        if (dot < 0.30) continue;
        det = ux * a->tz - uz * a->tx;
        /* [R14 JUNCTION item 4] "SAFEGUARDS AGAINST ROAD INTERSECTIONS PLACED
         * ON A CURVE." Deliberately NOT a third spacing rule: R11 CROSS already
         * scales crossing spacing with curvature and R12 CROSS proved that
         * premise with a control group, and the complaint survived both. This
         * gates on the ARM'S OWN BEARING instead, which is a different quantity
         * and the one the geometry actually fails on.
         *
         * A continuation is the only junction this generator places BECAUSE of
         * a bend, and it leans by the full turn angle. Past TD5_TG_DIAG_MAX_DEG
         * that lean is the doubling-back this file has forbidden since R8 --
         * the arm, its pavements, its crossing and its flanking massing all
         * take their bearing from this one number (tg_block_arm_skew), so a
         * skew over the ceiling puts the whole junction across the road it
         * leaves. Refusing the opening leaves the frontage BUILT and lets the
         * ordinary side street next door do the explaining, which is the same
         * fallback the R11 TURNGAP guard above already takes.
         *
         * Refused here rather than clamped: clamping would keep the junction
         * and point it somewhere that is neither the incoming heading (so it no
         * longer continues anything) nor square to the road, which is a third
         * kind of junction to explain rather than one fewer. */
        {
            const double sk = atan2(det, dot);
            const double mx = (double)td5_env_float("TD5RE_R14_TURN_SKEW_MAX",
                                  (float)TD5_TG_R14_SKEW_MAX_DEG, 0.0f, 90.0f)
                            * TD5_TG_PI / 180.0;
            const double mag = (sk < 0.0 ? -sk : sk);
            const int refuse = td5_env_flag_on("TD5RE_R14_TURN_SKEW")
                            && mag > mx;
            s_r14_turn_cand++;
            if (mag > s_r14_turn_worst) s_r14_turn_worst = mag;
            /* Per-candidate, so the ceiling above is chosen from a printed
             * distribution rather than from a guess about one seed. */
            if (td5_env_flag_on("TD5RE_R13_JUNC_REPORT"))
                TD5_LOG_I(LOG_TAG, "trackgen:   r14junc cont si=%d side=%s "
                          "skew=%.1fdeg bend=%.3f %s", si,
                          (sg > 0.0) ? "L" : "R", mag * 180.0 / TD5_TG_PI,
                          (double)s_turn_bend[si],
                          refuse ? "REFUSED" : "open");
            if (refuse) { s_r14_turn_refused++; continue; }
        }
        /* [R11 CITY item 10] A continuation may only open UNBROKEN frontage.
         * The feature exists to explain a bend that would otherwise turn in a
         * void, so opening one beside a street the block pattern already put
         * there explains nothing and costs a great deal: on seed 20260901 the
         * bend at 706 opened the LEFT frontage one span before the natural gap
         * at 708, stranding span 707 as a one-span block. tg_facade_isolated
         * (R6 item 16) then suppressed that block's wall -- correctly -- but
         * every junction emitter still reads the raw run/gap PATTERN, which
         * still said "built". So the arms, the crossing base and the
         * cross-street frontage walls all anchored themselves to a building
         * that was never emitted: free-standing walls with no body behind them
         * and two side streets on different bearings a single span apart. That
         * is both halves of item 10 -- "buildings with only a facade" and "a
         * weird mixture of crossings" -- from one cause.
         *
         * The guard is stated at the source rather than taught to each consumer:
         * require the pattern to be BUILT and STANDING across the whole window
         * the opening would sit in, so a continuation can never abut a gap and
         * can never strand a block. Rejecting it leaves the ordinary side
         * street next door doing the explaining, which is what a driver reads
         * anyway. TD5RE_R11_CITY_TURNGAP=0 restores the ungated map. */
        if (td5_env_flag_on("TD5RE_R11_CITY_TURNGAP")) {
            const int lf = (sg > 0.0);
            int j, clear = 1;
            for (j = si - TD5_TG_R11_TURN_CLEAR;
                 j <= si + TD5_TG_R11_TURN_CLEAR && clear; j++) {
                if (j <= 0 || j >= nspans) { clear = 0; break; }
                if (!tg_facade_built(j, lf) || !tg_facade_stands(j)) clear = 0;
            }
            if (!clear) continue;
        }
        s_r14_turn_opened++;          /* [R14] counted where the map is WRITTEN */
        s_turn_side[si] = (signed char)(sg > 0.0 ? 1 : -1);
        s_turn_skew[si] = (float)atan2(det, dot);
        last = si;
    }
}

/* Is span si / side `left` (1 = left of travel) the outside of a sharp bend
 * that a continuation street runs out of? */
int tg_turn_open(int si, int left)
{
    if (si <= 0 || si >= TD5_TG_MAX_SPANS) return 0;
    return s_turn_side[si] == (signed char)(left ? 1 : -1);
}

/* [R11 CROSS item 16] How bent the road is at span si, as |sin| of the heading
 * change over the turn map's own +/-TD5_TG_R8_TURN_BASE window. 0 on a
 * straight; TD5_TG_R8_TURN_SIN is the bend the R8 continuation test calls sharp
 * enough for a street to leave the road, so it is the natural full-scale point
 * for anything that wants to react to a bend. */
double tg_turn_bend(int si)
{
    if (si <= 0 || si >= TD5_TG_MAX_SPANS) return 0.0;
    return (double)s_turn_bend[si];
}

int tg_facade_built(int si, int left)
{
    /* [TOPOLOGY-FIRST] In a paved biome the frontage is open exactly where
     * the network put a street mouth (a candidate opening the raster refused
     * stays BUILT -- the fallback every junction emitter understands).
     * Elsewhere the hash rhythm still shapes what the emitters draw. */
    if (tg_network_built() && si > 0 &&
        tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0) {
        if (si < TD5_TG_FACADE_START_RUN &&
            td5_env_flag_on("TD5RE_AUTOTRACK_START_CITY"))
            return 1;
        {
            const int k = tg_net_mouth_kind(si, left);
            return !(k == TG_NE_STREET || k == TG_NE_AVENUE || k == TG_NE_CONTINUATION);
        }
    }
    return tg_facade_built_hash(si, left);
}

int tg_facade_built_hash(int si, int left)
{
    unsigned int block, phase, gs, gl;
    int av;

    /* [R8 CROSS item 9] A turn continuation opens the frontage on the outside of
     * the bend. Placed FIRST so every reader of this predicate -- carriageway,
     * arms, crossing, reveal row, flanking massing -- sees the same opening. */
    if (tg_turn_open(si, left)) return 0;

    /* Off the near end of the track there is nothing, so span 0 always gets a
     * corner return rather than a wall that starts as a bare edge. */
    if (si <= 0) return 0;
    /* "Start at the very beginnings with buildings": the opening stretch is
     * FORCED built on both sides. Otherwise the run/gap hash decides it, and
     * on most seeds it opens a gap right where the grid sits -- the start line
     * came up in bare ground even though span 0 is always the CITY biome. */
    if (si < TD5_TG_FACADE_START_RUN &&
        td5_env_flag_on("TD5RE_AUTOTRACK_START_CITY"))
        return 1;

    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    return (int)(phase < gs || phase >= gs + gl);
}

/* Does a facade actually STAND on side `left` (1=left,0=right) at span si? The
 * run/gap pattern (tg_facade_built) is not the whole story: a fork clears its
 * side<0 (=right, left==0) lateral for the branch corridor, and tg_side_geom
 * drops the wall there. Caps, step walls and neighbour-height queries must all
 * agree with tg_side_geom on this or a flank facing a suppressed side is left
 * open air -- the returning "some buildings still don't have sides, only a
 * facade" (item 15): a run whose FAR neighbour is a fork-cleared span had
 * cap_far == !tg_facade_built(si+1) == 0 (the pattern still says "built"), and
 * the step wall would even try to close to a building that was never emitted.
 * One predicate, shared, removes both mismatches. Gated so the widened coverage
 * can be A/B'd against the old cap-on-gap-only behaviour. */
int tg_side_built(int si, int left)
{
    if (!tg_facade_built(si, left)) return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_SIDE_CLOSE") &&
        tg_branches_enabled() && !left && tg_span_in_fork_clear(si))
        return 0;
    /* [R5 item 15] tg_facade_built is the run/gap PATTERN only -- biome- and
     * bridge-blind. On a bridge deck (tg_building_for_span returns early) or in a
     * tree-billboard biome (COAST/FOREST/... build no wall) the pattern still
     * says "built", so a cap keyed off tg_side_built left the flank open where a
     * facade run met a bridge or a non-facade biome: "one span of the front of a
     * building disconnected from the following spans" (span 1047, the INDUSTRIAL
     * band 1040-1049 wedged between bridge run 1000-1039 and COAST 1050+). A span
     * where no wall actually stands must read as a run boundary so its neighbour
     * caps. Default ON; TD5RE_AUTOTRACK_EDGE_CAP=0 restores the pattern-only
     * predicate for an A/B. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_EDGE_CAP") && !tg_facade_stands(si))
        return 0;
    /* [R11 BIOME item 4] The outskirts ramp opens some frontages at a
     * wilderness-to-town edge. Asked HERE, alongside the fork clearance and the
     * bridge/biome test, because those are the other two "the pattern says
     * built but no wall stands" cases -- so the corner returns, the step walls
     * and the no-lone-stub rule all treat a ramped-out run as a run boundary
     * and close it properly. */
    if (tg_town_ramp_open(si, left)) return 0;
    return 1;
}

/* [R6 item 16] A facade span whose side stands but whose BOTH along-road
 * neighbours do NOT -- a building one span long, capped on both ends, floating
 * detached from anything. It is the biome BLEND dithering a single facade-biome
 * span into a run of tree-biome (COAST) spans: seed 99991 spans 1043 and 1047,
 * lone INDUSTRIAL blocks in the COAST band just past the bridge -- "1 span of
 * building not connected with anything ... if this is the transition, remove the
 * logic". So: do not build a lone one-span frontage. tg_side_built (not the raw
 * pattern) is used for the neighbours so a bridge/biome edge already counts as
 * "not built", and it does NOT itself call this, so there is no recursion.
 * Per-side, since a run can end isolated on one kerb while the other continues. */
int tg_facade_isolated(int si, int left)
{
    if (!tg_side_built(si, left)) return 0;
    return !tg_side_built(si - 1, left) && !tg_side_built(si + 1, left);
}

/* [R11 CITY item 10] Is there a CORNER BLOCK on side `left` at span si -- a
 * frontage that is actually emitted, not merely one the run/gap pattern claims?
 * This is tg_side_geom's own `built` condition, stated once here so the junction
 * emitters can read it. They all keyed on the raw pattern, which outlives three
 * separate suppressions -- a fork-cleared lateral, a bridge or non-facade biome
 * span (tg_side_built folds both in), and the R6 one-span-block rule -- so a
 * pavement arm, a crossing base or a cross-street frontage wall could anchor
 * itself to a building that was never emitted. That is the "buildings with only
 * a facade, no body" half of item 10: the side-street walls are real geometry,
 * height-matched to a block that does not exist, standing on their own.
 *
 * Stated as one predicate rather than patched into each emitter, so the whole
 * family is covered and a future junction emitter inherits it. Nothing here can
 * ADD geometry: a corner either stands or the junction furniture that leaned on
 * it is not emitted. TD5RE_R11_CITY_CORNER=0 restores the pattern-only corner. */
int tg_r11_corner_stands(int si, int left)
{
    if (!td5_env_flag_on("TD5RE_R11_CITY_CORNER"))
        return tg_facade_built(si, left);
    if (!tg_side_built(si, left)) return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_NO_STUB") &&
        tg_facade_isolated(si, left)) return 0;
    return 1;
}

/* [R16 CITY item 1] "there's no sidewalk" between two sidewalks at the town
 * edge. Does the raised PAVEMENT / street grid stand on side `left` at span si,
 * as opposed to a building WALL?
 *
 * The outskirts DENSITY ramp (tg_town_ramp_open) retracts buildings across the
 * leading spans of a wilderness-to-town run. Its own design note is explicit
 * that it "removes BUILDINGS and leaves the street grid, the pavement and the
 * crossings exactly where they were" (see the R11 BIOME block in
 * td5_trackgen_internal.h). But tg_side_built folds the ramp in (so a retracted
 * run's WALL caps close cleanly -- correct for geometry), and
 * tg_r11_corner_stands reads tg_side_built, and the junction furniture reads
 * tg_r11_corner_stands. So at a side-street mouth whose flanking frontages were
 * ramp-retracted, tg_r11_arm_side found no corner, no pavement arm turned the
 * sidewalk down the street, tg_crossing_base painted no zebra, and the main
 * raised sidewalk stopped dead at the mouth with a raw gap -- exactly the
 * reported hole, and it clusters in the outskirts ramp band by construction.
 *
 * This is tg_r11_corner_stands WITHOUT the ramp gate: the pavement stands
 * wherever the run/gap PATTERN is built and a wall could actually stand there
 * (fork clearance and bridge/non-facade-biome edges DO remove the pavement, so
 * those two suppressions are kept), regardless of how many buildings the ramp
 * thinned. It is read ONLY by the pavement ARM (tg_r11_arm_side) and the
 * crossing base (tg_crossing_base) -- never by a wall emitter -- so a retracted
 * frontage still stands no lone side-street wall (R11 CITY item 10 is
 * untouched). TD5RE_R16_RAMP_JUNCTION=0 restores the ramp-blind junction. */
int tg_r16_pave_corner_stands(int si, int left)
{
    if (!td5_env_flag_on("TD5RE_R16_RAMP_JUNCTION"))
        return tg_r11_corner_stands(si, left);
    if (!td5_env_flag_on("TD5RE_R11_CITY_CORNER"))
        return tg_facade_built(si, left);
    if (!tg_facade_built(si, left)) return 0;
    /* The two suppressions that remove the PAVEMENT itself, mirrored from
     * tg_side_built; the ramp check there is deliberately NOT copied. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_SIDE_CLOSE") &&
        tg_branches_enabled() && !left && tg_span_in_fork_clear(si))
        return 0;
    if (td5_env_flag_on("TD5RE_AUTOTRACK_EDGE_CAP") && !tg_facade_stands(si))
        return 0;
    return 1;
}

/* Hash identifying the RUN span si belongs to on this side. A superblock now
 * holds up to TWO runs (before and after its side street), so keying pages and
 * floor counts on the superblock alone would give one texture and one height to
 * two buildings that are visibly separated by a street. */
unsigned int tg_facade_run_id(int si, int left)
{
    unsigned int block, phase, gs, gl;
    int av;
    tg_facade_block(si, left, &block, &phase, &gs, &gl, &av);
    /* The side is part of the key: with avenues both kerbs share a block index,
     * so without it the two facing blocks would be one building repeated. */
    return ((block * 2u + (phase >= gs + gl ? 1u : 0u)) * 2u
            + (unsigned)(left ? 1 : 0)) * 2246822519u;
}

int tg_city_district_floors(unsigned int block)
{
    double t, w;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_DISTRICTS")) return 0;
    t = (double)(block % (unsigned)TD5_TG_DISTRICT_BLOCKS)
      / (double)TD5_TG_DISTRICT_BLOCKS;
    w = 0.5 - 0.5 * cos(2.0 * 3.14159265358979 * t);
    return (int)(w * (double)TD5_TG_DOWNTOWN_FLOORS + 0.5);
}

/* Corner return at a run END, with real MASS. The first cut pushed a SINGLE
 * flat plane back along the lateral -- a zero-thickness sheet, which from any
 * oblique angle read as a folded piece of paper joined to the front wall: the
 * "two sides ... non width L shape" in the feedback. A run end now gets a
 * four-face prism (outer return, inner return one thickness into the run, the
 * rear face closing the back, and a roof), so the corner has body from every
 * angle and the block is capped when seen from a rise.
 *
 * `l` is the outward lateral unit, `ti` the along-road unit pointing INTO the
 * run. `cols` is how many whole page cells the return is deep (tg_facade_cap_cols
 * -- passing it in keeps the flank at the page's own aspect, like the front).
 * With thick == 0 the inner/rear/roof faces collapse to zero area, which the
 * screen-area cull drops -- that is the fallback when the knob is off. */
static void tg_facade_push_cap(double bx, double by, double bz,
                               double lx, double lz, double tix, double tiz,
                               double depth, double thick, double H, int rows,
                               int cols,
                               double *px, double *py, double *pz,
                               double *uu, double *vv, int *pn)
{
    const double dx = lx * depth, dz = lz * depth;
    const double tx = tix * thick, tz = tiz * thick;

    tg_facade_push_grid(bx, by, bz, dx, 0.0, dz, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, pn);
    tg_facade_push_grid(bx + tx, by, bz + tz, dx, 0.0, dz, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, pn);
    tg_facade_push_grid(bx + dx, by, bz + dz, tx, 0.0, tz, 0.0, H, 0.0,
                        1, rows, 0, rows, px, py, pz, uu, vv, pn);
    /* Roof: one horizontal cell, `up` reused as the along-road thickness. */
    tg_facade_push_grid(bx, by + H, bz, dx, 0.0, dz, tx, 0.0, tz,
                        cols, 1, 0, 1, px, py, pz, uu, vv, pn);
}

/* Push ONE quad from four explicit corners (near-bottom, far-bottom, far-top,
 * near-top order, like tg_facade_push_grid). Used where a face is not a
 * parallelogram -- a roof deck between a curved frontage and its back line --
 * so the corners can be taken from the geometry instead of a single lateral. */
static void tg_facade_push_quad(const double *xyz,
                                double *px, double *py, double *pz,
                                double *uu, double *vv, int *pn)
{
    static const double k_u[4] = { 0.0, 1.0, 1.0, 0.0 };
    static const double k_v[4] = { 1.0, 1.0, 0.0, 0.0 };
    const double e = TD5_TG_FACADE_UV_INSET;
    int i, n = *pn;
    if (n + 4 > TD5_TG_FACADE_MAXQUAD * 4) return;
    for (i = 0; i < 4; i++) {
        px[n] = xyz[i * 3 + 0];
        py[n] = xyz[i * 3 + 1];
        pz[n] = xyz[i * 3 + 2];
        uu[n] = k_u[i] > 0.5 ? 1.0 - e : e;
        vv[n] = k_v[i] > 0.5 ? 1.0 - e : e;
        n++;
    }
    *pn = n;
}

/* Which facade page a RUN uses -- keyed to the run hash so a whole building is
 * one texture but neighbouring buildings differ, the way a real street mixes
 * stone/glass/brick frontages. Variant 0 is the base WALL page; 1..N-1 are the
 * extra pages appended after GROUND.
 *
 * `rows` splits the palette by BUILDING CLASS, not at random: a run tall enough
 * to be a tower draws from the TOWER variants (office curtain wall) and anything
 * shorter from the low-rise masonry ones. Handing a 10-storey block a two-storey
 * shopfront texture is what makes a procedural city read as a texture soup
 * rather than a place. */
/* [R8 G1] Is `page` one of the VARIETY block's pages? The inventory line for
 * this area has to count what was SELECTED, not what was defined -- a page that
 * exists and is never picked is not a fix (the R7 item-4 lesson). Every R8
 * VARIETY emit site asks this and accounts on a hit, so "r8-variety: NONE
 * emitted" in race.log means the art really is unreachable. */
int tg_page_is_r8_variety(int page)
{
    return page >= TD5_TG_PAGE_R8_VARIETY
        && page <  TD5_TG_PAGE_R8_VARIETY + TD5_TG_R8_VARIETY_N;
}

int tg_facade_page_class(unsigned int gh, int rows)
{
    /* [R7 item 4] Draw from the UNION of the original 12 variants and the R7
     * variety pages, still split by class: a tower run picks among the original
     * tower variants (TOWER_FIRST..WALL_VARIANTS) plus the R7 tower pages, a
     * low-rise run among the original low variants (0..TOWER_FIRST) plus the R7
     * low pages. Same hash bits key the pick, so a given block keeps a stable
     * page. TD5RE_R7_CITY_VARIETY=0 restores the original 12-page pool for an
     * A/B. */
    /* [R8 G1 "more building variety"] The union grows again, in its OWN block:
     * R7's pages are untouched and the R8 pages are appended AFTER them, so the
     * pool is orig + R7 + R8 and the two knobs are independent
     * (TD5RE_R7_CITY_VARIETY=0 TD5RE_R8_VARIETY_FACADES=1 is a legal A/B).
     * Counts, tower class then low class:
     *   towers  3 orig + 5 R7 + 4 R8 = 12   lows  9 orig + 7 R7 + 6 R8 = 22
     * Same hash bits key the pick, so a block still keeps ONE stable page --
     * variety between buildings, not within one. */
    const int r7 = td5_env_flag_on("TD5RE_R7_CITY_VARIETY");
    const int r8 = td5_env_flag_on("TD5RE_R8_VARIETY_FACADES");
    unsigned int v;
    if (rows >= TD5_TG_FACADE_TALL_ROWS) {
        const int orig  = TD5_TG_WALL_VARIANTS - TD5_TG_WALL_TOWER_FIRST; /* 3 */
        const int extra = r7 ? TD5_TG_R7_WALL_TOWER_N : 0;
        const int more  = r8 ? TD5_TG_R8V_WALL_TOWER_N : 0;
        v = (gh >> 17) % (unsigned)(orig + extra + more);
        if ((int)v < orig)
            return TD5_TG_PAGE_WALL_EXTRA + TD5_TG_WALL_TOWER_FIRST + (int)v - 1;
        if ((int)v < orig + extra)
            return TD5_TG_PAGE_R7_WALL_TOWER + ((int)v - orig);
        return TD5_TG_PAGE_R8V_WALL_TOWER + ((int)v - orig - extra);
    } else {
        const int orig  = TD5_TG_WALL_TOWER_FIRST;                        /* 9 */
        const int extra = r7 ? TD5_TG_R7_WALL_LOW_N : 0;
        const int more  = r8 ? TD5_TG_R8V_WALL_LOW_N : 0;
        v = (gh >> 17) % (unsigned)(orig + extra + more);
        if ((int)v < orig)
            return v == 0 ? TD5_TG_PAGE_WALL
                          : (TD5_TG_PAGE_WALL_EXTRA + (int)v - 1);
        if ((int)v < orig + extra)
            return TD5_TG_PAGE_R7_WALL_LOW + ((int)v - orig);
        return TD5_TG_PAGE_R8V_WALL_LOW + ((int)v - orig - extra);
    }
}

/* [R15 TEX item 2] storefront anti-repeat rerolls. Declared here rather than
 * with the other R15 counters below because tg_store_page_reset needs it and
 * that sits with the page picker, not with the emitters. */
static long s_r15_store_reroll;

/* Which shop page a run's ground floor uses -- a different hash bit than the
 * wall page so the storefront and the tower above are chosen independently. */
static int tg_store_page_raw(unsigned int gh)
{
    return TD5_TG_PAGE_STORE + (int)((gh >> 23) % (unsigned)TD5_TG_STORE_VARIANTS);
}

/* [R15 TEX item 2] "a facade with a lot of chinese repeated elements ... this
 * in particular strike worse because it has text, we need to avoid repetition
 * on text textures."
 *
 * The pick above is a bare modulo over a SIX-page pool with no memory, so by
 * the birthday bound two consecutive facade runs land on the same storefront
 * about one time in six -- and a storefront is the surface at eye level, the
 * one carrying signage, so a repeat there reads as a copy-paste in a way a
 * repeated brick wall never does.
 *
 * Anti-repeat rather than a bigger pool: there are only 3+3 real source pages
 * (k_real_store + k_real_city_store), so "more variety" is not available
 * without new art. What IS available is never spending two ADJACENT runs on the
 * same page. One reroll on a different hash slice, then a forced step if the
 * reroll collides too -- so the result is still a pure function of the run hash
 * and the previous pick, and generation stays deterministic.
 *
 * One memory, not one per side: both sides of a span share a single mesh and
 * therefore a single storefront page (block_gh is tg_facade_run_id(si, 0)), so
 * the sequence a driver sees IS the span order this is called in. Reset per
 * build by tg_store_page_reset.
 * Knob TD5RE_R15_STORE_VARY (default ON) restores the memoryless pick. */
static int  s_r15_store_last = -1;

void tg_store_page_reset(void)
{
    s_r15_store_last = -1;
    s_r15_store_reroll = 0;
}

static int tg_store_page(unsigned int gh)
{
    int p = tg_store_page_raw(gh);
    if (!td5_env_flag_on("TD5RE_R15_STORE_VARY")) return p;
    if (p == s_r15_store_last) {
        /* Bits 11-13 are not read by any other facade decision on this run
         * (23-25 the store pick itself, 17-22 the wall class, 0-10 the block
         * pattern), so the reroll is independent of the pick it replaces. */
        const int alt = TD5_TG_PAGE_STORE
                      + (int)((gh >> 11) % (unsigned)TD5_TG_STORE_VARIANTS);
        p = (alt != p) ? alt
                       : TD5_TG_PAGE_STORE
                         + (((p - TD5_TG_PAGE_STORE) + 1) % TD5_TG_STORE_VARIANTS);
        s_r15_store_reroll++;
    }
    s_r15_store_last = p;
    return p;
}

/* Usable pavement width for a biome, 0 = "no raised pavement here" (the signal
 * the hook reads to lay a flat verge band instead -- see tg_verge_band_w). */
double tg_city_sidewalk_w(const TG_Biome *b)
{
    if (b->billboard || b->cell_w <= 0) return 0.0;
    return (double)b->sidewalk < TD5_TG_SIDEWALK_MIN ? TD5_TG_SIDEWALK_MIN
                                                     : (double)b->sidewalk;
}

double tg_city_sidewalk_w_at(const TG_NodeList *nl, int si,
                                    const TG_Biome *b)
{
    const double base = tg_city_sidewalk_w(b);
    double w;
    if (base <= 0.0) return 0.0;                    /* no pavement on this biome */
    if (!td5_env_flag_on("TD5RE_R10_WIDEWALK")) return base;
    w = tg_road_half_width(nl, si) * TD5_TG_WIDEWALK_FACTOR;
    if (w < base) w = base;                         /* never below the floor     */
    if (w > TD5_TG_WIDEWALK_MAX) w = TD5_TG_WIDEWALK_MAX;
    return w;
}

double tg_verge_band_w(const TG_Biome *b)
{
    if (tg_city_sidewalk_w(b) > 0.0) return 0.0;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_VERGE_BAND")) return 0.0;
    return TD5_TG_VERGE_W;
}

/* Rise of the pavement a facade stands on -- KERB_H only when the slab that
 * carries it is actually emitted. The wall used to add the kerb unconditionally
 * while the slab was behind TD5RE_AUTOTRACK_SIDEWALKS, so turning that knob off
 * left every building floating 130 raw over bare ground. */
double tg_city_kerb_h(const TG_Biome *b)
{
    if (!(tg_city_sidewalk_w(b) > 0.0)) return 0.0;
    return td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS") ? TD5_TG_KERB_H : 0.0;
}

/* ---- FACADE CELL SIZING (the page must map at its own aspect) -------------
 * A page cell is authored at cell_w x cell_h raw (CITY: 2150 x 2950, the
 * shipped level014 measurement). The geometry can only ever fit a WHOLE number
 * of cells along a frontage, and a frontage is one span -- 1500 raw -- so at
 * CITY sizes exactly one cell covers 1500 across while the code still gave it
 * 2950 up: the page came out squashed 1500/2150 = 0.70 across, which is the
 * "building textures should not be stretched" report. Windows read tall and
 * narrow, and the error is worse the wider the authored cell is.
 *
 * The along-road extent is not ours to choose (the wall must abut the span
 * endpoints or the street wall breaks), so the FLOOR HEIGHT is what gets
 * corrected: floor_h = cell_h * (effective cell width / authored cell width).
 * The page then maps at its authored aspect and the building is simply a little
 * shorter per floor.
 *
 * The scale is computed from the NOMINAL span length, not the span's own
 * length: neighbouring spans differ by a few percent on a curve (the outer
 * setback is a longer arc), and keying height to that would saw-tooth the
 * roofline of one continuous building. Residual stretch is that same few
 * percent, which is not visible. */
int tg_facade_cols_for(double len, double cell_w, int cap)
{
    int c;
    if (!(cell_w > 1.0)) return 1;
    c = (int)(len / cell_w + 0.5);
    if (c < 1) c = 1;
    if (c > cap) c = cap;
    return c;
}

/* Effective (as-built) cell width for biome b on a nominal-length frontage. */
static double tg_facade_cell_w(const TG_Biome *b)
{
    const double len = (double)TD5_TG_SPAN_LENGTH;
    return len / (double)tg_facade_cols_for(len, (double)b->cell_w, 4);
}

/* Height of ONE floor: the authored cell height, corrected so the page keeps
 * its aspect. Default ON; TD5RE_AUTOTRACK_FACADE_ASPECT=0 restores the raw
 * table height (and the stretch) for an A/B. */
double tg_facade_floor_h(const TG_Biome *b)
{
    if (b->cell_w <= 0 || b->cell_h <= 0) return (double)b->cell_h;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT"))
        return (double)b->cell_h;
    return (double)b->cell_h * tg_facade_cell_w(b) / (double)b->cell_w;
}

/* Whole cells across a corner return. Capped at 2 on purpose: the return is
 * emitted three faces deep at every run end, so each extra column costs
 * 2*rows+1 quads on a budget (TD5_TG_FACADE_MAXQUAD) that a tall tower already
 * nearly fills. */
static int tg_facade_cap_cols(const TG_Biome *b)
{
    return tg_facade_cols_for((double)b->depth, tg_facade_cell_w(b), 2);
}

/* Building depth, quantised to whole cells for the same reason as the height:
 * a return that is not a multiple of the cell width stretches the page across
 * the flank. CITY's authored 6000 raw becomes 2 x 1500 = 3000. */
double tg_facade_depth(const TG_Biome *b)
{
    if (b->cell_w <= 0) return (double)b->depth;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT"))
        return (double)b->depth;
    return (double)tg_facade_cap_cols(b) * tg_facade_cell_w(b);
}

/* [R8 item 6 -- "buildings look like they are all the same depth, some
 * buildings should have more depth"] How many whole cells deep THIS RUN is.
 *
 * Depth was one number per BIOME (tg_facade_depth above): every building on a
 * city street was the biome's `depth` field, quantised to the same 2 cells, so
 * the back line of the street was a single plane parallel to the road however
 * the heights varied. That is the complaint, and it is MASSING, not art -- no
 * amount of facade variety fixes a silhouette that is flat behind.
 *
 * Keyed to the RUN hash, exactly like the height and the page, so one building
 * has one depth and its neighbour has another, and a run keeps its depth along
 * its whole length (a building that changed depth span to span would saw-tooth
 * the back line, which is the same mistake tg_facade_floors avoids for height).
 * Whole cells, for the reason tg_facade_depth already gives: a return that is
 * not a multiple of the cell width stretches the page across the flank.
 *
 * BUDGET IS LOAD-BEARING here, not cosmetic. A corner return costs about
 * 2*cols*rows quads and TD5_TG_FACADE_MAXQUAD (320) is already nearly filled by
 * a 10-floor run with two caps a side; past it tg_facade_push_grid drops quads
 * SILENTLY, which reads as buildings missing their top floors. So a run tall
 * enough to be a tower gets at most one extra cell, a low-rise up to two.
 *
 * Depth grows along the OUTWARD lateral, i.e. away from the carriageway, so
 * this cannot push a building into the road and cannot trip the R7 guard.
 * TD5RE_R8_VARIETY_DEPTH=0 restores the single biome depth for an A/B. */
static int tg_facade_depth_cols(const TG_Biome *b, unsigned int gh, int rows)
{
    const int base = tg_facade_cap_cols(b);
    int extra;
    if (b->cell_w <= 0) return base;
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT")) return base;
    if (!td5_env_flag_on("TD5RE_R8_VARIETY_DEPTH")) return base;
    extra = (int)((gh >> 13) % 3u);                    /* 0, 1 or 2 cells */
    if (rows >= TD5_TG_FACADE_TALL_ROWS && extra > 1) extra = 1;
    return base + extra;
}

/* [R12 CITY item 10] The OUTWARD depth of the run at (si,left) -- what
 * tg_side_geom actually builds the corner return to, as opposed to the biome
 * constant tg_facade_depth. Stated once here because a second consumer needs it:
 * the cross-street frontage wall (tg_cross_emit_sidewalls) has to start BEYOND
 * the corner block's return or it grows out of the middle of it, and since R8
 * item 6 that return is per RUN (base + 0..2 extra cells), not per biome. The
 * caller passes the run's floor count, which it has already paid for. */
double tg_facade_run_depth(const TG_Biome *b, int si, int left,
                                  int floors, int *dcols_out)
{
    const int dcols = tg_facade_depth_cols(b, tg_facade_run_id(si, left), floors);
    if (dcols_out) *dcols_out = dcols;
    return (b->cell_w > 0 && td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_ASPECT"))
         ? (double)dcols * tg_facade_cell_w(b)
         : tg_facade_depth(b);
}

/* Along-road depth of a run-end corner return, keyed to the facade cell so it
 * scales with the biome's building size. 0.45 of a cell is enough mass to read
 * as a corner block without closing off the side street behind it. */
static double tg_facade_cap_thick(const TG_Biome *b)
{
    /* Default ON; TD5RE_AUTOTRACK_FACADE_MASS=0 restores the old flat plane. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS")) return 0.0;
    return (double)b->cell_w * 0.45;
}

/* Floor count of the run at span si on this side, 0 if the side is a gap. The
 * whole-block-then-tower-then-district climb was inlined in tg_side_geom; it is
 * pulled out here because the STEP-WALL pass (item 1) needs a NEIGHBOUR's height
 * without paying to build its full TG_SideGeom. tg_side_geom now calls this too,
 * so the two can never disagree on how tall a run is. */
int tg_facade_floors(int si, int left, const TG_Biome *b)
{
    unsigned int gh, blk, ph, gs, gl;
    int floors, av;

    if (!tg_facade_built(si, left)) return 0;
    gh = tg_facade_run_id(si, left);
    floors = b->floors_min + (int)((gh >> 7) % (unsigned)b->floors_extra);
    if (b->tower_mask && ((gh >> 3) & (unsigned)b->tower_mask) == 0)
        floors += 1 + (int)((gh >> 11) % 4);          /* whole-run tower cluster */
    tg_facade_block(si, left, &blk, &ph, &gs, &gl, &av);
    floors += tg_city_district_floors(blk);
    if (floors > TD5_TG_FACADE_MAX_ROWS) floors = TD5_TG_FACADE_MAX_ROWS;
    /* [R11 BIOME item 4] HEIGHT axis of the outskirts ramp. Applied LAST, after
     * the tower cluster and the downtown climb, so it caps whatever those
     * produced rather than racing them -- a tower cluster that rolls on the
     * first block of a town is exactly the lurch being fixed.
     *
     * The floor is one storey, never zero: a run the ramp keeps is a real
     * building, and "no building" is the DENSITY axis' job (tg_town_ramp_open),
     * not a zero-height wall. Ramping height also switches the ART for free --
     * tg_facade_page_class splits its palette at TD5_TG_FACADE_TALL_ROWS, so a
     * ramped-down run draws from the low-rise masonry pages instead of the
     * glass curtain-wall ones without this having to know that. */
    {
        const double r = tg_town_ramp(si);
        if (r < 1.0) {
            int capped = 1 + (int)((double)(floors - 1) * r + 0.5);
            if (capped < 1) capped = 1;
            if (capped < floors) floors = capped;
        }
    }
    return floors;
}

/* Streetlamp glows used to come from the prop layer as a lone additive
 * billboard at k_prop_pages[PP_LAMP].y_off = 2500 with NO POST under it, so the
 * light hung in mid-air -- the first item of the feedback. Real lamps (post +
 * arm/head + glow at the head) are emitted from tg_emit_fb_city instead, so the
 * prop layer must not also emit a bare glow. Kept as a switch rather than
 * deleting the prop block, so the old behaviour is one env var away for an A/B. */
static int tg_lamp_glow_from_props(const TG_Biome *b)
{
    if (!td5_trackgen_is_night()) return 0;    /* item 11: night tracks only */
    return b->prop_lamp && !td5_env_flag_on("TD5RE_AUTOTRACK_LAMP_POSTS");
}

/* Geometry for one side (0=right,1=left) of the wall at span si. built=0 when
 * the run/gap pattern or the branch-corridor exclusion skips this side. */
void tg_side_geom(const TG_NodeList *nl, int si, int left,
                         const TG_Biome *b, TG_SideGeom *g)
{
    const TG_Node *n0 = &nl->v[si];
    const TG_Node *n1 = &nl->v[si + 1];
    const double side = left ? 1.0 : -1.0;
    double set0, set1, flen;
    int floors;

    g->built = 0;
    if (!tg_facade_built(si, left)) return;
    if (tg_branches_enabled() && side * (double)tg_fork_side_at(si) > 0.0 && tg_span_in_fork_clear(si)) return;
    /* [R11 BIOME item 4] Outskirts ramp -- the same gate tg_side_built asks, so
     * the wall that is not emitted here is the wall the caps and step walls
     * already believe is absent. Placed before the lone-stub test so a run left
     * isolated BY the ramp is dropped too, instead of standing alone in the
     * gap the ramp just opened. */
    if (tg_town_ramp_open(si, left)) return;
    /* [R6 item 16] Never a lone one-span building (both neighbours unbuilt). */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_NO_STUB") && tg_facade_isolated(si, left))
        return;

    /* Height is a whole number of FLOORS, keyed to the RUN so a building is one
     * uniform block that steps at the next side street. tg_facade_floors folds
     * in the tower cluster and the downtown-district climb, and clamps to
     * TD5_TG_FACADE_MAX_ROWS -- a real ceiling, not cosmetic: at cols == 1 (every
     * city biome, a 1500-raw span against a 2150-raw cell) a run costs rows quads
     * for the front, rows for a tower back wall and 2*(2*rows + rows + 2) for the
     * two corner prisms -- about 12*rows + 4 per side. 10 floors keeps both sides
     * inside TD5_TG_FACADE_MAXQUAD; past it tg_facade_push_grid silently drops
     * quads, which shows up as buildings missing their top floors. */
    floors   = tg_facade_floors(si, left, b);
    g->rows  = floors;
    g->H     = (double)floors * tg_facade_floor_h(b);
    /* [R8 item 6] Depth is now per RUN, not per biome. Same run id the height
     * and the page use, so a building is one depth end to end. With the knob
     * off tg_facade_depth_cols returns tg_facade_cap_cols and this is exactly
     * the old tg_facade_depth. */
    g->depth = tg_facade_run_depth(b, si, left, floors, &g->dcols);

    g->lx0 = n0->tz * side; g->lz0 = -n0->tx * side;
    g->lx1 = n1->tz * side; g->lz1 = -n1->tx * side;
    /* Setback is the PAVEMENT width (tg_city_sidewalk_w), not the raw table
     * field: the wall must land on the back edge of the slab the hook lays, or
     * one of the two is left hanging. Base rises by the kerb for the same
     * reason -- the wall stands ON the pavement, not in the gutter.
     *
     * The setback is pushed out through the SHARED carriageway authority rather
     * than derived from a fixed width: BRANCH makes the corridor width per-fork
     * variable, so a facade keying off a constant road half-width would drift
     * onto a widened branch (the round-3 item 9 "buildings spawned ON the
     * branches"). tg_carriageway_clear_gap is the documented adoption for
     * facades and re-derives no fork arithmetic; it returns the pavement width
     * unchanged wherever the road edge is already the outermost tarmac -- every
     * span off a fork, and the whole +ve lateral -- so straight city blocks are
     * byte-identical. The right-side facade in a fork's cleared region is still
     * dropped by the tg_span_in_fork_clear guard above; this only hardens the
     * setback everywhere else, and keeps the whole building (front, caps and the
     * step wall, which all derive from set0/set1) off any carriageway. */
    {
        const double gap = tg_carriageway_clear_gap(nl, si, side,
                               tg_city_sidewalk_w_at(nl, si, b),
                               TD5_TG_CARRIAGEWAY_MARGIN);
        set0 = n0->width * 0.5 + gap;
        set1 = n1->width * 0.5 + gap;
        /* [R13 JUNCTION item 3] "BUILDINGS OVERLAPPING INTO THE ROAD" on a
         * close curve. The block's body is the frontage plane pushed `depth`
         * further along the SAME inward ray on both ends, so on the inside of a
         * bend the back face is the ruled quad that folds first: MEASURED, 25
         * spans of seed 20260901 carry a block whose back face lies past the
         * bend's convergence point, which puts it on the far limb of the bend
         * -- over the carriageway, over the pavement, or inside the block
         * opposite. Cap the depth at the room the bend actually leaves past the
         * pavement. Front, caps, roof and step wall all derive from bx/ax and
         * this depth, so the whole body follows one number. */
        {
            const double cap = tg_r13_fold_cap(nl, si, side, 0.0, 1e30,
                                               "TD5RE_R13_FOLD_MASS") - gap;
            /* Assigned only where the cap BINDS, so a run the bend does not
             * reach is bit-identical to the pre-R13 depth. */
            if (cap < g->depth)
                g->depth = (cap > TD5_TG_R13_MIN_DEPTH) ? cap
                                                        : TD5_TG_R13_MIN_DEPTH;
        }
    }

    g->bx = n0->x + g->lx0 * set0;
    g->bz = n0->z + g->lz0 * set0;
    /* [TOPOLOGY-FIRST] the wall stands on the WORLD's ground under its own
     * frontage line (the conformed bed beside an open span, the valley floor
     * beside a viaduct), never on the deck. */
    g->by = tg_world_h(g->bx, g->bz) + tg_city_kerb_h(b);
    if (g->by > n0->y + tg_city_kerb_h(b) + 400.0) g->by = n0->y + tg_city_kerb_h(b) + 400.0;
    g->ax = (n1->x + g->lx1 * set1) - g->bx;
    g->ay = n1->y - n0->y;
    g->az = (n1->z + g->lz1 * set1) - g->bz;

    /* [R11 CITY item 6] CORNER SETBACK. "The last building of the main road
     * before the intersection should end EARLIER, so it stops spawning over the
     * sidewalk" (20260901 span 466). The pavement ARM that turns the kerb down a
     * side street (tg_block_emit_arm) is a slab `sw` wide laid on the BUILT side
     * of the mouth -- it runs BACK along the main road from the corner node, by
     * exactly the sidewalk width, and outward down the street. The frontage ran
     * to that same corner node, so its last `sw` of wall, roof, mass and corner
     * prism stood ON that slab: the building overhung the side street's pavement.
     *
     * R10 already fixed the OTHER half of the same corner: tg_cross_emit_sidewalls
     * pushes the side street's own frontage wall along-road by `sw` for exactly
     * this reason ("the arm sidewalk now shows in front of the frontage"). The
     * main-road frontage was never given the matching setback, so the two walls
     * of one corner block disagreed by sw -- the main one sticking out past its
     * own return. Trimming the frontage by the same sw closes the corner: the two
     * walls now meet, and the arm pavement is clear from kerb to building line.
     *
     * The trim is applied only at an end where a junction arm ACTUALLY stands,
     * read from the shared tg_r11_arm_side rather than from the run/gap pattern,
     * so a run that ends at a bridge, a biome edge or a park keeps its full
     * frontage. Both ends can trim at once (a one-span block between two
     * streets), so the total is clamped: at most TD5_TG_R11_CORNER_KEEP of the
     * span may be given away, scaled proportionally, and the geometry after it
     * is a pure re-parametrisation of the same straight frontage segment --
     * caps, roof, mass and step wall all derive from bx/ax and follow it.
     * TD5RE_R11_CITY_CORNER=0 restores the flush frontage for an A/B. */
    if (td5_env_flag_on("TD5RE_R11_CITY_CORNER")) {
        const double al = sqrt(g->ax * g->ax + g->az * g->az);
        const double sw = tg_city_sidewalk_w(b);
        double tn = (tg_r11_arm_side(nl, si - 1, left) & 2) ? sw : 0.0;
        double tf = (tg_r11_arm_side(nl, si + 1, left) & 1) ? sw : 0.0;
        if (al > 1.0 && (tn > 0.0 || tf > 0.0)) {
            /* Two clamps, both measured on this span's OWN frontage length,
             * because that length is not the span length on a bend: the inside
             * kerb of a curve compresses the setback line (seed 20260901 span
             * 707 side R is 347 raw of a 1500 span). A share cap alone would
             * leave a 121-raw splinter of wall there, so the absolute floor
             * takes precedence -- below it the corner keeps its flush frontage,
             * which on such a span overhangs by less than it is long anyway. */
            const double room = al - TD5_TG_R11_CORNER_MIN;
            double cap = al * TD5_TG_R11_CORNER_KEEP;
            double fn, ff;
            if (cap > room) cap = room;
            if (cap <= 0.0) { tn = 0.0; tf = 0.0; }
            else if (tn + tf > cap) {
                const double k = cap / (tn + tf);
                tn *= k; tf *= k;
            }
            fn = tn / al; ff = tf / al;
            g->bx += g->ax * fn;
            g->by += g->ay * fn;
            g->bz += g->az * fn;
            g->ax *= 1.0 - fn - ff;
            g->ay *= 1.0 - fn - ff;
            g->az *= 1.0 - fn - ff;
            tg_acct(TG_ACCT_R11_CITY, si);
        }
    }

    flen = sqrt(g->ax * g->ax + g->az * g->az);
    if (flen < 1.0) flen = 1.0;
    /* [R16 CITY item 4] "this building is too small and looks distorted."
     * On a tight inside bend, or after the R11 corner trim, the as-built
     * frontage flen can fall well below one authored cell wide (measured 347
     * raw against a 1500 span). tg_facade_cols_for still rounds that up to a
     * single cell, so the wall page is squeezed to a fraction of its authored
     * width -- and the aspect correction does NOT compensate, because
     * tg_facade_cell_w keys on the NOMINAL span length, not this flen. The
     * result is a narrow, distorted stub. Below a sane minimum footprint, stand
     * no building at all: the kerb and raised pavement are emitted by other
     * hooks and remain, and the R16 outskirts park dressing fills the freed
     * ground. TD5RE_R16_MIN_FOOTPRINT=0 restores the squeezed stub for an A/B. */
    if (td5_env_flag_on("TD5RE_R16_MIN_FOOTPRINT") &&
        flen < 0.5 * tg_facade_cell_w(b))
        return;                                   /* g->built stays 0 */
    g->cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);

    /* A cap closes a flank whose neighbour side is NOT actually built -- which
     * includes a fork-cleared span (item 15), not just a run/gap boundary.
     * tg_side_built folds that in; on the left side and off any fork it is
     * exactly tg_facade_built, so straight blocks are unchanged. */
    g->cap_near = !tg_side_built(si - 1, left);
    g->cap_far  = !tg_side_built(si + 1, left);
    g->built = 1;
}

TG_R13Face s_r13_face[TD5_TG_MAX_SPANS][2];

static unsigned char s_r13_visit[TD5_TG_MAX_SPANS];  /* emitter reached span */

static int s_r13_nvtx[TD5_TG_MAX_SPANS];             /* vertices written      */

unsigned char s_r13_drop[TD5_TG_MAX_SPANS];   /* post-emit pass ate it */

static long s_r13_dropped;

void tg_r13_faces_reset(void)
{
    memset(s_r13_face, 0, sizeof s_r13_face);
    memset(s_r13_visit, 0, sizeof s_r13_visit);
    memset(s_r13_nvtx, 0, sizeof s_r13_nvtx);
    memset(s_r13_drop, 0, sizeof s_r13_drop);
    s_r13_dropped = 0;
}

void tg_r13_faces_dropped(int si)
{
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return;
    if (!s_r13_drop[si]) s_r13_dropped++;
    s_r13_drop[si] = 1;
}

static void tg_r13_faces_visit(int si)
{
    if (si >= 0 && si < TD5_TG_MAX_SPANS) s_r13_visit[si] = 1;
}

/* [R15] Round-15 counters OWNED BY THIS MODULE. They were one block in the
 * pre-split monolith, sitting next to the single report that read them; after
 * the trackgen split that report is per-module (tg_r15_city_report at the foot
 * of this file), so the counters live with the emitters that move them and stay
 * file-static where they belong.
 *   item 8b  blocks closed at the back
 *   item 1   no-entry disc posts modelled / picks moved off a non-junction span
 *   item 2   storefront anti-repeat rerolls
 *   item 5   monuments skipped for having frontage on both sides
 *   items 4+7+8a  back rows refused / pushed clear / pulled in to terminate */
static long s_r15_back_closed;
static long s_r15_sign_posts;
static long s_r15_sign_xing;
static long s_r15_statue_walled;
static long s_r15_backrow_nostreet;
static long s_r15_backrow_push;
static long s_r15_backrow_close;

/* Defined further down, beside tg_city_emit_backrows; used by it. */
static void tg_r15_occ_diag(const TG_FBHook *h, double sw);

static void tg_r13_faces_wrote(int si, int nvtx)
{
    if (si >= 0 && si < TD5_TG_MAX_SPANS) s_r13_nvtx[si] = nvtx;
}

static void tg_r13_faces_note(int si, int s, const TG_SideGeom *g, int retq)
{
    TG_R13Face *f;
    if (si < 0 || si >= TD5_TG_MAX_SPANS) return;
    f = &s_r13_face[si][s ? 1 : 0];
    f->emitted = 1;
    f->capn  = (unsigned char)(g->cap_near ? 1 : 0);
    f->capf  = (unsigned char)(g->cap_far ? 1 : 0);
    f->rets  = (unsigned char)(retq > 0 ? 1 : 0);
    f->rows  = (short)g->rows;
    f->dcols = (short)g->dcols;
}

void tg_r13_faces_report(int nspans)
{
    const int verbose = td5_env_flag_on("TD5RE_R13_FACES_REPORT");
    const int wspan = td5_env_int("TD5RE_R13_FACES_SPAN", -1, -1, 100000);
    const int wpad  = td5_env_int("TD5RE_R13_FACES_PAD", 12, 0, 4000);
    int si, s, ring = (s_ring_len > 0) ? s_ring_len : nspans;
    long sides = 0, ends = 0, open_ends = 0, runs = 0, flat_runs = 0;

    if (ring > nspans) ring = nspans;
    if (ring > TD5_TG_MAX_SPANS) ring = TD5_TG_MAX_SPANS;

    for (s = 0; s < 2; s++) {
        for (si = 0; si < ring; si++) {
            const TG_R13Face *f = &s_r13_face[si][s];
            if (verbose && wspan >= 0 && si >= wspan - wpad && si <= wspan + wpad)
                TD5_LOG_I(LOG_TAG, "r13faces: si=%d side=%s visit=%d emit=%d "
                          "drop=%d capn=%d capf=%d rets=%d rows=%d dcols=%d "
                          "nvtx=%d", si, s ? "L" : "R", (int)s_r13_visit[si],
                          (int)f->emitted, (int)s_r13_drop[si], (int)f->capn,
                          (int)f->capf, (int)f->rets, (int)f->rows,
                          (int)f->dcols, s_r13_nvtx[si]);
            if (!TG_R13_STANDS(si, s)) continue;
            sides++;
            if (si > 0 && TG_R13_STANDS(si - 1, s)) continue;
            {   /* first span of a run ON THE STRIP -- walk it once.
                 * The two ends of a surviving run are the two places a driver
                 * sees a building end-on, so they are what must carry a return.
                 * Measured on the SURVIVING run, not the emitted one: at a
                 * bridge exit the capped span was emitted and then eaten, and
                 * the run the strip actually carries starts one span later with
                 * a bare front plane. */
                int j = si, last = si, nopen = 0;
                while (j < ring && TG_R13_STANDS(j, s)) { last = j; j++; }
                runs++;
                ends += 2;
                if (!s_r13_face[si][s].capn) { open_ends++; nopen++; }
                if (!s_r13_face[last][s].capf) { open_ends++; nopen++; }
                if (nopen == 2) flat_runs++;
                if (verbose && nopen)
                    TD5_LOG_W(LOG_TAG, "r13faces: OPEN END side=%s spans "
                              "%d..%d (%d) near_cap=%d far_cap=%d rows=%d "
                              "dcols=%d", s ? "L" : "R", si, last,
                              last - si + 1, (int)s_r13_face[si][s].capn,
                              (int)s_r13_face[last][s].capf,
                              (int)f->rows, (int)f->dcols);
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "trackgen: r13faces SUMMARY frontage_runs=%ld "
              "FLAT_RUNS=%ld | span_sides=%ld run_ends=%ld "
              "MISSING_SIDE_FACES=%ld buildings_dropped=%ld",
              runs, flat_runs, sides, ends, open_ends, s_r13_dropped);
}

static int tg_emit_street_wall(const TG_NodeList *nl, int si,
                               const TG_Biome *b, TG_Buf *blk)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    TG_SideGeom sd[2];
    /* Pages are keyed to the RIGHT side's run: both sides share one mesh, so
     * they cannot carry different pages, and the run id at least keeps a whole
     * building on one texture across the side streets. */
    unsigned int block_gh = tg_facade_run_id(si, 0);
    const double cap_thick = tg_facade_cap_thick(b);
    /* [R8 item 6] The return depth used to be one biome constant read here for
     * the whole mesh; it now lives on each side's TG_SideGeom as `dcols`,
     * because the two sides belong to different runs and so to different
     * depths. Nothing mesh-wide is left to compute. */
    int n = 0, n_store, n_ret = 0, s, nseg, seg_page[2], seg_nq[2];
    int wall_rows, step_max = 0;

    if (si + 1 >= nl->count) return 1;    /* need the far endpoint to abut */
    tg_r13_faces_visit(si);
    tg_side_geom(nl, si, 0, b, &sd[0]);
    tg_side_geom(nl, si, 1, b, &sd[1]);

    /* [R18 WATER item 2] "these buildings are floating over water." A frontage
     * stands at ROAD level (g->by = node y + kerb) and was blind to a bridge
     * run's RIVER beside the crossing, so a block set back toward the water hung
     * over it. This span itself is never a bridge deck (tg_building_for_span
     * returns early there); the offender is a block on a NEAR span whose lateral
     * setback reaches the river. Drop a side whose frontage line lies over the
     * river -- built=0 is exactly how a side is already suppressed, so mesh
     * accounting is unchanged. Same rectangle the far-band cull and the trees use
     * (tg_point_over_bridge_water). TD5RE_R18_BUILDING_OVER_BRIDGE_WATER=0
     * restores the old placement. */
    if (td5_env_flag_on("TD5RE_R18_BUILDING_OVER_BRIDGE_WATER")) {
        int sw;
        for (sw = 0; sw < 2; sw++) {
            TG_SideGeom *g = &sd[sw];
            if (!g->built) continue;
            if (tg_point_over_bridge_water(nl, si, g->bx, g->bz) ||
                tg_point_over_bridge_water(nl, si, g->bx + g->ax, g->bz + g->az))
                g->built = 0;
        }
    }

    /* STOREFRONT pass: the ground floor (row 0) of each front plane goes FIRST
     * in the vertex list, so a leading command can bind the shop page. */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 0, 1,
                            px, py, pz, uu, vv, &n);
    }
    n_store = n;

    /* FACADE pass: the floors ABOVE the shop, plus the deep return walls that
     * turn the corner at run ends (the multi-sided buildings). */
    for (s = 0; s < 2; s++) {
        TG_SideGeom *g = &sd[s];
        if (!g->built) continue;
        tg_facade_push_grid(g->bx, g->by, g->bz, g->ax, g->ay, g->az,
                            0.0, g->H, 0.0, g->cols, g->rows, 1, g->rows,
                            px, py, pz, uu, vv, &n);
        /* MASS, on every span of the run rather than only at its ends. The
         * front plane is a zero-thickness sheet: at run INTERIOR spans there was
         * nothing at all behind it, so a tall run seen from a rise, a crest or
         * an approaching curve showed one face and a knife-edge roofline -- the
         * "taller buildings only have a facade ... just one visible face"
         * report. Two faces close it for the cost of a roof quad plus, on
         * TOWERS only, a back wall:
         *   ROOF  -- always. Four explicit corners (the back line follows the
         *            frontage through the curve), so the deck cannot gap at the
         *            joint the way a single-lateral parallelogram would.
         *   BACK  -- towers only. A low block is hidden by the back rows behind
         *            it, but a tower stands over them and was see-through from
         *            behind and down every cross street. Gated by height so the
         *            quad budget is only spent where it shows. */
        if (g->depth > 1.0 && td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS")) {
            const double d = g->depth;
            double q[12];
            q[0] = g->bx;                  q[1]  = g->by + g->H;
            q[2] = g->bz;
            q[3] = g->bx + g->ax;          q[4]  = g->by + g->ay + g->H;
            q[5] = g->bz + g->az;
            q[6] = g->bx + g->ax + g->lx1 * d; q[7] = g->by + g->ay + g->H;
            q[8] = g->bz + g->az + g->lz1 * d;
            q[9] = g->bx + g->lx0 * d;     q[10] = g->by + g->H;
            q[11] = g->bz + g->lz0 * d;
            tg_facade_push_quad(q, px, py, pz, uu, vv, &n);

            if (g->rows >= TD5_TG_FACADE_TALL_ROWS) {
                const double bx2 = g->bx + g->lx0 * d, bz2 = g->bz + g->lz0 * d;
                tg_facade_push_grid(bx2, g->by, bz2,
                                    (g->bx + g->ax + g->lx1 * d) - bx2, g->ay,
                                    (g->bz + g->az + g->lz1 * d) - bz2,
                                    0.0, g->H, 0.0, g->cols, g->rows, 0, g->rows,
                                    px, py, pz, uu, vv, &n);
            }
            /* [R15 CITY item 8b] "this building ... has no side to it, it looks
             * hollow."
             *
             * MEASURED CONTRADICTION, not a new rule. The TALL_ROWS gate above
             * is justified by "a low block is hidden by the back rows behind
             * it" -- but tg_city_emit_backrows refuses on exactly the opposite
             * condition: `if (gate && tg_facade_built(si, s)) continue`. Back
             * rows fill STREET GAPS only. A span whose frontage IS built (which
             * is every span that reaches this code) therefore has NOTHING
             * behind it, so a sub-TALL_ROWS block was open at the back down
             * every cross street and from every rise -- the reported hollow.
             *
             * Closed with ONE flat quad rather than the tower's full window
             * grid: that answers the see-through without spending a cols x rows
             * budget on massing nobody reads windows on. The tower keeps its
             * grid above. Same four corners as that grid's plane, wound
             * near-bottom / far-bottom / far-top / near-top to match
             * tg_facade_push_quad's u/v tables. */
            else if (td5_env_flag_on("TD5RE_R15_BACK_CLOSE")) {
                const double bx2 = g->bx + g->lx0 * d, bz2 = g->bz + g->lz0 * d;
                const double fx2 = g->bx + g->ax + g->lx1 * d;
                const double fz2 = g->bz + g->az + g->lz1 * d;
                double qb[12];
                qb[0] = bx2;  qb[1]  = g->by;                qb[2]  = bz2;
                qb[3] = fx2;  qb[4]  = g->by + g->ay;        qb[5]  = fz2;
                qb[6] = fx2;  qb[7]  = g->by + g->ay + g->H; qb[8]  = fz2;
                qb[9] = bx2;  qb[10] = g->by + g->H;         qb[11] = bz2;
                tg_facade_push_quad(qb, px, py, pz, uu, vv, &n);
                s_r15_back_closed++;
            }
        }
        n_ret = n;
        if (g->cap_near || g->cap_far) {
            /* Along-road unit, needed to give the corner prism its thickness.
             * Clamped to most of the span so a return can never overrun the
             * span it belongs to and poke out of the far end of the run. */
            const double alen = sqrt(g->ax * g->ax + g->az * g->az);
            const double aux = (alen > 1.0) ? g->ax / alen : 0.0;
            const double auz = (alen > 1.0) ? g->az / alen : 1.0;
            double thick = cap_thick;
            if (alen > 1.0 && thick > alen * 0.9) thick = alen * 0.9;
            /* [R8 item 6] The return is `dcols` cells deep, this RUN's depth,
             * not the one biome depth -- the prism has to follow the body or
             * the corner would fold back short of the roof it closes. */
            if (g->cap_near)
                tg_facade_push_cap(g->bx, g->by, g->bz, g->lx0, g->lz0,
                                   aux, auz, g->depth, thick, g->H, g->rows,
                                   g->dcols, px, py, pz, uu, vv, &n);
            if (g->cap_far)
                tg_facade_push_cap(g->bx + g->ax, g->by + g->ay, g->bz + g->az,
                                   g->lx1, g->lz1, -aux, -auz,
                                   g->depth, thick, g->H, g->rows,
                                   g->dcols, px, py, pz, uu, vv, &n);
        }
        tg_r13_faces_note(si, s, g, n - n_ret);
        /* SIDE WALL at a height STEP (item 1). Two ADJACENT built runs of
         * different height share a flush frontage, but the taller one's flank
         * ABOVE the shorter roof was open air, so a driver looked straight into
         * the building -- the "buildings with different height look like they are
         * hollow inside" report. A run END (the neighbour is a gap) is already
         * closed by the corner prism above; this closes a run-to-run height
         * CHANGE, which the prisms never touched because both spans are BUILT.
         *
         * Emitted only at the FAR boundary (si|si+1) so each seam is filled
         * exactly once -- the near boundary is the previous span's far boundary --
         * and only across the EXPOSED band [shorter roof, taller roof], so no quad
         * is spent where the two roofs already meet flush. The face lies in the
         * lateral-vertical plane at the shared endpoint, its depth the building's
         * own, so from either approach direction it caps the step. Same page as
         * the facade above (it IS the building's side); grouped after the
         * storefront so it lands in the wall command.
         *
         * Dedicated knob TD5RE_AUTOTRACK_STEP_WALLS (default on) so this one
         * feature can be A/B'd in isolation from the rest of the FACADE_MASS
         * pass. CARRIAGEWAY CLEARANCE is inherited: the base derives from set1,
         * which tg_side_geom now pushes out through the shared authority, so the
         * seam can no more land on a branch than the front facade can -- no
         * separate guard is needed here. */
        if (g->built && td5_env_flag_on("TD5RE_AUTOTRACK_FACADE_MASS") &&
            td5_env_flag_on("TD5RE_AUTOTRACK_STEP_WALLS") &&
            tg_side_built(si + 1, s)) {
            const int nrows = tg_facade_floors(si + 1, s, b);
            if (nrows > 0 && nrows != g->rows) {
                const int hi = nrows > g->rows ? nrows : g->rows;
                const int lo = nrows < g->rows ? nrows : g->rows;
                const double fh = tg_facade_floor_h(b);
                const double fx = g->bx + g->ax, fy = g->by + g->ay,
                             fz = g->bz + g->az;
                tg_facade_push_grid(fx, fy, fz,
                                    g->lx1 * g->depth, 0.0, g->lz1 * g->depth,
                                    0.0, (double)hi * fh, 0.0,
                                    g->dcols, hi, lo, hi,
                                    px, py, pz, uu, vv, &n);
                if (nrows > step_max) step_max = nrows;
                tg_acct(TG_ACCT_STEPWALL, si);
            }
        }
    }

    if (n <= 0) return 1;
    /* Pages keyed to the PERIOD-block (a run lives inside one block) so a whole
     * building keeps one shop + one wall page and neighbours differ. Ground
     * quads [0,n_store) sample the shop page, the rest the wall page. */
    /* Page CLASS follows the taller of the two sides -- they share one mesh, so
     * they share one wall page, and a tower opposite a shop row should read as
     * the tower it is. */
    wall_rows = sd[0].built ? sd[0].rows : 0;
    if (sd[1].built && sd[1].rows > wall_rows) wall_rows = sd[1].rows;
    /* A step wall borrowed from a taller NEIGHBOUR run can be the tallest thing
     * in the mesh; let it pull the wall page up to the tower class so the flank
     * matches the office block it belongs to rather than the low run beside it. */
    if (step_max > wall_rows) wall_rows = step_max;
    if (n_store > 0 && n_store < n) {
        seg_page[0] = tg_store_page(block_gh);  seg_nq[0] = n_store / 4;
        seg_page[1] = tg_facade_page_class(block_gh, wall_rows);
        seg_nq[1] = (n - n_store) / 4;
        nseg = 2;
    } else {
        seg_page[0] = (n_store >= n) ? tg_store_page(block_gh)
                                     : tg_facade_page_class(block_gh, wall_rows);
        seg_nq[0] = n / 4;
        nseg = 1;
    }
    /* [R8 G1 + item 6] Account this span for VARIETY when it actually took an
     * R8 facade page, or when its massing is DEEPER than the biome default --
     * the two halves of "more building variety" and "some buildings should have
     * more depth". Counted once per span, not once per side, so the number in
     * the inventory reads as "spans showing R8 variety". */
    {
        const int wall = seg_page[nseg - 1];
        const int base = tg_facade_cap_cols(b);
        const int deep = (sd[0].built && sd[0].dcols > base)
                      || (sd[1].built && sd[1].dcols > base);
        const int r8p = tg_page_is_r8_variety(wall)
                     || (nseg > 1 && tg_page_is_r8_variety(seg_page[0]));
        if (r8p || deep) tg_acct(TG_ACCT_R8_VARIETY, si);
        /* Census the WALL page (seg_page[0] is the storefront, a separate
         * pool this area does not touch) and each built side's depth. */
        tg_var_note(TG_VAR_FACADE, wall);
        if (sd[0].built) tg_var_note(TG_VAR_DEPTH, sd[0].dcols);
        if (sd[1].built) tg_var_note(TG_VAR_DEPTH, sd[1].dcols);
        /* [R15 TEX item 2] ... and the storefront, which the line above
         * deliberately skipped ("a separate pool this area does not touch").
         * That exclusion is why a repeated shop sign never showed up in any
         * report. n_store > 0 is the same test the seg_page assignment uses,
         * so this counts exactly the runs that actually got a storefront. */
        if (n_store > 0) tg_var_note(TG_VAR_STORE, seg_page[0]);
    }
    /* One mesh, but up to one frontage per side, and a ground-floor storefront
     * command only when the run actually got one. */
    tg_acct_n(TG_ACCT_BUILDING, si,
              (sd[0].built ? 1 : 0) + (sd[1].built ? 1 : 0));
    if (n_store > 0) tg_acct(TG_ACCT_SHOPFRONT, si);
    tg_r13_faces_wrote(si, n);
    return tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, seg_page, seg_nq, nseg);
}

int tg_facade_stands(int si)
{
    if (si <= 0) return 0;
    if (tg_span_in_bridge_run(si)) return 0;
    if (tg_up_clear_span(si)) return 0;      /* [R11 item 7c] under the highway */
    return tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0;
}

int tg_building_for_span(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    const TG_Biome *b;

    /* Only span 0 is kept clear, not the whole grid stretch. The old
     * `si <= TD5_TG_GRID_SPAN` skipped 24 spans -- 36000 raw, the entire
     * opening straight -- so the race began in bare ground and the buildings
     * only cut in once the player was already moving. Nothing here is ever ON
     * the road (the wall sits behind the pavement), so the grid does not need
     * the clearance; it only needs somewhere for the near cap to end. */
    if (si <= 0) return 1;
    if (tg_span_in_bridge_run(si)) return 1;   /* deck is clear -- see the river */
    /* [R11 item 7c] Nothing stands where the overpass deck flies over. The deck
     * is 3000 deep along the road and sweeps out to the drawn edge, so a facade
     * on this span intersects it -- "it must not touch other buildings". */
    if (tg_up_clear_span(si)) return 1;
    /* [R7 item 7] wall-vs-tree keyed on the hardened city edge so the frontage
     * ends cleanly with its sidewalk, not a span early/late (see the helper). */
    b = &k_biomes[tg_scenery_biome_index(si)];

    if (!b->billboard || b->tree_n <= 0)
        return tg_emit_street_wall(nl, si, b, blk);

    /* [S2] A verge TREE belongs on this span instead of a wall, but it is
     * emitted by tg_building_verge_tree below rather than here.
     *
     * Split out because this was one of only FOUR callers of
     * tg_r12_flora_accept, whose +/-3 span rejection window is the single
     * cross-entry order dependency in the whole scenery build. Isolating the
     * flora callers as leaf emitters is what lets their DECISIONS be taken in
     * span order independently of where the mesh eventually lands.
     *
     * MESH ORDER IS UNCHANGED by the split, which is why it can be verified
     * byte-for-byte: the tree was the TAIL of this function, and the
     * street-wall branch above returns, so wall and tree are mutually
     * exclusive -- at most one of them emits for any given span. */
    return 1;
}

/* [S2] The verge tree for `si`, lifted verbatim out of tg_building_for_span.
 * Re-derives that function's cheap pure gates instead of being handed them, so
 * the two can be called in sequence sharing no state. */
int tg_building_verge_tree(const TG_NodeList *nl, int si, TG_Buf *blk)
{
    unsigned int h = (unsigned)si * 2654435761u;
    const TG_Biome *b;
    const TG_TreePage *tp;
    double side, gap, tw, th, jit, cx, cz;
    int tv;

    if (si <= 0) return 1;
    if (tg_span_in_bridge_run(si)) return 1;
    if (tg_up_clear_span(si)) return 1;
    b = &k_biomes[tg_scenery_biome_index(si)];
    /* The wall branch owns this span; tg_building_for_span already emitted it. */
    if (!b->billboard || b->tree_n <= 0) return 1;

    /* Trees: density-gated camera-facing billboards. Each biome MIXES several
     * species (tree_set) picked per-tree, each with its own shipped size, and a
     * scale jitter that keeps the page aspect. */
    if ((int)(h >> 28) > b->density) return 1;

    side = ((h >> 3) & 1) ? 1.0 : -1.0;
    /* [R12 CROSS item 5] A verge tree plants at 800..3200 lateral -- inside a
     * forest side road's carriageway. The on-road guard cannot see that lane
     * (tg_carriageway_reach does not know about it), so the trunk is suppressed
     * here at the placement, the same way tg_flora_gap_clear keeps trees off a
     * branch corridor. */
    /* [R14 FCROSS item 1c] "a forest crossing must not be TOUCHED by trees."
     * R12's gate asked only about THIS span, so a trunk planted on the shoulder
     * span still leaned its billboard over the mouth -- a billboard is up to
     * ~2400 raw wide against a 1500-raw span, so half of it lands on the lane.
     * The window helper widens the same question by one span either side. */
    if (tg_r14_fcross_clear_side(nl, si, side)) return 1;
    tv  = b->tree_set[(h >> 13) % (unsigned)b->tree_n];
    tp  = &k_tree_pages[tv];

    jit = 0.8 + (double)((h >> 9) % 41) * 0.01;    /* 0.80 .. 1.20 */
    tw  = (double)tp->w * jit;
    th  = (double)tp->h * jit;
    gap = 800.0 + (double)((h >> 5) % 2400);       /* set back off the verge */
    gap = tg_flora_gap_clear(nl, si, side, gap);    /* never on a branch */

    {   /* [R7 item 18] on the ground, never on water/coastline */
        double base_y;
        if (!tg_flora_plant(nl, si, b, side, gap, tw, &cx, &cz, &base_y))
            return 1;

        /* [R12 item 4] not on top of a tree another emitter already planted. */
        if (!tg_r12_flora_accept(si, "near", tg_tree_slot(tv), side,
                                 cx, cz, tw, th)) return 1;
        tg_flora_diag(nl, si, "near", tg_tree_slot(tv), side, tw, th, cx, cz);
        tg_acct(TG_ACCT_TREE, si);
        return tg_emit_billboard_mesh(blk, cx, base_y, cz, tw * 0.5, th,
                                      tg_tree_slot(tv), 1);
    }
}

/* A verge side that the branch corridor bows into over the fork span range --
 * suppress props there for the same reason as facades/trees. */
int tg_side_blocked(int si, double side)
{
    return tg_branches_enabled() && side * (double)tg_fork_side_at(si) > 0.0
        && tg_span_in_fork_clear(si);
}

/* [R9 CITY item 4] Does the branch corridor ACTUALLY reach into this side's
 * verge at span si? tg_side_blocked above answers the BLANKET question -- "is
 * this span anywhere in a fork's cleared region" -- which is the right, cheap,
 * fail-safe answer for things that must never be near a corridor (facades,
 * trunks, props). It is the WRONG question for street furniture at the fork's two
 * MOUTHS, where the region is open but the corridor is still lined up with the
 * road and takes no verge at all.
 *
 * Item 4 ("there are no sidewalks on the merging of the branch on span 361") is
 * that error at the REJOIN. Measured on seed 99991 (r9city-win, fork 1 R=360):
 * spans 360/361/362 side R report reach=3000 against half=3000 -- the corridor
 * has fully rejoined and occupies nothing -- yet BOTH pavement providers are off
 * there. The main slab is dropped by the XSTOP gate because the block model
 * genuinely opens a frontage gap across those spans (blk=16, phase 8/9/10 of
 * gap [8,11)), and the pavement ARMS that are supposed to turn the kerb down that
 * side street are dropped by the blanket tg_side_blocked. Neither gate is wrong
 * on its own; together they leave bare ground where the branch merges.
 *
 * NOTE this is NOT the span-143 off-by-one at the other end: 143's drop was the
 * R8 taper on the slab, and 361's slab is at its FULL 900 width and dropped for a
 * different reason entirely. Same family (a fork-region blanket outliving its
 * reason), different mechanism -- measured, not inherited.
 *
 * Scoped deliberately to the junction furniture (arms + street flank) rather than
 * changed inside tg_side_blocked, because relaxing the blanket for facades and
 * trees is a separate question this item did not measure. Threshold matches
 * tg_pavement_side_width's, so the kerb and its arms agree span for span.
 * TD5RE_R9_CITY_ARM_MEASURED=0 restores the blanket gate for an A/B. */
int tg_side_corridor_here(const TG_NodeList *nl, int si, double side)
{
    if (!td5_env_flag_on("TD5RE_R9_CITY_ARM_MEASURED"))
        return tg_side_blocked(si, side);
    if (!tg_branches_enabled() || side * (double)tg_fork_side_at(si) < 0.0) return 0;
    if (!tg_span_in_fork_clear(si)) return 0;
    return (tg_carriageway_reach(nl, si, side)
            - tg_road_half_width(nl, si)) > 1.0;
}

/* [R14 BRANCH item 2a] Does the BRANCH CORRIDOR lay a pavement of its own
 * alongside main span si?
 *
 * MEASURED first, seed 20260901, TD5RE_R14_BRANCH_REPORT=1 (see the raw-band
 * sweep for why R9's DOUBLE_PAVEMENT could not see this -- it MERGES bands that
 * touch, so two slabs on the SAME lateral report as one healthy band):
 *
 *     si=511 side=R  city-slab[3000..4011] x branch-slab[3000..4088] ov=1011
 *     si=512 side=R  city-slab[3000..3782] x branch-slab[3188..4317] ov= 594
 *     si=513 side=R  city-slab[3000..3554] x branch-slab[3417..4545] ov= 137
 *
 * At si=511 the two are COINCIDENT: 1011 units of pavement laid twice, one slab
 * inside the other. Same class on the out-of-town pair, forks 0 and 1:
 * verge-band x branch-verge at 145/146/147, 320 and 359.
 *
 * The arithmetic is forced, not accidental. A corridor step k=0 has zero bow, so
 * tg_carriageway_reach is EXACTLY tg_road_half_width there; the main slab starts
 * at half and the branch slab starts at reach, i.e. at the same lateral. R8's
 * taper cannot help -- it is keyed on `over = reach - half`, which is 0 at the
 * mouth, so it returns the full width; and R9's contiguity rule only ever fires
 * on a POSITIVE gap, so a NEGATIVE one (an overlap) walks straight past it. R7,
 * R8 and R9 all measured the same edge and all measured it from the outside in.
 *
 * The stopping rule is OWNERSHIP. R9 already established what is wrong with the
 * main slab over a fork: it is laid on the PLAIN road edge, while the main
 * carriageway there has been narrowed to its left half and shifted away, so
 * from the first corridor span onward that slab is standing in the gore
 * attached to no carriageway at all. The branch's own kerb is derived from the
 * branch's ACTUAL bowed outer edge and therefore tracks the carriageway it
 * belongs to. So on a span where the corridor lays a pavement, the corridor
 * OWNS that edge and the main road's outer pavement yields -- which removes the
 * doubling at the mouth and the stranded island further along in one rule,
 * instead of tapering one slab into the other and hoping the widths cancel.
 *
 * Restricted to the CORRIDOR spans [F+1, F+L] -- the spans a corridor step
 * actually rides on. The widened APPROACH spans before F carry no corridor and
 * therefore no branch pavement, so R7 item 6's "keep the kerb until the branch
 * really takes the verge" behaviour there is untouched, and so is the rejoin.
 * TD5RE_R14_FORK_PAVE=0 restores the R7/R8/R9 taper chain for an A/B. */
static int tg_r14_fork_pave(void)
{
    return td5_env_flag_on("TD5RE_R14_FORK_PAVE");
}

static int tg_r14_branch_pave_here(int si)
{
    int i;
    if (!tg_r14_fork_pave() || !tg_branches_enabled()) return 0;
    for (i = 0; i < s_fork_count; i++) {
        const TG_Biome *b;
        if (si < s_forks[i].F + 1 || si > s_forks[i].F + s_forks[i].len) continue;
        b = &k_biomes[tg_scenery_biome_index(si)];
        if (tg_city_sidewalk_w(b) > 0.0 &&
            td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) return 1;
        if (tg_verge_band_w(b) > 0.0 &&
            td5_env_flag_on("TD5RE_R5_BRANCH_VERGE")) return 1;
    }
    return 0;
}

/* [R14 BRANCH item 2a] Is span si inside a fork region, where the whole
 * side-street stack is suppressed?
 *
 * MEASURED, seed 20260901: every one of the 11 fork-region pavement holes --
 * including the three ONE-SPAN ones at 527, 548 and 600 that the user reported,
 * all on the LEFT -- reports `gate=xstop(frontage-gap)` with `soft=7 hard=7`.
 * The soft and hard biome cells AGREE at every hole span, so this is NOT the
 * dither R11 GUARD hardened, and R11's fix cannot reach it. It is the R6 CROSS
 * item 1 rule ("the raised pavement STOPS at a road intersection") firing on a
 * frontage gap inside a fork -- where tg_r10_cross_gates has already refused to
 * lay the side street, tg_crossing_base has refused the crossing, and
 * tg_block_emit_arm's junction furniture is blocked too. The pavement is
 * dropped for a mouth that nothing opens: the R13 RAIL "owned by nobody" shape,
 * one feature over, and the same exception R13 RAIL wrote for a bridge ramp.
 *
 * R9's own fork-mouth sweep could not see it either, for a second reason worth
 * recording: it sweeps the RIGHT side only ("no corridor ever bows left"), and
 * every one of these holes is on the LEFT.
 *
 * Applied to the pavement AND to the kerb railing, which carries a verbatim
 * copy of the same gate -- R13 RAIL's note that "this and the pavement below"
 * move together is still true. TD5RE_R14_FORK_PAVE=0 for an A/B. */
static int tg_r14_fork_nostreet(int si)
{
    return tg_r14_fork_pave() && tg_branches_enabled() &&
           tg_span_in_fork_clear(si);
}

double tg_pavement_side_width(const TG_NodeList *nl, int si,
                                     double side, double sw)
{
    double over, half, reach;
    if (!(sw > 0.0)) return 0.0;
    /* [R14 FCROSS item 1d] "the sidewalk must STOP where the street is." The
     * forest verge band ran straight across the crossing mouth, so the lane
     * came out with a paved stripe painted over its throat. This is the ONLY
     * place the forest crossing touches the pavement layer: the run-end
     * authority both pavements already share is asked one more question, rather
     * than the city junction stack being re-gated to fire in a wilderness (see
     * the R12 CROSS block for why that gate must stay shut). A city biome can
     * never answer yes -- tg_r12_fcross_at requires FOREST -- so the raised slab
     * is bit-identical and only the band changes. */
    if (tg_r14_fcross_pave_stop(nl, si, side)) return 0.0;
    if (!tg_branches_enabled() || side * (double)tg_fork_side_at(si) < 0.0) return sw;
    if (!tg_span_in_fork_clear(si)) return sw;
    /* [R14 BRANCH item 2a] the corridor owns this edge -- see above. */
    if (tg_r14_branch_pave_here(si)) return 0.0;
    if (!td5_env_flag_on("TD5RE_R7_PRE_BRANCH_PAVE")) return 0.0;
    half  = tg_road_half_width(nl, si);
    reach = tg_carriageway_reach(nl, si, side);
    over  = reach - half;
    if (over <= 1.0) return sw;              /* corridor not into the verge yet */
    if (!td5_env_flag_on("TD5RE_R8_CITY_PAVE_TAPER")) return 0.0;   /* R7 rule */
    sw -= over;
    if (sw < TD5_TG_PAVE_MIN_W) return 0.0;
    /* [R9 CITY item 2] contiguity with the branch's own kerb, see above. */
    if (td5_env_flag_on("TD5RE_R9_CITY_PAVE_JOIN") &&
        reach - (half + sw) > TD5_TG_PAVE_JOIN)
        return 0.0;
    return sw;
}

long s_r10_prop_skipped;     /* placements refused: on side-street tarmac */

/* [R14 FCROSS] The round's four items as four counters, declared here because
 * the first of them is incremented by tg_prop_one just below. */
long s_r14_fcross_animal_moved; /* animals sent to the far verge        */

long s_r14_fcross_side_hit;     /* "crossing on this side?" answered yes */

long s_r14_fcross_prop_skip;    /* furniture / billboards refused       */

long s_r14_fcross_pave_stop;    /* pavement span-sides ended at a mouth */

long s_r14_fcross_worn;         /* crossings given the worn dirt page   */

/* Emit one prop billboard (prop-page index pp) beside span si on `side`, `gap`
 * world units past the road edge, recording its mesh offset. */
int tg_prop_one(const TG_NodeList *nl, int si, int pp, double side,
                       double gap, TG_Buf *m, size_t *moff, int *pn)
{
    const TG_Node *n = &nl->v[si];
    const TG_PropPage *P = &k_prop_pages[pp];
    double lx = n->tz * side, lz = -n->tx * side;
    double cx = n->x + lx * (n->width * 0.5 + gap);
    double cz = n->z + lz * (n->width * 0.5 + gap);
    size_t b0 = m->len;

    if (tg_side_blocked(si, side)) return 1;
    /* [R10 SPAN66 item 1] A spectator's setback is measured from the MAIN road
     * edge, so at a frontage gap the "pavement" it aims for is side-street
     * TARMAC. The people in the user's span-66 frame are THESE billboards, not
     * the R9 furniture -- same frame, same error, two emitters. */
    if (tg_xstreet_occupies(nl, si, side, gap - (double)P->w * 0.5)) {
        tg_xstreet_audit(nl, si, side, gap - (double)P->w * 0.5,
                         pp == PP_LAMP ? "lamp-glow" : "prop-billboard", 0);
        s_r10_prop_skipped++;
        return 1;
    }
    /* [R14 FCROSS item 1c] The forest lane is the same error one biome over: a
     * setback measured from the MAIN road edge lands on side-street tarmac. It
     * gets its own predicate rather than a branch inside the city one, because
     * the two streets are separate elements with separate reaches. */
    if (tg_r14_fcross_occupies(nl, si, side, gap - (double)P->w * 0.5)) {
        s_r14_fcross_prop_skip++;
        return 1;
    }
    tg_xstreet_audit(nl, si, side, gap - (double)P->w * 0.5,
                     pp == PP_LAMP ? "lamp-glow" : "prop-billboard", 1);
    moff[*pn] = b0;
    if (!tg_emit_billboard_mesh(m, cx, n->y + (double)P->y_off, cz,
                                (double)P->w * 0.5, (double)P->h,
                                tg_prop_slot(pp), P->tag))
        return 0;
    /* PP_LAMP is the streetlamp GLOW, emitted through this same helper by the
     * lamp fixture (and by the props layer when the biome puts glows there).
     * It is accounted as a LAMP at the fixture, so keep it out of the prop
     * bucket or every lamp shows up twice under two different kinds. */
    if (m->len > b0) {
        (*pn)++;
        if (pp != PP_LAMP) tg_acct(TG_ACCT_PROP, si);
    }
    return 1;
}

/* Spectator density for biome `b`, overriding the shared table's prop_people.
 * The gate below fires when (hash>>28) <= density, so density d covers (d+1)/16
 * of spans. The table put crowds in four biomes -- CITY 6 (44% of spans),
 * COAST 6 (44%), ORIENTAL 4 (31%), INDUSTRIAL 3 (25%) -- which reads as a
 * permanent crowd lining an empty industrial road or a temple lane. A crowd is
 * a city (and seafront promenade) thing, so those keep a real density and
 * everywhere else drops to a rare passer-by:
 *   CITY 6 -> 44%   COAST 3 -> 25%   any other crowded biome 1 -> 12.5%
 * An accessor rather than an edit to k_biomes, so the shared table stays a
 * single definition; matched on name so it survives a table reorder. */
static int tg_people_density(const TG_Biome *b)
{
    if (b->prop_people <= 0) return 0;             /* biome wants none */
    if (!strcmp(b->name, "CITY"))  return b->prop_people;
    if (!strcmp(b->name, "COAST")) return 3;       /* promenade, not a grandstand */
    return 1;
}

/* [R13 PROPS item 7a] Animal-context share counters. Declared with the layer
 * that moves them rather than with the rest of the round's counters (which live
 * with the R9 INFRA furniture, 9000 lines further down) so the number and the
 * decision that produces it stay in one place. */
long s_r13_animal_kept;     /* animal picks on an un-paved biome       */

long s_r13_animal_town;     /* dropped: a town street is not a pasture */

/* Roadside prop layer for span si (spectators, streetlamps, statues, animals),
 * additional to the trees/facades. Each emitted billboard records its own mesh
 * offset, so props may be a variable count of differently-sized meshes. */
int tg_emit_props(const TG_NodeList *nl, int si, const TG_Biome *b,
                         TG_Buf *m, size_t *moff, int *pn, int cap)
{
    unsigned int h = (unsigned)si * 0x9E3779B9u;   /* independent of the tree hash */
    const int people = tg_people_density(b);
    double side;

    if (tg_span_in_bridge_run(si)) return 1;   /* nothing on the deck but rails */

    /* People: spectators on the sidewalk, sometimes a pair. */
    if (people > 0 && (int)(h >> 28) <= people && *pn < cap) {
        int pp = PP_PERSON0 + (int)((h >> 5) & 1);
        side = ((h >> 3) & 1) ? 1.0 : -1.0;
        if (!tg_prop_one(nl, si, pp, side, 400.0 + (double)((h >> 6) % 800),
                         m, moff, pn)) return 0;
        if (((h >> 20) & 1) && *pn < cap &&
            !tg_prop_one(nl, si, pp ^ 1, side,
                         1000.0 + (double)((h >> 7) % 700), m, moff, pn))
            return 0;
    }
    /* Streetlamp glows: periodic, both curbs (additive). */
    if (tg_lamp_glow_from_props(b) && (si % 7) == 0) {
        int s;
        for (s = 0; s < 2 && *pn < cap; s++)
            if (!tg_prop_one(nl, si, PP_LAMP, s ? 1.0 : -1.0, 300.0,
                             m, moff, pn)) return 0;
    }
    /* Statue / monument: sparse landmark. */
    if (b->prop_statue >= 0 && (si % 29) == 0 && *pn < cap) {
        side = ((h >> 9) & 1) ? 1.0 : -1.0;
        /* [R15 PROPS item 5] "this billboard sometimes clips into buildings, it
         * should be placed only on plazas."
         *
         * The monument is a 1800 x 4200 billboard planted at a FIXED gap of
         * 1500 (below), and tg_prop_one's three refusals (tg_side_blocked,
         * tg_xstreet_occupies, tg_r14_fcross_occupies) are all about ROAD
         * surfaces -- none of them knows where a facade wall stands. In CITY the
         * frontage setback is well inside 1500 + half the billboard, so on any
         * span with built frontage the landmark is planted through the shop
         * wall. It also skips tg_carriageway_clear_gap, which every other piece
         * of verge scenery goes through.
         *
         * "Only on plazas" as an implementable rule: a plaza is an OPENING in
         * the frontage, and tg_facade_built is this generator's single answer to
         * "is there a wall on this side here" -- the same predicate the back
         * rows, the cross street and the corner arms all key off. So prefer the
         * hashed side, fall back to the other, and if BOTH sides are walled
         * there is no plaza at this span and the landmark is skipped. Skipped,
         * not substituted: unlike the R12 disc this is a 1-in-29 LANDMARK, so
         * there is no density to preserve and a stand-in bin would be a
         * different object rather than the same object placed better. */
        int plaza = 1;
        if (td5_env_flag_on("TD5RE_R15_PLAZA_ONLY")) {
            const int want = (side > 0.0) ? 1 : 0;
            if (tg_facade_built(si, want)) {
                if (!tg_facade_built(si, want ^ 1)) side = -side;
                else { plaza = 0; s_r15_statue_walled++; }
            }
        }
        if (plaza &&
            !tg_prop_one(nl, si, b->prop_statue, side, 1500.0, m, moff, pn))
            return 0;
    }
    /* Animals: low density, set well back off the verge.
     *
     * [R13 PROPS item 7a] "Do not put ANIMAL textures in cities." Same CLASS of
     * bug as R12's no-entry disc and the same machinery, not a second gate: a
     * grazing animal is a claim about its surroundings, and on a paved street
     * with kerbs and shop fronts that claim is false, so no density makes it
     * right. Instrumented rather than guessed -- the deer in the frame are at
     * span 1677, which TD5RE_R9_INFRA_REPORT names ALPTOWN/pavement, and
     * k_biomes gives ALPTOWN (the R8 snow TOWN, sidewalk 400, facade frontage)
     * prop_animal = PP_DEER. It is the only settled biome carrying an animal.
     *
     * The discriminator is the PAVEMENT, not urbanity and not the biome name:
     * FIELDS is urbanity 1 as well and its sheep are correct, and what separates
     * the two is that FIELDS has no kerb. tg_city_sidewalk_w is the generator's
     * single answer to "is there a real street here", which is the same
     * predicate tg_infra_sign_biome_ok uses -- so this reads it rather than
     * inventing a parallel one, and any future town biome is covered free.
     *
     * DENSITY IS NEUTRAL. A refused animal is SUBSTITUTED by a townsperson at
     * the same side and the same setback, so the prop count, the element
     * inventory and TG_ACCT_PROP are unchanged and only the animal SHARE moves.
     * A pedestrian across a plaza is what belongs where the deer were standing.
     * TD5RE_R13_ANIMAL_CTX=0 restores the unconditional animal. */
    if (b->prop_animal >= 0 && (int)(h >> 28) <= 2 && *pn < cap) {
        int pp = b->prop_animal;
        side = ((h >> 11) & 1) ? 1.0 : -1.0;
        if (td5_env_flag_on("TD5RE_R13_ANIMAL_CTX")
            && tg_city_sidewalk_w(b) > 0.0) {
            pp = PP_PERSON0 + (int)((h >> 15) & 1);
            s_r13_animal_town++;
        } else {
            s_r13_animal_kept++;
        }
        /* [R14 FCROSS item 1c] "a forest crossing must not be touched by props"
         * -- the deer standing in the lane in the span-88 frame are THESE
         * billboards. Same SUBSTITUTION discipline R13 used for the town deer,
         * one axis over: a deer in a forest is right, it is only the SIDE that
         * is wrong, so the animal moves to the other verge instead of being
         * deleted. Prop count, TG_ACCT_PROP and the element inventory are
         * unchanged; only which verge it grazes on moves. If BOTH sides are
         * blocked (they never are -- a crossing has one side) the flip is left
         * alone and tg_prop_one's own gate refuses it. */
        if (td5_env_flag_on("TD5RE_R14_FCROSS_CLEAR")
            && tg_r14_fcross_clear_side(nl, si, side)
            && !tg_r14_fcross_clear_side(nl, si, -side)) {
            side = -side;
            s_r14_fcross_animal_moved++;
        }
        if (!tg_prop_one(nl, si, pp, side,
                         2500.0 + (double)((h >> 12) % 4000), m, moff, pn))
            return 0;
    }
    return 1;
}

/* Does span si carry city street furniture at all? Tree biomes have no
 * pavement (tg_city_sidewalk_w returns 0), and a bridge deck carries only its
 * own rails -- a pavement there would hang off the side of the deck. */
int tg_city_span_paved(const TG_FBHook *h)
{
    if (tg_span_in_bridge_run(h->si)) return 0;
    /* [R7 item 7] hardened city edge -- the raised pavement + kerb HEIGHT run to
     * the true end of the city cell instead of dithering out over the blend band.
     * Keep sw (below) sourced the same way so paved and width never disagree. */
    return tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(h->si)]) > 0.0;
}

/* Lateral frame for span si: both road edges at f=0 and f=1 plus the outward
 * unit at each end. Same computation tg_emit_ground uses, so the pavement
 * follows width changes, curvature and elevation exactly and abuts the skirt
 * without a seam. `out[]` = near-in-x, y, z, far-in-x, y, z, near-ux, near-uz,
 * far-ux, far-uz for side `sg` (+1 = left of travel). */
void tg_city_edge_frame(const TG_NodeList *nl, int si, double sg,
                               double *out)
{
    double nlx, nly, nlz, nrx, nry, nrz;
    double flx, fly, flz, frx, fry, frz;
    double nux, nuz, fux, fuz, len;

    tg_road_edge(nl, si, 0.0, 0.0, 1.0, &nlx, &nly, &nlz, &nrx, &nry, &nrz);
    tg_road_edge(nl, si, 1.0, 0.0, 1.0, &flx, &fly, &flz, &frx, &fry, &frz);
    nux = nlx - nrx; nuz = nlz - nrz;
    len = sqrt(nux * nux + nuz * nuz);
    if (len < 1e-6) { nux = 1.0; nuz = 0.0; } else { nux /= len; nuz /= len; }
    fux = flx - frx; fuz = flz - frz;
    len = sqrt(fux * fux + fuz * fuz);
    if (len < 1e-6) { fux = 1.0; fuz = 0.0; } else { fux /= len; fuz /= len; }

    if (sg > 0.0) {
        out[0] = nlx; out[1] = nly; out[2] = nlz;
        out[3] = flx; out[4] = fly; out[5] = flz;
        out[6] = nux; out[7] = nuz; out[8] = fux; out[9] = fuz;
    } else {
        out[0] = nrx; out[1] = nry; out[2] = nrz;
        out[3] = frx; out[4] = fry; out[5] = frz;
        out[6] = -nux; out[7] = -nuz; out[8] = -fux; out[9] = -fuz;
    }
}

/* Append one quad (loop order given by the caller) to the vertex arrays. */
void tg_city_push_quad(double *px, double *py, double *pz,
                              double *uu, double *vv, int *pn,
                              const double *xyz, const double *uv)
{
    int i, n = *pn;
    for (i = 0; i < 4; i++) {
        px[n] = xyz[i * 3 + 0];
        py[n] = xyz[i * 3 + 1];
        pz[n] = xyz[i * 3 + 2];
        uu[n] = uv[i * 2 + 0];
        vv[n] = uv[i * 2 + 1];
        n++;
    }
    *pn = n;
}

/* RAISED PAVEMENT between kerb and facade, one mesh for both sides: a top slab
 * plus the kerb face that closes the step down to the asphalt. The slab top
 * sits at TD5_TG_KERB_H, which is exactly where tg_side_geom now starts the
 * wall, so the two meet flush.
 *
 * UVs are isotropic (u = width/SPAN_LENGTH, v advances one tile per span) for
 * the same reason tg_emit_ground does it: a stretched u both smears the paving
 * and defeats the box-filter mips, which is what made the old ground shimmer. */
/* [R12 OVERPASS item 14b] Does the raised pavement actually STAND on side `s`
 * (1 = left) at span si? These are tg_city_emit_sidewalk's own gate conditions
 * stated once, so the end cap below closes exactly the runs the slab really
 * has -- a cap keyed off a looser predicate would float where the pattern says
 * "paved" but the emitter skipped the span, which is the mistake R5 item 15
 * documents for the facade caps. */
int tg_r12_pave_stands(const TG_NodeList *nl, int si, int s)
{
    const TG_Biome *b;
    double sw;
    if (!nl || si < 0 || si + 1 >= nl->count) return 0;
    if (tg_span_in_bridge_run(si)) return 0;          /* the deck carries none */
    /* [R14 BRANCH item 2a] The CORRIDOR'S kerb continues the run on this side,
     * so the run does not end here and must not be capped. Measured on seed
     * 20260901 with the ownership rule in: the doubling at 512/513 was gone but
     * si=511 still reported city-slab[3000..4011] against branch-slab, and the
     * band was span 510's own -- its FAR end cap, standing in the plane where
     * the branch pavement takes over. This predicate is R12's "is there a
     * neighbouring pavement on this side", and the honest answer once ownership
     * transfers is YES, by a different emitter. RIGHT side and a RAISED branch
     * slab only: a flat verge band has no cross-section to close, so a run
     * ending into one still wants its cap. */
    if (!s && tg_r14_branch_pave_here(si) &&
        tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0)
        return 1;
    b = &k_biomes[tg_scenery_biome_index(si)];
    if (!(tg_city_sidewalk_w(b) > 0.0)) return 0;
    sw = tg_city_sidewalk_w_at(nl, si, b);
    if (!(tg_pavement_side_width(nl, si, s ? 1.0 : -1.0, sw) > 0.0)) return 0;
    /* [R13 RAIL item 5b] mirrors the emitter's own ramp exception above -- this
     * predicate exists to be the emitter's gate stated once, so it has to move
     * with it or the R12 end cap floats where the slab is not. */
    /* [R14 BRANCH item 2a] and the same for a fork region, where the side
     * street this rule drops the slab FOR is suppressed wholesale. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XSTOP") && !tg_facade_built(si, s) &&
        !tg_r13_approach_span(si) && !tg_r14_fork_nostreet(si))
        return 0;
    return 1;
}

/* [R15 CITY item 6] Raised slabs dropped because a crossing is painted here. */
long s_r15_pave_xing;

int tg_city_emit_sidewalk(const TG_FBHook *h, double sw)
{
    double px[48], py[48], pz[48], uu[48], vv[48];
    double e[10], q[12], t[8];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int s, n = 0, sides = 0;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* [R8 CITY item 2] PER SIDE: the branch corridor eats into the right
         * verge gradually, so the slab there narrows span by span rather than
         * disappearing at the first unit of overlap. */
        const double sw_s = tg_pavement_side_width(h->nl, h->si, sg, sw);
        const double u_w = sw_s / (double)TD5_TG_SPAN_LENGTH;
        const double u_k = TD5_TG_KERB_H / (double)TD5_TG_SPAN_LENGTH;
        if (!(sw_s > 0.0)) continue;
        /* [R6 CROSS item 1] The raised pavement STOPS at a road intersection.
         * Where this side opens onto a side-street mouth (the frontage is a gap
         * here), the slab used to run straight across the mouth at kerb height,
         * so the pavement was "lifted during the crossing" and floated over the
         * cross-street asphalt that tg_city_emit_crossstreet lays at road level
         * (framedump top-down spans 178 / 206, seed 99991). Dropping the slab on
         * the mouth side leaves the carriageway flush and the corner pavement
         * arms (tg_block_emit_intersection) turn the sidewalk down the side
         * street instead. The built side keeps its pavement. */
        /* [R13 RAIL item 5b] ... except on a bridge ramp, where consequence 3
         * has already removed the side street this rule is dropping the slab
         * FOR. Without the exception the pavement is deleted for a mouth that
         * no longer exists and the run stops short of the deck. */
        /* [R14 BRANCH item 2a] ... and the same is true inside a FORK region:
         * tg_r10_cross_gates refuses the side street there, tg_crossing_base
         * refuses the crossing and the junction arms are blocked, so once again
         * the slab is being deleted for a mouth that nothing opens. MEASURED:
         * 11 fork-region holes on seed 20260901, three of them one-span, every
         * one reporting this gate and every one with soft == hard (so not the
         * dither R11 GUARD hardened). See tg_r14_fork_nostreet. */
        if (td5_env_flag_on("TD5RE_AUTOTRACK_XSTOP") &&
            !tg_facade_built(h->si, s) && !tg_r13_approach_span(h->si) &&
            !tg_r14_fork_nostreet(h->si))
            continue;
        /* [R15 CITY item 6] "this sidewalk is on top of another crossing."
         *
         * The R6 rule above is the right rule applied to only half its cases:
         * it drops the raised slab at a SIDE-STREET MOUTH (frontage gap) but
         * never asks tg_city_crossing_here, so at a painted crossing the slab
         * still runs through at TD5_TG_KERB_H over a decal that
         * tg_city_emit_crossing authors edge-to-edge AT ROAD LEVEL -- the same
         * "pavement lifted during the crossing" geometry the R6 comment
         * describes, reached by the other door. A crossing is an intersection
         * for this purpose too, so it gets the same answer. The corner arms
         * (tg_block_emit_intersection) carry the footway through, exactly as
         * they do at a mouth. */
        /* [R18 CITY items 3+4] "this building has no sidewalk" / "this sidewalk
         * is not properly folding into the intersection." R15 drops the raised
         * slab on BOTH sides of a crossing "expecting the corner arms to carry
         * the footway through, exactly as at a mouth." But tg_crossing_base only
         * paints a zebra where at least one side is a MOUTH (!tg_facade_built,
         * line ~2954), and tg_r11_arm_side ONLY fires on a mouth side too -- so on
         * the crossing's OTHER, BUILT side R15 removes the slab and no arm ever
         * replaces it: the built frontage loses its pavement (item 3) and the
         * footway stops dead at the junction instead of folding through it
         * (item 4). The zebra is authored road-edge to road-edge, inside the
         * carriageway; the raised slab sits beyond the kerb, so the two never
         * overlap and keeping the built side's slab reintroduces no z-fight. So
         * scope the crossing drop to mouth sides only, exactly where R6's XSTOP
         * and the arms already agree. TD5RE_R18_XING_BUILT_KEEP=0 restores R15's
         * blanket both-sides drop for an A/B. */
        if (td5_env_flag_on("TD5RE_R15_XING_PAVE") &&
            tg_city_crossing_here(h->si) && !tg_r13_approach_span(h->si) &&
            (!td5_env_flag_on("TD5RE_R18_XING_BUILT_KEEP") ||
             !tg_facade_built(h->si, s))) {
            s_r15_pave_xing++;
            continue;
        }
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Top slab: near-in, near-out, far-out, far-in. */
        q[0] = e[0];               q[1]  = e[1] + TD5_TG_KERB_H; q[2]  = e[2];
        q[3] = e[0] + e[6] * sw_s; q[4]  = e[1] + TD5_TG_KERB_H; q[5]  = e[2] + e[7] * sw_s;
        q[6] = e[3] + e[8] * sw_s; q[7]  = e[4] + TD5_TG_KERB_H; q[8]  = e[5] + e[9] * sw_s;
        q[9] = e[3];               q[10] = e[4] + TD5_TG_KERB_H; q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_w; t[3] = (double)h->si;
        t[4] = u_w; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);

        /* Kerb face, road-facing: bottom sits ON the asphalt so there is no
         * lip between the two, top meets the slab. */
        q[0] = e[0]; q[1]  = e[1];                 q[2]  = e[2];
        q[3] = e[0]; q[4]  = e[1] + TD5_TG_KERB_H; q[5]  = e[2];
        q[6] = e[3]; q[7]  = e[4] + TD5_TG_KERB_H; q[8]  = e[5];
        q[9] = e[3]; q[10] = e[4];                 q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_k; t[3] = (double)h->si;
        t[4] = u_k; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);

        /* [R14 BRANCH item 2b] OUTER FACE -- see the block comment above. */
        if (tg_r14_pave_face()) {
            const double ox = e[0] + e[6] * sw_s, oz = e[2] + e[7] * sw_s;
            const double fx = e[3] + e[8] * sw_s, fz = e[5] + e[9] * sw_s;
            const double u_f = (TD5_TG_KERB_H + TD5_TG_GROUND_DROP)
                             / (double)TD5_TG_SPAN_LENGTH;
            q[0] = ox; q[1]  = e[1] + TD5_TG_KERB_H;    q[2]  = oz;
            q[3] = ox; q[4]  = e[1] - TD5_TG_GROUND_DROP; q[5]  = oz;
            q[6] = fx; q[7]  = e[4] - TD5_TG_GROUND_DROP; q[8]  = fz;
            q[9] = fx; q[10] = e[4] + TD5_TG_KERB_H;    q[11] = fz;
            t[0] = 0.0; t[1] = (double)h->si;
            t[2] = u_f; t[3] = (double)h->si;
            t[4] = u_f; t[5] = (double)h->si + 1.0;
            t[6] = 0.0; t[7] = (double)h->si + 1.0;
            tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            s_r14_outer_faces++;
        }
        sides++;

        /* [R12 OVERPASS item 14b] END CAP. "Elevated sidewalks ... stop abruptly
         * just before the bridge. They need a proper termination."
         *
         * The slab and its kerb face are two open ribbons: there is a top and a
         * road-facing side and NOTHING closing the cross-section, so wherever a
         * pavement run ends you look straight into a 130-high hollow step. It is
         * invisible mid-run because the neighbour's ribbons continue, and it is
         * unmissable at a run END -- which is exactly a bridge mouth, because
         * tg_city_span_paved returns 0 on every span of a bridge run, so the
         * pavement stops dead one span before the deck.
         *
         * Same defect class as feedback item 7 this round ("the beginning of the
         * median has no visible end face"): geometry that was given sides and a
         * top but no cap. Closed with the cross-section quad at whichever end has
         * no neighbouring pavement on this side, so a run is capped at BOTH ends
         * and mid-run spans are untouched (no new co-planar faces, no z-fight).
         *
         * TD5RE_R12_TERMCAP=0 restores the open ends for an A/B. */
        if (td5_env_flag_on("TD5RE_R12_TERMCAP")) {
            int ec;
            for (ec = 0; ec < 2; ec++) {
                /* ec 0 = the NEAR end (f=0, towards span si-1), 1 = the FAR end. */
                const int    nb = ec ? h->si + 1 : h->si - 1;
                const double cx = ec ? e[3] : e[0];
                const double cy = ec ? e[4] : e[1];
                const double cz = ec ? e[5] : e[2];
                const double ux = ec ? e[8] : e[6];
                const double uz = ec ? e[9] : e[7];
                if (tg_r12_pave_stands(h->nl, nb, s)) continue;
                /* inner-bottom, outer-bottom, outer-top, inner-top. */
                q[0]  = cx;             q[1]  = cy;                 q[2]  = cz;
                q[3]  = cx + ux * sw_s; q[4]  = cy;                 q[5]  = cz + uz * sw_s;
                q[6]  = cx + ux * sw_s; q[7]  = cy + TD5_TG_KERB_H; q[8]  = cz + uz * sw_s;
                q[9]  = cx;             q[10] = cy + TD5_TG_KERB_H; q[11] = cz;
                t[0] = 0.0; t[1] = 0.0;
                t[2] = u_w; t[3] = 0.0;
                t[4] = u_w; t[5] = u_k;
                t[6] = 0.0; t[7] = u_k;
                tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
            }
        }
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    /* [R14 BRANCH item 2b] Count SIDES, not quads. This was `n / 8` (slab +
     * kerb = 2 quads a side), which R12's end caps already inflated and the
     * outer face would inflate again -- so the inventory number stopped meaning
     * "span-sides paved" the moment a third quad kind appeared. Counting the
     * sides directly keeps the metric stable under any future face. */
    tg_acct_n(TG_ACCT_SIDEWALK, h->si, sides);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R9 CITY item 2] provenance for the pavement-uniqueness sweep */
        const size_t p0 = h->blk->len;
        const int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                         &seg_page, &seg_nq, 1);
        tg_pave_mark(p0, h->blk->len, TG_PVS_CITY, h->si);
        return r;
    }
}

/* FLAT VERGE BAND -- the out-of-town "sidewalk" (item 7). One quad per side
 * lying on the ground beside the road, on the same paving page as the city
 * pavement, with no kerb face and no rise: outside a town a made-up margin is
 * paint and gravel, not a slab, and a raised lip out there would only be
 * something to trip a car that runs wide.
 *
 * The lift is 16 raw, chosen the same way TD5_TG_CROSS_LIFT was: there is no
 * polygon-offset path, the ground skirt sits 70 raw BELOW road level, so
 * anything in between wins the depth test without reading as a step. */
int tg_city_emit_verge_band(const TG_FBHook *h, double bw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    int seg_page = TD5_TG_PAGE_SIDEWALK, seg_nq;
    int s, n = 0;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* [R8 CITY item 2] same width authority as the city slab: the band
         * narrows into a fork instead of ending on one span. */
        const double bw_s = tg_pavement_side_width(h->nl, h->si, sg, bw);
        const double u_w = bw_s / (double)TD5_TG_SPAN_LENGTH;
        if (!(bw_s > 0.0)) continue;
        tg_city_edge_frame(h->nl, h->si, sg, e);

        /* Same winding and the same isotropic UV as the city slab, so the two
         * tile identically where a biome changes mid-block. */
        q[0] = e[0];               q[1]  = e[1] + TD5_TG_VERGE_LIFT; q[2]  = e[2];
        q[3] = e[0] + e[6] * bw_s; q[4]  = e[1] + TD5_TG_VERGE_LIFT; q[5]  = e[2] + e[7] * bw_s;
        q[6] = e[3] + e[8] * bw_s; q[7]  = e[4] + TD5_TG_VERGE_LIFT; q[8]  = e[5] + e[9] * bw_s;
        q[9] = e[3];               q[10] = e[4] + TD5_TG_VERGE_LIFT; q[11] = e[5];
        t[0] = 0.0; t[1] = (double)h->si;
        t[2] = u_w; t[3] = (double)h->si;
        t[4] = u_w; t[5] = (double)h->si + 1.0;
        t[6] = 0.0; t[7] = (double)h->si + 1.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_SIDEWALK, h->si, n / 4);   /* out-of-town verge band */
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R9 CITY item 2] provenance for the pavement-uniqueness sweep */
        const size_t p0 = h->blk->len;
        const int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                         &seg_page, &seg_nq, 1);
        tg_pave_mark(p0, h->blk->len, TG_PVS_VERGE, h->si);
        return r;
    }
}

/* [R8 CITY item 2] Per-span, per-side DIAGNOSTIC of every gate that can drop the
 * raised pavement. Item 2 ("on span 143 the sidewalk stop spawning here") has the
 * same SYMPTOM as R7 CITY item 7 (biome-edge ragged end) and R7 BRANCH item 6
 * (pre-fork right-side gap), but span 143 is neither a biome edge nor an approach
 * span on the branch side, so the mechanism must be measured rather than
 * inherited. Dumps: paved / sidewalk width / fork-clear membership / the branch
 * pavement block / the XSTOP facade gate / carriageway reach vs plain road half
 * width -- i.e. every `continue` in tg_city_emit_sidewalk, per side, so the log
 * names WHICH gate fired. Read-only, gated by TD5RE_R8_CITY_DIAG=1 and windowed
 * around TD5RE_R8_CITY_DIAG_SPAN (+/- TD5RE_R8_CITY_DIAG_PAD) so race.log stays
 * legible. */
void tg_r8_city_sidewalk_diag(const TG_FBHook *h)
{
    int tgt, pad, s;
    if (!td5_env_flag_on("TD5RE_R8_CITY_DIAG")) return;
    tgt = td5_env_int("TD5RE_R8_CITY_DIAG_SPAN", 143, 0, 100000);
    pad = td5_env_int("TD5RE_R8_CITY_DIAG_PAD", 24, 0, 4000);
    if (h->si < tgt - pad || h->si > tgt + pad) return;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const int paved = tg_city_span_paved(h);
        const double sw =
            tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(h->si)]);
        const double sw_s  = tg_pavement_side_width(h->nl, h->si, sg, sw);
        const int built    = tg_facade_built(h->si, s);
        const int xstop    = td5_env_flag_on("TD5RE_AUTOTRACK_XSTOP") && !built;
        const int forkclr  = tg_span_in_fork_clear(h->si);
        const int emitted  = paved && sw_s > 0.0 && !xstop;
        TD5_LOG_I(LOG_TAG,
            "r8city-diag: si=%d side=%s paved=%d sw=%.0f use=%.0f forkclear=%d "
            "built=%d xstop=%d reach=%.0f half=%.0f park=%d -> %s",
            h->si, s ? "L" : "R", paved, sw, sw_s, forkclr, built, xstop,
            tg_carriageway_reach(h->nl, h->si, sg),
            tg_road_half_width(h->nl, h->si), tg_block_is_park(h->si, s),
            emitted ? "PAVED"
                    : (!paved ? "drop:not-city"
                       : !(sw_s > 0.0) ? "drop:branch-corridor"
                       : "drop:xstop-gap"));
    }
}

/* PEDESTRIAN RAILING along the KERB: one alpha-keyed plane per side, scenery is
 * submitted CULL_NONE so a single plane reads from both sides. "most of the
 * times" in the feedback, not always -- a hash gate drops roughly a third of
 * spans, which is what breaks the railing into runs that end at crossings and
 * side streets instead of ringing the whole city.
 *
 * SIDE (2026-08-27, "fences should be on the side near the road"). The railing
 * used to stand at 0.88 of the pavement width, i.e. hard against the building
 * line, which reads as a fence around each property rather than street
 * furniture. A pedestrian guard rail exists to keep people OFF the carriageway,
 * so it belongs at the kerb: TD5_TG_FENCE_KERB back from the kerb face, far
 * enough that a car clipping the kerb does not pass through it and the posts
 * still stand on the slab. */

/* [R9 RAILFIX] THE definition of "a pedestrian kerb railing stands on the (si,
 * sg) road edge". Forward-declared at the top of this file because the ROADSIDE
 * guardrail -- defined hundreds of lines earlier -- has to ask it in order to
 * yield the edge (item 8: an armco prism and this railing were standing on the
 * same city kerb, which is the "double guardrails" report at span 957).
 *
 * The gate list below IS tg_city_emit_fence's gate list, and the emitter now
 * calls this instead of keeping its own copy. That is deliberate: a hand-off
 * between two emitters is only safe while both agree exactly on where the
 * handed-off thing is, and two copies of a five-clause predicate in a 16k-line
 * file will drift. One definition, two callers.
 *
 * The caller-side gates (paved / TD5RE_AUTOTRACK_SIDEWALKS) are folded in here
 * too, so the guardrail does not have to reconstruct the call chain. */
int tg_rail_kerbfence_here(int si, double sg)
{
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_SIDEWALKS")) return 0;
    if (si <= 0) return 0;
    /* RING GATE, and it is the whole reason an edge could lose its ONLY rail.
     * The scenery loop that runs tg_emit_fb_city bails with `if (si >= ring)
     * continue` -- fork pads and appended branch corridors carry road only. The
     * GUARDRAIL loop has no such gate. So on a corridor span the gate list below
     * could say "a railing stands here", the roadside rail would stand down for
     * it, and the railing would never actually be emitted: zero rails on that
     * edge. MEASURED before this line existed: 30 edges on seed 99991 had a
     * roadside rail and lost it to a railing that never ran.
     *
     * tg_rail_deck_here carries the identical gate for the identical reason.
     * Both predicates answer "does that emitter ACTUALLY RUN on this edge", not
     * "would that emitter like this edge" -- a hand-off to a claim that is never
     * made is worse than the doubling it was meant to remove.
     *
     * TD5RE_R9_RAILFIX_NOGATE=1 REMOVES this gate on purpose, so the ZERO-RAIL
     * counter can be pointed at it and the cost of dropping it MEASURED rather
     * than argued. It is a regression probe, not a tuning knob. */
    if (!td5_env_flag_off("TD5RE_R9_RAILFIX_NOGATE")) {
        if (si >= ((s_ring_len > 0) ? s_ring_len : si + 1)) return 0;
        if (tg_span_in_tunnel(si)) return 0;  /* bore: fb loop routes elsewhere */
    }
    if (tg_span_in_bridge_run(si)) return 0;            /* no pavement on a deck */
    if (!(tg_city_sidewalk_w(&k_biomes[tg_scenery_biome_index(si)]) > 0.0))
        return 0;                                        /* not a facade biome   */
    if (tg_city_crossing_here(si)) return 0;              /* pedestrians cross    */
    /* [R13 RAIL item 5b] CONSEQUENCE 2. A frontage gap breaks the railing
     * because a gap is a side-street MOUTH -- but no street opens onto a bridge
     * ramp (consequence 3 removes it), so on a ramp there is no mouth to break
     * for and the run carries through to the deck. Measured: this and the
     * pavement below are what went 1,0,0,0,0,1 on the left at 1314-1319. */
    /* [R14 BRANCH item 2a] and CONSEQUENCE 2 again for a FORK region, where the
     * side street is suppressed by tg_r10_cross_gates. The railing and the slab
     * must break on the same spans or the railing floats -- see
     * tg_r14_fork_nostreet. */
    if (!tg_facade_built(si, sg > 0.0) && !tg_r13_approach_span(si) &&
        !tg_r14_fork_nostreet(si)) return 0;
    if (tg_biome_cell_index(si) != tg_biome_cell_index(si - 1)) return 0;
    if (tg_side_blocked(si, sg)) return 0;                /* fork corridor        */
    return 1;
}

int tg_city_emit_fence(const TG_FBHook *h, double sw)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    double e[10], q[12], t[8];
    /* One page per span across, so the upright pitch is span-independent. */
    const double u_n = 1.0;
    int seg_page = TD5_TG_PAGE_FENCE, seg_nq;
    int s, n = 0;
    /* [R3 item 7] The kerb railing should read as a CONTINUOUS street edge and
     * break ONLY where it has a reason to: a pedestrian crossing over the road
     * (both kerbs), this side opening onto a side street (a facade gap is a
     * street mouth in this generator's block model), or a biome-run boundary
     * where the city itself ends. The old code dropped ~1/4 of spans through an
     * independent per-side hash (`(fh >> 29) >= 6`), which read as a railing
     * full of random holes rather than a street edge -- the user's report. That
     * hash is gone; the breaks below are the only ones now.
     * [R9 RAILFIX] Those breaks now live in tg_rail_kerbfence_here, unchanged,
     * so the roadside guardrail can ask the same question. */

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        /* Kerb-side, clamped so a narrow pavement still puts it inboard of the
         * building line rather than off the far edge of the slab. */
        const double back = (sw * 0.35 < TD5_TG_FENCE_KERB)
                          ? sw * 0.35 : TD5_TG_FENCE_KERB;
        if (!tg_rail_kerbfence_here(h->si, sg)) continue;
        tg_rail_edge_note(TG_RAIL_KERBFENCE, h->si, sg);   /* [R9 RAILFIX] */
        tg_city_edge_frame(h->nl, h->si, sg, e);

        q[0] = e[0] + e[6] * back; q[1]  = e[1] + TD5_TG_KERB_H;
        q[2] = e[2] + e[7] * back;
        q[3] = e[3] + e[8] * back; q[4]  = e[4] + TD5_TG_KERB_H;
        q[5] = e[5] + e[9] * back;
        q[6] = e[3] + e[8] * back; q[7]  = e[4] + TD5_TG_KERB_H + TD5_TG_FENCE_H;
        q[8] = e[5] + e[9] * back;
        q[9] = e[0] + e[6] * back; q[10] = e[1] + TD5_TG_KERB_H + TD5_TG_FENCE_H;
        q[11] = e[2] + e[7] * back;
        /* v = 1 at the base, matching the page's top-down row order. */
        t[0] = 0.0; t[1] = 1.0;
        t[2] = u_n; t[3] = 1.0;
        t[4] = u_n; t[5] = 0.0;
        t[6] = 0.0; t[7] = 0.0;
        tg_city_push_quad(px, py, pz, uu, vv, &n, q, t);
    }

    if (n <= 0) return 1;
    if (*h->nmesh >= h->maxmesh) return 1;
    seg_nq = n / 4;
    tg_acct_n(TG_ACCT_FENCE, h->si, n / 4);      /* one railing per side */
    h->moff[(*h->nmesh)++] = h->blk->len;
    return tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                              &seg_page, &seg_nq, 1);
}

static int tg_r11_xcurve(void) { return td5_env_flag_on("TD5RE_R11_XCURVE"); }

/* The widened gap for the pair (a, b), IGNORING the knob. Split out from the
 * gate below so the report can ask "what would the widened rule have wanted
 * here" on a knob-OFF run -- that is what makes the before and the after two
 * readings of one formula instead of two models of it. */
static int tg_r11_xgap_wide(int a, int b)
{
    double bend = tg_turn_bend(b), f;
    if (a > 0 && tg_turn_bend(a) > bend) bend = tg_turn_bend(a);
    f = bend / TD5_TG_R8_TURN_SIN;
    if (f > 1.0) f = 1.0;
    if (f < 0.0) f = 0.0;
    return TD5_TG_XMIN_GAP
         + (int)((double)(TD5_TG_R11_XGAP_MAX - TD5_TG_XMIN_GAP) * f + 0.5);
}

static int tg_r11_xgap(int a, int b)
{
    return tg_r11_xcurve() ? tg_r11_xgap_wide(a, b) : TD5_TG_XMIN_GAP;
}

/* The crossing predicate WITHOUT the min-spacing thinning: the FIRST span of a
 * non-park side-street gap on either kerb, off forks and bridge approaches.
 * Both sides are tested, so a street opening on the left gets one too. */
static int tg_crossing_base_raw(int si);

int tg_crossing_base(int si)
{
    int r;
    if (s_xmemo_armed && si >= 0 && si < TD5_TG_MAX_SPANS) {
        if (s_xbase_memo[si] >= 0) return s_xbase_memo[si];
        r = tg_crossing_base_raw(si);
        s_xbase_memo[si] = (signed char)r;
        return r;
    }
    return tg_crossing_base_raw(si);
}

static int tg_crossing_base_raw(int si)
{
    int s;
    if (si <= 1) return 0;
    /* Over a fork the main carriageway is HALF width and shifted, so a
     * kerb-to-kerb quad would float across the gore. The pavements survive it
     * (the shifted half road's outer edge is still the full-width edge) but a
     * crossing spans both edges, so skip the whole fork region. */
    if (tg_branches_enabled() && tg_span_in_fork_clear(si)) return 0;
    /* [R6 CROSS item 15] no crossing right after (or before) a bridge run. */
    if (td5_env_flag_on("TD5RE_AUTOTRACK_XBRIDGE_GATE") &&
        tg_span_near_bridge(si, TD5_TG_XBRIDGE_CLEAR)) return 0;
    /* [R12 OVERPASS item 11c] "AVOID street intersections underneath a highway
     * overpass" -- a junction under the deck is unreadable.
     *
     * Deliberately the same SHAPE as the bridge-approach gate directly above,
     * and for the same reason: both say "this stretch of road is already doing
     * something the player has to read, do not stack a junction on top of it".
     * The band is TWO spans wider than the deck's own footprint
     * (tg_up_clear_span) on each side, because a zebra is painted at the FIRST
     * span of a side-street gap and its kerb-to-kerb quad plus the pavement
     * arms that turn down the street run for several spans after it -- gating
     * on the footprint alone would still put the mouth of the junction against
     * a pier. Asked here in tg_crossing_base rather than in
     * tg_city_crossing_here so the min-spacing thinning sees the same set of
     * candidate crossings the emitter does, and the kerb-fence break gate,
     * which reads the same predicate, opens in the same places.
     *
     * TD5RE_R12_UP_XGATE=0 restores junctions under the deck for an A/B. */
    if (tg_up_xclear_span(si)) return 0;
    /* [R13 RAIL item 5b] ...and not on a bridge RAMP, for the same reason the
     * side street is refused there (tg_emit_fb_city): a zebra is a place people
     * walk ACROSS, and the ramp has no pavement on the far side to walk to. It
     * is also the last hole in the ramp's barrier: R11 CROSS deliberately drops
     * the roadside rail at a zebra, so a crossing painted on a ramp re-opened
     * the gap this item is about. MEASURED on seed 777 -- span 834, cross=1,
     * both edges bare, the only 2 of 105 ramp edges still unrailed; 0 after. */
    if (tg_r13_approach_span(si)) return 0;
    for (s = 0; s < 2; s++) {
        /* [R11 CITY item 10] the corner must actually STAND -- a zebra painted
         * against a suppressed block is a crossing into nothing.
         * [R16 CITY item 1] but a ramp-retracted frontage still carries the
         * pavement and the street grid, so the crossing forms there too (the
         * pave-corner predicate == tg_r11_corner_stands when the R16 knob is
         * off). */
        if (!(!tg_facade_built(si, s) && tg_r16_pave_corner_stands(si - 1, s)))
            continue;
        /* [R4 CROSS item 4] A zebra marks a road you cross INTO A STREET. On
         * seed 99991 one gap in four is a PARK (a green lawn + hedge from the
         * kerb out, tg_block_is_park), and painting a pedestrian crossing in
         * front of it is exactly the "weird green texture on some crossings"
         * report: the crosswalk reads as leading into a bright-green wall. A
         * park frontage is not a through street, so it gets no crossing. The
         * side-street mouths that ARE through streets still do. */
        if (tg_block_is_park(si, s)) continue;
        return 1;
    }
    return 0;
}

/* Where a pedestrian crossing is actually painted: a base crossing span that
 * survives the min-spacing thinning. The thinning is a greedy left-to-right
 * keep/drop simulated over a bounded backward window, so the decision is a pure
 * function of si with no cross-call state (this is called from both the zebra
 * emitter and the kerb-fence break gate, possibly out of span order). */
/* [R14 GENPERF 2026-09-03] Per-build memo for the two crossing predicates.
 * tg_city_crossing_here rescans TD5_TG_XMIN_LOOK (48) earlier spans through
 * tg_crossing_base on EVERY call, and it is asked several times per span-side
 * by the kerb fence, the roadside rail, the crossing emitters and the
 * rail-edge report -- measured 0.6 s (rail/street-crosses) + most of the kerb
 * fence's 1.3 s on one build. Both answers depend only on tables that are
 * final once tg_emit_models' prepass has run (forks, bridge runs, the R12/R13
 * clear tables, the turn map, the facade block pattern), so they are cached
 * from that point: tg_xmemo_reset(0) at build start disarms the cache,
 * tg_xmemo_reset(1) after the prepass arms it. -1 = not yet computed. */
static int tg_city_crossing_here_raw(int si);

int tg_city_crossing_here(int si)
{
    int r;
    if (s_xmemo_armed && si >= 0 && si < TD5_TG_MAX_SPANS) {
        if (s_xhere_memo[si] >= 0) return s_xhere_memo[si];
        r = tg_city_crossing_here_raw(si);
        s_xhere_memo[si] = (signed char)r;
        return r;
    }
    return tg_city_crossing_here_raw(si);
}

static int tg_city_crossing_here_raw(int si)
{
    int j, last = -1000000;
    if (!tg_crossing_base(si)) return 0;
    /* DEFAULT ON; TD5RE_AUTOTRACK_XMIN=0 restores the un-thinned behaviour. */
    if (!td5_env_flag_on("TD5RE_AUTOTRACK_XMIN")) return 1;
    for (j = si - TD5_TG_XMIN_LOOK; j < si; j++) {
        if (j > 1 && tg_crossing_base(j) && j - last >= tg_r11_xgap(last, j))
            last = j;                 /* j is a KEPT crossing */
    }
    return si - last >= tg_r11_xgap(last, si);
}

/* [R11 CROSS item 16] THE INSTRUMENT the diagnosis was made with, and the same
 * one that measures the result: every crossing this build actually paints, with
 * the gap to its predecessor and the bend at both ends. Reflects whatever the
 * knob is set to, so the knob-off run IS the "before" count and the knob-on run
 * the "after" -- one report, two numbers, no second model of the rule.
 * TD5RE_R11_XCURVE_DIAG=1. */
void tg_r11_xcurve_report(int nspans)
{
    int si, prev = -1, kept = 0, tight = 0, tight_bend = 0;
    /* THE CONTROL. "14 of 19 tight pairs are on a bend" only means something
     * beside the same figure for the pairs that are NOT tight -- if the whole
     * track is bent then curvature discriminates nothing and this rule is a
     * second thinner stacked on a wrong theory. So both populations are
     * summarised, and so is the bend of every span that carries a crossing at
     * all. */
    int loose = 0, loose_bend = 0;
    double tight_sum = 0.0, loose_sum = 0.0, all_sum = 0.0;
    int all_n = 0, all_bend = 0;
    if (!td5_env_flag_off("TD5RE_R11_XCURVE_DIAG")) return;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    for (si = 0; si < nspans; si++) {
        int gap, wide;
        double pb, sb, mb;
        if (!tg_city_crossing_here(si)) continue;
        kept++;
        all_n++;
        all_sum += tg_turn_bend(si);
        if (tg_turn_bend(si) >= TD5_TG_R8_TURN_SIN) all_bend++;
        if (prev < 0) { prev = si; continue; }
        gap  = si - prev;
        wide = tg_r11_xgap_wide(prev, si);
        pb   = tg_turn_bend(prev);
        sb   = tg_turn_bend(si);
        mb   = (sb > pb) ? sb : pb;
        if (gap >= wide) {
            loose++;
            loose_sum += mb;
            if (mb >= TD5_TG_R8_TURN_SIN) loose_bend++;
        } else {
            tight_sum += mb;
        }
        if (gap < wide) {
            tight++;
            if (tg_turn_bend(si) >= TD5_TG_R8_TURN_SIN
                || tg_turn_bend(prev) >= TD5_TG_R8_TURN_SIN) tight_bend++;
            TD5_LOG_I(LOG_TAG, "trackgen: [R11 XCURVE] pair %4d->%4d gap=%2d "
                      "widened=%2d bend=%.3f/%.3f UNDER",
                      prev, si, gap, wide,
                      tg_turn_bend(prev), tg_turn_bend(si));
        }
        prev = si;
    }
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R11 XCURVE] crossings kept=%d, pairs under the "
              "widened gap=%d (of which at a sharp bend=%d) "
              "(knob TD5RE_R11_XCURVE=%s)",
              kept, tight, tight_bend, tg_r11_xcurve() ? "on" : "off");
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R11 XCURVE] control: UNDER pairs n=%d mean-bend=%.3f "
              "sharp=%d/%d | OK pairs n=%d mean-bend=%.3f sharp=%d/%d | all "
              "crossing spans n=%d mean-bend=%.3f sharp=%d",
              tight, tight ? tight_sum / (double)tight : 0.0, tight_bend, tight,
              loose, loose ? loose_sum / (double)loose : 0.0, loose_bend, loose,
              all_n, all_n ? all_sum / (double)all_n : 0.0, all_bend);
}

/* ZEBRA CROSSING: one flat quad lying on the road, kerb to kerb.
 *
 * Two conventions copied from tg_emit_road_quad, which this has to sit on top
 * of without fighting it: the corners come from tg_road_edge (so the crossing
 * follows the same curvature and camber as the asphalt under it) and u runs
 * 0..lanes across the road. That makes the page tile once per lane, and since
 * the page carries two bars, a 4-lane road gets 8 bars -- bars running ALONG
 * travel, which is how a crossing is actually painted.
 *
 * Z-FIGHTING: the renderer has no polygon-offset path, so the only lever is a
 * lift. TD5_TG_CROSS_LIFT is 20 raw (~0.08 wu) -- far less than the 70 the
 * ground skirt drops by, enough to win the depth test at the distances a
 * crossing is visible from, and too small to see as a step. */
int tg_city_emit_crossing(const TG_FBHook *h)
{
    double px[4], py[4], pz[4], uu[4], vv[4];
    double l0x, l0y, l0z, r0x, r0y, r0z;
    double l1x, l1y, l1z, r1x, r1y, r1z;
    const double f0 = 0.22, f1 = 0.62;    /* ~600 raw of crossing, in-span */
    const double L = (double)h->lanes;
    int seg_page = TD5_TG_PAGE_CROSSING, seg_nq = 1;
    int n = 0;

    /* [R5 CROSS item 12] the zebra reads correctly only on tarmac; skip it on
     * the pale gravel/cobble surfaces where it clashes (the side-street mouth
     * still draws, on the biome road page). */
    if (!tg_span_surface_is_tarmac(h->si)) return 1;

    tg_road_edge(h->nl, h->si, f0, 0.0, 1.0, &l0x, &l0y, &l0z, &r0x, &r0y, &r0z);
    tg_road_edge(h->nl, h->si, f1, 0.0, 1.0, &l1x, &l1y, &l1z, &r1x, &r1y, &r1z);

    px[n]=r0x; py[n]=r0y+TD5_TG_CROSS_LIFT; pz[n]=r0z; uu[n]=0.0; vv[n]=0.0; n++;
    px[n]=l0x; py[n]=l0y+TD5_TG_CROSS_LIFT; pz[n]=l0z; uu[n]=L;   vv[n]=0.0; n++;
    px[n]=l1x; py[n]=l1y+TD5_TG_CROSS_LIFT; pz[n]=l1z; uu[n]=L;   vv[n]=1.0; n++;
    px[n]=r1x; py[n]=r1y+TD5_TG_CROSS_LIFT; pz[n]=r1z; uu[n]=0.0; vv[n]=1.0; n++;

    if (*h->nmesh >= h->maxmesh) return 1;
    tg_acct(TG_ACCT_CROSSING, h->si);
    h->moff[(*h->nmesh)++] = h->blk->len;
    {   /* [R8 GUARD] A zebra is authored KERB TO KERB, so the on-road guard's
         * coverage test would drop it on sight. It is marked DECAL instead of
         * exempt: covering the road is licensed, but ONLY flush with the tarmac
         * (this quad is lifted TD5_TG_CROSS_LIFT = 20 raw). A crossing slab that
         * floats above the road is still rejected, which is R8 item 3 stated as
         * a rule rather than as a special case. */
        const size_t d0 = h->blk->len;
        int r = tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n,
                                   &seg_page, &seg_nq, 1);
        tg_guard_mark(d0, h->blk->len, TG_GK_DECAL, h->si);
        return r;
    }
}

int tg_city_emit_lamp(const TG_FBHook *h, double sw)
{
    const TG_Node *n = &h->nl->v[h->si];
    /* Post stands on the pavement a little back from the kerb face, so a car
     * clipping the kerb does not visually pass through it. */
    const double stand = (sw > 0.0) ? sw * 0.35 : 300.0;
    /* Biome-aware kerb height (r2-city item 7): outside a town the verge is
     * flat paint, not a slab, so the post stands on the ground rather than a
     * step that is not there. Supersedes the fixed TD5_TG_KERB_H this used. */
    const double base_y = n->y + tg_city_kerb_h(h->b);
    /* Where the lantern ends up, relative to the post: the arm's far end. */
    const double reach = TD5_TG_LAMP_W * (1.0 - TD5_TG_LAMP_POST_U);
    int s;

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double lx = n->tz * sg, lz = -n->tx * sg;   /* outward unit */
        const double post = n->width * 0.5 + stand;
        /* Quad spans the lateral axis: outer edge (u = 0) sits outboard of the
         * post by the page's own margin, inner edge (u = LAMP_U) hangs over the
         * road, so the head really is above the carriageway. */
        const double o = post + TD5_TG_LAMP_W * TD5_TG_LAMP_POST_U;
        const double in = o - TD5_TG_LAMP_W;
        double px[4], py[4], pz[4], uu[4], vv[4];
        int seg_page = TD5_TG_PAGE_LAMPPOST, seg_nq = 1;

        if (tg_side_blocked(h->si, sg)) continue;
        if (*h->nmesh + 2 > h->maxmesh) return 1;

        /* Quad loop: outer-bottom, inner-bottom, inner-top, outer-top. v = 1 at
         * the base, matching the page's top-down row order. */
        px[0] = n->x + lx * o; py[0] = base_y;                  pz[0] = n->z + lz * o;
        px[1] = n->x + lx * in; py[1] = base_y;                 pz[1] = n->z + lz * in;
        px[2] = n->x + lx * in; py[2] = base_y + TD5_TG_LAMP_H; pz[2] = n->z + lz * in;
        px[3] = n->x + lx * o; py[3] = base_y + TD5_TG_LAMP_H;  pz[3] = n->z + lz * o;
        uu[0] = 0.0;            vv[0] = 1.0;
        uu[1] = TD5_TG_LAMP_U;  vv[1] = 1.0;
        uu[2] = TD5_TG_LAMP_U;  vv[2] = 0.0;
        uu[3] = 0.0;            vv[3] = 0.0;

        h->moff[(*h->nmesh)++] = h->blk->len;
        if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, 4,
                                &seg_page, &seg_nq, 1))
            return 0;
        tg_acct(TG_ACCT_LAMP, h->si);
        /* GLOW at the lantern: tg_prop_one places PP_LAMP at y + its own y_off
         * (2500 == TD5_TG_LAMP_H) and measures `gap` from the road edge, so the
         * arm's reach comes off the stand to land the glow on the head. */
        if (!tg_prop_one(h->nl, h->si, PP_LAMP, sg,
                         stand - reach, h->blk, h->moff, h->nmesh))
            return 0;
    }
    return 1;
}

/* One BACKGROUND building written as its own mesh. `solid` closes it into a BOX
 * (front grid + roof + two side returns + a back sheet) instead of the old bare
 * front grid. The bare grid is a zero-thickness sheet: seen from anything but
 * dead-on -- which is most of a curving street and the whole fork gore -- it read
 * as a paper sliver, the "background buildings ... look wrong" (item 3) and part
 * of "buildings ... only a facade" (item 15). Scenery is submitted CULL_NONE, so
 * every face reads from both sides and the winding only has to be self-consistent.
 * `bx,by,bz` is the front-left base, `a*` the along-frontage vector to the
 * front-right base, `l*0`/`l*1` the OUTWARD lateral units at the two ends and
 * `depth` how far back the box goes. Returns 0 only on a buffer write failure; a
 * full mesh table is a silent no-op success, matching the back-row contract. */
int tg_bg_building_box(TG_Buf *blk, size_t *moff, int *nmesh, int maxmesh,
                              double bx, double by, double bz,
                              double ax, double ay, double az,
                              double lx0, double lz0, double lx1, double lz1,
                              double depth, double H, int cols, int rows,
                              int page, int solid, int si)
{
    double px[TD5_TG_FACADE_MAXQUAD * 4], py[TD5_TG_FACADE_MAXQUAD * 4];
    double pz[TD5_TG_FACADE_MAXQUAD * 4], uu[TD5_TG_FACADE_MAXQUAD * 4];
    double vv[TD5_TG_FACADE_MAXQUAD * 4];
    int seg_nq, n = 0;

    tg_facade_push_grid(bx, by, bz, ax, ay, az, 0.0, H, 0.0,
                        cols, rows, 0, rows, px, py, pz, uu, vv, &n);
    if (solid && depth > 1.0) {
        const double blx = bx + lx0 * depth, blz = bz + lz0 * depth;
        const double frx = bx + ax, fry = by + ay, frz = bz + az;
        const double brx = frx + lx1 * depth, brz = frz + lz1 * depth;
        double q[12];
        /* ROOF (matches the front-facade MASS roof winding). */
        q[0] = bx;  q[1] = by + H;       q[2] = bz;
        q[3] = frx; q[4] = fry + H;      q[5] = frz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = blx; q[10] = by + H;      q[11] = blz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* LEFT return. */
        q[0] = bx;  q[1] = by;           q[2] = bz;
        q[3] = blx; q[4] = by;           q[5] = blz;
        q[6] = blx; q[7] = by + H;       q[8] = blz;
        q[9] = bx;  q[10] = by + H;      q[11] = bz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* RIGHT return. */
        q[0] = frx; q[1] = fry;          q[2] = frz;
        q[3] = brx; q[4] = fry;          q[5] = brz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = frx; q[10] = fry + H;     q[11] = frz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
        /* BACK sheet. */
        q[0] = blx; q[1] = by;           q[2] = blz;
        q[3] = brx; q[4] = fry;          q[5] = brz;
        q[6] = brx; q[7] = fry + H;      q[8] = brz;
        q[9] = blx; q[10] = by + H;      q[11] = blz;
        tg_facade_push_quad(q, px, py, pz, uu, vv, &n);
    }
    if (n <= 0) return 1;
    if (*nmesh >= maxmesh) return 1;
    seg_nq = n / 4;
    moff[(*nmesh)++] = blk->len;
    if (!tg_write_quad_mesh(blk, px, py, pz, uu, vv, n, &page, &seg_nq, 1))
        return 0;
    tg_acct(TG_ACCT_BUILDING, si);
    return 1;
}

/* [R15 OCC] THE INSTRUMENT. TD5RE_R15_OCC_DIAG=<span> dumps a +/-8 window of
 * every input the three placement decisions read, per (span, side), so the
 * picker's world position can be turned into a decision trace instead of a
 * hypothesis. Added because the first cut of the fix reported nostreet=0 and
 * only 3 pushes on the reported seed -- i.e. it was not firing where the user
 * is looking, and the honest next step is to measure which input disagrees
 * rather than to widen the rule until the counter moves. Read-only, opt-in. */
static void tg_r15_occ_diag(const TG_FBHook *h, double sw)
{
    const int c = td5_env_int("TD5RE_R15_OCC_DIAG", -1, -1, 100000);
    int s;
    if (c < 0 || h->si < c - 8 || h->si > c + 8) return;
    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        double xr = 0.0;
        const int here  = tg_xstreet_here(h->nl, h->si, sg, &xr);
        const double hw = tg_road_half_width(h->nl, h->si);
        const double pw = tg_pavement_side_width(h->nl, h->si, sg,
                              tg_city_sidewalk_w_at(h->nl, h->si, h->b));
        TD5_LOG_W(LOG_TAG, "[R15 OCC DIAG] si=%4d %-5s node=(%.0f,%.0f,%.0f) "
                  "built=%d park=%d "
                  "blocked=%d xhere=%d xreach=%.0f half=%.0f pave=%.0f "
                  "occ(road)=%.0f occ(all)=%.0f xreach_at=%.0f sw=%.0f",
                  h->si, s ? "left" : "right",
                  h->nl->v[h->si].x, h->nl->v[h->si].y, h->nl->v[h->si].z,
                  tg_facade_built(h->si, s),
                  tg_block_is_park(h->si, s), tg_side_blocked(h->si, sg),
                  here, xr, hw, pw,
                  tg_occ_reach(h->nl, h->si, sg, TG_OCC_ROAD),
                  tg_occ_reach(h->nl, h->si, sg, TG_OCC_ALL),
                  tg_xstreet_reach_at(h->nl, h->si, sg,
                                      tg_block_arm_skew(h->si, s), h->b, sw),
                  sw);
    }
}


int tg_city_emit_backrows(const TG_FBHook *h, double sw)
{
    const TG_Biome *b = h->b;
    const TG_Node *n0 = &h->nl->v[h->si];
    const TG_Node *n1;
    int s, r;

    if (h->si + 1 >= h->nl->count) return 1;
    n1 = &h->nl->v[h->si + 1];
    tg_r15_occ_diag(h, sw);

    /* Default ON (gated to real streets); TD5RE_AUTOTRACK_BACKROW_STREETS=0
     * restores the old "a row behind every span, both sides" behaviour for an
     * A/B -- that is the state the round-3 report was taken against. */
    const int gate = td5_env_flag_on("TD5RE_AUTOTRACK_BACKROW_STREETS");

    for (s = 0; s < 2; s++) {
        const double sg = s ? 1.0 : -1.0;
        const double lx0 = n0->tz * sg, lz0 = -n0->tx * sg;
        const double lx1 = n1->tz * sg, lz1 = -n1->tx * sg;
        unsigned int blk, ph, gs, gl;
        int av, nrow;
        if (tg_side_blocked(h->si, sg)) continue;
        /* GATE to a street opening. A solid frontage hides whatever is behind
         * it, so a back row there only ever pokes above the front roofline --
         * the "rows behind rows" everywhere. tg_facade_built == 0 on this side is
         * the gap the cross-street asphalt (tg_city_emit_crossstreet) runs
         * through, and the ONLY place the block behind is actually visible. */
        if (gate && tg_facade_built(h->si, s)) continue;
        /* [R8 CITY item 12] "avoid using city background texture when in the
         * middle of a park." A gap is either a through STREET or a PARK, and the
         * back row exists to fill the view down a street mouth. The park lawn
         * (tg_block_emit_park) runs out to tg_city_crossst_reach -- sw + facade
         * depth + TWO BACKROW_GAPs since R7 CROSS item 2 -- while row 0 of this
         * emitter stands at sw + depth + ONE BACKROW_GAP, i.e. squarely INSIDE
         * the lawn, so a city block was planted in the middle of the green.
         * Confirmed by a matched-pose top-down A/B at span 115 on seed 99991
         * with TD5RE_AUTOTRACK_PARKS=1: a dark building box sits on the lawn
         * with backrows on and the lawn is clear with them off.
         *
         * This is a placement-validity rule of the same family as "no tree over
         * water" (R7 FLORA item 18) and "no green median in a tunnel": the
         * backdrop is not wrong, its CONTEXT is. So it is suppressed only where
         * the gap is a park -- every street mouth keeps its receding block, and
         * the frontage runs either side of the park are untouched. It also puts
         * this emitter on the SAME park predicate the cross-street carriageway
         * and the corner intersection already read, which is the invariant the
         * park code documents ("a park and a side street are mutually exclusive
         * on a given gap ... the single predicate both read").
         * TD5RE_R8_CITY_PARK_BACKDROP=0 restores the block-in-the-park for an
         * A/B. Parks are default OFF (TD5RE_AUTOTRACK_PARKS), so with stock
         * settings this branch is unreachable and nothing changes. */
        if (tg_block_is_park(h->si, s) &&
            td5_env_flag_on("TD5RE_R8_CITY_PARK_BACKDROP")) {
            tg_acct(TG_ACCT_R8_CITY, h->si);   /* backdrop refused: park gap */
            continue;
        }
        /* [R15 CITY item 4] "this building is next to no road at the end of the
         * crossing street, you must add a street and a sidewalk before showing
         * this building."
         *
         * A frontage GAP is necessary but not sufficient. This emitter's own
         * purpose, stated above, is that "the back row exists to fill the view
         * down a street mouth" -- and since R8 CROSS item 1 its setback is
         * literally rebased on tg_xstreet_reach_at, i.e. it is positioned as the
         * thing that TERMINATES a vista. But its gates are a strict subset of
         * the street's: tg_emit_fb_city refuses the carriageway on a bridge run,
         * an overpass clear span and a ramp approach, and the emitter itself
         * refuses on tg_span_near_bridge and tg_side_corridor_here -- none of
         * which stop the back row. On any such gap the reveal building stood at
         * the far end of a street that was never laid, terminating nothing.
         *
         * tg_xstreet_here is the authority that reads the crossstreet emitter's
         * OWN gates in its own order (see its block comment), so asking it is
         * asking the street itself rather than modelling it a second time.
         *
         * MEASURED (TD5RE_R15_OCC_DIAG=65 on the reported seed): at span 65 left
         * the street IS laid -- xhere=1, reaching 22800 from the centreline --
         * so refusing the building on "no street" fires ZERO times and was the
         * wrong reading of the report. The defect is the GAP, handled at the
         * setback below. This gate is kept for the genuine case (a frontage gap
         * the street emitter refused for a bridge run / clear span / ramp) where
         * it is still the right answer, but it is correctly rare. */
        if (td5_env_flag_on("TD5RE_R15_BACKROW_STREET")) {
            double xr15 = 0.0;
            if (!tg_xstreet_here(h->nl, h->si, sg, &xr15)) {
                s_r15_backrow_nostreet++;
                continue;
            }
        }
        /* Street WIDTH CLASS (item 2). An avenue opens both kerbs and carries
         * traffic, so it reveals a deeper, taller block receding; a narrow
         * pedestrian side street reveals a single closer, lower row. Read-only
         * use of the same partition the frontage and crossings key off, so all
         * three agree on which openings are avenues. */
        tg_facade_block(h->si, s, &blk, &ph, &gs, &gl, &av);
        nrow = (gate && !av) ? 1 : TD5_TG_BACKROW_N;
        for (r = 0; r < nrow; r++) {
            /* Row hash: per span, per side, per row -- so neighbouring spans
             * step in height and the two rows never line up. */
            const unsigned int rh = ((unsigned)h->si * 31u + (unsigned)s * 7u
                                     + (unsigned)r) * 2246822519u;
            double set, H, ax, az, ay, bx, by, bz, flen;
            int rows, cols, page;

            if ((rh >> 29) == 0u) continue;      /* ~12% of slots left empty */
            /* Each row sits a building depth plus clear air behind the last. */
            set = sw + tg_facade_depth(b) * (double)(r + 1)
                + TD5_TG_BACKROW_GAP * (double)(r + 1)
                + (double)(rh % 1800u);
            /* [R8 CROSS item 1] MASSING SETBACK. This row is emitted ONLY on a
             * span whose frontage is a street opening, i.e. exactly where
             * tg_city_emit_crossstreet lays a carriageway -- and the setback
             * above put it one building depth plus one clear gap out, which is
             * INSIDE that carriageway. The row therefore stood ACROSS the mouth
             * of the side street, right behind the flanking pavement arm: the
             * user's "buildings are spawning on the edge of the sidewalks near
             * the street". It also capped how long the street could ever LOOK,
             * which is why R7's reach increment did not read as longer.
             *
             * A reveal building belongs at the FAR END of the vista it reveals,
             * terminating the street the way a real block closes a view. Rebase
             * the setback on the street's own reach, walking outward one whole
             * block per row from there. What lines the street's FLANKS is
             * emitted separately (tg_cross_emit_street_flank), so the two halves
             * of item 1 -- how long the street is and what stands beside it --
             * are answered by the same model. Park gaps keep the old setback:
             * a park has no carriageway to stand clear of. */
            if (td5_env_flag_on("TD5RE_R8_CROSS_REACH") &&
                !tg_facade_built(h->si, s) && !tg_block_is_park(h->si, s))
                set = tg_xstreet_reach_at(h->nl, h->si, sg,
                                          tg_block_arm_skew(h->si, s), b, sw)
                    + TD5_TG_BACKROW_GAP
                    + (tg_facade_depth(b) + TD5_TG_BACKROW_GAP) * (double)r
                    + (double)(rh % 1800u);
            /* [R15 CITY item 4] "this building is next to no road at the end of
             * the crossing street, you must add a street and a sidewalk before
             * showing this building."
             *
             * MEASURED, and it is not the building being unwanted -- it is a
             * VOID in front of it. TD5RE_R15_OCC_DIAG=65 on the reported seed,
             * span 65 left: the street is laid to 22800 from the centreline
             * (xhere=1, xreach_at 19800 off a 3000 half-width), while row 0 sets
             * back xreach_at + BACKROW_GAP(3200) + rh%1800, i.e. it stands at
             * 26000..27800. So 3200 to 5000 units of bare ground sit between the
             * end of the tarmac and the block that is supposed to CLOSE it --
             * which is exactly "no road at the end of the crossing street".
             *
             * R8's own words for this row are "a reveal building belongs at the
             * FAR END of the vista it reveals, TERMINATING the street the way a
             * real block closes a view". A 3200+ gap does not terminate
             * anything. So row 0 is pulled in to sit one PAVEMENT width past the
             * tarmac -- which is both the smallest honest gap and, literally,
             * the sidewalk the report asks for in front of the building -- and
             * the 0..1800 jitter is dropped for that row only, since it is the
             * jitter that makes the void variable and can never help a row whose
             * job is to line up with the street's end. Rows 1+ keep the full
             * block-per-row walk and their jitter: they are depth behind the
             * terminating block, not the terminator. */
            if (r == 0 && td5_env_flag_on("TD5RE_R15_BACKROW_CLOSE") &&
                !tg_facade_built(h->si, s) && !tg_block_is_park(h->si, s)) {
                const double pw = tg_pavement_side_width(h->nl, h->si, sg, sw);
                const double term = tg_xstreet_reach_at(h->nl, h->si, sg,
                                        tg_block_arm_skew(h->si, s), b, sw)
                                  + (pw > 0.0 ? pw : sw);
                if (term < set) { set = term; s_r15_backrow_close++; }
            }
            /* [R15 CITY items 7 + 8a] "this building is on top of a street" /
             * "on top of a sidewalk".
             *
             * The R8 rebase above already aims this row past the street -- but
             * it re-derives the reach PRIVATELY, with this emitter's own `sw`
             * (the biome base width) where the street and the pavement use
             * tg_city_sidewalk_w_at and the per-side tg_pavement_side_width. The
             * two answers are equal only when neither narrowing applies, and
             * where they differ the row lands on the very surface it is meant to
             * stand behind. Nothing here consults the pavement at all, which is
             * item 8a: the raised slab is in no envelope in this generator.
             *
             * So take the FLOOR from the shared authority instead of trusting a
             * private derivation: whatever road, street or pavement actually
             * reaches at this (span, side), stand a clear BACKROW_GAP beyond it
             * and step out one block per row from there. tg_occ_reach measures
             * from the centreline and `set` from the kerb, hence the half-width
             * subtraction. Only ever pushes OUTWARD -- max(), never a move in --
             * so a row already clear of everything keeps its hashed position and
             * this cannot shuffle geometry it was not aimed at. */
            if (td5_env_flag_on("TD5RE_R15_BACKROW_OCC")) {
                const double occ = tg_occ_reach(h->nl, h->si, sg, TG_OCC_ALL);
                if (occ > 0.0) {
                    /* The floor is "not standing ON any of it", NOT "a clear
                     * BACKROW_GAP behind it". tg_occ_reach already carries the
                     * street's own TD5_TG_R10_XSTREET_MARGIN of clear air, and
                     * adding a second 3200 here would push row 0 back out past
                     * the terminating position item 4 just pulled it in to --
                     * the two rules would fight and the void would return. Rows
                     * 1+ still walk one block out each, which is their own
                     * spacing, not a clearance. */
                    const double floor_set = occ - n0->width * 0.5
                        + (tg_facade_depth(b) + TD5_TG_BACKROW_GAP) * (double)r;
                    if (floor_set > set) {
                        if (r == 0) s_r15_backrow_push++;
                        set = floor_set;
                    }
                }
            }
            rows = b->floors_min + (int)((rh >> 9) % 5u) + ((gate && av) ? 1 : 0);
            /* [R11 BIOME item 4] Third axis of the outskirts ramp: the massing
             * BEHIND the street. Ramping only the frontage would leave a full
             * town standing one block back, visible straight down every side
             * street, so the depth of the town has to thin with its face. Same
             * ramp value the front wall used at this span, so the two cannot
             * disagree about how built-up this stretch is. */
            {
                const double tr = tg_town_ramp(h->si);
                if (tr < 1.0) {
                    int capped = 1 + (int)((double)(rows - 1) * tr + 0.5);
                    if (capped >= 1 && capped < rows) rows = capped;
                }
            }
            H    = (double)rows * tg_facade_floor_h(b);

            bx = n0->x + lx0 * (n0->width * 0.5 + set);
            bz = n0->z + lz0 * (n0->width * 0.5 + set);
            by = tg_world_h(bx, bz);               /* [TOPOLOGY-FIRST] */
            if (by > n0->y + 400.0) by = n0->y + 400.0;
            ax = (n1->x + lx1 * (n1->width * 0.5 + set)) - bx;
            ay = n1->y - n0->y;
            az = (n1->z + lz1 * (n1->width * 0.5 + set)) - bz;

            /* Columns from the row's OWN frontage length, which at a big setback
             * on a curve is appreciably longer than a span: the fixed 2 columns
             * this used squashed the page to 750 raw across, a third of its
             * authored 2150, and the back rows read as a different (much finer)
             * building than the front row of the same block. */
            flen = sqrt(ax * ax + az * az);
            cols = tg_facade_cols_for(flen, (double)b->cell_w, 4);
            page = tg_facade_page_class(rh, rows);
            /* Solid box (item 3): the old flat grid read as a paper sliver from
             * any oblique angle. Default ON; TD5RE_AUTOTRACK_BACKROW_SOLID=0
             * restores the flat sheet for an A/B. */
            if (!tg_bg_building_box(h->blk, h->moff, h->nmesh, h->maxmesh,
                                    bx, by, bz, ax, ay, az, lx0, lz0, lx1, lz1,
                                    tg_facade_depth(b), H, cols, rows, page,
                                    td5_env_flag_on("TD5RE_AUTOTRACK_BACKROW_SOLID"),
                                    h->si))
                return 0;
        }
    }
    return 1;
}

/* Both of this area's deliverables share ONE reserved accounting slot, so the
 * element inventory alone cannot say which of them fired. These counters carry
 * the split, logged as their own line at the end of the build -- an explicit
 * number in race.log rather than a second area's enum slot borrowed. Reset per
 * build, alongside s_acct_count. */
long s_r9_infra_props;      /* furniture pieces emitted */

long s_r9_infra_ponds;      /* pond sheets emitted      */

/* [R12 PROPS] The two deliverables of this round, both of which change a
 * SHARE of the same total rather than the total itself -- so the inventory
 * count and even s_r9_infra_props are blind to them. These are the numbers the
 * round is judged on: benches built as a bench (item 2) and discs the sign
 * context/rate gate turned away (item 3), logged beside the R9 split. */
long s_r12_bench_form;      /* benches emitted in the new bench form  */

long s_r12_sign_kept;       /* sign picks that survived both gates     */

long s_r12_sign_ctx;        /* dropped: no road-network reason here    */

long s_r12_sign_rate;       /* dropped: allowed biome, thinned by rate */

/* [R13 PROPS] Same shape as the R12 pair above: every one of this round's items
 * changes a SHARE of the furniture total, never the total, so s_r9_infra_props
 * and the element inventory are blind to all of them. These are the numbers. */
long s_r13_bench_end_uv;    /* benches whose FAR end took the fixed u  */

long s_r13_plank_crop;      /* barrier planks built from the sub-rect  */

long s_r13_awn_form;        /* awnings emitted as a wall-hung awning   */

long s_r13_awn_kept;        /* awning picks with a frontage to hang on */

long s_r13_awn_ctx;         /* dropped: nothing to hang the awning off */

/* TD5RE_R9_INFRA_REPORT=1 names every piece as it is placed. The element
 * inventory proves the EMITTER fired; this says WHICH piece stands WHERE, which
 * is what lets a frame be aimed at a real placement instead of a guessed span
 * (a frame of an empty verge proves nothing about a class). Capped only so a
 * pathological track cannot fill the log; the cap sits above any real count. */
const char *const k_infra_names[IP_COUNT] = {
    "bin", "crate", "crate-fragile", "cardboard-box", "phone-box",
    "bench", "awning", "roadworks", "barrier-plank", "road-sign", "rickshaw"
};

int s_r9_infra_reported;

const TG_InfraProp k_infra_props[IP_COUNT] = {
    /* bin: 0.55 m across, 1.05 m tall, lid page on top */
    { TG_INFRA_BINBODY, TG_INFRA_BINBODY, TG_INFRA_BINLID,
      0.55 * TD5_TG_INFRA_M, 1.05 * TD5_TG_INFRA_M, 0.55 * TD5_TG_INFRA_M, 0.0 },
    /* crate: 0.9 m cube */
    { TG_INFRA_CRATE, TG_INFRA_CRATE, TG_INFRA_CRATE,
      0.90 * TD5_TG_INFRA_M, 0.90 * TD5_TG_INFRA_M, 0.90 * TD5_TG_INFRA_M, 0.0 },
    /* stencilled crate, same box, different face */
    { TG_INFRA_CRATEFRG, TG_INFRA_CRATEFRG, TG_INFRA_CRATE,
      0.85 * TD5_TG_INFRA_M, 0.85 * TD5_TG_INFRA_M, 0.85 * TD5_TG_INFRA_M, 0.0 },
    /* cardboard box: smaller, stacked next to the crates */
    { TG_INFRA_CARDBOX, TG_INFRA_CARDBOX, TG_INFRA_CARDBOX,
      0.60 * TD5_TG_INFRA_M, 0.60 * TD5_TG_INFRA_M, 0.60 * TD5_TG_INFRA_M, 0.0 },
    /* phone box: 0.9 x 0.9 footprint, 2.4 m tall */
    { TG_INFRA_PHONE, TG_INFRA_PHONE, TG_INFRA_PHONE,
      0.90 * TD5_TG_INFRA_M, 2.40 * TD5_TG_INFRA_M, 0.90 * TD5_TG_INFRA_M, 0.0 },
    /* bench: ornate ends, slatted seat, 1.7 m along the pavement */
    { TG_INFRA_BENCHEND, TG_INFRA_BENCH, TG_INFRA_BENCH,
      0.50 * TD5_TG_INFRA_M, 0.85 * TD5_TG_INFRA_M, 1.70 * TD5_TG_INFRA_M, 0.0 },
    /* shop awning: hangs 2.2 m up, 2.4 m along the frontage */
    { TG_INFRA_CANOPY, TG_INFRA_CANOPY, TG_INFRA_CANOPY,
      1.10 * TD5_TG_INFRA_M, 0.45 * TD5_TG_INFRA_M, 2.40 * TD5_TG_INFRA_M,
      2.20 * TD5_TG_INFRA_M },
    /* roadworks barrier: banded, 2.6 m along, waist high */
    { TG_INFRA_WORKY, TG_INFRA_WORKY, TG_INFRA_WORKY,
      0.30 * TD5_TG_INFRA_M, 1.00 * TD5_TG_INFRA_M, 2.60 * TD5_TG_INFRA_M, 0.0 },
    /* barrier plank: flat, so a billboard is the honest form */
    { -1, TG_INFRA_REDTAPE, -1,
      2.60 * TD5_TG_INFRA_M, 1.10 * TD5_TG_INFRA_M, 0.0, 0.0 },
    /* road sign: flat disc on a post we do not model, so it is lifted */
    { -1, TG_INFRA_SIGN, -1,
      0.80 * TD5_TG_INFRA_M, 0.80 * TD5_TG_INFRA_M, 0.0, 1.90 * TD5_TG_INFRA_M },
    /* rickshaw: a cutout of a shafted cart, ORIENTAL only */
    { -1, TG_INFRA_RICKSHAW, -1,
      2.10 * TD5_TG_INFRA_M, 1.80 * TD5_TG_INFRA_M, 0.0, 0.0 }
};

/* One furniture BOX: four upright faces plus a lid, written as a single mesh
 * with three texture segments in vertex order (2 end quads, 2 side quads, 1
 * top). Scenery is submitted CULL_NONE, so the winding only has to be
 * self-consistent. `ax/az` is the along-road unit and `lx/lz` the outward
 * lateral one, so a box on a curve sits square to the kerb it stands on. */
static int tg_infra_box(const TG_FBHook *h, const TG_InfraProp *P,
                        double cx, double base_y, double cz,
                        double ax, double az, double lx, double lz)
{
    double px[20], py[20], pz[20], uu[20], vv[20];
    const double hw = P->w * 0.5, hd = P->d * 0.5;
    const double y0 = base_y + P->lift, y1 = y0 + P->hgt;
    int seg_page[3], seg_nq[3], n = 0, e, s;

    /* Corner offsets in (lateral, along) order: 0 = -w -d, 1 = +w -d,
     * 2 = +w +d, 3 = -w +d, walked anticlockwise seen from above. */
    static const double cl[4] = { -1.0,  1.0, 1.0, -1.0 };
    static const double cd[4] = { -1.0, -1.0, 1.0,  1.0 };
    double kx[4], kz[4];

    for (e = 0; e < 4; e++) {
        kx[e] = cx + lx * (cl[e] * hw) + ax * (cd[e] * hd);
        kz[e] = cz + lz * (cl[e] * hw) + az * (cd[e] * hd);
    }
    /* Faces in segment order: the two ENDS (spanning the lateral axis) first,
     * then the two long SIDES, then the lid. Edge e joins corner e to e+1, so
     * edges 0 and 2 are the ends and 1 and 3 the sides. */
    for (s = 0; s < 2; s++) {
        for (e = s; e < 4; e += 2) {
            const int f = (e + 1) & 3;
            px[n] = kx[e]; py[n] = y0; pz[n] = kz[e]; uu[n] = 0.0; vv[n] = 1.0; n++;
            px[n] = kx[f]; py[n] = y0; pz[n] = kz[f]; uu[n] = 1.0; vv[n] = 1.0; n++;
            px[n] = kx[f]; py[n] = y1; pz[n] = kz[f]; uu[n] = 1.0; vv[n] = 0.0; n++;
            px[n] = kx[e]; py[n] = y1; pz[n] = kz[e]; uu[n] = 0.0; vv[n] = 0.0; n++;
        }
    }
    for (e = 0; e < 4; e++) {
        static const double tu[4] = { 0.0, 1.0, 1.0, 0.0 };
        static const double tv[4] = { 0.0, 0.0, 1.0, 1.0 };
        px[n] = kx[e]; py[n] = y1; pz[n] = kz[e];
        uu[n] = tu[e]; vv[n] = tv[e]; n++;
    }
    seg_page[0] = TD5_TG_INFRA_PAGE(P->page_end);  seg_nq[0] = 2;
    seg_page[1] = TD5_TG_INFRA_PAGE(P->page_side); seg_nq[1] = 2;
    seg_page[2] = TD5_TG_INFRA_PAGE(P->page_top);  seg_nq[2] = 1;

    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 3))
        return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_R9_INFRA, h->si);
    s_r9_infra_props++;
    return 1;
}

static int tg_infra_bench(const TG_FBHook *h, const TG_InfraProp *P,
                          double cx, double base_y, double cz,
                          double ax, double az, double lx, double lz)
{
    double px[16], py[16], pz[16], uu[16], vv[16];
    const double hw = P->w * 0.5, hd = P->d * 0.5;
    const double y0 = base_y + P->lift, y1 = y0 + P->hgt;
    const double ys = y0 + P->hgt * TD5_TG_BENCH_SEAT;
    int seg_page[2], seg_nq[2], n = 0, e;
    static const double cl[4] = { -1.0,  1.0, 1.0, -1.0 };
    static const double cd[4] = { -1.0, -1.0, 1.0,  1.0 };
    double kx[4], kz[4];

    for (e = 0; e < 4; e++) {
        kx[e] = cx + lx * (cl[e] * hw) + ax * (cd[e] * hd);
        kz[e] = cz + lz * (cl[e] * hw) + az * (cd[e] * hd);
    }
    /* Ends (segment 0): edges 0 and 2, each spanning the lateral axis at one end
     * of the length.
     *
     * [R13 PROPS item 2] "on the OUTER side of the bench the texture is
     * MIRRORED". R12 kept tg_infra_box's mapping byte-for-byte, and that mapping
     * is a WRAP: the corner walk is anticlockwise, so every edge runs u 0->1
     * around the box and the two ends therefore point their u axes at OPPOSITE
     * lateral directions. On a closed box that is correct -- the page wraps and
     * meets itself. On a bench it is not, because the two ends are not two faces
     * of one wrapped skin, they are two copies of ONE asymmetric cutout.
     *
     * Measured, not assumed: td6_bench.png q2 (TG_INFRA_BENCHEND) decodes to
     * 1011 opaque texels of ornate cast iron whose mirror agreement about the
     * page centre is 75%, with the tall backrest post packed against the u=1
     * edge (opaque x range 1..63, right half 540 texels vs left 471). So u=1 is
     * the BACK of the profile, and the back of the bench is the OUTBOARD (+w)
     * lateral -- that is where the backrest quad below stands.
     *
     * The wrap gave u=1 at +w on edge 0 and u=1 at -w on edge 2, so exactly ONE
     * of the two ends had its cast-iron back turned to the road: the outer end
     * read as the mirror of the inner one. u is now pinned to the LATERAL, not
     * to the walk, so both ends carry the same profile the same way round.
     * TD5RE_R13_BENCH_UV=0 restores the wrap for an A/B. */
    for (e = 0; e < 4; e += 2) {
        const int f = (e + 1) & 3;
        /* Corner 1 and 2 are the +w (outboard) pair, so on edge 2 the endpoints
         * arrive in the other lateral order and u has to be swapped to match. */
        const double ue = (e == 2 && td5_env_flag_on("TD5RE_R13_BENCH_UV"))
                          ? 1.0 : 0.0;
        const double uf = 1.0 - ue;
        if (ue > 0.0) s_r13_bench_end_uv++;
        px[n] = kx[e]; py[n] = y0; pz[n] = kz[e]; uu[n] = ue; vv[n] = 1.0; n++;
        px[n] = kx[f]; py[n] = y0; pz[n] = kz[f]; uu[n] = uf; vv[n] = 1.0; n++;
        px[n] = kx[f]; py[n] = y1; pz[n] = kz[f]; uu[n] = uf; vv[n] = 0.0; n++;
        px[n] = kx[e]; py[n] = y1; pz[n] = kz[e]; uu[n] = ue; vv[n] = 0.0; n++;
    }
    /* SEAT (segment 1, quad 1): horizontal at ys, corners walked along the
     * length first so u is the 1.70 m axis and the slats run down the bench. */
    px[n] = kx[0]; py[n] = ys; pz[n] = kz[0]; uu[n] = 0.0; vv[n] = 0.0; n++;
    px[n] = kx[3]; py[n] = ys; pz[n] = kz[3]; uu[n] = 1.0; vv[n] = 0.0; n++;
    px[n] = kx[2]; py[n] = ys; pz[n] = kz[2]; uu[n] = 1.0; vv[n] = 1.0; n++;
    px[n] = kx[1]; py[n] = ys; pz[n] = kz[1]; uu[n] = 0.0; vv[n] = 1.0; n++;
    /* BACKREST (segment 1, quad 2): upright from the seat to the top on the
     * OUTBOARD lateral edge (corners 1 and 2 are at +w, and lx/lz points away
     * from the road), so the bench looks at the carriageway. u along again. */
    px[n] = kx[1]; py[n] = ys; pz[n] = kz[1]; uu[n] = 0.0; vv[n] = 1.0; n++;
    px[n] = kx[2]; py[n] = ys; pz[n] = kz[2]; uu[n] = 1.0; vv[n] = 1.0; n++;
    px[n] = kx[2]; py[n] = y1; pz[n] = kz[2]; uu[n] = 1.0; vv[n] = 0.0; n++;
    px[n] = kx[1]; py[n] = y1; pz[n] = kz[1]; uu[n] = 0.0; vv[n] = 0.0; n++;

    seg_page[0] = TD5_TG_INFRA_PAGE(P->page_end);  seg_nq[0] = 2;
    seg_page[1] = TD5_TG_INFRA_PAGE(P->page_side); seg_nq[1] = 2;

    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, seg_page, seg_nq, 2))
        return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_R9_INFRA, h->si);
    s_r9_infra_props++;
    s_r12_bench_form++;
    return 1;
}

static int tg_infra_plank(const TG_FBHook *h, const TG_InfraProp *P,
                          double cx, double base_y, double cz,
                          double ax, double az, double lx, double lz)
{
    double px[12], py[12], pz[12], uu[12], vv[12];
    /* The 2.60 m width lies ALONG the road; the piece has no lateral depth. */
    const double ha = P->w * 0.5;
    const double y0 = base_y + P->lift, y1 = y0 + P->hgt;
    /* Plank height from the sub-rect's own aspect: 13 texels tall over 58 wide
     * at 2.60 m gives 0.58 m, which is a real barrier board. It hangs from the
     * top of the piece so the posts show below it. */
    const double plank_h = P->w * (TD5_TG_PLANK_V1 - TD5_TG_PLANK_V0)
                                / (TD5_TG_PLANK_U1 - TD5_TG_PLANK_U0);
    /* Post width the same way: 7 texels over 29 tall at the full piece height. */
    const double post_w  = P->hgt * (TD5_TG_POST_U1 - TD5_TG_POST_U0)
                                  / (TD5_TG_POST_V1 - TD5_TG_POST_V0);
    int seg_page = TD5_TG_INFRA_PAGE(P->page_side), seg_nq = 3, n = 0, q;
    double yp = y1 - plank_h;

    if (yp < y0) yp = y0;
    /* PLANK, then the two POSTS, all one page so the mesh stays one command. */
    for (q = 0; q < 3; q++) {
        /* q0 = the plank across the whole length; q1/q2 = a post at each end,
         * inset by its own half width so it sits under the plank, not past it. */
        const double c  = (q == 0) ? 0.0 : (q == 1 ? -(ha - post_w * 0.5)
                                                   :  (ha - post_w * 0.5));
        const double hh = (q == 0) ? ha : post_w * 0.5;
        const double ly0 = (q == 0) ? yp : y0;
        const double ly1 = (q == 0) ? y1 : y1;
        const double u0 = (q == 0) ? TD5_TG_PLANK_U0 : TD5_TG_POST_U0;
        const double u1 = (q == 0) ? TD5_TG_PLANK_U1 : TD5_TG_POST_U1;
        const double v0 = (q == 0) ? TD5_TG_PLANK_V0 : TD5_TG_POST_V0;
        const double v1 = (q == 0) ? TD5_TG_PLANK_V1 : TD5_TG_POST_V1;
        const double x0 = cx + ax * (c - hh), z0 = cz + az * (c - hh);
        const double x1 = cx + ax * (c + hh), z1 = cz + az * (c + hh);
        px[n] = x0; py[n] = ly0; pz[n] = z0; uu[n] = u0; vv[n] = v1; n++;
        px[n] = x1; py[n] = ly0; pz[n] = z1; uu[n] = u1; vv[n] = v1; n++;
        px[n] = x1; py[n] = ly1; pz[n] = z1; uu[n] = u1; vv[n] = v0; n++;
        px[n] = x0; py[n] = ly1; pz[n] = z0; uu[n] = u0; vv[n] = v0; n++;
    }
    (void)lx; (void)lz;

    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
        return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_R9_INFRA, h->si);
    s_r9_infra_props++;
    s_r13_plank_crop++;
    return 1;
}

static int tg_infra_awning(const TG_FBHook *h, const TG_InfraProp *P,
                           double cx, double base_y, double cz,
                           double ax, double az, double lx, double lz)
{
    double px[8], py[8], pz[8], uu[8], vv[8];
    const double hw = P->w * 0.5, hd = P->d * 0.5;
    /* lx/lz points AWAY from the road, so +w is the wall and -w the free edge. */
    const double y_wall = base_y + P->lift + P->hgt;
    const double y_free = y_wall - TD5_TG_AWN_FALL;
    const double y_hem  = y_free - TD5_TG_AWN_VALANCE;
    int seg_page = TD5_TG_INFRA_PAGE(P->page_side), seg_nq = 2, n = 0;
    /* Four ground-plan corners: (lateral, along) = wall/free x near/far. */
    const double wx0 = cx + lx * hw - ax * hd, wz0 = cz + lz * hw - az * hd;
    const double wx1 = cx + lx * hw + ax * hd, wz1 = cz + lz * hw + az * hd;
    const double fx0 = cx - lx * hw - ax * hd, fz0 = cz - lz * hw - az * hd;
    const double fx1 = cx - lx * hw + ax * hd, fz1 = cz - lz * hw + az * hd;

    /* SHEET: sloping, u along the frontage, v across from the wall (0) to the
     * free edge, stopping inside the fabric band so no fringe lands on top. */
    px[n] = wx0; py[n] = y_wall; pz[n] = wz0; uu[n] = 0.0; vv[n] = 0.0; n++;
    px[n] = wx1; py[n] = y_wall; pz[n] = wz1; uu[n] = 1.0; vv[n] = 0.0; n++;
    px[n] = fx1; py[n] = y_free; pz[n] = fz1; uu[n] = 1.0;
    vv[n] = TD5_TG_AWN_FABRIC_V; n++;
    px[n] = fx0; py[n] = y_free; pz[n] = fz0; uu[n] = 0.0;
    vv[n] = TD5_TG_AWN_FABRIC_V; n++;
    /* VALANCE: hangs at the free edge, whole page top to fringe. */
    px[n] = fx0; py[n] = y_hem;  pz[n] = fz0; uu[n] = 0.0; vv[n] = 1.0; n++;
    px[n] = fx1; py[n] = y_hem;  pz[n] = fz1; uu[n] = 1.0; vv[n] = 1.0; n++;
    px[n] = fx1; py[n] = y_free; pz[n] = fz1; uu[n] = 1.0; vv[n] = 0.0; n++;
    px[n] = fx0; py[n] = y_free; pz[n] = fz0; uu[n] = 0.0; vv[n] = 0.0; n++;

    h->moff[*h->nmesh] = h->blk->len;
    if (!tg_write_quad_mesh(h->blk, px, py, pz, uu, vv, n, &seg_page, &seg_nq, 1))
        return 0;
    (*h->nmesh)++;
    tg_acct(TG_ACCT_R9_INFRA, h->si);
    s_r9_infra_props++;
    s_r13_awn_form++;
    return 1;
}

/* One furniture piece, box or billboard, at `gap` out from the road edge. */
int tg_infra_place(const TG_FBHook *h, int kind, double side,
                          double gap, double base_y)
{
    const TG_InfraProp *P = &k_infra_props[kind];
    const TG_NodeList *nl = h->nl;
    const TG_Node *n = &nl->v[h->si];
    const double lx = n->tz * side, lz = -n->tx * side;
    const double ax = n->tx, az = n->tz;
    double out, cx, cz;

    if (tg_side_blocked(h->si, side)) return 1;
    if (*h->nmesh + 1 >= h->maxmesh)  return 1;   /* budget, not an error */

    /* [R10 SPAN66] ENFORCEMENT EVIDENCE KNOB, dev only.
     * TD5RE_R10_PROP_FORCE=<span> plants THAT span's piece on the MAIN
     * CENTRELINE, so the guard's verdict on furniture is something the log can be
     * made to SHOW rather than something this file asserts. It bypasses the
     * placement fix on purpose: the half under test is the backstop. Scoped to
     * one span because forcing every piece into the road makes the guard reject
     * ~500 meshes and the build stops being a comparable A/B. */
    if (h->si > 0 &&
        td5_env_int("TD5RE_R10_PROP_FORCE", 0, 0, 100000) == h->si) {
        TD5_LOG_W(LOG_TAG, "trackgen: [R10 SPAN66] FORCING %s onto the "
                  "centreline at span %d (enforcement test)",
                  k_infra_names[kind], h->si);
        return tg_infra_box(h, P, n->x, base_y, n->z, ax, az, lx, lz);
    }

    /* [R10 SPAN66 item 1] Refuse the piece where the pavement it is aiming for
     * is actually a side street's asphalt. There is no footway at a crossing
     * mouth to move it onto, so nothing is placed rather than something being
     * shoved to the far end of the street. */
    if (tg_xstreet_occupies(nl, h->si, side, gap - P->w * 0.5)) {
        tg_xstreet_audit(nl, h->si, side, gap - P->w * 0.5,
                         k_infra_names[kind], 0);
        s_r10_prop_skipped++;
        return 1;
    }
    tg_xstreet_audit(nl, h->si, side, gap - P->w * 0.5, k_infra_names[kind], 1);

    /* [R14 FCROSS item 1c] The CRATE in the span-88 frame is this emitter: a
     * verge setback of TD5_TG_VERGE_W + 200 + 0..511 (~1900 raw at most) against
     * a forest lane reaching 13000+, so the piece stands squarely on the new
     * tarmac. tg_guard_validate_entry does NOT catch it -- furniture is judged
     * against tg_footway_reach, and until this round that reach knew about city
     * side streets only. Both halves are fixed: refused HERE at the placement,
     * and tg_footway_reach widened so the backstop would have caught it too. */
    if (tg_r14_fcross_occupies(nl, h->si, side, gap - P->w * 0.5)) {
        s_r14_fcross_prop_skip++;
        return 1;
    }

    /* Push the setback out past any branch carriageway bowing into this
     * lateral, exactly as the pavement and the trees do. The half-width of the
     * piece is added so the FOOTPRINT clears, not just its centre. */
    out = tg_carriageway_clear_gap(nl, h->si, side, gap + P->w * 0.5,
                                   TD5_TG_CARRIAGEWAY_MARGIN);
    cx = n->x + lx * (n->width * 0.5 + out);
    cz = n->z + lz * (n->width * 0.5 + out);

    if (P->page_top < 0) {
        size_t b0;
        /* [R13 PROPS item 4a] The plank is the one billboard on this menu whose
         * page holds a sub-rectangle rather than a whole piece. */
        if (kind == IP_REDTAPE && td5_env_flag_on("TD5RE_R13_PLANK_CROP"))
            return tg_infra_plank(h, P, cx, base_y, cz, ax, az, lx, lz);
        /* [R15 PROPS item 1] "this stop sign should be placed alongside a stick
         * that holds it."
         *
         * The table row for IP_SIGN says so itself: "flat disc on a post we do
         * not model, so it is lifted". A 0.80 m disc floating 1.90 m up with
         * nothing under it is the whole complaint. Model the post, using the
         * SAME two-crossed-quads form and page tg_emit_r11_sign already uses --
         * at 6 cm the silhouette is all a driver resolves, and a cross is half
         * the geometry of a box for an identical read. Only the lifted disc
         * needs one; every other billboard on this menu (plank, rickshaw) sits
         * on the ground with lift 0. */
        if (kind == IP_SIGN && P->lift > 1.0 &&
            td5_env_flag_on("TD5RE_R15_SIGN_POST")) {
            if (*h->nmesh + 2 >= h->maxmesh) return 1;   /* budget for both */
            {
                double ppx[8], ppy[8], ppz[8], puu[8], pvv[8];
                const double hw = TD5_TG_R11_SIGN_POST_W * 0.5;
                const double ptop = base_y + P->lift + TD5_TG_R11_SIGN_POST_OV;
                int sp = TD5_TG_PAGE_R11_SIGN_POST, sq = 2, k = 0;
                size_t pb0 = h->blk->len;
                ppx[k]=cx-lx*hw; ppy[k]=base_y; ppz[k]=cz-lz*hw;
                puu[k]=0.0; pvv[k]=1.0; k++;
                ppx[k]=cx+lx*hw; ppy[k]=base_y; ppz[k]=cz+lz*hw;
                puu[k]=1.0; pvv[k]=1.0; k++;
                ppx[k]=cx+lx*hw; ppy[k]=ptop;   ppz[k]=cz+lz*hw;
                puu[k]=1.0; pvv[k]=0.0; k++;
                ppx[k]=cx-lx*hw; ppy[k]=ptop;   ppz[k]=cz-lz*hw;
                puu[k]=0.0; pvv[k]=0.0; k++;
                ppx[k]=cx-ax*hw; ppy[k]=base_y; ppz[k]=cz-az*hw;
                puu[k]=0.0; pvv[k]=1.0; k++;
                ppx[k]=cx+ax*hw; ppy[k]=base_y; ppz[k]=cz+az*hw;
                puu[k]=1.0; pvv[k]=1.0; k++;
                ppx[k]=cx+ax*hw; ppy[k]=ptop;   ppz[k]=cz+az*hw;
                puu[k]=1.0; pvv[k]=0.0; k++;
                ppx[k]=cx-ax*hw; ppy[k]=ptop;   ppz[k]=cz-az*hw;
                puu[k]=0.0; pvv[k]=0.0; k++;
                h->moff[*h->nmesh] = pb0;
                if (!tg_write_quad_mesh(h->blk, ppx, ppy, ppz, puu, pvv, 8,
                                        &sp, &sq, 1))
                    return 0;
                if (h->blk->len > pb0) { (*h->nmesh)++; s_r15_sign_posts++; }
            }
        }
        b0 = h->blk->len;
        h->moff[*h->nmesh] = b0;
        if (!tg_emit_billboard_mesh(h->blk, cx, base_y + P->lift, cz,
                                    P->w * 0.5, P->hgt,
                                    TD5_TG_INFRA_PAGE(P->page_side), 1))
            return 0;
        if (h->blk->len > b0) {
            (*h->nmesh)++;
            tg_acct(TG_ACCT_R9_INFRA, h->si);
            s_r9_infra_props++;
        }
        return 1;
    }
    /* [R12 PROPS item 2] A bench is the one piece on this menu that is not a
     * closed volume, so it gets its own assembly. Everything else stays a box. */
    if (kind == IP_BENCH && td5_env_flag_on("TD5RE_R12_BENCH_FORM"))
        return tg_infra_bench(h, P, cx, base_y, cz, ax, az, lx, lz);
    /* [R13 PROPS item 5a] An awning is not a closed volume either -- it is a
     * sheet hung on a wall. Same exception, same reason. */
    if (kind == IP_CANOPY && td5_env_flag_on("TD5RE_R13_AWNING"))
        return tg_infra_awning(h, P, cx, base_y, cz, ax, az, lx, lz);
    return tg_infra_box(h, P, cx, base_y, cz, ax, az, lx, lz);
}

/* The menu for a biome: which pieces belong beside THIS road, in the order a
 * per-span hash picks from. Returns the count written into `out`. Kept as a
 * function rather than a column in k_biomes so the shared biome table stays a
 * single definition, matched on name the way tg_people_density is. */
int tg_infra_menu(const TG_Biome *b, int paved, int *out)
{
    int n = 0;

    /* BIOME FIRST, pavement second. The first cut had it the other way round
     * and the ORIENTAL and INDUSTRIAL branches sat INSIDE the paved arm -- but
     * ORIENTAL is a billboard biome here, so tg_city_sidewalk_w returns 0 for
     * it and that whole arm was unreachable. The rickshaw, the piece with the
     * strongest biome tie on the list, was never placed once on either seed.
     * Caught by the per-piece report, not by the mesh count, which was happily
     * counting 546 pieces the entire time -- the round's own method note that a
     * count going up is not evidence of correct placement. */
    if (!strcmp(b->name, "ORIENTAL")) {
        out[n++] = IP_RICKSHAW;
        out[n++] = IP_CRATE;
        out[n++] = IP_SIGN;
        out[n++] = IP_RICKSHAW;
        if (paved) { out[n++] = IP_BIN; out[n++] = IP_CANOPY; }
        return n;
    }
    if (!strcmp(b->name, "INDUSTRIAL")) {
        out[n++] = IP_CRATE;
        out[n++] = IP_CRATEFRG;
        out[n++] = IP_CARDBOX;
        out[n++] = IP_SIGN;
        if (paved) out[n++] = IP_BIN;
        return n;
    }
    if (paved) {   /* CITY and any other biome with a real pavement */
        out[n++] = IP_BIN;
        out[n++] = IP_SIGN;
        out[n++] = IP_BIN;                        /* bins are the common case */
        out[n++] = IP_PHONE;
        out[n++] = IP_BENCH;
        out[n++] = IP_CANOPY;                     /* hangs off a frontage */
        return n;
    }
    /* Unpaved: a verge, not a pavement. Only what plausibly stands on grass,
     * so no awning (it needs a shopfront to hang from) and no phone box. */
    if (!strcmp(b->name, "COAST")) {
        out[n++] = IP_BENCH;
        out[n++] = IP_SIGN;
        out[n++] = IP_BIN;
    } else {
        out[n++] = IP_SIGN;
        out[n++] = IP_CRATE;
        out[n++] = IP_BENCH;
    }
    return n;
}

static int tg_infra_sign_biome_ok(const TG_Biome *b, int paved)
{
    if (paved) return 1;                       /* a pavement means a street  */
    return !strcmp(b->name, "ORIENTAL") || !strcmp(b->name, "INDUSTRIAL");
}

/* [R15 PROPS item 1] "only on the crossing streets."
 *
 * R12 above narrowed the disc from "any verge" to "any settled biome", which
 * still leaves it standing along open frontage where there is no junction to
 * forbid a turn into. A no-entry disc is a statement about a JUNCTION, so the
 * span itself has to be one. tg_city_crossing_here is the generator's single
 * answer to "is a crossing laid at this span" (memoised, span-only), and the
 * +/-1 window puts the disc on the CORNER rather than in the mouth -- the mouth
 * itself is refused later by tg_xstreet_occupies, which would otherwise make
 * this gate and that refusal cancel out to nothing placed at all.
 *
 * Same SUBSTITUTE-don't-skip discipline as R12: the refused pick becomes a bin
 * or a bench, so furniture density and s_r9_infra_props stay comparable across
 * the A/B and only the sign SHARE moves. */
int tg_r15_sign_at_crossing(int si, double side)
{
    const int sidx = (side > 0.0) ? 1 : 0;
    if (!td5_env_flag_on("TD5RE_R15_SIGN_XING")) return 1;
    /* A "crossing street" is, for this purpose, either a painted crossing or a
     * SIDE-STREET MOUTH. Both are needed: painted crossings are thinned hard by
     * TD5RE_AUTOTRACK_XMIN, so keying on them alone put ONE disc on the whole
     * 1987-span seed-1459285111 track (measured). A frontage GAP is the same
     * thing the cross-street carriageway, the back rows and the corner arms all
     * key off -- tg_facade_built == 0 on this side IS the opening the street
     * runs through -- and there are far more of them. The +/-1 window keeps the
     * disc on the CORNER; the mouth itself is refused later by
     * tg_xstreet_occupies. */
    if (!tg_facade_built(si, sidx)) return 1;
    if (si > 0 && !tg_facade_built(si - 1, sidx)) return 1;
    if (!tg_facade_built(si + 1, sidx)) return 1;
    return tg_city_crossing_here(si)
        || (si > 0 && tg_city_crossing_here(si - 1))
        || tg_city_crossing_here(si + 1);
}

/* Resolve a menu pick. Returns the kind to place; only IP_SIGN can change. */
int tg_infra_sign_filter(const TG_Biome *b, int si, double side,
                                int paved, int kind, unsigned int hh)
{
    if (kind != IP_SIGN) return kind;
    if (!td5_env_flag_on("TD5RE_R12_SIGN_CTX")) { s_r12_sign_kept++; return kind; }
    if (!tg_r15_sign_at_crossing(si, side)) {
        s_r15_sign_xing++;
        return paved ? IP_BIN : (((hh >> 3) & 1u) ? IP_BIN : IP_BENCH);
    }
    /* [R15 PROPS item 1] At a junction, DROP the R12 rate thinning.
     *
     * MEASURED, not assumed: with the crossing gate stacked on top of R12's
     * 1-in-4 rate (itself on top of ~1 span in 5 carrying furniture and a
     * 1-in-6 menu pick), seed 1459285111 produced 178 substitutions and ZERO
     * discs -- the sign was gated out of existence and no post was ever built.
     * R12's rate exists because the disc was appearing where there was nothing
     * to forbid; now that CONTEXT selects the junctions, the rate is doing that
     * job a second time. Keep the biome gate, drop the dice. */
    if (tg_infra_sign_biome_ok(b, paved)) {
        /* The crossing gate has already done the thinning R12's 1-in-4 rate
         * (TD5_TG_SIGN_KEEP_1_IN, bits 25-27) used to do, so the disc is kept
         * outright here. With TD5RE_R15_SIGN_XING=0 the gate above passes every
         * span and this becomes R12's "allowed biome" arm unthinned -- which is
         * why that knob is an A/B for the CONTEXT rule, not a full R12 restore;
         * TD5RE_R12_SIGN_CTX=0 above is the full restore. */
        s_r12_sign_kept++;
        return kind;
    }
    s_r12_sign_ctx++;
    if (paved) return IP_BIN;
    return ((hh >> 3) & 1u) ? IP_BIN : IP_BENCH;
}

/* [R13 PROPS item 5a] The awning's half of the same rule: an awning is a claim
 * that there is a SHOP FRONT immediately behind it, so it may only be picked
 * where a facade actually stands on this side. The frontage authority is asked
 * (tg_facade_stands for the span, tg_facade_built for the side) rather than a
 * biome name or a re-derived hash -- the mistake that made trees stand on branch
 * carriageways for three rounds. Refused picks SUBSTITUTE, so the furniture
 * total and the r9-infra run list are unchanged, exactly as R12's sign does.
 * `side` follows tg_emit_fb_infra's convention: > 0 is the left kerb. */
int tg_infra_awning_filter(int si, double side, int paved, int kind,
                                  unsigned int hh)
{
    if (kind != IP_CANOPY) return kind;
    if (!td5_env_flag_on("TD5RE_R13_AWNING")) { s_r13_awn_kept++; return kind; }
    if (tg_facade_stands(si) && tg_facade_built(si, side > 0.0 ? 1 : 0)) {
        s_r13_awn_kept++;
        return kind;
    }
    s_r13_awn_ctx++;
    /* Bit 4 is untouched by every other decision on this span (see the sign
     * filter's bit map), so the stand-in is independent of all of them. */
    if (paved) return ((hh >> 4) & 1u) ? IP_BIN : IP_BENCH;
    return IP_BIN;
}

/* [R15] Per-module half of the round-15 report. Split out of the single
 * tg_r15_sky_report the work was first written against: after the trackgen
 * split its counters live in four different modules, and a file-static
 * cannot be read from another translation unit. One report per owning
 * module keeps the counters static where they belong.  */
void tg_r15_city_report(void)
{
    TD5_LOG_I(LOG_TAG, "[R15 CITY item 8b] sub-TALL_ROWS blocks closed at the "
              "back = %ld (knob TD5RE_R15_BACK_CLOSE=%s)", s_r15_back_closed,
              td5_env_flag_on("TD5RE_R15_BACK_CLOSE") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 PROPS item 1] no-entry discs: posts modelled=%ld, "
              "picks substituted away from a non-junction span=%ld (knobs "
              "TD5RE_R15_SIGN_POST=%s TD5RE_R15_SIGN_XING=%s)",
              s_r15_sign_posts, s_r15_sign_xing,
              td5_env_flag_on("TD5RE_R15_SIGN_POST") ? "on" : "off",
              td5_env_flag_on("TD5RE_R15_SIGN_XING") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 TEX item 2] storefront anti-repeat: rerolls=%ld "
              "(knob TD5RE_R15_STORE_VARY=%s); see the store-pages census for "
              "the resulting spread", s_r15_store_reroll,
              td5_env_flag_on("TD5RE_R15_STORE_VARY") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 PROPS item 5] monuments skipped, frontage both "
              "sides so no plaza = %ld (knob TD5RE_R15_PLAZA_ONLY=%s)",
              s_r15_statue_walled,
              td5_env_flag_on("TD5RE_R15_PLAZA_ONLY") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 CITY item 6] raised slabs dropped at a painted "
              "crossing = %ld (knob TD5RE_R15_XING_PAVE=%s)", s_r15_pave_xing,
              td5_env_flag_on("TD5RE_R15_XING_PAVE") ? "on" : "off");
    TD5_LOG_I(LOG_TAG, "[R15 OCC items 4+7+8a] back rows refused (gap with no "
              "street laid)=%ld, span-sides pushed clear of road/street/"
              "pavement=%ld, row-0 pulled in to terminate the street=%ld "
              "(knobs TD5RE_R15_BACKROW_STREET=%s TD5RE_R15_BACKROW_OCC=%s "
              "TD5RE_R15_BACKROW_CLOSE=%s)", s_r15_backrow_nostreet,
              s_r15_backrow_push, s_r15_backrow_close,
              td5_env_flag_on("TD5RE_R15_BACKROW_STREET") ? "on" : "off",
              td5_env_flag_on("TD5RE_R15_BACKROW_OCC") ? "on" : "off",
              td5_env_flag_on("TD5RE_R15_BACKROW_CLOSE") ? "on" : "off");
}
