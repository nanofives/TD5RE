/**
 * td5_arcade.c -- ARCADE mode: 3x-collision launch + collectible road power-ups
 *
 * PORT-ONLY (no original-binary analog). See td5_arcade.h for the model.
 *
 * Determinism: pad layout + kinds are derived once at race init from the track
 * ring (td5_track_get_span_count / td5_track_get_span_center_world) with NO RNG;
 * pickup detection keys off the REPLICATED per-actor track span (+0x082); every
 * effect mutates replicated actor state with integer fixed-point math. This keeps
 * netplay/replay in lockstep (the same guarantee td5_msvc_rand gives the sim).
 */

#include "td5_arcade.h"

/* td5_types.h MUST precede td5_actor_struct.h: the actor header guards its
 * TD5_Mat3x3 typedef on TD5_TYPES_H and #defines TD5_VMODE_* that collide with
 * td5_types.h's enum of the same names unless the enum is parsed first. */
#include "td5_types.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_physics.h"
#include "td5re.h"      /* g_td5 (num_human_players, g_traffic_slot_base) */
#include "td5_platform.h"
#include "../../../re/include/td5_actor_struct.h"

#include <stdlib.h>   /* getenv, atoi */
#include <string.h>   /* memset */

#define LOG_TAG "arcade"

/* ======================================================================
 * Tunables (all overridable via env knob; cached on first read)
 * ====================================================================== */
static int knob(const char *name, int dflt, int lo, int hi) {
    const char *e = getenv(name);
    int v = e ? atoi(e) : dflt;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

#define ARC_MAX_PADS      64
#define ARC_MAX_HAZARDS   32
#define ARC_MAX_SLOTS     TD5_ACTOR_MAX_TOTAL_SLOTS   /* 32 */

/* ======================================================================
 * State
 * ====================================================================== */
typedef struct ArcPad {
    int32_t x, y, z;     /* world pos (24.8 fixed); y already lifted for render */
    int16_t span;        /* track ring span this pad sits on (pickup trigger)   */
    uint8_t kind;        /* TD5_PU_*                                            */
    uint8_t active;      /* 1 = collectable, 0 = dormant (respawning)           */
    int16_t respawn;     /* ticks until re-activation when dormant              */
} ArcPad;

typedef struct ArcHazard {
    int32_t x, y, z;     /* world pos (24.8 fixed); y = drop height for render */
    int16_t ttl;         /* ticks remaining        */
    uint8_t owner;       /* slot that dropped it    */
    uint8_t grace;       /* ticks before owner can be hit by their own drop */
} ArcHazard;

static int       s_inited;
static int       s_active;                 /* arcade mode active for THIS race */
static ArcPad    s_pads[ARC_MAX_PADS];
static int       s_pad_count;
static ArcHazard s_haz[ARC_MAX_HAZARDS];
static int       s_ring;                   /* cached span count */

/* per-racer-slot effect state */
static uint8_t   s_effect[ARC_MAX_SLOTS];        /* TD5_PU_* active, or NONE */
static int16_t   s_effect_frames[ARC_MAX_SLOTS]; /* frames remaining          */

/* ======================================================================
 * Helpers
 * ====================================================================== */

/* Number of racer slots in play (humans + AI). g_traffic_slot_base = first
 * traffic slot, so racers are [0, g_traffic_slot_base). */
static int racer_count(void) {
    int n = g_traffic_slot_base;
    if (n < 1) n = TD5_MAX_RACER_SLOTS;
    if (n > ARC_MAX_SLOTS) n = ARC_MAX_SLOTS;
    return n;
}

static TD5_Actor *slot_actor(int slot) {
    return td5_game_get_actor(slot);
}

/* Trigger power-up `kind` on racer `slot` immediately. */
static void apply_pickup(int slot, int kind, TD5_Actor *a) {
    switch (kind) {
    case TD5_PU_NITRO: {
        /* One-shot horizontal-speed burst along the current travel direction. */
        int pct = knob("TD5RE_ARCADE_NITRO_PCT", 60, 10, 300);
        int32_t vx = a->linear_velocity_x, vz = a->linear_velocity_z;
        a->linear_velocity_x = vx + (int32_t)(( (int64_t)vx * pct) / 100);
        a->linear_velocity_z = vz + (int32_t)(( (int64_t)vz * pct) / 100);
        s_effect[slot] = TD5_PU_NITRO;
        s_effect_frames[slot] = 30;   /* HUD/FX flash only */
        TD5_LOG_I(LOG_TAG, "slot=%d NITRO +%d%%", slot, pct);
        break;
    }
    case TD5_PU_GHOST:
        s_effect[slot] = TD5_PU_GHOST;
        s_effect_frames[slot] = knob("TD5RE_ARCADE_GHOST_FRAMES", 120, 30, 600);
        TD5_LOG_I(LOG_TAG, "slot=%d GHOST %d frames", slot, s_effect_frames[slot]);
        break;
    case TD5_PU_WRECK:
        s_effect[slot] = TD5_PU_WRECK;
        s_effect_frames[slot] = knob("TD5RE_ARCADE_WRECK_FRAMES", 150, 30, 600);
        TD5_LOG_I(LOG_TAG, "slot=%d WRECKING BALL %d frames", slot, s_effect_frames[slot]);
        break;
    case TD5_PU_HAZARD: {
        /* Drop a hazard a little behind the car (along reverse travel dir). */
        int32_t hx = a->world_pos.x, hz = a->world_pos.z;
        int32_t vx = a->linear_velocity_x, vz = a->linear_velocity_z;
        int64_t sp2 = (int64_t)vx*vx + (int64_t)vz*vz;
        if (sp2 > (int64_t)64*64) {
            /* normalize (vx,vz), step back ~3 world units (768 fixed) */
            int sp = 0; int64_t t = sp2; while (t > 0) { sp++; t >>= 2; } /* rough */
            /* cheap reverse offset: scale velocity vector down by a constant */
            hx -= vx / 4;
            hz -= vz / 4;
            (void)sp;
        }
        for (int h = 0; h < ARC_MAX_HAZARDS; h++) {
            if (s_haz[h].ttl <= 0) {
                s_haz[h].x = hx; s_haz[h].y = a->world_pos.y; s_haz[h].z = hz;
                s_haz[h].ttl = (int16_t)knob("TD5RE_ARCADE_HAZARD_TTL", 900, 60, 1800);
                s_haz[h].owner = (uint8_t)slot;
                s_haz[h].grace = 30;   /* don't immediately trip your own drop */
                break;
            }
        }
        s_effect[slot] = TD5_PU_HAZARD;
        s_effect_frames[slot] = 20;   /* HUD flash "dropped" */
        TD5_LOG_I(LOG_TAG, "slot=%d HAZARD dropped", slot);
        break;
    }
    default: break;
    }
}

/* Spin a car out (hazard hit): big yaw kick + halve horizontal speed, once. */
static void spin_out(TD5_Actor *a) {
    int spin = knob("TD5RE_ARCADE_HAZARD_SPIN", 0x6000, 0x1000, 0x7FFF);
    /* alternate spin sign by current lateral-speed sign so it feels like a slide */
    int sign = (a->lateral_speed >= 0) ? 1 : -1;
    a->angular_velocity_yaw += sign * spin;
    a->linear_velocity_x /= 2;
    a->linear_velocity_z /= 2;
}

/* ======================================================================
 * Lifecycle
 * ====================================================================== */

void td5_arcade_init_race(void) {
    s_inited = 1;
    memset(s_pads, 0, sizeof(s_pads));
    memset(s_haz,  0, sizeof(s_haz));
    memset(s_effect, 0, sizeof(s_effect));
    memset(s_effect_frames, 0, sizeof(s_effect_frames));
    s_pad_count = 0;
    s_active = (td5_physics_get_dynamics() == 0);   /* mode 0 = ARCADE (wild) */

    if (!s_active) {
        TD5_LOG_I(LOG_TAG, "init: SIMULATION mode — arcade pads/effects OFF");
        return;
    }

    s_ring = td5_track_get_span_count();
    if (s_ring <= 0) {
        TD5_LOG_W(LOG_TAG, "init: no track ring (%d) — no pads placed", s_ring);
        return;
    }

    int spacing = knob("TD5RE_ARCADE_PAD_SPACING", 40, 8, 400);
    int count = s_ring / spacing;
    if (count < 6)  count = 6;
    if (count > ARC_MAX_PADS) count = ARC_MAX_PADS;

    int lift = knob("TD5RE_ARCADE_PAD_LIFT", 384, 0, 4096);  /* ~1.5 world units */
    int placed = 0;
    for (int i = 0; i < count; i++) {
        int span = (int)(((int64_t)i * s_ring) / count);
        int x = 0, y = 0, z = 0;
        if (!td5_track_get_span_center_world(span, &x, &y, &z))
            continue;
        ArcPad *p = &s_pads[placed];
        p->x = x; p->y = y + lift; p->z = z;
        p->span = (int16_t)span;
        p->kind = (uint8_t)(TD5_PU_NITRO + (i % TD5_PU_KINDS));  /* 1..4 cycling */
        p->active = 1;
        p->respawn = 0;
        placed++;
    }
    s_pad_count = placed;
    TD5_LOG_I(LOG_TAG, "init: ARCADE on — ring=%d pads=%d (spacing=%d)",
              s_ring, s_pad_count, spacing);
}

void td5_arcade_shutdown_race(void) {
    s_active = 0;
    s_pad_count = 0;
    memset(s_haz, 0, sizeof(s_haz));
    memset(s_effect, 0, sizeof(s_effect));
    memset(s_effect_frames, 0, sizeof(s_effect_frames));
}

/* ======================================================================
 * Per-tick update
 * ====================================================================== */

void td5_arcade_tick(void) {
    if (!s_active) return;

    int racers = racer_count();
    int allow_ai = knob("TD5RE_ARCADE_AI_PICKUPS", 1, 0, 1);

    /* --- decay per-slot effect timers --- */
    for (int s = 0; s < racers; s++) {
        if (s_effect_frames[s] > 0) {
            if (--s_effect_frames[s] <= 0) s_effect[s] = TD5_PU_NONE;
        }
    }

    /* --- pad respawn cooldowns --- */
    for (int i = 0; i < s_pad_count; i++) {
        if (!s_pads[i].active && s_pads[i].respawn > 0) {
            if (--s_pads[i].respawn <= 0) s_pads[i].active = 1;
        }
    }

    /* --- pickup detection (span-based, scale-independent) --- */
    for (int s = 0; s < racers; s++) {
        TD5_Actor *a = slot_actor(s);
        if (!a) continue;
        if (a->finish_time != 0) continue;             /* finished — no pickups */
        if (!allow_ai) {
            /* humans only: in split-screen the human players occupy the lowest
             * slots [0, num_human_players); everything above is AI-driven. */
            if (s >= g_td5.num_human_players) continue;
        }
        int aspan = a->track_span_normalized;
        if (aspan < 0) continue;
        for (int i = 0; i < s_pad_count; i++) {
            ArcPad *p = &s_pads[i];
            if (!p->active) continue;
            int fdiff = ((aspan - p->span) % s_ring + s_ring) % s_ring;
            if (fdiff <= 2) {                          /* at or just past the pad */
                apply_pickup(s, p->kind, a);
                p->active = 0;
                p->respawn = (int16_t)knob("TD5RE_ARCADE_PAD_RESPAWN", 150, 30, 1200);
            }
        }
    }

    /* --- hazards: TTL + spin-out the next car to touch one --- */
    int radius = knob("TD5RE_ARCADE_HAZARD_RADIUS", 1024, 256, 8192);  /* fixed */
    int64_t r2 = (int64_t)radius * radius;
    for (int h = 0; h < ARC_MAX_HAZARDS; h++) {
        if (s_haz[h].ttl <= 0) continue;
        s_haz[h].ttl--;
        if (s_haz[h].grace > 0) s_haz[h].grace--;
        for (int s = 0; s < racers; s++) {
            if (s_haz[h].grace > 0 && s == s_haz[h].owner) continue;
            TD5_Actor *a = slot_actor(s);
            if (!a) continue;
            if (a->finish_time != 0) continue;
            if (td5_arcade_slot_is_ghost(s)) continue;  /* ghosts pass hazards */
            int64_t dx = (int64_t)a->world_pos.x - s_haz[h].x;
            int64_t dz = (int64_t)a->world_pos.z - s_haz[h].z;
            if (dx*dx + dz*dz <= r2) {
                spin_out(a);
                TD5_LOG_I(LOG_TAG, "slot=%d hit HAZARD (owner=%d)", s, s_haz[h].owner);
                s_haz[h].ttl = 0;   /* one-shot trap */
                break;
            }
        }
    }
}

/* ======================================================================
 * Mode gate
 * ====================================================================== */
int td5_arcade_mode_active(void) { return s_active; }

/* ======================================================================
 * Physics-side queries
 * ====================================================================== */
int td5_arcade_collision_mult_q8(void) {
    if (!s_active) return 0x100;   /* 1.0 — faithful path */
    return knob("TD5RE_ARCADE_COLLISION_MULT", 3, 1, 8) << 8;
}

int td5_arcade_slot_is_ghost(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_effect[slot] == TD5_PU_GHOST;
}

int td5_arcade_slot_is_wrecking(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_effect[slot] == TD5_PU_WRECK;
}

int td5_arcade_launch_active(void) { return s_active; }

int td5_arcade_launch_gate(void) {
    /* Faithful gate is 90000; lower it so ordinary arcade crashes go airborne. */
    return knob("TD5RE_ARCADE_LAUNCH_GATE", 20000, 1000, 90000);
}

int td5_arcade_launch_div(void) {
    /* Faithful divisor is 6 (vel_y = impact_mag/6); smaller = higher launch. */
    return knob("TD5RE_ARCADE_LAUNCH_DIV", 3, 1, 12);
}

void td5_arcade_note_ram(int aggressor, int victim, int impact_mag) {
    (void)aggressor; (void)victim; (void)impact_mag;
    /* reserved hook (scoring / FF could attach here) */
}

/* ======================================================================
 * Render-side queries
 * ====================================================================== */
int td5_arcade_pad_count(void) { return s_active ? s_pad_count : 0; }

int td5_arcade_pad_get(int i, float *wx, float *wy, float *wz,
                       int *active, int *kind) {
    if (!s_active || i < 0 || i >= s_pad_count) return 0;
    ArcPad *p = &s_pads[i];
    if (wx) *wx = (float)p->x * (1.0f / 256.0f);
    if (wy) *wy = (float)p->y * (1.0f / 256.0f);
    if (wz) *wz = (float)p->z * (1.0f / 256.0f);
    if (active) *active = p->active;
    if (kind)   *kind   = p->kind;
    return 1;
}

int td5_arcade_hazard_count(void) { return s_active ? ARC_MAX_HAZARDS : 0; }

int td5_arcade_hazard_get(int i, float *wx, float *wy, float *wz, int *owner) {
    if (!s_active || i < 0 || i >= ARC_MAX_HAZARDS) return 0;
    if (s_haz[i].ttl <= 0) return 0;
    if (wx) *wx = (float)s_haz[i].x * (1.0f / 256.0f);
    if (wy) *wy = (float)s_haz[i].y * (1.0f / 256.0f);
    if (wz) *wz = (float)s_haz[i].z * (1.0f / 256.0f);
    if (owner) *owner = s_haz[i].owner;
    return 1;
}

float td5_arcade_slot_render_alpha(int slot) {
    if (td5_arcade_slot_is_ghost(slot)) {
        return knob("TD5RE_ARCADE_GHOST_ALPHA_PCT", 35, 5, 95) / 100.0f;
    }
    return 1.0f;
}

/* ======================================================================
 * HUD-side queries
 * ====================================================================== */
int td5_arcade_active_effect(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return TD5_PU_NONE;
    return s_effect[slot];
}

int td5_arcade_active_frames(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_effect_frames[slot];
}
