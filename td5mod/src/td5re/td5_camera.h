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

#define TD5_CAMERA_PRESET_COUNT  7

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
void td5_camera_update_trackside(TD5_Actor *actor, int view_index);
void td5_camera_update_transition_state(int player, int view_index);
void td5_camera_update_transition_timer(void);
void td5_camera_cache_vehicle_angles(TD5_Actor *actor, int view_index);
void td5_camera_get_position(float *x, float *y, float *z);
void td5_camera_get_basis(float *right, float *up, float *forward);

#endif /* TD5_CAMERA_H */
