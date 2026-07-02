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
#include "td5_config.h"       /* shared TD5RE_* env-knob helpers */

#define LOG_TAG "physics"   /* routes to race.log; lines are prefixed "car_damage:" */

/* ------------------------------------------------------------------ knobs */
/* Parsed once in td5_damage_init(). All have safe defaults. */
static int32_t s_max_hp        = 2400000;  /* full health, in impact_mag units (3x durability) */
static int     s_impact_pct    = 100;      /* % of impact_mag subtracted from health per hit */
static int32_t s_min_impact    = 4000;     /* impacts below this don't chip health/deform */
static int32_t s_knockout      = 0;        /* health <= this -> wrecked */
static int     s_deform_k      = 40;       /* # nearest verts pushed per hit */
/* Deformation is sized RELATIVE to the car's own model extent so dents are
 * always clearly visible regardless of a car's modelling scale. */
static int32_t s_dent_ref_mag  = 90000;    /* impact_mag that yields a full single-hit dent */
static int     s_dent_frac     = 45;       /* full single-hit dent as % of the car's half-extent (0.75x) */
static int     s_disp_frac     = 90;       /* clamp on accumulated dent as % of the car's half-extent (0.75x) */
static float   s_radius_frac   = 0.55f;    /* deform influence radius as a fraction of max model extent */
static int     s_penalty_pct   = 45;       /* max % steering-authority reduction at zero health */
static int     s_wall_scale    = 400;      /* wall approach-speed -> impact_mag scale (%) */
static int     s_finish_orbit   = 1;       /* orbit the chase cam around a finished/wrecked car */
static int     s_orbit_speed    = 24;      /* 12-bit angle increment per sim-tick (4096 = full circle) */
static int     s_orbit_hold_ms  = 6000;    /* post-finish hold (ms) so the SP orbit has time to play */
static int     s_smoke_light   = 40;       /* % health lost to start light smoke */
static int     s_smoke_black   = 65;       /* % health lost to switch to black smoke */
static int     s_smoke_fire    = 88;       /* % health lost to add fire/sparks */
static int     s_scuff_pct     = 55;       /* max diffuse darkening (%) on a fully-scuffed vertex */
/* Env overrides for the three LEVEL-driven knobs (-1 = not set -> use the Game
 * Options toughness/deform level instead). Captured once in init; applied (with
 * the current levels) at every race start via apply_levels(). */
static int     s_hp_env        = -1;
static int     s_dent_env      = -1;
static int     s_disp_env      = -1;
static int     s_inited        = 0;

/* ------------------------------------------------------- per-slot deform */
typedef struct {
    const TD5_MeshHeader *mesh;   /* mesh the deltas are sized to (NULL = none) */
    int     vcount;               /* vertices the deltas cover */
    int     cap;                  /* allocated capacity (>= vcount) */
    float  *dx, *dy, *dz;         /* per-vertex model-space deltas (cap each) */
    float  *scuff;                /* per-vertex damage darkening 0..1 (cap each) */
    int     dirty;                /* any nonzero delta this race? */
} DamageSlot;

static DamageSlot s_slot[TD5_MAX_TOTAL_ACTORS];
static uint8_t    s_ko_notified[TD5_MAX_TOTAL_ACTORS];  /* mark_broken_down fired once per knockout */

/* Per-impact-EVENT tracking so a SUSTAINED contact (grinding a wall, resting
 * against a car — especially slow/stopped) doesn't drain the bar every tick.
 * A new collision event (first contact after a gap) deals full damage; while
 * contact persists, only a hit HARDER than the event's peak deals the extra. */
static int32_t s_last_contact_tick[TD5_MAX_TOTAL_ACTORS];
static int32_t s_event_peak_mag[TD5_MAX_TOTAL_ACTORS];
static int     s_contact_gap = 8;   /* ticks of no-contact that end a collision event */

/* Env-knob parse/clamp helpers now live in td5_config.c (shared across modules). */

/* Map the global Game Options toughness/deform LEVELS (0=Low,1=Normal,2=High)
 * to the active health + deformation magnitudes. Env knobs, when set, win over
 * the level. Called at init and at every race start so a menu change takes
 * effect on the next race. Normal == the user's tuned 3x durability / 0.75x
 * deform baseline. */
static void apply_levels(void) {
    int t = g_td5.ini.car_damage_toughness; if (t < 0) t = 0; if (t > 2) t = 2;
    int d = g_td5.ini.car_damage_deform;    if (d < 0) d = 0; if (d > 2) d = 2;
    static const int32_t HP[3]   = { 1200000, 2400000, 4800000 };  /* Low/Normal/High */
    static const int     DENT[3] = { 23, 45, 68 };
    static const int     DISP[3] = { 45, 90, 135 };
    s_max_hp    = (s_hp_env   >= 0) ? (int32_t)s_hp_env : HP[t];
    s_dent_frac = (s_dent_env >= 0) ? s_dent_env        : DENT[d];
    s_disp_frac = (s_disp_env >= 0) ? s_disp_env        : DISP[d];
}

int td5_damage_init(void) {
    if (s_inited) return 1;        /* module loader treats non-zero as success */
    s_inited = 1;
    s_impact_pct  = td5_env_int  ("TD5RE_DAMAGE_IMPACT_SCALE",  100,     1,    2000);
    s_min_impact  = td5_env_int  ("TD5RE_DAMAGE_MIN_IMPACT",    4000,    0,    1000000);
    s_knockout    = td5_env_int  ("TD5RE_DAMAGE_KNOCKOUT",      0,       0,    100000000);
    s_deform_k    = td5_env_int  ("TD5RE_DAMAGE_DEFORM_K",      40,      1,    4096);
    s_dent_ref_mag= td5_env_int  ("TD5RE_DAMAGE_DENT_REF_MAG",  90000,   1000, 100000000);
    /* HP / dent / disp come from the Game Options LEVELS (apply_levels); an env
     * override, when present, wins. -1 = no override. */
    s_hp_env      = td5_env_int_opt("TD5RE_DAMAGE_MAX_HP",      1000, 100000000, -1);
    s_dent_env    = td5_env_int_opt("TD5RE_DAMAGE_DENT_FRAC",   1,    200,       -1);
    s_disp_env    = td5_env_int_opt("TD5RE_DAMAGE_DISP_FRAC",   1,    300,       -1);
    s_radius_frac = td5_env_float("TD5RE_DAMAGE_RADIUS_FRAC",   0.55f,   0.02f, 4.0f);
    s_penalty_pct = td5_env_int  ("TD5RE_DAMAGE_PENALTY",       45,      0,    95);
    s_wall_scale  = td5_env_int  ("TD5RE_DAMAGE_WALL_SCALE",    400,     0,    5000);
    s_finish_orbit= td5_env_int  ("TD5RE_DAMAGE_FINISH_ORBIT",  1,       0,    1);
    s_orbit_speed = td5_env_int  ("TD5RE_DAMAGE_ORBIT_SPEED",   24,      1,    512);
    s_orbit_hold_ms = td5_env_int("TD5RE_DAMAGE_ORBIT_HOLD_MS", 6000,    0,    60000);
    s_smoke_light = td5_env_int  ("TD5RE_DAMAGE_SMOKE_LIGHT",   40,      0,    100);
    s_smoke_black = td5_env_int  ("TD5RE_DAMAGE_SMOKE_BLACK",   65,      0,    100);
    s_smoke_fire  = td5_env_int  ("TD5RE_DAMAGE_SMOKE_FIRE",    88,      0,    100);
    s_scuff_pct   = td5_env_int  ("TD5RE_DAMAGE_SCUFF",         55,      0,    100);
    s_contact_gap = td5_env_int  ("TD5RE_DAMAGE_CONTACT_GAP",   8,       1,    300);
    apply_levels();   /* seed HP/dent/disp from the saved levels (or env override) */
    TD5_LOG_I(LOG_TAG, "car_damage: init max_hp=%d impact_pct=%d min_imp=%d ko=%d "
              "K=%d dent_ref=%d dent_frac=%d disp_frac=%d radius=%.2f penalty=%d smoke=%d/%d/%d",
              s_max_hp, s_impact_pct, s_min_impact, s_knockout, s_deform_k,
              s_dent_ref_mag, s_dent_frac, s_disp_frac, (double)s_radius_frac,
              s_penalty_pct, s_smoke_light, s_smoke_black, s_smoke_fire);
    return 1;        /* success (module loader: !init() == failure) */
}

void td5_damage_shutdown(void) {
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        free(s_slot[i].dx); free(s_slot[i].dy); free(s_slot[i].dz); free(s_slot[i].scuff);
        memset(&s_slot[i], 0, sizeof(s_slot[i]));
    }
    s_inited = 0;
}

int td5_damage_enabled(void) {
    return g_td5.ini.car_damage != 0;
}

/* The HUD health bar + the wreck/knockout mechanic (and its health-driven
 * handling penalty + damage smoke). Requires the master CarDamage on AND the
 * [Game] CarDamageBar sub-toggle. When this is off, accumulated damage never
 * wrecks a car or ends the race; only the impact-driven mesh deformation (dents)
 * and the collision physics remain — those are gated by td5_damage_enabled()
 * alone, not by this. Default on. */
int td5_damage_bar_enabled(void) {
    return td5_damage_enabled() && g_td5.ini.car_damage_bar != 0;
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
    apply_levels();   /* pick up any Game Options toughness/deform change for this race */
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
            if (ds->scuff) memset(ds->scuff, 0, (size_t)ds->cap * sizeof(float));
        }
        ds->mesh = NULL; ds->vcount = 0; ds->dirty = 0;
        s_ko_notified[i] = 0;
        s_last_contact_tick[i] = -1000000;   /* first contact is always a fresh event */
        s_event_peak_mag[i] = 0;

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

/* [RESET-CAR REPAIR 2026-06-29] Fully repair ONE slot mid-race: restore health to
 * full, clear the accumulated/knockout state, reset the per-event contact tracker,
 * and erase the body deformation + scuff (a reset car is a FRESH car). Called by
 * the manual stuck-recovery path (td5_physics_recover_player) so a damaged or
 * knocked-out car is actually recovered.
 *
 * WHY this is required: the original ResetVehicleActorState (0x00405D70) had no
 * health field to restore (the original has NO damage model — RE-confirmed), so a
 * faithful in-place reset leaves the port-only health untouched. Without this:
 *   - a heavily-damaged car that resets keeps its low health and is wrecked by the
 *     next bump (the user's "reset car -> complete break of car"), and
 *   - a knocked-out car (health<=0) stays physics-FROZEN forever, because health
 *     never regenerates, so the manual reset can never un-stick it.
 * Inert when the master CarDamage toggle is off (faithful sim unchanged). */
void td5_damage_repair_actor(int slot) {
    if (!td5_damage_enabled()) return;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (!s_inited) td5_damage_init();

    TD5_Actor *a = dmg_actor(slot);
    if (a) {
        a->damage_health = s_max_hp;
        a->damage_accum  = 0;
        a->damage_magic  = TD5_DAMAGE_ACTOR_MAGIC;
    }

    /* Reset the knockout + per-event contact bookkeeping so the first post-reset
     * contact reads as a fresh event (and the KO hook can fire again later). */
    s_ko_notified[slot]      = 0;
    s_last_contact_tick[slot] = -1000000;
    s_event_peak_mag[slot]    = 0;

    /* Erase the accumulated dents + scuff (keep the allocation for reuse). */
    {
        DamageSlot *ds = &s_slot[slot];
        if (ds->dx && ds->cap > 0) {
            memset(ds->dx, 0, (size_t)ds->cap * sizeof(float));
            memset(ds->dy, 0, (size_t)ds->cap * sizeof(float));
            memset(ds->dz, 0, (size_t)ds->cap * sizeof(float));
            if (ds->scuff) memset(ds->scuff, 0, (size_t)ds->cap * sizeof(float));
        }
        ds->dirty = 0;
    }

    TD5_LOG_I(LOG_TAG, "car_damage: repair slot=%d (health restored to %d, dents cleared)",
              slot, s_max_hp);
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

int td5_damage_finish_orbit_enabled(void) {
    return td5_damage_enabled() && s_finish_orbit;
}

int td5_damage_finish_orbit_speed(void) {
    return s_orbit_speed;
}

int td5_damage_finish_orbit_hold_ms(void) {
    return s_orbit_hold_ms;
}

int td5_damage_actor_knocked_out(const TD5_Actor *a) {
    /* Gate on the bar/wreck sub-toggle: with the damage bar off, a car is never
     * "knocked out" by accumulated damage, so the physics freeze, the race-
     * completion gate, and the finish-orbit cam all treat it as never-wrecked. */
    if (!td5_damage_bar_enabled() || !a) return 0;
    if (a->damage_magic != TD5_DAMAGE_ACTOR_MAGIC) return 0;
    return a->damage_health <= s_knockout;
}

int td5_damage_slot_knocked_out(int slot) {
    return td5_damage_actor_knocked_out(dmg_actor(slot));
}

int td5_damage_smoke_tier(int slot) {
    /* Damage smoke escalates with the (hidden) health meter — suppress it when
     * the bar/wreck mechanic is off so there's no health-driven smoke without a
     * visible bar. Impact dents are unaffected (handled in apply_deform). */
    if (!td5_damage_bar_enabled()) return 0;
    float h = td5_damage_health01(slot);
    if (h < 0.0f) return 0;
    int lost = (int)((1.0f - h) * 100.0f + 0.5f);
    if (lost >= s_smoke_fire)  return 3;
    if (lost >= s_smoke_black) return 2;
    if (lost >= s_smoke_light) return 1;
    return 0;
}

float td5_damage_handling_scale(int slot) {
    /* The steering-authority penalty is driven by the (hidden) health meter — so
     * it follows the bar/wreck toggle, not the master. With the bar off there is
     * no invisible handling nerf; dents and collisions still apply normally. */
    if (!td5_damage_bar_enabled()) return 1.0f;
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
        float *ns = (float *)realloc(ds->scuff, (size_t)vcount * sizeof(float));
        if (!nx || !ny || !nz || !ns) { free(nx); free(ny); free(nz); free(ns); return 0; }
        ds->dx = nx; ds->dy = ny; ds->dz = nz; ds->scuff = ns; ds->cap = vcount;
    }
    memset(ds->dx, 0, (size_t)vcount * sizeof(float));
    memset(ds->dy, 0, (size_t)vcount * sizeof(float));
    memset(ds->dz, 0, (size_t)vcount * sizeof(float));
    memset(ds->scuff, 0, (size_t)vcount * sizeof(float));
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

    /* Base dent magnitude for this hit, sized RELATIVE to the car's own extent so
     * it's always visible: a hit of s_dent_ref_mag dents s_dent_frac% of the
     * half-extent in one go; smaller hits scale down linearly. */
    float ref       = max_ext;                                   /* half the largest model dimension */
    float dent_full = ref * ((float)s_dent_frac / 100.0f);       /* full single-hit dent */
    float sev       = (float)impact_mag / (float)s_dent_ref_mag; /* 1.0 at a reference crash */
    if (sev > 1.0f) sev = 1.0f;
    float dent = dent_full * sev;
    float max_disp = ref * ((float)s_disp_frac / 100.0f);        /* accumulated clamp */
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
        /* clamp accumulated per-vertex displacement (extent-relative) */
        float mag = sqrtf(nxv*nxv + nyv*nyv + nzv*nzv);
        if (mag > max_disp && mag > 1e-4f) {
            float k = max_disp / mag;
            nxv *= k; nyv *= k; nzv *= k;
        }
        ds->dx[i] = nxv; ds->dy[i] = nyv; ds->dz[i] = nzv;

        /* Accumulate per-vertex "scuff" (damage darkening), proportional to the
         * hit severity + falloff, so the struck panels visibly scuff/scorch. */
        float sc = ds->scuff[i] + sev * w;
        ds->scuff[i] = (sc > 1.0f) ? 1.0f : sc;
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

    /* --- Per-EVENT gate: don't keep draining the bar while contact PERSISTS ---
     * A real crash is the moment of impact, not every tick the cars stay touching.
     * If contact lapsed for > s_contact_gap ticks, this is a NEW event -> full
     * damage. While contact continues, only a hit HARDER than the event's running
     * peak deals the difference; a steady grind (mag flat or falling — typical
     * when slow/stopped) deals nothing further. */
    int32_t tick = (int32_t)g_td5.simulation_tick_counter;
    int32_t dt   = tick - s_last_contact_tick[slot];
    int32_t eff;
    if (dt < 0 || dt > s_contact_gap) {
        eff = mag;                              /* fresh collision event */
        s_event_peak_mag[slot] = mag;
    } else if (mag > s_event_peak_mag[slot]) {
        eff = mag - s_event_peak_mag[slot];     /* harder hit mid-contact -> only the extra */
        s_event_peak_mag[slot] = mag;
    } else {
        s_last_contact_tick[slot] = tick;       /* sustained contact: no new damage */
        return;
    }
    s_last_contact_tick[slot] = tick;

    /* Drive health down (port-only formula) using the EVENT-effective magnitude. */
    int64_t dmg64 = (int64_t)eff * (int64_t)s_impact_pct / 100;
    int32_t dmg = (dmg64 > 0x7FFFFFFF) ? 0x7FFFFFFF : (int32_t)dmg64;
    int32_t before = actor->damage_health;
    actor->damage_health -= dmg;
    if (actor->damage_health < 0) actor->damage_health = 0;
    /* cumulative absorbed (saturating) */
    if (actor->damage_accum < 0x7FFFFFFF - dmg) actor->damage_accum += dmg;
    else actor->damage_accum = 0x7FFFFFFF;

    /* Deform the body mesh at the hit region (event-effective magnitude, so a
     * sustained grind doesn't keep crumpling the panel). */
    if (hit) apply_deform(slot, eff, hit);

    /* Knockout transition: fire the broken-down hook once. The health-based
     * knocked-out test (used by the freeze + completion gate) is inherently
     * permanent because health never regenerates. mark_actor_broken_down ends
     * any in-progress cop chase and parks knocked-out TRAFFIC permanently;
     * knocked-out RACERS are immobilized by the physics arrest-style freeze. */
    if (actor->damage_health <= s_knockout && before > s_knockout &&
        !s_ko_notified[slot] && td5_damage_bar_enabled()) {
        s_ko_notified[slot] = 1;
        td5_ai_mark_actor_broken_down(slot);
        TD5_LOG_I(LOG_TAG, "car_damage: KNOCKOUT slot=%d (mag=%d eff=%d dmg=%d hp %d->0)",
                  slot, mag, eff, dmg, before);
    } else {
        TD5_LOG_I(LOG_TAG, "car_damage: hit slot=%d mag=%d eff=%d dmg=%d hp=%d/%d "
                  "lat=%d fwd=%d side=%d tier=%d",
                  slot, mag, eff, dmg, actor->damage_health, s_max_hp,
                  hit ? hit->lat : 0, hit ? hit->fwd : 0, hit ? hit->is_side : 0,
                  td5_damage_smoke_tier(slot));
    }
}

int td5_damage_get_scuff(int slot, const TD5_MeshHeader *mesh,
                         const float **scuff, int *vcount) {
    if (!td5_damage_enabled() || s_scuff_pct <= 0) return 0;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !mesh) return 0;
    DamageSlot *ds = &s_slot[slot];
    if (!ds->dirty || ds->mesh != mesh || !ds->scuff) return 0;
    if (ds->vcount != mesh->total_vertex_count) return 0;
    if (scuff)  *scuff  = ds->scuff;
    if (vcount) *vcount = ds->vcount;
    return 1;
}

float td5_damage_scuff_strength(void) {
    return (float)s_scuff_pct / 100.0f;   /* max darkening fraction at full scuff */
}

void td5_damage_on_wall_impact(TD5_Actor *actor, int32_t approach_speed,
                               const TD5_DamageHit *hit) {
    if (!td5_damage_enabled() || !actor) return;
    int32_t spd = approach_speed < 0 ? -approach_speed : approach_speed;
    /* Convert the wall approach speed into the same impact_mag domain as a V2V
     * hit, then route through the shared damage path (health + deform + smoke). */
    int32_t mag = (int32_t)((int64_t)spd * s_wall_scale / 100);
    td5_damage_on_impact(actor, mag, hit);
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
