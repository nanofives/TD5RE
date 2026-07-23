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
#include "td5_ai.h"     /* td5_ai_actor_is_broken_down (battle dedup) */
#include "td5_damage.h" /* [2026-07-04] td5_damage_enabled/repair_actor — INDESTRUCTIBLE
                         * + REPAIR gating and REPAIR's actual repair action */
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
static int16_t   s_effect_max[ARC_MAX_SLOTS];    /* frames the active effect STARTED with
                                                  * (full duration) — the HUD bar's 100%
                                                  * reference, so it shows the whole timer
                                                  * and not just the last seconds. */

/* per-racer-slot OIL-SLICK drift state (HAZARD victim): the car drifts
 * uncontrollably in a random direction for a few seconds while still rolling. */
static int16_t   s_oiled_frames[ARC_MAX_SLOTS];  /* ticks left drifting on oil */
static int8_t    s_oiled_dir[ARC_MAX_SLOTS];     /* random drift direction (-1/+1) */
static uint32_t  s_oiled_rng[ARC_MAX_SLOTS];     /* per-slot wobble stream     */
static uint32_t  s_oil_event_ctr;                /* decorrelates successive oilings */

/* [FREEZE REWORK 2026-07-04] per-slot freeze-VICTIM recovery state: a car just
 * rammed by a FREEZE holder has its speed cut to 1/3, then eased back to normal
 * over s_frozen_total ticks (td5_arcade_tick ramps a Q8 scale each tick). Covers
 * traffic slots too, so "whatever you crash" includes background cars. This is
 * the VICTIM's state — distinct from s_effect[]==TD5_PU_FREEZE, which is the
 * HOLDER's own buff (arms the crash-speed-cut on their next heavy ram). */
static int16_t   s_frozen_frames[ARC_MAX_SLOTS];
static int16_t   s_frozen_total[ARC_MAX_SLOTS];  /* frames the recovery ramp STARTED with */

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

/* [ARCADE EXPANSION 2026-06-28] Racers + traffic slot span — effects that reach
 * traffic (FREEZE, MAGNET) iterate over this so they can grab/freeze background
 * cars, not just rival racers. */
static int total_slots(void) {
    int n = g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS;
    if (n < 1) n = TD5_MAX_RACER_SLOTS;
    if (n > ARC_MAX_SLOTS) n = ARC_MAX_SLOTS;
    return n;
}

static TD5_Actor *slot_actor(int slot) {
    return td5_game_get_actor(slot);
}

/* [ARCADE EXPANSION 2026-06-28, re-tuned 2026-07-04] Weighted random power-up
 * kind from a private xorshift stream seeded off td5_game_get_race_seed() (NO
 * shared CRT rand on the sim path -> netplay/replay-deterministic). Advances
 * *rng. Three weights are now CONDITIONAL (computed fresh per call, cheap —
 * called once per box at race init, never per-tick):
 *   - INDESTRUCTIBLE (was WRECK): "should not be rare" -> common tier (5), but
 *     only offered when car damage is enabled (it's meaningless without a
 *     damage system to be immune to). 0 when damage is off.
 *   - MAGNET: only obtainable in Traffic Battle mode (its whole point is
 *     dragging traffic into your ram path). 0 in normal races.
 *   - REPAIR: only offered when car damage is enabled (nothing to repair
 *     otherwise). 0 when damage is off.
 * SHIELD and ROCKET are fully removed (not just weight-zeroed like HAZARD) —
 * their ids are retired, not reused (see td5_arcade.h). */
static int arc_pick_kind(uint32_t *rng) {
    /* weight per kind, indexed [TD5_PU_NITRO .. TD5_PU_REPAIR] */
    uint8_t w[TD5_PU_KINDS + 1] = {
        0,  /* TD5_PU_NONE   (unused) */
        5,  /* TD5_PU_NITRO  — common */
        3,  /* TD5_PU_GHOST          */
        0,  /* TD5_PU_INDESTRUCTIBLE — set below (common, damage-gated) */
        0,  /* TD5_PU_HAZARD — REMOVED 2026-06-28 (no longer spawned)            */
        0,  /* id 5 (was SHIELD) — REMOVED 2026-07-04, never spawned */
        3,  /* TD5_PU_FREEZE         */
        0,  /* TD5_PU_MAGNET — set below (Traffic Battle only) */
        0,  /* id 8 (was ROCKET) — REMOVED 2026-07-04, never spawned */
        0,  /* TD5_PU_REPAIR — set below (damage-gated) */
    };
    if (td5_damage_enabled()) {
        w[TD5_PU_INDESTRUCTIBLE] = 5;   /* common, not rare — per "should not be rare" */
        w[TD5_PU_REPAIR]         = 3;
    }
    if (td5_game_battle_mode_active()) {
        w[TD5_PU_MAGNET] = 3;
    }
    int total = 0;
    for (int k = TD5_PU_NITRO; k <= TD5_PU_KINDS; k++) total += w[k];
    if (total <= 0) total = 1;
    *rng ^= *rng << 13; *rng ^= *rng >> 17; *rng ^= *rng << 5;
    int r = (int)(*rng % (uint32_t)total);
    for (int k = TD5_PU_NITRO; k <= TD5_PU_KINDS; k++) {
        r -= w[k];
        if (r < 0) return k;
    }
    return TD5_PU_NITRO;
}

/* Trigger power-up `kind` on racer `slot` immediately. */
static void apply_pickup(int slot, int kind, TD5_Actor *a) {
    switch (kind) {
    case TD5_PU_NITRO: {
        /* [2026-06-27] Sustained ACCELERATION boost (was a one-shot velocity
         * bump). While NITRO is active the racer's drive torque is scaled by
         * TD5RE_ARCADE_NITRO_ACCEL_PCT (default 250 = 2.5x) — applied at the
         * physics drive-torque chokepoint via td5_arcade_slot_accel_q8(). The
         * boost lasts a real, longer window (TD5RE_ARCADE_NITRO_FRAMES, default
         * 150 = ~5 s @30Hz) instead of the old single-tick speed kick. Pure
         * acceleration: the per-car top-speed gate in the physics callers is
         * untouched, so the car builds speed harder but is not warped past its
         * cap. The (void)a keeps the unused-param warning away now the burst is
         * gone. */
        (void)a;
        s_effect[slot] = TD5_PU_NITRO;
        /* [2026-06-28] Duration +20% (150 -> 180, ~6 s @30Hz) per "last 20% more". */
        s_effect_frames[slot] = knob("TD5RE_ARCADE_NITRO_FRAMES", 180, 30, 600);
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d NITRO %d frames (accel x%d%%)",
                  slot, s_effect_frames[slot],
                  knob("TD5RE_ARCADE_NITRO_ACCEL_PCT", 250, 100, 800));
        break;
    }
    case TD5_PU_GHOST:
        s_effect[slot] = TD5_PU_GHOST;
        /* [2026-06-27] Duration doubled 120 -> 240 (~8 s @30Hz) per "last twice
         * as long". [2026-06-28] +20% on top (240 -> 288, ~9.6 s) per "last 20%
         * more". Knob TD5RE_ARCADE_GHOST_FRAMES still overrides. */
        s_effect_frames[slot] = knob("TD5RE_ARCADE_GHOST_FRAMES", 288, 30, 1200);
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d GHOST %d frames", slot, s_effect_frames[slot]);
        break;
    case TD5_PU_INDESTRUCTIBLE:
        /* [RENAMED 2026-07-04, was TD5_PU_WRECK] Only ever offered when car
         * damage is enabled (arc_pick_kind gates the spawn weight) and no
         * longer rare (common tier alongside NITRO). Behavior unchanged: immune
         * to hits, everything you ram gets the boosted launch. */
        s_effect[slot] = TD5_PU_INDESTRUCTIBLE;
        s_effect_frames[slot] = knob("TD5RE_ARCADE_INDESTRUCTIBLE_FRAMES", 360, 30, 1200);
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d INDESTRUCTIBLE %d frames", slot, s_effect_frames[slot]);
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
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d HAZARD dropped", slot);
        break;
    }
    /* [ARCADE EXPANSION 2026-06-28] ---- new kinds ---- */
    /* TD5_PU_SHIELD -- [REMOVED 2026-07-04] no case; id retired. */
    case TD5_PU_FREEZE:
        /* [REWORKED 2026-07-04] No longer an instant EMP burst. FREEZE is now a
         * HOLDER buff: while it's active, your next heavy rams cut the VICTIM's
         * speed to 1/3 and ease it back to normal over ~3s (see td5_arcade_tick
         * and td5_arcade_note_ram, which does the actual cut/arm on a ram event —
         * this only starts the "loaded" window). Glow is drawn on the victim, not
         * the holder (td5_arcade_slot_is_freeze_victim). */
        (void)a;
        s_effect[slot] = TD5_PU_FREEZE;
        s_effect_frames[slot] = knob("TD5RE_ARCADE_FREEZE_FRAMES", 300, 30, 1200);
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d FREEZE armed %d frames (next rams slow the victim)",
                  slot, s_effect_frames[slot]);
        break;
    case TD5_PU_MAGNET:
        /* Tractor beam: while active, nearby TRAFFIC is dragged toward you each
         * tick (ram fuel for the battle) — applied in td5_arcade_tick. Only ever
         * offered in Traffic Battle mode (arc_pick_kind gates the spawn weight). */
        (void)a;
        s_effect[slot] = TD5_PU_MAGNET;
        s_effect_frames[slot] = knob("TD5RE_ARCADE_MAGNET_FRAMES", 240, 30, 1200);
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d MAGNET %d frames", slot, s_effect_frames[slot]);
        break;
    /* TD5_PU_ROCKET -- [REMOVED 2026-07-04] no case; id retired. */
    case TD5_PU_REPAIR:
        /* [2026-07-04] Only ever offered when car damage is enabled (arc_pick_kind
         * gates the spawn weight). Now actually REPAIRS the car — restores health
         * + clears dents via td5_damage_repair_actor — in addition to the
         * original "get out of trouble" clear of your own oil-drift / freeze-
         * victim recovery state. Inert (no-op) inside td5_damage_repair_actor
         * itself when CarDamage is off, but the gated spawn means that shouldn't
         * happen in practice. */
        (void)a;
        s_oiled_frames[slot]  = 0;
        s_frozen_frames[slot] = 0;
        td5_damage_repair_actor(slot);
        s_effect[slot] = TD5_PU_REPAIR;
        s_effect_frames[slot] = 30;   /* HUD flash "REPAIR" */
        s_effect_max[slot]    = s_effect_frames[slot];
        TD5_LOG_I(LOG_TAG, "slot=%d REPAIR (health/dents restored, oil/freeze cleared)", slot);
        break;
    default: break;
    }
}

/* Hazard hit: put a car into an uncontrollable OIL DRIFT for a few seconds. The
 * car keeps rolling forward but slews toward a RANDOM direction (deterministic
 * across peers via the replicated race seed); the per-tick drift is applied in
 * td5_arcade_tick. */
static void start_oil_drift(int slot, TD5_Actor *a) {
    if (slot < 0 || slot >= ARC_MAX_SLOTS || !a) return;
    /* [2026-06-27] Oil-drift duration doubled 75 -> 150 (~5 s @30Hz) per "last
     * twice as long". This is the HAZARD power-up's effect on its victim. */
    int frames = knob("TD5RE_ARCADE_OIL_FRAMES", 150, 15, 600);   /* ~5 s @30Hz */
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
    /* [ARCADE EXPANSION 2026-06-28] Weighted kind (was uniform pad_index%KINDS) —
     * keeps the deterministic per-race seed stream but favours common boosts. */
    p->kind = (uint8_t)arc_pick_kind(rng);
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
    memset(s_effect_max, 0, sizeof(s_effect_max));
    memset(s_oiled_frames, 0, sizeof(s_oiled_frames));
    memset(s_oiled_dir, 0, sizeof(s_oiled_dir));
    memset(s_oiled_rng, 0, sizeof(s_oiled_rng));
    memset(s_frozen_frames, 0, sizeof(s_frozen_frames));
    memset(s_frozen_total, 0, sizeof(s_frozen_total));
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
    /* [DRAG 2026-06-28] Drag race force-overrides power-ups OFF regardless of the
     * Game Options toggle — a clean lane-change sprint, no item boxes / pickups /
     * hazards. (The dynamics choice still decides arcade-vs-sim collisions.) */
    if (g_td5.ini.powerups == 0 || g_td5.drag_race_enabled) {
        s_box_half = 0;
        s_lane_w = 0;
        TD5_LOG_I(LOG_TAG, "init: %s — no item boxes/hazards",
                  g_td5.drag_race_enabled ? "DRAG RACE (power-ups force-off override)"
                                          : "ARCADE on but POWER-UPS disabled (Game Options)");
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
    /* [2026-06-28] Spacing defaults lowered again (150/50 -> 100/40) so the ring
     * holds yet more item boxes ("more / more-frequent power-ups"). Smaller
     * spacing = more boxes (count = usable / spacing). Knobs
     * TD5RE_ARCADE_SPACING_MAX / _MIN / TD5RE_ARCADE_PAD_SPACING still override. */
    int sp_max = knob("TD5RE_ARCADE_SPACING_MAX", 100, 20, 4000);
    int sp_min = knob("TD5RE_ARCADE_SPACING_MIN", 40, 10, 4000);
    if (sp_min > sp_max) sp_min = sp_max;
    int spacing = sp_max - (num_h - 1) * ((sp_max - sp_min) / 5);
    if (spacing < sp_min) spacing = sp_min;
    if (spacing > sp_max) spacing = sp_max;
    /* [ARCADE EXPANSION 2026-06-28] Optional DENSITY tier (0=sparse, 1=normal,
     * 2=dense, 3=mega) scales the spacing so a player (or a TRAFFIC BATTLE) can
     * dial the power-up flood up or down. Source priority:
     *   TD5RE_ARCADE_DENSITY knob  >  battle_powerup_density (MP-replicated)  >  1.
     * Denser tier => smaller spacing => more boxes. Deterministic (config only). */
    {
        int dens = -1;
        const char *de = getenv("TD5RE_ARCADE_DENSITY");
        if (de && de[0]) dens = atoi(de);
        else if (td5_game_battle_mode_active())
            dens = g_td5.mp_mode_config.battle_powerup_density;
        if (dens >= 0) {
            if (dens > 3) dens = 3;
            static const int k_dens_pct[4] = { 150, 100, 65, 40 };  /* % of spacing */
            spacing = (spacing * k_dens_pct[dens]) / 100;
            if (spacing < 6) spacing = 6;
        }
    }
    { const char *e = getenv("TD5RE_ARCADE_PAD_SPACING"); if (e) { int v = atoi(e); if (v >= 6) spacing = v; } }

    int count = usable / spacing;
    /* [2026-06-27] Minimum box count raised 4 -> 8 so even short tracks get a
     * healthy scatter of power-ups. */
    if (count < 8)  count = 8;
    if (count > ARC_MAX_PADS) count = ARC_MAX_PADS;

    /* Hang the box just over the road: lift its centre ~1.1x the box half-size so
     * it floats close to the asphalt and the car drives through it (RENDER units). */
    int auto_lift = (int)(((int64_t)s_box_half * 11) / 10);
    int lift = knob("TD5RE_ARCADE_PAD_LIFT", auto_lift, 0, 200000);

    /* Single boxes alternate left/right shoulder; side=0 forces centre (legacy). */
    int side = knob("TD5RE_ARCADE_SIDE", 1, 0, 1);

    /* [2026-06-28] Pull the shoulder boxes one lane in toward the centreline (per
     * "one lane closer to the center"). 0 restores the original outer-edge
     * placement. The chosen lanes are clamped per-span below so the two sides
     * never cross on a narrow road. */
    int lane_inset = knob("TD5RE_ARCADE_LANE_INSET", 1, 0, 8);

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

        /* Shoulder lanes, inset one lane toward centre. Clamp into [0,lanes-1]
         * and collapse to the centre lane if the inset would make them cross. */
        int left_lane  = lane_inset;
        int right_lane = lanes - 1 - lane_inset;
        if (left_lane  > lanes - 1) left_lane  = lanes - 1;
        if (right_lane < 0)         right_lane = 0;
        if (left_lane  > right_lane) left_lane = right_lane = lanes / 2;

        /* [ITEM CHAOS 2026-07-04] Mashed-style: ONE box in EVERY lane at this
         * spawn point (up to 4 wide on a 4-lane span), instead of the regular
         * mode's 1-2 boxes. Spawn-point spacing/frequency along the ring is
         * unchanged — this only changes how many boxes land at each point. */
        if (g_td5.ini.powerups == 2) {   /* [ITEM CHAOS] 0=OFF 1=CASUAL 2=CHAOS */
            int nlanes = lanes; if (nlanes > 4) nlanes = 4;
            for (int ln = 0; ln < nlanes && s_pad_count < ARC_MAX_PADS; ln++)
                arc_emit_box(span, ln, lanes, lift, &rng);
        } else if (make_dbl && s_pad_count + 2 <= ARC_MAX_PADS) {
            arc_emit_box(span, left_lane,  lanes, lift, &rng);     /* left  shoulder */
            arc_emit_box(span, right_lane, lanes, lift, &rng);     /* right shoulder */
        } else if (side && lanes > 1) {
            arc_emit_box(span, (i & 1) ? right_lane : left_lane, lanes, lift, &rng);
        } else {
            arc_emit_box(span, -1, lanes, lift, &rng);            /* centre */
        }
    }

    /* One-shot scale sanity + layout summary. pad[0] and the player's spawn
     * (world_pos/256) are BOTH render units, so they must share magnitude. */
    {
        TD5_Actor *p0 = td5_game_get_actor(0);
        TD5_LOG_I(LOG_TAG,
                  "init: ARCADE on — ring=%d pads=%d humans=%d spacing=%d dbl%%=%d powerups_mode=%d pad0=(%d,%d,%d) player/256=(%d,%d,%d)",
                  s_ring, s_pad_count, num_h, spacing, dbl_pct, g_td5.ini.powerups,
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
    memset(s_effect_max, 0, sizeof(s_effect_max));
    memset(s_oiled_frames, 0, sizeof(s_oiled_frames));
    memset(s_oiled_dir, 0, sizeof(s_oiled_dir));
    memset(s_oiled_rng, 0, sizeof(s_oiled_rng));
    memset(s_frozen_frames, 0, sizeof(s_frozen_frames));
    memset(s_frozen_total, 0, sizeof(s_frozen_total));
}

/* ======================================================================
 * Per-tick update
 * ====================================================================== */

void td5_arcade_tick(void) {
    if (!s_active) return;

    int racers = racer_count();
    int allow_ai = knob("TD5RE_ARCADE_AI_PICKUPS", 1, 0, 1);
    /* Pickup "hitbox": collect when within this many spans of the box
     * (longitudinal) AND within this many lanes of the box's side lane
     * (lateral) — the side placement needs the lane gate. [2026-07-23] Longitudinal
     * window tightened 2 -> 1 span: with 2, the box was collectable across ~3
     * spans of track, so in chaos mode (many boxes) it was unpredictable which one
     * you grabbed. Knob TD5RE_ARCADE_PICKUP_SPANS still overrides. */
    int pick_win = knob("TD5RE_ARCADE_PICKUP_SPANS", 1, 1, 30);
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

    /* [FREEZE REWORK 2026-07-04] Crash-victim recovery ramp: a car just rammed
     * by a FREEZE holder (armed in td5_arcade_note_ram) has its speed scaled by
     * a factor that grows from 1/3 (the tick right after the hit) up to 1.0
     * across the recovery window, so it visibly struggles back up to speed
     * instead of snapping back instantly. Runs for racers AND traffic ("whatever
     * you crash"). Deterministic (replicated velocity + config only). Glow is
     * read separately via td5_arcade_slot_is_freeze_victim (render side). */
    for (int s = 0; s < total_slots(); s++) {
        if (s_frozen_frames[s] <= 0) continue;
        TD5_Actor *a = slot_actor(s);
        if (!a || a->finish_time != 0) { s_frozen_frames[s] = 0; s_frozen_total[s] = 0; continue; }
        int total = s_frozen_total[s] > 0 ? s_frozen_total[s] : 1;
        int elapsed = total - s_frozen_frames[s];
        /* Q8 scale ramps ~1/3 (85/256) -> 1.0 (256/256) linearly across the window. */
        int32_t scale_q8 = (int32_t)(85 + ((171LL * elapsed) / total));
        if (scale_q8 > 256) scale_q8 = 256;
        a->linear_velocity_x = (int32_t)(((int64_t)a->linear_velocity_x * scale_q8) >> 8);
        a->linear_velocity_z = (int32_t)(((int64_t)a->linear_velocity_z * scale_q8) >> 8);
        s_frozen_frames[s]--;
    }

    /* [ARCADE EXPANSION 2026-06-28] MAGNET: each magnet holder drags nearby
     * TRAFFIC toward it (ram fuel for the battle). Position lerp toward the
     * holder (same 24.8 world units, bounded fraction), gated by a lane-scaled
     * radius. Deterministic: replicated world positions + config only. */
    {
        int pull_div = knob("TD5RE_ARCADE_MAGNET_PULL_DIV", 24, 4, 256);   /* smaller=stronger */
        int mag_lanes = knob("TD5RE_ARCADE_MAGNET_LANES",   6,  1, 40);
        int64_t mag_r  = (int64_t)s_lane_w * mag_lanes;      /* render units */
        int64_t mag_r2 = mag_r * mag_r;
        for (int s = 0; s < racers; s++) {
            if (s_effect[s] != TD5_PU_MAGNET) continue;
            TD5_Actor *m = slot_actor(s);
            if (!m) continue;
            for (int t = g_traffic_slot_base; t < total_slots(); t++) {
                TD5_Actor *o = slot_actor(t);
                if (!o || td5_ai_actor_is_broken_down(t)) continue;
                int64_t dx = ((int64_t)m->world_pos.x - o->world_pos.x) >> 8;  /* render units */
                int64_t dz = ((int64_t)m->world_pos.z - o->world_pos.z) >> 8;
                int64_t d2 = dx*dx + dz*dz;
                if (d2 == 0 || d2 > mag_r2) continue;
                o->world_pos.x += (int32_t)((m->world_pos.x - o->world_pos.x) / pull_div);
                o->world_pos.z += (int32_t)((m->world_pos.z - o->world_pos.z) / pull_div);
            }
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
        /* [PICKUP DETERMINISM 2026-07-23] In chaos mode several boxes can sit
         * inside the pickup window at once. Previously the loop called
         * apply_pickup for EVERY box in range without breaking, so the effect
         * you ended up with was whichever box came LAST in s_pads[] array order
         * — arbitrary, hence "unpredictable which one I pick up". Instead: scan
         * the whole window, then collect only the SINGLE nearest box (smallest
         * longitudinal distance first, then lateral; deterministic index
         * tie-break). One box per tick, and always the closest one. */
        int best = -1;
        int best_score = 0;
        for (int i = 0; i < s_pad_count; i++) {
            ArcPad *p = &s_pads[i];
            if (!p->active) continue;
            int fdiff = ((aspan - p->span) % s_ring + s_ring) % s_ring;
            if (fdiff > pick_win) continue;             /* not at the box longitudinally */
            int ldiff = alane - (int)p->sub_lane;
            if (ldiff < 0) ldiff = -ldiff;
            if (ldiff > lane_tol) continue;             /* not on the box's side — steer over */
            /* Longitudinal distance dominates; lateral breaks ties within a span. */
            int score = fdiff * (lane_tol + 1) + ldiff;
            if (best < 0 || score < best_score) { best = i; best_score = score; }
        }
        if (best >= 0) {
            ArcPad *p = &s_pads[best];
            apply_pickup(s, p->kind, a);
            p->active = 0;
            /* Respawn the box 5 s after pickup. At the fixed 30 Hz sim tick
             * that's 150 ticks. */
            p->respawn = (int16_t)knob("TD5RE_ARCADE_PAD_RESPAWN", 150, 15, 1800);
            TD5_LOG_I(LOG_TAG, "slot=%d picked nearest pad=%d kind=%d score=%d",
                      s, best, p->kind, best_score);
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
            if (td5_arcade_slot_is_ghost(s)) continue;     /* ghosts pass hazards   */
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
    /* Horizontal knockback as a PERCENT of the faithful impulse, returned in
     * 24.8. [2026-06-26] Was an integer 3x — but the impulse feeds impact_mag,
     * so 3x ALSO tripled the angular crash-scatter + lift and tripped the
     * heavy-crash gate on routine rams ("ramming from behind too strong").
     * Default dropped to 140 (1.4x): a noticeable, "slightly more than sim"
     * shove that no longer over-drives the launch branch. The tumble/launch is
     * tamed separately (td5_arcade_scatter_pct + the gate/div below).
     * Knob TD5RE_ARCADE_COLLISION_MULT_PCT (50..400). */
    int pct = knob("TD5RE_ARCADE_COLLISION_MULT_PCT", 140, 50, 400);
    return (pct * 0x100) / 100;
}

int td5_arcade_scatter_pct(void) {
    /* Percent of the faithful angular crash-scatter (roll/yaw/pitch kicks)
     * applied on a heavy arcade hit. The faithful kicks (pitch up to 0x7FFF)
     * flip a rammed car nose-over so it takes off into the air — the real
     * cause of "launching opponents up". Default 35 keeps a hard hit dramatic
     * (shove + spin) without flipping it airborne. 100 = faithful, 0 = none.
     * Knob TD5RE_ARCADE_SCATTER_PCT (0..100). */
    return knob("TD5RE_ARCADE_SCATTER_PCT", 35, 0, 100);
}

int td5_arcade_slot_is_ghost(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_effect[slot] == TD5_PU_GHOST;
}

int td5_arcade_slot_is_wrecking(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_effect[slot] == TD5_PU_INDESTRUCTIBLE;
}

/* TD5_PU_SHIELD / td5_arcade_slot_is_shielded -- [REMOVED 2026-07-04]. */

int td5_arcade_slot_accel_q8(int slot) {
    /* NITRO acceleration boost as a Q8 drive-torque multiplier (0x100 = 1.0).
     * While NITRO is active for this racer, return TD5RE_ARCADE_NITRO_ACCEL_PCT
     * (default 250 = 2.5x); otherwise 1.0. Consumed at the physics drive-torque
     * chokepoint (td5_physics_compute_drive_torque). Inert (1.0) outside arcade
     * mode or when NITRO is not active.
     *
     * DETERMINISM: reads getenv (process config, identical across lockstep peers)
     * and s_effect[] (replicated sim state), so the multiplier is bit-identical
     * on every client — same guarantee as the MP-catchup / manual / weight
     * multipliers it sits beside in the torque pipeline. */
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0x100;
    /* [FREEZE REWORK 2026-07-04] A freeze VICTIM no longer has drive torque
     * zeroed — the td5_arcade_tick velocity ramp alone produces the "cut to
     * 1/3, ease back over 3s" feel, and zeroing torque here would fight the
     * recovery (the engine needs to be able to push the car back up to speed).
     * ROCKET's accel branch is gone with the power-up (removed 2026-07-04). */
    if (s_effect[slot] != TD5_PU_NITRO) return 0x100;
    int pct = knob("TD5RE_ARCADE_NITRO_ACCEL_PCT", 250, 100, 800);  /* 250 = 2.5x */
    return (pct * 0x100) / 100;
}

int td5_arcade_slot_topspeed_pct(int slot) {
    /* [ARCADE NITRO 2026-07-04] Raise the effective top-speed cap (default
     * +50%) while NITRO is active, so the boosted drive torque can actually be
     * exploited instead of hitting the same faithful cap sooner. Read at the
     * physics top-speed-rating chokepoint (phys_top_speed_rating). */
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 100;
    if (s_effect[slot] != TD5_PU_NITRO) return 100;
    return knob("TD5RE_ARCADE_NITRO_TOPSPEED_PCT", 150, 100, 300);
}

int td5_arcade_slot_is_freeze_victim(int slot) {
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    return s_frozen_frames[slot] > 0;
}

int td5_arcade_launch_active(void) { return s_active; }

int td5_arcade_launch_gate(void) {
    /* Heavy-crash gate: only hits above this trip the dramatic scatter+lift.
     * [2026-06-26] Default raised 20000 -> 90000 (== faithful). The old 20000
     * meant routine rear-ends triggered the full crash reaction; at 90000 only
     * genuine hard crashes do, so ordinary rams are just a (boosted) shove.
     * Lower it toward 1000 for more drama; raise it for less.
     * Knob TD5RE_ARCADE_LAUNCH_GATE (1000..400000). */
    return knob("TD5RE_ARCADE_LAUNCH_GATE", 90000, 1000, 400000);
}

int td5_arcade_launch_div(void) {
    /* Faithful divisor is 6 (vel_y = impact_mag/6); smaller = higher launch.
     * [2026-06-26] Default raised 3 -> 16 in two tuning passes so airborne
     * launches are ~5x lower than the original arcade feel (impact_mag/3 sent
     * cars flying off the map, borderline unplayable). 16 = 2x the first pass
     * (8), i.e. exactly half the launch again. Knob TD5RE_ARCADE_LAUNCH_DIV
     * (1..32) restores any prior feel at runtime. */
    return knob("TD5RE_ARCADE_LAUNCH_DIV", 16, 1, 32);
}

void td5_arcade_note_ram(int aggressor, int victim, int impact_mag) {
    /* [FREEZE REWORK 2026-07-04] If the aggressor currently holds FREEZE, arm
     * the victim's crash-speed-cut: td5_arcade_tick ramps their velocity from
     * ~1/3 back to full over the recovery window starting THIS tick. Works for
     * racers AND traffic ("whatever you crash"), independent of battle mode —
     * this call site fires on every heavy V2V hit, not just battle scoring. A
     * fresh ram while already recovering re-arms the full window (deepest cut
     * wins via straight overwrite; no stacking). */
    if (s_active && aggressor >= 0 && aggressor < ARC_MAX_SLOTS &&
        s_effect[aggressor] == TD5_PU_FREEZE &&
        victim >= 0 && victim < ARC_MAX_SLOTS) {
        TD5_Actor *v = slot_actor(victim);
        if (v && v->finish_time == 0) {
            int frz = knob("TD5RE_ARCADE_FREEZE_RECOVER_FRAMES", 90, 15, 300);
            s_frozen_frames[victim] = (int16_t)frz;
            s_frozen_total[victim]  = (int16_t)frz;
            TD5_LOG_I(LOG_TAG, "slot=%d FREEZE-rammed slot=%d — speed cut, recovering over %d frames",
                      aggressor, victim, frz);
        }
    }

    /* [TRAFFIC BATTLE 2026-06-28] Score traffic destruction. A racer (humans +
     * AI, slot < g_traffic_slot_base) that rams a TRAFFIC car (slot >= base) hard
     * enough to cross the NPC-fatal threshold scores one WRECK. Deduped on the
     * victim's broken-down state so each destroyed traffic car counts exactly
     * once: the collision resolver marks the victim broken-down right after this
     * call, so subsequent overlaps see it already broken and don't re-score; a
     * recycled traffic slot clears the flag and can be scored afresh.
     *
     * Independent of arcade DYNAMICS — battle scoring works in SIMULATION mode
     * too (it gates on td5_game_battle_mode_active(), not on s_active). The whole
     * test is a pure function of replicated state (slot indices, impact_mag,
     * broken-down flag) + the process-wide npc_fatal_mag knob, so wreck counts
     * stay bit-identical across lockstep peers. */
    if (!td5_game_battle_mode_active()) return;
    if (aggressor < 0 || aggressor >= g_traffic_slot_base) return;  /* aggressor = racer */
    if (victim   <  g_traffic_slot_base) return;                    /* victim = traffic  */
    if (impact_mag < td5_physics_npc_fatal_mag()) return;           /* fatal hit only    */
    if (td5_ai_actor_is_broken_down(victim)) return;                /* already scored    */
    td5_game_add_wanted_kill(aggressor);
    TD5_LOG_I(LOG_TAG, "battle: slot=%d WRECKED traffic slot=%d (impact=%d) -> wrecks=%d",
              aggressor, victim, impact_mag, td5_game_get_wanted_kills(aggressor));
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

int td5_arcade_active_max_frames(int slot) {
    /* Full duration of the CURRENTLY-active effect (the value it started with).
     * The HUD bar divides frames-remaining by this so it depicts the WHOLE
     * timer. 0 when nothing is active so the HUD can fall back safely. */
    if (!s_active || slot < 0 || slot >= ARC_MAX_SLOTS) return 0;
    if (s_effect[slot] == TD5_PU_NONE) return 0;
    return s_effect_max[slot];
}
