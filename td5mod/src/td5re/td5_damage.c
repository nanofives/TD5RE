/* ========================================================================
 * td5_damage.c -- GTA4-style car damage system (PORT-ONLY)
 *
 * See td5_damage.h for the design summary and the (two) RE facts this rests
 * on. Everything numeric below is a port-only engineering choice exposed via
 * TD5RE_DAMAGE_* env knobs; nothing here is reverse-engineered. The whole
 * module is inert when [Game] CarDamage = 0, so the faithful Sim is unchanged.
 *
 * Health lives in the actor padding (actor->damage_health, +0x1D8). Per-vertex
 * deformation lives in module-private per-slot delta buffers (too large for the
 * 24-byte actor gap); the render transform adds them to model pos each frame.
 * ======================================================================== */
#include "td5_damage.h"
#include "../../../re/include/td5_actor_struct.h"   /* full TD5_Actor (damage_* fields) */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5re.h"
#include "td5_game.h"
#include "td5_render.h"
#include "td5_ai.h"
#include "td5_platform.h"

#define LOG_TAG "physics"   /* routes to race.log; lines are prefixed "car_damage:" */

/* ------------------------------------------------------------------ knobs */
/* Parsed once in td5_damage_init(). All have safe defaults. */
static int32_t s_max_hp        = 400000;   /* full health, in impact_mag units (a heavy ~155k crash ~= 39%) */
static int     s_impact_pct    = 100;      /* % of impact_mag subtracted from health per hit */
static int32_t s_min_impact    = 4000;     /* impacts below this don't chip health/deform */
static int32_t s_knockout      = 0;        /* health <= this -> wrecked */
static int     s_deform_k      = 24;       /* # nearest verts pushed per hit */
static float   s_max_disp      = 18.0f;    /* clamp on accumulated per-vertex displacement (model units) */
static float   s_dent_div      = 30000.0f; /* impact_mag / this = base dent (model units), before falloff */
static float   s_dent_max      = 12.0f;    /* clamp on a single hit's base dent (model units) */
static float   s_radius_frac   = 0.45f;    /* deform influence radius as a fraction of max model extent */
static int     s_penalty_pct   = 45;       /* max % steering-authority reduction at zero health */
static int     s_smoke_light   = 40;       /* % health lost to start light smoke */
static int     s_smoke_black   = 65;       /* % health lost to switch to black smoke */
static int     s_smoke_fire    = 88;       /* % health lost to add fire/sparks */
static int     s_inited        = 0;

/* ------------------------------------------------------- per-slot deform */
typedef struct {
    const TD5_MeshHeader *mesh;   /* mesh the deltas are sized to (NULL = none) */
    int     vcount;               /* vertices the deltas cover */
    int     cap;                  /* allocated capacity (>= vcount) */
    float  *dx, *dy, *dz;         /* per-vertex model-space deltas (cap each) */
    int     dirty;                /* any nonzero delta this race? */
} DamageSlot;

static DamageSlot s_slot[TD5_MAX_TOTAL_ACTORS];
static uint8_t    s_ko_notified[TD5_MAX_TOTAL_ACTORS];  /* mark_broken_down fired once per knockout */

/* ----------------------------------------------------------------- utils */
static int env_int(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name);
    if (!e || !e[0]) return def;
    long v = strtol(e, NULL, 0);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return (int)v;
}
static float env_float(const char *name, float def, float lo, float hi) {
    const char *e = getenv(name);
    if (!e || !e[0]) return def;
    float v = (float)atof(e);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

int td5_damage_init(void) {
    if (s_inited) return 1;        /* module loader treats non-zero as success */
    s_inited = 1;
    s_max_hp      = env_int  ("TD5RE_DAMAGE_MAX_HP",        400000,  1000, 100000000);
    s_impact_pct  = env_int  ("TD5RE_DAMAGE_IMPACT_SCALE",  100,     1,    2000);
    s_min_impact  = env_int  ("TD5RE_DAMAGE_MIN_IMPACT",    4000,    0,    1000000);
    s_knockout    = env_int  ("TD5RE_DAMAGE_KNOCKOUT",      0,       0,    100000000);
    s_deform_k    = env_int  ("TD5RE_DAMAGE_DEFORM_K",      24,      1,    4096);
    s_max_disp    = env_float("TD5RE_DAMAGE_MAX_DISP",      18.0f,   0.0f, 1000.0f);
    s_dent_div    = env_float("TD5RE_DAMAGE_DENT_DIV",      30000.0f,1.0f, 1.0e9f);
    s_dent_max    = env_float("TD5RE_DAMAGE_DENT_MAX",      12.0f,   0.0f, 1000.0f);
    s_radius_frac = env_float("TD5RE_DAMAGE_RADIUS_FRAC",   0.45f,   0.02f, 4.0f);
    s_penalty_pct = env_int  ("TD5RE_DAMAGE_PENALTY",       45,      0,    95);
    s_smoke_light = env_int  ("TD5RE_DAMAGE_SMOKE_LIGHT",   40,      0,    100);
    s_smoke_black = env_int  ("TD5RE_DAMAGE_SMOKE_BLACK",   65,      0,    100);
    s_smoke_fire  = env_int  ("TD5RE_DAMAGE_SMOKE_FIRE",    88,      0,    100);
    TD5_LOG_I(LOG_TAG, "car_damage: init max_hp=%d impact_pct=%d min_imp=%d ko=%d "
              "K=%d max_disp=%.1f dent_div=%.0f penalty=%d smoke=%d/%d/%d",
              s_max_hp, s_impact_pct, s_min_impact, s_knockout, s_deform_k,
              (double)s_max_disp, (double)s_dent_div, s_penalty_pct,
              s_smoke_light, s_smoke_black, s_smoke_fire);
    return 1;        /* success (module loader: !init() == failure) */
}

void td5_damage_shutdown(void) {
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        free(s_slot[i].dx); free(s_slot[i].dy); free(s_slot[i].dz);
        memset(&s_slot[i], 0, sizeof(s_slot[i]));
    }
    s_inited = 0;
}

int td5_damage_enabled(void) {
    return g_td5.ini.car_damage != 0;
}

/* Resolve a slot to its actor (NULL if out of range / unspawned). */
static TD5_Actor *dmg_actor(int slot) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return NULL;
    return td5_game_get_actor(slot);
}

/* Lazily (re)initialize an actor's health fields if the magic guard is unset. */
static void ensure_health_init(TD5_Actor *a) {
    if (!a) return;
    if (a->damage_magic != TD5_DAMAGE_ACTOR_MAGIC) {
        a->damage_health = s_max_hp;
        a->damage_accum  = 0;
        a->damage_magic  = TD5_DAMAGE_ACTOR_MAGIC;
    }
}

void td5_damage_reset_race(void) {
    if (!s_inited) td5_damage_init();
    int enabled = td5_damage_enabled();
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        /* Zero the deltas (keep the allocation for reuse) and detach the mesh so
         * a new car/mesh forces a fresh ensure on the next hit. Done even when
         * disabled so a leftover dent from an earlier enabled race can't linger
         * if the toggle changed between races. */
        DamageSlot *ds = &s_slot[i];
        if (ds->dx && ds->cap > 0) {
            memset(ds->dx, 0, (size_t)ds->cap * sizeof(float));
            memset(ds->dy, 0, (size_t)ds->cap * sizeof(float));
            memset(ds->dz, 0, (size_t)ds->cap * sizeof(float));
        }
        ds->mesh = NULL; ds->vcount = 0; ds->dirty = 0;
        s_ko_notified[i] = 0;

        /* Init health ONLY when enabled — when off we never touch the actor
         * padding, so the faithful sim is byte-identical to the original. */
        if (enabled) {
            TD5_Actor *a = dmg_actor(i);
            if (a) {
                a->damage_health = s_max_hp;
                a->damage_accum  = 0;
                a->damage_magic  = TD5_DAMAGE_ACTOR_MAGIC;
            }
        }
    }
    TD5_LOG_I(LOG_TAG, "car_damage: reset_race (enabled=%d, max_hp=%d)",
              enabled, s_max_hp);
}

/* health fraction 0..1 for an actor (1 = pristine, -1 = uninitialized). */
static float actor_health01(const TD5_Actor *a) {
    if (!a || a->damage_magic != TD5_DAMAGE_ACTOR_MAGIC) return -1.0f;
    if (s_max_hp <= 0) return 1.0f;
    float h = (float)a->damage_health / (float)s_max_hp;
    if (h < 0.0f) h = 0.0f;
    if (h > 1.0f) h = 1.0f;
    return h;
}

float td5_damage_health01(int slot) {
    return actor_health01(dmg_actor(slot));
}

int td5_damage_actor_knocked_out(const TD5_Actor *a) {
    if (!td5_damage_enabled() || !a) return 0;
    if (a->damage_magic != TD5_DAMAGE_ACTOR_MAGIC) return 0;
    return a->damage_health <= s_knockout;
}

int td5_damage_slot_knocked_out(int slot) {
    return td5_damage_actor_knocked_out(dmg_actor(slot));
}

int td5_damage_smoke_tier(int slot) {
    if (!td5_damage_enabled()) return 0;
    float h = td5_damage_health01(slot);
    if (h < 0.0f) return 0;
    int lost = (int)((1.0f - h) * 100.0f + 0.5f);
    if (lost >= s_smoke_fire)  return 3;
    if (lost >= s_smoke_black) return 2;
    if (lost >= s_smoke_light) return 1;
    return 0;
}

float td5_damage_handling_scale(int slot) {
    if (!td5_damage_enabled()) return 1.0f;
    float h = td5_damage_health01(slot);
    if (h < 0.0f) return 1.0f;
    float lost = 1.0f - h;
    float scale = 1.0f - lost * ((float)s_penalty_pct / 100.0f);
    float floor_v = 1.0f - ((float)s_penalty_pct / 100.0f);
    if (scale < floor_v) scale = floor_v;
    if (scale > 1.0f) scale = 1.0f;
    return scale;
}

/* (Re)size the per-slot delta buffers to match mesh; zero on resize/mesh-change. */
static int ensure_slot_buffers(DamageSlot *ds, const TD5_MeshHeader *mesh, int vcount) {
    if (vcount <= 0 || vcount > 200000) return 0;
    if (ds->mesh == mesh && ds->vcount == vcount && ds->dx) return 1;
    if (ds->cap < vcount) {
        float *nx = (float *)realloc(ds->dx, (size_t)vcount * sizeof(float));
        float *ny = (float *)realloc(ds->dy, (size_t)vcount * sizeof(float));
        float *nz = (float *)realloc(ds->dz, (size_t)vcount * sizeof(float));
        if (!nx || !ny || !nz) { free(nx); free(ny); free(nz); return 0; }
        ds->dx = nx; ds->dy = ny; ds->dz = nz; ds->cap = vcount;
    }
    memset(ds->dx, 0, (size_t)vcount * sizeof(float));
    memset(ds->dy, 0, (size_t)vcount * sizeof(float));
    memset(ds->dz, 0, (size_t)vcount * sizeof(float));
    ds->mesh = mesh; ds->vcount = vcount; ds->dirty = 0;
    return 1;
}

/* Push the K vertices nearest the contact point inward, accumulating + clamping. */
static void apply_deform(int slot, int32_t impact_mag, const TD5_DamageHit *hit) {
    TD5_MeshHeader *mesh = td5_render_get_vehicle_mesh(slot);
    if (!mesh) return;
    int count = mesh->total_vertex_count;
    const TD5_MeshVertex *verts = (const TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
    if (!verts || count <= 0) return;

    DamageSlot *ds = &s_slot[slot];
    if (!ensure_slot_buffers(ds, mesh, count)) return;

    /* Model-space AABB (cheap; hits are rare). */
    float minx = verts[0].pos_x, maxx = minx;
    float miny = verts[0].pos_y, maxy = miny;
    float minz = verts[0].pos_z, maxz = minz;
    for (int i = 1; i < count; i++) {
        float x = verts[i].pos_x, y = verts[i].pos_y, z = verts[i].pos_z;
        if (x < minx) minx = x; else if (x > maxx) maxx = x;
        if (y < miny) miny = y; else if (y > maxy) maxy = y;
        if (z < minz) minz = z; else if (z > maxz) maxz = z;
    }
    float cenx = 0.5f * (minx + maxx);
    float ceny = 0.5f * (miny + maxy);
    float cenz = 0.5f * (minz + maxz);
    float ext_x = 0.5f * (maxx - minx);
    float ext_z = 0.5f * (maxz - minz);
    float max_ext = ext_x; if (ext_z > max_ext) max_ext = ext_z;
    if (0.5f * (maxy - miny) > max_ext) max_ext = 0.5f * (maxy - miny);
    if (max_ext <= 0.001f) return;

    /* Contact point on the struck face (signs from the physics contact). */
    float ctx = (hit->lat > 0) ? maxx : (hit->lat < 0) ? minx : cenx;
    float ctz = (hit->fwd > 0) ? maxz : (hit->fwd < 0) ? minz : cenz;
    float cty = ceny;
    /* If the solver said "side" but gave no lateral sign, bias to whichever
     * lateral face the contact is nearer (defensive; usually lat is set). */
    if (hit->is_side && hit->lat == 0) ctx = (ctz >= cenz) ? maxx : minx;

    /* Inward push direction = contact -> centre, with a slight downward crumple. */
    float inx = cenx - ctx;
    float iny = ceny - cty - 0.20f * (maxy - miny);   /* crumple downward */
    float inz = cenz - ctz;
    float inlen = sqrtf(inx*inx + iny*iny + inz*inz);
    if (inlen < 1e-4f) { inx = 0.0f; iny = -1.0f; inz = 0.0f; inlen = 1.0f; }
    inx /= inlen; iny /= inlen; inz /= inlen;

    /* Base dent magnitude for this hit. */
    float dent = (float)impact_mag / s_dent_div;
    if (dent > s_dent_max) dent = s_dent_max;
    if (dent <= 0.0f) return;

    float radius = s_radius_frac * max_ext;
    if (radius <= 0.001f) return;
    float r2 = radius * radius;

    /* Top-K nearest selection (small K, rare event). Store index + dist2. */
    int   K = s_deform_k; if (K > count) K = count;
    /* file-static scratch (apply_deform runs only on the serial sim tick, so it
     * is non-reentrant; render-side get_deform only ever reads). KMAX matches
     * the s_deform_k clamp ceiling. */
    enum { KMAX = 4096 };
    if (K > KMAX) K = KMAX;
    static int   sel_idx[KMAX];
    static float sel_d2[KMAX];
    int   nsel = 0;
    float worst = 0.0f;   /* largest selected dist2 */

    for (int i = 0; i < count; i++) {
        float dx = verts[i].pos_x - ctx;
        float dy = verts[i].pos_y - cty;
        float dz = verts[i].pos_z - ctz;
        float d2 = dx*dx + dy*dy + dz*dz;
        if (d2 > r2) continue;                 /* outside influence radius */
        if (nsel < K) {
            sel_idx[nsel] = i; sel_d2[nsel] = d2;
            if (d2 > worst) worst = d2;
            nsel++;
            if (nsel == K) {
                /* recompute worst now the buffer is full */
                worst = 0.0f;
                for (int s = 0; s < nsel; s++) if (sel_d2[s] > worst) worst = sel_d2[s];
            }
        } else if (d2 < worst) {
            /* replace the current worst */
            int wi = 0; float wv = sel_d2[0];
            for (int s = 1; s < nsel; s++) if (sel_d2[s] > wv) { wv = sel_d2[s]; wi = s; }
            sel_idx[wi] = i; sel_d2[wi] = d2;
            worst = 0.0f;
            for (int s = 0; s < nsel; s++) if (sel_d2[s] > worst) worst = sel_d2[s];
        }
    }
    if (nsel == 0) return;

    float dmax = sqrtf(worst); if (dmax < 1e-4f) dmax = 1e-4f;

    for (int s = 0; s < nsel; s++) {
        int i = sel_idx[s];
        float d = sqrtf(sel_d2[s]);
        float w = 1.0f - d / dmax;            /* linear falloff: nearest = full */
        if (w < 0.0f) w = 0.0f;
        float amt = dent * w;
        float nxv = ds->dx[i] + inx * amt;
        float nyv = ds->dy[i] + iny * amt;
        float nzv = ds->dz[i] + inz * amt;
        /* clamp accumulated per-vertex displacement */
        float mag = sqrtf(nxv*nxv + nyv*nyv + nzv*nzv);
        if (mag > s_max_disp && mag > 1e-4f) {
            float k = s_max_disp / mag;
            nxv *= k; nyv *= k; nzv *= k;
        }
        ds->dx[i] = nxv; ds->dy[i] = nyv; ds->dz[i] = nzv;
    }
    ds->dirty = 1;
}

void td5_damage_on_impact(TD5_Actor *actor, int32_t impact_mag,
                          const TD5_DamageHit *hit) {
    if (!td5_damage_enabled() || !actor) return;
    int slot = (int)actor->slot_index;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;

    int32_t mag = impact_mag < 0 ? -impact_mag : impact_mag;
    if (mag < s_min_impact) return;            /* resting/grazing contact: ignore */

    ensure_health_init(actor);

    /* Drive health down (port-only formula). */
    int64_t dmg64 = (int64_t)mag * (int64_t)s_impact_pct / 100;
    int32_t dmg = (dmg64 > 0x7FFFFFFF) ? 0x7FFFFFFF : (int32_t)dmg64;
    int32_t before = actor->damage_health;
    actor->damage_health -= dmg;
    if (actor->damage_health < 0) actor->damage_health = 0;
    /* cumulative absorbed (saturating) */
    if (actor->damage_accum < 0x7FFFFFFF - dmg) actor->damage_accum += dmg;
    else actor->damage_accum = 0x7FFFFFFF;

    /* Deform the body mesh at the hit region. */
    if (hit) apply_deform(slot, mag, hit);

    /* Knockout transition: fire the broken-down hook once. The health-based
     * knocked-out test (used by the freeze + completion gate) is inherently
     * permanent because health never regenerates. mark_actor_broken_down ends
     * any in-progress cop chase and parks knocked-out TRAFFIC permanently;
     * knocked-out RACERS are immobilized by the physics arrest-style freeze. */
    if (actor->damage_health <= s_knockout && before > s_knockout &&
        !s_ko_notified[slot]) {
        s_ko_notified[slot] = 1;
        td5_ai_mark_actor_broken_down(slot);
        TD5_LOG_I(LOG_TAG, "car_damage: KNOCKOUT slot=%d (mag=%d dmg=%d hp %d->0)",
                  slot, mag, dmg, before);
    } else {
        TD5_LOG_I(LOG_TAG, "car_damage: hit slot=%d mag=%d dmg=%d hp=%d/%d "
                  "lat=%d fwd=%d side=%d tier=%d",
                  slot, mag, dmg, actor->damage_health, s_max_hp,
                  hit ? hit->lat : 0, hit ? hit->fwd : 0, hit ? hit->is_side : 0,
                  td5_damage_smoke_tier(slot));
    }
}

int td5_damage_get_deform(int slot, const TD5_MeshHeader *mesh,
                          const float **dx, const float **dy,
                          const float **dz, int *vcount) {
    if (!td5_damage_enabled()) return 0;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !mesh) return 0;
    DamageSlot *ds = &s_slot[slot];
    if (!ds->dirty || ds->mesh != mesh || !ds->dx) return 0;
    if (ds->vcount != mesh->total_vertex_count) return 0;
    if (dx) *dx = ds->dx;
    if (dy) *dy = ds->dy;
    if (dz) *dz = ds->dz;
    if (vcount) *vcount = ds->vcount;
    return 1;
}
