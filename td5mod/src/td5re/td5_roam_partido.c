/**
 * td5_roam_partido.c -- GEO FREE ROAM: the place boundary + the light street layer (PORT-ONLY, F9b).
 * Contract: td5_roam_partido.h.
 *
 * The boundary is PLACE.JSON boundary.rings ([lat, lon] pairs) projected with the place's own
 * projection block (the maths of td5_roam_latlon_to_place, td5_roam_mode.c). The street layer reads
 * every ROADS tile listed in tiles/INDEX.JSON once: per record the class, lanes and "points" (place
 * frame), with the drivable filter of td5_roam_way_drop_reason reduced to what a record says
 * (bus stop / razed / services / raceway classes, area=yes, parking aisles, drive-throughs out).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include "td5_platform.h"
#include "td5_config.h"         /* [roam G6] td5_env_int */
#include "td5_geo.h"
#include "td5_geo_roads.h"
#include "td5_roam_tile.h"
#include "td5_roam_partido.h"
#include "deps/cjson/cJSON.h"

#define LOG_TAG "roam"

#define RP_UPM        430.0
#define RP_RAD        0.017453292519943295
#define RP_WGS_A      6378137.0
#define RP_WGS_F      (1.0 / 298.257223563)
#define RP_RING_MAX   8
#define RP_CELL       430000.0          /* street grid cell: 1 km */

typedef struct { double lat0, lon0, upm, mdlat, mdlon, cs, sn, offx, offz; } RpProj;

typedef struct {
    int32_t minx, minz, maxx, maxz;
    int     pt0, npt;
    int     name_id;            /* [roam HB] interned street-name id, -1 unnamed */
    uint8_t klass, lanes;
} RpWay;

typedef struct {
    /* boundary (main thread) */
    char     place[64];
    int      loaded, have;
    double  *ring[RP_RING_MAX];
    int      nring_pt[RP_RING_MAX];
    int      nring;
    double   bminx, bminz, bmaxx, bmaxz;
    /* street layer (loader thread writes, then publishes `ready`) */
    char     s_place[64];
    volatile long ready, loading;
    RpWay   *way;   int nway;
    int32_t *pt;    int npt;
    char    *npool;  int *noff;  int nname;      /* [roam HB] interned street names (UTF-8) */
    int      gnx, gnz;
    double   gx0, gz0;
    int     *gstart, *gitem;     /* CSR: way indices per cell */
    int     *stamp;  int stamp_gen;
    double   ms;
} RpState;

static RpState s_pt;

/* ------------------------------------------------------------ helpers ------ */

static char *rp_slurp(const char *path, size_t cap)
{
    TD5_File *f = td5_plat_file_open(path, "rb");
    int64_t n;
    char *buf;
    if (!f) return NULL;
    n = td5_plat_file_size(f);
    if (n <= 0 || (size_t)n > cap) { td5_plat_file_close(f); return NULL; }
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf) { td5_plat_file_close(f); return NULL; }
    if (td5_plat_file_read(f, buf, (size_t)n) != (size_t)n) { free(buf); td5_plat_file_close(f); return NULL; }
    buf[n] = '\0';
    td5_plat_file_close(f);
    return buf;
}

static int rp_proj_of(const cJSON *root, RpProj *p)
{
    const cJSON *pr = cJSON_GetObjectItem(root, "projection");
    const cJSON *a, *o, *u, *r, *ox, *oz;
    double e2, s, w, phi, rot;
    if (!pr || !cJSON_IsObject(pr)) return 0;
    a  = cJSON_GetObjectItem(pr, "lat0");
    o  = cJSON_GetObjectItem(pr, "lon0");
    u  = cJSON_GetObjectItem(pr, "units_per_metre");
    r  = cJSON_GetObjectItem(pr, "rotation_rad");
    ox = cJSON_GetObjectItem(pr, "offset_x");
    oz = cJSON_GetObjectItem(pr, "offset_z");
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(o)) return 0;
    p->lat0 = a->valuedouble;
    p->lon0 = o->valuedouble;
    p->upm  = cJSON_IsNumber(u) ? u->valuedouble : RP_UPM;
    rot     = cJSON_IsNumber(r) ? r->valuedouble : 0.0;
    p->cs   = cos(rot);
    p->sn   = sin(rot);
    p->offx = cJSON_IsNumber(ox) ? ox->valuedouble : 0.0;
    p->offz = cJSON_IsNumber(oz) ? oz->valuedouble : 0.0;
    e2  = RP_WGS_F * (2.0 - RP_WGS_F);
    phi = p->lat0 * RP_RAD;
    s   = sin(phi);
    w   = 1.0 - e2 * s * s;
    p->mdlat = RP_RAD * RP_WGS_A * (1.0 - e2) / pow(w, 1.5);
    p->mdlon = RP_RAD * RP_WGS_A * cos(phi) / sqrt(w);
    return 1;
}

static void rp_project(const RpProj *p, double lat, double lon, double *x, double *z)
{
    const double e = (lon - p->lon0) * p->mdlon, n = (lat - p->lat0) * p->mdlat;
    *x = (e * p->cs - n * p->sn) * p->upm + p->offx;
    *z = (e * p->sn + n * p->cs) * p->upm + p->offz;
}

/* ------------------------------------------------------------ boundary ----- */

static void rp_free_boundary(void)
{
    int r;
    for (r = 0; r < s_pt.nring; r++) free(s_pt.ring[r]);
    memset(s_pt.ring, 0, sizeof s_pt.ring);
    memset(s_pt.nring_pt, 0, sizeof s_pt.nring_pt);
    s_pt.nring = 0;
    s_pt.have = 0;
}

int td5_roam_partido_load(const char *place)
{
    char path[512];
    char *json;
    cJSON *root, *b, *rings;
    RpProj pj;
    int r;
    if (!place || !place[0]) return 0;
    if (s_pt.loaded && strcmp(s_pt.place, place) == 0) return s_pt.have;
    rp_free_boundary();
    snprintf(s_pt.place, sizeof s_pt.place, "%s", place);
    s_pt.loaded = 1;
    td5_geo_source_path(path, sizeof path, place, "PLACE.JSON");
    json = rp_slurp(path, (size_t)16 * 1024 * 1024);
    if (!json) return 0;
    root = cJSON_Parse(json);
    free(json);
    if (!root) return 0;
    b = cJSON_GetObjectItem(root, "boundary");
    rings = b ? cJSON_GetObjectItem(b, "rings") : NULL;
    if (rings && cJSON_IsArray(rings) && rp_proj_of(root, &pj)) {
        const int nr = cJSON_GetArraySize(rings);
        s_pt.bminx = s_pt.bminz = 1e300;
        s_pt.bmaxx = s_pt.bmaxz = -1e300;
        for (r = 0; r < nr && s_pt.nring < RP_RING_MAX; r++) {
            const cJSON *ring = cJSON_GetArrayItem(rings, r), *it;
            const int n = cJSON_GetArraySize(ring);
            double *xz;
            int k = 0;
            if (n < 3) continue;
            xz = (double *)malloc((size_t)n * 2u * sizeof(double));
            if (!xz) break;
            cJSON_ArrayForEach(it, ring) {
                const cJSON *la = cJSON_GetArrayItem(it, 0), *lo = cJSON_GetArrayItem(it, 1);
                if (!cJSON_IsNumber(la) || !cJSON_IsNumber(lo) || k >= n) continue;
                rp_project(&pj, la->valuedouble, lo->valuedouble, &xz[2 * k], &xz[2 * k + 1]);
                if (xz[2 * k] < s_pt.bminx) s_pt.bminx = xz[2 * k];
                if (xz[2 * k] > s_pt.bmaxx) s_pt.bmaxx = xz[2 * k];
                if (xz[2 * k + 1] < s_pt.bminz) s_pt.bminz = xz[2 * k + 1];
                if (xz[2 * k + 1] > s_pt.bmaxz) s_pt.bmaxz = xz[2 * k + 1];
                k++;
            }
            if (k < 3) { free(xz); continue; }
            s_pt.ring[s_pt.nring] = xz;
            s_pt.nring_pt[s_pt.nring] = k;
            s_pt.nring++;
        }
        s_pt.have = s_pt.nring > 0;
    }
    cJSON_Delete(root);
    TD5_LOG_I(LOG_TAG, "ROAM_PARTIDO: place=%s boundary %s (%d ring(s), bbox %.1f x %.1f km)", place,
              s_pt.have ? "loaded" : "absent", s_pt.nring,
              s_pt.have ? (s_pt.bmaxx - s_pt.bminx) / RP_UPM / 1000.0 : 0.0,
              s_pt.have ? (s_pt.bmaxz - s_pt.bminz) / RP_UPM / 1000.0 : 0.0);
    return s_pt.have;
}

int td5_roam_partido_has_boundary(void) { return s_pt.have; }

int td5_roam_partido_contains(double px, double pz)
{
    int r, inside = 0;
    if (!s_pt.have) return 1;
    if (px < s_pt.bminx || px > s_pt.bmaxx || pz < s_pt.bminz || pz > s_pt.bmaxz) return 0;
    for (r = 0; r < s_pt.nring; r++) {
        const double *p = s_pt.ring[r];
        const int n = s_pt.nring_pt[r];
        int i, j, c = 0;
        for (i = 0, j = n - 1; i < n; j = i++) {
            const double xi = p[2 * i], zi = p[2 * i + 1], xj = p[2 * j], zj = p[2 * j + 1];
            if (((zi > pz) != (zj > pz)) && (px < (xj - xi) * (pz - zi) / (zj - zi) + xi)) c = !c;
        }
        if (c) inside = !inside;     /* even-odd over every ring (holes would toggle back) */
    }
    return inside;
}

double td5_roam_partido_dist_m(double px, double pz)
{
    double best = 1e300;
    int r;
    if (!s_pt.have || td5_roam_partido_contains(px, pz)) return 0.0;
    for (r = 0; r < s_pt.nring; r++) {
        const double *p = s_pt.ring[r];
        const int n = s_pt.nring_pt[r];
        int i, j;
        for (i = 0, j = n - 1; i < n; j = i++) {
            const double ax = p[2 * j], az = p[2 * j + 1], bx = p[2 * i], bz = p[2 * i + 1];
            const double ex = bx - ax, ez = bz - az, l2 = ex * ex + ez * ez;
            double t = l2 > 0.0 ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0, dx, dz, d;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            dx = ax + ex * t - px; dz = az + ez * t - pz;
            d = dx * dx + dz * dz;
            if (d < best) best = d;
        }
    }
    return sqrt(best) / RP_UPM;
}

int td5_roam_partido_tile_ok(const RoamGrid *g, int tx, int tz)
{
    double c[4][2], cx = 0.0, cz = 0.0;
    int k;
    if (!g) return 0;
    if (!s_pt.have) return 1;
    roam_grid_corners(g, tx, tz, c);
    for (k = 0; k < 4; k++) { cx += c[k][0] * 0.25; cz += c[k][1] * 0.25; }
    /* half diagonal of a 256 m tile = 181 m, + the build halo */
    return td5_roam_partido_dist_m(cx, cz) <= 181.1 + ROAM_HALO_M;
}

int td5_roam_partido_ring_count(void) { return s_pt.nring; }

const double *td5_roam_partido_ring(int r, int *n)
{
    if (r < 0 || r >= s_pt.nring) { if (n) *n = 0; return NULL; }
    if (n) *n = s_pt.nring_pt[r];
    return s_pt.ring[r];
}

/* ------------------------------------------------------------ street layer -- */

typedef struct { void *p; int n, cap; size_t sz; } RpVec;

static void *rp_vec_add(RpVec *v, int count)
{
    if (v->n + count > v->cap) {
        int nc = v->cap ? v->cap : 4096;
        void *np;
        while (nc < v->n + count) nc *= 2;
        np = realloc(v->p, (size_t)nc * v->sz);
        if (!np) return NULL;
        v->p = np; v->cap = nc;
    }
    v->n += count;
    return (char *)v->p + (size_t)(v->n - count) * v->sz;
}

static int rp_class(const char *s)
{
    if (!s || !s[0]) return TD5_GEO_RC_UNKNOWN;
    if (!strncmp(s, "motorway", 8))  return TD5_GEO_RC_MOTORWAY;
    if (!strncmp(s, "trunk", 5))     return TD5_GEO_RC_TRUNK;
    if (!strncmp(s, "primary", 7))   return TD5_GEO_RC_PRIMARY;
    if (!strncmp(s, "secondary", 9)) return TD5_GEO_RC_SECONDARY;
    if (!strncmp(s, "tertiary", 8))  return TD5_GEO_RC_TERTIARY;
    if (!strcmp(s, "unclassified"))  return TD5_GEO_RC_UNCLASSIFIED;
    if (!strcmp(s, "residential"))   return TD5_GEO_RC_RESIDENTIAL;
    if (!strcmp(s, "living_street")) return TD5_GEO_RC_LIVING;
    if (!strcmp(s, "service"))       return TD5_GEO_RC_SERVICE;
    return TD5_GEO_RC_UNKNOWN;
}

static int rp_drivable(const cJSON *r, const char *cls)
{
    const cJSON *tags = cJSON_GetObjectItem(r, "tags");
    if (cls && (!strcmp(cls, "bus_stop") || !strcmp(cls, "razed") || !strcmp(cls, "services") ||
                !strcmp(cls, "raceway")))
        return 0;
    if (tags) {
        const cJSON *ar = cJSON_GetObjectItem(tags, "area"), *sv = cJSON_GetObjectItem(tags, "service");
        if (cJSON_IsString(ar) && !strcmp(ar->valuestring, "yes")) return 0;
        if (cJSON_IsString(sv) && (!strcmp(sv->valuestring, "parking_aisle") || !strcmp(sv->valuestring, "drive-through")))
            return 0;
    }
    return 1;
}

/* [roam HB] street-name interning: name -> id; id indexes the offset table into the char pool.
 * Loader thread only. */
typedef struct { RpVec pool, off; int *tab; int tab_n; } RpNames;

static int rp_name_intern(RpNames *nm, const char *s)
{
    uint32_t h = 2166136261u;
    const char *c;
    const int len = (int)strlen(s);
    int slot, start;
    char *d;
    int *o;
    for (c = s; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    slot = (int)(h & (uint32_t)(nm->tab_n - 1));
    while (nm->tab[slot]) {
        const int id = nm->tab[slot] - 1;
        if (!strcmp((const char *)nm->pool.p + ((const int *)nm->off.p)[id], s)) return id;
        slot = (slot + 1) & (nm->tab_n - 1);
    }
    if (nm->off.n + 1 >= nm->tab_n / 2) return -1;       /* table nearly full: leave the rest unnamed */
    start = nm->pool.n;
    d = (char *)rp_vec_add(&nm->pool, len + 1);
    if (!d) return -1;
    memcpy(d, s, (size_t)len + 1);
    o = (int *)rp_vec_add(&nm->off, 1);
    if (!o) { nm->pool.n = start; return -1; }
    *o = start;
    nm->tab[slot] = nm->off.n;
    return nm->off.n - 1;
}

static void rp_streets_load(const char *place)
{
    char path[512], idx[512];
    char *json;
    cJSON *root, *layers, *roads, *tiles, *t;
    RpVec ways = { NULL, 0, 0, sizeof(RpWay) }, pts = { NULL, 0, 0, sizeof(int32_t) };
    RpNames nm;
    const uint32_t t0 = td5_plat_time_ms();
    int files = 0, i;
    memset(&nm, 0, sizeof nm);
    nm.pool.sz = 1; nm.off.sz = sizeof(int); nm.tab_n = 1 << 17;
    nm.tab = (int *)calloc((size_t)nm.tab_n, sizeof(int));
    td5_geo_source_path(idx, sizeof idx, place, "tiles/INDEX.JSON");
    json = nm.tab ? rp_slurp(idx, (size_t)64 * 1024 * 1024) : NULL;
    if (!json) { free(nm.tab); TD5_LOG_W(LOG_TAG, "ROAM_PARTIDO: no %s, no street layer", idx); return; }
    root = cJSON_Parse(json);
    free(json);
    if (!root) return;
    layers = cJSON_GetObjectItem(root, "layers");
    roads = layers ? cJSON_GetObjectItem(layers, "ROADS") : NULL;
    tiles = roads ? cJSON_GetObjectItem(roads, "tiles") : NULL;
    cJSON_ArrayForEach(t, tiles) {
        const cJSON *ix = cJSON_GetArrayItem(t, 0), *iz = cJSON_GetArrayItem(t, 1);
        char rel[96];
        char *body;
        cJSON *doc, *arr, *r;
        if (!cJSON_IsNumber(ix) || !cJSON_IsNumber(iz)) continue;
        snprintf(rel, sizeof rel, "tiles/ROADS_%d_%d.JSON", ix->valueint, iz->valueint);
        td5_geo_source_path(path, sizeof path, place, rel);
        body = rp_slurp(path, (size_t)64 * 1024 * 1024);
        if (!body) continue;
        doc = cJSON_Parse(body);
        free(body);
        if (!doc) continue;
        files++;
        arr = cJSON_GetObjectItem(doc, "roads");
        cJSON_ArrayForEach(r, arr) {
            const cJSON *cl = cJSON_GetObjectItem(r, "class"), *ln = cJSON_GetObjectItem(r, "lanes");
            const cJSON *pa = cJSON_GetObjectItem(r, "points"), *p;
            const char *cls = cJSON_IsString(cl) ? cl->valuestring : NULL;
            const int np = cJSON_GetArraySize(pa);
            RpWay *w;
            int32_t *dst;
            int k = 0;
            if (np < 2 || !rp_drivable(r, cls)) continue;
            w = (RpWay *)rp_vec_add(&ways, 1);
            dst = (int32_t *)rp_vec_add(&pts, 2 * np);
            if (!w || !dst) goto oom;
            w->pt0 = (pts.n - 2 * np) / 2;
            {
                const cJSON *nmj = cJSON_GetObjectItem(r, "name");
                w->name_id = (cJSON_IsString(nmj) && nmj->valuestring[0]) ? rp_name_intern(&nm, nmj->valuestring) : -1;
            }
            w->klass = (uint8_t)rp_class(cls);
            w->lanes = (uint8_t)(cJSON_IsNumber(ln) && ln->valueint > 0 && ln->valueint < 16 ? ln->valueint : 2);
            w->minx = w->minz = INT32_MAX;
            w->maxx = w->maxz = INT32_MIN;
            cJSON_ArrayForEach(p, pa) {
                const cJSON *x = cJSON_GetObjectItem(p, "x"), *z = cJSON_GetObjectItem(p, "z");
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(z) || k >= np) continue;
                dst[2 * k]     = (int32_t)floor(x->valuedouble + 0.5);
                dst[2 * k + 1] = (int32_t)floor(z->valuedouble + 0.5);
                if (dst[2 * k] < w->minx) w->minx = dst[2 * k];
                if (dst[2 * k] > w->maxx) w->maxx = dst[2 * k];
                if (dst[2 * k + 1] < w->minz) w->minz = dst[2 * k + 1];
                if (dst[2 * k + 1] > w->maxz) w->maxz = dst[2 * k + 1];
                k++;
            }
            w->npt = k;
            pts.n -= 2 * (np - k);
            if (k < 2) ways.n--;
        }
        cJSON_Delete(doc);
    }
    cJSON_Delete(root);
    root = NULL;
    if (ways.n == 0) goto oom;
    {
        /* the 1 km bucket grid (CSR) over the ways' bboxes */
        const RpWay *W = (const RpWay *)ways.p;
        double x0 = 1e300, z0 = 1e300, x1 = -1e300, z1 = -1e300;
        int nx, nz, total = 0, *cnt;
        for (i = 0; i < ways.n; i++) {
            if (W[i].minx < x0) x0 = W[i].minx;
            if (W[i].minz < z0) z0 = W[i].minz;
            if (W[i].maxx > x1) x1 = W[i].maxx;
            if (W[i].maxz > z1) z1 = W[i].maxz;
        }
        nx = (int)((x1 - x0) / RP_CELL) + 1;
        nz = (int)((z1 - z0) / RP_CELL) + 1;
        cnt = (int *)calloc((size_t)nx * nz + 1, sizeof(int));
        if (!cnt) goto oom;
        for (i = 0; i < ways.n; i++) {
            const int a0 = (int)((W[i].minx - x0) / RP_CELL), a1 = (int)((W[i].maxx - x0) / RP_CELL);
            const int b0 = (int)((W[i].minz - z0) / RP_CELL), b1 = (int)((W[i].maxz - z0) / RP_CELL);
            int a, b;
            for (b = b0; b <= b1; b++) for (a = a0; a <= a1; a++) { cnt[b * nx + a]++; total++; }
        }
        s_pt.gstart = (int *)malloc(((size_t)nx * nz + 1) * sizeof(int));
        s_pt.gitem = (int *)malloc((size_t)(total > 0 ? total : 1) * sizeof(int));
        s_pt.stamp = (int *)calloc((size_t)ways.n, sizeof(int));
        if (!s_pt.gstart || !s_pt.gitem || !s_pt.stamp) { free(cnt); goto oom; }
        s_pt.gstart[0] = 0;
        for (i = 0; i < nx * nz; i++) s_pt.gstart[i + 1] = s_pt.gstart[i] + cnt[i];
        memset(cnt, 0, (size_t)nx * nz * sizeof(int));
        for (i = 0; i < ways.n; i++) {
            const int a0 = (int)((W[i].minx - x0) / RP_CELL), a1 = (int)((W[i].maxx - x0) / RP_CELL);
            const int b0 = (int)((W[i].minz - z0) / RP_CELL), b1 = (int)((W[i].maxz - z0) / RP_CELL);
            int a, b;
            for (b = b0; b <= b1; b++) for (a = a0; a <= a1; a++) {
                const int c = b * nx + a;
                s_pt.gitem[s_pt.gstart[c] + cnt[c]++] = i;
            }
        }
        free(cnt);
        s_pt.gnx = nx; s_pt.gnz = nz; s_pt.gx0 = x0; s_pt.gz0 = z0;
    }
    s_pt.npool = (char *)nm.pool.p; s_pt.noff = (int *)nm.off.p; s_pt.nname = nm.off.n;
    free(nm.tab); nm.tab = NULL;
    s_pt.way = (RpWay *)ways.p; s_pt.nway = ways.n;
    s_pt.pt = (int32_t *)pts.p; s_pt.npt = pts.n / 2;
    s_pt.ms = (double)(td5_plat_time_ms() - t0);
    snprintf(s_pt.s_place, sizeof s_pt.s_place, "%s", place);
    TD5_LOG_I(LOG_TAG, "ROAM_PARTIDO: street layer %d ways, %d points, %d street names from %d ROADS tiles in %.0f ms",
              s_pt.nway, s_pt.npt, s_pt.nname, files, s_pt.ms);
    return;
oom:
    if (root) cJSON_Delete(root);
    free(nm.tab); free(nm.pool.p); free(nm.off.p);
    free(ways.p); free(pts.p);
    free(s_pt.gstart); free(s_pt.gitem); free(s_pt.stamp);
    s_pt.gstart = s_pt.gitem = s_pt.stamp = NULL;
    TD5_LOG_W(LOG_TAG, "ROAM_PARTIDO: street layer not built (no ways / out of memory)");
}

static char s_async_place[64];

static void rp_thread(void *arg)
{
    (void)arg;
#ifndef TD5RE_RELEASE
    /* [roam G6 DEV] TD5RE_ROAM_STREETS_DELAY_MS=n: the load takes n ms longer (a cold disk took 4.7 s),
     * so the geo map's "waiting for the street layer" path can be driven on purpose */
    Sleep((DWORD)td5_env_int("TD5RE_ROAM_STREETS_DELAY_MS", 0, 0, 60000));
#endif
    rp_streets_load(s_async_place);
#ifdef _WIN32
    MemoryBarrier();
    InterlockedExchange(&s_pt.ready, s_pt.nway > 0 ? 1 : -1);
    InterlockedExchange(&s_pt.loading, 0);
#else
    s_pt.ready = s_pt.nway > 0 ? 1 : -1;
    s_pt.loading = 0;
#endif
}

static void *s_thread;

void td5_roam_partido_streets_begin_async(const char *place)
{
    if (!place || !place[0] || s_pt.loading || s_pt.ready) return;
    snprintf(s_async_place, sizeof s_async_place, "%s", place);
    s_pt.loading = 1;
    s_thread = td5_plat_thread_create(rp_thread, NULL);
    if (!s_thread) rp_thread(NULL);
}

int td5_roam_partido_streets_ensure(const char *place)
{
    if (!s_pt.ready && !s_pt.loading) td5_roam_partido_streets_begin_async(place);
    if (s_thread) { td5_plat_thread_join(s_thread); s_thread = NULL; }
    return s_pt.ready == 1;
}

int td5_roam_partido_streets_ready(void)
{
    if (s_pt.ready == 1 && s_thread) { td5_plat_thread_join(s_thread); s_thread = NULL; }
    return s_pt.ready == 1;
}

static int rp_cells(double minx, double minz, double maxx, double maxz, int *a0, int *a1, int *b0, int *b1)
{
    *a0 = (int)floor((minx - s_pt.gx0) / RP_CELL); *a1 = (int)floor((maxx - s_pt.gx0) / RP_CELL);
    *b0 = (int)floor((minz - s_pt.gz0) / RP_CELL); *b1 = (int)floor((maxz - s_pt.gz0) / RP_CELL);
    if (*a0 < 0) *a0 = 0;
    if (*b0 < 0) *b0 = 0;
    if (*a1 >= s_pt.gnx) *a1 = s_pt.gnx - 1;
    if (*b1 >= s_pt.gnz) *b1 = s_pt.gnz - 1;
    return *a0 <= *a1 && *b0 <= *b1;
}

int td5_roam_partido_streets_query(double minx, double minz, double maxx, double maxz,
                                   TD5_PartidoStreetCb cb, void *user)
{
    int a0, a1, b0, b1, a, b, n = 0;
    if (!td5_roam_partido_streets_ready() || !cb) return 0;
    if (!rp_cells(minx, minz, maxx, maxz, &a0, &a1, &b0, &b1)) return 0;
    if (++s_pt.stamp_gen == 0x7FFFFFFF) { memset(s_pt.stamp, 0, (size_t)s_pt.nway * sizeof(int)); s_pt.stamp_gen = 1; }
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int c = b * s_pt.gnx + a;
            int k;
            for (k = s_pt.gstart[c]; k < s_pt.gstart[c + 1]; k++) {
                const int i = s_pt.gitem[k];
                const RpWay *w = &s_pt.way[i];
                if (s_pt.stamp[i] == s_pt.stamp_gen) continue;
                s_pt.stamp[i] = s_pt.stamp_gen;
                if (w->maxx < minx || w->minx > maxx || w->maxz < minz || w->minz > maxz) continue;
                cb(user, w->klass, w->lanes, s_pt.pt + 2 * w->pt0, w->npt);
                n++;
            }
        }
    return n;
}

int td5_roam_partido_nearest_street(double px, double pz, double max_m, double *sx, double *sz, double *dist_m)
{
    const double R = max_m * RP_UPM;
    int a0, a1, b0, b1, a, b, found = 0;
    double best = R * R;
    if (!td5_roam_partido_streets_ready()) return 0;
    if (!rp_cells(px - R, pz - R, px + R, pz + R, &a0, &a1, &b0, &b1)) return 0;
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int c = b * s_pt.gnx + a;
            int k;
            for (k = s_pt.gstart[c]; k < s_pt.gstart[c + 1]; k++) {
                const RpWay *w = &s_pt.way[s_pt.gitem[k]];
                int j;
                if (w->maxx < px - R || w->minx > px + R || w->maxz < pz - R || w->minz > pz + R) continue;
                for (j = 0; j + 1 < w->npt; j++) {
                    const double ax = s_pt.pt[2 * (w->pt0 + j)], az = s_pt.pt[2 * (w->pt0 + j) + 1];
                    const double ex = s_pt.pt[2 * (w->pt0 + j + 1)] - ax, ez = s_pt.pt[2 * (w->pt0 + j + 1) + 1] - az;
                    const double l2 = ex * ex + ez * ez;
                    double t = l2 > 0.0 ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0, qx, qz, d;
                    if (t < 0.0) t = 0.0;
                    if (t > 1.0) t = 1.0;
                    qx = ax + ex * t; qz = az + ez * t;
                    d = (qx - px) * (qx - px) + (qz - pz) * (qz - pz);
                    if (d < best) { best = d; if (sx) *sx = qx; if (sz) *sz = qz; found = 1; }
                }
            }
        }
    if (found && dist_m) *dist_m = sqrt(best) / RP_UPM;
    return found;
}

void td5_roam_partido_streets_stats(int *ways, int *points, double *ms)
{
    if (ways) *ways = s_pt.nway;
    if (points) *points = s_pt.npt;
    if (ms) *ms = s_pt.ms;
}

/* ---- [roam HB] street names -------------------------------------------------------------- */

const char *td5_roam_partido_name(int id)
{
    return (s_pt.ready == 1 && id >= 0 && id < s_pt.nname) ? s_pt.npool + s_pt.noff[id] : "";
}

int td5_roam_partido_named_query(double minx, double minz, double maxx, double maxz,
                                 TD5_PartidoNamedCb cb, void *user)
{
    int a0, a1, b0, b1, a, b, n = 0;
    if (!td5_roam_partido_streets_ready() || !cb) return 0;
    if (!rp_cells(minx, minz, maxx, maxz, &a0, &a1, &b0, &b1)) return 0;
    if (++s_pt.stamp_gen == 0x7FFFFFFF) { memset(s_pt.stamp, 0, (size_t)s_pt.nway * sizeof(int)); s_pt.stamp_gen = 1; }
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int c = b * s_pt.gnx + a;
            int k;
            for (k = s_pt.gstart[c]; k < s_pt.gstart[c + 1]; k++) {
                const int i = s_pt.gitem[k];
                const RpWay *w = &s_pt.way[i];
                if (s_pt.stamp[i] == s_pt.stamp_gen) continue;
                s_pt.stamp[i] = s_pt.stamp_gen;
                if (w->name_id < 0) continue;
                if (w->maxx < minx || w->minx > maxx || w->maxz < minz || w->minz > maxz) continue;
                cb(user, w->name_id, w->klass, s_pt.pt + 2 * w->pt0, w->npt);
                n++;
            }
        }
    return n;
}

int td5_roam_partido_name_at(double px, double pz, double max_m, double *dist_m)
{
    const double R = max_m * RP_UPM;
    int a0, a1, b0, b1, a, b, best_id = -1;
    double best = R * R;
    if (!td5_roam_partido_streets_ready()) return -1;
    if (!rp_cells(px - R, pz - R, px + R, pz + R, &a0, &a1, &b0, &b1)) return -1;
    for (b = b0; b <= b1; b++)
        for (a = a0; a <= a1; a++) {
            const int c = b * s_pt.gnx + a;
            int k;
            for (k = s_pt.gstart[c]; k < s_pt.gstart[c + 1]; k++) {
                const RpWay *w = &s_pt.way[s_pt.gitem[k]];
                int j;
                if (w->name_id < 0) continue;
                if (w->maxx < px - R || w->minx > px + R || w->maxz < pz - R || w->minz > pz + R) continue;
                for (j = 0; j + 1 < w->npt; j++) {
                    const double ax = s_pt.pt[2 * (w->pt0 + j)], az = s_pt.pt[2 * (w->pt0 + j) + 1];
                    const double ex = s_pt.pt[2 * (w->pt0 + j + 1)] - ax, ez = s_pt.pt[2 * (w->pt0 + j + 1) + 1] - az;
                    const double l2 = ex * ex + ez * ez;
                    double t = l2 > 0.0 ? ((px - ax) * ex + (pz - az) * ez) / l2 : 0.0, qx, qz, d;
                    if (t < 0.0) t = 0.0;
                    if (t > 1.0) t = 1.0;
                    qx = ax + ex * t; qz = az + ez * t;
                    d = (qx - px) * (qx - px) + (qz - pz) * (qz - pz);
                    if (d < best) { best = d; best_id = w->name_id; }
                }
            }
        }
    if (best_id >= 0 && dist_m) *dist_m = sqrt(best) / RP_UPM;
    return best_id;
}
