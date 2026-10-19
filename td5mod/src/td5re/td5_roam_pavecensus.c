/**
 * td5_roam_pavecensus.c -- GEO FREE ROAM G4b: pavement vs ground-as-drawn census (PORT-ONLY, DEV-ONLY).
 *
 * Builds the SCENERY layer of every tile the caller hands over and walks the pavement faces
 * (pages SIDEWALK and the F4a lawn page) of its records. Each vertex is compared with the ground as
 * the renderer draws it (the caller's ground callback: lattice + G4 clamp + drop). Per face class:
 *   back   the vertical face under a slab's outer edge (colour 0xFFA8A8A8): gap = its bottom vertex
 *          above the drawn ground. > 0 is a hole between the face and the ground (the slab floats)
 *   slab   a white SIDEWALK quad whose vertices 2, 3 are the top points of back faces (a pavement slab
 *          of the F4b emitter: vertices 0, 1 on the kerb line, 2, 3 on the outer edge):
 *          rise  = outer edge - kerb edge (the cross-fall; the emitter caps it at +-0.25 * width)
 *          above = outer edge height above the drawn ground, minus the nominal (kerb height + drop)
 *   flat   any face with its normal up on those pages (slabs, footpaths, lawns): the lowest vertex under
 *          the drawn ground (the ground covers it)
 * Output <log>/roam_pavement_census.{txt,csv}. Knob in td5_roam_render.c: TD5RE_ROAM_PAVE_CENSUS=1.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5_roam_pavecensus.h"

#ifndef TD5RE_RELEASE

#include "td5_trackgen_internal.h"

#define PC_UPM 430.0
#define PC_CM  (PC_UPM / 100.0)

typedef struct { float val, aux; double x, z, y; int tx, tz, cls; } PcRow;   /* cls 0 slab above, 1 back gap, 2 slab cross-fall, 3 sunk */

static const double k_thr_cm[6] = { 25, 50, 100, 200, 400, 800 };

typedef struct { uint64_t *k; uint32_t cap; } PcSet;

static uint64_t pc_key(float x, float z)
{
    uint32_t a, b;
    memcpy(&a, &x, 4); memcpy(&b, &z, 4);
    return ((uint64_t)a << 32) | b;
}

static uint32_t pc_slot(const PcSet *s, uint64_t key) { return (uint32_t)((key * 0x9E3779B97F4A7C15ull) >> 40) & (s->cap - 1); }

static void pc_set_add(PcSet *s, uint64_t key)
{
    uint32_t h = pc_slot(s, key);
    while (s->k[h] && s->k[h] != key + 1u) h = (h + 1) & (s->cap - 1);
    s->k[h] = key + 1u;
}

static int pc_set_has(const PcSet *s, uint64_t key)
{
    uint32_t h = pc_slot(s, key);
    while (s->k[h]) { if (s->k[h] == key + 1u) return 1; h = (h + 1) & (s->cap - 1); }
    return 0;
}

static uint32_t pc_rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static float    pc_rdf(const uint8_t *p)  { float v; memcpy(&v, p, 4); return v; }

static int pc_cmp(const void *a, const void *b)
{
    const PcRow *x = (const PcRow *)a, *y = (const PcRow *)b;
    if (x->cls != y->cls) return x->cls < y->cls ? -1 : 1;
    return fabsf(x->val) > fabsf(y->val) ? -1 : (fabsf(x->val) < fabsf(y->val) ? 1 : 0);
}

int td5_roam_pave_census(const RoamTile *tiles, int ntile, TD5_RoamScnSource *scn, TD5_RoamHeight *hc,
                         TD5_RoamGroundFn ground, void *gu, double drop_units, const double proj[4], const char *outprefix)
{
    const double nominal = TD5_TG_KERB_H + drop_units;
    long touch = 0, nflat = 0, nback = 0, nslab = 0, nvert = 0, nnog = 0, sink0 = 0, gap0 = 0;
    long hab[6] = { 0 }, hgap[6] = { 0 }, hrise[6] = { 0 }, hfall[6] = { 0 };
    double worst_above = -1e30, worst_gap = -1e30, worst_rise = -1e30, worst_fall = 1e30, lo_min = 1e30;
    PcRow *keep = NULL;
    long nkeep = 0, capkeep = 0;
    int k, tn = 0;
    char path[600];
    FILE *o, *csv;
    const char *ld = td5_plat_log_dir();
    uint32_t t0 = td5_plat_time_ms();
    PcSet set;
    double dbg_x = 0.0, dbg_z = 0.0, dbg_r = 0.0;
    { const char *e = getenv("TD5RE_PAVE_DBG"); if (e) sscanf(e, "%lf,%lf,%lf", &dbg_x, &dbg_z, &dbg_r); }

    set.cap = 1u << 18;
    set.k = (uint64_t *)malloc((size_t)set.cap * sizeof(uint64_t));
    if (!set.k) return 1;

#define PC_KEEP(CLS, VAL, AUX, X, Y, Z) do { \
        if (nkeep == capkeep) { capkeep = capkeep ? capkeep * 2 : 4096; keep = (PcRow *)realloc(keep, (size_t)capkeep * sizeof *keep); \
            if (!keep) return 1; } \
        keep[nkeep].val = (float)(VAL); keep[nkeep].aux = (float)(AUX); keep[nkeep].x = (X); keep[nkeep].y = (Y); keep[nkeep].z = (Z); \
        keep[nkeep].tx = sc->tx; keep[nkeep].tz = sc->tz; keep[nkeep].cls = (CLS); nkeep++; } while (0)

    for (k = 0; k < ntile; k++) {
        const RoamTile *t = &tiles[k];
        TD5_RoamScenery *sc = td5_roam_scn_build(scn, t, hc);
        uint32_t r;
        int pass;
        if (!sc) continue;
        tn++;
        memset(set.k, 0, (size_t)set.cap * sizeof(uint64_t));
        for (pass = 0; pass < 2; pass++) {
            for (r = 0; r < sc->nrec; r++) {
                const uint8_t *rec = sc->blob + sc->rec_off[r];
                const size_t avail = sc->blob_bytes - sc->rec_off[r];
                const uint32_t ncmd = pc_rd32(rec + 4), nv = pc_rd32(rec + 8);
                const uint32_t cmd_off = pc_rd32(rec + TD5_MESH_DISK_OFF_COMMANDS), vtx_off = pc_rd32(rec + TD5_MESH_DISK_OFF_VERTICES);
                uint32_t ci, cursor = 0;
                if (cmd_off + ncmd * 16u > avail || vtx_off + nv * 44u > avail) continue;
                for (ci = 0; ci < ncmd; ci++) {
                    const uint8_t *cm = rec + cmd_off + ci * 16u;
                    const int page = (int)(int16_t)(pc_rd32(cm) >> 16);
                    const uint32_t tc = pc_rd32(cm + 8) & 0xFFFFu, qc = pc_rd32(cm + 8) >> 16;
                    const int ours = page == TD5_TG_PAGE_SIDEWALK || page == TD5_TG_PAGE_R3_BLOCK + 0;
                    uint32_t p;
                    for (p = 0; p < tc + qc; p++) {
                        const int nvp = p < tc ? 3 : 4;
                        double x[4], y[4], z[4], g[4], rel[4];
                        float fx[4], fz[4];
                        uint32_t col = 0;
                        int q, okv = 1;
                        if (cursor + (uint32_t)nvp > nv) break;
                        for (q = 0; q < nvp; q++) {
                            const uint8_t *v = rec + vtx_off + (cursor + (uint32_t)q) * 44u;
                            fx[q] = pc_rdf(v); fz[q] = pc_rdf(v + 8);
                            x[q] = (double)fx[q] + (double)sc->org_x;
                            y[q] = (double)pc_rdf(v + 4);
                            z[q] = (double)fz[q] + (double)sc->org_z;
                            if (q == 0) col = pc_rd32(v + 24);
                        }
                        cursor += (uint32_t)nvp;
                        if (!ours) continue;
                        if (pass == 0) {
                            if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFA8A8A8u && nvp == 4) {
                                pc_set_add(&set, pc_key(fx[0], fz[0]));
                                pc_set_add(&set, pc_key(fx[1], fz[1]));
                            }
                            continue;
                        }
                        if (dbg_r > 0.0 && hypot(x[0] - dbg_x, z[0] - dbg_z) < dbg_r) {
                            double gg[4]; int okk[4];
                            for (q = 0; q < nvp; q++) okk[q] = ground(gu, x[q], z[q], &gg[q]);
                            fprintf(stderr, "PAVEDBG page %d col %08x n%d:", page, col, nvp);
                            for (q = 0; q < nvp; q++) fprintf(stderr, " (%.0f %.0f y%.0f g%s%.0f)", x[q], z[q], y[q], okk[q] ? "" : "!", okk[q] ? gg[q] : 0.0);
                            fputc(10, stderr);
                        }
                        for (q = 0; q < nvp; q++) {
                            if (!ground(gu, x[q], z[q], &g[q])) { okv = 0; break; }
                            rel[q] = y[q] - g[q];
                        }
                        nvert += nvp;
                        if (!okv) { nnog++; continue; }
                        {
                            const double ax = x[1] - x[0], ay = y[1] - y[0], az = z[1] - z[0];
                            const double bx = x[nvp - 1] - x[0], by = y[nvp - 1] - y[0], bz = z[nvp - 1] - z[0];
                            const double nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                            const double nl = sqrt(nx * nx + ny * ny + nz * nz);
                            const int up = nl > 0.0 && fabs(ny) / nl > 0.7;
                            if (up) {
                                double lo = 1e30;
                                for (q = 0; q < nvp; q++) if (rel[q] < lo) lo = rel[q];
                                nflat++;
                                if (lo < lo_min) lo_min = lo;
                                if (lo < 0.0) { sink0++; PC_KEEP(3, lo / PC_CM, 0, x[0], y[0], z[0]); }
                                else if (lo < 10.0 * PC_CM) { touch++; PC_KEEP(4, lo / PC_CM, 0, x[0], y[0], z[0]); }   /* z-fights the ground */
                                if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFFFFFFFu && nvp == 4 &&
                                    pc_set_has(&set, pc_key(fx[2], fz[2])) && pc_set_has(&set, pc_key(fx[3], fz[3]))) {
                                    const double rise = 0.5 * (y[2] + y[3]) - 0.5 * (y[0] + y[1]);
                                    const double above = (rel[2] > rel[3] ? rel[2] : rel[3]) - nominal;
                                    nslab++;
                                    if (above > worst_above) worst_above = above;
                                    if (rise > worst_rise) worst_rise = rise;
                                    if (rise < worst_fall) worst_fall = rise;
                                    for (q = 0; q < 6; q++) {
                                        if (above > k_thr_cm[q] * PC_CM) hab[q]++;
                                        if (rise > k_thr_cm[q] * PC_CM) hrise[q]++;
                                        if (-rise > k_thr_cm[q] * PC_CM) hfall[q]++;
                                    }
                                    if (above > 25.0 * PC_CM) PC_KEEP(0, above / PC_CM, rise / PC_CM, x[2], y[2], z[2]);
                                    if (rise > 25.0 * PC_CM || -rise > 25.0 * PC_CM) PC_KEEP(2, rise / PC_CM, above / PC_CM, x[2], y[2], z[2]);
                                }
                            } else if (page == TD5_TG_PAGE_SIDEWALK && col == 0xFFA8A8A8u) {
                                double bot = 1e30;
                                int bq = 0;
                                for (q = 0; q < nvp; q++) if (rel[q] < bot) { bot = rel[q]; bq = q; }
                                nback++;
                                if (bot > worst_gap) worst_gap = bot;
                                if (bot > 0.0) gap0++;
                                for (q = 0; q < 6; q++) if (bot > k_thr_cm[q] * PC_CM) hgap[q]++;
                                if (bot > 0.0) PC_KEEP(1, bot / PC_CM, 0, x[bq], y[bq], z[bq]);
                            }
                        }
                    }
                }
            }
        }
        td5_roam_scn_free(sc);
    }
    qsort(keep, (size_t)nkeep, sizeof *keep, pc_cmp);
    snprintf(path, sizeof path, "%sroam_pavement_census.txt", ld ? ld : "");
    o = fopen(path, "w");
    snprintf(path, sizeof path, "%sroam_pavement_census.csv", ld ? ld : "");
    csv = fopen(path, "w");
    if (o) {
        int h;
        fprintf(o, "roam pavement census (G4b): %d tiles, %ld pavement vertices (%ld over no ground), nominal slab height above the drawn "
                   "ground %.1f cm (kerb %.1f + drop %.1f)\n", tn, nvert, nnog, nominal / PC_CM, TD5_TG_KERB_H / PC_CM, drop_units / PC_CM);
        fprintf(o, "BACK faces (under a slab outer edge): %ld; bottom ABOVE the drawn ground (a hole under the slab): %ld, worst %.1f cm\n",
                nback, gap0, worst_gap / PC_CM);
        fprintf(o, "SLABS (F4b pavement quads): %ld; outer edge above the drawn ground beyond nominal: worst %.1f cm; cross-fall outer - kerb: "
                   "worst up %.1f cm, worst down %.1f cm\n", nslab, worst_above / PC_CM, worst_rise / PC_CM, worst_fall / PC_CM);
        fprintf(o, "FLAT faces (slab / footpath / lawn, normal up): %ld; lowest vertex vs ground %.1f cm; faces with a vertex under the ground %ld\n",
                nflat, lo_min / PC_CM, sink0);
        fprintf(o, "threshold_cm  back_gap_above  slab_outer_above_nominal  slab_rise_up  slab_fall_down\n");
        for (h = 0; h < 6; h++) fprintf(o, "%5.0f  %10ld  %10ld  %10ld  %10ld\n", k_thr_cm[h], hgap[h], hab[h], hrise[h], hfall[h]);
        fprintf(o, "\nworst rows: cls(0 slab above nominal, 1 back gap, 2 cross-fall, 3 sunk, 4 within 10 cm of the ground) value_cm aux_cm tile place_x place_z y lat lon\n");
        for (k = 0; k < nkeep && k < 80; k++) {
            const PcRow *f = &keep[k];
            fprintf(o, "%d %.1f %.1f %d,%d %.0f %.0f %.0f %.6f %.6f\n", f->cls, f->val, f->aux, f->tx, f->tz, f->x, f->z, f->y,
                    proj && proj[1] > 0 ? proj[0] + f->z / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + f->x / (proj[3] * PC_UPM) : 0.0);
        }
        fclose(o);
    }
    if (csv) {
        fprintf(csv, "cls,value_cm,aux_cm,tx,tz,place_x,place_z,y,lat,lon\n");
        for (k = 0; k < nkeep; k++) {
            const PcRow *f = &keep[k];
            fprintf(csv, "%d,%.1f,%.1f,%d,%d,%.0f,%.0f,%.0f,%.6f,%.6f\n", f->cls, f->val, f->aux, f->tx, f->tz, f->x, f->z, f->y,
                    proj && proj[1] > 0 ? proj[0] + f->z / (proj[1] * PC_UPM) : 0.0,
                    proj && proj[3] > 0 ? proj[2] + f->x / (proj[3] * PC_UPM) : 0.0);
        }
        fclose(csv);
    }
    fprintf(stderr, "PAVECENSUS: %d tiles, back faces %ld (hole under %ld, >25cm %ld), slabs %ld (above nominal >50cm %ld, >100cm %ld; "
            "cross-fall up >50cm %ld, down >50cm %ld), flat %ld (a vertex under the ground %ld), %u ms\n", tn, nback, gap0, hgap[0], nslab,
            hab[1], hab[2], hrise[1], hfall[1], nflat, sink0, (unsigned)(td5_plat_time_ms() - t0));
    (void)outprefix;
    free(keep); free(set.k);
    return 1;
}

#endif /* !TD5RE_RELEASE */
