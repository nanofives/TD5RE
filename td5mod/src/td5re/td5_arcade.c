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
#include <math.h>     /* sqrt — auto-scale the item-box from the road span length */

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

#define ARC_MAX_PADS      128
#define ARC_MAX_HAZARDS   32
#define ARC_MAX_SLOTS     TD5_ACTOR_MAX_TOTAL_SLOTS   /* 32 */

/* ======================================================================
 * State
 * ====================================================================== */
typedef struct ArcPad {
    int32_t x, y, z;     /* world pos in RENDER units (=world_pos/256); y lifted */
    int16_t span;        /* track ring span this box sits on (pickup trigger)    */
    int8_t  sub_lane;    /* lane the box sits in (sideline); pickup is lane-gated */
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
static int32_t   s_box_half;               /* item-box visual half-size, RENDER units
                                            * (= world_pos/256 scale, like render_pos).
                                            * Auto-derived from the track span length so
                                            * the floating box reads ~1 lane wide on any
                                            * track regardless of absolute world scale. */
static int32_t   s_lane_w;                 /* one lane width, RENDER units (oil-slick /
                                            * hazard trigger + visual are sized off this). */

/* per-racer-slot effect state */
static uint8_t   s_effect[ARC_MAX_SLOTS];        /* TD5_PU_* active, or NONE */
static int16_t   s_effect_frames[ARC_MAX_SLOTS]; /* frames remaining          */

/* per-racer-slot OIL-SLICK drift state (HAZARD victim): the car drifts
 * uncontrollably in a random direction for a few seconds while still rolling. */
static int16_t   s_oiled_frames[ARC_MAX_SLOTS];  /* ticks left drifting on oil */
static int8_t    s_oiled_dir[ARC_MAX_SLOTS];     /* random drift direction (-1/+1) */
static uint32_t  s_oiled_rng[ARC_MAX_SLOTS];     /* per-slot wobble stream     */
static uint32_t  s_oil_event_ctr;                /* decorrelates successive oilings */

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

/* Hazard hit: put a car into an uncontrollable OIL DRIFT for a few seconds. The
 * car keeps rolling forward but slews toward a RANDOM direction (deterministic
 * across peers via the replicated race seed); the per-tick drift is applied in
 * td5_arcade_tick. */
static void start_oil_drift(int slot, TD5_Actor *a) {
    if (slot < 0 || slot >= ARC_MAX_SLOTS || !a) return;
    int frames = knob("TD5RE_ARCADE_OIL_FRAMES", 75, 15, 300);   /* ~2.5 s @30Hz */
    s_oiled_frames[slot] = (int16_t)frames;
    uint32_t seed = td5_game_get_race_seed()
                    ^ (0x9E3779B9u * (uint32_t)(slot + 1))
                    ^ (s_oil_event_ctr++ * 2654435761u);
    if (seed == 0) seed = 0xD1B54A35u;
    s_oiled_rng[slot] = seed;
    s_oiled_dir[slot] = (seed & 1u) ? 1 : -1;                    /* random left / right */
    /* small momentum bleed at entry so the slick visibly grabs the car */
    a->linear_velocity_x -= a->linear_velocity_x / 8;
    a->linear_velocity_z -= a->linear_velocity_z / 8;
}

/* Append one box to s_pads at (span, sub_lane) with a RANDOM kind. sub<0 uses the
 * span-centre lane. Coords come back in RENDER units. Advances *rng for the kind.
 * Returns 1 if a box was placed. */
static int arc_emit_box(int span, int sub, int lanes, int lift, uint32_t *rng) {
    if (s_pad_count >= ARC_MAX_PADS) return 0;
    int x = 0, y = 0, z = 0;
    if (sub >= 0 && lanes > 1) {
        int lx, ly, lz;
        if (td5_track_get_span_lane_world(span, sub, &lx, &ly, &lz)) {
            x = lx / 256; y = ly / 256; z = lz / 256;   /* 24.8 fixed -> render units */
        } else if (!td5_track_get_span_center_world(span, &x, &y, &z)) {
            return 0;
        }
    } else {
        if (!td5_track_get_span_center_world(span, &x, &y, &z)) return 0;
        sub = lanes / 2;
    }
    ArcPad *p = &s_pads[s_pad_count];
    p->x = x; p->y = y + lift; p->z = z;
    p->span = (int16_t)span;
    p->sub_lane = (int8_t)sub;
    *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
    p->kind = (uint8_t)(TD5_PU_NITRO + (*rng % (uint32_t)TD5_PU_KINDS));
    p->active = 1;
    p->respawn = 0;
    s_pad_count++;
    return 1;
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
    memset(s_oiled_frames, 0, sizeof(s_oiled_frames));
    memset(s_oiled_dir, 0, sizeof(s_oiled_dir));
    memset(s_oiled_rng, 0, sizeof(s_oiled_rng));
    s_oil_event_ctr = 0;
    s_pad_count = 0;
    s_active = (td5_physics_get_dynamics() == 0);   /* mode 0 = ARCADE (wild) */

    if (!s_active) {
        TD5_LOG_I(LOG_TAG, "init: SIMULATION mode — arcade pads/effects OFF");
        return;
    }

    /* Game Options "POWER-UPS" toggle: when off, the wild ARCADE collision/launch
     * still applies (that's the DYNAMICS choice) but NO item boxes / pickups /
     * hazards are placed. s_active stays 1; s_pad_count just stays 0. */
    if (g_td5.ini.powerups == 0) {
        s_box_half = 0;
        s_lane_w = 0;
        TD5_LOG_I(LOG_TAG, "init: ARCADE on but POWER-UPS disabled (Game Options) — no item boxes");
        return;
    }

    s_ring = td5_track_get_span_count();
    if (s_ring <= 0) {
        TD5_LOG_W(LOG_TAG, "init: no track ring (%d) — no pads placed", s_ring);
        return;
    }

    /* --- Derive the floating-box size from the road's longitudinal scale.
     * td5_track_get_span_center_world returns RENDER units (= world_pos/256, the
     * same large-magnitude space as actor render_pos — verified at
     * td5_render.c:1469, NOT integer-coord/256). Sizing the box to a fraction of
     * the mean span length makes it read ~1 lane wide on every track without
     * hardcoding an absolute world scale. */
    {
        double acc = 0.0; int samples = 0, prev_ok = 0, px = 0, pz = 0;
        for (int s = 0; s < s_ring && samples < 12; s++) {
            int cx = 0, cy = 0, cz = 0;
            if (!td5_track_get_span_center_world(s, &cx, &cy, &cz)) { prev_ok = 0; continue; }
            if (prev_ok) {
                double dx = (double)(cx - px), dz = (double)(cz - pz);
                double d = sqrt(dx * dx + dz * dz);
                if (d > 1.0) { acc += d; samples++; }
            }
            px = cx; pz = cz; prev_ok = 1;
        }
        int span_len = samples ? (int)(acc / samples) : 256;
        int box_pct  = knob("TD5RE_ARCADE_BOX_PCT", 22, 5, 400);   /* % of span length */
        int box_half = (int)(((int64_t)span_len * box_pct) / 100);
        if (box_half < 16)   box_half = 16;
        if (box_half > 8000) box_half = 8000;
        s_box_half = knob("TD5RE_ARCADE_BOX_SIZE", box_half, 8, 40000);

        /* Lane width (RENDER units) = distance between two adjacent lane centres.
         * get_span_lane_world returns 24.8 fixed, so /256 -> render units. Sample
         * the first span that actually has >= 2 lanes. Drives the oil-slick size
         * + spin-out radius so the hazard is "3 lanes wide" on any track. */
        s_lane_w = 0;
        for (int s = 0; s < s_ring && s < 400; s++) {
            int lc = td5_track_get_span_lane_count(s);
            if (lc < 2) continue;
            int ax, ay, az, bx, by, bz;
            if (td5_track_get_span_lane_world(s, 0, &ax, &ay, &az) &&
                td5_track_get_span_lane_world(s, 1, &bx, &by, &bz)) {
                double ddx = (double)(ax - bx), ddz = (double)(az - bz);
                int w = (int)(sqrt(ddx * ddx + ddz * ddz) / 256.0);  /* fixed -> render */
                if (w > 0) { s_lane_w = w; break; }
            }
        }
        if (s_lane_w <= 0) s_lane_w = s_box_half * 3;   /* fallback */
        TD5_LOG_I(LOG_TAG, "init: scale — mean span_len=%d (n=%d) box_half=%d lane_w=%d [render units]",
                  span_len, samples, s_box_half, s_lane_w);
    }

    /* Don't place boxes right on top of the start line — give the player room.
     * First box lands at START_SPAN (default 100; ~190 m in on a typical strip);
     * the rest spread over the remaining ring. */
    int start_span = knob("TD5RE_ARCADE_START_SPAN", 100, 0, 100000);
    if (start_span >= s_ring) start_span = 0;     /* track shorter than offset */
    int usable = s_ring - start_span;
    if (usable < 1) usable = s_ring;

    /* Frequency scales with the number of HUMAN players: 1 human -> ~SPACING_MAX
     * (300) spans between boxes, falling toward SPACING_MIN (100) as humans are
     * added, so a fuller race has more power-ups to go round. PAD_SPACING overrides. */
    int num_h = g_td5.num_human_players; if (num_h < 1) num_h = 1;
    int sp_max = knob("TD5RE_ARCADE_SPACING_MAX", 300, 20, 4000);
    int sp_min = knob("TD5RE_ARCADE_SPACING_MIN", 100, 10, 4000);
    if (sp_min > sp_max) sp_min = sp_max;
    int spacing = sp_max - (num_h - 1) * ((sp_max - sp_min) / 5);
    if (spacing < sp_min) spacing = sp_min;
    if (spacing > sp_max) spacing = sp_max;
    { const char *e = getenv("TD5RE_ARCADE_PAD_SPACING"); if (e) { int v = atoi(e); if (v >= 6) spacing = v; } }

    int count = usable / spacing;
    if (count < 4)  count = 4;
    if (count > ARC_MAX_PADS) count = ARC_MAX_PADS;

    /* Hang the box just over the road: lift its centre ~1.1x the box half-size so
     * it floats close to the asphalt and the car drives through it (RENDER units). */
    int auto_lift = (int)(((int64_t)s_box_half * 11) / 10);
    int lift = knob("TD5RE_ARCADE_PAD_LIFT", auto_lift, 0, 200000);

    /* Single boxes alternate left/right shoulder; side=0 forces centre (legacy). */
    int side = knob("TD5RE_ARCADE_SIDE", 1, 0, 1);

    /* With >4 HUMAN players, each spawn point has a growing chance to hold TWO
     * boxes — one on the left-most lane, one on the right-most — so a crowded
     * field still has enough to grab. 25% at 5 humans, +15% per extra human. */
    int dbl_pct = (num_h > 4) ? (25 + (num_h - 5) * 15) : 0;
    if (dbl_pct > 90) dbl_pct = 90;

    /* Random kind per box, netplay-deterministic (private xorshift off the
     * REPLICATED per-race seed; no shared CRT rand on the sim path). */
    uint32_t rng = td5_game_get_race_seed() ^ 0x1B0CA9E5u;
    if (rng == 0) rng = 0x9E3779B9u;

    s_pad_count = 0;
    for (int i = 0; i < count && s_pad_count < ARC_MAX_PADS; i++) {
        int span = start_span + (int)(((int64_t)i * usable) / count);
        int lanes = td5_track_get_span_lane_count(span);
        if (lanes < 1) lanes = 1;

        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;       /* roll for double */
        int make_dbl = (dbl_pct > 0) && (lanes > 1) && ((int)(rng % 100u) < dbl_pct);

        if (make_dbl && s_pad_count + 2 <= ARC_MAX_PADS) {
            arc_emit_box(span, 0,         lanes, lift, &rng);      /* left  edge */
            arc_emit_box(span, lanes - 1, lanes, lift, &rng);      /* right edge */
        } else if (side && lanes > 1) {
            arc_emit_box(span, (i & 1) ? (lanes - 1) : 0, lanes, lift, &rng);
        } else {
            arc_emit_box(span, -1, lanes, lift, &rng);            /* centre */
        }
    }

    /* One-shot scale sanity + layout summary. pad[0] and the player's spawn
     * (world_pos/256) are BOTH render units, so they must share magnitude. */
    {
        TD5_Actor *p0 = td5_game_get_actor(0);
        TD5_LOG_I(LOG_TAG,
                  "init: ARCADE on — ring=%d pads=%d humans=%d spacing=%d dbl%%=%d pad0=(%d,%d,%d) player/256=(%d,%d,%d)",
                  s_ring, s_pad_count, num_h, spacing, dbl_pct,
                  s_pad_count ? s_pads[0].x : 0,
                  s_pad_count ? s_pads[0].y : 0,
                  s_pad_count ? s_pads[0].z : 0,
                  p0 ? (int)(p0->world_pos.x / 256) : 0,
                  p0 ? (int)(p0->world_pos.y / 256) : 0,
                  p0 ? (int)(p0->world_pos.z / 256) : 0);
    }
}

void td5_arcade_shutdown_race(void) {
    s_active = 0;
    s_pad_count = 0;
    memset(s_haz, 0, sizeof(s_haz));
    memset(s_effect, 0, sizeof(s_effect));
    memset(s_effect_frames, 0, sizeof(s_effect_frames));
    memset(s_oiled_frames, 0, sizeof(s_oiled_frames));
    memset(s_oiled_dir, 0, sizeof(s_oiled_dir));
    memset(s_oiled_rng, 0, sizeof(s_oiled_rng));
}

/* ======================================================================
 * Per-tick update
 * ====================================================================== */

void td5_arcade_tick(void) {
    if (!s_active) return;

    int racers = racer_count();
    int allow_ai = knob("TD5RE_ARCADE_AI_PICKUPS", 1, 0, 1);
    /* Pickup "hitbox": collect when within this many spans of the box
     * (longitudinal, back to the original tight 2) AND within this many lanes of
     * the box's side lane (lateral) — the side placement needs the lane gate. */
    int pick_win = knob("TD5RE_ARCADE_PICKUP_SPANS", 2, 1, 30);
    int lane_tol = knob("TD5RE_ARCADE_PICKUP_LANES", 1, 0, 8);

    /* --- decay per-slot effect timers --- */
    for (int s = 0; s < racers; s++) {
        if (s_effect_frames[s] > 0) {
            if (--s_effect_frames[s] <= 0) s_effect[s] = TD5_PU_NONE;
        }
    }

    /* --- OIL-SLICK drift: while a car is "oiled" it slews uncontrollably toward
     * its random direction (with wobble) but keeps rolling forward. Runs AFTER
     * physics (tick is post-physics), so the yaw perturbation integrates next
     * tick. Deterministic: the per-slot wobble stream + direction come from the
     * replicated race seed, so every peer/replay drifts identically. --- */
    {
        int yaw_base = knob("TD5RE_ARCADE_OIL_YAW", 0x300, 0x40, 0x4000);
        for (int s = 0; s < racers; s++) {
            if (s_oiled_frames[s] <= 0) continue;
            TD5_Actor *a = slot_actor(s);
            if (!a || a->finish_time != 0) { s_oiled_frames[s] = 0; continue; }
            s_oiled_rng[s] ^= s_oiled_rng[s] << 13;
            s_oiled_rng[s] ^= s_oiled_rng[s] >> 17;
            s_oiled_rng[s] ^= s_oiled_rng[s] << 5;
            int wob = (int)(s_oiled_rng[s] % 1201u) - 600;        /* -600..600 wobble */
            a->angular_velocity_yaw += s_oiled_dir[s] * yaw_base + wob;
            s_oiled_frames[s]--;
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
        /* One power-up at a time: while an effect is active you can't grab another
         * box (the box stays for someone else). */
        if (s_effect[s] != TD5_PU_NONE) continue;
        if (!allow_ai) {
            /* humans only: in split-screen the human players occupy the lowest
             * slots [0, num_human_players); everything above is AI-driven. */
            if (s >= g_td5.num_human_players) continue;
        }
        int aspan = a->track_span_normalized;
        if (aspan < 0) continue;
        int alane = (int)a->track_sub_lane_index;       /* the car's lane this tick */
        for (int i = 0; i < s_pad_count; i++) {
            ArcPad *p = &s_pads[i];
            if (!p->active) continue;
            int fdiff = ((aspan - p->span) % s_ring + s_ring) % s_ring;
            if (fdiff > pick_win) continue;             /* not at the box longitudinally */
            int ldiff = alane - (int)p->sub_lane;
            if (ldiff < 0) ldiff = -ldiff;
            if (ldiff > lane_tol) continue;             /* not on the box's side — steer over */
            {
                apply_pickup(s, p->kind, a);
                p->active = 0;
                /* Respawn the box 5 s after pickup. At the fixed 30 Hz sim tick
                 * that's 150 ticks. */
                p->respawn = (int16_t)knob("TD5RE_ARCADE_PAD_RESPAWN", 150, 15, 1800);
            }
        }
    }

    /* --- hazards: TTL + spin-out any car within ~3 lanes of the slick ---
     * Radius is 1.5 lanes (so the slick is "3 lanes wide"), in RENDER units. The
     * old default (1024) was treated as 24.8 fixed = ~4 render units, far smaller
     * than a car, so the slick almost never triggered. */
    int radius = (s_lane_w * 3) / 2;
    { const char *e = getenv("TD5RE_ARCADE_HAZARD_RADIUS"); if (e) { int v = atoi(e); if (v > 0) radius = v; } }
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
            /* >>8 converts the 24.8-fixed world delta to RENDER units (radius scale) */
            int64_t dx = ((int64_t)a->world_pos.x - s_haz[h].x) >> 8;
            int64_t dz = ((int64_t)a->world_pos.z - s_haz[h].z) >> 8;
            if (dx*dx + dz*dz <= r2) {
                start_oil_drift(s, a);
                TD5_LOG_I(LOG_TAG, "slot=%d hit OIL — drifting (owner=%d)", s, s_haz[h].owner);
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
    /* p->x/y/z are already RENDER units (the scale td5_render_load_translation /
     * actor render_pos use) — return verbatim. NO /256 here: that extra divide is
     * the old bug that shrank every pad's coords 256x (placing them at the origin,
     * off the track). Hazards differ: they store actor world_pos (24.8 fixed), so
     * td5_arcade_hazard_get DOES /256 below. */
    if (wx) *wx = (float)p->x;
    if (wy) *wy = (float)p->y;
    if (wz) *wz = (float)p->z;
    if (active) *active = p->active;
    if (kind)   *kind   = p->kind;
    return 1;
}

/* Item-box visual half-size in RENDER units (auto-scaled from the span length at
 * race init). The renderer sizes the rotating cube / icon / glow off this. 0 when
 * arcade mode is inactive. */
float td5_arcade_box_half_world(void) {
    return s_active ? (float)s_box_half : 0.0f;
}

/* Oil-slick / hazard radius in RENDER units (1.5 lanes -> the slick is 3 lanes
 * wide). The renderer sizes the oil puddle off this so the visual matches the
 * spin-out trigger area. 0 outside arcade mode. */
float td5_arcade_hazard_radius_world(void) {
    return s_active ? (float)((s_lane_w * 3) / 2) : 0.0f;
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
