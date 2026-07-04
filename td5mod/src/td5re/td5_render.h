/**
 * td5_render.h -- Scene setup, mesh transform, frustum cull
 *
 * Software transform + D3D rasterization pipeline. All vertex transformation,
 * lighting, clipping, and projection are CPU-side. GPU used only for final
 * rasterization via DrawPrimitive (pre-transformed vertices, FVF 0x1C4).
 *
 * Original functions:
 *   0x40AE10  InitializeRaceRenderGlobals
 *   0x40AE80  InitializeRaceRenderState
 *   0x40AEC0  ReleaseRaceRenderResources
 *   0x40ADE0  BeginRaceScene
 *   0x40AE00  EndRaceScene
 *   0x40AF10  ConfigureRaceFogColorAndMode
 *   0x40AF50  ApplyRaceFogRenderState
 *   0x40B070  SetRaceRenderStatePreset (4 presets)
 *   0x43DA80  LoadRenderRotationMatrix
 *   0x43DC20  LoadRenderTranslation
 *   0x43DAF0  PushRenderTransform
 *   0x43DB70  PopRenderTransform
 *   0x43DC50  TransformVec3ByRenderMatrixFull
 *   0x43DD60  TransformMeshVerticesToView
 *   0x43DDF0  ComputeMeshVertexLighting (3-light diffuse)
 *   0x42DCA0  IsBoundingSphereVisibleInCurrentFrustum (5-plane test)
 *   0x42DE10  TestMeshAgainstViewFrustum
 *   0x43DCB0  TransformAndQueueTranslucentMesh
 *   0x4314B0  RenderPreparedMeshResource (dispatch table walk)
 *   0x4317F0  ClipAndSubmitProjectedPolygon (near-plane + screen clip)
 *   0x4323D0  RenderTrackSegmentBatch (X-axis clipper)
 *   0x4326D0  RenderTrackSegmentBatchVariant (Y-axis clipper)
 *   0x432AB0  AppendClippedPolygonTriangleFan
 *   0x431270  RenderTrackSpanDisplayList
 *   0x4312E0  InitializeTranslucentPrimitivePipeline
 *   0x431460  QueueTranslucentPrimitiveBatch
 *   0x431340  FlushQueuedTranslucentPrimitives
 *   0x4329E0  FlushImmediateDrawPrimitiveBatch
 *   0x43E3B0  InsertBillboardIntoDepthSortBuckets
 *   0x43E550  QueueProjectedPrimitiveBucketEntry
 *   0x43E2F0  FlushProjectedPrimitiveBuckets (4096 buckets)
 *   0x43E7E0  ConfigureProjectionForViewport
 *   0x43DEC0  ApplyMeshProjectionEffect (water/envmap UV)
 *   0x40BD20  RenderRaceActorsForView
 *   0x40C7E0  BuildSpecialActorOverlayQuads
 *   0x40B9F0  GetTextureSlotStatus
 *   0x40BA10  AdvanceTexturePageUsageAges
 *   0x40BA60  ResetTexturePageCacheState
 *   0x40BAA0  QueryRaceTextureCapacity
 *   0x40B660  BindRaceTexturePage
 *   0x40B830  AdvanceTextureStreamingScheduler
 *   0x40B1D0  BuildTrackTextureCacheImpl
 *   0x430D30  ParseAndDecodeCompressedTrackData
 *   0x43D7C0  AdvanceGlobalSkyRotation
 *   0x43CDC0  AdvanceWorldBillboardAnimations
 *   0x446F00  RenderVehicleWheelBillboards
 *   0x43F210  RenderTireTrackPool
 */

#ifndef TD5_RENDER_H
#define TD5_RENDER_H

#include "td5_types.h"

/* --- Module lifecycle --- */
int  td5_render_init(void);
void td5_render_shutdown(void);
void td5_render_frame(void);

/* --- Scene brackets --- */
void td5_render_begin_scene(void);
void td5_render_end_scene(void);

/* --- Render state --- */
void td5_render_set_preset(TD5_RenderPreset preset);
void td5_render_set_fog(int enable);
void td5_render_configure_fog(uint32_t color, int enable);

/* --- Transform stack (single-depth) --- */
void td5_render_load_rotation(const TD5_Mat3x3 *rot);
void td5_render_load_translation(const TD5_Vec3f *pos);
void td5_render_push_transform(void);
void td5_render_pop_transform(void);
void td5_render_transform_vec3(const float *in, float *out);

/* --- Vertex transform & lighting --- */
void td5_render_transform_mesh_vertices(TD5_MeshHeader *mesh);
/* slot >= 0 applies that slot's paint tint (s_vehicle_tint); slot < 0 = no tint. */
void td5_render_compute_vertex_lighting(TD5_MeshHeader *mesh, int slot);

/* [DYNAMIC LIGHTS] Install the model->world basis used to transform registered
 * dynamic point lights (td5_light.c) into the mesh's model space for the NEXT
 * td5_render_compute_vertex_lighting call. origin = mesh world position (world
 * units); rot9 = body->world rotation (9 floats, row-major) or NULL for identity
 * (track geometry already in world-offset space). Per-pane state. */
void td5_render_set_light_basis(const float origin[3], const float *rot9);

/* [DYNAMIC LIGHTS] Enable/disable dark-mode lighting: dims the ambient +
 * directional base and lowers the clamp floor so dark areas read as dark and
 * dynamic lights (headlights) illuminate them. Default off ([Lighting] DarkMode
 * / --DarkMode / TD5RE_LIGHT_DARK_MODE). */
void td5_render_set_dark_mode(int on);

/* [DEFERRED LIGHTS] Screen-space light pass for the current viewport. Call once
 * per viewport after the opaque world (track + actors), before translucent/HUD.
 * vp_x/vp_y = the pane's origin in render-target pixels (0,0 single-view). */
void td5_render_apply_light_pass(int vp_x, int vp_y);

/* [LIGHT2 P2] Screen-space ray-marched sun shadows for the current viewport.
 * Call once per viewport after the opaque world, BEFORE
 * td5_render_apply_light_pass (additive light pools must not be darkened). */
void td5_render_apply_shadow_pass(int vp_x, int vp_y);

/* [LIGHT2 P3] Screen-space reflections for the current viewport. Call once
 * per viewport AFTER td5_render_apply_light_pass (reflections mirror the lit
 * + shadowed scene), before translucent VFX/HUD. */
void td5_render_apply_ssr_pass(int vp_x, int vp_y);

/* [LIGHT2 P0] Per-frame G-buffer gate — call once per rendered race frame
 * BEFORE the world pass (next to td5_light_begin_frame). See
 * LIGHTING_REWORK_PLAN.md. */
void td5_render_lighting2_frame_begin(void);

/* [AUTO LIGHTS] 1 when the current environment is poorly lit (dark track zone or
 * non-clear weather) and headlights should auto-enable; 0 in bright daylight. */
int  td5_render_env_is_dark(void);

/* [task#21 TD6 per-area lighting zones — REMOVED/DEFERRED 2026-06-19 at user
 * request; see the note in td5_render.c. The RE + LIGHTZONES.BIN extractor
 * (re/tools/extract_td6_lightzones.py) are kept for a future revisit.] */

/* [task#14] Draw the visible TD6 breakable-prop boxes for this view (texture-free
 * grey boxes at each un-broken prop). Call once per view after the track + actors
 * are drawn. London/TD6 only; A/B via TD5RE_TD6_PROP_MESH. */
void render_td6_props(const TD5_Actor *ref);

/* Load the de-indexed COL furniture meshes (PROPMESH.BIN) used by render_td6_props.
 * Called at TD6 track load; pass NULL/0 to clear. */
void td5_render_load_td6_prop_meshes(const void *data, size_t size);

/* Per-slot paint TINT (0xRRGGBB; 0 = white/identity). Used to color a grayscale
 * TD6 car body. TD5 cars/AI keep tint 0 and render unchanged. */
void td5_render_set_vehicle_tint(int slot, uint32_t rgb);

/* [WHEEL OVERHAUL 2026-06-12] Per-slot procedural wheel STYLE index. Assigned
 * once per race (random, from the deterministic per-race RNG so netplay stays
 * in sync). Wrapped into the style-table range. -1 / unset slots fall back to a
 * stable per-slot hash. Honoured only when TD5RE_WHEEL_OVERHAUL != 0. */
void td5_render_set_wheel_style(int slot, int style);

/* Mark a slot as holding a ported TD6 car (transcoded mesh). Suppresses the
 * chrome/projection reflection overlay for that slot (TD6 cars have a grayscale
 * body + unused env-map mesh, so the overlay misrenders as a "lights shader" in
 * planar-scroll light zones). Reset by td5_render_set_vehicle_mesh. */
void td5_render_set_vehicle_is_td6(int slot, int is_td6);

/* [S23] Install authored rear/brake-light positions for a slot (model-space
 * int16[3] ×2: light0 = +X right, light1 = -X left). Used for ported TD6 cars
 * whose binary carparam.dat has wrong taillight values at +0x60/+0x68 — the
 * loader passes the car's :CAR_LIGHTS0/1: from its TD6 param.scr. Pass l0/l1 =
 * NULL (or omit the call) to clear and fall back to the cardef hardpoint. Reset
 * by td5_render_set_vehicle_mesh. */
void td5_render_set_vehicle_taillights(int slot, const int16_t *l0, const int16_t *l1);

/* Photo-booth render mode: draw only the player car over a chroma background
 * (sky + track spans suppressed here; VFX/HUD/clear suppressed in the game frame)
 * for offline car-preview (carpic) generation. */
void td5_render_set_photobooth(int on);
int  td5_render_photobooth_active(void);

/* Drive the fixed-angle inspection camera from code (the photo booth uses it to
 * frame the car at a chosen azimuth/elevation/distance). az/el in degrees,
 * world-relative; slot = actor to frame. Dev-only (no-op in release). */
void td5_render_set_inspect_cam(int on, float az_deg, float el_deg, float dist, int slot);

/* Per-actor track-zone driven lighting basis update.
 * Walks the per-track light-zone array for `actor->track_span_raw`, picks the
 * directional vector + ambient color from the current zone, transforms the
 * world-frame contribution into body space via the actor's rotation matrix,
 * and writes s_light_dirs[]/s_ambient_intensity for the next call to
 * td5_render_compute_vertex_lighting(). Mirrors
 * ApplyTrackLightingForVehicleSegment @ 0x00430150. */
void td5_render_apply_track_lighting(int slot, TD5_Actor *actor);

/* --- Frustum culling --- */
int  td5_render_is_sphere_visible(float cx, float cy, float cz, float radius);
int  td5_render_test_mesh_frustum(TD5_MeshHeader *mesh, float *out_depth);

/* --- Mesh rendering --- */
void td5_render_span_display_list(void *display_list_block);
void td5_render_prepared_mesh(TD5_MeshHeader *mesh);
void td5_render_actors_for_view(int view_index);
void td5_render_set_vehicle_mesh(int slot, TD5_MeshHeader *mesh);
TD5_MeshHeader *td5_render_get_vehicle_mesh(int slot);
/* [POLICE rewrite] Dedicated police mesh drawn over cop slots (NULL = none). */
void td5_render_set_cop_mesh(TD5_MeshHeader *mesh);

/* --- Projection --- */
void td5_render_configure_projection(int width, int height);
void td5_render_set_projection_center(float cx, float cy);
void td5_render_recompute_frustum_for_trackside(void);

/**
 * Transform a model-space point through the current render transform
 * (3x4 matrix) and perspective-project to screen coordinates.
 * Returns 1 on success, 0 if the point is behind the near clip plane.
 */
int td5_render_transform_and_project(float mx, float my, float mz,
                                     float *sx, float *sy, float *sz, float *rhw);

/* --- Debug line overlay (collision wireframe) ---
 * Submits world-space line segments. Each call appends one segment to an
 * internal batch. Call td5_render_debug_lines_flush() once per frame after
 * the main world pass to render and reset the batch.
 *
 * argb is a 32-bit packed color (0xAARRGGBB); alpha is ignored (lines draw
 * opaque) but must be 0xFF for vertex-color modulation to land at full
 * intensity. */
void td5_render_debug_line_world(float x0, float y0, float z0,
                                 float x1, float y1, float z1,
                                 uint32_t argb);
void td5_render_debug_lines_flush(void);
void td5_render_debug_lines_reset(void);

/* [dynamic-traffic] Whole-actor draw fade (0 = invisible, 255 = opaque /
 * normal). Flushes the pending immediate batch on change so one actor's
 * faded triangles can never batch with another's. Consumed by the flush
 * fixup (vertex alpha scale + VEHICLE_FADE preset remap) and the direct-draw
 * car accessories (shadow). Always reset to 255 after the bracketed draws. */
void td5_render_set_actor_draw_alpha(int alpha);

/* --- Translucent pipeline --- */
void td5_render_init_translucent_pipeline(void);
void td5_render_queue_translucent_batch(void *record);
void td5_render_flush_translucent(void);
void td5_render_flush_immediate_batch(void);

/* [ARCADE] Draw the glowing power-up pads + dropped hazards for the current
 * viewport. Call from the per-view world-FX pass (after particles). No-op
 * outside ARCADE mode. */
void td5_render_arcade_pads(void);

/* --- Depth-sorted pipeline (4096 buckets) --- */
void td5_render_queue_projected_entry(void *entry, int bucket, uint32_t flags, int texture_page);
void td5_render_flush_projected_buckets(void);

/* --- Deferred additive pass ---
 * Begin before the opaque world pass (enables deferral of type-3 batches
 * inside flush_immediate_internal). Flush after the opaque world AND the
 * projected-bucket flush have drained, so lights composite on top of
 * everything behind them including alpha-keyed trees. */
void td5_render_begin_world_pass(void);
void td5_render_flush_deferred_additive(void);

/* [Phase B Stage 2b] Per-pane render-scratch pool for concurrent pane recording.
 * pool_ensure lazily allocates `count` RenderScratch instances (1=ok, 0=OOM).
 * bind(i) points THIS thread's scratch at instance i; unbind() restores the
 * shared default. Workers bind their pane index before recording; the serial
 * path never calls these (stays on the default instance). */
int  td5_render_scratch_pool_ensure(int count);
void td5_render_scratch_bind(int index);
void td5_render_scratch_unbind(void);
/* Bake the camera module's current basis/pos into the bound g_rs (per-pane camera
 * for threaded panes). td5_render_set_camera_prebaked(1) makes render_actors use
 * that baked camera instead of re-reading the shared current snapshot. */
void td5_render_bake_camera(void);
void td5_render_set_camera_prebaked(int on);
void td5_render_log_pane_proj(int vp);   /* [diag] log per-pane projection inputs */

/* --- Texture cache --- */
void td5_render_reset_texture_cache(void);
void td5_render_advance_texture_ages(void);
int  td5_render_bind_texture_page(int page_id);

/* --- Vehicle Projection Effect / Chrome Reflection (0x43DEC0) --- */
int  td5_render_load_environs_textures(int level_number);
void td5_render_apply_mesh_projection_effect(TD5_MeshHeader *mesh, int slot);
void td5_render_update_projection_effect(int slot, TD5_Actor *actor);

/* --- 4-Pass Race Rendering (0x40B070) --- */

/**
 * Race render pass IDs, matching original SetRaceRenderStatePreset param.
 *   Pass 0 (SKY):     MODULATEALPHA texture blend, alpha blend OFF
 *   Pass 1 (OPAQUE):  COPY texture blend, alpha blend ON
 *   Pass 3 (ALPHA):   COPY texture blend, alpha blend OFF
 */
typedef enum TD5_RaceRenderPass {
    TD5_RACE_PASS_SKY     = 0,
    TD5_RACE_PASS_OPAQUE  = 1,
    TD5_RACE_PASS_UNUSED  = 2,
    TD5_RACE_PASS_ALPHA   = 3
} TD5_RaceRenderPass;

void td5_render_set_race_pass(TD5_RaceRenderPass pass);

/* --- Per-Tick Fog Fade (0x40A490) --- */

/**
 * Per-tick fog fade management during scene transitions.
 * Manages fog_transition_counter and calls td5_render_set_fog_level().
 */
void td5_render_per_tick_fog_fade(void);

/**
 * Set fog intensity level for a viewport.
 * level=0 disables fog, level>0 scales fog start/end.
 */
void td5_render_set_fog_level(int viewport, int level);

/**
 * Set fog transition counter for scene fade effect.
 */
void td5_render_set_fog_transition(int counter);
int  td5_render_get_fog_transition(void);

/* --- Sky --- */
void td5_render_advance_sky_rotation(void);
void td5_render_load_sky(const char *path);
void td5_render_draw_sky(void);
/* [AUTO LIGHTS] Average luminance (0..255) of the loaded sky texture, or -1 if
 * none. Per-track brightness baseline for the auto-headlight verdict. */
float td5_render_sky_luma(void);

/* --- Billboard animation --- */
void td5_render_advance_billboard_anims(void);

/* --- HUD / sprite pipeline --- */
/* 0x432BD0: Build sprite quad template from layout params */
void td5_render_build_sprite_quad(int *params);
/* 0x4315B0: Submit immediate translucent primitive */
void td5_render_submit_translucent(uint16_t *quad_data);
void td5_render_submit_translucent_low_ref(uint16_t *quad_data);
void td5_render_submit_translucent_hud(uint16_t *quad_data);
void td5_render_submit_translucent_world(uint16_t *quad_data);
void td5_render_submit_tire_mark(uint16_t *quad_data);
/* Active projection parameters (so VFX billboards size/place quads with the
 * SAME focal/center the world geometry uses). */
float td5_render_get_focal_length(void);
float td5_render_get_center_x(void);
float td5_render_get_center_y(void);
/* 0x43E640: Set viewport clip rect */
void td5_render_set_clip_rect(float left, float right, float top, float bottom);
/* 0x439E60: Render radial pulse overlay effect */
void td5_render_radial_pulse(float dt);

/* --- Render dimension globals (defined in td5_render.c) --- */
extern float g_render_width_f;
extern float g_render_height_f;
extern int   g_render_width;
extern int   g_render_height;

/* --- Math helpers (defined in td5_render.c) ---
 * 12-bit angle trig + rotation/basis utilities used by camera, physics, track.
 * Original 0x43DA80..0x43DEC0 region. */
float CosFloat12bit(unsigned int angle);
float SinFloat12bit(int angle);
int   CosFixed12bit(unsigned int angle);
int   SinFixed12bit(int angle);
int   AngleFromVector12(int x, int z);
void  MultiplyRotationMatrices3x3(float *A, float *B, float *out);
void  TransformVector3ByBasis(float *matrix, void *vec, int *out);
/* [FIX 2026-05-24 OVERSIGHT: case_1_2_basis_transform; orig 0x0042DB40]
 * Same math as TransformVector3ByBasis but clamps each output coordinate
 * to int16 range via (int)(short) cast (matches orig __ftol + (int)(short)). */
void  ConvertFloatVec3ToIntVec3(float *matrix, void *vec, int *out);
void  BuildRotationMatrixFromAngles(float *out, short *angles);
void  ConvertFloatVec3ToShortAngles(short *in, short *out);
void  LoadRenderRotationMatrix(float *matrix);

#endif /* TD5_RENDER_H */
