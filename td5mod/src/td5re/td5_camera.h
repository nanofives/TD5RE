/**
 * td5_camera.h -- Chase cam, trackside cam, spline cam, camera transforms
 *
 * 7 chase camera presets at 0x463098 (16 bytes each).
 * Camera basis: right/up/forward (3x3 float) + position (3 floats).
 *
 * Original functions:
 *   0x401450  LoadCameraPresetForView
 *   0x401590  UpdateChaseCamera
 *   0x401950  UpdateTracksideOrbitCamera
 *   0x401C20  UpdateVehicleRelativeCamera
 *   0x401E10  UpdateRaceCameraTransitionState
 *   0x402000  ResetRaceCameraSelectionState
 *   0x4020B0  InitializeTracksideCameraProfiles
 *   0x402200  SelectTracksideCameraProfile
 *   0x402480  UpdateTracksideCamera
 *   0x402950  UpdateStaticTracksideCamera
 *   0x402A80  CacheVehicleCameraAngles
 *   0x402AD0  UpdateSplineTracksideCamera
 *   0x402E00  CycleRaceCameraPreset
 *   0x42CE50  SetCameraWorldPosition
 *   0x42D0B0  BuildCameraBasisFromAngles
 *   0x42D410  FinalizeCameraProjectionMatrices
 *   0x42D5B0  OrientCameraTowardTarget
 */

#ifndef TD5_CAMERA_H
#define TD5_CAMERA_H

#include "td5_types.h"

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_camera_init(void);
void td5_camera_shutdown(void);
void td5_camera_tick(void);

/* Split camera-tick entry points — matches original RunRaceFrame order:
   Cache angles BEFORE physics, chase update AFTER physics. */
void td5_camera_cache_angles(void);
void td5_camera_update_chase_all(void);

/* [CAMERA REWRITE 2026-06-07] Unified FPS-independent pipeline.
 *  - solve_tick_all: run once per fixed 30 Hz sim tick (replaces the in-loop
 *    td5_camera_update_chase_all call) — solves every view's desired pose.
 *  - apply_view: run once per viewport per render frame (replaces the in-render-
 *    loop td5_camera_update_transition_state call) — interpolates + builds basis.
 *  - snap_poses: re-seed prev=cur so the next frame doesn't glide across a
 *    teleport (race start / preset change / resume-from-pause).
 * Each transparently falls back to the legacy path when TD5RE_CAM_NEW=0. */
void td5_camera_solve_tick_all(void);
void td5_camera_apply_view(int view);
void td5_camera_snap_poses(void);

/* Per-render-frame camera position finalization. Writes g_camWorldPos[v]
   from current orbit state + vel*subTickFraction so the camera stays
   synchronized with the car-mesh render extrapolation every render frame
   (not just sim-tick frames). */
void td5_camera_finalize_chase_pos(TD5_Actor *actor, int view);
void td5_camera_finalize_all(void);

/* Re-anchor (snap) the per-render-frame chase-camera smoothing so the next
   finalize does NOT glide across a discontinuity. Call on race start / preset
   change / resume-from-pause. Render-only; never feeds the sim. */
void td5_camera_snap_smoothing(void);

/* Camera globals owned by td5_camera.c, read by the HUD/render side.
 *  g_subTickFraction     - render-frame extrapolation fraction [0,1).
 *  g_camWorldPos[v]       - finalized per-viewport camera world position.
 *  g_cameraTransitionActive - pre-race countdown fly-in timer (0 = running). */
extern float g_subTickFraction;
extern int   g_camWorldPos[TD5_MAX_VIEWPORTS][3];
extern int   g_cameraTransitionActive;

/* ========================================================================
 * Camera preset table (7 presets x 16 bytes at 0x463098)
 *
 * Layout per entry:
 *   +0x00 short  mode          (0=chase orbit, 1=bumper/vehicle-relative)
 *   +0x02 short  elevation     (12-bit angle, stored << 8 = 20-bit)
 *   +0x04 short  radius_raw    (* 256.0 -> float orbit distance)
 *   +0x06 short  height_raw    (* 256.0 -> float target height)
 *   +0x08 int    extra_param_1
 *   +0x0C int    extra_param_2
 * ======================================================================== */

typedef struct TD5_CameraPreset {
    int16_t  mode;
    int16_t  elevation_angle;
    int16_t  orbit_radius_raw;
    int16_t  height_target_raw;
    int32_t  extra_param_1;
    int32_t  extra_param_2;
} TD5_CameraPreset;

#define TD5_CAMERA_PRESET_COUNT  14  /* 0-6: normal, 7-9: unused, 10-13: countdown fly-in */

/* ========================================================================
 * Trackside camera profile (16 bytes per entry, terminated by -1)
 *
 *   +0x00 short  behavior_type  (0-10)
 *   +0x02 short  span_range_start
 *   +0x04 short  span_range_end
 *   +0x06 short  anchor_span
 *   +0x08 short  span_offset
 *   +0x0A short  height_param
 *   +0x0C short  spline_speed    (type 6 only)
 *   +0x0E short  spline_nodes    (type 6 only)
 * ======================================================================== */

typedef struct TD5_TracksideProfile {
    int16_t  behavior_type;
    int16_t  span_range_start;
    int16_t  span_range_end;
    int16_t  anchor_span;
    int16_t  span_offset;
    int16_t  height_param;
    int16_t  spline_speed;
    int16_t  spline_node_count;
} TD5_TracksideProfile;

/* ========================================================================
 * Chase camera
 * ======================================================================== */

/** Load camera preset for a viewport. (0x401450)
 *  @param actor        pointer to actor struct
 *  @param force_reload nonzero to reset interpolation targets
 *  @param view         viewport index (0 or 1)
 *  @param save_state   nonzero to persist preset ID into packed save bytes
 */
void LoadCameraPresetForView(int actor, int force_reload, int view, int save_state);

/** Main chase camera update. (0x401590)
 *  @param actor           pointer to actor struct
 *  @param do_track_heading  1 = update orbit angle tracking vehicle heading
 *  @param view            viewport index
 */
void UpdateChaseCamera(int actor, int do_track_heading, int view);

/** Orbit camera around a vehicle (trackside replay mode). (0x401950) */
void UpdateTracksideOrbitCamera(int actor, int is_active, int view);

/** Vehicle-relative camera (bumper/in-car). (0x401C20) */
void UpdateVehicleRelativeCamera(int actor, int view);

/** Race camera transition state machine. (0x401E10) */
void UpdateRaceCameraTransitionState(int actor, int view);

/** Reset camera selection state. (0x402000)
 *  @param clear_or_restore  0=restore from packed save, nonzero=reset to defaults
 */
void ResetRaceCameraSelectionState(int clear_or_restore);

/** Initialize trackside camera profiles from track data. (0x4020B0) */
void InitializeTracksideCameraProfiles(void);

/** Select appropriate trackside camera profile based on vehicle span. (0x402200) */
void SelectTracksideCameraProfile(int actor, int view);

/** Master trackside camera dispatcher. (0x402480) */
void UpdateTracksideCamera(int actor, int view);

/** Static trackside camera with look-at target. (0x402950) */
void UpdateStaticTracksideCamera(int actor, int view);

/** Cache vehicle orientation angles into per-view state. (0x402A80) */
void CacheVehicleCameraAngles(int actor, int view);

/** Spline-based trackside camera. (0x402AD0) */
void UpdateSplineTracksideCamera(int actor, int view, int spline_type);

/** Cycle the camera preset for a view. (0x402E00)
 *  @return  the wrap count (preset / 7)
 */
int CycleRaceCameraPreset(int view, int delta);

/* ========================================================================
 * Camera transform pipeline
 * ======================================================================== */

/** Set camera world position from integer coordinates. (0x42CE50) */
void SetCameraWorldPosition(int *pos);

/** Build camera basis matrices from Euler angles {pitch, yaw, roll}. (0x42D0B0) */
void BuildCameraBasisFromAngles(short *angles);

/** Finalize projection matrices (scale + copy). (0x42D410) */
void FinalizeCameraProjectionMatrices(void);

/** Orient camera to look at a target position. (0x42D5B0) */
void OrientCameraTowardTarget(int *target_pos, unsigned int yaw_offset);

/* ========================================================================
 * High-level module API (wrappers used by td5_game / td5_render)
 * ======================================================================== */

void td5_camera_update_chase(TD5_Actor *actor, int player, int view_index);
void td5_camera_set_preset(int preset_index);
void td5_camera_set_rear_view(int view, int active);

/* Snap this viewport's chase-cam yaw offset back to normal (behind the car).
 * Clears any residual spin left by the CarDamage finish-orbit (knockout) once
 * the car has recovered. No-op on an out-of-range view. */
void td5_camera_reset_yaw_offset(int view);
/* [CAR BROKE DOWN 2026-07-10] Begin a render-only chase-camera glide from the
 * last-emitted pose to the live follow (used when a breakdown recovery teleports
 * the car 30 spans back). No-op if the view has no prior pose or the knob is 0. */
void td5_camera_begin_recovery_glide(int view);
void td5_camera_update_trackside(TD5_Actor *actor, int view_index);
void td5_camera_update_transition_state(int player, int view_index);
void td5_camera_update_transition_timer(void);
void td5_camera_cache_vehicle_angles(TD5_Actor *actor, int view_index);
void td5_camera_get_position(float *x, float *y, float *z);
void td5_camera_get_basis(float *right, float *up, float *forward);

/* Bind the strip-span / vertex tables for trackside-camera dispatch.
 * Mirrors the original BindTrackStripRuntimePointers (0x444070), which wires
 * g_trackStripRecords (0x4c3d9c) + g_trackVertexPool (0x4c3d98) consumed by
 * UpdateTracksideCamera/UpdateSplineTracksideCamera. Called by
 * td5_track_load_strip after the strip blob is parsed. Pass NULL to clear
 * (used when the strip is unloaded between races). */
void td5_camera_bind_track_geometry(const void *span_base,
                                    const void *vertex_base);

/* Bind the active per-track trackside-camera profile table (orig
 * LoadTrackRuntimeData @0x42fd4b: gTracksideCameraProfiles =
 * g_perTrackTracksideCameraProfilePtrs[track_pool_index - 1]). pool_index is the
 * 1-based track-pool index (see td5_asset_track_pool_index); 0 or out-of-range
 * clears the pointer (→ replay falls back to chase). Call at race init before
 * InitializeTracksideCameraProfiles. */
void td5_camera_bind_trackside_profiles(int pool_index_1based);

/* True when the replay trackside camera has usable profile data for this race. */
int  td5_camera_replay_trackside_ready(void);

#endif /* TD5_CAMERA_H */
