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

#define TD5_VFX_TIRE_TRACK_POOL_SIZE    240     /* [2026-06-02] was 80 (orig 0x50): a
                                                 * sustained drift/donut saturated all 80 slots
                                                 * so the trail came out sparse/dotted. Enlarged
                                                 * to 240 for a continuous trail. HARD CAP <255:
                                                 * the slot index is stored in a uint8_t (emitter
                                                 * desc pool_slot AND the actor +0x371 byte, where
                                                 * 0xFF = "no emitter"), so >256 wraps and corrupts
                                                 * earlier slots -> marks vanished. Port-only. */
#define TD5_VFX_TIRE_TRACK_SLOT_STRIDE  236     /* 0xEC bytes per emitter record */
#define TD5_VFX_TIRE_TRACK_LIFETIME     600     /* ticks before expiry */
#define TD5_VFX_TIRE_TRACK_FADE_START   300     /* ticks before fade begins */
#define TD5_VFX_TIRE_TRACK_Y_OFFSET     -20.0f  /* flush with road surface */

#define TD5_VFX_MAX_DENSITY_PAIRS       6       /* max weather density pairs per track */

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_vfx_init(void);
void td5_vfx_shutdown(void);
void td5_vfx_tick(void);

/* Global gate for the procedural (texture-free) VFX shaders. 1 = on (default),
 * 0 = legacy textured path (TD5RE_FX_PROC=0). Shared by td5_vfx.c and the
 * taillight/cop-marker glow paths in td5_render.c. */
int  td5_vfx_proc_enabled(void);

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
void td5_vfx_render_tire_marks(void);
void td5_vfx_update_tire_track_emitters(TD5_Actor *actor, int view_index);

/* ========================================================================
 * Vehicle smoke
 * ======================================================================== */

void td5_vfx_spawn_smoke(TD5_Actor *actor);
void td5_vfx_spawn_rear_wheel_smoke(TD5_Actor *actor, int view_index);

/* Engine-rev gated random smoke puff (orig 0x00401370 SpawnRandomVehicleSmokePuff).
 * Called per-frame per visible racer from the actor render path. Gates on
 * engine_speed_accum<4000 && encounter_steering_cmd>200 && rand()%engine<500;
 * spawns from probe_RR/RL midpoint via td5_vfx_spawn_smoke_puff_at_point. */
void td5_vfx_spawn_random_smoke_puff(TD5_Actor *actor, int view_index);

/* ========================================================================
 * Taillights
 * ======================================================================== */

void td5_vfx_init_taillight_templates(void);
void td5_vfx_render_taillights(int actor_index);

/* ========================================================================
 * Tracked-actor marker billboards (cop chase visuals)
 *
 * Also the home of the AdvanceWorldBillboardAnimations 0x43CDC0 port:
 * td5_vfx_advance_tracked_marker_phases() advances the cop-marker strobe
 * phases (the only thing that orig function animates).
 *
 * Port of orig 0x0043c9e0 InitializeTrackedActorMarkerBillboards. Caches
 * UVs + texture pages for POLICELT_RED / POLICELT_BLUE / POLICE_RED /
 * POLICE_BLUE atlas entries, owns per-marker animation phase counters
 * advanced by td5_vfx_advance_tracked_marker_phases. Consumed by the
 * render-side RenderTrackedActorMarker port in td5_render.c, which
 * gates its draw on g_wantedModeEnabled + tracked-target slot index.
 * ======================================================================== */

#define TD5_VFX_TRACKED_MARKER_COUNT      2  /* front + back marker per actor */
#define TD5_VFX_TRACKED_LAYERS_PER_MARK   3  /* red strobe + blue strobe + base */

void td5_vfx_init_tracked_actor_marker_billboards(void);
void td5_vfx_advance_tracked_marker_phases(void);

/* Per-layer UV / page accessor — used by render to draw the 3-layer
 * billboard stack (red strobe + blue strobe + base marker) per marker. */
int  td5_vfx_tracked_marker_get_page(int marker, int layer);
void td5_vfx_tracked_marker_get_uv(int marker, int layer,
                                    float *u0, float *v0, float *u1, float *v1);
int  td5_vfx_tracked_marker_get_phase(int marker);
int  td5_vfx_tracked_marker_initialized(void);

/* ========================================================================
 * Weather state accessors (used by td5_sound_update_ambient)
 *
 * [CONFIRMED @ 0x00440B00]: UpdateVehicleAudioMix reads per-view weather
 * particle active count (g_weatherActiveCountView0) and g_weatherType
 * to gate rain sound playback/volume.
 * ======================================================================== */

int  td5_vfx_get_weather_active_count(int view_index);
int  td5_vfx_get_weather_type(void);

#endif /* TD5_VFX_H */
