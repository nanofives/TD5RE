/**
 * td5_tg_pages.c -- auto-track TEXTURES.DAT: every page emitter (procedural + real shipped pages)
 *
 * Split from the td5_trackgen.c monolith (2026-09-06); shared declarations
 * live in td5_trackgen_internal.h. Element map: docs/plans/AUTOTRACK_ELEMENT_CATALOG.md.
 */
#include "td5_trackgen_internal.h"

/* ==== [R12 TEX items 8b + 12b] THE "TILE TEXTURE" ==========================
 *
 * "The snow FLOOR reads as tiles -- do not use that texture" (span 407) and
 * "stop using the tile texture for the coastline and the bridge pillars"
 * (span 523). The round brief's hypothesis was that all three roles share ONE
 * page. MEASURED, with tg_r12_tex_report on seed 20260901, they do NOT:
 *   span 407  snow floor  -> page 373 (R8 snow-ground variant 2)
 *   span 523  bank/shore  -> page 5   (GROUND, the gorge skirt beside a run)
 *   span 523  pillars     -> page 128 (R4 cast-concrete pier, bridge style 0)
 * Three different pages, and the coastline's own R9 SHORE page (473) decodes as
 * a clean graded beach, so it is not the surface being complained about at all.
 *
 * What the three DO share is one AUTHORING IDIOM. Every one of them picks its
 * coarse tone from a mask of the form
 *     patch = ((x >> k) + (y >> k) * C) * hash;   if ((patch >> 30) ... )
 * i.e. a hard two-state decision taken per AXIS-ALIGNED CELL. Decoded (the PPM
 * dump, TD5RE_R8_TEXDUMP=1) page 373 is literally a chequerboard of 8x8-texel
 * squares, page 5 a grid of dark 8x8 squares, page 128 a run of 16-texel bands.
 * Each page is then drawn at a SHORT world period -- the ground skirt tiles once
 * per span (1500 units, tg_emit_ground), the pier at 600 -- so the cell grid
 * repeats every few metres and the eye reads a tiled floor.
 *
 * So the fix is a PROPERTY fix, not a blacklist: keep every page's palette and
 * its intent (drifts on snow, gravel stains on concrete, form-board grain on a
 * pier) and only replace the hard cell mask with a SMOOTH one. This is
 * wrapping value noise: a hash per cell CORNER, smoothstep-interpolated across
 * the cell, summed over two octaves. Its level sets are irregular curves that
 * cross cell boundaries, so no square survives, and because the corner hash
 * wraps modulo the cell count the page still tiles exactly as before.
 *
 * Returns 0..255. TD5RE_R12_TEX_BLOTCH=0 restores the hard cell masks. */
static int tg_r12_tex_blotch_on(void)
{
    return td5_env_flag_on("TD5RE_R12_TEX_BLOTCH");   /* default ON */
}

static unsigned tg_r12_corner(int cx, int cy, int n, unsigned seed)
{
    unsigned h;
    cx = ((cx % n) + n) % n;                 /* wrap, so the page still tiles */
    cy = ((cy % n) + n) % n;
    h = (unsigned)cx * 374761393u + (unsigned)cy * 668265263u + seed;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (h ^ (h >> 16)) & 0xFFu;
}

/* One octave: bilinear-with-smoothstep value noise on a `cell`-texel grid. */
static int tg_r12_octave(int x, int y, int cell, unsigned seed)
{
    const int n  = TD5_TG_TEX_DIM / (cell > 0 ? cell : 1);
    const int cx = x / cell, cy = y / cell;
    const int fx = x - cx * cell, fy = y - cy * cell;
    /* smoothstep(t) on 0..256 fixed point, t = f / cell. */
    const int tx = (fx * 256) / cell, ty = (fy * 256) / cell;
    const int sx = (tx * tx * (768 - 2 * tx)) >> 17;   /* 3t^2-2t^3, 0..256 */
    const int sy = (ty * ty * (768 - 2 * ty)) >> 17;
    const unsigned a = tg_r12_corner(cx,     cy,     n, seed);
    const unsigned b = tg_r12_corner(cx + 1, cy,     n, seed);
    const unsigned c = tg_r12_corner(cx,     cy + 1, n, seed);
    const unsigned d = tg_r12_corner(cx + 1, cy + 1, n, seed);
    const int top = (int)a + (((int)b - (int)a) * sx >> 8);
    const int bot = (int)c + (((int)d - (int)c) * sx >> 8);
    return top + ((bot - top) * sy >> 8);
}

/* Two octaves, the second at half the cell and a third of the weight, so a
 * drift has a ragged edge instead of a mathematically smooth one. */
static int tg_r12_blotch(int x, int y, int cell, unsigned seed)
{
    int c2 = cell / 2;
    int v  = tg_r12_octave(x, y, cell, seed);
    if (c2 < 2) return v;
    v = (v * 3 + tg_r12_octave(x, y, c2, seed ^ 0x9E3779B9u)) / 4;
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* Page 0: asphalt with a lane line down one edge of the tile. The road mesh's
 * UVs run u = 0..lanes and tile, so a tile edge lands exactly on every lane
 * boundary -- a stripe at u=0 therefore draws lane dividers AND both road
 * edges for free, with no extra geometry. */
static void tg_emit_texture_page_asphalt(TG_Buf *out)
{
    unsigned int rng = 0x1234567u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);  /* pad[3] */
    tg_put_u8(out, 0);                                        /* type: opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Palette is BGR. 0..11 asphalt greys, 12..15 near-white for the marking. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (44 + i * 3) : (196 + (i - 12) * 12);
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (x <= 1) {
            /* Lane marking, dashed down-track so it reads as road paint. */
            idx = ((y % 24) < 16) ? (12 + (int)((rng >> 16) % 4)) : 6;
        } else {
            idx = (int)((rng >> 16) % 12);   /* asphalt grain */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 1 (+ variants): building wall -- banded masonry with lit windows. The
 * `variant` seeds the concrete tone, window tint and window rhythm so the
 * procedural streetscape mixes several facades the way the real one does. */
static void tg_emit_texture_page_wall(TG_Buf *out, int variant)
{
    unsigned int rng = 0x9E3779B9u + (unsigned)variant * 0x2545F491u;
    int wcols  = 3 + (variant % 3);      /* window bays per cell: 3..5 */
    int warm   = (variant & 1) ? 24 : 0; /* some blocks read brick-warm */
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 concrete greys, 10..12 mortar shadow, 13..15 lit windows. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 96 + i * 7;         g = 92 + i * 7;
                           r = 88 + i * 7 + warm;  }
        else if (i < 13) { b = 54;                 g = 52;
                           r = 50 + warm;          }
        else             { b = 150 + (i-13) * 30 - warm; g = 170 + (i-13) * 28;
                           r = 190 + (i-13) * 20 + warm;   }
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int cellpx = TD5_TG_TEX_DIM / (wcols + 1);   /* window cell pitch */
        int wx = (cellpx > 0) ? (x % cellpx) : x;
        int wy = y % 16;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (wy < 2 || wx < 2) {
            idx = 10 + (int)((rng >> 16) % 3);          /* storey / pier lines */
        } else if (wx >= 3 && wx <= cellpx - 3 && wy >= 5 && wy <= 12) {
            /* Window, lit or dark per cell so the facade is not uniform. */
            unsigned int cell = (unsigned)((y / 16) * 8 + (x / cellpx)) * 2654435761u;
            idx = ((cell >> 28) & 1) ? (13 + (int)((rng >> 18) % 3)) : 11;
        } else {
            idx = (int)((rng >> 16) % 10);               /* concrete */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Storefront pages: a glazed ground floor -- a coloured awning/sign band across
 * the top, big mullioned shop windows below with the odd lit pane, and a dark
 * doorway. `variant` recolours the awning (red/green/blue) so shops differ. */
static void tg_emit_texture_page_store(TG_Buf *out, int variant)
{
    static const int awn[3][3] = {           /* BGR awnings */
        { 40, 40, 170 }, { 60, 150, 60 }, { 160, 90, 40 } };
    unsigned int rng = 0x1234567u + (unsigned)variant * 0x9E3779B9u;
    int av = variant % 3, i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..5 dark glass, 6..8 frame/mullion + doorway, 9..11 awning, 12..15
     * reflection / lit-sign highlights. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 6)       { b = 46 + i * 6;  g = 40 + i * 5;  r = 34 + i * 4; }
        else if (i < 9)  { b = 26;          g = 24;          r = 22;         }
        else if (i < 12) { b = awn[av][0];  g = awn[av][1];  r = awn[av][2]; }
        else             { b = 150 + (i-12) * 24; g = 160 + (i-12) * 22;
                           r = 170 + (i-12) * 18; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 12) {
            idx = 9 + (int)((rng >> 16) % 3);            /* awning / sign band */
        } else if (y > 46 && x > 26 && x < 38) {
            idx = 6 + (int)((rng >> 16) % 3);            /* dark doorway */
        } else if ((x % 16) < 2 || (y % 20) < 2) {
            idx = 6 + (int)((rng >> 16) % 3);            /* window frame/mullion */
        } else {
            unsigned int pane = (unsigned)((y / 20) * 4 + x / 16) * 2654435761u;
            idx = ((pane >> 29) == 0) ? (12 + (int)((rng >> 18) % 4))
                                      : (int)((rng >> 16) % 6);   /* lit / glass */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 2: foliage / vegetation -- mottled greens for hedgerows and treelines.
 * Deliberately noisy rather than structured: these boxes stand in for organic
 * mass, so any regular pattern reads as wrong. */
static void tg_emit_texture_page_green(TG_Buf *out)
{
    unsigned int rng = 0x51ED2701u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. Dark shadowed foliage up to sunlit leaf, with a little earth. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 12) { b = 28 + i * 3; g = 52 + i * 9; r = 24 + i * 4; }
        else        { b = 46;         g = 58;         r = 62 + (i-12) * 6; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Clumped rather than per-texel noise: bias by a coarse cell so the
         * canopy has light and dark masses instead of uniform static. */
        {
            int x = i % TD5_TG_TEX_DIM, y = i / TD5_TG_TEX_DIM;
            unsigned int cell = (unsigned)((y / 8) * 8 + (x / 8)) * 2654435761u;
            int bias = (int)((cell >> 29) % 5);
            idx = (int)((rng >> 16) % 8) + bias;
            if (idx > 15) idx = 15;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Tree pages: an ALPHA-KEYED silhouette (type 1, index 0 = transparent key) so
 * the surround cuts out. `shape` selects the species outline so the procedural
 * fallback still mixes deciduous/conifer/palm/topiary/willow like the real set. */
static void tg_emit_texture_page_tree(TG_Buf *out, int shape)
{
    unsigned int rng = 0x2545F491u + (unsigned)shape * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key (never drawn), 1..3 trunk browns, 4..15 canopy greens
     * (olive-yellow for palm fronds). */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)                       { b = 255; g = 0;  r = 255; }
        else if (i < 4)                   { b = 30 + i * 6; g = 44 + i * 8; r = 62 + i * 10; }
        else if (shape == TG_TREE_PALM)   { b = 24 + i;     g = 70 + i * 8; r = 40 + i * 4;  }
        else                              { b = 26 + i * 2; g = 60 + i * 10; r = 22 + i * 3; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        /* y=0 is the TOP of the page; the billboard maps the base to v=1. */
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;

        switch (shape) {
        case TG_TREE_CONIFER:      /* triangle widening to the base */
            if (y >= 52) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int hw = 2 + (y * 26) / 52 - (int)((rng >> 19) % 4);
                   if (dx > -hw && dx < hw) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_PALM:         /* tall bare trunk, fronds fanning at the top */
            if (y >= 20) { if (dx > -2 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 8, rad = 20 - (ry * ry) / 6 - (int)((rng >> 19) % 4);
                   if (dx * dx < rad * rad && ((x + y) & 3) != 0)
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        case TG_TREE_TOPIARY:      /* tight round ball on a short stem */
            if (y >= 48) { if (dx > -2 && dx < 2) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 26, rad = 22 - (int)((rng >> 19) % 2);
                   if (dx * dx + ry * ry < rad * rad) idx = 4 + (int)((rng >> 16) % 8); }
            break;
        case TG_TREE_WILLOW:       /* wide drooping canopy with trailing strands */
            if (y >= 50) { if (dx > -3 && dx < 3) idx = 1 + (int)((rng >> 17) % 3); }
            else { int rad = 24 - (y / 4) - (int)((rng >> 19) % 3);
                   if (dx > -rad && dx < rad && (y < 30 || ((y + dx) & 1)))
                       idx = 4 + (int)((rng >> 16) % 12); }
            break;
        default:                   /* TG_TREE_DECID: rough blob */
            if (y >= 44) { if (dx > -4 && dx < 4) idx = 1 + (int)((rng >> 17) % 3); }
            else { int ry = y - 22, rad = 26 - (ry * ry) / 18 - (int)((rng >> 19) % 5);
                   if (dx * dx < rad * rad) idx = 4 + (int)((rng >> 16) % 12); }
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Procedural prop silhouettes (fallback when real textures are off): a person,
 * a stone statue, an animal, or a radial streetlamp glow. Alpha-keyed (index 0)
 * except the lamp, which is additive (type 3). Crude but recognisable. */
static void tg_emit_texture_page_prop(TG_Buf *out, int kind)
{
    unsigned int rng = 0x51ED2701u + (unsigned)kind * 0x9E3779B9u;
    int type = (kind == TG_PROP_LAMP) ? 3 : 1;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)                     { b = 255; g = 0; r = 255; }   /* key */
        else if (kind == TG_PROP_LAMP)  { int v = 60 + i * 13; if (v > 255) v = 255;
                                          b = v; g = v; r = (v > 30 ? v - 30 : 0); }
        else if (kind == TG_PROP_STATUE){ int v = 70 + i * 11; if (v > 255) v = 255;
                                          b = v; g = v; r = v; }
        else if (kind == TG_PROP_ANIMAL){ b = 40 + i * 6; g = 44 + i * 7; r = 52 + i * 9; }
        else if (i < 8)                 { b = 40 + i * 4; g = 44 + i * 5; r = 60 + i * 8; }
        else                            { b = 90; g = 70; r = 60; }   /* person cloth */
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP */
        int dx = x - 32;
        int idx = 0;
        rng = rng * 1103515245u + 12345u;
        switch (kind) {
        case TG_PROP_LAMP: {
            int d2 = dx * dx + (y - 32) * (y - 32), rad = 28;
            if (d2 < rad * rad) { int t = 15 - (d2 * 15) / (rad * rad);
                                  idx = t < 1 ? 1 : t; }
            break; }
        case TG_PROP_ANIMAL:
            if (y >= 30 && y < 46 && dx > -16 && dx < 16) idx = 2 + (int)((rng >> 16) % 6);
            else if (y >= 46 && y < 58 && (x % 10) < 3)   idx = 2 + (int)((rng >> 16) % 4);
            break;
        case TG_PROP_STATUE:
            if (y >= 54) { if (dx > -14 && dx < 14) idx = 4 + (int)((rng >> 16) % 6); }
            else if (y >= 16) { if (dx > -8 && dx < 8) idx = 6 + (int)((rng >> 16) % 8); }
            else { if (dx > -5 && dx < 5) idx = 6 + (int)((rng >> 16) % 8); }
            break;
        default: /* TG_PROP_PERSON */
            if (y >= 8 && y < 18) { if (dx > -5 && dx < 5) idx = 9 + (int)((rng >> 16) % 3); }
            else if (y >= 18 && y < 40) { if (dx > -8 && dx < 8) idx = 1 + (int)((rng >> 16) % 7); }
            else if (y >= 40 && y < 60) { if ((dx > -8 && dx < -1) || (dx > 1 && dx < 8))
                                              idx = 1 + (int)((rng >> 16) % 5); }
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Water page: deep blue with lighter ripple crests and the odd foam fleck. */
static void tg_emit_texture_page_water(TG_Buf *out)
{
    unsigned int rng = 0x009E12D3u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR: 0..11 blue deepening, 12..15 foam highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 12) { b = 150 + i * 8; g = 90 + i * 9;  r = 40 + i * 6; }
        else        { b = 235;         g = 210 + (i-12) * 10; r = 200 + (i-12) * 12; }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int y = i / TD5_TG_TEX_DIM;
        int band, idx;
        rng = rng * 1103515245u + 12345u;
        band = (y + (int)((rng >> 20) % 3)) % 8;
        if (band < 2) idx = 8 + (int)((rng >> 16) % 4);      /* ripple crest */
        else          idx = (int)((rng >> 16) % 8);          /* blue */
        if (((rng >> 24) & 31) == 0) idx = 12 + (int)((rng >> 16) % 4); /* foam */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Road-surface pages for the themed biomes: gravel, dirt, ice, cobble. No lane
 * paint (only the base tarmac page carries markings). `kind` is an RS_* value. */
static void tg_emit_texture_page_roadsurf(TG_Buf *out, int kind)
{
    unsigned int rng = 0x00C0FFEEu + (unsigned)kind * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        switch (kind) {
        case RS_DIRT:   b = 40 + i * 5;  g = 60 + i * 6;  r = 80 + i * 8;  break;
        case RS_ICE:    b = 210 + i * 3; g = 205 + i * 3; r = 195 + i * 3; break;
        case RS_COBBLE: b = 78 + i * 7;  g = 76 + i * 7;  r = 74 + i * 7;  break;
        default:        b = 96 + i * 6;  g = 96 + i * 6;  r = 94 + i * 6;  break; /* gravel */
        }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        switch (kind) {
        case RS_COBBLE: {                        /* stone blocks with mortar */
            int cx = x % 12, cy = y % 12;
            idx = (cx < 2 || cy < 2) ? 1 + (int)((rng >> 16) % 2)
                                     : 6 + (int)((rng >> 16) % 8);
            break; }
        case RS_ICE:                             /* pale, faint cracks */
            idx = 10 + (int)((rng >> 16) % 6);
            if (((rng >> 24) & 63) == 0) idx = 2 + (int)((rng >> 16) % 3);
            break;
        case RS_DIRT: {                          /* brown with down-track ruts */
            int rut = (x % 20);
            idx = (rut < 3) ? 2 + (int)((rng >> 16) % 3)
                            : 4 + (int)((rng >> 16) % 10);
            break; }
        default:                                 /* gravel speckle */
            idx = (int)((rng >> 16) % 14);
            break;
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 4: crash barrier -- galvanised steel with a dark shadow gutter along the
 * bottom and a rhythm of darker post marks.
 *
 * Reusing the building-wall page (the cheap option) was tried on paper and
 * rejected: that page is banded masonry WITH LIT WINDOWS, so a barrier drawn
 * with it reads as a low garden wall rather than as a road barrier. A page is
 * ~30 lines here, so a dedicated one is the cheaper mistake to avoid.
 *
 * The V axis runs up the barrier's face (see tg_emit_guardrail), so row 0 is
 * the bottom edge -- hence the gutter lives in the low rows, not the high ones.
 */
static void tg_emit_texture_page_rail(TG_Buf *out)
{
    unsigned int rng = 0x7F4A7C15u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..9 steel greys (cooler + brighter than the wall page's concrete),
     * 10..12 shadow/gutter, 13..15 specular highlight along the top rib. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 10)      { b = 150 + i * 8; g = 148 + i * 8; r = 142 + i * 8; }
        else if (i < 13) { b = 60;          g = 58;          r = 56;          }
        else             { b = 228 + (i-13) * 8; g = 228 + (i-13) * 8;
                           r = 224 + (i-13) * 8; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        /* [R11 GUARD item 11] Row 0 is the TOP of a page everywhere else in this
         * file, and this one page was authored bottom-up because tg_emit_guardrail
         * used to sample it that way. Now that the emitter maps v = 1 at the base
         * like the rest of the generator, the page has to turn over with it --
         * otherwise fixing the four real rails would flip the fallback. Mirrored
         * rather than re-authored so the A/B knob restores it exactly. */
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        if (tg_rail_vflip_on()) y = TD5_TG_TEX_DIM - 1 - y;
        rng = rng * 1103515245u + 12345u;
        if (y < 8) {
            idx = 10 + (int)((rng >> 16) % 3);        /* gutter under the rail */
        } else if (y >= 26 && y <= 34) {
            idx = 13 + (int)((rng >> 17) % 3);        /* highlight rib */
        } else if ((x % 32) < 3) {
            idx = 10 + (int)((rng >> 18) % 3);        /* post every half tile */
        } else {
            idx = (int)((rng >> 16) % 10);            /* steel */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Page 5: bare GROUND -- flat concrete/gravel for the terrain skirt. The point
 * is that it has NO structure: the CITY/INDUSTRIAL biomes used to floor their
 * ground with the WALL page, whose storey/window grid, stretched over the
 * undulating skirt, warped into a rippled "distorted" look filling the
 * background. Just tonal grain here -- a couple of darker gravel patches so it
 * is not a flat sheet, but no lines, so it reads as ground over any slope. */
static void tg_emit_texture_page_ground(TG_Buf *out)
{
    unsigned int rng = 0x2545F491u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..11 mid concrete greys (a touch warmer/darker than the steel rail),
     * 12..15 darker gravel/stain patches. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (104 + i * 5) : (70 - (i - 12) * 8);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));   /* faintly warm */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        /* Low-frequency patch mask so darker gravel clumps instead of speckling;
         * no axis-aligned lines, so nothing to shear over a slope. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 9) * 2654435761u;
        int idx;
        /* [R12 TEX item 12b] The gorge skirt beside a bridge run is this page
         * (tg_ground_page_for_span returns GROUND over a run), tiled once per
         * span, and the 8x8 cell mask below drew it as a grid of dark squares.
         * Same ink, smooth mask -- see tg_r12_blotch. */
        const int dark = tg_r12_tex_blotch_on()
                       ? (tg_r12_blotch(x, y, 8, 0x2545F491u) < 84)
                       : ((patch >> 29) == 0);
        rng = rng * 1103515245u + 12345u;
        if (dark)
            idx = 12 + (int)((rng >> 16) % 4);           /* gravel/stain */
        else
            idx = (int)((rng >> 16) % 12);               /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R3 BLOCK] pages for parks & houses (feedback items 5-6). One generator,
 * `which` selects the artwork so the four block pages share one function:
 *   0 park LAWN   -- mowed grass, brighter and more uniform than the mottled
 *                    hedgerow GREEN page, with faint mow stripes down-track so a
 *                    flat lawn is not a dead sheet.
 *   1 park HEDGE  -- dense dark clipped foliage, low-frequency clumps, no lines.
 *   2 house WALL  -- warm plaster/brick with small windows and a door, a domestic
 *                    frontage rather than the tall CITY office grid.
 *   3 house ROOF  -- terracotta tiles in horizontal courses.
 * Isotropic where it can be (lawn/hedge tile over a curved skirt); the house
 * pages map one cell per face so a light window grid is fine. */
static void tg_emit_texture_page_r3_block(TG_Buf *out, int which)
{
    unsigned int rng = 0x6C078965u + (unsigned)which * 0x9E3779B9u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Palette (BGR). */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (which == 0) {            /* mowed grass: green ramp, light->deep */
            b = 40 + i * 3;  g = 96 + i * 8;  r = 44 + i * 4;
        } else if (which == 1) {     /* hedge: darker, denser green */
            b = 24 + i * 2;  g = 58 + i * 6;  r = 22 + i * 3;
        } else if (which == 2) {     /* house wall: warm plaster/brick */
            if (i < 10)      { b = 120 + i * 6; g = 138 + i * 6; r = 158 + i * 6; }
            else if (i < 13) { b = 40;          g = 40;          r = 44;          }
            else             { b = 150 + (i-13)*24; g = 172 + (i-13)*22;
                               r = 196 + (i-13)*18; }   /* lit panes */
        } else {                     /* roof: terracotta tiles */
            if (i < 12) { b = 40 + i * 3; g = 60 + i * 4; r = 120 + i * 8; }
            else        { b = 30;         g = 44;         r = 90 + (i-12)*6; }
        }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (which == 0) {
            /* Grass grain with a faint vertical mow stripe every 8 texels. */
            int base = 6 + (int)((rng >> 16) % 8);
            if (((x >> 3) & 1) == 0) base += 2;
            if (base > 15) base = 15;
            idx = base;
        } else if (which == 1) {
            /* Clumped foliage, no structure -- reads as a hedge over any slope. */
            unsigned int cell = (unsigned)((y / 8) * 8 + (x / 8)) * 2654435761u;
            idx = ((cell >> 29) == 0) ? (int)((rng >> 16) % 5)
                                      : 5 + (int)((rng >> 16) % 10);
        } else if (which == 2) {
            /* Two window bays + a central door on the ground row. */
            int cell = TD5_TG_TEX_DIM / 3;              /* 3 bays across */
            int wx = x % cell, wy = y % 20;
            if (wy < 2 || wx < 2) {
                idx = 10 + (int)((rng >> 16) % 3);      /* trim / mortar lines */
            } else if (y > 48 && x > 27 && x < 37) {
                idx = 10 + (int)((rng >> 16) % 3);      /* door */
            } else if (wx >= 3 && wx <= cell - 3 && wy >= 5 && wy <= 15) {
                unsigned int pane = (unsigned)((y / 20) * 3 + x / cell)
                                    * 2654435761u;
                idx = ((pane >> 29) == 0) ? (13 + (int)((rng >> 18) % 3)) : 11;
            } else {
                idx = (int)((rng >> 16) % 10);          /* plaster */
            }
        } else {
            /* Terracotta courses: a darker mortar line every 8 rows. */
            idx = ((y & 7) == 0) ? 12 + (int)((rng >> 16) % 4)
                                 : (int)((rng >> 16) % 12);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* ===================== [FB] FEEDBACK-BATCH PAGES =====================
 * One emitter per page slot reserved in the TD5_TG_PAGE_* block above. Each was
 * seeded as tonal grain when the slots were carved and then filled in with real
 * artwork by the area that owns it, so the shared placeholder helper is gone.
 *
 * The recurring lesson in these emitters: a page has to read at the SIZE its
 * geometry maps it at, and several of them are stretched over surfaces that
 * curve or climb, where any axis-aligned feature ladders or shears. */

/* City street furniture: 0 = SIDEWALK paving, 1 = CROSSING (zebra), 2 = FENCE
 * railing (alpha-keyed, index 0 must stay transparent).
 *
 * All three have to read at the SIZE the geometry maps them at, which is what
 * decides the patterns below:
 *   SIDEWALK is mapped isotropically at one page per SPAN_LENGTH (1500 raw), so
 *     16-texel slabs come out at ~375 raw -- a paving slab, not a tile floor.
 *   CROSSING is mapped u = 0..lanes across the road, so each page repeat covers
 *     one lane; two bars per page gives the ~8 bars a 4-lane crossing wants.
 *     Its background must MATCH the asphalt or the crossing reads as a grey
 *     patch on the road rather than paint on it.
 *   FENCE is one page per span, so 8 uprights per page is one every ~190 raw.
 *     Everything that is not metal is index 0 and cuts out. */
static void tg_emit_texture_page_fb_city(TG_Buf *out, int which)
{
    unsigned int rng = 0xC17A0000u + (unsigned)which * 0x9E3779B9u;
    const int type = (which == 2) ? 1 : 0;      /* FENCE is alpha-keyed */
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. Index 0 is the transparent key on the FENCE page only -- the other
     * two are opaque, so index 0 is an ordinary colour there. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (which == 2) {
            if (i == 0)      { b = 255; g = 0;   r = 255; }        /* key   */
            else if (i < 11) { b = 118 + i * 5; g = 122 + i * 5; r = 128 + i * 5; }
            else             { b = 62 + i * 2;  g = 64 + i * 2;  r = 68 + i * 2;  }
        } else if (which == 1) {
            /* 0..7 asphalt greys, 8..15 worn white paint. */
            if (i < 8) { b = 52 + i * 4;       g = 52 + i * 4;       r = 54 + i * 4; }
            else       { b = 196 + (i - 8) * 7; g = 198 + (i - 8) * 7; r = 200 + (i - 8) * 7; }
        } else {
            /* 0..11 paving greys, 12..15 darker mortar joints. */
            if (i < 12) { b = 136 + i * 5; g = 138 + i * 5; r = 136 + i * 5; }
            else        { b = 94 - (i - 12) * 7; g = 96 - (i - 12) * 7; r = 94 - (i - 12) * 7; }
        }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;       /* y = 0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (which == 2) {
            /* Uprights every 8 texels, plus a top rail, a waist rail and a
             * bottom rail. The geometry maps v = 1 at the base, matching this
             * top-down row order. */
            if ((x & 7) < 2 || y < 4 || (y >= 27 && y < 31) || y >= 60)
                idx = 1 + (int)((rng >> 16) % 10);
            else
                idx = 0;                        /* keyed out */
        } else if (which == 1) {
            /* Two full-length bars per page, bars varying in x (== u, across
             * the road) so they run ALONG the direction of travel. Painted
             * edges get a texel of grain so they are not razor-straight. */
            const int b0 = x & 31;
            idx = (b0 >= 3 && b0 < 21) ? (8 + (int)((rng >> 16) % 8))
                                       : (int)((rng >> 16) % 8);
        } else {
            /* 16-texel slabs with mortar joints, plus per-texel grain. */
            idx = ((x & 15) == 0 || (y & 15) == 0)
                  ? 12 + (int)((rng >> 18) % 4)
                  : (int)((rng >> 16) % 12);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 CROSS item 9] CROSS-STREET asphalt with a LONGITUDINAL centre line.
 *
 * The side-street carriageway (tg_city_emit_crossstreet) used the biome road
 * page, whose lane paint is a stripe at the u=0 tile edge -- authored for the
 * MAIN road where u runs 0..lanes ACROSS. On the cross-street the mesh maps u
 * along the OUTWARD run and v across the ~one-lane width, so that same stripe
 * comes out as a rung every ~1500 raw ACROSS the side street -- rungs, not lane
 * markings. The user asked for "lane markers on the perpendicular direction if
 * it's a crossing", i.e. a line running DOWN the side street.
 *
 * This page puts the marking where the cross-street mapping needs it: a dashed
 * white line along the page's X axis (== u == outward) at its Y centre (== v
 * centre == the middle of the street width), so it reads as one broken centre
 * line the length of the perpendicular street. Asphalt grey elsewhere so it
 * still matches the main carriageway it leaves. OPAQUE. */
static void tg_emit_texture_page_r4_cross(TG_Buf *out)
{
    unsigned int rng = 0x5C0554E1u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR: 0..11 asphalt greys (match the main road page), 12..15 near-white. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (44 + i * 3) : (196 + (i - 12) * 12);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;       /* == u, along the street       */
        const int y = i / TD5_TG_TEX_DIM;       /* == v, across the street      */
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Centre line: 2 rows either side of y=32, dashed along x so it reads as
         * road paint rather than a solid rail. Tiles once in v per span, so the
         * line lands mid-street on every span it covers. */
        if (y >= 30 && y <= 33 && (x % 20) < 12)
            idx = 12 + (int)((rng >> 16) % 4);
        else
            idx = (int)((rng >> 16) % 12);       /* asphalt grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* First row of the densest TG_TREELINE_WIN-row window of a 64x64 index page. */
static int tg_treeline_src_window(const unsigned char *idx)
{
    int y, best_y = 0, best = -1;

    for (y = 0; y + TG_TREELINE_WIN <= TD5_TG_TEX_DIM; y++) {
        int fill = 0, r, x;
        for (r = y; r < y + TG_TREELINE_WIN; r++)
            for (x = 0; x < TD5_TG_TEX_DIM; x++)
                if (idx[r * TD5_TG_TEX_DIM + x]) fill++;
        if (fill > best) { best = fill; best_y = y; }
    }
    return best_y;
}

/* Most common non-key index inside that window -- the canopy's dominant colour,
 * used whenever a sample lands in a keyed gap and the band must stay solid. */
static int tg_treeline_src_fill(const unsigned char *idx, int y0)
{
    int hist[256], i, r, x, best = 0;

    memset(hist, 0, sizeof(hist));
    for (r = y0; r < y0 + TG_TREELINE_WIN; r++)
        for (x = 0; x < TD5_TG_TEX_DIM; x++) {
            const int v = idx[r * TD5_TG_TEX_DIM + x];
            if (v) hist[v]++;
        }
    for (i = 1; i < 256; i++) if (hist[i] > hist[best]) best = i;
    return best ? best : 1;
}

/* Real-texture band: shipped palette verbatim (no haze tint -- that is exactly
 * what greyed the synthetic page out) over shipped canopy texels, cut off at
 * the top by the same lumpy crown line the synthetic page uses. The crown line
 * is a MASK, not art, so it carries no placeholder colour of its own. */
/* [R8 TERRAIN item 14 VARY] Per-variant source. Variant 0 is the shipped page
 * verbatim (TG_TREELINE_SRC = tree 1, the measured densest broadleaf canopy), so
 * the A/B against round 7 is a true "before". The other three take a different
 * source tree, a different crown-cell width and a different crown RNG, which
 * changes both the foliage colour/texel mass AND the silhouette rhythm -- the
 * two things that made one page recur visibly every four spans. Source indices
 * are into k_real_tree_* (10 shipped foliage pages); the densest 20-row window
 * of each is still COMPUTED, so a thin or trunk-heavy source cannot slide the
 * band onto bark. */
static const struct { int src, cell; unsigned seed; } k_r8_treeline_var[4] = {
    { TG_TREELINE_SRC,  9, 0x77E1B3C5u },   /* 0: the round-7 page, unchanged  */
    { 4,               13, 0x2F19A77Bu },   /* 1: wider crowns, other foliage  */
    { 7,                7, 0x51C0DE33u },   /* 2: tighter, busier crown line   */
    { 9,               11, 0x9A3B7E11u }    /* 3: conifer-ish narrow silhouette*/
};

static void tg_emit_texture_page_fb_treeline_real(TG_Buf *out, int variant)
{
    const int vi   = (variant < 0 || variant >= 4) ? 0 : variant;
    const int vsrc = (k_r8_treeline_var[vi].src < k_real_tree_count)
                   ? k_r8_treeline_var[vi].src : TG_TREELINE_SRC;
    const int vcell = k_r8_treeline_var[vi].cell;
    const unsigned char *sidx = k_real_tree_idx[vsrc];
    const unsigned char *spal = k_real_tree_pal[vsrc];
    const int paln = k_real_tree_paln[vsrc];
    const int wy   = tg_treeline_src_window(sidx);
    const int fill = tg_treeline_src_fill(sidx, wy);
    unsigned int rng = k_r8_treeline_var[vi].seed;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln * 3; i++) tg_put_u8(out, spal[i]);

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* Crown line: 9-texel cells, each a crown of its own height, rounded
         * off at its shoulders, so the top edge is lumpy like a real canopy
         * instead of a straight cut. Keyed above it, foliage below. */
        const unsigned int c = (unsigned)(x / vcell) * 2654435761u;
        const int crown = 12 + (int)((c >> 28) % 10);          /* 12..21 */
        const int xm    = x % vcell;
        const int top   = crown + ((xm < 2 || xm > vcell - 3) ? 3 : 0);
        int cut, span, sx, sy, v, t;

        rng = rng * 1103515245u + 12345u;
        cut = top - (int)((rng >> 22) % 3);                    /* 10..24 */
        if (y < cut) { tg_put_u8(out, 0); continue; }

        /* Nine contiguous source columns per crown cell, each cell starting
         * somewhere else across the page, so the band does not repeat one
         * tree's silhouette along the whole horizon. Vertically the band's
         * crown maps to the window's crown, keeping the source's own top-lit
         * gradient instead of inventing one. */
        span = TD5_TG_TEX_DIM - 1 - cut;
        if (span < 1) span = 1;
        sx = (int)(((c >> 8) + (unsigned)(x % vcell)) % (unsigned)TD5_TG_TEX_DIM);
        sy = wy + ((y - cut) * (TG_TREELINE_WIN - 1)) / span;

        /* A sample can land in a keyed gap between the source tree's leaves;
         * step across the window until it finds canopy, and fall back to the
         * dominant colour rather than punching a hole in the backdrop. */
        v = 0;
        for (t = 0; t < 8 && v == 0; t++) {
            const int rx = (sx + t * 5) & (TD5_TG_TEX_DIM - 1);
            const int ry = wy + ((sy - wy + t) % TG_TREELINE_WIN);
            v = sidx[ry * TD5_TG_TEX_DIM + rx];
        }
        tg_put_u8(out, (unsigned)(v ? v : fill));
    }
}

/* Synthetic fallback, used only with TD5RE_AUTOTRACK_REAL_TEX=0. Kept as it
 * was reported so the =0 side stays a faithful "before" to compare against. */
static void tg_emit_texture_page_fb_treeline_proc(TG_Buf *out)
{
    unsigned int rng = 0x77E1B3C5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key, 1..7 shadowed mass, 8..15 sunlit crowns. A distant
     * treeline is desaturated and blue-shifted by haze, which is what keeps it
     * reading as BACKGROUND rather than a second row of trees. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)     { b = 255; g = 0;          r = 255; }
        else if (i < 8) { b = 64 + i * 4; g = 62 + i * 5; r = 44 + i * 3; }
        else            { b = 88 + i * 2; g = 92 + i * 4; r = 66 + i * 3; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* Crown line: 9-texel cells, each a crown of its own height, rounded
         * off at its shoulders, so the top edge is lumpy like a real canopy
         * instead of a straight cut. Keyed above it, foliage below. */
        const unsigned int c = (unsigned)(x / 9) * 2654435761u;
        const int crown = 12 + (int)((c >> 28) % 10);          /* 12..21 */
        const int top   = crown + (((x % 9) < 2 || (x % 9) > 6) ? 3 : 0);
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < top - (int)((rng >> 22) % 3)) { tg_put_u8(out, 0); continue; }
        /* Lit at the crowns where the sun hits, darker down in the mass. */
        idx = (y < top + 8) ? (8 + (int)((rng >> 16) % 8))
                            : (1 + (int)((rng >> 16) % 7));
        tg_put_u8(out, (unsigned)idx);
    }
}

static void tg_emit_texture_page_fb_treeline(TG_Buf *out, int variant)
{
    if (tg_real_textures_enabled()) tg_emit_texture_page_fb_treeline_real(out, variant);
    else                            tg_emit_texture_page_fb_treeline_proc(out);
}

/* ---- [R8 TERRAIN item 16] SNOW APPEARANCE ----
 * "if there's snow the median should be snowy too but a different texture, and
 * use different snow ground textures too."
 *
 * Two distinct requests, and the second one names the defect precisely: there
 * has only ever been ONE snow page (TD5_TG_PAGE_SNOW), used for every snow
 * ground quad on the track and for the snow biome's far ridge flank as well, so
 * a whole ALPINE run is a single 64x64 tile repeated to the horizon.
 *
 * VARIANTS differ in the two things that read at ground scale: the coarse drift
 * mask's cell size (how big the shaded patches are) and how much of the page is
 * shade rather than sunlit crust. All three keep the same blue-grey-to-white
 * palette as TD5_TG_PAGE_SNOW, so neighbouring biome cells on different variants
 * do not show a colour seam where they meet -- only a texture-scale change.
 *
 * The MEDIAN page is deliberately NOT one of these. A median is ploughed or
 * walked snow between two carriageways: harder, greyer, with a directional
 * grain rather than drift patches, and darker than the open field so it reads as
 * a separate surface rather than a continuation of the ground the user is
 * complaining reads as one flat sheet. */
static void tg_emit_texture_page_r8_snow_ground(TG_Buf *out, int variant)
{
    /* cell = drift patch size in texels; shade = 1-in-N patches shadowed. */
    static const struct { int cell, shade; unsigned seed; } k_var[3] = {
        {  4, 3, 0x51A7C001u },   /* fine granular crust                */
        { 16, 4, 0x51A7C002u },   /* broad wind drifts                  */
        {  8, 2, 0x51A7C003u }    /* heavily shadowed, hummocky         */
    };
    const int vi = (variant < 0 || variant >= 3) ? 0 : variant;
    unsigned int rng = k_var[vi].seed;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, identical ramp to TD5_TG_PAGE_SNOW: 0..5 blue-grey shadow,
     * 6..15 up to near-white sunlit crust. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 6) { b = 208 + i * 6; g = 198 + i * 7; r = 188 + i * 8; }
        else       { b = 244 + (i - 6); g = 240 + (i - 6); r = 236 + (i - 6) * 2; }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const unsigned int patch = (unsigned)((x / k_var[vi].cell)
                                 + (y / k_var[vi].cell) * 11) * 2654435761u;
        int idx;
        /* [R12 TEX item 8b] "the snow FLOOR reads as tiles -- do not use that
         * texture". It is not the texture, it is this mask. `patch >> 30` has
         * four values, so `% 2` shades HALF the cells and `% 3` shades two in
         * four -- a hard 50/50 decision per axis-aligned cell, decoded (page
         * 373 in the TEXDUMP) as a literal chequerboard of 8x8-texel squares,
         * tiled once per 1500-unit span by tg_emit_ground. The drift INTENT is
         * right and the palette is right; the mask has to stop being a grid.
         * Threshold picked per variant to land near the old shaded fraction. */
        const int shaded = tg_r12_tex_blotch_on()
                         ? (tg_r12_blotch(x, y, k_var[vi].cell * 4 > 32
                                                ? 32 : k_var[vi].cell * 4,
                                          k_var[vi].seed)
                            < (k_var[vi].shade >= 4 ? 96 : 120))
                         : (((patch >> 30) % (unsigned)k_var[vi].shade) == 0);
        rng = rng * 1103515245u + 12345u;
        idx = 6 + (int)((rng >> 16) % 10);                    /* sunlit crust */
        if (shaded)
            idx = (int)((rng >> 18) % 6);                     /* drift shade  */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Ploughed / trodden snow for a snow-biome median: a darker, greyer ramp than
 * the ground page and a LONGITUDINAL grain instead of drift patches, so the
 * strip between the carriageways reads as a different surface at a glance. */
static void tg_emit_texture_page_r8_snow_median(TG_Buf *out)
{
    unsigned int rng = 0x51A7DEEDu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Compressed range and a cooler cast: 0..7 grey slush, 8..15 dull white.
     * Peak is 226 against the ground page's 255, which is the whole point --
     * a median must not be the brightest thing beside the road. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 8) { b = 152 + i * 5; g = 146 + i * 5; r = 140 + i * 5; }
        else       { b = 196 + (i - 8) * 4; g = 192 + (i - 8) * 4;
                     r = 186 + (i - 8) * 4; }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* Grain runs along v (the median's length): banded in x, near-constant
         * in y, so a long thin strip does not tile visibly across its width. */
        const unsigned int band = (unsigned)(x >> 2) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        idx = (int)((band >> 29) % 6) + (int)((rng >> 20) % 3);
        if (((band >> 26) & 7u) == 0) idx += 8;    /* an occasional cleared strip */
        if (idx > TD5_TG_PAL_COUNT - 1) idx = TD5_TG_PAL_COUNT - 1;
        (void)y;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 5] Distant CITY skyline. Same role and format as the treeline page
 * (alpha-keyed, keyed above the roofline) but the silhouette is blocky building
 * tops rather than a forest crown, so an urban biome's far ridge reads as a city
 * skyline. Hazy cool-grey so it sits BACK like the treeline does, not as a wall
 * of near buildings. Tiles once per span along the band, exactly as the treeline
 * page does, so it shares that page's proven horizontal behaviour. */
static void tg_emit_texture_page_fb_skyline(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                  /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0 = key; 1..7 hazy building bodies (blue a touch high, low
     * saturation, so the skyline recedes); 8..15 brighter lit windows / sunlit
     * walls. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)     { b = 255; g = 0;          r = 255; }
        else if (i < 8) { b = 96 + i * 4; g = 92 + i * 3; r = 86 + i * 3; }
        else            { b = 150 + (i - 8) * 6; g = 148 + (i - 8) * 6;
                          r = 138 + (i - 8) * 5; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;      /* y=0 is the TOP of the page */
        /* ~8-column building blocks, each a flat roof at its own height, so the
         * skyline is a stepped run of low-rise and towers. Keyed above the roof,
         * facade below. */
        const unsigned int c = (unsigned)(x / 8) * 2654435761u;
        int roof = 8 + (int)((c >> 27) % 32);              /* 8..39 rows down */
        int body, win;
        if (((c >> 20) & 7u) == 0u) roof = 3 + (int)((c >> 23) % 5); /* tower */
        if (y < roof) { tg_put_u8(out, 0); continue; }     /* sky above roof */
        /* Facade: a window grid on the lit tones over a per-block body grey.
         * Windows every 4th column (offset per block) and every 4th row down. */
        body = 2 + (int)((c >> 8) % 5);                    /* 2..6 body grey */
        win  = (((unsigned)x & 3u) == ((c >> 4) & 3u)) &&
               (((y - roof) & 3) == 1);
        tg_put_u8(out, (unsigned)(win ? (9 + (int)((c >> 12) % 6)) : body));
    }
}

/* Tunnel lining -- deliberately NOT a building facade: no windows, no storey
 * grid, no straight bright lines at all.
 *
 * Two facts force that. First, the lining is drawn by tg_emit_box_mesh, which
 * sets UV = 2*half/tile and so TILES the page over a face several thousand
 * world units long: a page with any strong axis-aligned feature repeats it as a
 * visible ladder, and shears it the moment the box follows a curve or a grade.
 * Second, the page it used to borrow (TD5_TG_PAGE_WALL) is the city facade --
 * under TD5RE_AUTOTRACK_REAL_TEX a photographic office frontage -- which is why
 * tunnel interiors read as building windows.
 *
 * So: damp cast concrete. A narrow cool-grey ramp plus a LOW-FREQUENCY patch
 * mask (the trick the rail page already uses) so the darker damp stains CLUMP
 * into blotches instead of speckling per texel. Every feature is isotropic, so
 * it tiles and stretches with nothing for the eye to lock onto. */
static void tg_emit_texture_page_fb_tunnel(TG_Buf *out)
{
    unsigned int rng = 0x7011u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..10 cool concrete greys (blue channel a touch high, so the lining
     * stays distinct from the warm-grey ground page); 11..15 darker damp/soot
     * stains for the patch mask below. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 11) ? (92 + i * 4) : (66 - (i - 11) * 9);
        if (v < 0) v = 0;
        tg_put_u8(out, (unsigned)(v + 6 < 255 ? v + 6 : 255));  /* B, cooler */
        tg_put_u8(out, (unsigned)v);                            /* G */
        tg_put_u8(out, (unsigned)v);                            /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* 8x8-texel cells hashed to a patch id: blotches ~1/8 of the page wide,
         * which at the 3000-unit tile the walls use is a stain a car length
         * across. No axis-aligned run survives, so nothing shears. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < 2)
            idx = 11 + (int)((rng >> 16) % 5);          /* damp / soot stain */
        else
            idx = (int)((rng >> 16) % 11);              /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Extra tunnel LININGS for item 16a: same isotropic cast-concrete recipe as the
 * base page above (no axis-aligned feature, so nothing shears as a bore curves),
 * but each variant shifts the grey ramp and its warmth and its stain density so
 * consecutive tunnels plainly differ. variant is 1..4; variant 0 is the base
 * page and is not generated here. The palette/texel structure is identical to
 * tg_emit_texture_page_fb_tunnel so the two pages sit at the same tone family --
 * a tunnel network, not four unrelated boxes. */
static void tg_emit_texture_page_tunnel_var(TG_Buf *out, int variant)
{
    /* vi indexes the tweak tables. Per variant: grey-ramp start, R/G/B tint
     * offsets (warm sand / cool soot / pale new / damp green-blue) and a stain
     * frequency (higher = more damp blotching). */
    static const int base_lo[4]  = { 100,  74, 122,  90 };
    static const int dR[4]       = {  16,   0,   4, -10 };
    static const int dG[4]       = {   4,  -2,   2,   6 };
    static const int dB[4]       = { -10,   6,   0,  14 };
    static const unsigned bl[4]  = {   2u,  3u,  1u,  2u };
    const int vi = (variant >= 1 && variant <= 4) ? variant - 1 : 0;
    unsigned int rng = 0x7011u + (unsigned)variant * 0x9E3779B1u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 11) ? (base_lo[vi] + i * 4)
                         : (base_lo[vi] - 30 - (i - 11) * 9);
        int b = v + dB[vi], g = v + dG[vi], r = v + dR[vi];
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < bl[vi])
            idx = 11 + (int)((rng >> 16) % 5);          /* damp / soot stain */
        else
            idx = (int)((rng >> 16) % 11);              /* concrete grain */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8c] Proper cast-concrete bore lining, replacing the isotropic
 * grain that read in frame as a grey checkerboard placeholder. A real road-tunnel
 * lining has a clear ROAD-LEVEL DATUM -- a dark dado/kerb band at the bottom with
 * a bright reflective strip just above it -- and light concrete PANELS divided by
 * thin recessed joints above that. The datum keys on the V axis, which in the
 * swept mesh is HEIGHT up the wall (row y=0 = v=0 = road level), so it stays put
 * as the bore curves; the panel joints run both axes but stay low-contrast so a
 * curving segment cannot shear a hard line. variant 0..3 shift tone/warmth and
 * panel pitch so consecutive bores plainly differ (item 16a lives on here). */
static void tg_emit_texture_page_r6_tunnel_lining(TG_Buf *out, int variant)
{
    static const int base_lo[4] = { 150, 134, 168, 122 }; /* light-concrete floor */
    static const int warm[4]    = {  10,  -6,   2,  -3 };  /* +R/-B = warm, -R/+B = cool */
    static const int pitch[4]   = {  16,  16,  21,  13 };  /* panel size, texels */
    const int vi = (variant >= 0 && variant <= 3) ? variant : 0;
    const int lo = base_lo[vi];
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..9 light concrete ramp; 10 bright reflective strip; 11 recessed joint;
     * 12..13 dark dado/kerb; 14..15 mid soot stain. BGR, R vs B tilt = warmth. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v, b, g, r;
        if      (i <= 9)  v = lo + i * 3;
        else if (i == 10) v = 236;
        else if (i == 11) v = lo - 58;
        else if (i <= 13) v = lo - 80;
        else              v = lo - 34;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - warm[vi]; g = v; r = v + warm[vi];
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int p = pitch[vi];
        int idx;
        if (y <= 8)                             /* road-level dado band */
            idx = 12 + ((x ^ y) & 1);
        else if (y <= 10)                       /* reflective strip above dado */
            idx = 10;
        else if ((x % p) == 0 || (y % p) == 0)  /* recessed panel joints */
            idx = 11;
        else {                                  /* concrete panels, faint variation */
            unsigned int h = (unsigned)((x / p) + (y / p) * 7) * 2654435761u;
            idx = (int)((h >> 28) % 8);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8b] Portal header for the run mouths. The lintel samples only
 * v 0..0.17 (a short band above the opening), so the design lives in the low
 * rows: a bright reflective lower lip that meets the mouth top, then a lit
 * concrete face. Reads as a proper portal beam instead of the washed-out white
 * lining the swept mouth used before. */
static void tg_emit_texture_page_r6_tunnel_portal(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..11 warm-grey concrete ramp; 12 bright reflective lip; 13..15 dark trim. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v, b, g, r;
        if      (i <= 11) v = 120 + i * 8;
        else if (i == 12) v = 242;
        else              v = 70 - (i - 13) * 16;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - 8; g = v; r = v + 8;             /* faintly warm */
        b = b < 0 ? 0 : b;
        r = r > 255 ? 255 : r;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        if (y <= 2)                     idx = 13 + (y % 3);   /* dark shadow lip top */
        else if (y <= 5)                idx = 12;             /* bright reflective lip */
        else if ((x % 32) < 1)          idx = 13;             /* pier joint */
        else                            idx = 5 + ((x >> 4) & 3) + ((y >> 4) & 1);
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 TUNNEL item 8d] Emissive wall-lamp strip. The engine has no interior
 * lighting, so a "lamp" is a bright textured quad drawn with a warm-white vertex
 * colour: a hot core tube with a small dark housing top and bottom. Tiled along
 * the bore top on both walls it gives the run a run of side lights. */
static void tg_emit_texture_page_r6_tunnel_lamp(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0 dark housing; 1..3 warm halo ramp; 4..15 hot white-yellow core. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i == 0)      { b = 24;  g = 22;  r = 20; }        /* housing */
        else if (i <= 3) { b = 120 + i * 20; g = 150 + i * 24; r = 170 + i * 26; }
        else             { b = 210; g = 244; r = 255; }       /* hot core */
        b = b > 255 ? 255 : b; g = g > 255 ? 255 : g; r = r > 255 ? 255 : r;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        if (y <= 6 || y >= 57)      idx = 0;                  /* housing top/bottom */
        else if (y <= 12 || y >= 51) idx = 1 + ((y ^ i) & 2); /* warm halo */
        else                        idx = 4 + (i & 11);       /* hot core */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Bridge DECK surface (item 11). The road quad maps this page v = si + f, one
 * page repeat per span, u = 0..lanes across -- so a page with any longitudinal
 * (lane-paint) feature would tile down the deck as a stack of identical road
 * tiles, which is exactly the "tiled road on a bridge" report. Two rules follow:
 *   - ACROSS the deck (u): plain cast concrete, NO lane markings.
 *   - ALONG the deck (v): one dark EXPANSION-JOINT band per page repeat, i.e.
 *     one transverse seam per span, which is what a real segmented deck shows
 *     and reads as structure rather than tarmac. The seam is transverse, so it
 *     only stays straight because bridge runs are kept near-straight (item 15). */
static void tg_emit_texture_page_bridge_deck(TG_Buf *out)
{
    unsigned int rng = 0xBD9C5A11u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..12 neutral mid concrete greys, 13..15 dark joint/shadow. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 13) ? (120 + i * 4) : (58 - (i - 13) * 12);
        if (v < 0) v = 0;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* The expansion joint: the bottom 3 rows (v just past the span join) go
         * dark, plus a 1-row soft shadow. A darker slab per ~8 texels elsewhere
         * so the concrete is not a flat sheet. No vertical (lane) structure. */
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 9) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 3)              idx = 13 + (int)((rng >> 16) % 3);  /* joint    */
        else if (y == 3)        idx = 12;                          /* shadow   */
        else if ((patch >> 30) == 0)
                                idx = 8 + (int)((rng >> 16) % 4);  /* stain    */
        else                    idx = (int)((rng >> 16) % 12);     /* concrete */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 16b] GUARDRAIL face for the sloped bridge parapet. Its quad maps
 * u across the panel HEIGHT and v along the road, so this page is authored in
 * those axes (unlike TD5_TG_PAGE_RAIL, an armco meant to lie horizontally, which
 * sampled rotated here and read as noise). page-X therefore IS the barrier
 * height (0 = deck, 63 = top) and page-Y runs along the road:
 *   - a solid pale-concrete barrier body,
 *   - a darker METAL CAP RAIL across the top rows (constant height),
 *   - a shadow groove at mid height,
 *   - vertical expansion JOINTS at a road interval (bands in page-Y). */
static void tg_emit_texture_page_r4_guardrail(TG_Buf *out)
{
    unsigned int rng = 0x51E7A113u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..3 dark steel (cap rail / groove), 4..11 pale concrete body,
     * 12..15 sunlit concrete highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 4)       { b = 96 + i * 6;  g = 98 + i * 6;  r = 100 + i * 6; }
        else if (i < 12) { b = 168 + (i-4) * 6; g = 170 + (i-4) * 6;
                           r = 172 + (i-4) * 6; }
        else             { b = 224 + (i-12) * 6; g = 226 + (i-12) * 6;
                           r = 228 + (i-12) * 6; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 22) < 2)          idx = 1 + (int)((rng >> 16) % 3); /* joint  */
        else if (x >= 55)          idx = (int)((rng >> 16) % 4);     /* cap    */
        else if (x >= 50)          idx = 12 + (int)((rng >> 16) % 4);/* cap lip*/
        else if (x >= 30 && x <= 32) idx = 1;                        /* groove */
        else                       idx = 4 + (int)((rng >> 16) % 8); /* body   */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R9 BRIDGE item 9] PYLON -- the ABOVE-deck verticals (gantry legs, tower
 * legs). "The texture for these pillars should be different."
 *
 * The complaint is real and its cause is that they were on the PIER page: a
 * bridge whose tower is the same cast concrete as the pier reads as one
 * extruded lump, and the checkerboard blotching visible in the report's own
 * screenshot is that page sampled across a narrow post. This is painted STEEL
 * instead -- a warm dark grey-green with strong VERTICAL ribs (page-X, i.e.
 * around the post at the 600-unit pier tile) and rivet rows across, so the
 * member reads as fabricated and as a different material from the concrete it
 * stands on. That material change at the waterline is exactly what makes a real
 * bridge legible as "one structure, two parts". */
static void tg_emit_texture_page_r9_pylon(TG_Buf *out)
{
    unsigned int rng = 0x2C71A9D3u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..3 deep shadow between ribs, 4..10 painted steel body,
     * 11..15 rib highlight / rivet head. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 4)       { b = 40 + i * 5;      g = 46 + i * 5;      r = 44 + i * 5; }
        else if (i < 11) { b = 86 + (i-4) * 5;  g = 96 + (i-4) * 5;  r = 92 + (i-4) * 5; }
        else             { b = 138 + (i-11) * 7; g = 150 + (i-11) * 7;
                           r = 145 + (i-11) * 7; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* around the post */
        const int y = i / TD5_TG_TEX_DIM;   /* up the post     */
        const int rib = x % 16;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (rib < 2)                idx = (int)((rng >> 16) % 4);      /* groove */
        else if (rib == 2 || rib == 13)
                                    idx = 11 + (int)((rng >> 16) % 3); /* rib lip */
        else if ((y % 21) < 2 && (rib == 5 || rib == 10))
                                    idx = 13 + (int)((rng >> 16) % 3); /* rivet  */
        else                        idx = 4 + (int)((rng >> 16) % 7);  /* paint  */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R9 BRIDGE item 9] BEAM -- the HORIZONTAL members (gantry cross-beam, tower
 * cross-member). "The horizontal beam should have a different texture as well."
 *
 * Its box is 3000-tiled along the road, so page-X runs LENGTHWISE along the beam
 * and page-Y across its depth. The pier page is authored for a vertical column
 * and, drawn in these axes, gave the mottled checkerboard the screenshot shows
 * on the beam. This is a plate girder seen side-on: continuous dark FLANGE bands
 * top and bottom, a lighter web between, and regular stiffener ribs and bolt
 * rows down the length -- structure that runs the right way. */
static void tg_emit_texture_page_r9_beam(TG_Buf *out)
{
    unsigned int rng = 0x77B3E509u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..4 dark flange steel, 5..11 mid web, 12..15 stiffener highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 5)       { b = 52 + i * 5;      g = 58 + i * 5;      r = 60 + i * 5; }
        else if (i < 12) { b = 108 + (i-5) * 6; g = 116 + (i-5) * 6; r = 118 + (i-5) * 6; }
        else             { b = 160 + (i-12) * 8; g = 168 + (i-12) * 8;
                           r = 170 + (i-12) * 8; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* along the beam  */
        const int y = i / TD5_TG_TEX_DIM;   /* across its face */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 9 || y > 54)        idx = (int)((rng >> 16) % 5);      /* flange  */
        else if (y < 12 || y > 51)  idx = 12 + (int)((rng >> 16) % 4); /* fillet  */
        else if ((x % 18) < 3)      idx = 12 + (int)((rng >> 16) % 4); /* stiffener */
        else if ((x % 18) == 9 && ((y - 12) % 13) < 2)
                                    idx = 1 + (int)((rng >> 16) % 3);  /* bolt    */
        else                        idx = 5 + (int)((rng >> 16) % 7);  /* web     */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R9 BRIDGE item 9] SHORE -- the coastline band where the river meets the
 * bank. "The coastline geometry looks wrong."
 *
 * The R4 coast page it replaces is a WATER-tiled page shared with the river, so
 * the band read as more water lying at a wrong angle rather than as a shore.
 * This one is authored for the band's own axes -- page-Y runs from the WATERLINE
 * (y=0) up the beach to the BANK (y=63) -- so the surface actually grades: wet
 * dark shingle at the water, coarse pebble, then dry sand blending into bank
 * grass at the top. */
static void tg_emit_texture_page_r9_shore(TG_Buf *out)
{
    unsigned int rng = 0x4D19C7B7u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..3 wet dark shingle, 4..8 pebble, 9..12 dry sand, 13..15 grass. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (i < 4)       { b = 74 + i * 5;      g = 76 + i * 5;      r = 70 + i * 5; }
        else if (i < 9)  { b = 118 + (i-4) * 7; g = 124 + (i-4) * 7; r = 122 + (i-4) * 7; }
        else if (i < 13) { b = 150 + (i-9) * 8; g = 172 + (i-9) * 8; r = 190 + (i-9) * 8; }
        else             { b = 58 + (i-13) * 8; g = 112 + (i-13) * 10;
                           r = 54 + (i-13) * 8; }
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int y = i / TD5_TG_TEX_DIM;   /* waterline (0) -> bank (63) */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 14)      idx = (int)((rng >> 16) % 4);            /* wet shingle */
        else if (y < 32) idx = 4 + (int)((rng >> 16) % 5);        /* pebble      */
        else if (y < 50) idx = 9 + (int)((rng >> 16) % 4);        /* dry sand    */
        else if (y < 56) idx = ((rng >> 20) & 1u) ? 12 : 13;      /* sand/grass  */
        else             idx = 13 + (int)((rng >> 16) % 3);       /* bank grass  */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 18] PIER / TOWER concrete. Smooth cast concrete for a LIT exterior:
 * a tight pale-grey ramp with faint vertical form-board seams and only sparse
 * grain -- deliberately none of the damp/soot blotching the bore LINING page
 * carries, which read as a checkerboard on a bright tower. */
static void tg_emit_texture_page_r4_pier(TG_Buf *out)
{
    unsigned int rng = 0x9A31C7E5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. 0..11 a narrow pale-concrete ramp, 12..15 faint form-seam shadow. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (176 + i * 5) : (150 - (i - 12) * 10);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 3 ? v - 3 : 0));   /* faintly warm grey */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 4) + (y >> 4) * 5) * 2654435761u;
        int idx;
        /* [R12 TEX item 12b] The 16-texel cell mask below banded the whole page
         * into four columns, and at the 600-unit pier tile that grid repeats up
         * a leg as stacked squares -- the "tile texture" on the pillars. The
         * form-board SEAMS stay (they are the material); only the grain patches
         * move to the smooth mask. */
        const int grain = tg_r12_tex_blotch_on()
                        ? (tg_r12_blotch(x, y, 16, 0x9A31C7E5u) < 116)
                        : ((patch >> 30) == 0);
        rng = rng * 1103515245u + 12345u;
        if ((x % 16) == 0)          idx = 12 + (int)((rng >> 16) % 3); /* seam */
        else if (grain)             idx = 6 + (int)((rng >> 16) % 3);  /* grain*/
        else                        idx = (int)((rng >> 16) % 12);     /* face */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 item 20] COASTLINE / shore band where the bridge water meets the bank.
 * Warm sand grading into darker wet sand / shingle at the waterline. Isotropic
 * and line-free like the GROUND page, so it never shears where the shore slopes;
 * the wet band is a low-frequency patch, not an axis-aligned edge. */
static void tg_emit_texture_page_r4_coast(TG_Buf *out)
{
    unsigned int rng = 0xC0A57011u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, warm: r > g > b. 0..10 dry sand ramp, 11..15 darker wet sand/shingle. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int base = (i < 11) ? (150 + i * 8) : (120 - (i - 11) * 12);
        if (base < 0) base = 0;
        if (base > 255) base = 255;
        tg_put_u8(out, (unsigned)(base > 40 ? base - 40 : 0));   /* b */
        tg_put_u8(out, (unsigned)(base > 18 ? base - 18 : 0));   /* g */
        tg_put_u8(out, (unsigned)base);                          /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 7) * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((patch >> 29) < 2u)
            idx = 11 + (int)((rng >> 16) % 5);           /* wet sand / shingle */
        else
            idx = (int)((rng >> 16) % 11);               /* dry sand grain     */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R5 item 17a] ARMCO steel guardrail face. The R4 page reads as a concrete
 * barrier ("still not a guardrail texture"); this is the galvanized W-beam the
 * user expects: cool blue-grey steel with the two horizontal corrugation ridges
 * of a W-section, a dark rail cap, and vertical posts at a road interval.
 *
 * Same axes as the R4 page so the panel UVs need no change: page-X is the
 * barrier HEIGHT (0 = deck .. 63 = top), page-Y runs ALONG the road. The two
 * beam ridges are therefore constant-X bands and the posts are constant-Y bands. */
static void tg_emit_texture_page_r5_guardrail(TG_Buf *out)
{
    unsigned int rng = 0x7A5CE001u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR galvanized steel: cool (b >= g >= r). 0..3 dark groove/post shadow,
     * 4..11 mid steel ramp, 12..15 bright spangle highlight. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v;
        if (i < 4)       v = 70 + i * 10;            /* dark groove / shadow */
        else if (i < 12) v = 130 + (i - 4) * 9;      /* mid steel            */
        else             v = 210 + (i - 12) * 11;    /* bright spangle       */
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                             /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 6 ? v - 6 : 0));          /* g           */
        tg_put_u8(out, (unsigned)(v > 12 ? v - 12 : 0));        /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 20) < 3)                    /* vertical post, full height     */
            idx = 1 + (int)((rng >> 16) % 2);
        else if (x >= 58)                    /* dark rail cap along the top    */
            idx = (int)((rng >> 16) % 2);
        else if ((x >= 22 && x <= 26) || (x >= 40 && x <= 44))
            idx = 12 + (int)((rng >> 16) % 4);   /* the two W-beam ridge crests */
        else if (x == 33 || x < 8)           /* centre valley + skirt shadow    */
            idx = 1 + (int)((rng >> 16) % 3);
        else                                 /* steel body, faint spangle       */
            idx = 4 + (int)((rng >> 16) % 8);
        tg_put_u8(out, (unsigned)idx);
    }
}

static int tg_r13_rail_stack(void)
{
    return td5_env_flag_on("TD5RE_R13_RAIL_STACK");
}

/* One texel of the stacked profile. `rng` is the caller's already-advanced
 * stream, so switching the knob does not reshuffle the rest of the page. */
static int tg_r13_rail_stack_idx(int x, int y, unsigned int rng)
{
    const int post = (y % 18) < 3;
    int v;

    if (x >= TD5_TG_R13_RAIL_LO0 && x <= TD5_TG_R13_RAIL_LO1) {
        /* Pressed lips read as shadow, so the section has a defined edge
         * instead of fading into the air around it. */
        if (x == TD5_TG_R13_RAIL_LO0 || x == TD5_TG_R13_RAIL_LO1)
            return 1 + (int)((rng >> 16) % 3);
        {   const int d1 = x > TD5_TG_R13_RAIL_LOA
                         ? x - TD5_TG_R13_RAIL_LOA : TD5_TG_R13_RAIL_LOA - x;
            const int d2 = x > TD5_TG_R13_RAIL_LOB
                         ? x - TD5_TG_R13_RAIL_LOB : TD5_TG_R13_RAIL_LOB - x;
            v = 12 - ((d1 < d2) ? d1 : d2);
        }
    } else if (x >= TD5_TG_R13_RAIL_HI0 && x <= TD5_TG_R13_RAIL_HI1) {
        if (x == TD5_TG_R13_RAIL_HI0 || x == TD5_TG_R13_RAIL_HI1)
            return 1 + (int)((rng >> 16) % 3);
        v = 13 - (x > TD5_TG_R13_RAIL_HIC
                ? x - TD5_TG_R13_RAIL_HIC : TD5_TG_R13_RAIL_HIC - x);
    } else if (post && x <= TD5_TG_R13_RAIL_POST) {
        return 1 + (int)((rng >> 16) % 3);
    } else {
        return 0;                        /* transparent -- this is the point */
    }

    if (v < 4)  v = 4;                   /* steel body, never post-dark      */
    if (v > 15) v = 15;
    if (v > 4 && ((rng >> 16) & 3u) == 0u) v--;   /* one step of grain       */
    return v;
}

static void tg_emit_texture_page_r6_guardrail_alpha(TG_Buf *out)
{
    unsigned int rng = 0x6A1CD00Du;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 1);                                    /* 1 = alpha-keyed */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* index 0 = transparent key (magenta, never drawn); 1..3 dark steel edge/
     * post shadow, 4..11 mid galvanized steel, 12..15 bright spangle. Cool
     * (b >= g >= r) like the R5 armco so lit/shadowed rails still match. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v;
        if (i == 0) { tg_put_u8(out, 255); tg_put_u8(out, 0); tg_put_u8(out, 255); continue; }
        if (i < 4)       v = 74 + i * 10;
        else if (i < 12) v = 132 + (i - 4) * 9;
        else             v = 212 + (i - 12) * 11;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                            /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 6 ? v - 6 : 0));          /* g           */
        tg_put_u8(out, (unsigned)(v > 12 ? v - 12 : 0));        /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;   /* barrier HEIGHT, 0=deck..63=top */
        const int y = i / TD5_TG_TEX_DIM;   /* along the road                 */
        const int post = (y % 18) < 3;            /* vertical post at a pitch  */
        const int beam = (x >= 16 && x <= 52);    /* the W-beam's vertical run */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (tg_r13_rail_stack()) {
            idx = tg_r13_rail_stack_idx(x, y, rng);
        } else if (beam) {
            /* [R12 OVERPASS item 14c] "The bridge guardrails render DOUBLE-
             * FOLDED down the middle -- the rail looks creased/mirrored along
             * its length."
             *
             * THIS PAGE IS THE DEFECT, and its UVs are not. It was worth
             * checking, because the obvious suspect was the R11 item 11 V-flip:
             * the roadside rail was found sampling its page upside down and the
             * bridge parapet is a separate emitter that could have been on the
             * old convention too. It is not. This page declares page-X = barrier
             * HEIGHT (0 = deck, 63 = top) and page-Y = along the road, and
             * tg_emit_bridge_rail_panel maps u across the panel height and v
             * along the road -- they agree exactly. There is no flip and no
             * transposition to fix, and no second rail on the edge either (R9
             * RAILFIX makes the roadside rail yield every deck edge to this
             * parapet, so what the player sees is one quad, once).
             *
             * What the player sees is the PROFILE painted into it. The old bands
             * were two near-WHITE crests (indices 12-15) at x 20-24 and 44-48
             * with a near-BLACK valley (indices 1-2) one texel wide at x = 34
             * between them, and nothing in between: a hard, maximum-contrast,
             * perfectly symmetric pattern about the beam's mid-height. Stretched
             * over a 420-unit barrier and run the length of a bridge, that is
             * not read as a corrugation, it is read as a crease with the rail
             * mirrored above and below it -- the report, verbatim.
             *
             * A real W-beam is one pressed section catching light along two soft
             * ridges, so the fix is to SHADE it rather than to band it: a
             * continuous ramp keyed on the distance from the nearer crest
             * centre, with the valley only a few steps darker than the steel
             * body instead of nine. The section is still symmetric -- a W is --
             * but symmetry stops being the loudest thing in the image, which is
             * what makes it read as one beam.
             *
             * TD5RE_R12_RAIL_WBEAM=0 restores the hard bands for an A/B. */
            if (td5_env_flag_on("TD5RE_R12_RAIL_WBEAM")) {
                /* Distance from the nearer of the two ridge crests (22 and 46),
                 * 0 at a crest and 12 at the beam's outer edges. */
                const int d1 = x > 22 ? x - 22 : 22 - x;
                const int d2 = x > 46 ? x - 46 : 46 - x;
                const int d  = (d1 < d2) ? d1 : d2;
                int v = 13 - d;                     /* 13 at a crest, 1 at 12 out */
                if (v < 3) v = 3;                   /* never as dark as a post    */
                if (v > 15) v = 15;
                /* One step of grain, so the steel is not a flat gradient. */
                if (v > 3 && ((rng >> 16) & 3u) == 0u) v--;
                idx = v;
            } else if ((x >= 20 && x <= 24) || (x >= 44 && x <= 48))
                idx = 12 + (int)((rng >> 16) % 4);   /* the two W ridge crests */
            else if (x == 34 || x == 16 || x == 52)
                idx = 1 + (int)((rng >> 16) % 2);    /* centre valley + edges  */
            else
                idx = 4 + (int)((rng >> 16) % 8);    /* steel body             */
        } else if (post) {
            idx = 1 + (int)((rng >> 16) % 3);        /* post above/below beam  */
        } else {
            idx = 0;                                 /* transparent air        */
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 item 17] STEEL pier/tower face -- cool blue-grey plate steel with dark
 * I-beam flange shadows down the edges and centre and a rivet grid, so a
 * steel-style crossing reads as a braced viaduct, not cast concrete. Opaque. */
static void tg_emit_texture_page_r6_pier_steel(TG_Buf *out)
{
    unsigned int rng = 0x53EE1002u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 4) ? (60 + i * 10)
              : (i < 12) ? (120 + (i - 4) * 10)
                         : (210 + (i - 12) * 11);
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);                          /* b (coolest) */
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));        /* g           */
        tg_put_u8(out, (unsigned)(v > 10 ? v - 10 : 0));      /* r           */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (x < 6 || x > 57 || (x >= 30 && x <= 33))
            idx = (int)((rng >> 16) % 4);            /* I-beam flange shadow */
        else if ((x % 8) == 0 && (y % 8) == 0)
            idx = 12 + (int)((rng >> 16) % 4);       /* rivet head highlight */
        else
            idx = 4 + (int)((rng >> 16) % 8);        /* web steel            */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R7 item 9] Concrete TUNNEL PORTAL facade. The R6 portal was a single thin
 * lintel beam over the mouth on TD5_TG_PAGE_R6_TUNNEL+4, so the entrance read as
 * "a weird grey texture ... doesn't look like the entrance of a tunnel". Shipped
 * TD5 has no portal-facade sprite to mine: its *TUN*.TGA billboards are interior
 * "light at the end of the tunnel" glows (single/twin arch glows, edge-light
 * streaks) and the mouth surround is darkened track MESH, confirmed by decoding
 * all 12 environs *TUN* pages. So this page is authored to dress a proper facade
 * FRAME (header + jambs, built in tg_emit_tunnel_swept) as cast concrete civil
 * work: a bright coping band along the top, horizontal form-board courses and
 * vertical form-joint lines over a warm-grey field, a weathered darker base.
 * Opaque. y=0 is the TOP of the page (coping), so the frame maps V=0 to its top
 * edge. */
static void tg_emit_texture_page_r7_tunnel_portal(TG_Buf *out)
{
    unsigned int rng = 0x7A11B00Cu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..1 form-joint shadow, 2..12 concrete ramp, 13 bright coping, 14..15 base. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 2)  ? (56 + i * 20)
              : (i < 13) ? (120 + (i - 2) * 11)
              : (i == 13) ? 238
                          : (90 - (i - 14) * 22);
        int b, g, r;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        b = v - 6; g = v; r = v + 8;                        /* faintly warm */
        if (b < 0)   b = 0;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;              /* y=0 is the TOP */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 4)                        idx = 13;    /* coping highlight  */
        else if (y == 4 || y == 5)        idx = 1;     /* shadow under coping */
        else if (y > 58)                  idx = 14 + (int)((rng >> 16) & 1); /* base */
        else if ((y % 20) < 1)            idx = 1;     /* horizontal form course */
        else if ((x % 32) < 1)            idx = 2;     /* sparse vertical joint */
        else idx = 4 + (int)(((unsigned)((x >> 4) + (y >> 4)) + (rng >> 19)) % 7u);
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R8 item 17] BORE FLOOR / tunnel median surface. Grey made concrete with a
 * faint aggregate speckle and no directional structure -- the gore is a
 * horizontal strip seen at a shallow angle down the bore, so any axis-aligned
 * line in the page shears into a stripe (the rule the GROUND page comment sets
 * out). Deliberately darker than the roadside sidewalk page so it sits in the
 * bore's shadowed interior instead of glowing. Opaque. */
static void tg_emit_texture_page_r8_bore_median(TG_Buf *out)
{
    unsigned int rng = 0x8B0DE117u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* One narrow neutral ramp -- dim concrete, cool rather than warm so it
     * reads as indoor surface next to the lit portal. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 74 + i * 5;                       /* 74..149 */
        tg_put_u8(out, (unsigned)(v + 6 > 255 ? 255 : v + 6));   /* b */
        tg_put_u8(out, (unsigned)v);                             /* g */
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));           /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Low-frequency patch + per-texel grain, no grid. */
        idx = 5 + (int)(((rng >> 17) & 7u));
        if (((rng >> 26) & 31u) == 0u) idx = 2;   /* sparse dark aggregate */
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R8 item 4] TUNNEL PORTAL headwall, replacing TD5_TG_PAGE_R7_BRIDGE+0.
 *
 * The complaint sharpened this round from "the mouth looks weird" to a specific
 * one: "I don't want you to use this grey-ish texture on this place, the one
 * with LIGHTER GRAY STRIPES." Dumping the page that was actually being chosen
 * (log/tgpage_301.ppm, TD5RE_R8_TEXDUMP=1) shows exactly that and identifies
 * where the stripes come from -- two independent sources, both in the R7 page:
 *
 *   - a near-white coping band across rows 0..3 with a dark shadow line under
 *     it, and
 *   - a dark form-course line every 20 rows down the whole page.
 *
 * On the header those read as one coping, which is what they were for. On the
 * JAMBS they do not: the jamb face is (bore height 2600 + header 1500) = 4100
 * tall and V was scaled by the 3000-unit world tile, so the page repeated 1.37
 * times up each jamb and the bright coping band reappeared a third of the way
 * up -- a light horizontal stripe across the middle of a concrete pier, twice,
 * on both jambs of every mouth.
 *
 * So this page carries NO axis-aligned banding at all. A portal headwall is one
 * cast pour: low-frequency weathering patches, streaking down from the top
 * where water runs, per-texel grain, and nothing periodic. The coping is now a
 * geometric fact (the header slab's own top edge), not a stripe painted into a
 * tiling page, so it cannot repeat no matter how the face is mapped. Same rule
 * the GROUND page comment states for terrain, applied to a vertical face.
 *
 * NOT the art-mining outcome the round asked for: shipped level textures are
 * unnamed tex_NNN.png and identifying a real portal headwall among them was out
 * of reach this round. This is authored, and it is a page CHOICE change with
 * both pages dumped so the swap is auditable. */
static void tg_emit_texture_page_r8_tunnel_portal(TG_Buf *out)
{
    unsigned int rng = 0xC0117A15u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* One narrow, low-contrast concrete ramp. Narrow ON PURPOSE: the previous
     * page spread 56..238, and that range is what let a band read as a stripe
     * at distance. 96..171 keeps the face legible as concrete while making any
     * residual pattern sub-threshold. Faintly warm, like cast concrete. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 96 + i * 5;
        int b = v - 5, g = v, r = v + 7;
        if (b < 0) b = 0;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Two superimposed low-frequency patch fields at coprime cell sizes, so
         * the sum has no period inside the page and no visible cell grid. */
        idx = 7
            + (int)((((unsigned)(x / 7) * 2654435761u) >> 29) & 3u)
            - (int)((((unsigned)(y / 11) * 2246822519u) >> 29) & 3u);
        /* Vertical weather streaking: slowly varying in x, biased downward, and
         * deliberately NOT a repeating column -- the darkening is a function of
         * a hash of the column, not of x modulo anything. */
        if (((((unsigned)x * 2654435761u) >> 27) & 7u) == 0u && y > 8)
            idx -= 1 + (y * 2) / TD5_TG_TEX_DIM;
        idx += (int)((rng >> 22) & 1u);                  /* per-texel grain */
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* ==========================================================================
 * [R9 TUNNEL] pages. Seven, all dumped and eyeballed via TD5RE_R8_TEXDUMP=1
 * (log/tgpage_NNN.ppm) rather than trusted from the id written into the mesh --
 * that is the R8 lesson, and it is what finally moved this item.
 * ====================================================================== */

/* [item 5c] PORTAL FACE. The fourth page for this surface, so it has to differ
 * from the previous three by DESIGN and not by tone.
 *
 * R7 was banded concrete: rejected as "lighter gray stripes". R8 answered that
 * by removing every periodic feature, giving a flat even pale-grey pour -- and
 * the report came back "the gray-ish texture looks wrong here". R8's page is
 * not a bad concrete page; a flat even pale grey IS what "grey-ish" means, so a
 * fifth attempt at cleaner grey concrete would be the same answer again.
 *
 * So change the material, not the shade. This is BOARD-FORMED concrete: poured
 * against timber shuttering, which leaves vertical board marks and a joint
 * every board width, and it is what real portal headwalls of this era look
 * like. The structure is VERTICAL, which keeps the R8 rule intact -- the thing
 * that produced the stripes was a horizontal feature repeating up a tall face,
 * and a vertical feature on a face mapped V 0..1 cannot repeat at all. Warmed
 * off neutral grey so it separates from the road, the guardrails and the
 * hillside, none of which it should be confusable with. */
static void tg_emit_texture_page_r9_portal_face(TG_Buf *out)
{
    unsigned int rng = 0x9B10A7C3u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Warm buff concrete, r > g > b, and a WIDER ramp than R8's 96..171: the
     * board relief needs contrast to read as relief, and unlike R8's banding
     * there is no horizontal feature here for contrast to turn into a stripe. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 78 + i * 7;
        int b = v - 16, g = v - 4, r = v + 10;
        if (b < 0) b = 0;
        if (g < 0) g = 0;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int board = x / 8;             /* 8 boards across the page */
        const int inb   = x % 8;
        int idx;
        rng = rng * 1103515245u + 12345u;
        /* Each board takes its own tone from a hash of the board index, the way
         * real shuttering does -- no two boards weather alike. */
        idx = 8 + (int)((((unsigned)board * 2654435761u) >> 29) & 3u) - 1;
        if (inb == 0)      idx -= 4;         /* shadowed shutter joint      */
        else if (inb == 1) idx += 2;         /* lit lip beside the joint    */
        /* Vertical grain WITHIN a board: slowly varying down the board, so the
         * face has length without gaining any horizontal feature. */
        idx += (int)((((unsigned)(board * 64 + y / 5) * 2246822519u) >> 30) & 1u);
        idx += (int)((rng >> 23) & 1u);      /* per-texel grain */
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 5a] PORTAL SURROUND -- the crown band above the lintel and the two
 * buttress wings. THE page this item was always about; see the long note in
 * tg_emit_fb_tunnel. Coursed rubble-stone revetment: what a real cutting is
 * faced with either side of a portal, and unmistakably not the mottled green
 * hillside page that has been there for four rounds. Deliberately COARSER and
 * darker than the board-formed face above, so the mouth reads as a concrete
 * portal set into a stone revetment rather than as one continuous slab. */
static void tg_emit_texture_page_r9_portal_surround(TG_Buf *out)
{
    unsigned int rng = 0x5EA10C42u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Cool grey stone with a faint brown cast. 0..2 are the mortar shadow. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int base = (i < 3) ? (48 + i * 9) : (96 + (i - 3) * 11);
        if (base > 255) base = 255;
        tg_put_u8(out, (unsigned)(base > 12 ? base - 12 : 0));  /* b */
        tg_put_u8(out, (unsigned)(base >  6 ? base -  6 : 0));  /* g */
        tg_put_u8(out, (unsigned)base);                         /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* Rubble courses: rows of unequal blocks, each course offset, block
         * widths varying with a hash so no two courses align. */
        const int course = y / 8;
        const unsigned int ch = (unsigned)course * 2654435761u;
        const int cw   = 9 + (int)((ch >> 28) & 3u);        /* 9..12 wide */
        const int ofs  = (int)((ch >> 21) & 15u);
        const int inx  = (x + ofs) % cw;
        const int iny  = y % 8;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (iny == 0 || inx == 0) {
            idx = (int)((rng >> 17) % 3u);                  /* mortar joint */
        } else {
            const unsigned int bh =
                (unsigned)(course * 31 + (x + ofs) / cw) * 2246822519u;
            idx = 6 + (int)((bh >> 29) & 7u);               /* per-block tone */
            idx += (int)((rng >> 19) % 3u) - 1;             /* stone grain    */
            /* Top of each block catches the light -- gives the coursing depth
             * without a page-wide horizontal line, since the block edges are
             * already staggered by ofs. */
            if (iny == 1) idx += 2;
        }
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 5e] BORE CEILING. "walls and roofing should have different texture."
 * A tunnel roof is not a tunnel wall: it carries transverse precast segments
 * with a joint between each, and it is darker because nothing lights it. This
 * page's ribs run across V, and the roof quad maps V across the bore WIDTH, so
 * the ribs cross the road the way real segment joints do. */
static void tg_emit_texture_page_r9_bore_ceiling(TG_Buf *out)
{
    unsigned int rng = 0x33C0FFEEu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* Dark, faintly blue -- the enclosure vertex colour is already blue-grey
     * and the ceiling should sit UNDER the walls in value, not beside them. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 40 + i * 6;
        int b = v + 8, g = v + 2, r = v;
        if (b > 255) b = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int rib = y % 16;              /* four segment joints per page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (rib == 0)      idx = 1;          /* the joint itself: near black  */
        else if (rib == 1) idx = 9;          /* lit edge of the next segment  */
        else               idx = 5 + (int)((rng >> 21) % 3u);
        /* Soot streaking along the bore, hashed per column so it does not tile. */
        if (((((unsigned)x * 2654435761u) >> 28) & 7u) == 0u) idx -= 2;
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 13] UNDERPASS ABUTMENT -- the retaining wall carrying the crossing.
 * Board-marked like the portal face, but grey rather than buff and with a
 * coarser board, so a city underpass and a mountain portal share a family
 * without reading as the same object. */
static void tg_emit_texture_page_r9_up_abutment(TG_Buf *out)
{
    unsigned int rng = 0x71B4C0DEu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 84 + i * 8;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int inb = x % 16;              /* wide boards */
        int idx;
        rng = rng * 1103515245u + 12345u;
        idx = 8 + (int)((((unsigned)(x / 16) * 2654435761u) >> 30) & 3u) - 1;
        if (inb == 0) idx -= 5;                          /* shutter joint */
        idx += (int)((((unsigned)(y / 7) * 2246822519u) >> 30) & 1u);
        /* Splash staining up from the road: the bottom rows of a wall beside a
         * carriageway are always dirtier than the top. */
        if (y > TD5_TG_TEX_DIM - 10) idx -= 2;
        idx += (int)((rng >> 23) & 1u);
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 13] UNDERPASS DECK SOFFIT -- the underside of the crossing, and the
 * face a driver going under it actually looks at. Longitudinal precast BEAMS
 * with a shadow gap between them, which is exactly how a short-span road
 * bridge is built and immediately distinguishes it from the bore ceiling's
 * transverse segments. Dark: it is permanently in its own shadow. */
static void tg_emit_texture_page_r9_up_soffit(TG_Buf *out)
{
    unsigned int rng = 0x2D19F0A7u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 34 + i * 7;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int beam = x % 12;             /* beams run along U */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (beam == 0)      idx = 0;         /* shadow gap between beams  */
        else if (beam == 1) idx = 10;        /* lit arris of the next one */
        else                idx = 5 + (int)((rng >> 20) % 3u);
        idx += (int)((((unsigned)(y / 9) * 2654435761u) >> 30) & 1u);
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 13] UNDERPASS DECK TOP -- the highway carriageway overhead. Asphalt
 * with a lane line, so from below and from the approach the thing crossing over
 * is legible as A ROAD. That legibility is the whole request: "underpasses
 * BELOW HIGHWAYS". */
static void tg_emit_texture_page_r9_up_deck(TG_Buf *out)
{
    unsigned int rng = 0x48A2C39Bu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* 0..11 asphalt, 12..15 paint. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (38 + i * 5) : (196 + (i - 12) * 18);
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        idx = 4 + (int)((rng >> 18) % 5u);                  /* asphalt grain */
        /* Centre lane line, dashed along U (which runs along the crossing). */
        if (y >= TD5_TG_TEX_DIM / 2 - 1 && y <= TD5_TG_TEX_DIM / 2 &&
            (x % 20) < 12)
            idx = 13;
        /* Edge lines, solid. */
        if (y < 2 || y > TD5_TG_TEX_DIM - 3) idx = 12;
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [item 13] UNDERPASS PARAPET / DECK FASCIA. Clean precast panel with a
 * recessed drip groove near the bottom -- the detail that makes a fascia read
 * as a fascia. Light, because a parapet is the top edge against the sky. */
static void tg_emit_texture_page_r9_up_parapet(TG_Buf *out)
{
    unsigned int rng = 0x6C0FFA31u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 108 + i * 8;
        if (v > 255) v = 255;
        tg_put_u8(out, (unsigned)(v > 3 ? v - 3 : 0));
        tg_put_u8(out, (unsigned)v);
        tg_put_u8(out, (unsigned)v);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        int idx;
        rng = rng * 1103515245u + 12345u;
        idx = 10 + (int)((rng >> 19) % 3u) - 1;
        /* Panel joint every 32 texels, vertical only. */
        if ((x % 32) == 0) idx -= 5;
        /* Drip groove: one dark row low on the face. This IS a horizontal
         * feature, and it is safe where the R7 coping was not, because a
         * parapet face is mapped once over a 620-unit-tall object -- it can
         * never repeat up a 4100-unit jamb the way the R7 band did. */
        if (y == TD5_TG_TEX_DIM - 12) idx -= 6;
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R11 SIGNS item 16] SIGN POST -- galvanised steel tube, seen side on.
 *
 * WHY THIS PAGE EXISTS AT ALL. The three mined arrow panels come out of the crop
 * with a key of 0.0%: a rectangular sign is a SOLID panel, so there is no
 * transparent margin and nothing in the artwork suggests a support. The
 * pre-existing IP_SIGN sets the precedent of a disc "on a post we do not model"
 * (see k_infra_props), and at a 0.8 m disc tucked against a frontage that reads
 * as an omission. At 0.9 x 1.8 m lifted clear of head height it would read as a
 * blue rectangle hanging in the air, which is worse than no sign. So the post is
 * modelled, and it needs one page of its own.
 *
 * Vertical only: the tube is mapped once over a ~2 m post, so a horizontal
 * feature could not repeat up it, but there is nothing a real galvanised tube
 * has to say horizontally either. A bright edge on one side and a shaded one on
 * the other is what makes an untextured cylinder read as round. */
static void tg_emit_texture_page_r11_sign_post(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, faintly cool: galvanised steel is grey with a blue cast, and a
     * neutral grey post beside a saturated blue panel reads as brown by
     * contrast. 0 darkest .. 15 brightest. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 96 + i * 9;
        if (v > 250) v = 250;
        tg_put_u8(out, (unsigned)(v + 5 > 255 ? 255 : v + 5));   /* B */
        tg_put_u8(out, (unsigned)v);                             /* G */
        tg_put_u8(out, (unsigned)(v > 4 ? v - 4 : 0));           /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        /* Cylindrical shading across the tube: brightest a third of the way in
         * from the lit edge, falling off to both silhouettes. */
        const int d  = (x * 3 >= TD5_TG_TEX_DIM)
                     ? (x * 3 - TD5_TG_TEX_DIM) : (TD5_TG_TEX_DIM - x * 3);
        int idx = 13 - (d * 11) / (TD5_TG_TEX_DIM * 2);
        if (idx < 0)  idx = 0;
        if (idx > 15) idx = 15;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R6 item 17] MASONRY pier face -- warm sandstone ashlar in staggered courses
 * (mortar lines every few rows, vertical joints offset course to course), so a
 * stone-style crossing reads as a masonry viaduct. Opaque. */
static void tg_emit_texture_page_r6_pier_stone(TG_Buf *out)
{
    unsigned int rng = 0x57021ACEu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* warm sandstone, r >= g >= b. 0..2 mortar shadow, 3..15 stone ramp. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int base = (i < 3) ? (70 + i * 10) : (120 + (i - 3) * 10);
        if (base > 255) base = 255;
        tg_put_u8(out, (unsigned)(base > 30 ? base - 30 : 0));   /* b */
        tg_put_u8(out, (unsigned)(base > 14 ? base - 14 : 0));   /* g */
        tg_put_u8(out, (unsigned)base);                          /* r */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        const int course = y / 10;
        const int joint  = ((x + (course & 1) * 8) % 16) < 1;   /* staggered */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if ((y % 10) < 1 || joint)
            idx = (int)((rng >> 16) % 3);            /* mortar line */
        else
            idx = 3 + (int)((rng >> 16) % 13);       /* stone face  */
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Terrain: 0 = SNOW ground, 1 = distant HILL / mountain flank.
 *
 * Both follow the rule the GROUND page comment sets out: NO structure and no
 * axis-aligned lines, because these pages are stretched over sloping,
 * undulating terrain where any grid shears into a rippled pattern. All the
 * variation is low-frequency patches plus per-texel grain.
 *
 * SNOW is deliberately not pure white: a flat 255 sheet loses all shape at
 * distance and every mip level collapses to the same colour, so the palette runs
 * cool blue-shadow to sunlit white and a coarse patch mask picks out drifts.
 *
 * HILL is mapped v=0 at the CREST (tg_emit_far_band winds the ridge with v=1 at
 * the base), so the page's TOP rows are the snowline and the rest is rock. */
static void tg_emit_texture_page_fb_terrain(TG_Buf *out, int which)
{
    unsigned int rng = which ? 0x4111BEEFu : 0x4222FEEDu;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR. SNOW: 0..5 blue-grey shadow, 6..15 up to near-white sunlit crust.
     * HILL: 0..9 rock (cool grey-brown), 10..15 pale snowline. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int b, g, r;
        if (!which) {
            if (i < 6) { b = 208 + i * 6; g = 198 + i * 7; r = 188 + i * 8; }
            else       { b = 244 + (i - 6); g = 240 + (i - 6); r = 236 + (i - 6) * 2; }
        } else {
            if (i < 10) { b = 92 + i * 4; g = 96 + i * 4; r = 88 + i * 5; }
            else        { b = 214 + (i - 10) * 8; g = 212 + (i - 10) * 8;
                          r = 208 + (i - 10) * 9; }
        }
        if (b > 255) b = 255;
        if (g > 255) g = 255;
        if (r > 255) r = 255;
        tg_put_u8(out, (unsigned)b);
        tg_put_u8(out, (unsigned)g);
        tg_put_u8(out, (unsigned)r);
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        const int x = i % TD5_TG_TEX_DIM;
        const int y = i / TD5_TG_TEX_DIM;
        /* Coarse 8x8-texel patch mask: drifts on snow, shadowed faces on rock. */
        const unsigned int patch = (unsigned)((x >> 3) + (y >> 3) * 11)
                                 * 2654435761u;
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (!which) {
            /* [R12 TEX item 8b] Same grid mask as the R8 snow variants; this is
             * the base SNOW page they fall back to. See tg_r12_blotch. */
            const int shaded = tg_r12_tex_blotch_on()
                             ? (tg_r12_blotch(x, y, 32, 0x4222FEEDu) < 96)
                             : ((patch >> 30) == 0);
            idx = 6 + (int)((rng >> 16) % 10);                /* sunlit crust */
            if (shaded) idx = (int)((rng >> 18) % 6);         /* drift shade */
        } else {
            /* Snowline over the top ~14 rows, ragged so it is not a hard band. */
            const int line = 10 + (int)((patch >> 29) % 5);
            if (y < line) idx = 10 + (int)((rng >> 16) % 6);
            else          idx = (int)((rng >> 16) % 10);
        }
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Gantry FRAME page: the uprights and the panel's underside cap.
 *
 * [FB r2 item 12] The panel itself no longer samples this page -- it carries the
 * shipped START / FINISH artwork instead (see tg_emit_gantry). Only the legs
 * (u in [0, 0.12], the left column band) and the cap (v in [0.9, 1]) are left
 * here, so the chequer band is now just what the cap's underside shows. Painted
 * rather than grained, because both users map flat unlit strips of it. */
static void tg_emit_texture_page_fb_banner(TG_Buf *out)
{
    /* 0 = post grey, 1 = surround (near black), 2 = white, 3 = mid grey. */
    static const unsigned char k_pal[4][3] = {
        {  92,  92,  94 },   /* BGR: post/leg grey       */
        {  22,  22,  24 },   /* surround                 */
        { 236, 240, 244 },   /* chequer light            */
        {  34,  34,  36 }    /* chequer dark             */
    };
    const int band_lo = TD5_TG_TEX_DIM / 4;          /* chequer band, top    */
    const int band_hi = TD5_TG_TEX_DIM - band_lo;    /* chequer band, bottom */
    const int cell    = TD5_TG_TEX_DIM / 8;          /* 8 units, so 8x4 cells */
    const int leg_col = (TD5_TG_TEX_DIM * 12) / 100; /* the legs' u window   */
    int i, y, x;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                /* opaque page */
    tg_put_u32(out, TD5_TG_PAL_COUNT);
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        const unsigned char *c = k_pal[i & 3];
        tg_put_u8(out, c[0]); tg_put_u8(out, c[1]); tg_put_u8(out, c[2]);
    }

    for (y = 0; y < TD5_TG_TEX_DIM; y++) {
        for (x = 0; x < TD5_TG_TEX_DIM; x++) {
            int idx;
            if (x < leg_col)                       idx = 0;   /* leg column   */
            else if (y < band_lo || y >= band_hi)  idx = 1;    /* surround     */
            else idx = (((x / cell) + (y / cell)) & 1) ? 3 : 2;
            tg_put_u8(out, (unsigned)idx);
        }
    }
}

/* [R5 STRUCT item 1] Dedicated SOLID leg-face page for the start/finish gantry
 * uprights. The legs used to sample a 12% window (u 0..0.12) of the shared
 * BANNER page, whose left column is post-grey and the rest is the black surround
 * + chequer end block. But the solid leg column is only 7 texels wide (leg_col =
 * 64*12/100 = 7, u <= 0.109), so the leg's u=0.12 right edge OVERSHOOTS the
 * column into the surround and chequer -- the reported "texture not fully
 * covering it, bleeding the edges into another texture" (framedump
 * log/r5s_leg_left_base.png: a black strip and white chequer blobs down the leg
 * edge). This page is cast concrete edge to edge, so there is no neighbouring
 * region for the leg UVs to bleed into -- the bleed is impossible by
 * construction, not merely inset away. Opaque, neutral grey (BGR == R == G),
 * matched to the old post-grey (~92) with faint vertical cast-joint lines so a
 * column reads as a column rather than a flat card. */
static void tg_emit_texture_page_r5_leg(TG_Buf *out)
{
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, neutral grey around the old post-grey (92,92,94). A small spread so
     * the joint lines below have something to pick from. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = 78 + (i % 8) * 5;                            /* 78..113 */
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;
        int idx = 3 + ((x * 7 + y) & 1);                     /* base concrete    */
        /* Two faint vertical cast joints so the post has some vertical grain. */
        if (x == TD5_TG_TEX_DIM / 3 || x == (2 * TD5_TG_TEX_DIM) / 3) idx = 1;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* [R4 BRANCH item 8] Vertical concrete KERB face for the two side walls of an
 * avenue median (see tg_emit_avenue_divider). Plain cast concrete: a lighter
 * kerb cap at the page top (v -> 0 is the top of the wall, matching the tree
 * page's "y=0 is the TOP" convention) over a mid-grey body with faint cast-joint
 * lines, so a planted or kerbed median reads as a real kerb instead of grass or
 * paving slabs climbing its walls. Opaque, neutral grey (BGR == R == G). */
static void tg_emit_texture_page_branch_kerb(TG_Buf *out)
{
    unsigned int rng = 0x4B00B1E5u;
    int i;

    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, 0);                                        /* opaque */
    tg_put_u32(out, TD5_TG_PAL_COUNT);

    /* BGR, neutral grey. 0..11 concrete body greys, 12..15 the light kerb cap. */
    for (i = 0; i < TD5_TG_PAL_COUNT; i++) {
        int v = (i < 12) ? (96 + i * 6) : (188 + (i - 12) * 10);
        tg_put_u8(out, (unsigned)v);   /* B */
        tg_put_u8(out, (unsigned)v);   /* G */
        tg_put_u8(out, (unsigned)v);   /* R */
    }

    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) {
        int x = i % TD5_TG_TEX_DIM;
        int y = i / TD5_TG_TEX_DIM;    /* y = 0 is the TOP of the page */
        int idx;
        rng = rng * 1103515245u + 12345u;
        if (y < 6) idx = 12 + (int)((rng >> 17) % 4);    /* light kerb cap */
        else       idx = (int)((rng >> 16) % 12);        /* concrete grain */
        if (y >= 6 && (y % 20) == 0)                     /* faint cast joints */
            idx = (idx > 2) ? idx - 2 : 0;
        (void)x;
        tg_put_u8(out, (unsigned)idx);
    }
}

/* Emit a page from real (already palette-indexed) TD5 texture data, in the same
 * on-disk page format as the procedural emitters -- pad, opaque type, palette
 * count, BGR palette, then the 64x64 index bytes. */
static void tg_emit_real_page(TG_Buf *out, const unsigned char *pal, int paln,
                              const unsigned char *idx, int type)
{
    int i;
    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);         /* 0 opaque, 1 alpha-keyed (idx 0) */
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln * 3; i++) tg_put_u8(out, pal[i]);
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) tg_put_u8(out, idx[i]);
}

/* [R8 G1] Same page, PALETTE DIMMED to num/den, with a slight warm bias so the
 * result reads as sodium-lit rather than as a grey photograph. Only the palette
 * is touched -- the 4096 indices are copied verbatim -- so the artwork, its key
 * coverage and its alpha type are bit-identical to the source.
 *
 * Entry 0 is left alone: on an alpha-keyed page it IS the key, and darkening a
 * key colour is at best a no-op and at worst turns a transparent texel into a
 * visible dark one if the renderer ever compares by colour rather than index. */
static void tg_emit_real_page_dim(TG_Buf *out, const unsigned char *pal,
                                  int paln, const unsigned char *idx, int type,
                                  int num, int den)
{
    int i;
    tg_put_u8(out, 0); tg_put_u8(out, 0); tg_put_u8(out, 0);
    tg_put_u8(out, (unsigned)type);
    tg_put_u32(out, (unsigned)paln);
    for (i = 0; i < paln; i++) {
        /* Source is BGR. Blue is dimmed hardest and red least, which is the
         * warm bias -- a night banner lit by street lighting, not a photograph
         * with the brightness pulled down. */
        static const int k_bias[3] = { 82, 92, 108 };   /* B, G, R, percent */
        int c;
        for (c = 0; c < 3; c++) {
            int v = (i == 0) ? pal[i * 3 + c]
                  : pal[i * 3 + c] * num * k_bias[c] / (den * 100);
            if (v > 255) v = 255;
            if (v < 0) v = 0;
            tg_put_u8(out, (unsigned)v);
        }
    }
    for (i = 0; i < TD5_TG_TEX_TEXELS; i++) tg_put_u8(out, idx[i]);
}

/* [R8 G1] The two alternative start/finish banner sets, in the
 * START_L, START_R, FINISH_L, FINISH_R order tg_banner_page indexes.
 *   +0..+3  level003's black-serif-on-pale-blue word set (mined, real art)
 *   +4..+7  the shipped Keswick set at 45% brightness, for a night track
 * Emitted with each source page's own alpha type so a keyed banner stays keyed.
 * If the mined header is ever short, the slot falls back to the shipped page's
 * artwork rather than to a blank -- an empty banner would read as a missing
 * gantry, which is a far worse failure than a repeated one. */
static void tg_emit_r8var_banner_pages(TG_Buf *pages)
{
    static const unsigned char *const k_src_pal[4] = {
        k_furn_start_l_pal, k_furn_start_r_pal,
        k_furn_finish_l_pal, k_furn_finish_r_pal };
    static const unsigned char *const k_src_idx[4] = {
        k_furn_start_l_idx, k_furn_start_r_idx,
        k_furn_finish_l_idx, k_furn_finish_r_idx };
    const int k_src_paln[4] = { k_furn_start_l_paln, k_furn_start_r_paln,
                                k_furn_finish_l_paln, k_furn_finish_r_paln };
    const int k_src_type[4] = { k_furn_start_l_type, k_furn_start_r_type,
                                k_furn_finish_l_type, k_furn_finish_r_type };
    int v;

    for (v = 0; v < TD5_TG_R8V_BANNER_N; v++) {
        if (v < k_real_r8var_bann_count)
            tg_emit_real_page(&pages[TD5_TG_PAGE_R8V_BANNER + v],
                              k_real_r8var_bann_pal[v],
                              k_real_r8var_bann_paln[v],
                              k_real_r8var_bann_idx[v], 1);
        else
            tg_emit_real_page(&pages[TD5_TG_PAGE_R8V_BANNER + v],
                              k_src_pal[v], k_src_paln[v], k_src_idx[v],
                              k_src_type[v]);
        tg_emit_real_page_dim(&pages[TD5_TG_PAGE_R8V_BANNER_NIGHT + v],
                              k_src_pal[v], k_src_paln[v], k_src_idx[v],
                              k_src_type[v], 45, 100);
    }
}

/* [R8 G1] Real guardrail pages, alpha-keyed on index 0. A slot the header does
 * not fill falls back to the procedural armco so tg_rail_page can never land on
 * a blank page. */
static void tg_emit_r8var_rail_pages(TG_Buf *pages)
{
    int v;
    for (v = 0; v < TD5_TG_R8V_RAIL_N; v++) {
        if (v < k_real_r8var_rail_count)
            tg_emit_real_page(&pages[TD5_TG_PAGE_R8V_RAIL + v],
                              k_real_r8var_rail_pal[v],
                              k_real_r8var_rail_paln[v],
                              k_real_r8var_rail_idx[v], 1);
        else
            tg_emit_texture_page_rail(&pages[TD5_TG_PAGE_R8V_RAIL + v]);
    }
}

/* Fill the wall/store/grass/tree/prop pages with REAL TD5 texture data borrowed
 * from shipped tracks instead of the procedural placeholders, so the auto-track
 * reads like an actual TD5 level.
 *
 * DEFAULT ON since 2026-08-27 (TD5RE_AUTOTRACK_REAL_TEX=0 restores procedural
 * art). It was opt-in while td5_tg_real_tex.h only carried the facade set, and
 * the header comment above still said "tree + rail stay procedural" long after
 * the foliage pages landed -- which is why placeholder tree silhouettes were
 * still what a default run put on screen. The borrowed set is now complete for
 * every page this branch fills, VERIFIED against the header data:
 *   wall  5 of TD5_TG_WALL_VARIANTS  5   store 3 of TD5_TG_STORE_VARIANTS 3
 *   tree 10 of TD5_TG_TREE_VARIANTS 10   prop  7 of TD5_TG_PROP_COUNT     7
 * and every one of those 25 pages is well formed -- palette length == 3*paln,
 * 4096 index bytes, max index < paln. The 17 alpha-keyed pages (10 tree, 7
 * prop) all carry palette entry 0 = black with real key coverage (tree 35..83%
 * of texels, prop 32..74%), so index 0 is genuinely the transparent key on all
 * of them and nothing keys away real foliage.
 *
 * Nothing ELSE moves when this flips: road, ground, rail, water, the themed
 * road surfaces and every [FB] page are emitted procedurally on BOTH sides of
 * the branch (road/ground deliberately so -- see tg_emit_textures). */
int tg_real_textures_enabled(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_REAL_TEX");
}

/* Street FURNITURE (the kerb railing) on shipped art rather than the painted
 * stand-in. Default ON and independent of TD5RE_AUTOTRACK_REAL_TEX: that knob
 * trades a neutral generic street for photographic Sydney facades, which is
 * a taste call, whereas a real railing has no such downside. The knob exists so
 * the painted page is still one A/B away. */
static int tg_furniture_real_pages(void)
{
    return td5_env_flag_on("TD5RE_AUTOTRACK_REAL_FURNITURE");
}

static int tg_emit_textures(TG_Buf *out)
{
    TG_Buf pages[TD5_TG_PAGE_COUNT];
    const unsigned int count = TD5_TG_PAGE_COUNT;
    unsigned int cursor = 4 + 4 * count;
    unsigned int i;

    memset(pages, 0, sizeof(pages));

    /* ROAD and GROUND are ALWAYS procedural: the shipped city road/ground pages
     * are sandstone tan and read muddy under the auto-track, whereas the
     * procedural asphalt is grey with lane paint and the procedural ground is
     * neutral grey concrete -- both closer to what a generic street wants. Only
     * the FACADE walls (and grass) borrow real TD5 pages, which is where the
     * photographic detail actually pays off. */
    tg_emit_texture_page_asphalt(&pages[TD5_TG_PAGE_ROAD]);
    tg_emit_texture_page_ground(&pages[TD5_TG_PAGE_GROUND]);
    if (tg_real_textures_enabled()) {
        int v, w;
        tg_emit_real_page(&pages[TD5_TG_PAGE_WALL],
                          k_real_wall_pal[0], k_real_wall_paln[0], k_real_wall_idx[0], 0);
        for (v = 1; v < k_real_wall_count && v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1],
                              k_real_wall_pal[v], k_real_wall_paln[v], k_real_wall_idx[v], 0);
        /* Variants from the OTHER city tracks, laid down after the level014
         * ones: low-rise masonry, then the tower class at WALL_TOWER_FIRST.
         * Each loop stops at the smaller of its source count and the slots it
         * owns, so adding a page to either header cannot walk into the next
         * group's pages. */
        for (v = 0; v < k_real_city_low_count; v++) {
            w = k_real_wall_count + v;
            if (w >= TD5_TG_WALL_TOWER_FIRST) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + w - 1],
                              k_real_city_low_pal[v], k_real_city_low_paln[v],
                              k_real_city_low_idx[v], 0);
        }
        for (v = 0; v < k_real_city_tower_count; v++) {
            w = TD5_TG_WALL_TOWER_FIRST + v;
            if (w >= TD5_TG_WALL_VARIANTS) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_WALL_EXTRA + w - 1],
                              k_real_city_tower_pal[v], k_real_city_tower_paln[v],
                              k_real_city_tower_idx[v], 0);
        }
        for (v = 0; v < k_real_store_count && v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_STORE + v],
                              k_real_store_pal[v], k_real_store_paln[v], k_real_store_idx[v], 0);
        for (v = 0; v < k_real_city_store_count; v++) {
            w = k_real_store_count + v;
            if (w >= TD5_TG_STORE_VARIANTS) break;
            tg_emit_real_page(&pages[TD5_TG_PAGE_STORE + w],
                              k_real_city_store_pal[v], k_real_city_store_paln[v],
                              k_real_city_store_idx[v], 0);
        }
        /* [R7 item 4] extra facade variety in the R7_CITY block. Any slot the
         * header does not fill falls back to a procedural wall so the selection
         * pool never lands on a blank (black) page. */
        for (v = 0; v < TD5_TG_R7_WALL_LOW_N; v++) {
            if (v < k_real_r7city_low_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                  k_real_r7city_low_pal[v],
                                  k_real_r7city_low_paln[v],
                                  k_real_r7city_low_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                          TD5_TG_WALL_VARIANTS + v);
        }
        for (v = 0; v < TD5_TG_R7_WALL_TOWER_N; v++) {
            if (v < k_real_r7city_tower_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                  k_real_r7city_tower_pal[v],
                                  k_real_r7city_tower_paln[v],
                                  k_real_r7city_tower_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                          TD5_TG_WALL_VARIANTS
                                          + TD5_TG_R7_WALL_LOW_N + v);
        }
        /* [R8 G1] The R8 facade block, filled the same way and with the same
         * "never leave a selectable slot blank" fallback. Seeds for the
         * procedural fallback continue past the R7 range so no two variety
         * pages are the same procedural wall. */
        for (v = 0; v < TD5_TG_R8V_WALL_LOW_N; v++) {
            if (v < k_real_r8var_low_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R8V_WALL_LOW + v],
                                  k_real_r8var_low_pal[v],
                                  k_real_r8var_low_paln[v],
                                  k_real_r8var_low_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R8V_WALL_LOW + v],
                                          TD5_TG_WALL_VARIANTS
                                          + TD5_TG_R7_WALL_LOW_N
                                          + TD5_TG_R7_WALL_TOWER_N + v);
        }
        for (v = 0; v < TD5_TG_R8V_WALL_TOWER_N; v++) {
            if (v < k_real_r8var_tower_count)
                tg_emit_real_page(&pages[TD5_TG_PAGE_R8V_WALL_TOWER + v],
                                  k_real_r8var_tower_pal[v],
                                  k_real_r8var_tower_paln[v],
                                  k_real_r8var_tower_idx[v], 0);
            else
                tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R8V_WALL_TOWER + v],
                                          TD5_TG_WALL_VARIANTS
                                          + TD5_TG_R7_WALL_LOW_N
                                          + TD5_TG_R7_WALL_TOWER_N
                                          + TD5_TG_R8V_WALL_LOW_N + v);
        }
        tg_emit_real_page(&pages[TD5_TG_PAGE_GREEN],
                          k_real_green_pal, k_real_green_paln, k_real_green_idx, 0);
        /* Thematic trees: alpha-keyed (type 1), index 0 transparent. */
        for (v = 0; v < TD5_TG_TREE_VARIANTS && v < k_real_tree_count; v++)
            tg_emit_real_page(&pages[tg_tree_slot(v)],
                              k_real_tree_pal[v], k_real_tree_paln[v], k_real_tree_idx[v], 1);
        /* [R5 item 18] Tall Moscow park trees, alpha-keyed like the rest. Capped
         * at the FLORA block width so a header edit cannot walk past it. */
        for (v = 0; v < k_r5_flora_tree_count && v < TD5_TG_R5_FLORA_N; v++)
            tg_emit_real_page(&pages[tg_flora_tree_slot(v)],
                              k_r5_flora_tree_pal[v], k_r5_flora_tree_paln[v],
                              k_r5_flora_tree_idx[v], 1);
        /* Props: people/statue/animal (type 1), lamp glow (type 3 additive). */
        for (v = 0; v < TD5_TG_PROP_COUNT && v < k_real_prop_count; v++)
            tg_emit_real_page(&pages[tg_prop_slot(v)],
                              k_real_prop_pal[v], k_real_prop_paln[v],
                              k_real_prop_idx[v], k_prop_pages[v].type);
    } else {
        int v;
        tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL], 0);
        for (v = 1; v < TD5_TG_WALL_VARIANTS; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_WALL_EXTRA + v - 1], v);
        /* [R7 item 4] procedural fill of the R7 variety pages -- distinct seeds
         * past the original variant range so each is its own wall. */
        for (v = 0; v < TD5_TG_R7_WALL_LOW_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_LOW + v],
                                      TD5_TG_WALL_VARIANTS + v);
        for (v = 0; v < TD5_TG_R7_WALL_TOWER_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R7_WALL_TOWER + v],
                                      TD5_TG_WALL_VARIANTS + TD5_TG_R7_WALL_LOW_N + v);
        /* [R8 G1] and the R8 variety block, distinct seeds again. */
        for (v = 0; v < TD5_TG_R8V_WALL_LOW_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R8V_WALL_LOW + v],
                                      TD5_TG_WALL_VARIANTS + TD5_TG_R7_WALL_LOW_N
                                      + TD5_TG_R7_WALL_TOWER_N + v);
        for (v = 0; v < TD5_TG_R8V_WALL_TOWER_N; v++)
            tg_emit_texture_page_wall(&pages[TD5_TG_PAGE_R8V_WALL_TOWER + v],
                                      TD5_TG_WALL_VARIANTS + TD5_TG_R7_WALL_LOW_N
                                      + TD5_TG_R7_WALL_TOWER_N
                                      + TD5_TG_R8V_WALL_LOW_N + v);
        for (v = 0; v < TD5_TG_STORE_VARIANTS; v++)
            tg_emit_texture_page_store(&pages[TD5_TG_PAGE_STORE + v], v);
        tg_emit_texture_page_green(&pages[TD5_TG_PAGE_GREEN]);
        /* Procedural trees vary by shape (deciduous/conifer). */
        for (v = 0; v < TD5_TG_TREE_VARIANTS; v++)
            tg_emit_texture_page_tree(&pages[tg_tree_slot(v)], k_tree_pages[v].shape);
        /* Procedural props by kind (person/statue/animal/lamp). */
        for (v = 0; v < TD5_TG_PROP_COUNT; v++)
            tg_emit_texture_page_prop(&pages[tg_prop_slot(v)], k_prop_pages[v].kind);
    }
    tg_emit_texture_page_rail(&pages[TD5_TG_PAGE_RAIL]);
    tg_emit_texture_page_water(&pages[TD5_TG_PAGE_WATER]);
    {   /* themed road-surface pages (variant 0 = base asphalt, already done) */
        int v;
        for (v = 1; v < TD5_TG_ROAD_VARIANTS; v++)
            tg_emit_texture_page_roadsurf(&pages[tg_road_slot(v)],
                                          k_road_surf[v].proc_kind);
    }
    /* [FB 2026-08-26] reserved feedback-batch pages -- one owner each. */
    tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_SIDEWALK], 0);
    tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_CROSSING], 1);
    if (tg_furniture_real_pages())
        tg_emit_real_page(&pages[TD5_TG_PAGE_FENCE], k_furn_fence_pal,
                          k_furn_fence_paln, k_furn_fence_idx, k_furn_fence_type);
    else
        tg_emit_texture_page_fb_city(&pages[TD5_TG_PAGE_FENCE], 2);
    tg_emit_texture_page_fb_treeline(&pages[TD5_TG_PAGE_TREELINE], 0);
    {   /* [R8 TERRAIN items 14/16] tree-line variants, snow ground variants and
         * the snow median page. Always emitted so the pages exist whether or not
         * the selection knobs route anything to them -- a page slot that is
         * referenced but never written decodes as garbage. */
        int v;
        for (v = 0; v < TD5_TG_R8_TREELINE_N; v++)
            tg_emit_texture_page_fb_treeline(
                &pages[TD5_TG_PAGE_R8_TREELINE + v], v);
        for (v = 0; v < TD5_TG_R8_SNOWGND_N; v++)
            tg_emit_texture_page_r8_snow_ground(
                &pages[TD5_TG_PAGE_R8_SNOWGND + v], v);
        tg_emit_texture_page_r8_snow_median(&pages[TD5_TG_PAGE_R8_SNOWMED]);
    }
    {   /* [R9 INFRA] Street-furniture pages. ALWAYS emitted, and deliberately
         * outside tg_real_textures_enabled(): that knob trades a neutral street
         * for photographic facades, which is a taste call, whereas a bin that
         * looks like a bin has no downside. A slot the header does not fill is
         * left to the procedural filler below rather than decoded as garbage. */
        int v;
        for (v = 0; v < TD5_TG_PROPS_TEX_COUNT && v < TD5_TG_R9_INFRA_N; v++)
            tg_emit_real_page(&pages[TD5_TG_INFRA_PAGE(v)], k_props_pal[v],
                              k_props_paln[v], k_props_idx[v], k_props_type[v]);
    }
    {   /* [R11 SIGNS item 16] Direction-arrow panels + their post. ALWAYS
         * emitted, for the same reason as the R9 furniture above: the facade
         * knob is a taste call, whereas a sign that tells you which way the
         * road goes has no downside. The loop stops at the smaller of the
         * header's count and the slots this area owns, so growing the header
         * cannot walk into the post slot. */
        int v;
        for (v = 0; v < k_real_r11sign_arrow_count
                    && v < TD5_TG_R11_SIGN_N - 1; v++)
            tg_emit_real_page(&pages[TD5_TG_PAGE_R11_SIGN + v],
                              k_real_r11sign_arrow_pal[v],
                              k_real_r11sign_arrow_paln[v],
                              k_real_r11sign_arrow_idx[v], 1);
        tg_emit_texture_page_r11_sign_post(
            &pages[TD5_TG_PAGE_R11_SIGN_POST]);
    }
    tg_emit_texture_page_fb_skyline(&pages[TD5_TG_PAGE_R4_SKYLINE]);  /* [R4 item 5] */
    tg_emit_texture_page_fb_tunnel(&pages[TD5_TG_PAGE_TUNNEL]);
    /* [R3 BRIDGE] deck surface (item 11) + 4 extra tunnel linings (item 16a). */
    tg_emit_texture_page_bridge_deck(&pages[TD5_TG_PAGE_BRIDGE_DECK]);
    {
        int v;
        for (v = 1; v < TD5_TG_TUNNEL_VARIANTS; v++)
            tg_emit_texture_page_tunnel_var(&pages[TD5_TG_PAGE_TUNNEL_VAR + v - 1], v);
    }
    /* [R6 TUNNEL item 8] proper concrete lining variants (b/c), portal header,
     * and emissive wall-lamp strip -- all in the reserved R6_TUNNEL page block. */
    {
        int v;
        for (v = 0; v < 4; v++)
            tg_emit_texture_page_r6_tunnel_lining(&pages[TD5_TG_PAGE_R6_TUNNEL + v], v);
    }
    tg_emit_texture_page_r6_tunnel_portal(&pages[TD5_TG_PAGE_R6_TUNNEL + 4]);
    tg_emit_texture_page_r6_tunnel_lamp(&pages[TD5_TG_PAGE_R6_TUNNEL + 5]);
    tg_emit_texture_page_fb_terrain(&pages[TD5_TG_PAGE_SNOW], 0);
    tg_emit_texture_page_fb_terrain(&pages[TD5_TG_PAGE_HILL], 1);
    tg_emit_texture_page_fb_banner(&pages[TD5_TG_PAGE_BANNER]);
    /* [FB r2] Real shipped furniture art. Unlike the level014 pages above these
     * are NOT behind TD5RE_AUTOTRACK_REAL_TEX: there is no procedural stand-in
     * worth keeping for a lamp post or a word, so they are the only artwork
     * these three page groups ever carry. */
    tg_emit_real_page(&pages[TD5_TG_PAGE_LAMPPOST], k_furn_lamp_pal,
                      k_furn_lamp_paln, k_furn_lamp_idx, k_furn_lamp_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_START_L], k_furn_start_l_pal,
                      k_furn_start_l_paln, k_furn_start_l_idx, k_furn_start_l_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_START_R], k_furn_start_r_pal,
                      k_furn_start_r_paln, k_furn_start_r_idx, k_furn_start_r_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_FINISH_L], k_furn_finish_l_pal,
                      k_furn_finish_l_paln, k_furn_finish_l_idx, k_furn_finish_l_type);
    tg_emit_real_page(&pages[TD5_TG_PAGE_FINISH_R], k_furn_finish_r_pal,
                      k_furn_finish_r_paln, k_furn_finish_r_idx, k_furn_finish_r_type);
    /* [R8 G1] The two ALTERNATIVE banner sets. Filled unconditionally, next to
     * the shipped set and for the same reason: there is no procedural stand-in
     * for a word, and an unreferenced page costs nothing but its 4KB. */
    tg_emit_r8var_banner_pages(pages);
    /* [R8 G1] Real photographic guardrails, alpha-keyed on index 0. Also
     * unconditional: TD5RE_AUTOTRACK_REAL_TEX only gates the pages that HAVE a
     * procedural stand-in, and tg_rail_page falls back to TD5_TG_PAGE_RAIL --
     * a different page entirely -- when its own knob is off. */
    tg_emit_r8var_rail_pages(pages);
    /* [R4 BRANCH item 8] concrete kerb face for avenue-median side walls. */
    tg_emit_texture_page_branch_kerb(&pages[TD5_TG_PAGE_BRANCH_KERB]);
    /* [R3 BLOCK] park & house art (feedback items 5-6). Slots +4..+9 of the
     * BLOCK reservation stay empty (unreferenced empty pages are free). */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 0], 0);  /* lawn  */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 1], 1);  /* hedge */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 2], 2);  /* wall  */
    tg_emit_texture_page_r3_block(&pages[TD5_TG_PAGE_R3_BLOCK + 3], 3);  /* roof  */
    /* [R4 BRIDGE] guardrail face (16b), pier/tower concrete (18), shore (20). */
    tg_emit_texture_page_r4_guardrail(&pages[TD5_TG_PAGE_R4_GUARDRAIL]);
    tg_emit_texture_page_r4_pier(&pages[TD5_TG_PAGE_R4_PIER]);
    tg_emit_texture_page_r4_coast(&pages[TD5_TG_PAGE_R4_COAST]);
    /* [R5 item 17a] armco steel W-beam guardrail (replaces the R4 concrete). */
    tg_emit_texture_page_r5_guardrail(&pages[TD5_TG_PAGE_R5_BRIDGE + 0]);
    /* [R6 item 13] alpha-keyed W-beam guardrail (transparent between beam/posts). */
    tg_emit_texture_page_r6_guardrail_alpha(&pages[TD5_TG_PAGE_R6_BRIDGE + 0]);
    /* [R6 item 17] per-style pier faces: steel plate + masonry ashlar. */
    tg_emit_texture_page_r6_pier_steel(&pages[TD5_TG_PAGE_R6_BRIDGE + 1]);
    tg_emit_texture_page_r6_pier_stone(&pages[TD5_TG_PAGE_R6_BRIDGE + 2]);
    /* [R7 item 9] concrete tunnel-portal facade (framed mouth, replaces the
     * R6 thin lintel). */
    tg_emit_texture_page_r7_tunnel_portal(&pages[TD5_TG_PAGE_R7_BRIDGE + 0]);
    /* [R8 item 17] bore floor / tunnel median concrete (replaces the biome
     * grass page on a fork gore that runs through a tunnel). */
    tg_emit_texture_page_r8_bore_median(&pages[TD5_TG_PAGE_R8_BRIDGE + 0]);
    /* [R8 item 4] unbanded cast-concrete portal headwall (replaces the R7 page
     * whose coping band repeated up the jambs as "lighter gray stripes"). */
    tg_emit_texture_page_r8_tunnel_portal(&pages[TD5_TG_PAGE_R8_BRIDGE + 1]);
    /* [R9 TUNNEL items 5,13] portal face/surround, bore ceiling, and the four
     * pages of the new city UNDERPASS element. */
    tg_emit_texture_page_r9_portal_face(&pages[TD5_TG_PAGE_R9_PORTAL_FACE]);
    tg_emit_texture_page_r9_portal_surround(&pages[TD5_TG_PAGE_R9_PORTAL_SURR]);
    tg_emit_texture_page_r9_bore_ceiling(&pages[TD5_TG_PAGE_R9_BORE_CEIL]);
    tg_emit_texture_page_r9_up_abutment(&pages[TD5_TG_PAGE_R9_UP_ABUT]);
    tg_emit_texture_page_r9_up_soffit(&pages[TD5_TG_PAGE_R9_UP_SOFFIT]);
    tg_emit_texture_page_r9_up_deck(&pages[TD5_TG_PAGE_R9_UP_DECK]);
    tg_emit_texture_page_r9_up_parapet(&pages[TD5_TG_PAGE_R9_UP_PARAPET]);
    /* [R4 CROSS item 9] cross-street asphalt with a longitudinal centre line. */
    tg_emit_texture_page_r4_cross(&pages[TD5_TG_PAGE_R4_CROSS + 0]);
    /* [R5 STRUCT item 1] solid concrete leg-face page for the gantry uprights. */
    tg_emit_texture_page_r5_leg(&pages[TD5_TG_PAGE_R5_LEG]);
    /* [R9 item 9] distinct materials for the bridge's ABOVE-deck pillars, its
     * horizontal beam, and the shore band -- all three previously shared a page
     * with something authored for other axes. */
    tg_emit_texture_page_r9_pylon(&pages[TD5_TG_PAGE_R9_PYLON]);
    tg_emit_texture_page_r9_beam(&pages[TD5_TG_PAGE_R9_BEAM]);
    tg_emit_texture_page_r9_shore(&pages[TD5_TG_PAGE_R9_SHORE]);

    for (i = 0; i < count; i++) {
        if (pages[i].oom) {
            for (i = 0; i < count; i++) tg_buf_free(&pages[i]);
            return 0;
        }
    }

    /* [R8 BRIDGE item 4] The complaint about the tunnel mouth is a page CHOICE
     * ("this grey-ish texture, the one with lighter gray stripes"), and two
     * rounds were spent arguing about pages by their NUMBER. Dump every
     * assembled page as a PPM so a selection claim can be looked at instead of
     * asserted. TD5RE_R8_TEXDUMP=1; writes log/tgpage_NNN.ppm. */
    if (td5_env_flag_off("TD5RE_R8_TEXDUMP")) {
        unsigned p;
        for (p = 0; p < count; p++) {
            char path[160];
            FILE *f;
            const unsigned char *b = pages[p].b;
            unsigned paln;
            int t;
            if (pages[p].len < 8u + 3u * 16u + (unsigned)TD5_TG_TEX_TEXELS)
                continue;
            paln = (unsigned)b[4] | ((unsigned)b[5] << 8)
                 | ((unsigned)b[6] << 16) | ((unsigned)b[7] << 24);
            if (paln == 0u || paln > 256u) continue;
            snprintf(path, sizeof(path), "log/tgpage_%03u.ppm", p);
            f = fopen(path, "wb");
            if (!f) continue;
            fprintf(f, "P6\n%d %d\n255\n", TD5_TG_TEX_DIM, TD5_TG_TEX_DIM);
            for (t = 0; t < TD5_TG_TEX_TEXELS; t++) {
                unsigned idx = b[8 + 3u * paln + (unsigned)t];
                const unsigned char *c = b + 8 + 3u * (idx < paln ? idx : 0u);
                fputc(c[2], f); fputc(c[1], f); fputc(c[0], f);  /* BGR -> RGB */
            }
            fclose(f);
        }
        TD5_LOG_I(LOG_TAG, "trackgen: R8 TEXDUMP wrote %u page PPMs to log/",
                  count);
    }

    tg_put_u32(out, count);
    for (i = 0; i < count; i++) {           /* absolute page offsets */
        tg_put_u32(out, cursor);
        cursor += (unsigned)pages[i].len;
    }
    for (i = 0; i < count; i++) {
        if (!tg_buf_need(out, pages[i].len)) break;
        memcpy(out->b + out->len, pages[i].b, pages[i].len);
        out->len += pages[i].len;
    }
    for (i = 0; i < count; i++) tg_buf_free(&pages[i]);

    TD5_LOG_I(LOG_TAG, "trackgen: textures = %u page(s), %zu bytes",
              count, out->len);
    return !out->oom;
}

/* ---------------------------------------------------------- config ------- */
void td5_trackgen_default_spec(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    memset(spec, 0, sizeof(*spec));
    spec->seed          = 0;
    spec->target_spans  = 1800;
    spec->lanes         = 4;
    spec->lane_width    = TD5_TG_LANE_WIDTH;
    spec->span_length   = TD5_TG_SPAN_LENGTH;
    spec->elevation_amplitude = 6000;
    spec->circuit       = 0;    /* point-to-point: no lap wrap, cheap ribbon */
    spec->curve_safety_x100 = 180;   /* 1.80 = the Python tool's 1.5 + headroom */
    spec->max_grade_x1000   = 120;   /* 0.120 = the Python tool's max_grade */
    spec->weight[TD5_TG_STRAIGHT]  = 35;
    spec->weight[TD5_TG_CURVE]     = 40;
    spec->weight[TD5_TG_ACUTE]     = 15;
    spec->weight[TD5_TG_DUAL_LANE] = 10;
}

void td5_trackgen_apply_config(TD5_TrackGenSpec *spec)
{
    if (!spec) return;
    spec->target_spans = td5_env_int("TD5RE_AUTOTRACK_SPANS",
                                     spec->target_spans, 60, TD5_TG_MAX_SPANS);
    /* [LANES] BASE lane count. The old 2..4 clamp cited "shipped tracks
     * never exceed 4 lanes"; the census (docs/plans/AUTOTRACK_TRACK_CENSUS.md)
     * shows 2..10 per span, and the rail / edge LUTs are per span TYPE, not
     * per lane count. Sections vary around this base (TD5RE_AUTOTRACK_LANE_*). */
    spec->lanes        = td5_env_int("TD5RE_AUTOTRACK_LANES",
                                     spec->lanes, 2, 8);
    spec->elevation_amplitude =
        td5_env_int("TD5RE_AUTOTRACK_ELEVATION",
                    spec->elevation_amplitude, 0, 40000);
    spec->weight[TD5_TG_STRAIGHT] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_STRAIGHT",
                    spec->weight[TD5_TG_STRAIGHT], 0, 100);
    spec->weight[TD5_TG_CURVE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_CURVE",
                    spec->weight[TD5_TG_CURVE], 0, 100);
    spec->weight[TD5_TG_ACUTE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_ACUTE",
                    spec->weight[TD5_TG_ACUTE], 0, 100);
    spec->weight[TD5_TG_DUAL_LANE] =
        td5_env_int("TD5RE_AUTOTRACK_PCT_DUAL",
                    spec->weight[TD5_TG_DUAL_LANE], 0, 100);
    spec->curve_safety_x100 =
        td5_env_int("TD5RE_AUTOTRACK_CURVESAFE",
                    spec->curve_safety_x100, 100, 800);
    spec->max_grade_x1000 =
        td5_env_int("TD5RE_AUTOTRACK_GRADE",
                    spec->max_grade_x1000, 0, 200);
}

/* [R11 GUARD] Per-span dump of the STRUCTURAL ROAD EDGE -- everything item 5
 * ("remove the slow transition for guardrails and sidewalks") can possibly be
 * about, on one line per span, so the ramp can be MEASURED instead of guessed
 * at from a frame. Prints the hard cell biome, the dithered biome the scenery
 * emitters actually ask for, the scenery biome R7 hardened at city edges, the
 * pavement/verge width per side, whether a kerb railing stands, whether the
 * roadside barrier gate passes, and which page the barrier wears.
 *
 * A "slow transition" shows up here as a run of spans where hard != soft, i.e.
 * the 20-span dither band around a biome cell boundary, with the pavement width
 * and the rail page flipping span to span across it.
 *
 * Read-only, opt-in via TD5RE_R11_GUARD_REPORT=1, windowed by
 * TD5RE_R11_GUARD_REPORT_LO/HI so race.log stays legible. */
static void tg_r11_guard_report(const TG_NodeList *nl, int nspans)
{
    int si, lo, hi, band = 0, flips = 0;
    if (!td5_env_flag_off("TD5RE_R11_GUARD_REPORT")) return;
    lo = td5_env_int("TD5RE_R11_GUARD_REPORT_LO", 0, 0, 100000);
    hi = td5_env_int("TD5RE_R11_GUARD_REPORT_HI", 100000, 0, 100000);
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- [R11 GUARD] structural road edge ----");
    for (si = 1; si + 2 < nl->count && si < nspans; si++) {
        const int hard = tg_biome_cell_index(si);
        const int soft = tg_biome_for_span(si);
        const int scen = tg_scenery_biome_index(si);
        const double sw = tg_city_sidewalk_w_at(nl, si, &k_biomes[scen]);
        const double bw = tg_verge_band_w(&k_biomes[scen]);
        const double base = (sw > 0.0) ? sw : bw;
        if (hard != soft) band++;
        if (hard != soft && tg_biome_index_is_city(hard) != tg_biome_index_is_city(soft))
            flips++;
        if (si < lo || si > hi) continue;
        TD5_LOG_I(LOG_TAG,
            "r11guard: si=%d hard=%s soft=%s scen=%s dither=%d walk=%.0f "
            "verge=%.0f paveL=%.0f paveR=%.0f fenceL=%d fenceR=%d "
            "railgate=%d railpage=%d railL=%u railR=%u",
            si, k_biomes[hard].name, k_biomes[soft].name, k_biomes[scen].name,
            hard != soft, sw, bw,
            tg_pavement_side_width(nl, si,  1.0, base),
            tg_pavement_side_width(nl, si, -1.0, base),
            tg_rail_kerbfence_here(si,  1.0), tg_rail_kerbfence_here(si, -1.0),
            tg_span_needs_guardrail(nl, si, nspans), tg_rail_page(si),
            (unsigned)s_rail_edge[si][0], (unsigned)s_rail_edge[si][1]);
    }
    TD5_LOG_I(LOG_TAG,
              "r11guard: dithered spans=%d (of %d), city-boundary dithers=%d",
              band, nspans, flips);
}

/* ============ [R13 RAIL item 5b] WHAT ACTUALLY STANDS AT A BRIDGE MOUTH =====
 * "Between the road and the bridge, the guardrails and the sidewalk
 *  DISAPPEAR."  -- and R12 already reported this class fixed, so the first job
 *  is to decide WHICH claim is true rather than to extend that fix:
 *
 *   (a) the R12 termination CAP does not reach this hand-off, i.e. the
 *       cross-section is still open at 1155 -- an END FACE is missing; or
 *   (b) the cap fires and the report is about something else entirely: a real
 *       LENGTH of road that carries no barrier and no pavement at all.
 *
 * Those have opposite fixes, and no amount of reading decides between them,
 * because the two treatments are gated by four different predicates that only
 * coincide by construction. So print all four, per span, across every mouth on
 * the track: whether the span is inside the bridge run, its lift over local
 * ground, whether the PARAPET stands (tg_rail_deck_here -- the only thing that
 * can own a deck edge), whether the ROADSIDE gate passes, what each edge
 * actually ended up carrying (s_rail_edge, recorded at emit time, so this is
 * what was written and not what was intended), and whether the raised pavement
 * stands per side (tg_r12_pave_stands -- the sidewalk emitter's own gate).
 *
 * A run of spans with rail=0 on both edges and pave=0 on both sides IS answer
 * (b), stated as a length. Mouths are found rather than passed in, so this
 * measures every bridge on the track and not the two spans in the report.
 *
 * Read-only, opt-in via TD5RE_R13_RAIL_REPORT=1. */
static void tg_r13_rail_mouth_report(const TG_NodeList *nl, int nspans)
{
    int si, gap_run = 0, gap_worst = 0, gap_at = -1, mouths = 0;
    if (!td5_env_flag_off("TD5RE_R13_RAIL_REPORT")) return;
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- [R13 RAIL 5b] bridge-mouth cross-section ----");
    for (si = 1; si + 2 < nl->count && si < nspans; si++) {
        const int in0 = tg_span_in_bridge_run(si - 1);
        const int in1 = tg_span_in_bridge_run(si);
        int k;
        if (in0 == in1) continue;
        mouths++;
        TD5_LOG_I(LOG_TAG, "trackgen: [R13 5b] mouth @%d (%s)", si,
                  in1 ? "road -> deck" : "deck -> road");
        for (k = -10; k <= 10; k++) {
            const int s = si + k;
            double lift;
            if (s < 1 || s + 2 >= nl->count || s >= nspans) continue;
            lift = nl->v[s].y - tg_local_ground_y(nl, s);
            {   /* The sub-predicates the pavement gate is made of, so a hole is
                 * attributed to ONE of them instead of to "the sidewalk". */
                const TG_Biome *bb = &k_biomes[tg_scenery_biome_index(s)];
                const double swb = tg_city_sidewalk_w_at(nl, s, bb);
                TD5_LOG_I(LOG_TAG,
                    "trackgen: [R13 5b]   si=%d run=%d lift=%.0f deck=%d rgate=%d "
                    "railL=%u railR=%u paveL=%d paveR=%d fenceL=%d fenceR=%d "
                    "| biome=%s sw=%.0f wL=%.0f wR=%.0f fbL=%d fbR=%d "
                    "blkL=%d blkR=%d cross=%d",
                    s, tg_span_in_bridge_run(s), lift, tg_rail_deck_here(nl, s),
                    tg_span_needs_guardrail(nl, s, nspans),
                    (unsigned)s_rail_edge[s][0], (unsigned)s_rail_edge[s][1],
                    tg_r12_pave_stands(nl, s, 1), tg_r12_pave_stands(nl, s, 0),
                    tg_rail_kerbfence_here(s, 1.0), tg_rail_kerbfence_here(s, -1.0),
                    bb->name, swb,
                    tg_pavement_side_width(nl, s,  1.0, swb),
                    tg_pavement_side_width(nl, s, -1.0, swb),
                    tg_facade_built(s, 1), tg_facade_built(s, 0),
                    tg_side_blocked(s, 1.0), tg_side_blocked(s, -1.0),
                    tg_city_crossing_here(s));
            }
        }
    }
    /* The number the item is: the longest unbroken length of road where NEITHER
     * edge carries a rail of any class AND neither side carries a pavement. */
    for (si = 1; si + 2 < nl->count && si < nspans; si++) {
        const int bare = !s_rail_edge[si][0] && !s_rail_edge[si][1] &&
                         !tg_r12_pave_stands(nl, si, 1) &&
                         !tg_r12_pave_stands(nl, si, 0);
        if (bare && tg_span_near_bridge(si, 12)) {
            gap_run++;
            if (gap_run > gap_worst) { gap_worst = gap_run; gap_at = si; }
        } else {
            gap_run = 0;
        }
    }
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R13 5b] mouths=%d  longest bare stretch near a bridge "
              "= %d spans, ending @%d", mouths, gap_worst, gap_at);
    /* The number the FIX is: ramp spans, and how many of their edges ended up
     * with no rail of any class. The second must be 0 with TD5RE_R13_APPROACH
     * on -- that pair is the A/B. */
    {
        int ramp = 0, bare_edge = 0;
        for (si = 1; si + 2 < nl->count && si < nspans; si++) {
            if (!tg_r13_approach_span(si)) continue;
            ramp++;
            if (!s_rail_edge[si][0]) bare_edge++;
            if (!s_rail_edge[si][1]) bare_edge++;
        }
        TD5_LOG_I(LOG_TAG,
                  "trackgen: [R13 5b] ramp spans=%d, ramp edges with NO rail=%d "
                  "(knob TD5RE_R13_APPROACH=%s)", ramp, bare_edge,
                  td5_env_flag_on("TD5RE_R13_APPROACH") ? "on" : "off");
    }
}

/* [R12 TEX] Which PAGE does each complained-about surface actually sample?
 *
 * Round 12 items 8b and 12b describe "the tile texture" in THREE roles (snow
 * floor, coastline, bridge pillars) and the round brief's hypothesis was that
 * one shared page is behind all three. A page claim must be measured, not read
 * off a name: this prints, per span in a window, the page index every one of
 * those surfaces resolves to, plus the world size one page tile is drawn at, so
 * the shared-root question is answered by three columns of numbers.
 *
 * Read-only; opt-in via TD5RE_R12_TEX_REPORT=1, windowed by
 * TD5RE_R12_TEX_SPAN (+/- TD5RE_R12_TEX_PAD, default 6). Pair it with
 * TD5RE_R8_TEXDUMP=1 to look at the pages the indices name. */
static void tg_r12_tex_report(const TG_NodeList *nl, int nspans)
{
    int si, lo, hi, mid, pad;

    if (!td5_env_flag_off("TD5RE_R12_TEX_REPORT")) return;
    mid = td5_env_int("TD5RE_R12_TEX_SPAN", -1, -1, 100000);
    pad = td5_env_int("TD5RE_R12_TEX_PAD", 6, 0, 200);
    if (mid < 0) { lo = 0; hi = nspans - 1; }
    else { lo = mid - pad; hi = mid + pad; }
    if (lo < 0) lo = 0;
    if (hi > nspans - 1) hi = nspans - 1;

    TD5_LOG_I(LOG_TAG, "R12TEX hdr si,biome,snow,bridge,surf_page,gnd_page,"
                       "median_page,pier_style,pier_page,pylon_page,coast_page,"
                       "tl_page,tl_band,tl_tilew");
    for (si = lo; si <= hi; si++) {
        const TG_Biome *b = &k_biomes[tg_biome_cell_index(si)];
        const int inbr = tg_span_in_bridge_run(si);
        const int style = tg_bridge_style(si);
        const double band = tg_treeline_height(b);
        TD5_LOG_I(LOG_TAG,
                  "R12TEX %d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.0f,%.0f",
                  si, b->name, tg_biome_is_snow(b), inbr,
                  tg_topo_surface_page(si),
                  tg_ground_page_for_span(si, b),
                  tg_r8_median_page_ex(si, -1, 1),
                  style, tg_bridge_pier_page_for(style),
                  TD5_TG_PAGE_R9_PYLON,
                  td5_env_flag_on("TD5RE_R9_BRIDGE_COAST")
                      ? TD5_TG_PAGE_R9_SHORE : TD5_TG_PAGE_R4_COAST,
                  tg_r8_treeline_page(si), band,
                  band > 0.0 ? band : 0.0);
    }
    (void)nl;
}

/* [R12 FLORA] Report on the two ledgers filled during the build.
 *
 * PART 1 (item 4, "billboard trees fight over which draws in front"): every
 * PAIR of tree plants within a few spans of each other whose centres are close
 * enough that the two camera-facing quads overlap on screen. Camera-facing
 * billboards are all PARALLEL vertical planes, so a pair whose centres are d
 * apart differs in depth by at most d -- when d is small next to the quads'
 * width the two nearly-coplanar quads overlap over most of their area with a
 * depth difference in the last bits of the depth buffer, and which one wins
 * flips with the camera. That is the z-fight, and it is measurable here without
 * a frame: report d, the widths, and d as a fraction of the narrower quad.
 *
 * PART 2 (item 9, "no tree background for a few spans"): the per-span tree-line
 * band decision, with the span's hard cell biome next to the dithered biome the
 * emitter actually asked, so a hole can be attributed rather than guessed at.
 *
 * Read-only, opt-in via TD5RE_R12_FLORA_REPORT=1, windowed by
 * TD5RE_R12_FLORA_REPORT_LO/HI. */
static void tg_r12_flora_report(const TG_NodeList *nl, int nspans)
{
    static const char *k_why[] = { "OK", "treeline-off", "grid", "list-end",
                                   "bridge", "biome-has-no-band", "no-slots" };
    int i, j, si, lo, hi, pairs = 0, holes = 0, dither_holes = 0, runs = 0;
    int prev_hole = 0;

    if (!tg_report_wanted("TD5RE_R12_FLORA_REPORT")) return;
    lo = td5_env_int("TD5RE_R12_FLORA_REPORT_LO", 0, 0, 100000);
    hi = td5_env_int("TD5RE_R12_FLORA_REPORT_HI", 100000, 0, 100000);
    if (nspans > TD5_TG_MAX_SPANS) nspans = TD5_TG_MAX_SPANS;

    TD5_LOG_I(LOG_TAG, "trackgen: ---- [R12 FLORA] plants=%d ----",
              s_r12_flora_n);
    for (i = 0; i < s_r12_flora_n; i++) {
        const TG_R12FloraRec *a = &s_r12_flora[i];
        for (j = i + 1; j < s_r12_flora_n; j++) {
            const TG_R12FloraRec *b = &s_r12_flora[j];
            double dx, dz, d, narrow;
            /* Emitters run in several passes, so the ledger is only roughly in
             * span order -- window on the span NUMBER, never on ledger index. */
            if (b->si < a->si - 3 || b->si > a->si + 3) continue;
            dx = b->cx - a->cx; dz = b->cz - a->cz;
            d  = sqrt(dx * dx + dz * dz);
            narrow = (a->tw < b->tw) ? a->tw : b->tw;
            if (!(d < narrow * 0.5)) continue; /* quads do not overlap on screen */
            pairs++;
            if (a->si < lo || a->si > hi) continue;
            TD5_LOG_I(LOG_TAG,
                "r12flora-pair: si=%d/%d %s/%s page=%d/%d d=%.0f "
                "tw=%.0f/%.0f d/narrow=%.2f",
                a->si, b->si, a->kind, b->kind, a->page, b->page, d,
                a->tw, b->tw, d / (narrow > 0.0 ? narrow : 1.0));
        }
    }
    if (s_r12_band_seen) {
        for (si = 1; si < nspans && si + 2 < nl->count; si++) {
            const int why = s_r12_band_why[si];
            const int hard = tg_biome_cell_index(si);
            const int soft = tg_biome_for_span(si);
            const int hole = (why != TG_R12_BAND_OK);
            if (hole) {
                holes++;
                if (!prev_hole) runs++;
                /* A hole the HARD cell disagrees with is a dither artefact:
                 * the run this span belongs to does carry a tree line. */
                if (why == TG_R12_BAND_BIOME &&
                    tg_treeline_height(&k_biomes[hard]) > 0.0)
                    dither_holes++;
            }
            prev_hole = hole;
            if (si < lo || si > hi) continue;
            TD5_LOG_I(LOG_TAG,
                "r12flora-band: si=%d why=%s hard=%s soft=%s hardband=%.0f "
                "softband=%.0f",
                si, k_why[why], k_biomes[hard].name, k_biomes[soft].name,
                tg_treeline_height(&k_biomes[hard]),
                tg_treeline_height(&k_biomes[soft]));
        }
    }
    TD5_LOG_I(LOG_TAG,
              "r12flora: overlap-pairs=%d spacing-rejects=%d band-holes=%d "
              "runs=%d dither-holes=%d (of %d spans)",
              pairs, s_r12_flora_rejects, holes, runs, dither_holes, nspans);
}

/* ----------------------------------------------------------- build ------- */
int td5_trackgen_build_level(const TD5_TrackGenSpec *spec, int level_num,
                             int *out_spans)
{
    char dir[256];
    TG_NodeList nl;
    TG_Buf strip, left, right, info;
    int tally[TD5_TG_SECTION_COUNT];
    int nspans = 0, ok = 0;

    if (!spec) return 0;

    /* [R8 G1] Latch the build seed for the emitters that need a whole-TRACK
     * choice rather than a per-span one -- the start/finish banner set is one
     * object per track, so it cannot key off a span hash the way the facades
     * and the guardrails do. Latched here, not in regenerate, for the same
     * reason tg_acct_reset is: a direct build_level call must see its own seed.
     * Read only through tg_gen_seed(). */
    s_gen_seed = spec->seed;
    /* [R12 FLORA] the ledgers are per-BUILD, same argument as tg_acct_reset. */
    s_r12_flora_n = 0; s_r12_band_seen = 0; s_r12_flora_rejects = 0;
    memset(s_r12_band_why, 0, sizeof(s_r12_band_why));

    memset(&nl, 0, sizeof(nl));
    memset(&strip, 0, sizeof(strip));
    memset(&left, 0, sizeof(left));
    memset(&right, 0, sizeof(right));
    memset(&info, 0, sizeof(info));
    memset(tally, 0, sizeof(tally));

    /* [R14 GENPERF] Per-BUILD timer reset FIRST, so the reported total covers
     * the whole call and the phases before the mkdir (biome layout) are not
     * zeroed after they run -- which is exactly what hid the biome number. */
    tg_zone_reset();
    s_tg_build_t0 = td5_plat_time_us();
    s_tg_progress = 2;
    tg_xmemo_reset(0);                    /* crossing memo: disarmed until the prepass */

    /* [R2 item 24] Clear the inventory BEFORE anything is emitted. Placed here
     * rather than in regenerate so a direct build_level call (the S2 regen
     * self-check, a future editor) reports its own elements and not the
     * previous build's. */
    tg_acct_reset();

    /* [R2 item 23] Lay the biome grid out BEFORE the centerline walk: the strip
     * emitter asks tg_surface_attr for every span, so the grid has to exist by
     * then. Driven by the seed, not by the geometry RNG, so it never perturbs
     * the road. Laid out for the whole cell array rather than for nspans, so a
     * span past the ring (an appended branch corridor) still has a biome. */
    {
        TG_ZONE_BEGIN(TG_ZONE_BIOME);
        tg_biome_layout(spec->seed, spec->target_spans);
        TG_ZONE_END(TG_ZONE_BIOME);
    }

    tg_srand(spec->seed);

    snprintf(dir, sizeof(dir), "re/assets/levels/level%03d", level_num);
    _mkdir(dir);


    {
        TG_ZONE_BEGIN(TG_ZONE_CENTERLINE);
        if (!tg_build_centerline(spec, &nl, tally)) {
            TD5_LOG_E(LOG_TAG, "trackgen: centerline build failed");
            goto done;
        }
        TG_ZONE_END(TG_ZONE_CENTERLINE);
        /* Disjoint from the walk above so the two numbers still sum. */
        TG_ZONE_BEGIN(TG_ZONE_ELEVATION);
        tg_apply_elevation(spec, &nl);
        TG_ZONE_END(TG_ZONE_ELEVATION);
    }

    if (td5_env_flag_off("TD5RE_AUTOTRACK_SELFCHECK")) {
        tg_selfcheck_ranges(&nl, td5_env_int("TD5RE_AUTOTRACK_BLOCK",
                                             TD5_TG_ORIGIN_BLOCK, 1, 20));
        s_selfcheck_regen_seed = spec->seed;   /* run after the build completes */
    }

    {
        TG_ZONE_BEGIN(TG_ZONE_STRIP);
        if (!tg_emit_strip(&nl, &strip, &nspans) || nspans < 8) {
            TD5_LOG_E(LOG_TAG, "trackgen: strip emit failed (spans=%d)", nspans);
            goto done;
        }
        TG_ZONE_END(TG_ZONE_STRIP);
    }
    /* [R3 item 17] Surface any floor/road-overlap or suspect fork geometry now
     * that the strip (and s_ring_len) exist -- read-only, one-off. */
    tg_validate_geometry_safety(&nl, nspans);
    /* [R13 RAIL item 5b] The ramp mask, as soon as the node list, the elevation
     * profile and s_ring_len are all final and BEFORE any scenery reads it. */
    tg_r13_approach_build(&nl, nspans);
    /* Routes must cover exactly the ring the strip header declares. */
    /* byte0 is the lateral corridor position (0 = left rail, 255 = right).
     * Straddle the centreline symmetrically so the AI's racing line runs down
     * the middle of a road whose width varies from section to section. */
    {
        TG_ZONE_BEGIN(TG_ZONE_ROUTES);
        if (!tg_emit_routes(&nl, nspans, 96,  &left) ||
            !tg_emit_routes(&nl, nspans, 160, &right)) {
            TD5_LOG_E(LOG_TAG, "trackgen: route emit failed");
            goto done;
        }
        TG_ZONE_END(TG_ZONE_ROUTES);
    }
    {
        int linf_ok;
        TG_ZONE_BEGIN(TG_ZONE_LEVELINF);
        linf_ok = tg_emit_levelinf(spec, nspans, &info);
        TG_ZONE_END(TG_ZONE_LEVELINF);
        if (!linf_ok || info.len != 100) {
            TD5_LOG_E(LOG_TAG, "trackgen: levelinf emit failed (len=%zu)", info.len);
            goto done;
        }
    }

    {
        int wok;
        TG_ZONE_BEGIN(TG_ZONE_WRITE_STRIP);
        wok = tg_write_file(dir, "STRIP.DAT", strip.b, strip.len)
           && tg_write_file(dir, "LEFT.TRK",  left.b,  left.len)
           && tg_write_file(dir, "RIGHT.TRK", right.b, right.len)
           && tg_write_file(dir, "LEVELINF.DAT", info.b, info.len);
        TG_ZONE_END(TG_ZONE_WRITE_STRIP);
        if (!wok) {
            goto done;
        }
    }

    /* MODELS.DAT is OPT-IN (TD5RE_AUTOTRACK_SCENERY=1) and all-or-nothing:
     * its mere presence disables the procedural ribbon renderer, so if the
     * mesh bytes are wrong the road goes INVISIBLE (still drivable). Default
     * off keeps the verified ribbon path as shipped. A stale MODELS.DAT from a
     * previous opt-in run would silently keep the ribbon disabled, so remove
     * it when the knob is off. */
    {
        char models_path[320];
        snprintf(models_path, sizeof(models_path), "%s/MODELS.DAT", dir);
        /* Scenery is now DEFAULT ON (textured road, buildings, bridges,
         * biomes, tree billboards -- all verified in frame). Set
         * TD5RE_AUTOTRACK_SCENERY=0 to fall back to the untextured procedural
         * ribbon. */
        /* s_want_scenery is the BOOT suppression (see its declaration); the
         * env knob is the player's ribbon-vs-scenery choice. Both must hold.
         * When suppressed we still fall through to the removal below: the boot
         * build writes a NEW strip under a NEW seed, so a MODELS.DAT left over
         * from a previous session would no longer match its geometry, and a
         * mismatched mesh file disables the ribbon and hides the road. */
        /* [SCENERY STREAMING] Geometry + textures now, scenery on a worker
         * after the level load. MODELS.DAT is REMOVED rather than left: it does
         * not exist yet, and a stale one from the previous race would be parsed
         * against this build's brand-new strip -- wrong meshes, and its mere
         * presence switches off the ribbon that is meant to cover the road
         * until the real scenery arrives. The node list transfers out of
         * build_level's ownership here (see s_stream_nl). */
        if (s_want_stream && s_want_scenery &&
            td5_env_flag_on("TD5RE_AUTOTRACK_SCENERY")) {
            TG_Buf tex;
            char tex_path[320];
            memset(&tex, 0, sizeof(tex));
            remove(models_path);

            s_stream_nl     = nl;
            nl.v            = NULL;   /* ownership moved; `done:` must not free */
            nl.count        = 0;
            nl.cap          = 0;
            s_stream_nspans = nspans;
            s_stream_lanes  = spec->lanes;
            s_stream_pending = 1;
            snprintf(s_stream_dir, sizeof s_stream_dir, "%s", dir);

            /* Texture pages must exist BEFORE the level load, because the
             * streamed meshes reference them by page id and the load is what
             * uploads them. They are cheap (~155 ms) and independent of the
             * mesh emit, so they stay synchronous. */
            snprintf(tex_path, sizeof tex_path, "%s/TEXTURES.DAT", dir);
            if (tg_emit_textures(&tex))
                tg_write_file(dir, "TEXTURES.DAT", tex.b, tex.len);
            else
                remove(tex_path);   /* stale pages would mis-texture the meshes */
            tg_buf_free(&tex);
            TD5_LOG_I(LOG_TAG, "trackgen: STREAMED build -- geometry + textures "
                      "done, %d spans of scenery deferred to the worker",
                      nspans);
        }
        else if (s_want_scenery && td5_env_flag_on("TD5RE_AUTOTRACK_SCENERY")) {
            TG_Buf models, tex;
            memset(&models, 0, sizeof(models));
            memset(&tex, 0, sizeof(tex));
            if (tg_emit_models(&nl, nspans, spec->lanes, &models)) {
                s_r13_models_bytes = (long)models.len;   /* [R13 BAND] share */
                TG_ZONE_BEGIN(TG_ZONE_WRITE_MODELS);
                tg_write_file(dir, "MODELS.DAT", models.b, models.len);
                tg_meshtag_write(dir);   /* [PICK] sidecar, separate file */
                TG_ZONE_END(TG_ZONE_WRITE_MODELS);
            }
            else
                TD5_LOG_W(LOG_TAG, "trackgen: models emit failed; "
                          "falling back to the ribbon renderer");
            /* Texture pages are only referenced by the mesh, so they follow
             * the same gate -- without MODELS.DAT nothing samples them. */
            {
                TG_ZONE_BEGIN(TG_ZONE_TEX);
                if (tg_emit_textures(&tex))
                    tg_write_file(dir, "TEXTURES.DAT", tex.b, tex.len);
                TG_ZONE_END(TG_ZONE_TEX);
            }
            tg_buf_free(&models);
            tg_buf_free(&tex);
        } else {
            char tex_path[320];
            snprintf(tex_path, sizeof(tex_path), "%s/TEXTURES.DAT", dir);
            remove(models_path);
            remove(tex_path);
        }
    }

    {
        TG_ZONE_BEGIN(TG_ZONE_SKY);
        tg_install_sky(dir, spec->seed);
        TG_ZONE_END(TG_ZONE_SKY);
    }

    TD5_LOG_I(LOG_TAG, "trackgen: seed=%u level=%d spans=%d len=%.0f world units",
              spec->seed, level_num, nspans,
              (double)nspans * (double)spec->span_length);
    {
        int s;
        for (s = 0; s < TD5_TG_SECTION_COUNT; s++)
            TD5_LOG_I(LOG_TAG, "trackgen:   %-10s x%d (weight %d)",
                      tg_section_name((TD5_TrackGenSection)s), tally[s],
                      spec->weight[s]);
    }
    {   /* Biome layout, so a run can be checked against what is on screen.
         * [R2 item 23] Logged as MERGED runs (consecutive same-biome cells
         * collapsed) with each biome's feature weighting, so "why are there no
         * tunnels here" and "why did the scenery change" are both answerable
         * from the log. */
        int s = 0, runs = 0;
        while (s < nspans) {
            int a, z, b = tg_biome_cell_index(s);
            tg_biome_run_bounds(s, &a, &z);
            if (z >= nspans) z = nspans - 1;
            TD5_LOG_I(LOG_TAG,
                      "trackgen:   biome run %2d: spans %5d-%5d %-11s "
                      "(climate=%d urbanity=%d bridge=%d%% tunnel=%d%% surf=%s)",
                      runs, a, z, k_biomes[b].name,
                      k_biomes[b].climate, k_biomes[b].urbanity,
                      tg_biome_bridge_pct(a), tg_biome_tunnel_pct(a),
                      k_road_surf[k_biomes[b].road_surf].grip_class == 1
                          ? "tarmac" : "special");
            /* [R8 BIOME item 19] Account the snowy runs so the element
             * inventory carries the proof that the snow-coherent path fired,
             * and so its run list IS the snow layout. */
            if (s_biome_snow_seed && k_biomes[b].climate == 2)
                tg_acct_range(TG_ACCT_R8_SNOWRUN, a, z);
            runs++;
            s = z + 1;
        }
        TD5_LOG_I(LOG_TAG,
                  "trackgen: [R8 BIOME] snow-coherent=%s (knob TD5RE_R8_BIOME_SNOW=%s, "
                  "one-side-sea TD5RE_R8_BIOME_SEA=%d)",
                  s_biome_snow_seed ? "YES -- warm biomes struck, cold preferred"
                                    : "no (seed rolled no ALPINE)",
                  td5_env_flag_on("TD5RE_R8_BIOME_SNOW") ? "on" : "off",
                  td5_env_int("TD5RE_R8_BIOME_SEA", 0, 0, 1));
        TD5_LOG_I(LOG_TAG,
                  "trackgen: %d biome run(s), cell=%d spans, blend=%d spans, "
                  "time=%s",
                  runs, TD5_TG_BIOME_RUN,
                  td5_env_int("TD5RE_AUTOTRACK_BIOME_BLEND",
                              TD5_TG_BIOME_BLEND, 0, TD5_TG_BIOME_RUN / 2),
                  s_is_night ? "NIGHT" : "DAY");
    }
    {   /* Branch nodes are held in s_forks rather than emitted through a single
         * call site, so they are accounted here where the table is complete. */
        int f;
        for (f = 0; f < s_fork_count; f++) {
            tg_acct_range(TG_ACCT_BRANCH, s_forks[f].F, s_forks[f].R);
            tg_acct_range(TG_ACCT_BRANCH, s_forks[f].cbase,
                          s_forks[f].cbase + s_forks[f].len - 1);
        }
    }
    s_tg_reports_t0 = td5_plat_time_us();  /* [R14 GENPERF] diagnostic block */
    s_tg_progress = 96;
    TG_TV(TG_T_RPT_ACCT,     tg_acct_report(nspans));
    TG_TV(TG_T_RPT_RAILEDGE, tg_rail_edge_report(&nl, nspans));  /* [R9 RAILFIX] per-edge uniqueness */
    TG_TV(TG_T_RPT_R11GUARD, tg_r11_guard_report(&nl, nspans));  /* [R11 GUARD] structural-edge dump  */
    TG_TV(TG_T_RPT_R13MOUTH, tg_r13_rail_mouth_report(&nl, nspans)); /* [R13 RAIL 5b] mouth cross-section */
    TG_TV(TG_T_RPT_R12FLORA, tg_r12_flora_report(&nl, nspans));  /* [R12 FLORA] plant/band ledgers, TD5RE_TG_REPORTS */
    TG_TV(TG_T_RPT_R9BRIDGE, tg_r9_bridge_report(&nl));          /* [R9 BRIDGE] tie + dry-band evidence, TD5RE_TG_REPORTS */
    /* [R9 INFRA] The two deliverables share one accounting slot, so print the
     * split explicitly. Ponds are accounted as WATER (they are water), which
     * means the r9-infra row is the FURNITURE row -- this line is what says so,
     * and what proves the pond half fired rather than leaving it inferred from
     * a water count that a coastal run would dominate anyway. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R9 INFRA] furniture=%ld piece(s) (knob "
              "TD5RE_R9_INFRA_PROPS=%s), ponds=%ld sheet(s) (knob "
              "TD5RE_R9_INFRA_PONDS=%s)",
              s_r9_infra_props,
              td5_env_flag_on("TD5RE_R9_INFRA_PROPS") ? "on" : "off",
              s_r9_infra_ponds,
              td5_env_flag_on("TD5RE_R9_INFRA_PONDS") ? "on" : "off");
    /* [R12 PROPS] Both of this round's items change a SHARE of the furniture
     * total above, not the total, so neither shows up in it or in the element
     * inventory. This line is the round's evidence: how many benches were built
     * as a bench, and how the disc picks split into kept / wrong-place /
     * thinned. Sign totals across the three counters equal the OLD sign count,
     * which is what makes the before/after a subtraction rather than two runs
     * that happen to differ. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R12 PROPS] bench-form=%ld (knob "
              "TD5RE_R12_BENCH_FORM=%s); sign picks=%ld -> kept=%ld, "
              "dropped ctx=%ld rate=%ld (knob TD5RE_R12_SIGN_CTX=%s)",
              s_r12_bench_form,
              td5_env_flag_on("TD5RE_R12_BENCH_FORM") ? "on" : "off",
              s_r12_sign_kept + s_r12_sign_ctx + s_r12_sign_rate,
              s_r12_sign_kept, s_r12_sign_ctx, s_r12_sign_rate,
              td5_env_flag_on("TD5RE_R12_SIGN_CTX") ? "on" : "off");
    /* [R13 PROPS] Same rule as the R12 line above: every item here moves a SHARE
     * of the furniture/prop totals, never a total, so this line is the round's
     * only evidence. Awning kept+ctx equals the OLD awning count and animal
     * kept+town equals the OLD animal count, which is what makes each of them a
     * subtraction rather than two runs that happen to differ. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R13 PROPS] bench-end-uv=%ld (knob "
              "TD5RE_R13_BENCH_UV=%s); planks=%ld (knob "
              "TD5RE_R13_PLANK_CROP=%s); awning picks=%ld -> kept=%ld, "
              "dropped no-frontage=%ld, built=%ld (knob TD5RE_R13_AWNING=%s); "
              "animal picks=%ld -> kept=%ld, dropped town=%ld (knob "
              "TD5RE_R13_ANIMAL_CTX=%s)",
              s_r13_bench_end_uv,
              td5_env_flag_on("TD5RE_R13_BENCH_UV") ? "on" : "off",
              s_r13_plank_crop,
              td5_env_flag_on("TD5RE_R13_PLANK_CROP") ? "on" : "off",
              s_r13_awn_kept + s_r13_awn_ctx, s_r13_awn_kept, s_r13_awn_ctx,
              s_r13_awn_form,
              td5_env_flag_on("TD5RE_R13_AWNING") ? "on" : "off",
              s_r13_animal_kept + s_r13_animal_town,
              s_r13_animal_kept, s_r13_animal_town,
              td5_env_flag_on("TD5RE_R13_ANIMAL_CTX") ? "on" : "off");
    /* [R10 SPAN66 item 1] The PLACEMENT half's number. The ENFORCEMENT half's
     * numbers are the guard's own "rejects by kind: prop" and its residual line
     * -- the residual now re-tests furniture against the widened envelope, so
     * "0 remaining after the pass" is the class-level zero for this item. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R10 SPAN66] furniture/people refused on side-street "
              "tarmac=%ld (knob TD5RE_R10_XSTREET_GUARD=%s)",
              s_r10_prop_skipped,
              tg_r10_xstreet_guard() ? "on" : "off");
    /* [R12 GEOM item 7] The CLASS-level number for "cap every island end": how
     * many median RUNS the track has and how many end faces closed them. Two
     * caps per run is the whole property -- a leading and a trailing face -- so
     * caps == 2 * runs is the assertion, and any run that only got one cap means
     * the neighbour predicate disagreed with the emitter at one end. Both are 0
     * with TD5RE_R12_MEDIAN_CAP=0, which is the A/B. */
    TD5_LOG_I(LOG_TAG,
              "trackgen: [R12 GEOM] median island runs=%ld, end faces "
              "capped=%ld (expect 2x runs) (knob TD5RE_R12_MEDIAN_CAP=%s)",
              s_r12_median_runs, s_r12_median_caps,
              tg_r12_median_cap() ? "on" : "off");
    TG_TV(TG_T_RPT_R8BRIDGE,  tg_r8_bridge_diag(&nl));            /* [R8 BRIDGE] opt-in measurement dump */
    TG_TV(TG_T_RPT_R12SPANQ,  tg_r12_spanq(nspans));              /* [R12 item 8a] opt-in span query     */
    TG_TV(TG_T_RPT_R8TERRAIN, tg_r8_terrain_extent_report(&nl, nspans));  /* [R8 TERRAIN] opt-in, ditto */
    TG_TV(TG_T_RPT_R9TOPO,    tg_r9_topo_report(&nl, nspans));    /* [R9 TOPO] class sweep, TD5RE_TG_REPORTS */
    TG_TV(TG_T_RPT_R14UP,     tg_r14_up_report(&nl, nspans));     /* [R14 item 3] overpass surround     */
    TG_TV(TG_T_RPT_R11XCURVE, tg_r11_xcurve_report(nspans));      /* [R11 CROSS item 16] opt-in, ditto */
    TG_TV(TG_T_RPT_R12FCROSS, tg_r12_fcross_report(&nl, nspans)); /* [R12 CROSS item 5]  opt-in, ditto */
    TG_TV(TG_T_RPT_R13BAND,   tg_r13_band_report(&nl, nspans));   /* [R13 BAND items 1a/1b] ledger, TD5RE_TG_REPORTS */
    TG_TV(TG_T_RPT_R14BAND,   tg_r14_band_report(&nl, nspans));   /* [R14 COAST item 5b] run ends   */
    TG_TV(TG_T_RPT_R11WATER,  tg_r11_water_diag(&nl, nspans));    /* [R11 WATER] wet footprint    */
    TG_TV(TG_T_RPT_R12TEX,    tg_r12_tex_report(&nl, nspans));    /* [R12 TEX] page-per-surface   */
    TG_TV(TG_T_RPT_R14COAST,  tg_r14_coast_report());             /* [R14 COAST item 5a] straddles */
    /* [R14 integration] the coast report is INSIDE the REPORTS zone on purpose:
     * closing the zone above it would leave its cost out of its own timing. */
    s_tg_zone_us[TG_ZONE_REPORTS] += td5_plat_time_us() - s_tg_reports_t0;
    s_tg_zone_n[TG_ZONE_REPORTS]++;
    /* [R14 GENPERF] Last thing the build does, so the total covers everything
     * above it including the file writes. */
    tg_zone_report(td5_plat_time_us() - s_tg_build_t0);
    ok = 1;

done:
    if (out_spans) *out_spans = nspans;
    free(nl.v);
    tg_buf_free(&strip);
    tg_buf_free(&left);
    tg_buf_free(&right);
    tg_buf_free(&info);
    return ok;
}
