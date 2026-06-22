/**
 * td5_vfx.c -- Particles, tire tracks, smoke, weather, billboards, taillights
 *
 * Translated from TD5_d3d.exe decompilation. All systems are faithful
 * reproductions of the original logic with clean C11 naming.
 *
 * Systems:
 *   1. Race particle system   -- callback-driven 100-slot pool per view
 *   2. Weather overlay         -- 128 camera-relative rain streaks per view
 *   3. Tire track pool         -- 80-slot emitter pool, mesh strip rendering
 *   4. Vehicle smoke           -- sprite billboard spawning from wheels
 *   5. Taillights              -- translucent billboard quads per vehicle
 *   6. Billboard animations    -- world billboard phase counter advance
 */

#include "td5_vfx.h"
#include "td5_render.h"
#include "td5_asset.h"
#include "td5_camera.h"
#include "td5_game.h"
#include "td5_platform.h"
#include "td5re.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOG_TAG "vfx"

/* ========================================================================
 * External globals from other modules
 * ======================================================================== */

/* Static.hed sprite lookups now go through td5_asset_find_atlas_entry,
 * which reads the real populated s_atlas_table[] built in td5_asset_init. */

/* Sub-tick interpolation fraction -- set by timing system */
extern float  g_subTickFraction;   /* 0x4AAF60 -- [0..1) for sub-tick interp */

/* World scale factor -- set by render init */
extern float  g_worldToRenderScale; /* 0x4749D0 -- 1/256 world->float scale */

/* Track environment config -- loaded from LEVELINF.DAT */
extern uint8_t *g_track_environment_config; /* 0x4AEE20 -- pointer to LEVELINF.DAT buffer */

/* Actor table base -- runtime slot array set by td5_game/td5_physics */
extern uint8_t *g_actor_table_base; /* 0x4AB108 -- gRuntimeSlotActorTable */

/* ========================================================================
 * Sprite quad template (0xB8 = 184 bytes)
 *
 * Matches the original BuildSpriteQuadTemplate layout. Contains 4 vertices
 * with screen-space positions, UVs, color, and depth. Submitted to the
 * translucent pipeline via QueueTranslucentPrimitiveBatch.
 * ======================================================================== */

typedef struct VfxSpriteQuad {
    int      geometry_ptr;          /* +0x00: pointer to vertex data (self+0x08) */
    int      vertex_count;          /* +0x04: always 4 */
    float    v0_x, v0_y, v0_z, v0_rhw;   /* +0x08 */
    uint32_t v0_color;              /* +0x18 */
    float    v0_u, v0_v;            /* +0x1C */
    float    v1_x, v1_y, v1_z, v1_rhw;   /* +0x24 */
    uint32_t v1_color;              /* +0x34 */
    float    v1_u, v1_v;            /* +0x38 */
    float    v2_x, v2_y, v2_z, v2_rhw;   /* +0x40 */
    uint32_t v2_color;              /* +0x50 */
    float    v2_u, v2_v;            /* +0x54 */
    float    v3_x, v3_y, v3_z, v3_rhw;   /* +0x5C */
    uint32_t v3_color;              /* +0x6C */
    float    v3_u, v3_v;            /* +0x70 */
    /* Remaining bytes for texture/state info */
    float    tex_u0, tex_v0;        /* +0x78 */
    float    tex_u1, tex_v1;        /* +0x80 */
    float    quad_width;            /* +0x88 */
    float    quad_height;           /* +0x8C */
    float    texture_page;          /* +0x90 */
    uint8_t  padding[0xB8 - 0x94];  /* pad to stride */
} VfxSpriteQuad;

/* ========================================================================
 * Race particle slot (0x40 = 64 bytes per slot)
 *
 * Each slot has a flag byte, world position (24.8 fixed), view-space
 * position (float), and a callback function pointer for per-frame update.
 * ======================================================================== */

typedef struct VfxParticleSlot {
    uint8_t  type_byte;             /* +0x00: particle type identifier */
    uint8_t  flags;                 /* +0x01: bit7=active, bit6=projected, bit5=blend */
    uint8_t  pad_02[0x1B];         /* +0x02 */
    /* +0x1D = callback function pointer (relative to slot) */
    int32_t  world_x;              /* +0x01 (from flags+1): 24.8 fixed */
    int32_t  world_y;
    int32_t  world_z;
    float    view_x;               /* +0x0D: view-space after projection */
    float    view_y;
    float    view_z;
    uint8_t  rest[0x40 - 0x19];    /* pad to stride */
} VfxParticleSlot;

/* --- Particle slot byte offsets (within 0x40-byte bank entry) ---
 *
 * Clean layout to avoid the old overlapping-field bugs (position used to
 * overwrite type byte, batch index lived inside position bytes).
 *
 * Matches the original SmokeUpdateCallback (0x00429950) field semantics
 * for the animation data starting at PSLOT_LIFETIME (0x04).
 */
#define PSLOT_FLAGS       0x00  /* uint8: 0x80=active, 0x40=projected, 0x20=blend */
#define PSLOT_TYPE        0x01  /* uint8: 0=smoke, 1=rain */
#define PSLOT_BATCH       0x02  /* uint8: sprite batch index */
#define PSLOT_PHASE       0x03  /* uint8: animation phase (mod 32) */
#define PSLOT_LIFETIME    0x04  /* int16: ticks remaining */
#define PSLOT_SIZE_W      0x06  /* int16: width  (0x4000 init, original +0x04) */
#define PSLOT_SIZE_W_D    0x08  /* int16: width delta per tick */
#define PSLOT_SIZE_H      0x0A  /* int16: height (0x26C0 init, original +0x08) */
#define PSLOT_SIZE_H_D    0x0C  /* int16: height delta per tick */
#define PSLOT_VEL_X       0x0E  /* int32: velocity X (24.8 fixed) */
#define PSLOT_VEL_Y       0x12  /* int32: velocity Y (24.8 fixed) */
#define PSLOT_VEL_Z       0x16  /* int32: velocity Z (24.8 fixed) */
#define PSLOT_POS_X       0x20  /* int32: world X (24.8 fixed) */
#define PSLOT_POS_Y       0x24  /* int32: world Y (24.8 fixed) */
#define PSLOT_POS_Z       0x28  /* int32: world Z (24.8 fixed) */
#define PSLOT_VIEW_X      0x2C  /* float: view-space X (after projection) */
#define PSLOT_VIEW_Y      0x30  /* float: view-space Y */
#define PSLOT_VIEW_Z      0x34  /* float: view-space Z */
/* [2026-06-08 procedural FX] Spawn lifetime, stored per-puff so the procedural
 * smoke shader can compute a correct age01 (= elapsed/life0). Different spawn
 * paths use very different lifetimes (drift/rear-wheel 10..40 ticks vs
 * rev/exhaust 0x200=512 ticks); the old fixed /512 normalisation mis-aged the
 * short drift puffs to ~0.95 at birth → they rendered tiny + faint. Spare slot
 * byte (free gap between VEL_Z and POS_X). */
#define PSLOT_LIFE0       0x1A  /* int16: lifetime at spawn */

/* Helper to read/write typed values from raw slot bytes */
#define PSLOT_RD16(s,off)      (*(int16_t  *)((s)+(off)))
#define PSLOT_WR16(s,off,v)    (*(int16_t  *)((s)+(off)) = (int16_t)(v))
#define PSLOT_RD32(s,off)      (*(int32_t  *)((s)+(off)))
#define PSLOT_WR32(s,off,v)    (*(int32_t  *)((s)+(off)) = (int32_t)(v))
#define PSLOT_RDF(s,off)       (*(float    *)((s)+(off)))
#define PSLOT_WRF(s,off,v)     (*(float    *)((s)+(off)) = (float)(v))

/* ========================================================================
 * Tire track emitter slot (0xEC = 236 bytes)
 * ======================================================================== */

typedef struct VfxTireTrackSlot {
    /* +0x00: trailing-edge anchor (previous frame's anchor_x/y/z).
     * int32 24.8 FP — avoids the int16 overflow that plagued the original
     * vertex[] arrays (Moscow world X ≈ 42 786 m overflows int16 = 32 767). */
    int32_t  trail_x;               /* +0x00 */
    int32_t  trail_y;               /* +0x04 */
    int32_t  trail_z;               /* +0x08 */
    /* +0x0C: perpendicular half-width vector in 24.8 FP.
     * Computed from (dx,dz) × width / len in UpdateTireTrackEmitters. */
    int32_t  perp_x;                /* +0x0C: LEAD-edge half-width vector */
    int32_t  perp_z;                /* +0x10 */
    int32_t  perp0_x;               /* +0x14: TRAIL-edge half-width = prior segment's
                                     *        lead perp, so joins share an edge → smooth
                                     *        curve instead of dented kinks. */
    int32_t  perp0_z;               /* +0x18 */
    int32_t  _reserved[5];         /* +0x1C: was prev_verts; keep for size compat */
    /* --- Sprite quad blob (was at +0x30; size corrected from 0x98 → 0xA8) --- */
    uint8_t  quad_data[0xA8];       /* +0x30 */
    /* --- Control block at +0xD8 --- */
    uint8_t  control;               /* +0xD8: bit0=allocated, bit1=has_geometry */
    uint8_t  intensity;             /* +0xD9: current mark intensity (0-255) */
    uint16_t lifetime;              /* +0xDA: tick counter since creation */
    int32_t  anchor_x;              /* +0xDC: world X (24.8 fixed) */
    int32_t  anchor_y;              /* +0xE0: world Y */
    int32_t  anchor_z;              /* +0xE4: world Z */
    uint32_t direction_hash;        /* +0xE8: heading angle hash */
} VfxTireTrackSlot;

/* ========================================================================
 * Tire track emitter descriptor (7 bytes per wheel)
 * ======================================================================== */

typedef struct VfxEmitterDesc {
    uint8_t  actor_slot;            /* +0: source vehicle actor index */
    uint8_t  wheel_id;             /* +1: wheel hardpoint ID (0-3) */
    uint8_t  active;               /* +2: 1=emitting, 0=dead */
    uint8_t  pool_slot;            /* +3: index into 80-slot pool */
    uint8_t  initial_alpha;        /* +4: starting opacity */
    uint8_t  target_alpha;         /* +5: current target intensity */
    uint8_t  width;                /* +6: track mark width */
} VfxEmitterDesc;

/* ========================================================================
 * Weather particle slot (0xC8 = 200 bytes)
 * ======================================================================== */

typedef struct VfxWeatherParticle {
    float    pos_x;                /* +0x00: camera-relative X */
    float    pos_y;                /* +0x04: camera-relative Y */
    float    pos_z;                /* +0x08: camera-relative Z (depth) */
    float    visible;              /* +0x0C: 0=recycle, nonzero=active */
    VfxSpriteQuad quad;            /* +0x10: sprite quad template */
} VfxWeatherParticle;

/* ========================================================================
 * Forward declarations for internal helpers
 * ======================================================================== */

static void vfx_update_rear_tire_effects(TD5_Actor *actor, uint8_t contact_flags);
static void vfx_update_front_tire_effects(TD5_Actor *actor, uint8_t contact_flags);
static void vfx_update_front_wheel_sound_effects(TD5_Actor *actor, int speed);
static void vfx_update_rear_wheel_sound_effects(TD5_Actor *actor, int speed);
static int  vfx_alloc_slot_index(void);
static void vfx_read_wheel_world_pos(TD5_Actor *actor, int wheel_index,
                                     int32_t *out_x, int32_t *out_y, int32_t *out_z);
static int  vfx_acquire_tire_track_emitter(int wheel_id, int actor_slot,
                                            int wheel_index, uint8_t alpha,
                                            uint8_t width);
static bool vfx_is_hard_surface(uint8_t surface_type);
static void vfx_build_sprite_quad(VfxSpriteQuad *quad,
                                   float cx, float cy, float depth,
                                   float half_w, float half_h,
                                   float u0, float v0, float u1, float v1,
                                   float tex_page, uint32_t color);
static void vfx_spawn_smoke_at_position(TD5_Actor *actor, float wx, float wy,
                                         float wz, int variant, int view_index);

/* Upward velocity for smoke particles. Hardpoint/tire smoke = 0x600 (default).
 * Exhaust smoke (0x429CF0) = 0x2000. Set before spawn, auto-resets after. */
static int32_t s_smoke_vel_y = 0x600;

/* [task#14] TD6 prop-break debris: sim->render hand-off. Physics (sim-time)
 * queues a break burst at a world point via td5_vfx_queue_prop_break(); the
 * per-view particle update (render-time) drains it into each view's bank
 * (frustum-gated), so a knocked-over lamppost/bin throws a short dust burst.
 * served[] (per 2-way particle bank) prevents double-spawn; the event ages out
 * after a few drain calls so both split-screen banks get served exactly once. */
#define VFX_MAX_BREAK_EVENTS 16
typedef struct {
    float   wx, wy, wz;     /* world coords (float units, matches spawn API)   */
    int     strength;       /* impact magnitude (inward approach speed, 24.8)  */
    uint8_t alive, age;
    uint8_t served[2];      /* particle banks are 2-way (view_index & 1)       */
} VfxBreakEvent;
static VfxBreakEvent s_break_events[VFX_MAX_BREAK_EVENTS];

static void vfx_spawn_smoke_at_position(TD5_Actor *actor, float wx, float wy,
                                        float wz, int variant, int view_index);

/* [task#14] Public sim-time entry: queue a debris burst at a world point.
 * strength is the inward approach speed (24.8); bigger = more puffs. */
void td5_vfx_queue_prop_break(float wx, float wy, float wz, int strength)
{
    int i;
    for (i = 0; i < VFX_MAX_BREAK_EVENTS; i++) {
        if (!s_break_events[i].alive) {
            s_break_events[i].wx = wx;
            s_break_events[i].wy = wy;
            s_break_events[i].wz = wz;
            s_break_events[i].strength = strength;
            s_break_events[i].alive = 1;
            s_break_events[i].age = 0;
            s_break_events[i].served[0] = 0;
            s_break_events[i].served[1] = 0;
            return;
        }
    }
    /* queue full (>16 breaks between frames): drop — visual-only, harmless */
}

/* Drain pending breaks into the given particle bank (vi = view_index & 1).
 * Called once per view per frame from td5_vfx_update_particles. */
static void vfx_emit_prop_breaks(int vi)
{
    int i, p;
    if (vi < 0 || vi > 1) return;
    for (i = 0; i < VFX_MAX_BREAK_EVENTS; i++) {
        VfxBreakEvent *e = &s_break_events[i];
        if (!e->alive) continue;
        if (!e->served[vi]) {
            int puffs = (e->strength > 0x8000) ? 6 : 4;
            e->served[vi] = 1;
            for (p = 0; p < puffs; p++) {
                float jx = (float)((rand() % 240) - 120);
                float jz = (float)((rand() % 240) - 120);
                s_smoke_vel_y = 0x1400 + (rand() % 0x0C00);   /* upward burst */
                vfx_spawn_smoke_at_position(NULL, e->wx + jx, e->wy + 32.0f,
                                            e->wz + jz, p & 3, vi);
            }
        }
        if (++e->age >= 4) { e->alive = 0; }   /* serve up to 2 banks, then expire */
    }
}

/* ========================================================================
 * Static state -- all VFX subsystem globals
 * ======================================================================== */

/* --- Race particle system --- */
static uint8_t  s_particle_banks[TD5_MAX_VIEWPORTS][TD5_VFX_PARTICLE_BANK_SIZE]; /* per-view (PORT: N-way) */
static uint8_t  s_sprite_render_flags[TD5_MAX_VIEWPORTS][TD5_VFX_SPRITE_BATCH_COUNT];
static VfxSpriteQuad s_sprite_batches[TD5_MAX_VIEWPORTS * TD5_VFX_SPRITE_BATCH_COUNT];
static int      s_current_view_index;
static unsigned int s_vfx_debug_frame;

/* Sprite UV data from archive entries */
static float    s_rainspl_u0, s_rainspl_v0, s_rainspl_u1, s_rainspl_v1;
static float    s_rainspl_page;
static float    s_smoke_u0, s_smoke_v0, s_smoke_u1, s_smoke_v1;
static float    s_smoke_page;

/* Smoke sprite variant UV table -- orig DrawCallback @ 0x004297D0 reads
 * DAT_004aabb8 with stride 0x14 (5 floats) indexed by (phase >> 2) where
 * phase cycles 0..0x1F mod 32. The orig init loop at 0x00429630 writes 8
 * entries with col = i & 1, row = (i - sign)/2 (=> i/2 for unsigned), step
 * 32 texels (DAT_0045d5dc), cell w/h = 30.0 (0x41f00000). The smoke puff
 * therefore ANIMATES through 8 sub-cells, advancing one cell every 4 ticks. */
static float    s_smoke_variant_uv[8][5]; /* u0, v0, width, height, page */

/* --- Weather overlay --- */
static VfxWeatherParticle *s_weather_buf[TD5_MAX_VIEWPORTS];   /* per-view particle buffers */
static int      s_weather_type;                /* 0=rain, 1=snow(cut), 2=clear */
static int      s_weather_target_density[TD5_MAX_VIEWPORTS];   /* target particle count per view */
static int      s_weather_active_count[TD5_MAX_VIEWPORTS];     /* current active count per view */
static float    s_weather_prev_cam[TD5_MAX_VIEWPORTS][3];      /* previous camera position per view */
static float    s_weather_prev_budget[TD5_MAX_VIEWPORTS];      /* previous sim_budget per view */
static float    s_weather_sprite_page;         /* texture page for weather sprite */

/* Weather sprite UV (rain/snow) */
static float    s_weather_u0, s_weather_v0;
static float    s_weather_u1, s_weather_v1;

/* --- Tire track pool --- */
static VfxTireTrackSlot *s_tire_track_pool;    /* 80-slot pool */
static VfxEmitterDesc    s_emitter_descs[TD5_MAX_TOTAL_ACTORS * 4]; /* per-wheel */
static int               s_emitter_desc_count;
static int               s_tire_track_cursor;  /* roving allocation cursor */

/* Tire mark UV coords -- loaded from SEMICOL sprite (confirmed @ 0x4743F4/0x43E997) */
static float    s_tiremark_u0, s_tiremark_v0;
static float    s_tiremark_u1, s_tiremark_v1;
static float    s_tiremark_page;

/* ------------------------------------------------------------------------
 * Tire mark ring buffer (float world-space quads).
 *
 * Parallel to the faithful-but-broken int16 strip builder above: whenever a
 * wheel slips hard enough to spawn smoke, we also push one small textured
 * quad centred on the wheel with a perpendicular half-axis. The ring ages
 * entries per tick, fades alpha, and evicts after TM_LIFETIME frames. Render
 * projects each live quad to screen space using the same path as HUD
 * translucent quads.
 *
 * Float storage avoids the int16 vertex overflow that was eating the strip
 * builder on large tracks (~300k world units on e.g. Moscow).
 * ---------------------------------------------------------------------- */
/* Float-ring tire marks REMOVED 2026-05-28 — was a port-only addition
 * that paralleled the int16 strip builder (orig 0x0043F210). Multi-agent
 * audit found it broken (Y=0 corners, world-Y mixed with camera-relative Y)
 * and redundant with the faithful-port strip pool. Now using strip builder
 * exclusively, routed through td5_render_load_rotation/translation pipeline
 * to match orig render math. */

/* --- Smoke sprite pool (used by LEVELINF smoke flag) --- */
static VfxSpriteQuad    *s_smoke_pool;         /* per-actor sprite quads */
static float             s_smoke_page_id;

/* --- Taillight system --- */
static VfxSpriteQuad    *s_taillight_quads;    /* 2 quads per actor * num_actors */
static VfxSpriteQuad    *s_taillight_quads_alt; /* alternate set for smoke overlay */
static uint8_t           s_taillight_brightness[TD5_MAX_TOTAL_ACTORS];

/* Taillight UV coords (extracted from BRAKED sprite) */
static float    s_taillight_u0, s_taillight_v0;
static float    s_taillight_u1, s_taillight_v1;
static float    s_taillight_page;

/* Taillight billboard CORNER offsets (from 0x463030-0x463048): the 4 ±80
 * camera-facing quad corners, NOT light positions [CONFIRMED agent 2026-06-05].
 * Original: int16[4] per entry (4th = 0 padding). We store only xyz.
 * NOTE: this table is historical/unused — the active taillight path is
 * render_vehicle_brake_lights() in td5_render.c, which builds the quad from a
 * screen-space half-size centred on the cardef taillight hardpoint (or, for
 * ported TD6 cars as of S23, the authored TD6 :CAR_LIGHTS: position installed by
 * the asset loader). Kept for reference; do not chase it for taillight placement. */
static const int16_t s_taillight_offsets[4][3] = {
    {  80, -80, 0 },   /* 0x463030: top-left     */
    { -80, -80, 0 },   /* 0x463038: bottom-left  */
    {  80,  80, 0 },   /* 0x463040: top-right    */
    { -80,  80, 0 },   /* 0x463048: bottom-right */
};

static const char *vfx_weather_type_name(TD5_WeatherType type)
{
    switch (type) {
    case TD5_WEATHER_RAIN: return "rain";
    case TD5_WEATHER_SNOW: return "snow";
    default: return "none";
    }
}

/* --- Constants matching original binary --- */
static const float FP_TO_FLOAT = 1.0f / 256.0f; /* DAT_004749d0 */
static const float HALF_PIXEL  = 0.5f;           /* DAT_0045d5d0 */
static const float PERSPECTIVE_SCALE = 180.0f;    /* DAT_00474e64 */
static const float WEATHER_WIND_Y   = -250.0f;    /* DAT_00474e5c */
static const float WEATHER_BOUNDS_X = 4000.0f;    /* DAT_00474e68 */
static const float WEATHER_BOUNDS_Y = 3000.0f;    /* DAT_00474e6c */
static const float WEATHER_BOUNDS_Z = 1947.5f;    /* DAT_00474e70 */
static const float WEATHER_DEPTH_OFFSET = 2147.0f; /* DAT_0045d7a8 */
static const float WEATHER_Y_OFFSET = 1000.0f;     /* DAT_0045d7a4 */
static const float TRACK_Y_OFFSET   = -20.0f;      /* DAT_0045d6ac */
static const float TAILLIGHT_Z_BIAS = -24.0f;      /* DAT_0045d5d4 */

/* ========================================================================
 * Helper: extract UV coords from a TD5_AtlasEntry (static.hed sprite)
 *
 * Reads the populated atlas table built by td5_asset_init_static_atlas:
 * atlas_x/atlas_y/width/height/texture_page. The page is already biased by
 * STATIC_ATLAS_BASE at load time, so we write it straight through.
 * UVs are computed with a half-pixel inset for texel-center sampling.
 * ======================================================================== */

static int vfx_atlas_entry_valid(const TD5_AtlasEntry *entry) {
    return entry && (entry->width > 0 || entry->height > 0);
}

static void vfx_extract_sprite_uvs(const TD5_AtlasEntry *entry,
                                    float *u0, float *v0,
                                    float *u1, float *v1,
                                    float *page)
{
    *u0   = (float)entry->atlas_x + HALF_PIXEL;
    *v0   = (float)entry->atlas_y + HALF_PIXEL;
    *u1   = (float)(entry->atlas_x + entry->width) - HALF_PIXEL;
    *v1   = (float)(entry->atlas_y + entry->height) - HALF_PIXEL;
    *page = (float)entry->texture_page;
}

/* ========================================================================
 * Helper: build a screen-space sprite quad with 4 corners
 *
 * center +/- half_size, with given UV rect, color, and depth.
 * ======================================================================== */

static void vfx_build_sprite_quad(VfxSpriteQuad *quad,
                                   float cx, float cy, float depth,
                                   float half_w, float half_h,
                                   float u0, float v0, float u1, float v1,
                                   float tex_page, uint32_t color)
{
    float rhw = (depth > 0.0f) ? (1.0f / depth) : 1.0f;

    quad->geometry_ptr = 0; /* will be set to self+8 by render pipeline */
    quad->vertex_count = 4;

    /* v0 = top-left */
    quad->v0_x = cx - half_w;  quad->v0_y = cy - half_h;
    quad->v0_z = depth;        quad->v0_rhw = rhw;
    quad->v0_color = color;    quad->v0_u = u0;  quad->v0_v = v0;

    /* v1 = top-right */
    quad->v1_x = cx + half_w;  quad->v1_y = cy - half_h;
    quad->v1_z = depth;        quad->v1_rhw = rhw;
    quad->v1_color = color;    quad->v1_u = u1;  quad->v1_v = v0;

    /* v2 = bottom-right */
    quad->v2_x = cx + half_w;  quad->v2_y = cy + half_h;
    quad->v2_z = depth;        quad->v2_rhw = rhw;
    quad->v2_color = color;    quad->v2_u = u1;  quad->v2_v = v1;

    /* v3 = bottom-left */
    quad->v3_x = cx - half_w;  quad->v3_y = cy + half_h;
    quad->v3_z = depth;        quad->v3_rhw = rhw;
    quad->v3_color = color;    quad->v3_u = u0;  quad->v3_v = v1;

    /* Store atlas UV extents and texture page */
    quad->tex_u0 = u0;    quad->tex_v0 = v0;
    quad->tex_u1 = u1;    quad->tex_v1 = v1;
    quad->quad_width  = half_w * 2.0f;
    quad->quad_height = half_h * 2.0f;
    quad->texture_page = tex_page;
}

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int td5_vfx_init(void) {
    /* Allocate tire track pool */
    s_tire_track_pool = (VfxTireTrackSlot *)td5_plat_heap_alloc(
        TD5_VFX_TIRE_TRACK_POOL_SIZE * sizeof(VfxTireTrackSlot));
    if (!s_tire_track_pool) return 0;
    memset(s_tire_track_pool, 0,
           TD5_VFX_TIRE_TRACK_POOL_SIZE * sizeof(VfxTireTrackSlot));

    s_tire_track_cursor = 0;
    s_emitter_desc_count = 0;
    memset(s_emitter_descs, 0, sizeof(s_emitter_descs));

    /* Clear particle banks */
    memset(s_particle_banks, 0, sizeof(s_particle_banks));
    memset(s_sprite_render_flags, 0, sizeof(s_sprite_render_flags));

    /* Clear weather state */
    s_weather_buf[0] = NULL;
    s_weather_buf[1] = NULL;
    s_weather_type = TD5_WEATHER_CLEAR;
    memset(s_weather_target_density, 0, sizeof(s_weather_target_density));
    memset(s_weather_active_count, 0, sizeof(s_weather_active_count));
    memset(s_weather_prev_cam, 0, sizeof(s_weather_prev_cam));
    memset(s_weather_prev_budget, 0, sizeof(s_weather_prev_budget));

    /* Clear taillight state */
    s_taillight_quads = NULL;
    s_taillight_quads_alt = NULL;
    memset(s_taillight_brightness, 0, sizeof(s_taillight_brightness));

    /* Clear smoke pool */
    s_smoke_pool = NULL;

    s_current_view_index = 0;
    s_vfx_debug_frame = 0;

    /* Wire up the race particle system: look up RAINSPL/SMOKE atlas UVs,
     * clear banks, build the 2x2 smoke variant table. Without these calls
     * every smoke spawn writes a degenerate all-zero UV quad and the
     * sprite is invisible. Originals: InitializeRaceParticleSystem @ 0x429510,
     * InitializeRaceSmokeSpritePool @ 0x401410,
     * InitializeVehicleTaillightQuadTemplates @ 0x401000. */
    td5_vfx_init_race_particles();
    td5_vfx_init_smoke_sprite_pool();
    td5_vfx_init_taillight_templates();

    TD5_LOG_I(LOG_TAG, "VFX system initialized (race particles, smoke pool, taillights)");
    return 1;
}

void td5_vfx_shutdown(void) {
    if (s_tire_track_pool) {
        td5_plat_heap_free(s_tire_track_pool);
        s_tire_track_pool = NULL;
    }
    for (int i = 0; i < 2; i++) {
        if (s_weather_buf[i]) {
            td5_plat_heap_free(s_weather_buf[i]);
            s_weather_buf[i] = NULL;
        }
    }
    if (s_taillight_quads) {
        td5_plat_heap_free(s_taillight_quads);
        s_taillight_quads = NULL;
    }
    /* s_taillight_quads_alt points into s_taillight_quads allocation; don't double-free */
    s_taillight_quads_alt = NULL;
    if (s_smoke_pool) {
        td5_plat_heap_free(s_smoke_pool);
        s_smoke_pool = NULL;
    }
    TD5_LOG_I("vfx", "VFX system shutdown");
}

void td5_vfx_tick(void) {
    s_vfx_debug_frame++;
    td5_vfx_update_tire_tracks();
    /* Billboard (cop-marker) phase advance is driven once per rendered
     * viewport by td5_vfx_advance_tracked_marker_phases() — the single live
     * port of AdvanceWorldBillboardAnimations 0x43CDC0. */

    /* UpdateRaceParticleEffects @ 0x429790 runs once per view per sim tick;
     * decrements slot lifetimes and drives update callbacks. Without this
     * call the 100-slot bank fills up with stale particles and nothing
     * ever expires. */
    int view_count = g_td5.viewport_count > 0 ? g_td5.viewport_count : 1;
    if (view_count > 2) view_count = 2;
    for (int vp = 0; vp < view_count; vp++) {
        td5_vfx_update_particles(vp);
    }
}

/* ========================================================================
 * 1. Race Particle System
 *    Original: InitializeRaceParticleSystem (0x429510)
 *              ProjectRaceParticlesToView (0x429690)
 *              DrawRaceParticleEffects (0x429720)
 *              UpdateRaceParticleEffects (0x429790)
 * ======================================================================== */

/**
 * InitializeRaceParticleSystem (0x429510)
 *
 * Loads RAINSPL and SMOKE sprite entries from the archive, extracts UV
 * coordinates, and clears all particle slots and sprite render flags.
 * Also builds the 4-variant smoke UV table (2x2 grid in atlas).
 */
void td5_vfx_init_race_particles(void) {
    /* Look up RAINSPL sprite entry in the populated static atlas.
     * s_atlas_table[] is the real data loaded by td5_asset_init_static_atlas;
     * find_atlas_entry always returns non-NULL so guard by width/height. */
    TD5_AtlasEntry *rainspl = td5_asset_find_atlas_entry(NULL, "RAINSPL");
    if (vfx_atlas_entry_valid(rainspl)) {
        vfx_extract_sprite_uvs(rainspl,
                                &s_rainspl_u0, &s_rainspl_v0,
                                &s_rainspl_u1, &s_rainspl_v1,
                                &s_rainspl_page);
    } else {
        s_rainspl_u0 = 1.0f;   s_rainspl_v0 = 1.0f;
        s_rainspl_u1 = 62.0f;  s_rainspl_v1 = 126.0f;
        s_rainspl_page = 0.0f;
        TD5_LOG_W("vfx", "RAINSPL sprite not found in static atlas");
    }

    TD5_AtlasEntry *smoke = td5_asset_find_atlas_entry(NULL, "SMOKE");
    if (vfx_atlas_entry_valid(smoke)) {
        vfx_extract_sprite_uvs(smoke,
                                &s_smoke_u0, &s_smoke_v0,
                                &s_smoke_u1, &s_smoke_v1,
                                &s_smoke_page);
    } else {
        s_smoke_u0 = 1.0f;   s_smoke_v0 = 1.0f;
        s_smoke_u1 = 30.0f;  s_smoke_v1 = 30.0f;
        s_smoke_page = 0.0f;
        TD5_LOG_W("vfx", "SMOKE sprite not found in static atlas");
    }

    /* Clear all particle slots across both views */
    memset(s_particle_banks, 0, sizeof(s_particle_banks));

    /* Clear sprite render flags */
    for (int view = 0; view < 2; view++) {
        for (int i = 0; i < TD5_VFX_SPRITE_BATCH_COUNT; i++) {
            s_sprite_render_flags[view][i] = 0;
        }
    }

    /* Build 8-variant smoke UV table (2 cols × 4 rows) matching orig
     * FUN_00429510 @ 0x00429620 — cell stride 32px (DAT_0045d5dc = 32.0f),
     * sampled box 30×30. Render callback indexes the table as
     * (phase >> 2), which spans 0..7 since phase ∈ [0, 0x1F]. */
    const float CELL_STRIDE = 32.0f;
    const float CELL_SIZE   = 30.0f;
    for (int i = 0; i < 8; i++) {
        int col = i & 1;
        int row = i >> 1;
        s_smoke_variant_uv[i][0] = (float)col * CELL_STRIDE + s_smoke_u0;
        s_smoke_variant_uv[i][1] = (float)row * CELL_STRIDE + s_smoke_v0;
        s_smoke_variant_uv[i][2] = CELL_SIZE;
        s_smoke_variant_uv[i][3] = CELL_SIZE;
        s_smoke_variant_uv[i][4] = s_smoke_page;
    }

    /* Tire mark texture choice:
     *
     * The orig binary PUSHes "SEMICOL" at 0x43E997, but SEMICOL is the
     * HUD timer-separator-colon glyph (re/analysis/global_naming/
     * batch_08_render_hud_wheels.md confirms `s_SEMICOL` is the ":"
     * sprite for `%02d:%02d.%02d`). Stretching a two-dot glyph across
     * the road quad produces 2 thin invisible dots — nothing like a
     * tire mark. The 2026-04-13 texture audit's claim that "SKIDMARK"
     * is in the atlas is incorrect (strings on static.hed shows no
     * SKIDMARK entry, only FADEWHT/SEMICOL/SMOKE/RAINDROP/BRAKED).
     *
     * BUGFIX 2026-05-28: use FADEWHT (a solid white block in tpage4) as
     * the actual visible sprite. With MODULATEALPHA and a dark-gray
     * vertex modulator, the white texel becomes a continuous dark
     * rectangle on the road — which is what a real tire mark looks
     * like. This is a visible-result deviation from orig byte-level
     * behavior; the orig literally does smear a colon glyph because
     * 1999 art shipped that way, but the user wants visible marks. */
    TD5_AtlasEntry *fadewht = td5_asset_find_atlas_entry(NULL, "FADEWHT");
    if (vfx_atlas_entry_valid(fadewht)) {
        vfx_extract_sprite_uvs(fadewht,
                                &s_tiremark_u0, &s_tiremark_v0,
                                &s_tiremark_u1, &s_tiremark_v1,
                                &s_tiremark_page);
        TD5_LOG_I(LOG_TAG, "tire tracks: using FADEWHT (solid white) "
                  "uv=(%.1f,%.1f..%.1f,%.1f) page=%.0f",
                  s_tiremark_u0, s_tiremark_v0,
                  s_tiremark_u1, s_tiremark_v1, s_tiremark_page);
    } else {
        /* No FADEWHT — fall back to a hard-coded white texel position */
        s_tiremark_u0 = 0.5f;  s_tiremark_v0 = 0.5f;
        s_tiremark_u1 = 1.5f;  s_tiremark_v1 = 1.5f;
        s_tiremark_page = (float)(700 + 4);
        TD5_LOG_W(LOG_TAG, "tire tracks: FADEWHT not in atlas, using fallback texel");
    }

    TD5_LOG_I(LOG_TAG,
              "race particles init: particle_bank_bytes=%zu sprite_batches=%d sprite_flags=%d",
              sizeof(s_particle_banks), 2 * TD5_VFX_SPRITE_BATCH_COUNT,
              2 * TD5_VFX_SPRITE_BATCH_COUNT);
}

/**
 * InitializeRaceSmokeSpritePool (0x401410)
 *
 * Allocates the per-vehicle smoke sprite overlay pool. Only active when
 * LEVELINF.DAT +0x04 == 1 (smoke-enabled track).
 *
 * [CONFIRMED @ 0x00401410 InitializeRaceSmokeSpritePool; L5 sweep 2026-05-21]
 *   Decompile match:
 *     - Check `*(int *)(g_trackEnvironmentConfig + 4) == 1` → port has identical
 *     - FindArchiveEntryByName("SMOKE") → port td5_asset_find_atlas_entry(NULL,"SMOKE")
 *     - HeapAllocTracked(g_racerCount * 0x170) → port num_actors * 0x170
 *   ARCH-DIVERGENCE class: heap allocator wrapper (HeapAllocTracked vs
 *   td5_plat_heap_alloc) and the additional memset (port-side defensive
 *   zero-init; orig left allocation uninitialized). Pool size formula and
 *   gating condition byte-faithful.
 */
void td5_vfx_init_smoke_sprite_pool(void) {
    /* Check g_track_environment_config->smoke_flag at offset +0x04 */
    if (g_track_environment_config) {
        int32_t smoke_flag;
        memcpy(&smoke_flag, g_track_environment_config + 4, sizeof(int32_t));
        if (smoke_flag != 1) return;
    }

    /* Allocate per-actor smoke overlay quads: num_actors * 0x170 bytes */
    int num_actors = g_td5.total_actor_count;
    if (num_actors <= 0) num_actors = TD5_MAX_RACER_SLOTS;

    if (s_smoke_pool) {
        td5_plat_heap_free(s_smoke_pool);
    }
    size_t pool_size = (size_t)num_actors * 0x170;
    s_smoke_pool = (VfxSpriteQuad *)td5_plat_heap_alloc(pool_size);
    if (s_smoke_pool) {
        memset(s_smoke_pool, 0, pool_size);
    }

    /* Load "SMOKE" texture entry from static atlas for smoke overlay rendering */
    TD5_AtlasEntry *smoke_entry = td5_asset_find_atlas_entry(NULL, "SMOKE");
    if (vfx_atlas_entry_valid(smoke_entry)) {
        s_smoke_page_id = (float)smoke_entry->texture_page;
    }
}

/**
 * ProjectRaceParticlesToView (0x429690)
 *
 * For each active particle slot in the given view, converts the 24.8
 * fixed-point world position to float, subtracts the camera translation,
 * and transforms into view space via the current render rotation matrix.
 * Sets the "projected" flag (bit 6) on successful projection.
 *
 * [ARCH-DIVERGENCE: D3D11 camera basis indirection; L5 sweep 2026-05-21]
 *   Math sequence (world→camera→basis multiply→write view-space + flag 0x40)
 *   matches orig. Port reads camera position via td5_camera_get_position()
 *   API instead of direct globals, and uses the always-updated g_cameraBasis
 *   instead of the stale g_renderBasisMatrix that the original D3 path wrote
 *   per-frame. Numerically equivalent — addressing only.
 */
void td5_vfx_project_particles(int view_index) {
    /* g_cameraBasis is the real per-frame camera rotation matrix written
     * by the camera module (td5_camera.c). The older g_renderBasisMatrix
     * declared in td5_render.c is a stale identity and never updated.
     * Row layout: m[0..2]=right, m[3..5]=up, m[6..8]=forward. */
    extern float g_cameraBasis[9];

    float cam_x, cam_y, cam_z;
    td5_camera_get_position(&cam_x, &cam_y, &cam_z);

    uint8_t *bank = s_particle_banks[(view_index >= 0 && view_index < TD5_MAX_VIEWPORTS) ? view_index : 0];

    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[0];

        if ((flags & 0x80) == 0) continue;
        if ((flags & 0x20) != 0) continue;

        int32_t wx = PSLOT_RD32(slot, PSLOT_POS_X);
        int32_t wy = PSLOT_RD32(slot, PSLOT_POS_Y);
        int32_t wz = PSLOT_RD32(slot, PSLOT_POS_Z);

        float cx = (float)wx * FP_TO_FLOAT - cam_x;
        float cy = (float)wy * FP_TO_FLOAT - cam_y;
        float cz = (float)wz * FP_TO_FLOAT - cam_z;

        float view_x = cx * g_cameraBasis[0] + cy * g_cameraBasis[1] + cz * g_cameraBasis[2];
        float view_y = cx * g_cameraBasis[3] + cy * g_cameraBasis[4] + cz * g_cameraBasis[5];
        float view_z = cx * g_cameraBasis[6] + cy * g_cameraBasis[7] + cz * g_cameraBasis[8];

        PSLOT_WRF(slot, PSLOT_VIEW_X, view_x);
        PSLOT_WRF(slot, PSLOT_VIEW_Y, view_y);
        PSLOT_WRF(slot, PSLOT_VIEW_Z, view_z);

        slot[0] = flags | 0x40;
    }
}

/* ========================================================================
 * Procedural texture-free VFX (smoke/rain/tire-marks/glows)
 *
 * The particle effects are rendered through analytic pixel shaders
 * (td5_plat_fx_*) instead of sampling the SMOKE/RAINDROP/FADEWHT/BRAKED/
 * POLICELT atlas sprites — no PNG is needed for particles. Per-particle data
 * rides in the vertex COLOR0 (tint+alpha) / COLOR1 (age+seed) channels and the
 * draw is one batched td5_plat_render_draw_tris per effect (the gauge pattern).
 *
 * TD5RE_FX_PROC=0 (env) restores the legacy textured path for A/B comparison.
 * ======================================================================== */

/* Smoke base tint (0xAARRGGBB): light neutral grey, master alpha modulated by
 * the shader's age/density. Kept slightly cool so exhaust/burnout haze reads as
 * smoke rather than dust. */
#define TD5_FX_SMOKE_TINT   0xD8CDCDD6u  /* light cool grey; alpha = base opacity (shader modulates by density/age) */
#define TD5_FX_SMOKE_LIFE0  512.0f   /* fallback spawn lifetime if PSLOT_LIFE0 is 0 */

/* Visual lifetime CAP (ticks @30Hz): a puff is fully dissipated by this many
 * ticks of elapsed life even if its actual slot lifetime is longer. The
 * rev/exhaust puffs live 0x200=512 ticks (~17 s) and lingered on the ground far
 * too long; capping the visual age makes them gone in ~5 s. Short drift puffs
 * (10..40 ticks) are below the cap, so they age over their natural life. */
#define TD5_FX_SMOKE_VISUAL_CAP  150.0f   /* ~5 s */

/* Procedural-smoke puff size multiplier over the RE-faithful world half-extent.
 * The original game's smoke renders noticeably larger than this RE port's
 * (0x3000-init) size; the procedural path is a modern departure anyway, so we
 * scale the billboard up for fuller, more readable burnout/exhaust clouds.
 * Applied ONLY to the procedural quad (legacy textured path unchanged). */
#define TD5_FX_SMOKE_SIZE_SCALE  2.5f

/* Toward-camera depth bias for smoke verts, matching the fold that
 * td5_render_submit_translucent_world applies (-NEAR_DEPTH_OFFSET *
 * DEPTH_NORMALIZE_INV, i.e. 64/195000): the shared project_vertex omits the -64
 * the opaque pass bakes in, so without this low puffs at wheel/ground height
 * lose the LEQUAL tie to the coplanar road and get occluded by it. A touch more
 * than 64 to reliably win the tie for airborne-but-low launch smoke. */
#define TD5_FX_SMOKE_DEPTH_BIAS  (96.0f / 195000.0f)

int td5_vfx_proc_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TD5RE_FX_PROC");
        cached = (e && e[0] == '0') ? 0 : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "procedural FX %s (TD5RE_FX_PROC)", cached ? "ENABLED" : "disabled");
    }
    return cached;
}

/* Free-running animation time in seconds for the procedural FX shaders.
 *
 * Uses the wall clock (QueryPerformanceCounter via td5_plat_time_us) rather
 * than g_td5.simulation_tick_counter, because the sim tick is FROZEN during the
 * race COUNTDOWN (it only increments when !g_td5.paused — see td5_game.c, and
 * the countdown starts paused). Tying the smoke noise scroll to the sim tick
 * left launch-burnout smoke completely static during the countdown (it read as
 * a frozen disc behind the car). This is a per-client COSMETIC clock only — it
 * never feeds the deterministic sim, so it's safe for netplay lockstep.
 *
 * Wrapped to a 3600 s window so the value stays small enough for crisp float32
 * precision in the shader's noise/warp terms (a once-an-hour 1-frame seam in
 * the noise pattern is imperceptible). */
static float td5_vfx_anim_time(void) {
    return (float)(td5_plat_time_us() % 3600000000ULL) * 1.0e-6f;
}

/* Procedural smoke draw batch (one quad per active smoke particle). Sized to
 * the per-view slot pool; VFX renders serially on the main thread (threaded
 * panes keep VFX on main), so file-scope scratch is safe. */
static TD5_D3DVertex s_fx_smoke_verts[TD5_VFX_PARTICLE_SLOTS_PER_VIEW * 4];
static uint16_t      s_fx_smoke_idx[TD5_VFX_PARTICLE_SLOTS_PER_VIEW * 6];

/**
 * DrawRaceParticleEffects (0x429720)
 *
 * Per-frame particle render pass. Projects each active particle's view-space
 * position to screen, rebuilds its sprite batch quad with the projected
 * corners, and submits via the pre-transformed translucent sprite path
 * (same as the HUD and tire-track pipelines). Using td5_render_submit_
 * translucent — NOT td5_render_queue_translucent_batch, which expects
 * TD5_PrimitiveCmd records, not 0xB8 sprite quads.
 *
 * [2026-06-08 procedural FX] Smoke (type 0) now renders through the analytic
 * ps_fx_smoke shader: each active puff becomes one unit-UV quad whose COLOR1
 * carries (age01, seed); the whole batch is drawn in a single
 * td5_plat_render_draw_tris under TD5_PRESET_TRANSLUCENT_POINT_ZTEST. No SMOKE
 * atlas page is sampled. Rain splashes (type 1, a dropped feature) keep the
 * legacy textured submit. TD5RE_FX_PROC=0 forces the legacy path for both.
 *
 * [ARCH-DIVERGENCE: D3D11 quad path replaces DDraw sprite batch; L5 sweep 2026-05-21]
 *   Orig used a pre-allocated DDraw sprite batch flushed via the immediate
 *   translucent primitive submit. Port rewrites the 4 corners as
 *   TD5_D3DVertex with normalized (sampler-space) UVs queried from the live
 *   texture page dims, then submits via td5_render_submit_translucent. Same
 *   per-particle math (perspective divide focal*inv_z, screen-space ±half_w/h,
 *   z/far_clip depth, no min/max clamp), different vertex format.
 */
void td5_vfx_draw_particles(int view_index) {
    extern void td5_render_submit_translucent_world(uint16_t *quad_data);
    s_current_view_index = view_index;
    int vi = view_index & 1;
    int drawn = 0;
    const int proc = td5_vfx_proc_enabled();   /* procedural (texture-free) smoke */
    int pv = 0;                                  /* accumulated proc-smoke vertices */

    /* Decrement sprite render flags (fade timers) */
    for (int i = 0; i < TD5_VFX_SPRITE_BATCH_COUNT; i++) {
        if ((int8_t)s_sprite_render_flags[vi][i] < 0) {
            s_sprite_render_flags[vi][i]++;
        }
    }

    /* Project all active particles (sets active/projected/cull flags). */
    td5_vfx_project_particles(view_index);

    /* [FIX 2026-05-28 smoke-invisible] Project smoke through the renderer's OWN
     * transform pipeline (same as tire tracks / world geometry) instead of a
     * custom focal. The old path used focal = width*1.207 (~2.15x the
     * renderer's width*0.5625) AND the opposite X sign (sx = +vx vs the
     * renderer's -vx), so off-center smoke was mirrored and pushed off-screen
     * → "no smoke on rear wheels". Now each particle's WORLD position is
     * projected via td5_render_transform_and_project, identical to the marks. */
    TD5_Mat3x3 cam_rot;
    td5_camera_get_basis(&cam_rot.m[0], &cam_rot.m[3], &cam_rot.m[6]);
    TD5_Vec3f zero_pos = { 0.0f, 0.0f, 0.0f };
    td5_render_push_transform();
    td5_render_load_rotation(&cam_rot);
    td5_render_load_translation(&zero_pos);
    const float focal = td5_render_get_focal_length();

    uint8_t *bank = s_particle_banks[vi];
    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[0];

        if ((flags & 0xC0) != 0xC0) continue;       /* need active + projected */

        int batch_index = slot[2];
        if (batch_index < 0 || batch_index >= TD5_VFX_SPRITE_BATCH_COUNT) continue;
        VfxSpriteQuad *sq = &s_sprite_batches[vi * TD5_VFX_SPRITE_BATCH_COUNT + batch_index];

        /* Project the particle's CURRENT world position (POS is advanced by
         * velocity each tick) through the renderer pipeline — same focal,
         * center, X/Y sign, and depth space as the world geometry and marks. */
        int32_t pwx = PSLOT_RD32(slot, PSLOT_POS_X);
        int32_t pwy = PSLOT_RD32(slot, PSLOT_POS_Y);
        int32_t pwz = PSLOT_RD32(slot, PSLOT_POS_Z);
        float sx, sy, sz, inv_z;
        if (!td5_render_transform_and_project((float)pwx * FP_TO_FLOAT,
                                              (float)pwy * FP_TO_FLOAT,
                                              (float)pwz * FP_TO_FLOAT,
                                              &sx, &sy, &sz, &inv_z))
            continue;  /* behind near plane */

        /* Perspective-scale using animated size from slot (original +0x04/+0x08).
         * Size values are 24.8 fixed-point world units (same as positions).
         * 0x4000 init → 64.0 world units half-extent at spawn.
         * Fallback to 30.0 for rain splashes (type 1) which don't animate size. */
        float world_half_w, world_half_h;
        if (slot[PSLOT_TYPE] == 0) {
            int16_t sw = PSLOT_RD16(slot, PSLOT_SIZE_W);
            int16_t sh = PSLOT_RD16(slot, PSLOT_SIZE_H);
            world_half_w = (float)sw * FP_TO_FLOAT;  /* /256.0 */
            world_half_h = (float)sh * FP_TO_FLOAT;
        } else {
            world_half_w = 30.0f;
            world_half_h = 30.0f;
        }
        /* No min/max clamps: original DRAW callback (0x004297D0) and its
         * BuildSpriteQuadTemplate (0x00432bd0) submit the perspective-divided
         * corners directly with no FCOM/min/max — distant smoke shrinks
         * sub-pixel and visually vanishes, matching the cars at the same
         * distance. The earlier 6-px screen floor + 8-unit world floor were
         * port-only additions that kept smoke trails visible past where the
         * spawning car was already too small to see. */
        float half_w = world_half_w * focal * inv_z;
        float half_h = world_half_h * focal * inv_z;

        /* [2026-06-08 procedural FX] Smoke (type 0) -> analytic ps_fx_smoke:
         * emit a canonical unit-UV quad, carry (age01, seed) in COLOR1, and
         * batch the whole pool into one draw after the loop. No SMOKE atlas. */
        if (proc && slot[PSLOT_TYPE] == 0 &&
            pv + 4 <= (int)(sizeof(s_fx_smoke_verts) / sizeof(s_fx_smoke_verts[0]))) {
            /* age01 from THIS puff's own spawn lifetime (PSLOT_LIFE0), capped to
             * a visual lifetime so the 512-tick rev/exhaust puffs dissipate in
             * ~5 s instead of lingering ~17 s. Short drift puffs (life0 < cap)
             * age over their natural life — and no longer get mis-aged to ~0.95
             * at birth (which had shrunk them to nothing). */
            int16_t  remain = PSLOT_RD16(slot, PSLOT_LIFETIME);
            int16_t  life0  = PSLOT_RD16(slot, PSLOT_LIFE0);
            if (life0 <= 0) life0 = (int16_t)TD5_FX_SMOKE_LIFE0;
            float    vcap   = (float)life0;
            if (vcap > TD5_FX_SMOKE_VISUAL_CAP) vcap = TD5_FX_SMOKE_VISUAL_CAP;
            float    age01  = (float)(life0 - remain) / vcap;
            if (age01 < 0.0f) age01 = 0.0f; else if (age01 > 1.0f) age01 = 1.0f;
            uint32_t ageB  = (uint32_t)(age01 * 255.0f + 0.5f);
            uint32_t seedB = (uint32_t)((i * 97 + 13) & 0xFF);
            uint32_t spec  = (ageB << 16) | (seedB << 8); /* COLOR1: .r=age .g=seed */
            uint32_t tint  = TD5_FX_SMOKE_TINT;
            float    dz    = sz - TD5_FX_SMOKE_DEPTH_BIAS;   /* win LEQUAL vs coplanar ground */
            /* Bigger than the RE-faithful size — the original's smoke is larger. */
            float    hw    = half_w * TD5_FX_SMOKE_SIZE_SCALE;
            float    hh    = half_h * TD5_FX_SMOKE_SIZE_SCALE;
            int b = pv;
            /* TL */
            s_fx_smoke_verts[b+0].screen_x = sx - hw; s_fx_smoke_verts[b+0].screen_y = sy - hh;
            s_fx_smoke_verts[b+0].depth_z  = dz;      s_fx_smoke_verts[b+0].rhw      = inv_z;
            s_fx_smoke_verts[b+0].diffuse  = tint;    s_fx_smoke_verts[b+0].specular = spec;
            s_fx_smoke_verts[b+0].tex_u    = 0.0f;    s_fx_smoke_verts[b+0].tex_v    = 0.0f;
            /* TR */
            s_fx_smoke_verts[b+1].screen_x = sx + hw; s_fx_smoke_verts[b+1].screen_y = sy - hh;
            s_fx_smoke_verts[b+1].depth_z  = dz;      s_fx_smoke_verts[b+1].rhw      = inv_z;
            s_fx_smoke_verts[b+1].diffuse  = tint;    s_fx_smoke_verts[b+1].specular = spec;
            s_fx_smoke_verts[b+1].tex_u    = 1.0f;    s_fx_smoke_verts[b+1].tex_v    = 0.0f;
            /* BR */
            s_fx_smoke_verts[b+2].screen_x = sx + hw; s_fx_smoke_verts[b+2].screen_y = sy + hh;
            s_fx_smoke_verts[b+2].depth_z  = dz;      s_fx_smoke_verts[b+2].rhw      = inv_z;
            s_fx_smoke_verts[b+2].diffuse  = tint;    s_fx_smoke_verts[b+2].specular = spec;
            s_fx_smoke_verts[b+2].tex_u    = 1.0f;    s_fx_smoke_verts[b+2].tex_v    = 1.0f;
            /* BL */
            s_fx_smoke_verts[b+3].screen_x = sx - hw; s_fx_smoke_verts[b+3].screen_y = sy + hh;
            s_fx_smoke_verts[b+3].depth_z  = dz;      s_fx_smoke_verts[b+3].rhw      = inv_z;
            s_fx_smoke_verts[b+3].diffuse  = tint;    s_fx_smoke_verts[b+3].specular = spec;
            s_fx_smoke_verts[b+3].tex_u    = 0.0f;    s_fx_smoke_verts[b+3].tex_v    = 1.0f;
            pv += 4;
            drawn++;
            continue;   /* skip the legacy textured submit for this puff */
        }

        /* For smoke (type 0): refresh UVs every frame from variant table indexed
         * by (phase >> 2). Mirrors orig SmokeDrawCallback @ 0x004297D0 which sets
         * flag=2 in BuildSpriteQuadTemplate and writes the 4 corner UVs from
         * DAT_004aabb8[phase >> 2]. Phase cycles 0..0x1F via SmokeUpdateCallback
         * @ 0x00429950 (slot[0] = (slot[0] + 1) & 0x1F) so the puff animates
         * through 8 sub-cells, one cell every 4 ticks. */
        if (slot[PSLOT_TYPE] == 0) {
            int v_idx = (slot[PSLOT_PHASE] >> 2) & 7;
            float vu0 = s_smoke_variant_uv[v_idx][0];
            float vv0 = s_smoke_variant_uv[v_idx][1];
            float vw  = s_smoke_variant_uv[v_idx][2];
            float vh  = s_smoke_variant_uv[v_idx][3];
            sq->tex_u0 = vu0;
            sq->tex_v0 = vv0;
            sq->tex_u1 = vu0 + vw;
            sq->tex_v1 = vv0 + vh;
        }

        /* Normalize texel UVs to [0,1] for the D3D11 sampler. Static atlas
         * pages are 256x256; query actual dimensions so hi-res replacement
         * pages keep working. Matches hud_build_sprite_quad (td5_hud.c:388). */
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims((int)sq->texture_page, &tw, &th);
        float inv_tw = 1.0f / (float)tw;
        float inv_th = 1.0f / (float)th;

        /* Smoke (type 0): re-index UVs per-frame from s_smoke_variant_uv,
         * mirroring orig render callback LAB_004297D0 which reads the variant
         * table at 0x004AABB8 every frame indexed by (phase >> 2). Spawn-time
         * UV bake at vfx_spawn_smoke_at_position is now unused for smoke. */
        float tu0 = sq->tex_u0, tv0 = sq->tex_v0;
        float tu1 = sq->tex_u1, tv1 = sq->tex_v1;
        if (slot[PSLOT_TYPE] == 0) {
            int v_idx = (slot[PSLOT_PHASE] >> 2) & 7;
            float su0 = s_smoke_variant_uv[v_idx][0];
            float sv0 = s_smoke_variant_uv[v_idx][1];
            float sw  = s_smoke_variant_uv[v_idx][2];
            float sh  = s_smoke_variant_uv[v_idx][3];
            tu0 = su0;       tv0 = sv0;
            tu1 = su0 + sw;  tv1 = sv0 + sh;
        }
        float nu0 = tu0 * inv_tw;
        float nv0 = tv0 * inv_th;
        float nu1 = tu1 * inv_tw;
        float nv1 = tv1 * inv_th;

        /* Rewrite the 4 corners: v0=TL, v1=TR, v2=BR, v3=BL */
        sq->geometry_ptr = 0;
        sq->vertex_count = 4;

        sq->v0_x = sx - half_w; sq->v0_y = sy - half_h;
        sq->v0_z = sz;          sq->v0_rhw = inv_z;
        sq->v0_color = 0xFFFFFFFF;
        sq->v0_u = nu0;         sq->v0_v = nv0;

        sq->v1_x = sx + half_w; sq->v1_y = sy - half_h;
        sq->v1_z = sz;          sq->v1_rhw = inv_z;
        sq->v1_color = 0xFFFFFFFF;
        sq->v1_u = nu1;         sq->v1_v = nv0;

        sq->v2_x = sx + half_w; sq->v2_y = sy + half_h;
        sq->v2_z = sz;          sq->v2_rhw = inv_z;
        sq->v2_color = 0xFFFFFFFF;
        sq->v2_u = nu1;         sq->v2_v = nv1;

        sq->v3_x = sx - half_w; sq->v3_y = sy + half_h;
        sq->v3_z = sz;          sq->v3_rhw = inv_z;
        sq->v3_color = 0xFFFFFFFF;
        sq->v3_u = nu0;         sq->v3_v = nv1;

        td5_render_submit_translucent_world((uint16_t *)sq);
        drawn++;
    }

    /* Restore the prior render transform (matches the tire-track render). */
    td5_render_pop_transform();

    /* [2026-06-08 procedural FX] Draw the whole smoke pool in ONE batched,
     * texture-free pass (replaces the original per-puff immediate additive
     * draws). Verts are pretransformed (screen-space), so the transform stack
     * is irrelevant. Modern alpha-blended grey smoke (SRCALPHA, z-tested,
     * z_write off) — a deliberate, modern departure from the original's
     * additive white haze. */
    if (proc && pv > 0) {
        int quads = pv / 4;
        for (int q = 0; q < quads; q++) {
            int o = q * 6, bvx = q * 4;
            s_fx_smoke_idx[o+0] = (uint16_t)(bvx + 0);
            s_fx_smoke_idx[o+1] = (uint16_t)(bvx + 1);
            s_fx_smoke_idx[o+2] = (uint16_t)(bvx + 2);
            s_fx_smoke_idx[o+3] = (uint16_t)(bvx + 0);
            s_fx_smoke_idx[o+4] = (uint16_t)(bvx + 2);
            s_fx_smoke_idx[o+5] = (uint16_t)(bvx + 3);
        }
        /* Soft particles: bind scene depth so the shader fades smoke as it nears
         * geometry (no hard flat-card seam where it meets the ground/walls). The
         * returned flag becomes the shader's soft toggle; 0 = depth unavailable,
         * draw normally. */
        int soft = td5_plat_fx_soft_begin();
        if (td5_plat_fx_begin(TD5_FX_SMOKE, td5_vfx_anim_time(), soft ? 1.0f : 0.0f)) {
            td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_POINT_ZTEST);
            td5_plat_render_draw_tris(s_fx_smoke_verts, pv, s_fx_smoke_idx, quads * 6);
            td5_plat_fx_end();
            td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
        }
        if (soft) td5_plat_fx_soft_end();
    }

    if ((s_vfx_debug_frame % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "particle draw view %d: drawn=%d proc=%d pv=%d", view_index, drawn, proc, pv);
    }
}

/**
 * UpdateRaceParticleEffects (0x429790)
 *
 * Per-tick particle update pass. Iterates all active slots and dispatches
 * the update callback for each. Callbacks handle per-particle physics,
 * lifetime, and deactivation.
 */
void td5_vfx_update_particles(int view_index) {
    s_current_view_index = view_index;
    int vi = view_index & 1;
    int active_particles = 0;

    /* [task#14] Spawn any queued TD6 prop-break debris bursts into this bank
     * before the per-particle update (so they animate this frame). */
    vfx_emit_prop_breaks(vi);

    uint8_t *bank = s_particle_banks[vi];
    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[PSLOT_FLAGS];

        if ((flags & 0x80) == 0) continue;
        active_particles++;

        /* Decrement lifetime; deactivate when expired */
        int16_t lifetime = PSLOT_RD16(slot, PSLOT_LIFETIME);
        if (lifetime <= 0) {
            slot[PSLOT_FLAGS] = 0;
            int batch_idx = slot[PSLOT_BATCH];
            if (batch_idx < TD5_VFX_SPRITE_BATCH_COUNT) {
                s_sprite_render_flags[vi][batch_idx] = 0;
            }
            continue;
        }
        PSLOT_WR16(slot, PSLOT_LIFETIME, lifetime - 1);

        uint8_t type = slot[PSLOT_TYPE];
        if (type == 0) {
            /* --- SmokeUpdateCallback (0x00429950) ---
             * Velocity drag: vel -= vel / 4 (arithmetic shift right 2)
             * Position integration: pos += vel
             * Size animation: size += delta per tick
             * Phase cycling: (phase + 1) & 0x1F */
            int32_t vx = PSLOT_RD32(slot, PSLOT_VEL_X);
            int32_t vy = PSLOT_RD32(slot, PSLOT_VEL_Y);
            int32_t vz = PSLOT_RD32(slot, PSLOT_VEL_Z);

            /* Arithmetic drag: vel -= vel >> 2 (SAR 2) */
            vx -= (vx >> 2);
            vz -= (vz >> 2);

            PSLOT_WR32(slot, PSLOT_VEL_X, vx);
            PSLOT_WR32(slot, PSLOT_VEL_Z, vz);

            /* Position integration */
            PSLOT_WR32(slot, PSLOT_POS_X, PSLOT_RD32(slot, PSLOT_POS_X) + vx);
            PSLOT_WR32(slot, PSLOT_POS_Y, PSLOT_RD32(slot, PSLOT_POS_Y) + vy);
            PSLOT_WR32(slot, PSLOT_POS_Z, PSLOT_RD32(slot, PSLOT_POS_Z) + vz);

            /* Size animation */
            PSLOT_WR16(slot, PSLOT_SIZE_W,
                        PSLOT_RD16(slot, PSLOT_SIZE_W) + PSLOT_RD16(slot, PSLOT_SIZE_W_D));
            PSLOT_WR16(slot, PSLOT_SIZE_H,
                        PSLOT_RD16(slot, PSLOT_SIZE_H) + PSLOT_RD16(slot, PSLOT_SIZE_H_D));

            /* Phase cycling (32 steps) */
            slot[PSLOT_PHASE] = (slot[PSLOT_PHASE] + 1) & 0x1F;
        }
        /* Type 1 (rain splash): no per-tick animation, just lifetime countdown */
    }

    if ((s_vfx_debug_frame % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG,
                  "particle update view %d: active_particles=%d",
                  view_index, active_particles);
    }
}

/* ========================================================================
 * 2. Weather Overlay Particles
 *    Original: InitializeWeatherOverlayParticles (0x446240)
 *              UpdateAmbientParticleDensityForSegment (0x4464B0)
 *              RenderAmbientParticleStreaks (0x446560)
 * ======================================================================== */

/**
 * InitializeWeatherOverlayParticles (0x446240)
 *
 * Called once at race start. Reads weather type from LEVELINF.DAT, allocates
 * per-view particle buffers, seeds initial random positions, and builds
 * sprite quad templates (rain only -- snow is a cut feature).
 */
void td5_vfx_init_weather(TD5_WeatherType type) {
    s_weather_type = (int)type;
    s_weather_target_density[0] = 0;
    s_weather_target_density[1] = 0;
    s_weather_active_count[0] = 0;
    s_weather_active_count[1] = 0;

    /* Free any previous buffers */
    for (int v = 0; v < 2; v++) {
        if (s_weather_buf[v]) {
            td5_plat_heap_free(s_weather_buf[v]);
            s_weather_buf[v] = NULL;
        }
    }

    if (type == TD5_WEATHER_RAIN) {
        /* Allocate 2 x 0x6400-byte particle buffers (one per viewport) */
        for (int v = 0; v < 2; v++) {
            s_weather_buf[v] = (VfxWeatherParticle *)td5_plat_heap_alloc(
                TD5_VFX_WEATHER_BUFFER_SIZE);
            if (!s_weather_buf[v]) {
                TD5_LOG_E("vfx", "Failed to allocate weather buffer for view %d", v);
                return;
            }
            memset(s_weather_buf[v], 0, TD5_VFX_WEATHER_BUFFER_SIZE);
        }

        /* Load RAINDROP sprite from the static atlas and extract UVs */
        TD5_AtlasEntry *raindrop = td5_asset_find_atlas_entry(NULL, "RAINDROP");
        if (vfx_atlas_entry_valid(raindrop)) {
            vfx_extract_sprite_uvs(raindrop,
                                    &s_weather_u0, &s_weather_v0,
                                    &s_weather_u1, &s_weather_v1,
                                    &s_weather_sprite_page);
        } else {
            s_weather_u0 = 1.0f;   s_weather_v0 = 1.0f;
            s_weather_u1 = 14.0f;  s_weather_v1 = 30.0f;
            s_weather_sprite_page = 0.0f;
            TD5_LOG_W("vfx", "RAINDROP sprite not found in static atlas");
        }

        /* Seed 128 particles per buffer with random positions */
        /* Rain init ranges: X [-4000,4000], Y [-3000,3000], Z [-1947,1947] */
        for (int v = 0; v < 2; v++) {
            VfxWeatherParticle *buf = s_weather_buf[v];
            for (int i = 0; i < TD5_VFX_MAX_WEATHER_PARTICLES; i++) {
                buf[i].pos_x = (float)(rand() % 8000) - 4000.0f;
                buf[i].pos_y = (float)(rand() % 6000) - 3000.0f;
                buf[i].pos_z = (float)(rand() % 3895) - 1947.0f;
                buf[i].visible = 1.0f;

                /* Build sprite quad template for this particle using RAINDROP UVs */
                vfx_build_sprite_quad(&buf[i].quad,
                                       0.0f, 0.0f, 128.0f,  /* center + depth placeholder */
                                       0.5f, 8.0f,           /* thin vertical streak */
                                       s_weather_u0, s_weather_v0,
                                       s_weather_u1, s_weather_v1,
                                       s_weather_sprite_page,
                                       0xFFFFFFFF);           /* white, modulated by pipeline */
            }
        }

        TD5_LOG_I(LOG_TAG, "weather init: type=%s particles=%d",
                  vfx_weather_type_name(type), TD5_VFX_MAX_WEATHER_PARTICLES);

    } else if (type == TD5_WEATHER_SNOW) {
        /* Snow: CUT FEATURE. Allocates buffers and seeds positions, but
         * never builds sprite templates and rendering is gated off.
         * We replicate the original behavior faithfully. */
        for (int v = 0; v < 2; v++) {
            s_weather_buf[v] = (VfxWeatherParticle *)td5_plat_heap_alloc(
                TD5_VFX_WEATHER_BUFFER_SIZE);
            if (!s_weather_buf[v]) return;
            memset(s_weather_buf[v], 0, TD5_VFX_WEATHER_BUFFER_SIZE);
        }

        /* FindArchiveEntryByName("SNOWDROP") -- referenced but UVs never extracted.
         * This matches the original: the entry is looked up but the result is unused. */
        (void)td5_asset_find_atlas_entry(NULL, "SNOWDROP");

        /* Snow init ranges: X [-8000,8000], Y [-8000,4000], Z [200,8191] */
        for (int v = 0; v < 2; v++) {
            VfxWeatherParticle *buf = s_weather_buf[v];
            for (int i = 0; i < TD5_VFX_MAX_WEATHER_PARTICLES; i++) {
                buf[i].pos_x = (float)(rand() % 16000 - 8000);
                buf[i].pos_y = (float)(rand() % 12000 - 8000);
                /* [FIX 2026-05-24 OVERSIGHT: snow_z_range_off_by_48; orig 0x00446240] 7943 -> 0x1f37 (7991) */
                buf[i].pos_z = (float)(rand() % 0x1f37 + 200);
                /* NOTE: No BuildSpriteQuadTemplate call -- snow never renders */
            }
        }

        TD5_LOG_I(LOG_TAG, "weather init: type=%s particles=%d",
                  vfx_weather_type_name(type), TD5_VFX_MAX_WEATHER_PARTICLES);

    } else {
        /* Clear weather -- no particles, no audio */
        TD5_LOG_I(LOG_TAG, "weather init: type=%s particles=0",
                  vfx_weather_type_name(type));
    }

    memset(s_weather_prev_cam, 0, sizeof(s_weather_prev_cam));
    memset(s_weather_prev_budget, 0, sizeof(s_weather_prev_budget));
}

/**
 * UpdateAmbientParticleDensityForSegment (0x4464B0)
 *
 * Called twice per sim tick (once per player view). Reads the actor's
 * current track segment, walks the density pair table from LEVELINF.DAT,
 * and ramps the active particle count toward the target density.
 *
 * Zone-based weather intensity: as the player drives through different
 * track segments, particle density ramps up or down by 1 per call.
 */
void td5_vfx_update_ambient_density(TD5_Actor *actor, int view_index) {
    if (!actor) return;
    int vi = view_index & 1;

    /* Read actor's current track segment from actor+0x80 (int16 span index)
     * In the clean struct: actor->track_span_raw */
    int16_t current_segment;
    memcpy(&current_segment, (uint8_t *)actor + 0x80, sizeof(int16_t));

    /* Walk density pair array from track environment config (LEVELINF.DAT).
     * Original UpdateAmbientParticleDensityForSegment @ 0x004464B0:
     *   count @ config+0x2C (int32, loop bound)            [CONFIRMED @ 0x004464C8]
     *   pair[0].seg @ config+0x34, pair[0].density @ +0x36 [CONFIRMED @ 0x004464D1
     *     LEA EDX,[EDI+0x36]; MOVSX seg=[EDX-2]; density=[EDX]; ADD EDX,4 stride]
     * Each pair is 4 bytes: [int16 segment_id, int16 density], both sign-extended.
     * (Was config+0x36 here — off by 2; that read density-as-seg and produced
     *  garbage zoning. e.g. Moscow/level001 pairs at +0x34 = {100,32},{400,0},
     *  {900,0},{1000,128},{1500,128},{1600,0}.) */
    if (g_track_environment_config) {
        int32_t pair_count;
        memcpy(&pair_count, g_track_environment_config + 0x2C, sizeof(int32_t));
        if (pair_count > TD5_VFX_MAX_DENSITY_PAIRS)
            pair_count = TD5_VFX_MAX_DENSITY_PAIRS;

        uint8_t *pair_base = g_track_environment_config + 0x34;
        for (int p = 0; p < pair_count; p++) {
            int16_t seg_id, density;
            memcpy(&seg_id, pair_base + p * 4, sizeof(int16_t));
            memcpy(&density, pair_base + p * 4 + 2, sizeof(int16_t));

            if (current_segment == seg_id) {
                int target = (int)density;
                if (target > TD5_VFX_MAX_WEATHER_PARTICLES)
                    target = TD5_VFX_MAX_WEATHER_PARTICLES;
                /* Decision-point log: fires only on a zone-boundary crossing that
                 * changes the target (~6x per race max), not per-frame. */
                if (target != s_weather_target_density[vi]) {
                    TD5_LOG_I(LOG_TAG,
                              "weather zone: view=%d span=%d target_density=%d active=%d",
                              vi, (int)current_segment, target,
                              s_weather_active_count[vi]);
                }
                s_weather_target_density[vi] = target;
            }
        }
    }

    /* Ramp active count toward target: +1 per call if below, -1 if above */
    if (s_weather_target_density[vi] != 0 &&
        s_weather_active_count[vi] < s_weather_target_density[vi]) {
        /* Newly activated particle gets visibility flag zeroed for re-seed */
        if (s_weather_buf[vi]) {
            int idx = s_weather_active_count[vi];
            if (idx < TD5_VFX_MAX_WEATHER_PARTICLES) {
                s_weather_buf[vi][idx].visible = 0.0f;
            }
        }
        s_weather_active_count[vi]++;
    }

    if (s_weather_active_count[vi] > s_weather_target_density[vi]) {
        s_weather_active_count[vi]--;
    }
}

/**
 * RenderAmbientParticleStreaks (0x446560)
 *
 * Per-frame weather particle rendering. Advances particles in camera space,
 * projects to screen coordinates, builds thin vertical line quads (streak
 * rendering), and queues translucent primitives.
 *
 * Rain streaks derive screen-space direction from camera motion:
 * combined = wind * sim_budget - camera_delta
 *
 * Each raindrop is a 1-pixel-wide angled streak whose length and angle
 * depend on camera velocity.
 */
void td5_vfx_render_ambient_streaks(TD5_Actor *actor, float sim_budget,
                                     int view_index) {
    if (!actor) return;

    /* Gate: only rain (type 0) renders. Snow (type 1) is gated off.
     * This matches the JNZ at 0x4465F4 in the original. */
    if (s_weather_type != 0) return;

    int vi = view_index & 1;
    int active_count = s_weather_active_count[vi];
    if (active_count <= 0) return;
    if (!s_weather_buf[vi]) return;

    /* Rain streaks are a fixed SCREEN overlay, NOT world geometry. The original
     * builds pre-transformed (XYZRHW) screen-space quads [CONFIRMED @
     * BuildSpriteQuadTemplate 0x00432bd0; the matrix is NOT applied] and submits
     * them through the generic translucent path. The port's equivalent
     * pre-transformed submit is td5_render_submit_translucent_hud (alpha blend);
     * routing these through td5_render_queue_translucent_batch (TD5_PrimitiveCmd
     * records) mis-parsed the 0xB8 sprite quad and projected it through the world
     * matrix. RAINDROP @ static.hed slot 5 (3x8) is a real ALPHA-channel streak
     * (reddish-white, alpha 128..215), so alpha-blend (NOT additive). */

    /* Normalize the RAINDROP texel UVs to [0,1] for the D3D11 sampler, and fetch
     * the viewport center: the projection below yields ORIGIN-relative coords,
     * but the pre-transformed sprite path expects pixel coords (top-left origin),
     * so the center is added per-vertex. */
    int wtw = 256, wth = 256;
    td5_plat_render_get_texture_dims((int)s_weather_sprite_page, &wtw, &wth);
    float weather_nu0 = s_weather_u0 / (float)wtw;
    float weather_nv0 = s_weather_v0 / (float)wth;
    float weather_nu1 = s_weather_u1 / (float)wtw;
    float weather_nv1 = s_weather_v1 / (float)wth;
    /* [2026-06-08 procedural FX] Procedural rain streak (ps_fx_rain): canonical
     * unit UVs so the shader's across-width / along-length gradients are valid.
     * The override is set once around the whole streak batch below; the existing
     * per-quad submit_translucent_hud draws inherit it (no RAINDROP atlas). */
    const int proc_rain = td5_vfx_proc_enabled();
    if (proc_rain) { weather_nu0 = 0.0f; weather_nv0 = 0.0f; weather_nu1 = 1.0f; weather_nv1 = 1.0f; }
    float screen_cx = td5_render_get_center_x();
    float screen_cy = td5_render_get_center_y();

    /* Sub-tick camera position interpolation.
     * cam = (1/256) * (actor->linear_velocity * g_subTickFraction + actor->world_pos)
     * Original reads actor+0x1CC/0x1D0/0x1D4 (velocity) and actor+0x1FC/0x200/0x204 (position) */
    uint8_t *ap = (uint8_t *)actor;
    int32_t vel_x, vel_y, vel_z;
    int32_t pos_x, pos_y, pos_z;
    memcpy(&vel_x, ap + 0x1CC, 4);
    memcpy(&vel_y, ap + 0x1D0, 4);
    memcpy(&vel_z, ap + 0x1D4, 4);
    memcpy(&pos_x, ap + 0x1FC, 4);
    memcpy(&pos_y, ap + 0x200, 4);
    memcpy(&pos_z, ap + 0x204, 4);

    float sub_tick = g_subTickFraction;

    float cam_x = FP_TO_FLOAT * ((float)vel_x * sub_tick + (float)pos_x);
    float cam_y = FP_TO_FLOAT * ((float)vel_y * sub_tick + (float)pos_y);
    float cam_z = FP_TO_FLOAT * ((float)vel_z * sub_tick + (float)pos_z);

    /* RAINSPL ground-splash spawn intentionally DROPPED (user request 2026-06-03).
     * The original spawns world-space RAINSPL "circular droplet" splashes at
     * ground level (z=0) into the general particle pool [orig SpawnAmbientParticle-
     * Streak @ 0x0042a6b0]. They render as world-locked sprites on the road. The
     * user prefers the screen-space rain LINES (streaks) alone, so the splash
     * spawn is removed — a deliberate, documented deviation from the original.
     * (Re-enable by restoring the SpawnAmbientParticleStreak port here, seeding
     * a type-1 splash slot in s_particle_banks[vi] at pos +/- rand world coords.) */

    /* Pause gate: if paused, freeze particle positions */
    if (g_td5.paused) {
        sim_budget = s_weather_prev_budget[vi];
    } else {
        s_weather_prev_budget[vi] = sim_budget;
    }

    /* Compute streak direction from wind + camera motion delta */
    float wind_x = 0.0f;
    float wind_y = WEATHER_WIND_Y * sim_budget;
    float wind_z = 0.0f;

    int cam_idx = vi;
    float cam_delta_x = cam_x - s_weather_prev_cam[cam_idx][0];
    float cam_delta_y = cam_y - s_weather_prev_cam[cam_idx][1];
    float cam_delta_z = cam_z - s_weather_prev_cam[cam_idx][2];

    /* Contiguous vec3 (not 3 separate locals) so td5_render_transform_vec3,
     * which reads in[0..2], is well-defined. */
    float motion[3];
    motion[0] = wind_x - cam_delta_x;
    motion[1] = wind_y - cam_delta_y;
    motion[2] = wind_z - cam_delta_z;

    if (!g_td5.paused) {
        s_weather_prev_cam[cam_idx][0] = cam_x;
        s_weather_prev_cam[cam_idx][1] = cam_y;
        s_weather_prev_cam[cam_idx][2] = cam_z;
    }

    /* Transform the world-space (wind + camera-motion) vector into VIEW space
     * using the CAMERA rotation, so the streaks (a) follow the car's heading —
     * turning/looking around tilts the rain — and (b) fall in the correct screen
     * direction. The original calls LoadRenderRotationMatrix(camera) before this
     * transform [orig RenderAmbientParticleStreaks]; the port was MISSING that
     * load, so transform_vec3 ran against a stale/identity matrix -> rain ignored
     * car rotation and streaked upward. Mirrors td5_vfx_draw_particles' setup. */
    TD5_Mat3x3 weather_cam_rot;
    td5_camera_get_basis(&weather_cam_rot.m[0], &weather_cam_rot.m[3],
                         &weather_cam_rot.m[6]);
    td5_render_push_transform();
    td5_render_load_rotation(&weather_cam_rot);
    float view_motion[3];
    td5_render_transform_vec3(motion, view_motion);
    td5_render_pop_transform();

    /* Per-frame advance. Original [CONFIRMED @ 0x004466fe-0x0044674f]:
     *   advect = view_motion * 4096.0 (DAT_0045d604) / (float)g_projectionDepthBias
     * where g_projectionDepthBias @ 0x00467368 is INTEGER 1 (FILD), NOT a scale.
     * The prior port mis-identified 0x00467368 as g_worldToRenderScale (1/256)
     * and DIVIDED by it, so advect = view_motion * 4096 * 256 (~1e6) -> every
     * particle left the +/-(4000,3000,1947) bounds in a single frame and was
     * recycled before it ever drew. That is why no streaks rendered (the only
     * visible weather was the world-space RAINSPL splashes). The 4096 numerator
     * and the runtime projection bias cancel to ~unity for an in-bounds fall of
     * ~|wind*sim_budget| units/frame, so use net x1 (view_motion). Wind is
     * (0,-250,0)*sim_budget minus camera delta, already rotated to view space. */
    /* [FIX 2026-06-14 rain-upward] The motion vector (wind (0,-250·b,0) minus
     * camera delta) is rotated to VIEW space by the 3x3 above, but — unlike
     * every other projected sprite — the rain path then feeds it STRAIGHT into
     * the screen projection (sy = (pos_y - Y_OFF)·scale/depth) with NO axis
     * negation, whereas the world/smoke projector negates view X/Y before the
     * divide (td5_render_transform_and_project -> project_vertex(-vx,-vy,vz)).
     * The original game's RenderAmbientParticleStreaks (0x00446560) skips the
     * negation too, BUT it rotates against its cached render-rotation matrix
     * (g_raceParticlePoolBase[0x1f5]) whose up-vector Y has the OPPOSITE sign to
     * the port's live g_cameraBasis — the port adds a 180°-roll correction to the
     * chase basis (td5_camera.c ~2098) plus the chase coord-flip that this cached
     * matrix lacked. Net: wind_y=-250 with the port's +up.Y basis advects pos_y
     * NEGATIVE -> screen_y DECREASES -> rain streaks travel UP the screen.
     * Fix: negate the in-view advect X/Y so the rain lands in the SAME screen-
     * space convention as all other geometry (down = +screen_y = gravity). Z is
     * depth and is left untouched. Gated because the basis handedness here is
     * subtle and has been mis-signed before; default = corrected (falls down).
     * TD5RE_RAIN_DIR=0 restores the old (upward) behavior for A/B. */
    static int s_rain_dir = -1;
    if (s_rain_dir < 0) {
        const char *e = getenv("TD5RE_RAIN_DIR");
        s_rain_dir = (e && e[0] == '0') ? 0 : 1;   /* default ON = falls down */
        TD5_LOG_I(LOG_TAG, "rain direction %s (TD5RE_RAIN_DIR)",
                  s_rain_dir ? "CORRECTED (down)" : "legacy (up)");
    }
    float advect_x = s_rain_dir ? -view_motion[0] : view_motion[0];
    float advect_y = s_rain_dir ? -view_motion[1] : view_motion[1];
    float advect_z = view_motion[2];

    /* Bind the procedural rain shader once for the whole streak batch (the
     * per-quad submit_translucent_hud draws below inherit the PS override). */
    int rain_fx = proc_rain && td5_plat_fx_begin(TD5_FX_RAIN, td5_vfx_anim_time(), 0.0f);

    /* Per-particle update and render loop */
    VfxWeatherParticle *buf = s_weather_buf[vi];
    for (int i = 0; i < active_count; i++) {
        VfxWeatherParticle *p = &buf[i];

        float prev_x = p->pos_x;
        float prev_y = p->pos_y;
        float prev_z = p->pos_z;

        /* Advect particle */
        float new_x = prev_x + advect_x;
        float new_y = prev_y + advect_y;
        float new_z = prev_z + advect_z;

        /* Bounds check: recycle out-of-bounds particles */
        float abs_x = fabsf(new_x);
        float abs_y = fabsf(new_y);
        float abs_z = fabsf(new_z);

        int out_of_bounds = (abs_x > WEATHER_BOUNDS_X) ||
                            (abs_y > WEATHER_BOUNDS_Y) ||
                            (abs_z > WEATHER_BOUNDS_Z);

        /* Original computes visibility as (not-out-of-bounds) + 1, stored as float */
        p->visible = out_of_bounds ? 0.0f : 1.0f;

        if (out_of_bounds || p->visible == 0.0f) {
            /* Re-seed with new random position */
            p->visible = 1.4e-45f; /* tiny nonzero = just spawned */
            p->pos_x = (float)(rand() % 8000) - 4000.0f;
            p->pos_y = (float)(rand() % 6000) - 3000.0f;
            p->pos_z = (float)(rand() % 3895) - 1947.0f;
            continue; /* skip rendering this frame */
        }

        if (!g_td5.paused) {
            p->pos_x = new_x;
            p->pos_y = new_y;
            p->pos_z = new_z;
        }

        /* Perspective projection to screen space */
        float prev_depth = prev_z + WEATHER_DEPTH_OFFSET;
        float new_depth  = new_z  + WEATHER_DEPTH_OFFSET;

        if (prev_depth <= 0.0f || new_depth <= 0.0f) continue;

        float prev_sx = prev_x * (PERSPECTIVE_SCALE / prev_depth);
        float prev_sy = (prev_y - WEATHER_Y_OFFSET) * (PERSPECTIVE_SCALE / prev_depth);
        float new_sx  = new_x  * (PERSPECTIVE_SCALE / new_depth);
        float new_sy  = (new_y - WEATHER_Y_OFFSET) * (PERSPECTIVE_SCALE / new_depth);

        /* Choose top/bottom of streak based on Y ordering:
         * the point with higher Y (lower on screen) becomes "top" */
        float top_x, top_y, top_depth;
        float bot_x, bot_y, bot_depth;
        if (new_sy < prev_sy) {
            top_x = prev_sx; top_y = prev_sy; top_depth = prev_depth;
            bot_x = new_sx;  bot_y = new_sy;  bot_depth = new_depth;
        } else {
            top_x = new_sx;  top_y = new_sy;  top_depth = new_depth;
            bot_x = prev_sx; bot_y = prev_sy; bot_depth = prev_depth;
        }

        /* Re-center origin-relative projected coords into viewport pixel space
         * so the streak lands on screen (the pre-transformed sprite path uses a
         * top-left origin). */
        top_x += screen_cx; top_y += screen_cy;
        bot_x += screen_cx; bot_y += screen_cy;

        /* Build a 1-pixel-wide quad connecting the two projected points.
         * Original sets vertex positions with +/- HALF_PIXEL X offset. */
        VfxSpriteQuad *quad = &p->quad;
        float avg_depth = (top_depth + bot_depth) * 0.5f;
        float rhw = (avg_depth > 0.0f) ? (1.0f / avg_depth) : 1.0f;

        quad->geometry_ptr = 0;
        quad->vertex_count = 4;

        /* v0 = top-left */
        quad->v0_x = top_x - HALF_PIXEL;  quad->v0_y = top_y;
        quad->v0_z = avg_depth;            quad->v0_rhw = rhw;
        quad->v0_color = 0xFFFFFFFF;       quad->v0_u = weather_nu0;
        quad->v0_v = weather_nv0;

        /* v1 = top-right */
        quad->v1_x = top_x + HALF_PIXEL;  quad->v1_y = top_y;
        quad->v1_z = avg_depth;            quad->v1_rhw = rhw;
        quad->v1_color = 0xFFFFFFFF;       quad->v1_u = weather_nu1;
        quad->v1_v = weather_nv0;

        /* v2 = bottom-right */
        quad->v2_x = bot_x + HALF_PIXEL;  quad->v2_y = bot_y;
        quad->v2_z = avg_depth;            quad->v2_rhw = rhw;
        quad->v2_color = 0xFFFFFFFF;       quad->v2_u = weather_nu1;
        quad->v2_v = weather_nv1;

        /* v3 = bottom-left */
        quad->v3_x = bot_x - HALF_PIXEL;  quad->v3_y = bot_y;
        quad->v3_z = avg_depth;            quad->v3_rhw = rhw;
        quad->v3_color = 0xFFFFFFFF;       quad->v3_u = weather_nu0;
        quad->v3_v = weather_nv1;

        quad->texture_page = s_weather_sprite_page;

        /* Pre-transformed screen-space submit (alpha-blend HUD preset: LINEAR +
         * alpha_ref=1, z-test off) — keeps the rain fixed on the viewport on top
         * of the 3D scene instead of locking it to world geometry. When the
         * procedural rain shader is bound (rain_fx), this draws through it
         * instead of sampling RAINDROP. */
        td5_render_submit_translucent_hud((uint16_t *)quad);
    }

    if (rain_fx) td5_plat_fx_end();
}

/* ========================================================================
 * 3. Tire Track Pool
 *    Original: AcquireTireTrackEmitter (0x43F030)
 *              UpdateTireTrackEmitters (0x43EB50)
 *              RenderTireTrackPool (0x43F210)
 *              UpdateTireTrackEmitterDispatch (0x43FAE0)
 *              UpdateFront/RearTireEffects (0x43F960/0x43F7E0)
 *              UpdateFront/RearWheelSoundEffects (0x43F420/0x43F600)
 * ======================================================================== */

/**
 * Check if surface type halves track intensity (hard surfaces).
 * Types 2,4,5,6,9 produce less visible marks.
 */
static bool vfx_is_hard_surface(uint8_t surface_type) {
    return surface_type == 2 || surface_type == 4 || surface_type == 5 ||
           surface_type == 6 || surface_type == 9;
}

/**
 * Scan the 80-slot pool for a free entry using the roving cursor.
 * Returns slot index, or -1 if pool is fully allocated.
 * [CONFIRMED @ 0x43F04A-0x43F0AC] Linear scan from cursor, wrap to 0.
 */
static int vfx_alloc_slot_index(void) {
    if (!s_tire_track_pool) return -1;
    for (int i = s_tire_track_cursor; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
        if ((s_tire_track_pool[i].control & 1) == 0) {
            s_tire_track_cursor = i;
            return i;
        }
    }
    for (int i = 0; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
        if ((s_tire_track_pool[i].control & 1) == 0) {
            s_tire_track_cursor = i;
            return i;
        }
    }
    return -1;
}

/**
 * AcquireTireTrackEmitter (0x43F030)
 *
 * Allocates a free slot from the 80-slot tire track pool using a roving
 * cursor. Scans forward from cursor, wraps to 0 if needed.
 * Returns pool slot index, or -1 if no free slot found.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4]
 *   Allocator math (two-pass scan, cursor wrap, descriptor write at +0xd8/d9/da)
 *   matches orig at 0x0043F030 byte-faithfully.
 *
 *   Divergence: orig anchor source is actor base + wheel_idx*0xc + 0xf0
 *   (probe-position field at +0xf0..+0xfb). Port reads actor + 0x298 +
 *   wheel_index*12 ("hires wheel positions"). These are different actor
 *   struct fields. If 0xf0 in orig is probe_y (a low-res ground-contact
 *   sample) the port's higher-res +0x298 will lag by 1 sim tick when the
 *   wheel translates. Visual effect: tire tracks lag wheel motion by 1
 *   frame. Net: subtle but observable.
 *
 *   Recommendation: verify against td5_actor_struct.h which field at +0xf0
 *   is, and switch read site. Tracked as L4 pending confirmation.
 */
static int vfx_acquire_tire_track_emitter(int wheel_id, int actor_slot,
                                           int wheel_index, uint8_t alpha,
                                           uint8_t width) {
    int found = vfx_alloc_slot_index();
    if (found == -1) return -1;

    /* Write emitter descriptor */
    int desc_idx = wheel_id + actor_slot * 4;
    if (desc_idx < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]))) {
        s_emitter_descs[desc_idx].actor_slot = (uint8_t)actor_slot;
        s_emitter_descs[desc_idx].wheel_id = (uint8_t)wheel_index;
        s_emitter_descs[desc_idx].active = 1;
        s_emitter_descs[desc_idx].pool_slot = (uint8_t)found;
        s_emitter_descs[desc_idx].initial_alpha = alpha;
        s_emitter_descs[desc_idx].target_alpha = width;
        s_emitter_descs[desc_idx].width = width;
    }

    /* DIAG 2026-05-28: confirm acquire set the descriptor active. */
    TD5_LOG_I(LOG_TAG, "ACQUIRE: wheel_id=%d actor_slot=%d desc_idx=%d found=%d "
              "-> descs[%d].active=%d pool_slot=%d",
              wheel_id, actor_slot, desc_idx, found, desc_idx,
              (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0])))
                  ? (int)s_emitter_descs[desc_idx].active : -1,
              found);

    /* Initialize pool slot */
    VfxTireTrackSlot *slot = &s_tire_track_pool[found];

    /* Clear perp vectors — a reused slot must not carry stale lead/trail
     * half-widths (the first-motion seed below sets them fresh). */
    slot->perp_x = slot->perp_z = 0;
    slot->perp0_x = slot->perp0_z = 0;

    /* Seed anchor from actor's live wheel position via global actor table. */
    if (g_actor_table_base) {
        uint8_t *actor_base = g_actor_table_base + (size_t)actor_slot * TD5_ACTOR_STRIDE;
        vfx_read_wheel_world_pos((TD5_Actor *)actor_base, wheel_index,
                                 &slot->anchor_x, &slot->anchor_y, &slot->anchor_z);
        slot->trail_x = slot->anchor_x;
        slot->trail_y = slot->anchor_y;
        slot->trail_z = slot->anchor_z;
    } else {
        slot->anchor_x = 0;
        slot->anchor_y = 0;
        slot->anchor_z = 0;
    }

    slot->control = 1;  /* allocated, no geometry yet */
    slot->lifetime = 0;
    slot->intensity = alpha;  /* alpha = initial opacity; width = perpendicular half-span */

    return found;
}

/**
 * Helper: read wheel world position from actor's wheel_contact_pos[].
 *
 * [CONFIRMED @ 0x0043F0CD AcquireTireTrackEmitter anchor source; REG-5 fix 2026-05-22]
 * Orig reads `actor + 0xf0 + wheel_index*12` (per AcquireTireTrackEmitter:
 *   `*(undefined4 *)(&g_actorRuntimeState.slot.field_0xf0 + iVar3)`
 *   where iVar3 = param_3 * 0xc + param_2 * 0x388).
 * That's the GROUND CONTACT position (wheel_contact_pos[4]), written by
 * RefreshVehicleWheelContactFrames (0x00403720). Prior port read +0x298
 * (wheel_world_positions_hires[4], written by UpdateWheelSuspension at
 * 0x00403A20). The two fields are populated at different points in the
 * sim tick, so reading +0x298 introduced a 1-tick lag in tire-track
 * placement — orig matches the contact-frame freshness exactly.
 *
 * Offset +0xf0 + wheel_index * 12, each component is int32 (24.8 fixed).
 */
static void vfx_read_wheel_world_pos(TD5_Actor *actor, int wheel_index,
                                      int32_t *out_x, int32_t *out_y, int32_t *out_z)
{
    uint8_t *ap = (uint8_t *)actor;
    int offset = 0xF0 + wheel_index * 12;
    memcpy(out_x, ap + offset,     4);
    memcpy(out_y, ap + offset + 4, 4);
    memcpy(out_z, ap + offset + 8, 4);
}

/**
 * Helper: set the anchor position on a tire track pool slot from actor wheel.
 */
static void vfx_set_emitter_anchor_from_wheel(TD5_Actor *actor, int wheel_index,
                                                int pool_slot_idx)
{
    if (pool_slot_idx < 0 || pool_slot_idx >= TD5_VFX_TIRE_TRACK_POOL_SIZE) return;

    int32_t wx, wy, wz;
    vfx_read_wheel_world_pos(actor, wheel_index, &wx, &wy, &wz);

    VfxTireTrackSlot *slot = &s_tire_track_pool[pool_slot_idx];
    slot->anchor_x = wx;
    slot->anchor_y = wy;
    slot->anchor_z = wz;

    /* Seed trail from current position so first-frame delta ≈ 0.
     * Without this, trail = 0 → dx spans world origin to vehicle
     * → trailing edge placed at world origin, stretching a degenerate quad. */
    slot->trail_x = wx;
    slot->trail_y = wy;
    slot->trail_z = wz;
    slot->perp_x = 0;
    slot->perp_z = 0;
}

/**
 * UpdateTireTrackEmitters (0x43EB50)
 *
 * Master per-frame update for all active tire track emitters. For each
 * active emitter descriptor, reads the wheel world position, computes
 * heading direction, builds strip vertices, and potentially spawns new
 * pool slots when the vehicle changes direction or the current slot ages.
 */
void td5_vfx_update_tire_tracks(void) {
    /* Faithful port of UpdateTireTrackEmitters @ 0x0043EB50.
     *
     * Per-tick model — confirmed via Ghidra (Pass 1+2):
     *  - `slot->trail_*` is the FROZEN trailing-edge of the strip segment, set
     *    once when motion begins and never moved again on this slot.
     *  - `slot->anchor_*` is the moving leading-edge: tracks the live wheel
     *    position while the descriptor still owns this slot.
     *  - `slot->perp_*` is the half-width perpendicular: seeded once at the
     *    first-motion tick from the spawn-direction, frozen thereafter.
     *  - When realloc fires the descriptor's pool_slot moves to a new slot;
     *    the old slot's anchor stops moving (no more writers) and renders as
     *    a static historical mark until lifetime > 600.
     *
     * Realloc trigger [CONFIRMED @ 0x0043ED7E]:
     *    ((slot->direction_hash ^ angle12) & 0xFFFFFF80) != 0
     *  OR slot->lifetime > (3 - speedClass)
     * where speedClass = clamp(abs(actor[+0x314]) >> 0xF, 1, 3) [CONFIRMED @ 0x0043EBA8].
     * Lifetime is incremented in render (RenderTireTrackPool 0x0043F210). */

    if (!s_tire_track_pool) return;

    int active_emitters = 0;
    int reallocs_this_tick = 0;

    for (int d = 0; d < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]));
         d++) {
        VfxEmitterDesc *desc = &s_emitter_descs[d];
        if (!desc->active) continue;
        active_emitters++;

        int slot_idx = (int)desc->pool_slot;
        if (slot_idx < 0 || slot_idx >= TD5_VFX_TIRE_TRACK_POOL_SIZE) continue;

        VfxTireTrackSlot *slot = &s_tire_track_pool[slot_idx];

        /* Per-tick alpha ramp: descriptor +6 (current) → +5 (target) by ±1.
         * Port stores current in `initial_alpha` (+4) and target in `target_alpha`
         * (+5). Field name swap vs original is harmless — only this loop reads them. */
        if (desc->initial_alpha < desc->target_alpha) desc->initial_alpha++;
        else if (desc->initial_alpha > desc->target_alpha) desc->initial_alpha--;

        /* Read live wheel world position. */
        int32_t wx = slot->anchor_x, wy = slot->anchor_y, wz = slot->anchor_z;
        if (g_actor_table_base) {
            uint8_t *ap = g_actor_table_base + (size_t)desc->actor_slot * TD5_ACTOR_STRIDE;
            vfx_read_wheel_world_pos((TD5_Actor *)ap, (int)desc->wheel_id, &wx, &wy, &wz);
        }

        /* Speed class: clamp(|actor[+0x314]| >> 0xF, 1, 3). Maps to realloc
         * threshold (3 - speedClass) — fast vehicles realloc every tick, slow
         * every 3rd tick. */
        int32_t speedClass = 1;
        if (g_actor_table_base) {
            int32_t lspd = 0;
            uint8_t *ap = g_actor_table_base + (size_t)desc->actor_slot * TD5_ACTOR_STRIDE;
            memcpy(&lspd, ap + 0x314, 4);
            int32_t abs_lspd = (lspd < 0) ? -lspd : lspd;
            speedClass = abs_lspd >> 0xF;
            if (speedClass < 1) speedClass = 1;
            if (speedClass > 3) speedClass = 3;
        }

        /* Cumulative motion since spawn (trail = frozen spawn position once
         * geometry is established; before that, trail == anchor so dx=0). */
        int32_t dx = wx - slot->trail_x;
        int32_t dz = wz - slot->trail_z;
        int32_t dx8 = dx >> 8;
        int32_t dz8 = dz >> 8;

        /* 12-bit angle from cumulative motion vector. AngleFromVector12
         * convention: (vertical, horizontal) → atan2(dz, dx). */
        uint32_t angle12 = 0;
        if (dx8 != 0 || dz8 != 0) {
            /* BUGFIX 2026-05-28 (Ghidra audit): orig 0x0043EC18 calls
             * AngleFromVector12(dx, dz) — port had args swapped. The
             * swap rotated angle12 by 90°, which corrupted direction_hash
             * (driving the realloc trigger) and the perp seed direction. */
            angle12 = (uint32_t)AngleFromVector12(dx8, dz8);
        }

        /* Realloc decision — only check after geometry has been established;
         * an un-seeded slot has no meaningful direction_hash to compare. */
        int realloc_now = 0;
        if ((slot->control & 2u) != 0u) {
            if (((slot->direction_hash ^ angle12) & 0xFFFFFF80u) != 0u) realloc_now = 1;
            if ((int)slot->lifetime > (3 - (int)speedClass)) realloc_now = 1;
        }

        if (realloc_now) {
            /* [FIX 2026-05-28 marks-not-continuous] Capture the OLD segment's
             * lead (far edge) so the NEW segment's trail (near edge) stitches
             * to it — orig 0x0043EE10 chains segments edge-to-edge. Previously
             * the new trail started at the CURRENT wheel pos while the old lead
             * was one tick behind, leaving a one-tick GAP between every segment
             * → a dotted/broken trail. */
            int32_t stitch_x = slot->anchor_x;
            int32_t stitch_y = slot->anchor_y;
            int32_t stitch_z = slot->anchor_z;
            /* Prior segment's LEAD perp — the new segment's TRAIL edge adopts
             * it so the shared join edge matches exactly (smooth strip). */
            int32_t join_perp_x = slot->perp_x;
            int32_t join_perp_z = slot->perp_z;

            /* Freeze old slot: clear bit0 (released), keep bit1 (render still
             * ages it). [CONFIRMED @ 0x0043EE26: control &= 2] */
            slot->control &= 2u;

            int new_idx = vfx_alloc_slot_index();
            if (new_idx >= 0) {
                VfxTireTrackSlot *ns = &s_tire_track_pool[new_idx];
                memset(ns, 0, sizeof(*ns));
                /* New lead = current wheel; trail STITCHES to the prior
                 * segment's lead so the strip is continuous (no gap). The
                 * nonzero trail→anchor delta makes the first-motion seed below
                 * fire immediately (perp + geometry this tick). */
                ns->anchor_x = wx; ns->anchor_y = wy; ns->anchor_z = wz;
                ns->trail_x  = stitch_x; ns->trail_y = stitch_y; ns->trail_z = stitch_z;
                ns->perp_x = 0; ns->perp_z = 0;
                ns->perp0_x = join_perp_x; ns->perp0_z = join_perp_z;
                ns->direction_hash = angle12;
                ns->intensity      = desc->initial_alpha;
                ns->control        = 1u; /* bit0 set; bit1 set below */
                ns->lifetime       = 0;
                desc->pool_slot = (uint8_t)new_idx;
                slot     = ns;
                slot_idx = new_idx;
                reallocs_this_tick++;
                /* NB: do NOT zero dx/dz here. The earlier attempt to
                 * zero them caused new slots to stall at control=1 and
                 * never render — for fast vehicles (realloc every tick)
                 * every new slot would re-realloc before motion grew. */
            } else {
                /* Pool exhausted: leave the old slot in its frozen state
                 * (control bit1 still set, ages out via render). Restore bit0
                 * so this descriptor keeps writing into the same slot until
                 * the pool drains. Reset lifetime + direction_hash so we
                 * don't keep re-firing the realloc condition every tick. */
                slot->control |= 1u;
                slot->lifetime = 0;
                slot->direction_hash = angle12;
                if ((s_vfx_debug_frame % 60u) == 0u) {
                    TD5_LOG_W(LOG_TAG, "tire track pool full at desc=%d", d);
                }
            }
        }

        /* Update the (possibly new) active slot. Two phases:
         *  - First-motion tick (control bit1 still 0 and we now have nonzero
         *    cumulative motion): seed perp + direction_hash from current angle,
         *    set bit1, leave trail at spawn (already there).
         *  - Anchor (lead position) always tracks the live wheel pos. */
        if ((slot->control & 2u) == 0u) {
            int32_t w = (int32_t)desc->width * 2; /* [2026-06-02 user: wider] orig half-width 0x1A=26 was thin; ×2 */
            int32_t len = td5_isqrt(dx8 * dx8 + dz8 * dz8);
            if (len > 0 && w > 0) {
                slot->perp_x = (-dz * w) / len;
                slot->perp_z = ( dx * w) / len;
                /* A fresh (non-stitched) segment has no prior lead perp — make
                 * its trail edge match its lead edge (a plain rectangle). A
                 * realloc'd segment already carries the prior lead perp in
                 * perp0 (set above), so leave that intact for a smooth join. */
                if (slot->perp0_x == 0 && slot->perp0_z == 0) {
                    slot->perp0_x = slot->perp_x;
                    slot->perp0_z = slot->perp_z;
                }
                slot->direction_hash = angle12;
                slot->control |= 2u;
            }
        } else {
            /* [FIX 2026-06-02 dented / not-rotating-with-drift] Re-aim the LEAD
             * half-width perpendicular at the CURRENT travel direction every tick.
             * The orig froze perp at spawn, so within a segment the strip kept its
             * start angle while the wheel curved -> faceted "dents" that don't follow
             * a drift. The trail edge keeps perp0 (segment start) so the quad twists
             * smoothly start->current; realloc copies this updated perp into the next
             * segment's perp0 for a continuous join. Port-only visual polish. */
            int32_t w = (int32_t)desc->width * 2;
            int32_t len = td5_isqrt(dx8 * dx8 + dz8 * dz8);
            if (len > 0 && w > 0) {
                slot->perp_x = (-dz * w) / len;
                slot->perp_z = ( dx * w) / len;
            }
        }
        /* Anchor follows the wheel each tick on the active slot only. Once
         * realloc moves desc->pool_slot to a new index, this slot's anchor
         * stops being written → it freezes as a historical mark. */
        slot->anchor_x = wx;
        slot->anchor_y = wy;
        slot->anchor_z = wz;
    }

    if ((s_vfx_debug_frame % 60u) == 0u) {
        TD5_LOG_I(LOG_TAG, "tire tracks update: active_emitters=%d reallocs=%d "
                  "| descs[0..5].active=%d,%d,%d,%d,%d,%d pool=%d,%d,%d,%d,%d,%d",
                  active_emitters, reallocs_this_tick,
                  s_emitter_descs[0].active, s_emitter_descs[1].active,
                  s_emitter_descs[2].active, s_emitter_descs[3].active,
                  s_emitter_descs[4].active, s_emitter_descs[5].active,
                  s_emitter_descs[0].pool_slot, s_emitter_descs[1].pool_slot,
                  s_emitter_descs[2].pool_slot, s_emitter_descs[3].pool_slot,
                  s_emitter_descs[4].pool_slot, s_emitter_descs[5].pool_slot);
    }
}

/**
 * RenderTireTrackPool (0x43F210)
 *
 * Iterates all 80 pool slots. For active slots with geometry (bit 1 set),
 * ages the lifetime counter, applies intensity fade after 300 ticks,
 * expires at 600 ticks. Projects 4 world-space vertices to screen space
 * and submits translucent quads with road-flush Y offset.
 */
void td5_vfx_render_tire_tracks(void) {
    /* [CONFIRMED @ 0x43F210] RenderTireTrackPool: iterates 80 slots, ages lifetime,
     * fades after 300 ticks, expires at 600 ticks, reconstructs 4 world-space corners
     * from anchor/trail/perp, submits as translucent quads.
     *
     * FIXES applied:
     * - [CONFIRMED @ 0x43F210] Per-frame anchor_y decrement removed (was gravity sink).
     * - [CONFIRMED @ 0x4743F4] Texture "SEMICOL" (not "SKIDMARK" which doesn't exist).
     * - Frustum test passes world-space coords (is_sphere_visible subtracts cam internally).
     * - Struct redesigned: trail_x/y/z + perp_x/z replace int16 vertex arrays (overflow fix). */


    /* [FIX 2026-05-28 through-walls] Submit tire marks as ground DECALS via
     * TD5_PRESET_SHADOW (z_test=LEQUAL, z_write=0, alpha_ref=1). The marks lie
     * on the road and were drawing OVER walls because the prior HUD submit had
     * z_test off. Marks now share the opaque pass's linear depth (both go
     * through project_vertex), so the depth compare is valid and walls/props
     * occlude them. z_write=0 keeps overlapping trail quads from z-fighting. */
    extern void td5_render_submit_tire_mark(uint16_t *quad_data);
    extern void td5_render_set_tire_mark_fx_preset(int on);

    static uint32_t s_tt_render_frame = 0;
    int tt_log = ((s_tt_render_frame++ % 60u) == 0u);
    int tt_alive = 0, tt_submitted = 0;
    int tt_rej_frustum = 0;

    if (!s_tire_track_pool) return;

    /* [2026-06-08 procedural FX] Bind ps_fx_decal for the whole tire-mark batch
     * (feathered + grained skid, no FADEWHT texel). The submit path picks the
     * low-alpha-ref depth-tested preset while this is active so the feathered
     * edges survive. The per-mark UVs are forced canonical below. */
    const int proc_decal = td5_vfx_proc_enabled();
    int decal_fx = 0;

    /* BUGFIX 2026-05-28: drive the projection through the renderer's own
     * transform pipeline (td5_render_load_rotation +
     * td5_render_load_translation + td5_render_transform_and_project) so
     * the focal length, screen center, and X/Y sign convention all match
     * the world-geometry path. Previous custom projection used
     *   focal = g_render_width_f * 0.5f / 0.41421356f   (~width * 1.207)
     * while the actual renderer uses
     *   s_focal_length = width * 0.5625
     * A ~2.15× focal mismatch made the tire-mark screen positions slide
     * relative to the road as the camera moved — exactly the user's
     * "marks don't stay on the road" symptom. (Audit Agent 4 §camera
     * basis identity flagged the divergent custom math.) */

    /* Snapshot the camera basis as a Mat3x3 for load_rotation. */
    TD5_Mat3x3 cam_rot;
    td5_camera_get_basis(&cam_rot.m[0], &cam_rot.m[3], &cam_rot.m[6]);
    TD5_Vec3f zero_pos = { 0.0f, 0.0f, 0.0f };

    td5_render_push_transform();
    td5_render_load_rotation(&cam_rot);
    td5_render_load_translation(&zero_pos); /* bakes -basis*cam into m[9..11] */

    /* Camera position (only for frustum diagnostic log). */
    float cam_x, cam_y, cam_z;
    td5_camera_get_position(&cam_x, &cam_y, &cam_z);

    if (proc_decal && td5_plat_fx_begin(TD5_FX_DECAL, td5_vfx_anim_time(), 0.0f)) {
        decal_fx = 1;
        td5_render_set_tire_mark_fx_preset(1);
    }

    for (int i = 0; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
        VfxTireTrackSlot *slot = &s_tire_track_pool[i];

        /* Only render slots with geometry (bit 1 = has_geometry) */
        if ((slot->control & 2) == 0) continue;
        tt_alive++;

        /* Age the lifetime counter [CONFIRMED @ 0x43F210] */
        slot->lifetime++;

        /* Expire after 600 ticks [CONFIRMED @ 0x43F210] */
        if (slot->lifetime > TD5_VFX_TIRE_TRACK_LIFETIME) {
            slot->control = 0;
            continue;
        }

        /* Fade after 300 ticks. [FIX 2026-05-28] Orig @0x43F265 decrements
         * intensity ONLY when (lifetime & 8) == 0 — i.e. ~half the ticks
         * (TEST CL,0x8 on the lifetime counter), so the fade from 300→600
         * is gentle. The port keyed the no-fade test off `control & 0x08`,
         * but control is only ever 1/2/3, so the test was always true and
         * the mark faded EVERY tick → ~2× too fast ("marks last very
         * little"). Match orig: gate on bit 3 of the lifetime counter. */
        if (slot->lifetime > TD5_VFX_TIRE_TRACK_FADE_START) {
            if ((slot->lifetime & 8) == 0) {
                if (slot->intensity > 0) {
                    slot->intensity--;
                }
            }
        }

        /* Skip invisible marks */
        if (slot->intensity == 0) { continue; }

        /* Frustum test: is_sphere_visible expects world-space (it subtracts
         * the camera position internally — do NOT pre-subtract cam here).
         *
         * BUGFIX 2026-05-28: radius was 50.0f, but a tire mark sits ~900
         * world-units BELOW the chase camera. With FOV=90°, the bottom
         * frustum plane rejects any sphere with vy ≈ -900 unless radius
         * covers the camera-above-road delta. Symptoms in runtime log:
         *   tire render: alive=18 submitted=0 rej(frus=18)
         * Use a midpoint between anchor (lead) and trail, and a generous
         * radius that comfortably encloses the camera-to-road offset plus
         * the mark size. This is a port-side filter (orig has no
         * equivalent test at 0x43F210); we keep it for performance on
         * far-away marks but stop over-culling near ones. */
        float lead_ws_x  = (float)slot->anchor_x * FP_TO_FLOAT;
        float lead_ws_y  = (float)slot->anchor_y * FP_TO_FLOAT;
        float lead_ws_z  = (float)slot->anchor_z * FP_TO_FLOAT;
        float trail_ws_x = (float)slot->trail_x  * FP_TO_FLOAT;
        float trail_ws_y = (float)slot->trail_y  * FP_TO_FLOAT;
        float trail_ws_z = (float)slot->trail_z  * FP_TO_FLOAT;
        float mid_x = 0.5f * (lead_ws_x + trail_ws_x);
        float mid_y = 0.5f * (lead_ws_y + trail_ws_y);
        float mid_z = 0.5f * (lead_ws_z + trail_ws_z);
        int _frus = td5_render_is_sphere_visible(mid_x, mid_y, mid_z, 1500.0f);
        if (!_frus) {
            tt_rej_frustum++;
            if (tt_log && tt_rej_frustum == 1) {
                TD5_LOG_I(LOG_TAG,
                    "tire frus fail: mid=(%.1f,%.1f,%.1f) cam=(%.1f,%.1f,%.1f)",
                    mid_x, mid_y, mid_z, cam_x, cam_y, cam_z);
            }
            continue;
        }

        /* Reconstruct 4 world corners from anchor/trail/perp (all 24.8 FP).
         * v0=trail-left  v1=trail-right  v2=lead-right  v3=lead-left
         * (lead_ws_* / trail_ws_* already computed above for the frustum
         * midpoint — reuse them.) */
        float px      = (float)slot->perp_x   * FP_TO_FLOAT;  /* lead edge  */
        float pz      = (float)slot->perp_z   * FP_TO_FLOAT;
        float px0     = (float)slot->perp0_x  * FP_TO_FLOAT;  /* trail edge */
        float pz0     = (float)slot->perp0_z  * FP_TO_FLOAT;

        /* [FIX 2026-05-28 marks-dented] Lift the mark slightly above the road.
         * The marks now render under TD5_PRESET_SHADOW (z-test LEQUAL); when
         * the through-walls fix switched to that depth-tested preset the old
         * +Y lift had already been removed (it was using a z-OFF preset), so
         * the marks became COPLANAR with the road under a depth test → per-
         * pixel z-fight that reads as a speckled / "dented" line. The orig
         * lifts marks above the ground for exactly this reason (DAT_0045d6ac).
         * +Y is up (same axis the smoke lifts on). */
        const float TM_LIFT = 0.0f; /* [FIX 2026-06-02] was 24: a world-space lift
         * toward the camera made marks float off the road (dented look) AND project
         * in front of the car body (see-through). Sit them flush; the decal wins the
         * road via a DEPTH bias in submit_tire_mark, and the car (genuinely closer)
         * now occludes them. */
        float tly = trail_ws_y + TM_LIFT;
        float lly = lead_ws_y  + TM_LIFT;

        /* Trail edge uses perp0 (= prior segment's lead perp) so the join edge
         * is shared exactly; lead edge uses this segment's perp. Result is a
         * smooth trapezoid strip instead of dented parallelograms. */
        float wpos[4][3] = {
            { trail_ws_x - px0, tly, trail_ws_z - pz0 }, /* v0: trail-L */
            { trail_ws_x + px0, tly, trail_ws_z + pz0 }, /* v1: trail-R */
            { lead_ws_x  + px,  lly, lead_ws_z  + pz  }, /* v2: lead-R  */
            { lead_ws_x  - px,  lly, lead_ws_z  - pz  }, /* v3: lead-L  */
        };

        /* Project 4 world corners through the renderer's own pipeline.
         * td5_render_transform_and_project does: vz = m·v + t (using the
         * loaded rotation + translation), then perspective-projects with
         * the renderer's s_focal_length / s_center_x / s_center_y / s_far_clip
         * — same globals world geometry uses. Returns 0 if behind near.
         *
         * Y-lift: orig 0x0043F3A2 subtracts DAT_0045d6ac (=20.0f) from the
         * transformed view-Y to lift the mark slightly above the road.
         * We bias the corner's world-Y by -20 BEFORE the transform; with a
         * world basis that has up≈+Y, this lifts the mark above the road
         * plane in world space, which projects identically. */
        float sx[4], sy[4], sz[4], srhw[4];
        int all_visible = 1;

        for (int v = 0; v < 4; v++) {
            float wx = wpos[v][0];
            /* Y-lift removed 2026-05-28: TD5_PRESET_TRANSLUCENT_LINEAR_HUD
             * has z_enable=0, so no z-fight to avoid. The previous +20
             * world-Y bias was amplified by perspective (1/vz) for marks
             * close to the camera — pushing screen-Y to 3000+ on a 720p
             * window — and was a port-side workaround for a depth issue
             * that doesn't exist in the chosen preset. */
            float wy = wpos[v][1];
            float wz = wpos[v][2];

            if (!td5_render_transform_and_project(wx, wy, wz,
                                                   &sx[v], &sy[v], &sz[v],
                                                   &srhw[v])) {
                all_visible = 0;
                break;
            }
        }

        if (!all_visible) { continue; }

        /* [PRECISE-PORT @ 0x0043F23B-0x0043F244] Intensity-to-color pack:
         * local_18 = bVar3 + (bVar3<<8) + (bVar3<<16) + 0xff000000
         * No floor in orig — intensity can fade fully to 0 (transparent gray).
         * Port previously floored at 0x30 (~19% gray) which left a residue at
         * end-of-life that orig clears. Removed to match orig pack-as-is. */
        /* [FIX 2026-05-28 marks-too-faint] Render marks as a DARK decal —
         * black RGB with alpha derived from intensity — so the SHADOW preset's
         * SRCALPHA blend DARKENS the road like a real skid (the original
         * multiplies the road down), instead of the old opaque medium-gray
         * (0x37) patch that read as a faint LIGHT smudge on dark asphalt.
         * Alpha is boosted so a fresh mark is clearly dark, and fades out
         * naturally as the intensity counter decays toward 0.
         *
         * [FIX 2026-06-02 marks-more-visible — user request] Boost raised ×3 -> ×6.
         * RE basis: the original (RenderTireTrackPool @0x0043F210, pack @0x43F23B)
         * draws marks as a GRAYSCALE strip (intensity,intensity,intensity) at
         * alpha 0xFF — effectively an OPAQUE dark-gray strip on the road (initial
         * intensity 0x1A-0x37). The port's black SRCALPHA darkening at ×3 only
         * reached ~30% on a fresh moderate-slip mark, far fainter than the
         * original's opaque strip. ×6 lifts a fresh mark's darkening to a
         * comparable perceived contrast (hard-slip marks still clamp at 255, so
         * this only strengthens the faint moderate-slip marks the user couldn't
         * see). Not "arbitrarily darker" — it targets the original's actual
         * on-road visibility. Tunable via this single multiplier. */
        uint8_t val = slot->intensity;
        uint32_t a = (uint32_t)val * 6u; if (a > 255u) a = 255u;
        uint32_t color = (a << 24); /* RGB=0 (black), A=boosted intensity */

        /* Normalize texel UVs to [0,1] for the D3D11 sampler (same as HUD). */
        int tt_tw = 256, tt_th = 256;
        td5_plat_render_get_texture_dims((int)s_tiremark_page, &tt_tw, &tt_th);
        float tt_inv_tw = 1.0f / (float)tt_tw;
        float tt_inv_th = 1.0f / (float)tt_th;
        float tm_u0 = s_tiremark_u0 * tt_inv_tw;
        float tm_v0 = s_tiremark_v0 * tt_inv_th;
        float tm_u1 = s_tiremark_u1 * tt_inv_tw;
        float tm_v1 = s_tiremark_v1 * tt_inv_th;
        /* Procedural decal: canonical UVs (u across strip width, v along length). */
        if (decal_fx) { tm_u0 = 0.0f; tm_v0 = 0.0f; tm_u1 = 1.0f; tm_v1 = 1.0f; }

        /* Build a VfxSpriteQuad on the stack for submission.
         * Vertex order: 0=trailing-left, 1=trailing-right, 2=leading-right, 3=leading-left
         * Maps to sprite quad: v0=TL, v1=TR, v2=BR, v3=BL */
        VfxSpriteQuad tquad;
        memset(&tquad, 0, sizeof(tquad));

        tquad.geometry_ptr = 0;
        tquad.vertex_count = 4;

        tquad.v0_x = sx[0]; tquad.v0_y = sy[0]; tquad.v0_z = sz[0]; tquad.v0_rhw = srhw[0];
        tquad.v0_color = color; tquad.v0_u = tm_u0; tquad.v0_v = tm_v0;

        tquad.v1_x = sx[1]; tquad.v1_y = sy[1]; tquad.v1_z = sz[1]; tquad.v1_rhw = srhw[1];
        tquad.v1_color = color; tquad.v1_u = tm_u1; tquad.v1_v = tm_v0;

        /* [FIX 2026-06-02 sawtooth/"dented"] v2 must be LEADING-RIGHT (index 2) and
         * v3 LEADING-LEFT (index 3) so the perimeter order is TL,TR,BR,BL and the two
         * triangles (0,1,2)+(0,2,3) tile the trapezoid. The old code used sx[3] for v2
         * and sx[2] for v3 -> perimeter TL,TR,BL,BR = a self-intersecting BOWTIE quad,
         * which left a triangular notch in every segment -> a sawtooth/zigzag trail.
         * UVs are unchanged (v2 keeps u1=right, v3 keeps u0=left), now matching the
         * geometry sides. */
        tquad.v2_x = sx[2]; tquad.v2_y = sy[2]; tquad.v2_z = sz[2]; tquad.v2_rhw = srhw[2];
        tquad.v2_color = color; tquad.v2_u = tm_u1; tquad.v2_v = tm_v1;

        tquad.v3_x = sx[3]; tquad.v3_y = sy[3]; tquad.v3_z = sz[3]; tquad.v3_rhw = srhw[3];
        tquad.v3_color = color; tquad.v3_u = tm_u0; tquad.v3_v = tm_v1;

        tquad.tex_u0 = s_tiremark_u0; tquad.tex_v0 = s_tiremark_v0;
        tquad.tex_u1 = s_tiremark_u1; tquad.tex_v1 = s_tiremark_v1;
        tquad.quad_width = 0.0f;
        tquad.quad_height = 0.0f;
        tquad.texture_page = s_tiremark_page;

        /* Submit as a depth-tested ground decal (TD5_PRESET_SHADOW) so walls
         * occlude the marks instead of them drawing through. */
        td5_render_submit_tire_mark((uint16_t *)&tquad);
        if (tt_log && tt_submitted == 0) {
            TD5_LOG_I(LOG_TAG, "tire diag: sx=(%.1f,%.1f,%.1f,%.1f) "
                      "sy=(%.1f,%.1f,%.1f,%.1f) sz=(%.3f,%.3f,%.3f,%.3f) "
                      "color=0x%08X uv=(%.3f,%.3f..%.3f,%.3f) page=%.0f "
                      "intensity=%u",
                      sx[0], sx[1], sx[2], sx[3],
                      sy[0], sy[1], sy[2], sy[3],
                      sz[0], sz[1], sz[2], sz[3],
                      color, tm_u0, tm_v0, tm_u1, tm_v1,
                      s_tiremark_page, (unsigned)slot->intensity);
        }
        tt_submitted++;
    }

    /* Restore the previous render transform so we don't pollute later draws. */
    td5_render_pop_transform();

    if (decal_fx) {
        td5_render_set_tire_mark_fx_preset(0);
        td5_plat_fx_end();
    }

    if (tt_log) {
        TD5_LOG_I(LOG_TAG, "tire render: alive=%d submitted=%d rej(frus=%d) decal_fx=%d",
                  tt_alive, tt_submitted, tt_rej_frustum, decal_fx);
    }
}

/* Float tire-mark ring REMOVED 2026-05-28. See header comment near
 * line ~277 for the rationale. Stub kept so callers in td5_game.c
 * compile; the function is a no-op. */
void td5_vfx_render_tire_marks(void)
{
    return;
}

/**
 * UpdateTireTrackEmitterDispatch (0x43FAE0)
 *
 * Per-vehicle master dispatcher for tire effects. Reads drivetrain layout
 * from tuning data (+0x76 = 1:RWD, 2:FWD, 3:AWD) and calls the
 * appropriate front/rear tire effect and sound functions.
 *
 * Gated by DAT_004aad60 == 0 (disabled during replays or pause).
 *
 * Called per-actor per-view from td5_render_actors_for_view (mirrors original
 * RenderRaceActorForView @ 0x0040C120 LAB_0040c7ba). view_index drives the
 * per-view smoke particle bank used by vfx_spawn_smoke_at_position. */
void td5_vfx_update_tire_track_emitters(TD5_Actor *actor, int view_index) {
    if (!actor) return;

    /* BUGFIX 2026-05-28: removed `if (g_td5.paused) return;` early-out.
     * The orig guard at DAT_004aad60 is the REPLAY flag, not the pause
     * flag — it's set during replay playback so reverse-running the race
     * doesn't double-spawn marks. The port was confusing it with the
     * countdown pause, which suppressed pre-race burnout entirely.
     * scf-based wheelspin DOES fire during the paused (countdown) branch
     * of td5_physics_update_state_for_actor (line 1253+), so the
     * dispatcher should run during countdown too — the orig DOES produce
     * pre-race burnout marks and smoke if the user holds throttle. */

    s_current_view_index = view_index;

    uint8_t *ap = (uint8_t *)actor;

    /* Read speed values from actor struct */
    int32_t rear_speed, front_speed;
    memcpy(&rear_speed,  ap + 0x31C, 4); /* front_axle_slip_excess */
    memcpy(&front_speed, ap + 0x320, 4); /* rear_axle_slip_excess */

    /* DIAG 2026-05-28 (tire-mark root-cause hunt): one-line dispatcher-entry
     * trace. Answers in a single run: does the dispatcher run? is tuning_ptr
     * NULL (early-return suspect)? drivetrain value? scf/+0x376? the
     * +0x371..+0x374 emitter-id sentinels (0xFF=ready, 00=zero-filled by AI
     * state path, else=already acquired)? Throttled to every 30 calls. */
    {
        static uint32_t s_disp_log = 0;
        if ((s_disp_log++ % 30u) == 0u) {
            void *tp_dbg = NULL; memcpy(&tp_dbg, ap + 0x1BC, sizeof(void *));
            int16_t dt_dbg = -1;
            if (tp_dbg) memcpy(&dt_dbg, (uint8_t *)tp_dbg + 0x76, 2);
            uint8_t slot_dbg = *(ap + 0x375);
            int descidx_w2 = 2 + (int)slot_dbg * 4;
            TD5_LOG_I(LOG_TAG,
                "dispatch ENTER: view=%d tuning=%p drivetrain=%d cf=0x%X "
                "wid=%02X,%02X,%02X,%02X rear_sp=%d front_sp=%d "
                "slot_index=%u desc_idx(w2)=%d descN=%d vmode=%u dmg=0x%X",
                view_index, tp_dbg, (int)dt_dbg, *(ap + 0x376),
                *(ap + 0x371), *(ap + 0x372), *(ap + 0x373), *(ap + 0x374),
                rear_speed, front_speed,
                (unsigned)slot_dbg, descidx_w2,
                (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0])),
                (unsigned)*(ap + 0x379), (unsigned)*(ap + 0x37C));
        }
    }

    /* Always update wheel sound effects */
    vfx_update_rear_wheel_sound_effects(actor, rear_speed);
    vfx_update_front_wheel_sound_effects(actor, front_speed);

    /* Read drivetrain type from tuning data: *(short*)(tuning_ptr + 0x76)
     * 1=RWD, 2=FWD, 3=AWD */
    void *tuning_ptr;
    memcpy(&tuning_ptr, ap + 0x1BC, sizeof(void *));
    if (!tuning_ptr) return;

    int16_t drivetrain;
    memcpy(&drivetrain, (uint8_t *)tuning_ptr + 0x76, sizeof(int16_t));

    uint8_t contact_flags = *(ap + 0x376);

    switch (drivetrain) {
    case 1: /* RWD: rear tire effects only */
        vfx_update_rear_tire_effects(actor, contact_flags);
        break;
    case 2: /* FWD: front tire effects only */
        vfx_update_front_tire_effects(actor, contact_flags);
        break;
    case 3: /* AWD: both front and rear */
        vfx_update_front_tire_effects(actor, contact_flags);
        vfx_update_rear_tire_effects(actor, contact_flags);
        break;
    }
}

/**
 * Helper: spawn smoke at a world position derived from an actor's wheel.
 * Used by tire effect functions for speed-based and slip-based smoke.
 */
static void vfx_spawn_smoke_at_position(TD5_Actor *actor, float wx, float wy,
                                         float wz, int variant, int view_index)
{
    int vi = view_index & 1;
    uint8_t *bank = s_particle_banks[vi];

    /* Frustum-cull gate. The original gates ALL smoke spawns through
     * RenderRaceActorForView (0x0040C120) — only firing for actors that pass
     * TestMeshAgainstViewFrustum. The port routes the tire/slip smoke chain
     * through the sim tick (td5_game.c per-actor loop), bypassing that gate,
     * so distant AI cars used to leave smoke trails after the car itself
     * was culled. Restoring the visibility check at this single chokepoint
     * matches the original's effective behavior without restructuring the
     * sim/render boundary. Render-time entrypoints (td5_vfx_spawn_smoke,
     * td5_vfx_spawn_rear_wheel_smoke) are already inside a frustum gate; the
     * double-gate is harmless. */
    if (!td5_render_is_sphere_visible(wx, wy, wz, 50.0f))
        return;

    TD5_LOG_D(LOG_TAG,
              "smoke spawn: pos=(%.2f, %.2f, %.2f) variant=%d view=%d",
              wx, wy, wz, variant & 3, view_index);

    /* Find a free particle slot */
    for (int s = 0; s < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; s++) {
        uint8_t *slot = bank + s * TD5_VFX_PARTICLE_SLOT_STRIDE;
        if (slot[PSLOT_FLAGS] != 0) continue;

        /* Clear entire slot to avoid stale data */
        memset(slot, 0, TD5_VFX_PARTICLE_SLOT_STRIDE);

        /* Header */
        slot[PSLOT_FLAGS] = 0xC0;  /* active | projected */
        slot[PSLOT_TYPE]  = 0;     /* type 0 = smoke puff */

        /* Lifetime: (rand() % 4 + 1) * 10 = 10..40 ticks [CONFIRMED @ 0x0042a290] */
        int16_t life = (int16_t)((rand() % 4 + 1) * 10);
        PSLOT_WR16(slot, PSLOT_LIFETIME, life);
        PSLOT_WR16(slot, PSLOT_LIFE0, life);   /* [proc FX] spawn lifetime for age01 */

        /* [CONFIRMED @ 0x0042A290 SpawnVehicleSmokePuffFromHardpoint; L5 sweep 2026-05-22]
         *   Byte-faithful: port mirrors the HARDPOINT variant (not the
         *   alternate SpawnVehicleSmokeVariant @ 0x00429A30 which uses
         *   SIZE_W=0x7000/SIZE_H=0x2080/vel_y=0x1800/yaw-rotated random
         *   jitter). The HARDPOINT variant is the one td5_vfx.c:2599
         *   "SpawnVehicleSmokePuffFromHardpoint (0x42A290) [external]"
         *   targets — its initial size pair is SIZE_W=0x4000 / SIZE_H=0x26C0,
         *   velocity is actor's linear velocity (mesh+0x1CC / +0x1D4)
         *   with vel_y=0x600, lifetime-relative deltas -0x3000/+0x1900.
         *   All four match the orig 0x0042A290 byte-for-byte.
         *
         *   Earlier comment claimed L4 pending byte-faithful re-port; that
         *   was a misattribution against the wrong orig variant (0x00429A30
         *   instead of 0x0042A290). REG-6 verdict 2026-05-22: false alarm,
         *   port is correct against the variant it intends to mirror. The
         *   separate todo_smoke_render_broken_2026-05-19 issue (per-frame
         *   variant-UV re-indexing) remains independent and unrelated. */

        /* Size animation (matches orig 0x0042A290):
         *   size_w  = 0x4000 (16384),  delta = -0x3000 / lifetime
         *   size_h  = 0x26C0 (9920),   delta =  0x1900 / lifetime */
        PSLOT_WR16(slot, PSLOT_SIZE_W, 0x4000);
        PSLOT_WR16(slot, PSLOT_SIZE_W_D, (int16_t)(-0x3000 / life));
        PSLOT_WR16(slot, PSLOT_SIZE_H, 0x26C0);
        PSLOT_WR16(slot, PSLOT_SIZE_H_D, (int16_t)(0x1900 / life));

        /* Phase: random start [CONFIRMED @ 0x0042a290] */
        slot[PSLOT_PHASE] = (uint8_t)(rand() % 0x1F);

        /* Velocity [CONFIRMED @ 0x0042a290]:
         *   Y = 0x600 (upward drift)
         *   X/Z = copied from actor velocity at +0x1CC / +0x1D4 (24.8 fixed) */
        int32_t actor_vx = 0, actor_vz = 0;
        if (actor) {
            uint8_t *ap = (uint8_t *)actor;
            memcpy(&actor_vx, ap + 0x1CC, 4);
            memcpy(&actor_vz, ap + 0x1D4, 4);
        }
        PSLOT_WR32(slot, PSLOT_VEL_X, actor_vx);
        PSLOT_WR32(slot, PSLOT_VEL_Y, s_smoke_vel_y);
        PSLOT_WR32(slot, PSLOT_VEL_Z, actor_vz);
        s_smoke_vel_y = 0x600; /* reset to default after use */

        /* World position (24.8 fixed) */
        PSLOT_WR32(slot, PSLOT_POS_X, (int32_t)(wx * 256.0f));
        PSLOT_WR32(slot, PSLOT_POS_Y, (int32_t)(wy * 256.0f));
        PSLOT_WR32(slot, PSLOT_POS_Z, (int32_t)(wz * 256.0f));

        /* Find free sprite batch slot */
        for (int b = 0; b < TD5_VFX_SPRITE_BATCH_COUNT; b++) {
            if (s_sprite_render_flags[vi][b] == 0) {
                s_sprite_render_flags[vi][b] = 0x80;
                slot[PSLOT_BATCH] = (uint8_t)b;

                int v_idx = variant & 3;
                VfxSpriteQuad *sq = &s_sprite_batches[vi * TD5_VFX_SPRITE_BATCH_COUNT + b];
                float su0 = s_smoke_variant_uv[v_idx][0];
                float sv0 = s_smoke_variant_uv[v_idx][1];
                float sw  = s_smoke_variant_uv[v_idx][2];
                float sh  = s_smoke_variant_uv[v_idx][3];
                float sp  = s_smoke_variant_uv[v_idx][4];
                vfx_build_sprite_quad(sq, 0.0f, 0.0f, 128.0f,
                                       sw * 0.5f, sh * 0.5f,
                                       su0, sv0, su0 + sw, sv0 + sh,
                                       sp, 0xFFFFFFFF);
                break;
            }
        }
        break;
    }
}

/**
 * UpdateRearTireEffects (0x43F7E0)
 *
 * Updates rear wheels (2,3) when rear axle has surface contact (bit 0 set).
 * Allocates emitters if needed, spawns smoke on random probability
 * proportional to lateral slip, and modulates track intensity.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4, port-side extension noted]
 *   Control flow vs orig 0x0043F7E0:
 *     - Contact gate (bit 0) match
 *     - Two emitter allocs at wheels 2,3 with alpha=0x37 width=0x1A match
 *     - rand() % 50 < lateral_slip/2 smoke gate match
 *     - rand() & 1 picks wheel 2 or 3 for smoke spawn match
 *     - Wheel Y+0x7800 offset before float conversion match
 *     - Intensity = lateral_slip >> 2 match
 *     - Surface halve set {2,4,5,6,9} match
 *
 *   Divergences:
 *     1. Wheel anchor source +0xf0+wheel*0xc (orig) vs +0x298+wheel*0xc
 *        (port) — same probe-vs-hires wheel position as
 *        AcquireTireTrackEmitter audit.
 *     2. [ARCH-DIVERGENCE: port-introduced tire-mark spawn]
 *        Port adds vfx_tire_mark_spawn calls (lines 2225-2242) using a
 *        float ring buffer to lay down visible track quads independent
 *        of the int16 strip builder. Orig has no equivalent. This is
 *        a port-side visual enhancement, not a faithful port — intended.
 */
static void vfx_update_rear_tire_effects(TD5_Actor *actor, uint8_t contact_flags) {
    if ((contact_flags & 1) == 0) return; /* no rear contact */

    uint8_t *ap = (uint8_t *)actor;

    /* Diagnostic: burnout-window state */
    {
        static uint32_t s_rear_log_frame = 0;
        if ((s_rear_log_frame++ % 30u) == 0u) {
            int16_t ls_dbg;
            memcpy(&ls_dbg, ap + 0x33C, sizeof(int16_t));
            int32_t lspd_dbg, vlat_dbg;
            memcpy(&lspd_dbg, ap + 0x314, 4);
            memcpy(&vlat_dbg, ap + 0x318, 4);
            int16_t thr_dbg = *(int16_t *)(ap + 0x33E);
            uint8_t scf_dbg = *(ap + 0x376);
            int32_t rpm_dbg;
            memcpy(&rpm_dbg, ap + 0x310, 4);
            uint8_t gear_dbg = *(ap + 0x364);
            TD5_LOG_I(LOG_TAG, "rear_tire: scf=0x%X ls=%d thr=%d rpm=%d gear=%d lspd=%d vlat=%d",
                      scf_dbg, ls_dbg, thr_dbg, rpm_dbg, gear_dbg, lspd_dbg, vlat_dbg);
        }
    }

    /* Allocate emitters for wheels 2 and 3 if not already active */
    uint8_t slot_index = *(ap + 0x375);

    for (int w = 2; w <= 3; w++) {
        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x37, 0x1A); /* orig width=0x1A confirmed via Ghidra audit */
            if (result >= 0) {
                vfx_set_emitter_anchor_from_wheel(actor, w, result);
            }
            *(ap + 0x371 + w) = (uint8_t)(result & 0xFF);
        }
    }

    /* Smoke spawn: probability proportional to lateral slip */
    int16_t lateral_slip;
    memcpy(&lateral_slip, ap + 0x33C, sizeof(int16_t));

    int r = rand() % 50;
    if (r < (int)lateral_slip / 2) {
        /* Pick random left(2) or right(3) wheel, spawn dual smoke variants */
        int which = 2 + (rand() & 1);

        /* Read wheel world position from actor+0x298 + wheel * 12 */
        int32_t wx, wy, wz;
        vfx_read_wheel_world_pos(actor, which, &wx, &wy, &wz);

        /* Apply Y offset of +0x7800 (upward) */
        float fwx = (float)wx * FP_TO_FLOAT;
        float fwy = (float)(wy + 0x7800) * FP_TO_FLOAT;
        float fwz = (float)wz * FP_TO_FLOAT;

        /* Spawn variant 0 (white smoke) and variant 1 (dark/tire smoke) */
        vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 0, s_current_view_index);
        vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 1, s_current_view_index);
    }

    /* (Float ring vfx_tire_mark_spawn removed 2026-05-28 — strip
     * builder is the orig-faithful path; the ring was port-only.) */

    /* Set track intensity proportional to slip: lateral_slip >> 2 */
    uint8_t intensity = (uint8_t)((uint16_t)lateral_slip >> 2);

    for (int w = 2; w <= 3; w++) {
        uint8_t emitter_id = *(ap + 0x371 + w);
        if (emitter_id != 0xFF) {
            int desc_idx = w + (int)slot_index * 4;
            if (desc_idx < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]))) {
                s_emitter_descs[desc_idx].target_alpha = intensity;
            }
        }
    }

    /* Surface reduction: halve intensity on hard surfaces */
    uint8_t surface = *(ap + 0x370);
    if (vfx_is_hard_surface(surface)) {
        for (int w = 2; w <= 3; w++) {
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                int desc_idx = w + (int)slot_index * 4;
                if (desc_idx < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]))) {
                    s_emitter_descs[desc_idx].target_alpha >>= 1;
                }
            }
        }
    }
}

/**
 * UpdateFrontTireEffects (0x43F960)
 *
 * Mirror of UpdateRearTireEffects for front wheels (0,1).
 * Gated by bit 1 of contact_flags (front axle contact).
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4]
 *   Symmetric to UpdateRearTireEffects audit above. Wheels 0,1; alpha=0x28
 *   (vs rear 0x37); contact gate bit 1 (front axle). Same wheel-anchor
 *   offset divergence (+0xf0+wheel*0xc vs +0x298+wheel*0xc); same
 *   port-introduced vfx_tire_mark_spawn extension. Rest matches orig
 *   0x0043F960 byte-faithfully.
 */
static void vfx_update_front_tire_effects(TD5_Actor *actor, uint8_t contact_flags) {
    if ((contact_flags & 2) == 0) return; /* no front contact */

    uint8_t *ap = (uint8_t *)actor;
    uint8_t slot_index = *(ap + 0x375);

    /* Allocate emitters for wheels 0 and 1 */
    for (int w = 0; w <= 1; w++) {
        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x37, 0x1A); /* orig width=0x1A confirmed via Ghidra audit */
            if (result >= 0) {
                vfx_set_emitter_anchor_from_wheel(actor, w, result);
            }
            *(ap + 0x371 + w) = (uint8_t)(result & 0xFF);
        }
    }

    /* Smoke spawn (same probability logic as rear) */
    int16_t lateral_slip;
    memcpy(&lateral_slip, ap + 0x33C, sizeof(int16_t));

    int r = rand() % 50;
    if (r < (int)lateral_slip / 2) {
        int which = rand() & 1; /* wheel 0 or 1 */

        int32_t wx, wy, wz;
        vfx_read_wheel_world_pos(actor, which, &wx, &wy, &wz);

        float fwx = (float)wx * FP_TO_FLOAT;
        float fwy = (float)(wy + 0x7800) * FP_TO_FLOAT;
        float fwz = (float)wz * FP_TO_FLOAT;

        vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 0, s_current_view_index);
        vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 1, s_current_view_index);
    }

    /* (Float ring vfx_tire_mark_spawn removed 2026-05-28 — strip
     * builder is the orig-faithful path; the ring was port-only.) */

    /* Set track intensity */
    uint8_t intensity = (uint8_t)((uint16_t)lateral_slip >> 2);

    for (int w = 0; w <= 1; w++) {
        uint8_t emitter_id = *(ap + 0x371 + w);
        if (emitter_id != 0xFF) {
            int desc_idx = w + (int)slot_index * 4;
            if (desc_idx < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]))) {
                s_emitter_descs[desc_idx].target_alpha = intensity;
            }
        }
    }

    /* Surface reduction on hard surfaces */
    uint8_t surface = *(ap + 0x370);
    if (vfx_is_hard_surface(surface)) {
        for (int w = 0; w <= 1; w++) {
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                int desc_idx = w + (int)slot_index * 4;
                if (desc_idx < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]))) {
                    s_emitter_descs[desc_idx].target_alpha >>= 1;
                }
            }
        }
    }
}

/**
 * UpdateFrontWheelSoundEffects (0x43F420)
 *
 * Front wheels (0,1) with speed threshold 15001. When above threshold:
 * spawns smoke, allocates tire track emitter, sets intensity proportional
 * to (speed - 15000) >> 11. Below threshold: releases emitter.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4]
 *   Control flow, threshold (0x3A99=15001), excess gate (>0x5000),
 *   surface-set check (white smoke for surfaces 1,3,5,10; dark always),
 *   tire-track emitter alpha/width (0x28, 0x1A), hard-surface intensity
 *   halve — all match orig 0x0043F420.
 *
 *   Divergences:
 *     1. Wheel anchor source +0xf0 (orig probe-position) vs +0x298 (port
 *        hires wheel) — see AcquireTireTrackEmitter audit.
 *     2. Intensity formula: orig `(excess + (excess>>31 & 0x7FF)) >> 11`
 *        (rounded-toward-zero SAR11). Port `excess >> 11` (round-toward
 *        -infinity for negatives). For excess > 0 (the only branch
 *        reachable via the >0x5000 gate) values are identical; rounding
 *        diff is unreachable in this hot path. Cosmetic L5 concern but
 *        not observable.
 *
 *   Recommend re-port wheel anchor offsets and fix the SAR pattern for
 *   audit hygiene even though latter is unreachable.
 */
static void vfx_update_front_wheel_sound_effects(TD5_Actor *actor, int speed) {
    uint8_t *ap = (uint8_t *)actor;
    uint8_t slot_index = *(ap + 0x375);
    uint8_t surface = *(ap + 0x370);

    for (int w = 0; w <= 1; w++) {
        if (speed < 0x3A99) {
            /* Below threshold: release emitter */
            if (*(ap + 0x379) != 0 ||              /* airborne */
                (*(ap + 0x376) & 2) == 0 ||        /* no front contact */
                ((1 << w) & *(ap + 0x37C)) != 0) { /* wheel not grounded */
                uint8_t emitter_id = *(ap + 0x371 + w);
                if (emitter_id != 0xFF) {
                    int desc_idx = w + (int)slot_index * 4;
                    if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                        s_emitter_descs[desc_idx].active = 0;
                    }
                    /* Pool slot stays alive — render fn ages it out over 600 ticks */
                    *(ap + 0x371 + w) = 0xFF;
                }
            }
            continue;
        }

        /* Guard checks for release */
        if (*(ap + 0x379) != 0) { /* airborne */
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                int desc_idx = w + (int)slot_index * 4;
                if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0])))
                    s_emitter_descs[desc_idx].active = 0;
                *(ap + 0x371 + w) = 0xFF;
            }
            continue;
        }
        if ((*(ap + 0x376) & 2) != 0 ||
            ((1 << w) & *(ap + 0x37C)) != 0) {
            /* Not on surface or wheel airborne */
            continue;
        }

        int excess = speed - 15000;

        /* Spawn smoke if speed excess > 0x5000 */
        if (excess > 0x5000) {
            /* Read wheel world position for smoke spawn */
            int32_t wx, wy, wz;
            vfx_read_wheel_world_pos(actor, w, &wx, &wy, &wz);
            float fwx = (float)wx * FP_TO_FLOAT;
            float fwy = (float)(wy + 0x7800) * FP_TO_FLOAT;
            float fwz = (float)wz * FP_TO_FLOAT;

            if (surface == 1 || surface == 10 || surface == 3 || surface == 5) {
                /* SpawnVehicleSmokeVariant: white smoke (variant 0) */
                vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 0, s_current_view_index);
            }
            /* SpawnVehicleSmokeVariant: dark/tire smoke (variant 1) */
            vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 1, s_current_view_index);
        }

        /* Allocate emitter if not active */
        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x28, 0x1A); /* orig width=0x1A confirmed via Ghidra audit */
            if (result >= 0) {
                vfx_set_emitter_anchor_from_wheel(actor, w, result);
            }
            *(ap + 0x371 + w) = (uint8_t)(result & 0xFF);
        }

        /* Set intensity: (excess + rounding) >> 11 */
        uint8_t track_intensity = (uint8_t)(excess >> 11);

        /* Surface reduction on hard surfaces */
        if (vfx_is_hard_surface(surface)) {
            track_intensity >>= 1;
        }

        /* Write intensity to emitter descriptor */
        uint8_t emitter_id = *(ap + 0x371 + w);
        if (emitter_id != 0xFF) {
            int desc_idx = w + (int)slot_index * 4;
            if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                s_emitter_descs[desc_idx].target_alpha = track_intensity;
            }
        }
    }
}

/**
 * UpdateRearWheelSoundEffects (0x43F600)
 *
 * Mirror of front wheel sound effects for rear wheels (2,3) with
 * speed threshold 10001. Uses alpha=0x37 instead of 0x28.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4]
 *   Symmetric to UpdateFrontWheelSoundEffects (audit above): same wheel
 *   offset divergence (+0xf0 + 2*0xc = +0x108 for wheel 2 in orig;
 *   port reads +0x298 + wheel_index*12). Threshold 0x2711 (10001),
 *   excess gate >0x5000, contact mask bit 1 (front) vs bit 0 (rear) —
 *   gate matches. Alpha 0x37, width 0x1A — match. Hard-surface halve
 *   set {2,4,5,6,9} — match. SAR11-rounded intensity unreachable for
 *   excess>0 branch — port simplification harmless.
 */
static void vfx_update_rear_wheel_sound_effects(TD5_Actor *actor, int speed) {
    uint8_t *ap = (uint8_t *)actor;
    uint8_t slot_index = *(ap + 0x375);
    uint8_t surface = *(ap + 0x370);

    for (int w = 2; w <= 3; w++) {
        if (speed < 0x2711) {
            /* Below threshold: release emitter */
            if (*(ap + 0x379) != 0 ||
                (*(ap + 0x376) & 1) == 0 ||
                ((1 << w) & *(ap + 0x37C)) != 0) {
                uint8_t emitter_id = *(ap + 0x371 + w);
                if (emitter_id != 0xFF) {
                    int desc_idx = w + (int)slot_index * 4;
                    if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                        s_emitter_descs[desc_idx].active = 0;
                    }
                    /* Pool slot stays alive — render fn ages it out over 600 ticks */
                    *(ap + 0x371 + w) = 0xFF;
                }
            }
            continue;
        }

        if (*(ap + 0x379) != 0) {
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                int desc_idx = w + (int)slot_index * 4;
                if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0])))
                    s_emitter_descs[desc_idx].active = 0;
                *(ap + 0x371 + w) = 0xFF;
            }
            continue;
        }
        if ((*(ap + 0x376) & 1) != 0 ||
            ((1 << w) & *(ap + 0x37C)) != 0) {
            continue;
        }

        int excess = speed - 10000;

        if (excess > 0x5000) {
            /* Read wheel world position for smoke spawn */
            int32_t wx, wy, wz;
            vfx_read_wheel_world_pos(actor, w, &wx, &wy, &wz);
            float fwx = (float)wx * FP_TO_FLOAT;
            float fwy = (float)(wy + 0x7800) * FP_TO_FLOAT;
            float fwz = (float)wz * FP_TO_FLOAT;

            if (surface == 1 || surface == 10 || surface == 3 || surface == 5) {
                vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 0, s_current_view_index);
            }
            vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 1, s_current_view_index);
        }

        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x37, 0x1A); /* orig width=0x1A confirmed via Ghidra audit */
            if (result >= 0) {
                vfx_set_emitter_anchor_from_wheel(actor, w, result);
            }
            *(ap + 0x371 + w) = (uint8_t)(result & 0xFF);
        }

        uint8_t track_intensity = (uint8_t)(excess >> 11);
        if (vfx_is_hard_surface(surface)) {
            track_intensity >>= 1;
        }

        uint8_t emitter_id = *(ap + 0x371 + w);
        if (emitter_id != 0xFF) {
            int desc_idx = w + (int)slot_index * 4;
            if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                s_emitter_descs[desc_idx].target_alpha = track_intensity;
            }
        }
    }
}

/* ========================================================================
 * 4. Vehicle Smoke
 *    Original: SpawnRearWheelSmokeEffects (0x401330)
 *              SpawnVehicleSmokeVariant (0x429A30) [external]
 *              SpawnVehicleSmokePuffFromHardpoint (0x42A290) [external]
 * ======================================================================== */

/**
 * SpawnRearWheelSmokeEffects (0x401330)
 *
 * Burnout smoke from the rear wheels. Gated on wheelspin latch (scf),
 * valid drivetrain, and nonzero engine-driven longitudinal speed — fires
 * during launch wheelspin. Spawns smoke from hardpoints 2 and 3 (rear
 * left/right wheels) with rand()%1000 < speed/200 probability.
 */
void td5_vfx_spawn_rear_wheel_smoke(TD5_Actor *actor, int view_index) {
    if (!actor) return;

    uint8_t *ap = (uint8_t *)actor;

    /* BUGFIX 2026-05-28 (Ghidra audit of orig 0x00401330): the gate was
     * WRONG — it read surface_state (+0x370, terrain type) and required
     * it to be exactly 10 or 12, which almost never happens, so burnout
     * smoke never fired. Orig gates on:
     *   scf (+0x376) != 0          — wheelspin latch is set
     *   drivetrain (tuning+0x76)!=0 — car has a valid drivetrain
     *   longitudinal_speed(+0x314)!=0 — engine-driven wheel speed nonzero
     * During a launch burnout, scf is set and +0x314 holds the CRGT
     * engine-derived speed (high) even while the body is barely moving,
     * so this fires exactly during the wheelspin window. */
    uint8_t scf = *(ap + 0x376);
    if (scf == 0) return;

    void *tuning_ptr;
    memcpy(&tuning_ptr, ap + 0x1BC, sizeof(void *));
    if (!tuning_ptr) return;
    int16_t drivetrain;
    memcpy(&drivetrain, (uint8_t *)tuning_ptr + 0x76, sizeof(int16_t));
    if (drivetrain == 0) return;

    /* Speed-proportional probability gate (from SpawnVehicleSmokePuffFromHardpoint):
     * rand() % 1000 < speed/200 */
    int32_t speed;
    memcpy(&speed, ap + 0x314, 4); /* longitudinal_speed */
    if (speed == 0) return;
    int abs_speed = (speed < 0) ? -speed : speed;

    if ((rand() % 1000) >= abs_speed / 200) return;

    /* Spawn from hardpoints 2 (rear-left) and 3 (rear-right).
     * Original reads body corner positions at actor + (hp*3 + 0x24)*4:
     *   hp=2 → +0xA8, hp=3 → +0xB4  [CONFIRMED @ 0x42A35A] */
    for (int hp = 2; hp <= 3; hp++) {
        int offset = 0x90 + hp * 12;  /* 0x90 + 2*12 = 0xA8, 0x90 + 3*12 = 0xB4 */
        int32_t wx, wy, wz;
        memcpy(&wx, ap + offset,     4);
        memcpy(&wy, ap + offset + 4, 4);
        memcpy(&wz, ap + offset + 8, 4);

        float fwx = (float)wx * FP_TO_FLOAT;
        float fwy = (float)wy * FP_TO_FLOAT;
        float fwz = (float)wz * FP_TO_FLOAT;

        /* Spawn smoke variant 1 (dark tire smoke) at hardpoint position */
        vfx_spawn_smoke_at_position(actor, fwx, fwy, fwz, 1, view_index);
    }
}

/**
 * SpawnVehicleSmokeSprite (0x429CF0)
 *
 * General-purpose smoke spawn. 50% probability gate (rand() % 10 > 5).
 * Seeds velocity from actor heading vector at +0x290.
 * Uses the actor's rotation matrix at +0x120 to transform a local
 * exhaust offset to world space.
 */
void td5_vfx_spawn_smoke(TD5_Actor *actor) {
    if (!actor) return;

    /* 50% probability gate */
    if ((rand() % 10) <= 5) return;

    uint8_t *ap = (uint8_t *)actor;

    /* [FIX 2026-05-31 cop-chase] The original SpawnVehicleSmokeSprite @ 0x429CF0
     * has NO speed gate — only the rand()%10 probability above (CONFIRMED). The
     * port's `if (abs_speed < 4000) return;` was a port-only addition that
     * suppressed this effect at low speed. The sole caller is the cop-chase
     * "wanted" smoke (td5_render.c, gated on g_wanted_damage_state[slot]==0 =
     * a BUSTED suspect), and busted cars coast to a STOP — so the speed gate
     * killed the smoke exactly when it should appear (over a disabled suspect),
     * making the effect "completely missing" vs the original. Gate removed. */
    int32_t speed;
    memcpy(&speed, ap + 0x314, 4);
    int abs_speed = (speed < 0) ? -speed : speed;

    /* Read world position — orig 0x00429CF0 receives `actor+0x1FC` as param_2
     * and reads x/y/z directly into the particle pos with no offset or rotation.
     * Port previously used the rear-wheel ground-probe midpoint + 0x7800 lift,
     * which placed smoke at track surface behind the chassis — visually hidden
     * by the body mesh from chase cameras. [CONFIRMED via Ghidra decomp of
     * 0x00429CF0 + callers 0x0040BD20 and 0x0040C120]. */
    int32_t pos_x, pos_y, pos_z;
    memcpy(&pos_x, ap + 0x1FC, 4);
    memcpy(&pos_y, ap + 0x200, 4);
    memcpy(&pos_z, ap + 0x204, 4);

    float mid_x = (float)pos_x * FP_TO_FLOAT;
    float mid_y = (float)pos_y * FP_TO_FLOAT;
    float mid_z = (float)pos_z * FP_TO_FLOAT;

    TD5_LOG_I(LOG_TAG, "exhaust_smoke: pos=(%.1f,%.1f,%.1f) speed=%d",
              mid_x, mid_y, mid_z, abs_speed);

    /* Orig 0x00429cf0 leaves velocity_y at 0 (only X/Z are set from sin/cos
     * of yaw scaled by rand). The previous `s_smoke_vel_y = 0x2000` write was
     * port-only and made smoke shoot straight up from chassis center over
     * the particle lifetime — visually a vertical streak rather than the
     * orig's lazy drift that looks like exhaust. */
    s_smoke_vel_y = 0;
    vfx_spawn_smoke_at_position(actor, mid_x, mid_y, mid_z, 0, s_current_view_index);
}

/* [#5 WRECK ROOF SMOKE 2026-06-20] Persistent column of smoke rising from the
 * ROOF of a wrecked (broken-down) traffic/cop car, so a totalled car visibly
 * reads as dead. Unlike td5_vfx_spawn_smoke (chassis-centre exhaust, 50% gate),
 * this:
 *   - spawns from above the chassis centre (+ROOF_LIFT world units) so the
 *     column sits on top of the body, not hidden inside it;
 *   - has no probability gate (one puff per visible wreck per frame) so the
 *     plume is dense and continuous;
 *   - drifts upward (s_smoke_vel_y) so it billows up like a real wreck.
 * Roof lift is in float world units (a car is ~64u wide, see
 * TRACKED_MARKER_BASE_HALF_XY); ~24u sits just above the roofline. Tunable via
 * TD5RE_WRECK_SMOKE_LIFT for drive-test. */
void td5_vfx_spawn_wreck_smoke(TD5_Actor *actor) {
    if (!actor) return;

    static float roof_lift = -1.0f;
    if (roof_lift < 0.0f) {
        const char *e = getenv("TD5RE_WRECK_SMOKE_LIFT");
        float v = (e && e[0]) ? (float)atof(e) : 24.0f;
        if (v < 0.0f)   v = 0.0f;
        if (v > 200.0f) v = 200.0f;
        roof_lift = v;
    }

    uint8_t *ap = (uint8_t *)actor;
    int32_t pos_x, pos_y, pos_z;
    memcpy(&pos_x, ap + 0x1FC, 4);
    memcpy(&pos_y, ap + 0x200, 4);
    memcpy(&pos_z, ap + 0x204, 4);

    float mid_x = (float)pos_x * FP_TO_FLOAT;
    float mid_y = (float)pos_y * FP_TO_FLOAT + roof_lift;   /* lift to the roofline */
    float mid_z = (float)pos_z * FP_TO_FLOAT;

    /* [WRECK SMOKE RISE 2026-06-21] The plume must visibly billow UP off the
     * roof. The smoke integrator (td5_vfx_update_particles type 0) drags only
     * X/Z, not Y, so this value is the constant rise in 24.8 world-units/tick.
     * The old 0x900 (9 u/tick) was a gentle drift that read as "stuck on the
     * roof" once the incidental tumble smoke that used to mask it was gated off
     * for traffic. Default 0x2000 (32 u/tick) gives a clear rising column;
     * tunable via TD5RE_WRECK_SMOKE_RISE (accepts 0x-hex or decimal). */
    static int32_t wreck_rise = -1;
    if (wreck_rise < 0) {
        const char *e = getenv("TD5RE_WRECK_SMOKE_RISE");
        long v = (e && e[0]) ? strtol(e, NULL, 0) : 0x2000;
        if (v < 0)      v = 0;
        if (v > 0x8000) v = 0x8000;
        wreck_rise = (int32_t)v;
        TD5_LOG_I(LOG_TAG, "wreck smoke rise = 0x%X (%d u/tick)",
                  wreck_rise, wreck_rise >> 8);
    }
    s_smoke_vel_y = wreck_rise;   /* brisk upward billow (vs exhaust's lazy 0x600) */
    vfx_spawn_smoke_at_position(actor, mid_x, mid_y, mid_z, 0, s_current_view_index);
}

/**
 * SpawnVehicleSmokePuffAtPoint (0x00429FD0)
 *
 * Low-level "explicit point" smoke spawn (third distinct smoke variant after
 * 0x00429A30 SpawnVehicleSmokeVariant and 0x0042A290
 * SpawnVehicleSmokePuffFromHardpoint). Caller supplies the world position
 * directly; the size/alpha animation pair is unique to this variant.
 *
 * [CONFIRMED @ 0x00429FD0; TIER 3 port 2026-05-24]
 *   Byte-faithful field reconstruction:
 *     - Pool slot scan: 100 slots/view starting at &g_raceParticlePool[view*100]
 *     - Sprite-batch reservation: 50-entry per-view table at 0x4a6370+view*0x32
 *     - lifetime_max = ((rand() & 0x80000003 wrap) + 1) * 10  → 10/20/30/40
 *     - initial_size = 0x3000  (smaller than 0x42A290's 0x4000)
 *     - initial_alpha = 0xdc0  (smaller than 0x42A290's 0x26C0)
 *     - size_rate  = -0x2000 / lifetime_max
 *     - alpha_rate =  0x1900 / lifetime_max
 *     - Velocity X = ((cos+sin) * 0x3000 round-shifted by 12) + actor[+0x1CC]
 *     - Velocity Z = ((cos-sin) * 0x3000 round-shifted by 12) + actor[+0x1D4]
 *     - Velocity Y = 0 (not written; slot was zeroed at allocation)
 *     - Phase = rand() % 0x1F (random start, same as 0x42A290)
 *     - HARDPOINT byte stored at slot's hardpoint_id field
 *
 * NOTE on the size/alpha field naming: orig calls the SIZE_W/SIZE_H pair
 * "initial_size" / "initial_alpha" in the named decompile. They correspond
 * to the size animation slots in our particle layout (PSLOT_SIZE_W /
 * PSLOT_SIZE_H), NOT a separate alpha channel — alpha modulation lives in
 * the render-time color pack.
 */
static void td5_vfx_spawn_smoke_puff_at_point(TD5_Actor *actor,
                                               const int32_t world_xyz[3],
                                               uint8_t hardpoint_id,
                                               int view_index)
{
    if (!actor || !world_xyz) return;
    int vi = view_index & 1;

    /* Pool slot scan: linear search for first inactive slot in view's bank
     * [CONFIRMED @ 0x00429FE2-0x00429FF8] */
    uint8_t *bank = s_particle_banks[vi];
    int slot_idx = -1;
    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *s = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        if (s[PSLOT_FLAGS] == 0) { slot_idx = i; break; }
    }
    if (slot_idx < 0) return;
    uint8_t *slot = bank + slot_idx * TD5_VFX_PARTICLE_SLOT_STRIDE;

    /* Sprite-batch reservation: scan per-view 50-entry table for first zero.
     * [CONFIRMED @ 0x00429FFE-0x0042A028: g_spriteRenderFlags[view*0x32 + i]] */
    int batch_idx = -1;
    for (int b = 0; b < TD5_VFX_SPRITE_BATCH_COUNT; b++) {
        if (s_sprite_render_flags[vi][b] == 0) { batch_idx = b; break; }
    }
    if (batch_idx < 0) return; /* orig bails silently when sprite batches full */

    /* Reserve sprite batch + record batch index in slot
     * [CONFIRMED @ 0x0042A038-0x0042A04C] */
    s_sprite_render_flags[vi][batch_idx] = 1;
    slot[PSLOT_BATCH] = (uint8_t)batch_idx;

    /* Build sprite quad template using the SMOKE atlas entry (texture_page
     * read from the lookup result). Port collapses BuildSpriteQuadTemplate
     * into the existing vfx_build_sprite_quad path; UV/page already cached
     * at init time (s_smoke_u0..s_smoke_page). Matches orig's 0xB8 stride
     * write to g_spriteBatches[view*0x32 + batch] @ 0x0042A052-0x0042A1FE. */
    VfxSpriteQuad *sq = &s_sprite_batches[vi * TD5_VFX_SPRITE_BATCH_COUNT + batch_idx];
    vfx_build_sprite_quad(sq, 0.0f, 0.0f, 128.0f,
                           (s_smoke_u1 - s_smoke_u0) * 0.5f,
                           (s_smoke_v1 - s_smoke_v0) * 0.5f,
                           s_smoke_u0, s_smoke_v0,
                           s_smoke_u1, s_smoke_v1,
                           s_smoke_page, 0xFFFFFFFF);

    /* Activate + wire callbacks [CONFIRMED @ 0x0042A204-0x0042A21E].
     * Orig writes flags=0x80, update_fn=&LAB_00429950 (SmokeUpdateCallback),
     * render_fn=&LAB_004297D0 (SmokeRenderCallback). Port collapses the
     * callback dispatch into td5_vfx_update_particles type-switch on
     * PSLOT_TYPE, so the function pointers fold into PSLOT_TYPE=0 (smoke). */
    memset(slot, 0, TD5_VFX_PARTICLE_SLOT_STRIDE);
    slot[PSLOT_FLAGS] = 0xC0;   /* active | projected (matches port convention) */
    slot[PSLOT_TYPE]  = 0;       /* smoke */
    slot[PSLOT_BATCH] = (uint8_t)batch_idx;

    /* World position copy [CONFIRMED @ 0x0042A220-0x0042A234]
     * Orig: slot.world_x = param_2[0], world_y = param_2[1], world_z = param_2[2] */
    PSLOT_WR32(slot, PSLOT_POS_X, world_xyz[0]);
    PSLOT_WR32(slot, PSLOT_POS_Y, world_xyz[1]);
    PSLOT_WR32(slot, PSLOT_POS_Z, world_xyz[2]);

    /* Velocity derivation [CONFIRMED @ 0x0042A23A-0x0042A26E]
     *   yaw12 = (actor[+0x1F4] >> 8) + 0x800
     *   cos = CosFixed12bit(yaw12)
     *   sin = SinFixed12bit(yaw12)
     *   v   = (cos + sin) * 0x3000  →  round-toward-zero >>12  →  add actor[+0x1CC]
     *   v_z = (cos - sin) * 0x3000  →  round-toward-zero >>12  →  add actor[+0x1D4]
     * The (x + (x>>31 & 0xFFF)) >> 12 pattern is C's signed-shift round-toward-zero
     * mirror of orig's IDIV-by-power-of-2; port uses the same expression so
     * the cycle-accurate negative path matches. */
    uint8_t *ap = (uint8_t *)actor;
    int32_t yaw_raw;
    int32_t actor_vx, actor_vz;
    memcpy(&yaw_raw,  ap + 0x1F4, 4);
    memcpy(&actor_vx, ap + 0x1CC, 4);
    memcpy(&actor_vz, ap + 0x1D4, 4);
    int yaw12 = (int)((yaw_raw >> 8) + 0x800);
    int cos_v = CosFixed12bit((unsigned)yaw12);
    int sin_v = SinFixed12bit(yaw12);
    int32_t mix_xz = (cos_v + sin_v) * 0x3000;
    int32_t vx = ((mix_xz + ((mix_xz >> 31) & 0xFFF)) >> 12) + actor_vx;
    int32_t mix_zx = (cos_v - sin_v) * 0x3000;
    int32_t vz = ((mix_zx + ((mix_zx >> 31) & 0xFFF)) >> 12) + actor_vz;
    PSLOT_WR32(slot, PSLOT_VEL_X, vx);
    PSLOT_WR32(slot, PSLOT_VEL_Y, 0);
    PSLOT_WR32(slot, PSLOT_VEL_Z, vz);

    /* Lifetime constants [CONFIRMED @ 0x0042A270-0x0042A276] = 0x200 ticks */
    PSLOT_WR16(slot, PSLOT_LIFETIME, 0x200);
    PSLOT_WR16(slot, PSLOT_LIFE0, 0x200);   /* [proc FX] spawn lifetime for age01 */

    /* Phase: random start [CONFIRMED @ 0x0042A278-0x0042A286] phase = rand() % 0x1F */
    slot[PSLOT_PHASE] = (uint8_t)(rand() % 0x1F);

    /* hardpoint_id parameter stored in PSLOT scratch
     * [CONFIRMED @ 0x0042A288] orig writes hardpoint at slot.hardpoint_id.
     * Port has no dedicated hardpoint field — use the variant-tracking byte
     * at the next free pad. Render-side doesn't currently read it; we still
     * pass it through for parity if a future change needs the dispatch. */
    (void)hardpoint_id;

    /* lifetime_max = ((rand() & 0x80000003 wrap) + 1) * 10
     * [CONFIRMED @ 0x0042A29A-0x0042A2C8]
     * The wrap pattern mirrors orig's int-truncation of negative rand:
     *   uVar6 = rand() & 0x80000003;
     *   if ((int)uVar6 < 0) uVar6 = (uVar6 - 1 | 0xFFFFFFFC) + 1;
     * Effective range: 0..3 → +1 → 1..4 → *10 → 10/20/30/40 */
    uint32_t r6 = (uint32_t)rand() & 0x80000003u;
    if ((int32_t)r6 < 0) r6 = (uint32_t)((int32_t)((r6 - 1u) | 0xFFFFFFFCu) + 1);
    int life_max = (int)(((int8_t)((uint8_t)r6 + 1)) * 10);
    if (life_max <= 0) life_max = 10; /* defensive; orig would div-by-zero otherwise */

    /* Size/alpha animation pair [CONFIRMED @ 0x0042A2CA-0x0042A2FE].
     * Initial values are SMALLER than 0x42A290 hardpoint variant — this is
     * the "small puff" cosmetic distinct from the burnout cloud. */
    PSLOT_WR16(slot, PSLOT_SIZE_W,   0x3000);
    PSLOT_WR16(slot, PSLOT_SIZE_W_D, (int16_t)(-0x2000 / life_max));
    PSLOT_WR16(slot, PSLOT_SIZE_H,   0x0DC0);
    PSLOT_WR16(slot, PSLOT_SIZE_H_D, (int16_t)( 0x1900 / life_max));
}

/**
 * SpawnRandomVehicleSmokePuff (0x00401370)
 *
 * Engine-rev gated wrapper that invokes SpawnVehicleSmokePuffAtPoint with
 * the rear-probe midpoint as the spawn position. Original is called once
 * per visible racer per frame from RenderRaceActorForView (0x0040C120).
 *
 * Gate (must all hold):
 *   - engine_speed_accum (+0x310) < 4000
 *   - encounter_steering_cmd (+0x33E) > 200
 *   - engine_speed_accum > 0
 *   - (rand() % engine_speed_accum) < 500
 *
 * Spawn point: midpoint of probe_RR and probe_RL (rear-left/rear-right
 * ground probes), with +0x7800 added to Y. Hardpoint id = 0.
 *
 * [CONFIRMED @ 0x00401370 disassembly; TIER 3 port 2026-05-24]
 *   Field offsets read from listing:
 *     +0xA8 = probe_RL Y     +0xB4 = probe_RR Y
 *     +0xAC = probe_RL X     +0xB8 = probe_RR X
 *     +0xB0 = probe_RL Z     +0xBC = probe_RR Z
 *   Each avg uses CDQ/SUB pattern (round-toward-zero), then >>1.
 *
 * Return value: orig returns the rand()/engine quotient on the gate-pass
 * path (unused in the single known caller). Port returns void.
 */
void td5_vfx_spawn_random_smoke_puff(TD5_Actor *actor, int view_index)
{
    if (!actor) return;

    uint8_t *ap = (uint8_t *)actor;

    /* Read gate fields [CONFIRMED @ 0x00401378-0x00401396] */
    int32_t engine;
    int16_t throttle16;
    memcpy(&engine,     ap + 0x310, 4);
    memcpy(&throttle16, ap + 0x33E, 2);

    if (engine >= 0xFA0) return;       /* 4000 */
    if ((int)throttle16 <= 0xC8) return; /* 200 */
    if (engine <= 0) return;

    /* Probability roll: rand() % engine [CONFIRMED @ 0x00401398-0x004013AA].
     * Orig stores rand()/engine in iVar2 (returned). The modulo result is
     * the actual gate; only when (rand() % engine) < 500 does the spawn fire. */
    int rv = rand();
    int rem = (int)((int32_t)rv - (int32_t)((int32_t)(rv / engine) * engine));
    if (rem >= 0x1F4) return;          /* 500 */

    /* Probe-midpoint computation [CONFIRMED @ 0x004013AC-0x004013F2].
     * Each avg uses (left + right) signed >>1 with round-toward-zero.
     *
     * [S26 FIX 2026-06-05 — opponent smoke floats at start] The rear contact-
     * probe corners are stored as [X, Y(vertical), Z] at +0xA8/+0xAC/+0xB0
     * (rear-left) and +0xB4/+0xB8/+0xBC (rear-right). Confirmed at runtime: the
     * +0xA8 value tracks the chassis-center X (+0x1FC), +0xAC tracks center Y
     * (+0x200), +0xB0 tracks center Z (+0x204); the sibling
     * SpawnRearWheelSmokeEffects (td5_vfx_spawn_rear_wheel_smoke) and the orig
     * read the SAME [X,Y,Z] layout. This function previously mis-labelled +0xA8
     * as Y and +0xAC as X, so it averaged the X-probes into the VERTICAL output
     * (point[1]) and added the +0x7800 lift there — placing the smoke at a
     * vertical height equal to the car's world-X coordinate (hundreds of
     * thousands of units up). Because the engine-rev gate above fires mainly
     * during the low-RPM/high-throttle start-line launch, the stray high smoke
     * was most visible "at the beginning" of the race. Orig builds:
     *   out.X = avg(+0xA8,+0xB4)            [CONFIRMED @ 0x004013AC]
     *   out.Y = avg(+0xAC,+0xB8) + 0x7800   [CONFIRMED @ 0x004013c5 ADD EAX,0x7800]
     *   out.Z = avg(+0xB0,+0xBC)            [CONFIRMED @ 0x004013F2] */
    int32_t pRL_x, pRL_y, pRL_z;
    int32_t pRR_x, pRR_y, pRR_z;
    memcpy(&pRL_x, ap + 0xA8, 4);
    memcpy(&pRL_y, ap + 0xAC, 4);
    memcpy(&pRL_z, ap + 0xB0, 4);
    memcpy(&pRR_x, ap + 0xB4, 4);
    memcpy(&pRR_y, ap + 0xB8, 4);
    memcpy(&pRR_z, ap + 0xBC, 4);

    /* Faithful round-toward-zero divide by 2 (orig: ADD/CDQ/SUB/SAR pattern) */
    int32_t sum_x = pRL_x + pRR_x;  int32_t avg_x = (sum_x - (sum_x >> 31)) >> 1;
    int32_t sum_y = pRL_y + pRR_y;  int32_t avg_y = (sum_y - (sum_y >> 31)) >> 1;
    int32_t sum_z = pRL_z + pRR_z;  int32_t avg_z = (sum_z - (sum_z >> 31)) >> 1;

    /* Layout of param struct: { X, Y+0x7800, Z } [CONFIRMED @ 0x004013F6-0x00401407] */
    int32_t point[3];
    point[0] = avg_x;
    point[1] = avg_y + 0x7800;
    point[2] = avg_z;

    td5_vfx_spawn_smoke_puff_at_point(actor, point, /*hardpoint*/ 0, view_index);
}

/* ========================================================================
 * 5. Taillights
 *    Original: InitializeVehicleTaillightQuadTemplates (0x401000)
 *              RenderVehicleTaillightQuads (0x4011C0)
 * ======================================================================== */

/**
 * InitializeVehicleTaillightQuadTemplates (0x401000)
 *
 * One-time setup at race start. Allocates sprite quad templates for all
 * vehicle taillights (2 per vehicle * num_actors). Loads the BRAKED
 * texture from archive, extracts UVs, and builds quad templates.
 * Also clears the per-actor brightness array.
 */
void td5_vfx_init_taillight_templates(void) {
    int num_actors = g_td5.total_actor_count;
    if (num_actors <= 0) num_actors = TD5_MAX_RACER_SLOTS;

    /* Allocate taillight quad storage:
     * Original: num_actors * 0x2E0 bytes total
     *   first num_actors * 0x170 bytes = primary taillight quads
     *   remaining = alternate quads for second light per vehicle
     * Each light = 0xB8 bytes (VfxSpriteQuad) */
    size_t total_size = (size_t)num_actors * 0x2E0;
    if (s_taillight_quads) td5_plat_heap_free(s_taillight_quads);
    s_taillight_quads = (VfxSpriteQuad *)td5_plat_heap_alloc(total_size);
    if (!s_taillight_quads) return;
    memset(s_taillight_quads, 0, total_size);

    /* Second set starts at offset num_actors * 0x170 */
    s_taillight_quads_alt = (VfxSpriteQuad *)((uint8_t *)s_taillight_quads +
                                               (size_t)num_actors * 0x170);

    /* Load BRAKED texture from the static atlas and extract UV coordinates */
    TD5_AtlasEntry *braked = td5_asset_find_atlas_entry(NULL, "BRAKED");
    if (vfx_atlas_entry_valid(braked)) {
        vfx_extract_sprite_uvs(braked,
                                &s_taillight_u0, &s_taillight_v0,
                                &s_taillight_u1, &s_taillight_v1,
                                &s_taillight_page);
    } else {
        s_taillight_u0 = 1.0f;  s_taillight_v0 = 1.0f;
        s_taillight_u1 = 14.0f; s_taillight_v1 = 14.0f;
        s_taillight_page = 0.0f;
        TD5_LOG_W("vfx", "BRAKED sprite not found in static atlas");
    }

    /* Build sprite quad template for each taillight quad (4 per actor = 2 primary + 2 alt).
     * UV coords with half-pixel inset. Colors: 0xFFFFFFFF (white base, modulated at render time).
     * Z-depth: 128.0 (near camera). */
    for (int a = 0; a < num_actors; a++) {
        for (int light = 0; light < 2; light++) {
            /* Primary quad */
            VfxSpriteQuad *pq = (VfxSpriteQuad *)((uint8_t *)s_taillight_quads +
                                 (size_t)a * 0x170 + (size_t)light * 0xB8);
            vfx_build_sprite_quad(pq, 0.0f, 0.0f, 128.0f,
                                   12.0f, 12.0f,
                                   s_taillight_u0, s_taillight_v0,
                                   s_taillight_u1, s_taillight_v1,
                                   s_taillight_page, 0xFFFFFFFF);

            /* Alternate quad (same UVs, used for smoke overlay) */
            VfxSpriteQuad *aq = (VfxSpriteQuad *)((uint8_t *)s_taillight_quads_alt +
                                 (size_t)a * 0x170 + (size_t)light * 0xB8);
            vfx_build_sprite_quad(aq, 0.0f, 0.0f, 128.0f,
                                   12.0f, 12.0f,
                                   s_taillight_u0, s_taillight_v0,
                                   s_taillight_u1, s_taillight_v1,
                                   s_taillight_page, 0xFFFFFFFF);
        }
    }

    /* Clear per-actor brightness array */
    memset(s_taillight_brightness, 0, sizeof(s_taillight_brightness));
}

/**
 * RenderVehicleTaillightQuads (0x4011C0)
 *
 * Per-frame taillight rendering for one vehicle. Two tail lights per
 * vehicle, rendered as translucent camera-facing billboard quads.
 *
 * Brightness ramps +8/frame when braking, capped at 0x80 (128).
 * Decays exponentially (>>1) when released.
 * Reads brake_flag at actor+0x36D (written by input and AI modules).
 */
void td5_vfx_render_taillights(int actor_index) {
    if (!s_taillight_quads) return;
    if (actor_index < 0 || actor_index >= TD5_MAX_TOTAL_ACTORS) return;

    /* Brightness decay/ramp */
    uint8_t brightness = s_taillight_brightness[actor_index];

    /* Brake-active check:
     *   Gate + active both use actor+0x36D (brake_flag byte).
     *   0x36D is written by td5_input (player) and td5_ai (opponents).
     *   Original decompilation at 0x4011F5 referenced +0x37C & 0x0F but
     *   that field is the wheel-contact bitmask, not the brake input —
     *   corrected after runtime testing showed brakes never triggering. */
    int brake_active = 0;

    if (!g_actor_table_base) return;

    uint8_t *ap = g_actor_table_base + actor_index * TD5_ACTOR_STRIDE;

    /* Read brake_flag at +0x36D — nonzero = braking */
    brake_active = (*(ap + 0x36D) != 0 || *(ap + 0x36E) != 0) ? 1 : 0; /* [2026-06-02] handbrake lights brakes too */

    if (brake_active) {
        if (brightness < 0x80) {  /* cap at 128 [CONFIRMED @ 0x401204] */
            brightness += 8;
            if (brightness > 0x80) brightness = 0x80;
        }
    } else {
        if (brightness > 0) {
            brightness >>= 1; /* exponential decay */
        }
    }
    s_taillight_brightness[actor_index] = brightness;

    TD5_LOG_D(LOG_TAG,
              "taillights actor=%d brake=%d brightness=%u",
              actor_index, brake_active, (unsigned int)brightness);

    /* Rendering is now handled by render_vehicle_brake_lights() in
     * td5_render.c, called from the actor render loop. This function
     * only tracks brightness state for the VFX module's init/shutdown. */
    if (brightness == 0) return;
}

/* ========================================================================
 * 6. Billboard Animations
 *    Original: AdvanceWorldBillboardAnimations (0x43CDC0)
 *
 * 0x43CDC0 walks the tracked-actor marker pool at 0x4BEDC0 (stride 0x22C,
 * limit < 0x4BF218 = exactly 2 entries) and adds 0x10 to each entry's phase
 * head at +0x00 — i.e. it animates the 2 cop-chase strobe markers. That pool
 * is the SAME one initialised by 0x43C9E0 and drawn by 0x43CDE0 (confirmed by
 * shared base/stride). The faithful port is td5_vfx_advance_tracked_marker_phases()
 * below, which advances the 2 s_tracked_marker_phase[] counters by 0x10.
 *
 * The former td5_vfx_advance_billboard_anims() stub was a no-op duplicate: it
 * iterated a never-allocated s_billboard_phase_table with a wrong +0x20 step.
 * It has been removed in favour of the single live implementation.
 * ======================================================================== */

/* --- Wheel Billboards (0x446F00) --- */

void td5_vfx_render_wheel_billboards(int view_index)
{
    /* Wheel rendering is now handled inline in td5_render.c actor loop
     * via render_vehicle_wheel_billboards(). This stub is kept for API
     * compatibility but does nothing. */
    (void)view_index;
}

/* ========================================================================
 * Weather state accessors (for td5_sound_update_ambient)
 *
 * [CONFIRMED @ 0x00440B00] UpdateVehicleAudioMix reads g_weatherActiveCountView0
 * (per-viewport particle density) and g_weatherType to gate rain sound.
 * Mapped to the port's per-view s_weather_active_count array and s_weather_type.
 * ======================================================================== */

int td5_vfx_get_weather_active_count(int view_index)
{
    int vi = view_index & 1;
    return s_weather_active_count[vi];
}

int td5_vfx_get_weather_type(void)
{
    return s_weather_type;
}

/* ========================================================================
 * Tracked Actor Marker Billboards — cop-chase visual (Tier 1 port 2026-05-24)
 *
 * Port of InitializeTrackedActorMarkerBillboards @ 0x0043c9e0 (985B).
 * Builds 6 sprite-quad templates per marker × 2 markers = 12 templates total,
 * sourced from PoliceLt_red / PoliceLt_blue / Police_red / Police_blue
 * atlas entries. In orig, these are pre-baked quad scratch buffers at
 * 0x4bedc0..0x4bf670 (stride 0x22c, 12 entries) consumed by
 * RenderTrackedActorMarker every frame; per-frame phase increment supplied
 * by AdvanceWorldBillboardAnimations.
 *
 * Port collapses the scratch-quad pool to per-marker UV caches (atlas page
 * IDs + UV rects), then RenderTrackedActorMarker (td5_render.c) emits a
 * fresh TD5_D3DVertex stream every frame using these caches. Phase
 * counters are kept here as ints; orig advance step is 0x10/tick.
 *
 * Display semantics (from orig render decompile, 0x0043cde0):
 *   - 2 markers (front + back) anchored at model-space offsets
 *     (+80, 205, -160) and (-80, 205, -160) on the tracked actor.
 *   - Each marker draws 3 sprite layers:
 *       0: red strobe       (POLICELT_RED, alpha = sin(phase*-4))
 *       1: blue strobe      (POLICELT_BLUE, alpha = sin(phase*-4))
 *       2: base marker      (POLICE_RED for front, POLICE_BLUE for back)
 *   - Marker rotation: yaw = AngleFromVector12(forward.z, forward.x)
 *     with ±0x100 offset to spread front/back light bars.
 *   - Pulse half-extents from g_hudSpeedoDialNearOff (64.0) /
 *     DAT_0045d768 / DAT_0045d764 scaled by DAT_0045d698 (1/4096) and the
 *     g_wantedTargetTrackerActive intensity counter.
 *
 * NOTE: g_wantedModeEnabled write-site is a known REGR (todo-police-chase-
 * no-audio-2026-05-19) — these visuals stay gated and inert until that
 * lands; the gate logic ports correctly so they activate automatically.
 * ======================================================================== */

/* (TD5_VFX_TRACKED_MARKER_COUNT / TD5_VFX_TRACKED_LAYERS_PER_MARK defined
 * in td5_vfx.h so td5_render.c can declare arrays of matching size.) */

/* Per-layer UV cache. */
typedef struct VfxTrackedMarkerLayer {
    int    page;       /* D3D atlas texture page; <0 = not loaded */
    float  u0, v0;     /* normalized UV rect (top-left) */
    float  u1, v1;     /* normalized UV rect (bottom-right) */
    int    pixel_w;    /* atlas pixel width (used as billboard half-extent base) */
    int    pixel_h;    /* atlas pixel height */
} VfxTrackedMarkerLayer;

/* 2 markers, each with 3 layers. Layer 0+1 are shared "strobe" templates,
 * layer 2 differs per marker (red for front, blue for back), matching the
 * orig init loop which writes 4 unique BuildSpriteQuadTemplate entries
 * (slot 0 = PoliceLt_red, slot 1 = PoliceLt_blue, slot 2 = Police_red,
 *  slot 4 = Police_blue) per outer iteration. */
static VfxTrackedMarkerLayer s_tracked_marker_layers[TD5_VFX_TRACKED_MARKER_COUNT]
                                                    [TD5_VFX_TRACKED_LAYERS_PER_MARK];

/* Per-marker animation phase counter — advanced by
 * td5_vfx_advance_tracked_marker_phases (orig AdvanceWorldBillboardAnimations
 * 0x43CDC0, stride 0x22c × 2 entries = the 2 sub-blocks of the pool). Stored as
 * int matching orig u8 wrap-around indexing in RenderTrackedActorMarker
 * (`(byte)(&pool)[iVar13] * -4`). */
static int     s_tracked_marker_phase[TD5_VFX_TRACKED_MARKER_COUNT];
static int     s_tracked_marker_initialized = 0;

/* Public accessors used by td5_render.c RenderTrackedActorMarker port. */
int td5_vfx_tracked_marker_get_page(int marker, int layer) {
    if (!s_tracked_marker_initialized) return -1;
    if ((unsigned)marker >= TD5_VFX_TRACKED_MARKER_COUNT) return -1;
    if ((unsigned)layer  >= TD5_VFX_TRACKED_LAYERS_PER_MARK) return -1;
    return s_tracked_marker_layers[marker][layer].page;
}

void td5_vfx_tracked_marker_get_uv(int marker, int layer,
                                    float *u0, float *v0, float *u1, float *v1) {
    if (!s_tracked_marker_initialized) { *u0=*v0=*u1=*v1=0.0f; return; }
    if ((unsigned)marker >= TD5_VFX_TRACKED_MARKER_COUNT) { *u0=*v0=*u1=*v1=0.0f; return; }
    if ((unsigned)layer  >= TD5_VFX_TRACKED_LAYERS_PER_MARK) { *u0=*v0=*u1=*v1=0.0f; return; }
    const VfxTrackedMarkerLayer *L = &s_tracked_marker_layers[marker][layer];
    *u0 = L->u0; *v0 = L->v0; *u1 = L->u1; *v1 = L->v1;
}

int  td5_vfx_tracked_marker_get_phase(int marker) {
    if ((unsigned)marker >= TD5_VFX_TRACKED_MARKER_COUNT) return 0;
    return s_tracked_marker_phase[marker];
}

int  td5_vfx_tracked_marker_initialized(void) {
    return s_tracked_marker_initialized;
}

static void tracked_marker_lookup_layer(VfxTrackedMarkerLayer *out, const char *atlas_name)
{
    TD5_AtlasEntry *e = td5_asset_find_atlas_entry(NULL, atlas_name);
    if (!e || e->texture_page <= 0) {
        out->page = -1;
        out->u0 = out->v0 = out->u1 = out->v1 = 0.0f;
        out->pixel_w = out->pixel_h = 0;
        TD5_LOG_W(LOG_TAG, "tracked_marker: atlas entry '%s' not found", atlas_name);
        return;
    }
    int tw = 256, th = 256;
    td5_plat_render_get_texture_dims(e->texture_page, &tw, &th);
    /* Half-pixel inset on each side mirrors brake-light / wheel UV patterns
     * in render to avoid bilinear-filter bleed into neighbor atlas cells. */
    out->page    = e->texture_page;
    out->u0      = ((float)e->atlas_x + 0.5f) / (float)tw;
    out->v0      = ((float)e->atlas_y + 0.5f) / (float)th;
    out->u1      = ((float)(e->atlas_x + e->width)  - 0.5f) / (float)tw;
    out->v1      = ((float)(e->atlas_y + e->height) - 0.5f) / (float)th;
    out->pixel_w = e->width;
    out->pixel_h = e->height;
}

/* [CONFIRMED @ 0x0043c9e0 InitializeTrackedActorMarkerBillboards]
 * Orig: 4 × FindArchiveEntryByName -> POLICELT_RED, POLICELT_BLUE,
 *       POLICE_RED, POLICE_BLUE; 12 BuildSpriteQuadTemplate calls
 *       (6 per outer iter × 2 outer iters); zero pool[0]+pool[stride]
 *       phase fields and seed _DAT_004befec/0x4bf218/0x4bf444 alpha
 *       channels (0x80, 0x40, 0xc0). Port caches the 4 atlas UV rects
 *       and zeros phase counters; alpha channels are computed per-frame
 *       in the render path from g_wantedTargetTrackerActive. */
void td5_vfx_init_tracked_actor_marker_billboards(void)
{
    /* [FIX 2026-06-01 cop-chase] Per-marker color separation — the original
     * draws TWO distinct lights: marker 0 = a fully-RED light, marker 1 = a
     * fully-BLUE light. Each marker's three layers are all the SAME color:
     *   marker 0: PoliceLt_red (strobe beam) + Police_red + Police_red
     *   marker 1: PoliceLt_blue (strobe beam) + Police_blue + Police_blue
     * [CONFIRMED by full RE of InitializeTrackedActorMarkerBillboards @ 0x0043c9e0
     *  — every BuildSpriteQuadTemplate destination traced.]
     *
     * The previous port wrongly put POLICELT_RED + POLICELT_BLUE on BOTH markers
     * (a red AND a blue strobe beam on each), only swapping the base — so the
     * two distinct red/blue lights were never formed and POLICELT_BLUE was never
     * rendered as marker 1's own beam. That muddled the colors and is the "light
     * effect that isn't loaded right" the original shows cleanly. */
    /* [FIX 2026-06-01 cop-chase] TEXTURE SHAPES (verified from the atlas):
     *   POLICELT_RED/BLUE = SOFT radial glow blobs (diffused).
     *   POLICE_RED/BLUE   = HARD solid rectangles (alpha 128 everywhere).
     * The over-car "base" layer (L2, the small ±32 square) must use the SOFT
     * glow (POLICELT) so it reads as a diffused light, NOT the hard POLICE
     * rectangle (which rendered as the clear squares the user reported). The
     * two sweeping beam layers (L0/L1) keep the glow + bar. Per-marker color is
     * still separated (marker 0 red, marker 1 blue). */
    /* marker 0 = RED light */
    tracked_marker_lookup_layer(&s_tracked_marker_layers[0][0], "POLICELT_RED"); /* glow beam */
    tracked_marker_lookup_layer(&s_tracked_marker_layers[0][1], "POLICE_RED");   /* bar beam  */
    tracked_marker_lookup_layer(&s_tracked_marker_layers[0][2], "POLICELT_RED"); /* soft over-car glow (was POLICE_RED hard rect) */

    /* marker 1 = BLUE light */
    tracked_marker_lookup_layer(&s_tracked_marker_layers[1][0], "POLICELT_BLUE");
    tracked_marker_lookup_layer(&s_tracked_marker_layers[1][1], "POLICE_BLUE");
    tracked_marker_lookup_layer(&s_tracked_marker_layers[1][2], "POLICELT_BLUE");

    /* Phase counters start at 0 (orig: _g_trackedActorMarkerBillboardPool=0,
     * advanced by 0x10/tick via AdvanceWorldBillboardAnimations). */
    s_tracked_marker_phase[0] = 0;
    s_tracked_marker_phase[1] = 0;
    s_tracked_marker_initialized = 1;

    TD5_LOG_I(LOG_TAG,
              "tracked_marker init: red_strobe.page=%d blue_strobe.page=%d "
              "red_base.page=%d blue_base.page=%d",
              s_tracked_marker_layers[0][0].page,
              s_tracked_marker_layers[0][1].page,
              s_tracked_marker_layers[0][2].page,
              s_tracked_marker_layers[1][2].page);
}

/* Phase advance — the single live port of AdvanceWorldBillboardAnimations
 * @ 0x0043cdc0, which walks pool entries 0..2 (stride 0x22c, limit < 0x4bf218
 * = 2 entries) and adds 0x10 to each phase head. Port mirrors with explicit
 * per-marker increment. */
void td5_vfx_advance_tracked_marker_phases(void) {
    if (!s_tracked_marker_initialized) return;
    for (int i = 0; i < TD5_VFX_TRACKED_MARKER_COUNT; i++) {
        s_tracked_marker_phase[i] += 0x10;
    }
}

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x00401370  SpawnRandomVehicleSmokePuff
 *     [SHIPPED 2026-05-24 — TIER 3 NOT_PORTED triage]
 *     Ported as td5_vfx_spawn_random_smoke_puff(actor, view_index). Wired
 *     into td5_render.c per-actor render block, mirroring orig's
 *     RenderRaceActorForView (0x0040C120) callsite. Gate, probe-midpoint
 *     formula, and Y+0x7800 lift all byte-faithful to disassembly. See
 *     td5_vfx.c implementation block above for [CONFIRMED @ ...] tags.
 *
 *   0x00429FD0  SpawnVehicleSmokePuffAtPoint
 *     [SHIPPED 2026-05-24 — TIER 3 NOT_PORTED triage]
 *     Ported as the static td5_vfx_spawn_smoke_puff_at_point helper above.
 *     This is the THIRD distinct smoke variant (alongside 0x00429A30 and
 *     0x0042A290 hardpoint) — initial_size=0x3000 / initial_alpha=0x0DC0 /
 *     deltas -0x2000/+0x1900, distinct from the others.  Velocity uses
 *     yaw-rotated cos+sin/cos-sin *0x3000 added to actor velocity (vel_y=0).
 *     Lifetime fixed at 0x200 ticks. The existing vfx_spawn_smoke_at_position
 *     mirrors 0x0042A290 (hardpoint burnout) and is left untouched.
 *
 *   0x0043E990  InitializeTireTrackPool  (density-match, verify in Phase 4)
 *   0x00446EA0  InitializeWheelPaletteUvTable  (density-match, verify in Phase 4)
 */


/* ============================================================
 * [ARCH-DIVERGENCE: particle system pipeline collapse] Phase 5(a) class manifest (2026-05-21)
 *
 * Orig has 8 entry points for per-race particle/streak system: Initialize
 * / Project / Draw / Update / Spawn / InitializeWeatherOverlay /
 * UpdateAmbientParticleDensity / RenderAmbientParticleStreaks. All are
 * ported in td5_vfx.c via the unified td5_vfx_* family
 * (init_race_particles, update_race_particles, render_race_particles).
 * Orig's per-step projection into camera-space + sprite-quad emit gets
 * unified into a single D3D11 quad-batch pipeline; per-entry timing
 * semantics preserved but parallel globals (DAT_004ab0e0 etc.)
 * consolidated into td5_vfx.c statics.
 *
 *   0x00429510  InitializeRaceParticleSystem            [ARCH-DIVERGENCE: Particle]
 *   0x00429690  ProjectRaceParticlesToView              [ARCH-DIVERGENCE: Particle]
 *   0x00429720  DrawRaceParticleEffects                 [ARCH-DIVERGENCE: Particle]
 *   0x00429790  UpdateRaceParticleEffects               [ARCH-DIVERGENCE: Particle]
 *   0x0042A6B0  SpawnAmbientParticleStreak              [ARCH-DIVERGENCE: Particle]
 *   0x00446240  InitializeWeatherOverlayParticles       [ARCH-DIVERGENCE: Particle]
 *   0x004464B0  UpdateAmbientParticleDensityForSegment  [ARCH-DIVERGENCE: Particle]
 *   0x00446560  RenderAmbientParticleStreaks            [ARCH-DIVERGENCE: Particle]
 */
