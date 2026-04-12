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

/* ========================================================================
 * Tire track emitter slot (0xEC = 236 bytes)
 * ======================================================================== */

typedef struct VfxTireTrackSlot {
    int16_t  vertices[4][3];        /* +0x00: 4 corner vertices (int16 x,y,z) = 24 bytes */
    int16_t  prev_verts[4][3];      /* +0x18: previous frame corners = 24 bytes */
    uint8_t  quad_data[0x98];       /* +0x30: sprite quad template data */
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

/* ========================================================================
 * Static state -- all VFX subsystem globals
 * ======================================================================== */

/* --- Race particle system --- */
static uint8_t  s_particle_banks[2][TD5_VFX_PARTICLE_BANK_SIZE]; /* 2 views */
static uint8_t  s_sprite_render_flags[2][TD5_VFX_SPRITE_BATCH_COUNT];
static VfxSpriteQuad s_sprite_batches[2 * TD5_VFX_SPRITE_BATCH_COUNT];
static int      s_current_view_index;
static unsigned int s_vfx_debug_frame;

/* Sprite UV data from archive entries */
static float    s_rainspl_u0, s_rainspl_v0, s_rainspl_u1, s_rainspl_v1;
static float    s_rainspl_page;
static float    s_smoke_u0, s_smoke_v0, s_smoke_u1, s_smoke_v1;
static float    s_smoke_page;

/* Smoke sprite variant UV table (4 entries: 2x2 grid in atlas) */
static float    s_smoke_variant_uv[4][5]; /* u0, v0, width, height, page */

/* --- Weather overlay --- */
static VfxWeatherParticle *s_weather_buf[2];   /* per-view particle buffers */
static int      s_weather_type;                /* 0=rain, 1=snow(cut), 2=clear */
static int      s_weather_target_density[2];   /* target particle count per view */
static int      s_weather_active_count[2];     /* current active count per view */
static float    s_weather_prev_cam[2][3];      /* previous camera position per view */
static float    s_weather_prev_budget[2];      /* previous sim_budget per view */
static float    s_weather_sprite_page;         /* texture page for weather sprite */

/* Weather sprite UV (rain/snow) */
static float    s_weather_u0, s_weather_v0;
static float    s_weather_u1, s_weather_v1;

/* --- Tire track pool --- */
static VfxTireTrackSlot *s_tire_track_pool;    /* 80-slot pool */
static VfxEmitterDesc    s_emitter_descs[TD5_MAX_TOTAL_ACTORS * 4]; /* per-wheel */
static int               s_emitter_desc_count;
static int               s_tire_track_cursor;  /* roving allocation cursor */

/* Tire mark UV coords -- loaded from SKIDMARK sprite or fallback to SMOKE dark variant */
static float    s_tiremark_u0, s_tiremark_v0;
static float    s_tiremark_u1, s_tiremark_v1;
static float    s_tiremark_page;

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

/* Taillight billboard offset vectors (from 0x463030-0x463048)
 * Original: int16[4] per entry (4th = 0 padding). We store only xyz. */
static const int16_t s_taillight_offsets[4][3] = {
    {  80, -80, 0 },   /* 0x463030: top-left     */
    { -80, -80, 0 },   /* 0x463038: bottom-left  */
    {  80,  80, 0 },   /* 0x463040: top-right    */
    { -80,  80, 0 },   /* 0x463048: bottom-right */
};

/* --- Billboard animation --- */
static int32_t *s_billboard_phase_table;
static int      s_billboard_count;

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
static const float VIEW_SCALE       = 4096.0f;     /* DAT_0045d604 */

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

    /* Billboard init */
    s_billboard_phase_table = NULL;
    s_billboard_count = 0;

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
    td5_vfx_advance_billboard_anims();

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

    /* Build 4-variant smoke UV table (2x2 grid in smoke atlas)
     * variant = (col, row): u = col * half_width + u0, v = row * half_height + v0
     * Original uses DAT_0045d5dc as the half-size multiplier */
    float half_w = (s_smoke_u1 - s_smoke_u0) * 0.5f;
    float half_h = (s_smoke_v1 - s_smoke_v0) * 0.5f;
    for (int i = 0; i < 4; i++) {
        int col = i & 1;
        int row = i / 2;
        s_smoke_variant_uv[i][0] = (float)col * half_w + s_smoke_u0;
        s_smoke_variant_uv[i][1] = (float)row * half_h + s_smoke_v0;
        s_smoke_variant_uv[i][2] = half_w;   /* width */
        s_smoke_variant_uv[i][3] = half_h;   /* height */
        s_smoke_variant_uv[i][4] = s_smoke_page;
    }

    /* Look up SKIDMARK sprite for tire marks. TD5's static.hed does not
     * include one — the original binary likely drew tire marks procedurally
     * or used a level-specific texture. Fall back to FADEWHT (a 4x4 white
     * quad in tpage4 at 220,32) so the vertex color dominates the
     * modulation and we can draw semi-transparent dark strips. */
    TD5_AtlasEntry *skidmark = td5_asset_find_atlas_entry(NULL, "SKIDMARK");
    if (vfx_atlas_entry_valid(skidmark)) {
        vfx_extract_sprite_uvs(skidmark,
                                &s_tiremark_u0, &s_tiremark_v0,
                                &s_tiremark_u1, &s_tiremark_v1,
                                &s_tiremark_page);
    } else {
        /* Use FADEWHT — pure white 4x4 region on tpage4, alpha=0x80 after
         * speedo keying. A white texture lets the vertex color dominate
         * modulation so we can draw any tire-mark color we want. */
        TD5_AtlasEntry *fadewht = td5_asset_find_atlas_entry(NULL, "FADEWHT");
        if (vfx_atlas_entry_valid(fadewht)) {
            vfx_extract_sprite_uvs(fadewht,
                                    &s_tiremark_u0, &s_tiremark_v0,
                                    &s_tiremark_u1, &s_tiremark_v1,
                                    &s_tiremark_page);
        } else {
            s_tiremark_u0 = 0.5f;  s_tiremark_v0 = 0.5f;
            s_tiremark_u1 = 1.5f;  s_tiremark_v1 = 1.5f;
            s_tiremark_page = (float)(700 + 4);
        }
        TD5_LOG_I(LOG_TAG, "SKIDMARK not in static.hed; using FADEWHT as tire-mark base");
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
 */
void td5_vfx_project_particles(int view_index) {
    /* g_cameraBasis is the real per-frame camera rotation matrix written
     * by the camera module (td5_camera.c). The older g_renderBasisMatrix
     * declared in td5_render.c is a stale identity and never updated.
     * Row layout: m[0..2]=right, m[3..5]=up, m[6..8]=forward. */
    extern float g_cameraBasis[9];

    float cam_x, cam_y, cam_z;
    td5_camera_get_position(&cam_x, &cam_y, &cam_z);

    uint8_t *bank = s_particle_banks[view_index & 1];

    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[0];

        if ((flags & 0x80) == 0) continue;
        if ((flags & 0x20) != 0) continue;

        int32_t wx, wy, wz;
        memcpy(&wx, slot + 1, 4);
        memcpy(&wy, slot + 5, 4);
        memcpy(&wz, slot + 9, 4);

        float cx = (float)wx * FP_TO_FLOAT - cam_x;
        float cy = (float)wy * FP_TO_FLOAT - cam_y;
        float cz = (float)wz * FP_TO_FLOAT - cam_z;

        float view_x = cx * g_cameraBasis[0] + cy * g_cameraBasis[1] + cz * g_cameraBasis[2];
        float view_y = cx * g_cameraBasis[3] + cy * g_cameraBasis[4] + cz * g_cameraBasis[5];
        float view_z = cx * g_cameraBasis[6] + cy * g_cameraBasis[7] + cz * g_cameraBasis[8];

        memcpy(slot + 0x0D, &view_x, 4);
        memcpy(slot + 0x11, &view_y, 4);
        memcpy(slot + 0x15, &view_z, 4);

        slot[0] = flags | 0x40;
    }
}

/**
 * DrawRaceParticleEffects (0x429720)
 *
 * Per-frame particle render pass. Projects each active particle's view-space
 * position to screen, rebuilds its sprite batch quad with the projected
 * corners, and submits via the pre-transformed translucent sprite path
 * (same as the HUD and tire-track pipelines). Using td5_render_submit_
 * translucent — NOT td5_render_queue_translucent_batch, which expects
 * TD5_PrimitiveCmd records, not 0xB8 sprite quads.
 */
void td5_vfx_draw_particles(int view_index) {
    extern void td5_render_submit_translucent(uint16_t *quad_data);
    extern float g_render_width_f;
    extern float g_render_height_f;

    s_current_view_index = view_index;
    int vi = view_index & 1;
    int drawn = 0;


    /* Decrement sprite render flags (fade timers) */
    for (int i = 0; i < TD5_VFX_SPRITE_BATCH_COUNT; i++) {
        if ((int8_t)s_sprite_render_flags[vi][i] < 0) {
            s_sprite_render_flags[vi][i]++;
        }
    }

    /* Project all active particles to view space */
    td5_vfx_project_particles(view_index);

    /* Perspective projection parameters (match tire track pipeline) */
    const float focal    = g_render_width_f * 0.5f / 0.41421356f;
    const float center_x = g_render_width_f  * 0.5f;
    const float center_y = g_render_height_f * 0.5f;
    const float far_clip = 10000.0f;
    const float near_z   = 1.0f;

    uint8_t *bank = s_particle_banks[vi];
    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[0];

        if ((flags & 0xC0) != 0xC0) continue;       /* need active + projected */

        int batch_index = slot[2];
        if (batch_index < 0 || batch_index >= TD5_VFX_SPRITE_BATCH_COUNT) continue;
        VfxSpriteQuad *sq = &s_sprite_batches[vi * TD5_VFX_SPRITE_BATCH_COUNT + batch_index];

        float vx, vy, vz;
        memcpy(&vx, slot + 0x0D, 4);
        memcpy(&vy, slot + 0x11, 4);
        memcpy(&vz, slot + 0x15, 4);

        if (vz <= near_z) continue;

        float inv_z = 1.0f / vz;
        float sx = vx * focal * inv_z + center_x;
        float sy = -vy * focal * inv_z + center_y;
        float sz = vz * (1.0f / far_clip);
        if (sz > 1.0f) sz = 1.0f;

        /* Perspective-scale the sprite billboard. */
        const float WORLD_HALF = 30.0f;
        float half_w = WORLD_HALF * focal * inv_z;
        float half_h = WORLD_HALF * focal * inv_z;
        if (half_w < 6.0f) half_w = 6.0f;
        if (half_h < 6.0f) half_h = 6.0f;
        if (half_w > 200.0f) half_w = 200.0f;
        if (half_h > 200.0f) half_h = 200.0f;

        /* Normalize texel UVs to [0,1] for the D3D11 sampler. Static atlas
         * pages are 256x256; query actual dimensions so hi-res replacement
         * pages keep working. Matches hud_build_sprite_quad (td5_hud.c:388). */
        int tw = 256, th = 256;
        td5_plat_render_get_texture_dims((int)sq->texture_page, &tw, &th);
        float inv_tw = 1.0f / (float)tw;
        float inv_th = 1.0f / (float)th;
        float nu0 = sq->tex_u0 * inv_tw;
        float nv0 = sq->tex_v0 * inv_th;
        float nu1 = sq->tex_u1 * inv_tw;
        float nv1 = sq->tex_v1 * inv_th;

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

        td5_render_submit_translucent((uint16_t *)sq);
        drawn++;
    }

    if ((s_vfx_debug_frame % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "particle draw view %d: drawn=%d", view_index, drawn);
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

    uint8_t *bank = s_particle_banks[vi];
    for (int i = 0; i < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; i++) {
        uint8_t *slot = bank + i * TD5_VFX_PARTICLE_SLOT_STRIDE;
        uint8_t flags = slot[0];

        /* Original checks flags & 0x80 (active bit) */
        if ((flags & 0x80) != 0) {
            active_particles++;
            /* Dispatch by particle type byte at slot[1].
             * Type 0 = smoke puff: apply gravity + drag, decrement lifetime.
             * Type 1 = rain splash: expand + fade, short lifetime. */
            uint8_t type = slot[1];
            (void)type;

            /* Read lifetime counter at a known offset within the slot */
            uint8_t *lifetime_ptr = slot + 0x1C;
            uint8_t lifetime = *lifetime_ptr;

            /* Decrement lifetime; deactivate when expired */
            if (lifetime > 0) {
                (*lifetime_ptr)--;
            } else {
                /* Deactivate: clear active flag */
                slot[0] = 0;
                /* Free companion sprite render slot */
                int batch_idx = slot[2];
                if (batch_idx < TD5_VFX_SPRITE_BATCH_COUNT) {
                    s_sprite_render_flags[vi][batch_idx] = 0;
                }
            }
        }
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

        /* Snow init ranges: X [-8000,8000], Y [-8000,4000], Z [200,8143] */
        for (int v = 0; v < 2; v++) {
            VfxWeatherParticle *buf = s_weather_buf[v];
            for (int i = 0; i < TD5_VFX_MAX_WEATHER_PARTICLES; i++) {
                buf[i].pos_x = (float)(rand() % 16000 - 8000);
                buf[i].pos_y = (float)(rand() % 12000 - 8000);
                buf[i].pos_z = (float)(rand() % 7943 + 200);
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

    /* Walk density pair array from track environment config.
     * Original: pairs at gTrackEnvironmentConfig + 0x36, count at +0x2c.
     * Each pair is 4 bytes: [int16 segment_id, int16 density]. */
    if (g_track_environment_config) {
        int32_t pair_count;
        memcpy(&pair_count, g_track_environment_config + 0x2C, sizeof(int32_t));
        if (pair_count > TD5_VFX_MAX_DENSITY_PAIRS)
            pair_count = TD5_VFX_MAX_DENSITY_PAIRS;

        uint8_t *pair_base = g_track_environment_config + 0x36;
        for (int p = 0; p < pair_count; p++) {
            int16_t seg_id, density;
            memcpy(&seg_id, pair_base + p * 4, sizeof(int16_t));
            memcpy(&density, pair_base + p * 4 + 2, sizeof(int16_t));

            if (current_segment == seg_id) {
                int target = (int)density;
                if (target > TD5_VFX_MAX_WEATHER_PARTICLES)
                    target = TD5_VFX_MAX_WEATHER_PARTICLES;
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

    /* Rain splash spawn: random check proportional to density */
    int splash_roll = rand() & 0x7F;
    if (splash_roll <= active_count && active_count > 0) {
        /* SpawnAmbientParticleStreak -- spawns RAINSPL splash in general pool.
         * Allocates from general particle bank and builds a splash sprite quad
         * using the RAINSPL UV coordinates extracted at init time. */
        int32_t actor_speed;
        memcpy(&actor_speed, ap + 0x318, 4); /* lateral_speed as seed */

        /* Find a free particle slot in the view's bank */
        uint8_t *bank = s_particle_banks[vi];
        for (int s = 0; s < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; s++) {
            uint8_t *pslot = bank + s * TD5_VFX_PARTICLE_SLOT_STRIDE;
            if (pslot[0] == 0) {
                /* Found free slot -- initialize as rain splash particle */
                pslot[0] = 0xE0; /* active | projected | blend */
                pslot[1] = 1;    /* type 1 = rain splash */
                pslot[0x1C] = 20; /* lifetime: 20 ticks */

                /* Seed position near camera at ground level */
                int32_t splash_wx = pos_x + (rand() % 8000 - 4000) * 256;
                int32_t splash_wy = pos_y;
                int32_t splash_wz = pos_z + (rand() % 8000 - 4000) * 256;
                memcpy(pslot + 1, &splash_wx, 4);
                memcpy(pslot + 5, &splash_wy, 4);
                memcpy(pslot + 9, &splash_wz, 4);

                /* Find free sprite batch slot */
                for (int b = 0; b < TD5_VFX_SPRITE_BATCH_COUNT; b++) {
                    if (s_sprite_render_flags[vi][b] == 0) {
                        s_sprite_render_flags[vi][b] = 0x80; /* mark used */
                        pslot[2] = (uint8_t)b; /* store batch index */

                        /* Build splash quad */
                        VfxSpriteQuad *sq = &s_sprite_batches[vi * TD5_VFX_SPRITE_BATCH_COUNT + b];
                        vfx_build_sprite_quad(sq, 0.0f, 0.0f, 128.0f,
                                               4.0f, 4.0f,
                                               s_rainspl_u0, s_rainspl_v0,
                                               s_rainspl_u1, s_rainspl_v1,
                                               s_rainspl_page, 0xFFFFFFFF);
                        break;
                    }
                }
                break;
            }
        }
    }

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

    float motion_x = wind_x - cam_delta_x;
    float motion_y = wind_y - cam_delta_y;
    float motion_z = wind_z - cam_delta_z;

    if (!g_td5.paused) {
        s_weather_prev_cam[cam_idx][0] = cam_x;
        s_weather_prev_cam[cam_idx][1] = cam_y;
        s_weather_prev_cam[cam_idx][2] = cam_z;
    }

    /* Transform motion vector to view space via render rotation matrix */
    float view_motion[3];
    td5_render_transform_vec3(&motion_x, view_motion);

    /* Scale view-space motion by VIEW_SCALE (4096.0) / world_scale
     * Original: DAT_00467368 is a world scale factor = g_worldToRenderScale
     * which is 1/256 = 0.00390625. We use it as the denominator. */
    float world_scale = (g_worldToRenderScale > 0.0f) ? g_worldToRenderScale : 1.0f;
    float advect_x = view_motion[0] * VIEW_SCALE / world_scale;
    float advect_y = view_motion[1] * VIEW_SCALE / world_scale;
    float advect_z = view_motion[2] * VIEW_SCALE / world_scale;

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
        quad->v0_color = 0xFFFFFFFF;       quad->v0_u = s_weather_u0;
        quad->v0_v = s_weather_v0;

        /* v1 = top-right */
        quad->v1_x = top_x + HALF_PIXEL;  quad->v1_y = top_y;
        quad->v1_z = avg_depth;            quad->v1_rhw = rhw;
        quad->v1_color = 0xFFFFFFFF;       quad->v1_u = s_weather_u1;
        quad->v1_v = s_weather_v0;

        /* v2 = bottom-right */
        quad->v2_x = bot_x + HALF_PIXEL;  quad->v2_y = bot_y;
        quad->v2_z = avg_depth;            quad->v2_rhw = rhw;
        quad->v2_color = 0xFFFFFFFF;       quad->v2_u = s_weather_u1;
        quad->v2_v = s_weather_v1;

        /* v3 = bottom-left */
        quad->v3_x = bot_x - HALF_PIXEL;  quad->v3_y = bot_y;
        quad->v3_z = avg_depth;            quad->v3_rhw = rhw;
        quad->v3_color = 0xFFFFFFFF;       quad->v3_u = s_weather_u0;
        quad->v3_v = s_weather_v1;

        quad->texture_page = s_weather_sprite_page;

        /* Submit to translucent pipeline */
        td5_render_queue_translucent_batch(quad);
    }
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
 * AcquireTireTrackEmitter (0x43F030)
 *
 * Allocates a free slot from the 80-slot tire track pool using a roving
 * cursor. Scans forward from cursor, wraps to 0 if needed.
 * Returns pool slot index, or -1 if no free slot found.
 */
static int vfx_acquire_tire_track_emitter(int wheel_id, int actor_slot,
                                           int wheel_index, uint8_t alpha,
                                           uint8_t width) {
    if (!s_tire_track_pool) return -1;

    int found = -1;

    /* Phase 1: scan forward from cursor */
    if (s_tire_track_cursor < TD5_VFX_TIRE_TRACK_POOL_SIZE) {
        for (int i = s_tire_track_cursor; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
            if ((s_tire_track_pool[i].control & 1) == 0) {
                found = i;
                s_tire_track_cursor = i;
                break;
            }
        }
    }

    /* Phase 2: wrap to start if not found */
    if (found == -1) {
        for (int i = 0; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
            if ((s_tire_track_pool[i].control & 1) == 0) {
                found = i;
                s_tire_track_cursor = i;
                break;
            }
        }
    }

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

    /* Initialize pool slot */
    VfxTireTrackSlot *slot = &s_tire_track_pool[found];

    /* Copy wheel world position from actor's high-res wheel positions.
     * Actor struct offset +0x298 = wheel_world_positions_hires[4], each 12 bytes.
     * wheel_index selects which wheel (0=FL, 1=FR, 2=RL, 3=RR). */
    uint8_t *actor_base = (uint8_t *)((uintptr_t)actor_slot * TD5_ACTOR_STRIDE +
                          (uintptr_t)g_td5.total_actor_count); /* placeholder base */
    /* Use a safer approach: read from the emitter desc's actor slot.
     * We need the actual actor pointer -- get it via the global actor table. */
    /* Since we only have the actor_slot index, and the caller already has
     * the actor pointer in the tire effect functions, we read the wheel
     * position there and pass it through the descriptor. For now, use the
     * wheel world positions at actor+0x298 + wheel_index * 12. */
    slot->anchor_x = 0;
    slot->anchor_y = 0;
    slot->anchor_z = 0;

    slot->control = 1;  /* allocated, no geometry yet */
    slot->lifetime = 0;
    slot->intensity = width;

    return found;
}

/**
 * Helper: read wheel world position from actor's hires wheel positions.
 * Offset +0x298 + wheel_index * 12, each component is int32 (24.8 fixed).
 */
static void vfx_read_wheel_world_pos(TD5_Actor *actor, int wheel_index,
                                      int32_t *out_x, int32_t *out_y, int32_t *out_z)
{
    uint8_t *ap = (uint8_t *)actor;
    int offset = 0x298 + wheel_index * 12;
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
    if (!s_tire_track_pool) return;

    int active_emitters = 0;

    for (int d = 0; d < (int)(sizeof(s_emitter_descs) / sizeof(s_emitter_descs[0]));
         d++) {
        VfxEmitterDesc *desc = &s_emitter_descs[d];
        if (!desc->active) continue;
        active_emitters++;

        int slot_idx = (int)desc->pool_slot;
        if (slot_idx < 0 || slot_idx >= TD5_VFX_TIRE_TRACK_POOL_SIZE) continue;

        VfxTireTrackSlot *slot = &s_tire_track_pool[slot_idx];

        /* The actor pointer is resolved during the per-vehicle update dispatch
         * (vfx_update_tire_track_emitters). Here we only use the stored anchor
         * position which was set by vfx_set_emitter_anchor_from_wheel. */

        /* Compute heading direction from position delta (24.8 fixed) */
        int32_t dx = slot->anchor_x - (int32_t)(slot->prev_verts[0][0] << 8);
        int32_t dz = slot->anchor_z - (int32_t)(slot->prev_verts[0][2] << 8);

        /* Compute perpendicular offset for strip width:
         * width * cos/sin of heading, normalized by travel distance */
        int32_t w = (int32_t)desc->width;
        int32_t len = td5_isqrt(dx * dx + dz * dz);
        if (len > 0) {
            int32_t perp_x = (-dz * w) / len;
            int32_t perp_z = ( dx * w) / len;

            /* Build strip vertices: 4 corners forming a quad segment.
             * vertices[0],[1] = previous position (trailing edge)
             * vertices[2],[3] = current position (leading edge)
             * Y coordinate = anchor Y for road-flush placement */
            float anchor_fx = (float)slot->anchor_x * FP_TO_FLOAT;
            float anchor_fy = (float)slot->anchor_y * FP_TO_FLOAT;
            float anchor_fz = (float)slot->anchor_z * FP_TO_FLOAT;
            float perp_fx = (float)perp_x * FP_TO_FLOAT;
            float perp_fz = (float)perp_z * FP_TO_FLOAT;

            /* Copy current leading edge to trailing edge (previous frame's
             * leading edge becomes this frame's trailing edge) */
            if (slot->control & 2) {
                /* Already has geometry: shift leading edge to trailing */
                slot->vertices[0][0] = slot->vertices[2][0];
                slot->vertices[0][1] = slot->vertices[2][1];
                slot->vertices[0][2] = slot->vertices[2][2];
                slot->vertices[1][0] = slot->vertices[3][0];
                slot->vertices[1][1] = slot->vertices[3][1];
                slot->vertices[1][2] = slot->vertices[3][2];
            } else {
                /* First geometry frame: trailing edge = previous anchor */
                float prev_fx = (float)slot->prev_verts[0][0];
                float prev_fy = anchor_fy;
                float prev_fz = (float)slot->prev_verts[0][2];
                slot->vertices[0][0] = (int16_t)(prev_fx - perp_fx);
                slot->vertices[0][1] = (int16_t)(prev_fy);
                slot->vertices[0][2] = (int16_t)(prev_fz - perp_fz);
                slot->vertices[1][0] = (int16_t)(prev_fx + perp_fx);
                slot->vertices[1][1] = (int16_t)(prev_fy);
                slot->vertices[1][2] = (int16_t)(prev_fz + perp_fz);
            }

            /* Leading edge: current anchor +/- perpendicular */
            slot->vertices[2][0] = (int16_t)(anchor_fx - perp_fx);
            slot->vertices[2][1] = (int16_t)(anchor_fy);
            slot->vertices[2][2] = (int16_t)(anchor_fz - perp_fz);
            slot->vertices[3][0] = (int16_t)(anchor_fx + perp_fx);
            slot->vertices[3][1] = (int16_t)(anchor_fy);
            slot->vertices[3][2] = (int16_t)(anchor_fz + perp_fz);

            slot->control |= 2; /* has_geometry flag */
        }

        /* Save current anchor as previous for next frame delta computation */
        slot->prev_verts[0][0] = (int16_t)((float)slot->anchor_x * FP_TO_FLOAT);
        slot->prev_verts[0][2] = (int16_t)((float)slot->anchor_z * FP_TO_FLOAT);

        /* Check if direction changed enough to warrant a new pool slot
         * Original: ((old_heading ^ new_heading) & 0xFFFFFF80) != 0 */
        uint32_t new_hash = (uint32_t)(dx & 0xFFFFFF80);
        if ((slot->direction_hash ^ new_hash) & 0xFFFFFF80u) {
            /* Direction changed: mark current slot as finalized and
             * acquire a new one. The caller will handle reallocation
             * on the next emitter update cycle. */
            slot->direction_hash = new_hash;
        }
    }

    if ((s_vfx_debug_frame % 60u) == 0u) {
        TD5_LOG_D(LOG_TAG, "tire tracks update: active_emitters=%d", active_emitters);
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
    /* TODO: tire track strip geometry has multiple bugs:
     * - Degenerate quads (v0 == v2, zero-area strips)
     * - Per-frame gravity sinks anchor_y indefinitely
     * - No SKIDMARK texture in static.hed (FADEWHT fallback)
     * Disabled until the strip builder in update_tire_tracks is fixed. */
    return;

    extern void td5_render_submit_translucent(uint16_t *quad_data);
    extern float g_cameraBasis[9];
    extern float g_render_width_f;
    extern float g_render_height_f;

    static uint32_t s_tt_render_frame = 0;
    int tt_log = ((s_tt_render_frame++ % 60u) == 0u);
    int tt_alive = 0, tt_submitted = 0;

    if (!s_tire_track_pool) return;

    /* Per-frame gravity constant (applied to Y of each active mark).
     * Original: subtracts a small gravity offset per frame. */
    static const float PARTICLE_GRAVITY_PER_FRAME = 0.25f;

    /* Get camera position for view-space transformation */
    float cam_x, cam_y, cam_z;
    td5_camera_get_position(&cam_x, &cam_y, &cam_z);

    /* Projection parameters: focal length derived from render width,
     * matching the original's 640-width perspective scale. */
    float focal = g_render_width_f * 0.5f / 0.41421356f; /* tan(45/2) */
    float center_x = g_render_width_f * 0.5f;
    float center_y = g_render_height_f * 0.5f;
    float far_clip = 10000.0f;
    float near_clip = 1.0f;

    for (int i = 0; i < TD5_VFX_TIRE_TRACK_POOL_SIZE; i++) {
        VfxTireTrackSlot *slot = &s_tire_track_pool[i];

        /* Only render slots with geometry (bit 1 = has_geometry) */
        if ((slot->control & 2) == 0) continue;
        tt_alive++;

        /* Age the lifetime counter */
        slot->lifetime++;

        /* Expire after 600 ticks */
        if (slot->lifetime > TD5_VFX_TIRE_TRACK_LIFETIME) {
            slot->control = 0;
            continue;
        }

        /* Fade after 300 ticks: decrement intensity by 1 per tick.
         * Skip fade if bit 3 (0x08) is set in control flags (persistent marks). */
        if (slot->lifetime > TD5_VFX_TIRE_TRACK_FADE_START) {
            if (!(slot->control & 0x08)) {
                if (slot->intensity > 0) {
                    slot->intensity--;
                }
            }
        }

        /* Skip invisible marks */
        if (slot->intensity == 0) continue;

        /* Transform world anchor to view space for frustum test */
        float view_x = (float)slot->anchor_x * FP_TO_FLOAT - cam_x;
        float view_y = (float)slot->anchor_y * FP_TO_FLOAT - cam_y;
        float view_z = (float)slot->anchor_z * FP_TO_FLOAT - cam_z;

        /* Apply frustum test: skip if behind camera or too far */
        if (!td5_render_is_sphere_visible(view_x, view_y, view_z, 50.0f))
            continue;

        /* Project 4 world-space vertices to screen space.
         * Each vertex is int16 (x,y,z) in float-scale world units.
         * Transform: world -> camera-relative -> view (via basis matrix) -> screen */
        float sx[4], sy[4], sz[4], srhw[4];
        int all_visible = 1;

        for (int v = 0; v < 4; v++) {
            /* World position from int16 vertices */
            float wx = (float)slot->vertices[v][0];
            float wy = (float)slot->vertices[v][1] + TRACK_Y_OFFSET;
            float wz = (float)slot->vertices[v][2];

            /* Camera-relative */
            float cx = wx - cam_x;
            float cy = wy - cam_y;
            float cz = wz - cam_z;

            /* Apply view rotation via the real per-frame camera basis
             * (g_cameraBasis is written by td5_camera.c; the older
             * g_renderBasisMatrix is a stale identity). */
            float vx = cx * g_cameraBasis[0] + cy * g_cameraBasis[1] + cz * g_cameraBasis[2];
            float vy = cx * g_cameraBasis[3] + cy * g_cameraBasis[4] + cz * g_cameraBasis[5];
            float vz = cx * g_cameraBasis[6] + cy * g_cameraBasis[7] + cz * g_cameraBasis[8];

            /* Perspective project */
            if (vz <= near_clip) { all_visible = 0; break; }
            float inv_z = 1.0f / vz;
            sx[v]   = vx * focal * inv_z + center_x;
            sy[v]   = -vy * focal * inv_z + center_y; /* Y inverted for screen */
            sz[v]   = vz * (1.0f / far_clip);
            srhw[v] = inv_z;
        }

        if (!all_visible) continue;

        /* Apply gravity to Y coordinates (subtract per-frame gravity constant) */
        slot->anchor_y -= (int32_t)(PARTICLE_GRAVITY_PER_FRAME * 256.0f);

        uint8_t val = slot->intensity;
        if (val < 0x30) val = 0x30;
        uint32_t color = 0xFF000000u | ((uint32_t)val << 16) |
                         ((uint32_t)val << 8) | (uint32_t)val;

        /* Normalize texel UVs to [0,1] for the D3D11 sampler (same as HUD). */
        int tt_tw = 256, tt_th = 256;
        td5_plat_render_get_texture_dims((int)s_tiremark_page, &tt_tw, &tt_th);
        float tt_inv_tw = 1.0f / (float)tt_tw;
        float tt_inv_th = 1.0f / (float)tt_th;
        float tm_u0 = s_tiremark_u0 * tt_inv_tw;
        float tm_v0 = s_tiremark_v0 * tt_inv_th;
        float tm_u1 = s_tiremark_u1 * tt_inv_tw;
        float tm_v1 = s_tiremark_v1 * tt_inv_th;

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

        tquad.v2_x = sx[3]; tquad.v2_y = sy[3]; tquad.v2_z = sz[3]; tquad.v2_rhw = srhw[3];
        tquad.v2_color = color; tquad.v2_u = tm_u1; tquad.v2_v = tm_v1;

        tquad.v3_x = sx[2]; tquad.v3_y = sy[2]; tquad.v3_z = sz[2]; tquad.v3_rhw = srhw[2];
        tquad.v3_color = color; tquad.v3_u = tm_u0; tquad.v3_v = tm_v1;

        tquad.tex_u0 = s_tiremark_u0; tquad.tex_v0 = s_tiremark_v0;
        tquad.tex_u1 = s_tiremark_u1; tquad.tex_v1 = s_tiremark_v1;
        tquad.quad_width = 0.0f;
        tquad.quad_height = 0.0f;
        tquad.texture_page = s_tiremark_page;

        /* Submit as pre-transformed translucent quad (same path as HUD overlays) */
        td5_render_submit_translucent((uint16_t *)&tquad);
        tt_submitted++;
    }

    if (tt_log) {
        TD5_LOG_I(LOG_TAG, "tire render: alive=%d submitted=%d",
                  tt_alive, tt_submitted);
    }
}

/**
 * UpdateTireTrackEmitterDispatch (0x43FAE0)
 *
 * Per-vehicle master dispatcher for tire effects. Reads drivetrain layout
 * from tuning data (+0x76 = 1:RWD, 2:FWD, 3:AWD) and calls the
 * appropriate front/rear tire effect and sound functions.
 *
 * Gated by DAT_004aad60 == 0 (disabled during replays or pause).
 */
void td5_vfx_update_tire_track_emitters(TD5_Actor *actor) {
    if (!actor) return;

    /* Guard: disabled during replays/pause
     * Original: if (DAT_004aad60 != 0) return; */
    if (g_td5.paused) return;

    uint8_t *ap = (uint8_t *)actor;

    /* Read speed values from actor struct */
    int32_t rear_speed, front_speed;
    memcpy(&rear_speed,  ap + 0x31C, 4); /* front_axle_slip_excess */
    memcpy(&front_speed, ap + 0x320, 4); /* rear_axle_slip_excess */

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
    (void)actor;

    TD5_LOG_D(LOG_TAG,
              "smoke spawn: pos=(%.2f, %.2f, %.2f) variant=%d view=%d",
              wx, wy, wz, variant & 3, view_index);

    /* Find a free particle slot */
    for (int s = 0; s < TD5_VFX_PARTICLE_SLOTS_PER_VIEW; s++) {
        uint8_t *slot = bank + s * TD5_VFX_PARTICLE_SLOT_STRIDE;
        if (slot[0] != 0) continue;

        /* Found free slot -- initialize as smoke particle */
        slot[0] = 0xC0; /* active | projected */
        slot[1] = 0;    /* type 0 = smoke puff */
        slot[0x1C] = 40; /* lifetime: 40 ticks */

        /* Set world position (convert float back to 24.8 fixed) */
        int32_t fx = (int32_t)(wx * 256.0f);
        int32_t fy = (int32_t)(wy * 256.0f);
        int32_t fz = (int32_t)(wz * 256.0f);
        memcpy(slot + 1, &fx, 4);
        memcpy(slot + 5, &fy, 4);
        memcpy(slot + 9, &fz, 4);

        /* Find free sprite batch slot */
        for (int b = 0; b < TD5_VFX_SPRITE_BATCH_COUNT; b++) {
            if (s_sprite_render_flags[vi][b] == 0) {
                s_sprite_render_flags[vi][b] = 0x80; /* mark used */
                slot[2] = (uint8_t)b;

                /* Build smoke quad using variant UV from the 2x2 atlas grid */
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
 */
static void vfx_update_rear_tire_effects(TD5_Actor *actor, uint8_t contact_flags) {
    if ((contact_flags & 1) == 0) return; /* no rear contact */

    uint8_t *ap = (uint8_t *)actor;

    /* Allocate emitters for wheels 2 and 3 if not already active */
    uint8_t slot_index = *(ap + 0x375);

    for (int w = 2; w <= 3; w++) {
        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x37, 0x1A);
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
 */
static void vfx_update_front_tire_effects(TD5_Actor *actor, uint8_t contact_flags) {
    if ((contact_flags & 2) == 0) return; /* no front contact */

    uint8_t *ap = (uint8_t *)actor;
    uint8_t slot_index = *(ap + 0x375);

    /* Allocate emitters for wheels 0 and 1 */
    for (int w = 0; w <= 1; w++) {
        if (*(ap + 0x371 + w) == 0xFF) {
            int result = vfx_acquire_tire_track_emitter(
                w, (int)slot_index, w, 0x37, 0x1A);
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
                    /* Release emitter */
                    int desc_idx = emitter_id;
                    if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                        s_emitter_descs[desc_idx].active = 0;
                    }
                    if (emitter_id < TD5_VFX_TIRE_TRACK_POOL_SIZE) {
                        s_tire_track_pool[emitter_id].control = 0;
                    }
                    *(ap + 0x371 + w) = 0xFF;
                }
            }
            continue;
        }

        /* Guard checks for release */
        if (*(ap + 0x379) != 0) { /* airborne */
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                if (emitter_id < TD5_VFX_TIRE_TRACK_POOL_SIZE)
                    s_tire_track_pool[emitter_id].control = 0;
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
                w, (int)slot_index, w, 0x28, 0x1A);
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
                    int desc_idx = emitter_id;
                    if (desc_idx < (int)(sizeof(s_emitter_descs)/sizeof(s_emitter_descs[0]))) {
                        s_emitter_descs[desc_idx].active = 0;
                    }
                    if (emitter_id < TD5_VFX_TIRE_TRACK_POOL_SIZE) {
                        s_tire_track_pool[emitter_id].control = 0;
                    }
                    *(ap + 0x371 + w) = 0xFF;
                }
            }
            continue;
        }

        if (*(ap + 0x379) != 0) {
            uint8_t emitter_id = *(ap + 0x371 + w);
            if (emitter_id != 0xFF) {
                if (emitter_id < TD5_VFX_TIRE_TRACK_POOL_SIZE)
                    s_tire_track_pool[emitter_id].control = 0;
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
                w, (int)slot_index, w, 0x37, 0x1A);
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
 * Triggered on surface state 10 (0xA = burnout start) or 12 (0xC = burnout
 * sustain). Spawns smoke from hardpoints 2 and 3 (rear left/right wheels).
 *
 * Reads the actor's rotation matrix at +0x120 and wheel positions at +0x298
 * to transform local hardpoint offsets to world space.
 */
void td5_vfx_spawn_rear_wheel_smoke(TD5_Actor *actor, int view_index) {
    if (!actor) return;

    uint8_t *ap = (uint8_t *)actor;
    uint8_t surface_state = *(ap + 0x370);

    /* Only spawn on burnout states 10 and 12 */
    if (surface_state != 10 && surface_state != 12) return;

    /* Speed-proportional probability gate (from SpawnVehicleSmokePuffFromHardpoint):
     * rand() % 1000 < speed/200 */
    int32_t speed;
    memcpy(&speed, ap + 0x314, 4); /* longitudinal_speed */
    int abs_speed = (speed < 0) ? -speed : speed;

    if ((rand() % 1000) >= abs_speed / 200) return;

    /* Read actor's rotation matrix at +0x120 (3x3 row-major float[9]) */
    float rot[9];
    memcpy(rot, ap + 0x120, sizeof(float) * 9);

    /* Spawn from hardpoints 2 (rear-left) and 3 (rear-right) wheels.
     * Read wheel world positions from +0x298 + wheel * 12. */
    for (int hp = 2; hp <= 3; hp++) {
        int32_t wx, wy, wz;
        vfx_read_wheel_world_pos(actor, hp, &wx, &wy, &wz);

        /* Convert to float */
        float fwx = (float)wx * FP_TO_FLOAT;
        float fwy = (float)wy * FP_TO_FLOAT;
        float fwz = (float)wz * FP_TO_FLOAT;

        /* Apply a small upward offset using the rotation matrix's up vector
         * (row 1 of the 3x3 matrix at indices 3,4,5) scaled by a hardpoint
         * height of ~30 units */
        fwx += rot[3] * 30.0f;
        fwy += rot[4] * 30.0f;
        fwz += rot[5] * 30.0f;

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

    /* Read actor speed -- only spawn if moving */
    int32_t speed;
    memcpy(&speed, ap + 0x314, 4);
    int abs_speed = (speed < 0) ? -speed : speed;
    if (abs_speed < 4000) return;

    /* Read heading normal at +0x290 (3 x int16) */
    int16_t heading[3];
    memcpy(heading, ap + 0x290, sizeof(heading));

    /* Read world position */
    int32_t pos_x, pos_y, pos_z;
    memcpy(&pos_x, ap + 0x1FC, 4);
    memcpy(&pos_y, ap + 0x200, 4);
    memcpy(&pos_z, ap + 0x204, 4);

    /* Read rotation matrix for local-to-world transform */
    float rot[9];
    memcpy(rot, ap + 0x120, sizeof(float) * 9);

    /* Compute midpoint of two rear body corners (actor+0xA8..0xBC)
     * with upward Y offset of 0x7800 */
    int32_t corner_rl_x, corner_rl_y, corner_rl_z;
    int32_t corner_rr_x, corner_rr_y, corner_rr_z;
    memcpy(&corner_rl_x, ap + 0xA8, 4);
    memcpy(&corner_rl_y, ap + 0xAC, 4);
    memcpy(&corner_rl_z, ap + 0xB0, 4);
    memcpy(&corner_rr_x, ap + 0xB4, 4);
    memcpy(&corner_rr_y, ap + 0xB8, 4);
    memcpy(&corner_rr_z, ap + 0xBC, 4);

    float mid_x = (float)(corner_rl_x + corner_rr_x) * 0.5f * FP_TO_FLOAT;
    float mid_y = (float)((corner_rl_y + corner_rr_y) / 2 + 0x7800) * FP_TO_FLOAT;
    float mid_z = (float)(corner_rl_z + corner_rr_z) * 0.5f * FP_TO_FLOAT;

    vfx_spawn_smoke_at_position(actor, mid_x, mid_y, mid_z, 0, s_current_view_index);
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
    brake_active = (*(ap + 0x36D) != 0) ? 1 : 0;

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
 * ======================================================================== */

/**
 * AdvanceWorldBillboardAnimations (0x43CDC0)
 *
 * Simple phase counter advance for world billboard animations.
 * Iterates the billboard table at DAT_004BEDC0 with stride 0x22C (556 bytes),
 * incrementing the phase counter at offset +0x00 by 0x10 per tick.
 *
 * The table runs from 0x4BEDC0 to 0x4BF218 (range of ~0x458 = 1112 bytes).
 * At stride 0x22C, this is 2 entries (1112 / 556 = 2).
 */
void td5_vfx_advance_billboard_anims(void) {
    /* Original iterates int* from DAT_004bedc0 stepping by 0x8B dwords
     * (0x8B * 4 = 0x22C bytes) until address reaches 0x4BF218.
     * Each iteration: *phase_ptr += 0x10 */

    /* In the source port, this would iterate the billboard table
     * if we had it loaded. For now, iterate the count we know. */
    if (!s_billboard_phase_table) return;

    for (int i = 0; i < s_billboard_count; i++) {
        s_billboard_phase_table[i] += TD5_VFX_BILLBOARD_PHASE_INC;
    }
}

/* --- Wheel Billboards (0x446F00) --- */

void td5_vfx_render_wheel_billboards(int view_index)
{
    /* Wheel rendering is now handled inline in td5_render.c actor loop
     * via render_vehicle_wheel_billboards(). This stub is kept for API
     * compatibility but does nothing. */
    (void)view_index;
}
