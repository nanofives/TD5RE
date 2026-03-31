/**
 * td5_vfx.h -- Particles, tire tracks, smoke, weather, billboards, taillights
 *
 * Original functions:
 *   0x401000  InitializeVehicleTaillightQuadTemplates
 *   0x4011C0  RenderVehicleTaillightQuads
 *   0x401330  SpawnRearWheelSmokeEffects
 *   0x401410  InitializeRaceSmokeSpritePool
 *   0x429510  InitializeRaceParticleSystem
 *   0x429690  ProjectRaceParticlesToView
 *   0x429720  DrawRaceParticleEffects
 *   0x429790  UpdateRaceParticleEffects
 *   0x446240  InitializeWeatherOverlayParticles
 *   0x4464B0  UpdateAmbientParticleDensityForSegment
 *   0x446560  RenderAmbientParticleStreaks
 *   0x43EB50  UpdateTireTrackEmitters
 *   0x43F030  AcquireTireTrackEmitter
 *   0x43F210  RenderTireTrackPool
 *   0x43F420  UpdateFrontWheelSoundEffects
 *   0x43F600  UpdateRearWheelSoundEffects
 *   0x43F7E0  UpdateRearTireEffects
 *   0x43F960  UpdateFrontTireEffects
 *   0x43FAE0  UpdateTireTrackEmitterDispatch
 *   0x43CDC0  AdvanceWorldBillboardAnimations
 */

#ifndef TD5_VFX_H
#define TD5_VFX_H

#include "td5_types.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define TD5_VFX_MAX_WEATHER_PARTICLES   128     /* 0x80 particles per view */
#define TD5_VFX_WEATHER_PARTICLE_STRIDE 200     /* 0xC8 bytes per particle slot */
#define TD5_VFX_WEATHER_BUFFER_SIZE     25600   /* 0x6400 bytes per view buffer */

#define TD5_VFX_PARTICLE_SLOTS_PER_VIEW 100     /* general race particle pool */
#define TD5_VFX_PARTICLE_SLOT_STRIDE    64      /* 0x40 bytes per slot */
#define TD5_VFX_PARTICLE_BANK_SIZE      6400    /* 0x1900 bytes per view bank */
#define TD5_VFX_SPRITE_BATCH_COUNT      50      /* render slots per view */
#define TD5_VFX_SPRITE_BATCH_STRIDE     184     /* 0xB8 bytes per sprite quad */

#define TD5_VFX_TIRE_TRACK_POOL_SIZE    80      /* 0x50 slots */
#define TD5_VFX_TIRE_TRACK_SLOT_STRIDE  236     /* 0xEC bytes per emitter record */
#define TD5_VFX_TIRE_TRACK_LIFETIME     600     /* ticks before expiry */
#define TD5_VFX_TIRE_TRACK_FADE_START   300     /* ticks before fade begins */
#define TD5_VFX_TIRE_TRACK_Y_OFFSET     -20.0f  /* flush with road surface */

#define TD5_VFX_BILLBOARD_STRIDE        0x22C   /* 556 bytes per billboard entry */
#define TD5_VFX_BILLBOARD_PHASE_INC     0x10    /* phase counter increment per tick */

#define TD5_VFX_MAX_DENSITY_PAIRS       6       /* max weather density pairs per track */

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_vfx_init(void);
void td5_vfx_shutdown(void);
void td5_vfx_tick(void);

/* ========================================================================
 * Race particle system (callback-driven, 100 slots/view)
 * ======================================================================== */

void td5_vfx_init_race_particles(void);
void td5_vfx_init_smoke_sprite_pool(void);
void td5_vfx_update_particles(int view_index);
void td5_vfx_draw_particles(int view_index);
void td5_vfx_project_particles(int view_index);

/* ========================================================================
 * Weather / ambient particles (128 per view, viewport streaks)
 * ======================================================================== */

void td5_vfx_init_weather(TD5_WeatherType type);
void td5_vfx_update_ambient_density(TD5_Actor *actor, int view_index);
void td5_vfx_render_ambient_streaks(TD5_Actor *actor, float sim_budget,
                                     int view_index);

/* ========================================================================
 * Tire tracks (80-slot pool, mesh strip rendering)
 * ======================================================================== */

void td5_vfx_update_tire_tracks(void);
void td5_vfx_render_tire_tracks(void);
void td5_vfx_update_tire_track_emitters(TD5_Actor *actor);

/* ========================================================================
 * Vehicle smoke
 * ======================================================================== */

void td5_vfx_spawn_smoke(TD5_Actor *actor);
void td5_vfx_spawn_rear_wheel_smoke(TD5_Actor *actor, int view_index);

/* ========================================================================
 * Taillights
 * ======================================================================== */

void td5_vfx_init_taillight_templates(void);
void td5_vfx_render_taillights(int actor_index);

/* ========================================================================
 * Billboard animations
 * ======================================================================== */

void td5_vfx_advance_billboard_anims(void);

#endif /* TD5_VFX_H */
