/**
 * td5_camera.c -- Chase cam, trackside cam, spline cam, camera transforms
 *
 * Translated from TD5_d3d.exe decompilation. All functions preserve exact
 * original behavior using the 12-bit angle system (4096 = 360 degrees)
 * and fixed-point math.
 */

#include "td5_camera.h"
#include "td5_render.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_damage.h"   /* [CAR DAMAGE] end-of-race orbit around a finished/wrecked car */
#include "td5_platform.h"
#include "td5_hud.h"
#include "td5_ai.h"     /* td5_compute_heading_delta */
#include "td5_physics.h" /* td5_physics_get_crash_fx — crash-shake driver (Item #12) */
#include "td5re.h"
#include "td5_config.h"   /* shared TD5RE_* env-knob helpers */
/* Per-track trackside (replay) camera profile data, extracted from
 * TD5_d3d.exe @0x473780 (re/tools/extract_trackside_cam_profiles.py).
 * Defines s_per_track_camera_profiles[] + TD5_PER_TRACK_CAMERA_PROFILE_COUNT.
 * Include in exactly this one TU (the arrays are static). */
#include "td5_camera_profiles.h"
#include "../../../re/include/td5_actor_struct.h"  /* TD5_TrackProbeState — camera terrain probes walk via td5_track_update_probe_position */

#include <math.h>
#include <stdlib.h>
#include <stdio.h>   /* FILE/fopen/fprintf — used by the [Trace] TerrainCamProbe
                       CSV mirror in UpdateChaseCamera (gated by
                       g_td5.ini.trace_terrain_cam_probe). Inert when 0. */
#include <string.h>

#define LOG_TAG "camera"

static const char *trackside_behavior_name(int btype);

extern int g_replay_mode;

/* Forward declarations for functions defined at end of this file */
static void BuildCubicSpline3D(int *spline_state, int control_points);
static void EvaluateCubicSpline3D(int *out_pos, int *spline_state, int t);
static void RecomputeTracksideProjectionScale(void);
static void UpdateCameraTransitionHudIndicator(int view, int actor_index);

/* ========================================================================
 * Camera Globals (migrated from td5re_stubs.c — owned by this module)
 * ======================================================================== */

/* --- World scale --- */
float  g_worldToRenderScale = 0.00390625f;  /* 1/256 */
float  g_subTickFraction    = 0.0f;

/* --- Projection / FOV --- */
float  g_projectionScale    = 1.0f;
int    g_depthFovFactor     = 0;
float  g_projConst1         = 1.0f;
float  g_projDepthScale     = 65000.0f;
float  g_projFovScale       = 0.00024414063f; /* 1/4096 */
int    g_projDepthInt       = 0;
float  g_invDistScale       = 1.0f;

/* --- Camera output matrices --- */
float  g_cameraBasis[9]     = { 1,0,0, 0,1,0, 0,0,1 };
float  g_cameraPos[3]       = { 0, 0, 0 };
float  g_cameraBasisWork[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };
float  g_cameraSecondary[9] = { 1,0,0, 0,1,0, 0,0,1 };
/* Snapshot of g_cameraSecondary BEFORE FinalizeCameraProjectionMatrices
 * applies inv_proj * fov_factor scaling. Source-port use only: the
 * render basis (s_camera_basis in td5_render.c) is the unscaled
 * g_cameraBasis, so the billboard branch needs an unscaled yaw-stripped
 * counterpart to live in the same coordinate space as the rotation
 * already loaded in s_render_transform. */
float  g_cameraSecondaryUnscaled[9] = { 1,0,0, 0,1,0, 0,0,1 };
float  g_cameraTertiary[9]  = { 1,0,0, 0,1,0, 0,0,1 };

/* --- Per-view camera state arrays ---
 * [PORT ENHANCEMENT] grown from [2] to [TD5_MAX_VIEWPORTS] for N-way split. */
int    g_camElevationAngleFP[TD5_MAX_VIEWPORTS]     = {0};
short  g_camCachedAngles[TD5_MAX_VIEWPORTS][4]      = {{0}};
short  g_camOffsetVec[TD5_MAX_VIEWPORTS][4]         = {{0}};
float  g_camSmoothedHeight[TD5_MAX_VIEWPORTS]       = {0};
int    g_camHeightSampleOfs[TD5_MAX_VIEWPORTS]      = {0};
int    g_camTrackSpanOfs[TD5_MAX_VIEWPORTS]         = {0};
int    g_camBehaviorType[TD5_MAX_VIEWPORTS]         = {0};
float  g_camOrbitRadiusScale[TD5_MAX_VIEWPORTS]     = {0};
int    g_camOrbitOffset[TD5_MAX_VIEWPORTS][3]       = {{0}};
int    g_camFlyInCounter[TD5_MAX_VIEWPORTS]         = {0};
int    g_camRotationSlot[TD5_MAX_VIEWPORTS]         = {0};
float  g_camCurrentRadius[TD5_MAX_VIEWPORTS]        = {0};
int    g_camSplineParam[TD5_MAX_VIEWPORTS]          = {0};
float  g_camTargetHeight[TD5_MAX_VIEWPORTS]         = {0};
int    g_camOrbitAngleFP[TD5_MAX_VIEWPORTS]         = {0};
int    g_camAnchorSpan[TD5_MAX_VIEWPORTS]           = {0};
int    g_camPresetChangeFlag[TD5_MAX_VIEWPORTS]     = {0};
int    g_camWorldPos[TD5_MAX_VIEWPORTS][3]          = {{0}};
short  g_camOrientShort[TD5_MAX_VIEWPORTS][4]       = {{0}};
int    g_camHeadingDelta20[TD5_MAX_VIEWPORTS]       = {0};
int    g_camSplineNodeCount[TD5_MAX_VIEWPORTS]      = {0};
int    g_camYawOffset[TD5_MAX_VIEWPORTS]            = {0};
int    g_camReserved78[TD5_MAX_VIEWPORTS]           = {0};
int    g_camSplineAdvRate[TD5_MAX_VIEWPORTS]        = {0};
int    g_camAnchorX[TD5_MAX_VIEWPORTS]              = {0};
int    g_camHeightParam[TD5_MAX_VIEWPORTS]          = {0};
int    g_camAnchorZ[TD5_MAX_VIEWPORTS]              = {0};
int    g_camStoredPitch[TD5_MAX_VIEWPORTS]          = {0};

/* --- Scalar camera globals (per-view) --- */
int    g_raceCameraPresetId[TD5_MAX_VIEWPORTS]      = {0};
int    g_raceCameraPresetMode[TD5_MAX_VIEWPORTS]    = {0};
int    g_cameraProfileIndex[TD5_MAX_VIEWPORTS]      = {0};
int    g_cameraLastProjScale[TD5_MAX_VIEWPORTS]     = {0};
int    g_cameraProjDist[TD5_MAX_VIEWPORTS]          = {0};
int    g_cameraProjScaleComp[TD5_MAX_VIEWPORTS]     = {0};
int    g_cameraPrevPresetId[TD5_MAX_VIEWPORTS]      = {0};

unsigned char g_camPackedSave[TD5_MAX_VIEWPORTS]    = {0};

/* --- Trackside --- */
int    g_tracksideCameraProfileCount = 0;
short *g_tracksideCameraProfiles     = NULL;
int    g_tracksideTimer[TD5_MAX_VIEWPORTS]           = {0};
unsigned int g_tracksideYawOffset[TD5_MAX_VIEWPORTS] = {0};

/* --- Track geometry tables --- */
int    g_spanTable      = 0;
int    g_vertexTable    = 0;

/* --- Race state (camera-owned) --- */
int    g_cameraTransitionActive = 0xA000;
int    g_camTransitionGate      = 0;
static int s_flyin_preset_reloaded[TD5_MAX_VIEWPORTS] = {0};
extern int    g_actorSlotForView[TD5_MAX_VIEWPORTS];     /* td5_game.c */
extern int    g_actorBaseAddr;           /* td5_game.c */
extern uint8_t *g_actor_table_base;      /* td5_game.c */
extern int    g_game_type;               /* td5_game.c — g_selectedGameType equivalent */
int    g_trackType              = 0;
unsigned char g_actorAliveTable[TD5_MAX_TOTAL_ACTORS] = {0};
int    g_lookLeftRight[TD5_MAX_VIEWPORTS]       = {0};

/* --- Camera spline workspace --- */
int    g_camSplineState[TD5_MAX_VIEWPORTS][15]  = {{0}};

/* --- Constants in .data --- */
int    g_flyInThreshold  = 40;
float  g_const256        = 256.0f;
float  g_const32         = 32.0f;
float  g_dampWeight      = 0.125f;
float  g_nearZeroThreshold = 0.001f;

/* ========================================================================
 * Per-render-frame chase-camera smoothing snapshots [PORT — render-only]
 *
 * The chase spring (radius/height/pitch lerp at g_dampWeight, the 32.0 radius
 * term, the pitch >>3) is byte-faithful and runs ONCE per fixed 30 Hz sim
 * sub-tick inside UpdateChaseCamera — countdown AND race. The ORIGINAL ran
 * render==sim at 30 Hz, so it consumed the spring output directly. The port
 * renders faster than 30 Hz, so consuming the spring output un-interpolated
 * staircases it once per tick: visible as high-MS / low-FPS jitter, and — far
 * worse — as countdown bob, because simulation_tick_counter is FROZEN at 0 for
 * the whole countdown so the old tick-counter-keyed interpolation never
 * re-anchored.
 *
 * Fix: snapshot the spring OUTPUT (orbit offset, smoothed height) + the car's
 * vertical pose once per sub-tick (in UpdateChaseCamera, the producer), then
 * interpolate prev->cur by g_subTickFraction in finalize_chase_pos (the
 * consumer). This mirrors the original's own UpdateTracksideOrbitCamera, which
 * scales its height smoothing by the sub-tick fraction (@0x00401950). Keying on
 * the per-sub-tick producer (not simulation_tick_counter) is what makes the
 * countdown smooth. Render-only: the camera never feeds the sim, so this cannot
 * change physics / AI / replay determinism. Toggle with TD5RE_SMOOTH_CAM=0. */
static int   s_camSmoothInit[TD5_MAX_VIEWPORTS]   = {0};
static int   s_camOffPrev[TD5_MAX_VIEWPORTS][3]   = {{0}};
static int   s_camOffCur [TD5_MAX_VIEWPORTS][3]   = {{0}};
static float s_camHPrev  [TD5_MAX_VIEWPORTS]      = {0};
static float s_camHCur   [TD5_MAX_VIEWPORTS]      = {0};
static int   s_camYPrev  [TD5_MAX_VIEWPORTS]      = {0};
static int   s_camYCur   [TD5_MAX_VIEWPORTS]      = {0};

/* Re-anchor one view's smoothing (prev := cur on next producer call). */
static void td5_camera_snap_smoothing_view(int view);

/* ========================================================================
 * [CAMERA REWRITE 2026-06-07] Unified FPS-independent camera pose pipeline.
 *
 * Every camera mode's dynamics (chase spring, fly-in orbit, bumper angle
 * follow, trackside orbit/static/spline, replay profile selection) now run
 * ONCE per fixed 30 Hz sim tick in td5_camera_solve_tick_all(). Each mode's
 * existing updater is reused as-is: we run it with g_subTickFraction forced to
 * 0 (so its velocity-extrapolation terms vanish — extrapolation is re-applied
 * per render frame) and CAPTURE the eye / look-at / euler-angles it feeds to
 * SetCameraWorldPosition / OrientCameraTowardTarget / BuildCameraBasisFromAngles
 * into a per-view tick pose. The orbit/height/angle integrators that the port
 * had smeared across render frames via g_subTickFraction now advance a full
 * step per tick (see cam_integ_step()).
 *
 * td5_camera_apply_view() then, once per render frame per viewport, interpolates
 * prev->cur by g_subTickFraction and RE-PINS the car-locked components to the
 * exact body-mesh extrapolation (world_pos + linear_velocity*subtick for X, Y
 * AND Z, matching RenderRaceActorForView @ 0x40C164 / td5_render.c:3955-3957).
 * [WOBBLE FIX 2026-06-29] Y was previously sub-tick INTERPOLATED (lerp prev->cur)
 * while the body EXTRAPOLATED, so the car was pinned horizontally but bobbed
 * vertically against its own mesh once per 30 Hz tick — a beat that reads as a
 * fast vertical jitter on high-refresh monitors. Now all three axes extrapolate
 * to match the body. Result: identical motion at any FPS, and the followed car
 * is pixel-locked in frame (no wobble). TD5RE_CAM_PIN_Y=0 reverts Y to the old
 * interpolation for A/B testing.
 *
 * Legacy per-frame path (td5_camera_finalize_all + td5_camera_update_transition_state)
 * is preserved and selected when TD5RE_CAM_NEW=0 for A/B testing.
 * ======================================================================== */

typedef struct {
    int   eye[3];           /* captured world-space eye (24.8 FP) at the tick   */
    int   target[3];        /* captured look-at target (build_mode 0)           */
    short angles[3];        /* captured euler basis angles (build_mode 1=bumper)*/
    int   anchor[3];        /* followed car world_pos at this tick (re-pin base)*/
    int   yaw_offset;       /* OrientCameraTowardTarget yaw offset              */
    int   fov_factor;       /* g_depthFovFactor snapshot (trackside FOV)        */
    unsigned char build_mode;     /* 0 = orient-toward-target, 1 = euler basis  */
    unsigned char eye_car_locked; /* 1 = eye re-pins to body extrapolation      */
    unsigned char tgt_car_locked; /* 1 = look-at re-pins to body extrapolation  */
    unsigned char anchor_valid;   /* 0 = no followed car (debug / no actor)     */
    unsigned char valid;          /* a pose has been solved for this view       */
} TD5_CamPose;

static TD5_CamPose s_cam_pose_prev[TD5_MAX_VIEWPORTS];
static TD5_CamPose s_cam_pose_cur [TD5_MAX_VIEWPORTS];
static int         s_cam_pose_init[TD5_MAX_VIEWPORTS] = {0};
static int         s_cam_capture_view = -1;   /* >=0 => sinks also record into s_cam_pose_cur[view] */
/* g_depthFovFactor is defined above (camera projection FOV). */

/* TD5RE_CAM_NEW (default 1): 0 falls back to the legacy per-frame camera path. */
static int td5_camera_use_new_pipeline(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_CAM_NEW");
    }
    return s_mode;
}

/* Integrator step for the orbit/height/angle accumulators that the port had
 * scaled by g_subTickFraction (per-frame smear). New pipeline: advance a full
 * step per tick (1.0). Legacy path: keep the per-frame sub-tick smear. */
/* [WOBBLE FIX 2026-06-29] Pin the chase-camera follow anchor's Y to the body
 * mesh's velocity extrapolation (world_pos.y + vel_y*subtick) instead of sub-tick
 * interpolating prev->cur. Eliminates the high-refresh vertical jitter at race
 * start (see td5_camera_apply_view). Default ON; TD5RE_CAM_PIN_Y=0 reverts to the
 * old interpolation so the cause can be A/B confirmed in a single build. */
static int td5_camera_pin_anchor_y(void)
{
    static int s_pin = -1;
    if (s_pin < 0) {
        s_pin = td5_env_flag_on("TD5RE_CAM_PIN_Y");   /* default ON */
        TD5_LOG_I(LOG_TAG, "camera anchor-Y pin: %s (TD5RE_CAM_PIN_Y)",
                  s_pin ? "ON (extrapolate Y like body mesh)"
                        : "off (legacy prev->cur interpolation)");
    }
    return s_pin;
}

static float cam_integ_step(void)
{
    return td5_camera_use_new_pipeline() ? 1.0f : g_subTickFraction;
}

/* ========================================================================
 * [TASK 22] TD5RE_GROUND_CAM_FIX (default 1) — fix the in-car / "ground"
 * (bumper) camera coming out upside-down in the default new pipeline.
 *
 * Root cause: the in-car/bumper view (preset 6, mode 1) and the trackside
 * replay offset views (behaviour types 1/2/7/8) build their basis from the
 * car's Euler angles via BuildCameraBasisFromAngles (0x42d0b0), which applies
 * NO coordinate flip. The port's D3D11 transform consumes g_cameraBasis with
 * the opposite up/right handedness, so that un-flipped basis renders rolled
 * 180° about forward = upside down. UpdateVehicleRelativeCamera compensates
 * with a 180°-about-forward roll (negate right + up rows), but in the NEW
 * pipeline (TD5RE_CAM_NEW=1, default) td5_camera_apply_view rebuilds the basis
 * per render frame from the captured angles WITHOUT re-applying that flip, so
 * the compensation is discarded and the in-car cam is upside-down again.
 *
 * Fix: re-apply the same handedness flip in apply_view's euler-basis branch
 * (and, for consistency, give the trackside offset cams the same flip), plus a
 * small upward lift so the bumper eye sits at a usable hood/window height
 * instead of scraping the ground. "0" restores the (buggy) old behaviour.
 * ======================================================================== */
static int td5_camera_ground_cam_fix(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_GROUND_CAM_FIX");   /* default ON */
    }
    return s_mode;
}

/* Upward lift (24.8 FP world units) applied to the in-car/bumper eye so it
 * sits at roughly windscreen height rather than at the ground. ~150 world
 * units. Overridable via TD5RE_GROUND_CAM_LIFT (world units). */
static int td5_camera_ground_cam_lift(void)
{
    static int s_lift = -1;
    if (s_lift < 0) {
        int wu = td5_env_int("TD5RE_GROUND_CAM_LIFT", 150, 0, 100000);  /* ~150 world units */
        s_lift = wu << 8;                        /* world units -> 24.8 FP */
    }
    return s_lift;
}

/* Apply the D3D11 in-car handedness correction: a 180° roll about forward
 * (negate the right + up rows of g_cameraBasis, keep forward). Matches the
 * compensation baked into UpdateVehicleRelativeCamera. Determinant preserved;
 * look direction unchanged. */
static void cam_apply_incar_handedness_flip(void)
{
    g_cameraBasis[0] = -g_cameraBasis[0];   /* right.x */
    g_cameraBasis[1] = -g_cameraBasis[1];   /* right.y */
    g_cameraBasis[2] = -g_cameraBasis[2];   /* right.z */
    g_cameraBasis[3] = -g_cameraBasis[3];   /* up.x */
    g_cameraBasis[4] = -g_cameraBasis[4];   /* up.y */
    g_cameraBasis[5] = -g_cameraBasis[5];   /* up.z */
}

/* ========================================================================
 * [TASK 19] TD5RE_CAM_WALL_AVOID (default 1) — pull the camera in before it
 * clips through a wall/rail, in BOTH gameplay (chase) and replay (trackside)
 * views. The 2D segment raycast lives in td5_camera_raycast_to_wall (further
 * down). The clip is applied:
 *   - per render frame in td5_camera_apply_view for the new pipeline (covers
 *     chase + trackside replay look-at cameras), and
 *   - in td5_camera_finalize_chase_pos for the legacy per-frame chase path.
 * "0" disables the avoidance (camera may clip walls, as the original does).
 * ======================================================================== */
static int td5_camera_wall_avoid(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_CAM_WALL_AVOID");   /* default ON */
    }
    return s_mode;
}

/* ========================================================================
 * [ITEM #12] TD5RE_CRASH_SHAKE (default 1) — decaying screen shake on a
 * recent heavy crash. Consumes the per-slot crash sequence published by the
 * physics module via td5_physics_get_crash_fx(slot, &mag, &age): when the
 * sequence id is non-zero and the crash is recent (age < SHAKE_DECAY ticks)
 * we add a small, decaying positional jitter (and a tiny angular wobble) to
 * the finished per-frame camera eye in td5_camera_apply_view — the single
 * authoritative per-frame eye-write for the default new pipeline, so the
 * shake never enters the per-tick interpolation base (no determinism /
 * replay impact) and decays smoothly at any FPS.
 *
 * The jitter is DERIVED DETERMINISTICALLY from the sim tick + slot + age
 * (sine of tick*large-prime), NOT rand(), so it stays netplay-safe even
 * though the camera itself never feeds the sim. "0" reverts to no shake
 * (byte-identical camera output). */
static int td5_camera_crash_shake(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_CRASH_SHAKE");   /* default ON */
    }
    return s_mode;
}

/* Shake lifetime in sim ticks (linear falloff to 0 at this age). 24 ticks
 * @30Hz ≈ 0.8s. Matches the "age < 24" recency window in the contract. */
#define TD5_CRASH_SHAKE_DECAY    24
/* Peak positional jitter, 24.8 FP world units, at full intensity (~70 wu). */
#define TD5_CRASH_SHAKE_POS_FP   (70 << 8)
/* Peak angular wobble, 12-bit angle units (0x1000 = full circle); ~3.5 deg. */
#define TD5_CRASH_SHAKE_ANG      40
/* Crash magnitude that maps to full-intensity shake. The physics module only
 * records an event for ACUTE impacts (impact_mag > ~250000, see
 * td5_physics.c CRASH_FX_ACUTE_MAG), so every reported crash already clears
 * the floor; this reference (~4x the acute threshold) gives a usable
 * "big vs huge" gradient above it rather than always saturating. */
#define TD5_CRASH_SHAKE_MAG_REF  1000000

/* Deterministic pseudo-random in [-1,1] from an integer key (sine of a large
 * prime multiple — netplay-safe, no rand()). */
static float cam_crash_noise(uint32_t key)
{
    /* 2654435761u = Knuth's multiplicative hash constant (a large prime-ish
     * odd) — spreads adjacent keys far apart before the sine wrap. */
    float a = (float)(key * 2654435761u & 0xFFFFFFu);
    return sinf(a * 0.000312f);   /* arbitrary irrational-ish freq → decorrelated */
}

/* Compute this view's decaying crash shake. Returns 1 and fills the eye
 * offset (24.8 FP) + angular wobble (12-bit) when a recent crash is active
 * for the view's player slot; 0 otherwise. */
static int td5_camera_compute_crash_shake(int view, int eye_ofs[3], short ang_ofs[3])
{
    if (!td5_camera_crash_shake()) return 0;
    if (view < 0 || view >= TD5_MAX_VIEWPORTS) return 0;

    int slot = g_actorSlotForView[view];
    if (slot < 0) return 0;

    /* [GENTLE FLIP-RECOVERY 2026-06-21] The original never shakes the screen
     * during scripted recovery (vehicle_mode==1) — the crash shake is a port-
     * only addition. Hold it off while this slot is in gentle flip-recovery so
     * the recovery reads as a calm coast, not a rattle. (Physics-side gate:
     * TD5RE_RECOVERY_GENTLE.) */
    if (td5_physics_recovery_shake_suppressed(slot)) return 0;

    int32_t mag = 0;
    int age = 0;
    uint32_t seq = td5_physics_get_crash_fx(slot, &mag, &age);
    if (seq == 0) return 0;                         /* no crash yet */
    if (age < 0 || age >= TD5_CRASH_SHAKE_DECAY) return 0;  /* stale */

    /* Linear decay 1.0 -> 0.0 over the decay window. */
    float decay = 1.0f - (float)age / (float)TD5_CRASH_SHAKE_DECAY;

    /* Magnitude scale (0..1), saturating at the reference impact. */
    if (mag < 0) mag = -mag;
    float mscale = (float)mag / (float)TD5_CRASH_SHAKE_MAG_REF;
    if (mscale > 1.0f) mscale = 1.0f;

    float intensity = decay * mscale;
    if (intensity <= 0.0f) return 0;

    /* Deterministic jitter keyed on tick + slot + axis + crash sequence, so a
     * fresh crash (new seq) re-randomizes and successive ticks decorrelate. */
    uint32_t base = (uint32_t)g_td5.simulation_tick_counter * 977u
                  + (uint32_t)slot * 131u + seq * 0x9E37u;
    float jx = cam_crash_noise(base + 0u);
    float jy = cam_crash_noise(base + 101u);
    float jz = cam_crash_noise(base + 211u);
    float jr = cam_crash_noise(base + 307u);

    float amp = intensity * (float)TD5_CRASH_SHAKE_POS_FP;
    eye_ofs[0] = (int)(jx * amp);
    /* Vertical jolt slightly muted (vertical camera motion reads stronger). */
    eye_ofs[1] = (int)(jy * amp * 0.5f);
    eye_ofs[2] = (int)(jz * amp);

    short wob = (short)(int)(jr * intensity * (float)TD5_CRASH_SHAKE_ANG);
    ang_ofs[0] = 0;        /* leave pitch alone (look direction stays put) */
    ang_ofs[1] = 0;        /* yaw handled via positional jitter */
    ang_ofs[2] = wob;      /* small roll wobble = "rattle" */

    {
        static uint32_t s_shake_log_ctr;
        if ((s_shake_log_ctr++ % 30u) == 0u)
            TD5_LOG_I(LOG_TAG,
                "crash shake v%d slot=%d seq=%u age=%d mag=%d intensity=%.2f",
                view, slot, seq, age, (int)mag, intensity);
    }
    return 1;
}

void td5_camera_solve_tick_all(void);
void td5_camera_apply_view(int view);
void td5_camera_snap_poses(void);
static void update_debug_race_camera(int view);   /* defined later */

/* Camera presets from original binary at 0x463098 (7 entries, 16 bytes each) */
TD5_CameraPreset g_cameraPresets[TD5_CAMERA_PRESET_COUNT] = {
    { 0, 600,  2100, 510, 0, 0 },  /* preset  0: far chase */
    { 0, 550,  1710, 110, 0, 0 },  /* preset  1: medium chase */
    { 0, 475,  1500, 310, 0, 0 },  /* preset  2: close chase high */
    { 0, 400,  1350, 110, 0, 0 },  /* preset  3: close chase low */
    { 0, 325,  1200, 240, 0, 0 },  /* preset  4: tight chase */
    { 0, 240,  1550, 110, 0, 0 },  /* preset  5: wide low */
    { 1, 0,    0,    0,   (int)0xFF380000, 0 },  /* preset  6: bumper cam */
    { 0, 0,    0,    0,   0, 0 },  /* preset  7: unused */
    { 0, 0,    0,    0,   0, 0 },  /* preset  8: unused */
    { 0, 0,    0,    0,   0, 0 },  /* preset  9: unused */
    { 0, 400,  1600, 310, 0, 0 },  /* preset 10: fly-in level>=3 (0x401E10) */
    { 0, 2000, 1400, 110, 0, 0 },  /* preset 11: fly-in level 2  (high elevation side shot) */
    { 0, 300,  1600, 310, 0, 0 },  /* preset 12: fly-in level 1  */
    { 0, 550,  3800, 110, 0, 0 },  /* preset 13: fly-in level 0  (wide pull-back before GO) */
};

/* ========================================================================
 * Spline template table (6 templates x 8 shorts, on stack in original)
 *
 * Each template defines 4 control points with {span_delta, span_offset,
 * height_mod, sentinel}. Sentinel 0xFFFF terminates.
 * ======================================================================== */

/* [CONFIRMED @ 0x00402AD0 UpdateSplineTracksideCamera; REG-4 fix 2026-05-22]
 * Spline template table, reverse-engineered from the orig's stack-local
 * `local_5e` initialization. Each template is 4 control-point pairs of
 * (span_delta, vert_offset) — used by UpdateSplineTracksideCamera to read
 * 4 strip vertices around the anchor and build a cubic Catmull-Rom spline.
 *
 *   iter 0: span_anchor - 1            (= one strip before the camera anchor)
 *   iter 1: span_anchor                (= the anchor itself)
 *   iter 2: span_anchor + 40 or +10    (forward sweep, far)
 *   iter 3: span_anchor + 41 or +11    (forward sweep, far + 1)
 *
 * Prior port had flat-anchor pairs (all spans = 0, alternating vert -1/0)
 * which produced 4 collinear control points — Catmull-Rom of 4 collinear
 * points degenerates to a straight line, breaking trackside spline cam.
 *
 * Orig data per template:
 *   type 0: (-1,0) (0,0) (40, 0) (41, 0)  — forward sweep, flat anchor
 *   type 1: (-1,0) (0,0) (40, 4) (41, 4)  — forward sweep, +4 vert lift
 *   type 2: (-1,0) (0,0) (40,-4) (41,-4)  — forward sweep, -4 vert dip
 *   type 3: (-1,0) (0,0) (10, 4) (11, 4)  — short-range sweep, +4 lift
 *   type 4: same as type 3
 *   type 5: defensive duplicate (orig has no type 5; bounds-safe fallback)
 */
static const short s_splineTemplates[6][8] = {
    /* type 0 */ { -1, 0,   0, 0,   40,  0,   41,  0 },
    /* type 1 */ { -1, 0,   0, 0,   40,  4,   41,  4 },
    /* type 2 */ { -1, 0,   0, 0,   40, -4,   41, -4 },
    /* type 3 */ { -1, 0,   0, 0,   10,  4,   11,  4 },
    /* type 4 */ { -1, 0,   0, 0,   10,  4,   11,  4 },
    /* type 5 */ { -1, 0,   0, 0,   10,  4,   11,  4 },
};

/* Mirrors BindTrackStripRuntimePointers (0x444070): publishes the strip-span
 * and vertex-pool base addresses so trackside-camera modes (case 0 static FOV,
 * case 3 static, case 6 spline; UpdateStaticTracksideCamera / SelectTrackside-
 * CameraProfile) can index them. Without this the case-0 path NULL-derefs
 * (movzwl 0x4(%edx),%eax with edx=0) the first time the post-race or replay
 * cinematic switches to trackside view. Stored as integer so the existing
 * `base + idx*0x18` arithmetic in the call sites is unchanged. */
void td5_camera_bind_track_geometry(const void *span_base,
                                    const void *vertex_base)
{
    g_spanTable   = (int)(intptr_t)span_base;
    g_vertexTable = (int)(intptr_t)vertex_base;
}

/* ========================================================================
 * 0x42CE50 -- SetCameraWorldPosition
 *
 * Converts integer world-space position to float and stores in the
 * shared camera position globals.
 * ======================================================================== */

void SetCameraWorldPosition(int *pos)
{
    if (s_cam_capture_view >= 0) {
        TD5_CamPose *P = &s_cam_pose_cur[s_cam_capture_view];
        P->eye[0] = pos[0]; P->eye[1] = pos[1]; P->eye[2] = pos[2];
    }
    g_cameraPos[0] = (float)pos[0] * g_worldToRenderScale;
    g_cameraPos[1] = (float)pos[1] * g_worldToRenderScale;
    g_cameraPos[2] = (float)pos[2] * g_worldToRenderScale;
}

/* ========================================================================
 * 0x42D0B0 -- BuildCameraBasisFromAngles
 *
 * Builds THREE independent rotation matrices from Euler angles
 * {pitch, yaw, roll} using sequential axis rotations and the 12-bit
 * trig lookup tables.
 *
 * Matrix 1 (g_cameraBasis @ 0x4AAFA0): yaw -> pitch -> roll
 * Matrix 2 (g_cameraSecondary @ 0x4AB070): pitch -> roll
 * Matrix 3 (g_cameraTertiary @ 0x4AB040): roll only
 *
 * Calls FinalizeCameraProjectionMatrices at the end.
 * ======================================================================== */

void BuildCameraBasisFromAngles(short *angles)
{
    if (s_cam_capture_view >= 0) {
        TD5_CamPose *P = &s_cam_pose_cur[s_cam_capture_view];
        P->angles[0] = angles[0]; P->angles[1] = angles[1]; P->angles[2] = angles[2];
        P->build_mode = 1;
    }
    /*
     * Directly follows the Ghidra decompilation of FUN_0042d0b0.
     *
     * The local rotation matrix on Ghidra's stack maps as:
     *   [local_30  local_2c  local_28]
     *   [local_24  local_20  local_1c]
     *   [local_18  local_14  local_10]
     *
     * For each axis rotation the pattern is:
     *   - Set the "1" element on the axis' row/col to 1.0
     *   - Zero the off-axis elements
     *   - Place sin into the two diagonal corners
     *   - Place cos into one off-diagonal, -cos into the other
     *   - Multiply into the target basis
     */

    float rot[9];
    float s, c;
    short pitch = angles[0];
    short yaw   = angles[1];
    short roll  = angles[2];

    /* ---- Primary basis (g_cameraBasis): yaw -> pitch -> roll ---- */

    /* Identity */
    g_cameraBasis[0] = 1.0f; g_cameraBasis[1] = 0.0f; g_cameraBasis[2] = 0.0f;
    g_cameraBasis[3] = 0.0f; g_cameraBasis[4] = 1.0f; g_cameraBasis[5] = 0.0f;
    g_cameraBasis[6] = 0.0f; g_cameraBasis[7] = 0.0f; g_cameraBasis[8] = 1.0f;

    /* Yaw (param_1[1]): center=[1,1]=1, sin->[0,0],[2,2], cos->[0,2], -cos->[2,0]
     * NOTE: sin/cos swapped — see BuildRotationMatrixFromAngles comment. */
    s = CosFloat12bit((unsigned int)(unsigned short)yaw);
    c = SinFloat12bit(yaw);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraBasis, g_cameraBasis);

    /* Pitch (param_1[0]): center=[0,0]=1, sin->[1,1],[2,2], cos->[2,1], -cos->[1,2] */
    s = CosFloat12bit((unsigned int)(unsigned short)pitch);
    c = SinFloat12bit(pitch);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraBasis, g_cameraBasis);

    /* Roll (param_1[2]): center=[2,2]=1, sin->[0,0],[1,1], cos->[1,0], -cos->[0,1] */
    s = CosFloat12bit((unsigned int)(unsigned short)roll);
    c = SinFloat12bit(roll);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraBasis, g_cameraBasis);

    /* ---- Secondary basis (g_cameraSecondary): pitch -> roll ---- */

    g_cameraSecondary[0] = 1.0f; g_cameraSecondary[1] = 0.0f; g_cameraSecondary[2] = 0.0f;
    g_cameraSecondary[3] = 0.0f; g_cameraSecondary[4] = 1.0f; g_cameraSecondary[5] = 0.0f;
    g_cameraSecondary[6] = 0.0f; g_cameraSecondary[7] = 0.0f; g_cameraSecondary[8] = 1.0f;

    /* Pitch */
    s = CosFloat12bit((unsigned int)(unsigned short)pitch);
    c = SinFloat12bit(pitch);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraSecondary, g_cameraSecondary);

    /* Roll */
    s = CosFloat12bit((unsigned int)(unsigned short)roll);
    c = SinFloat12bit(roll);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraSecondary, g_cameraSecondary);

    /* ---- Tertiary basis (g_cameraTertiary): roll only ---- */

    g_cameraTertiary[0] = 1.0f; g_cameraTertiary[1] = 0.0f; g_cameraTertiary[2] = 0.0f;
    g_cameraTertiary[3] = 0.0f; g_cameraTertiary[4] = 1.0f; g_cameraTertiary[5] = 0.0f;
    g_cameraTertiary[6] = 0.0f; g_cameraTertiary[7] = 0.0f; g_cameraTertiary[8] = 1.0f;

    /* Roll */
    s = CosFloat12bit((unsigned int)(unsigned short)roll);
    c = SinFloat12bit(roll);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraTertiary, g_cameraTertiary);

    FinalizeCameraProjectionMatrices();
}

/* ========================================================================
 * 0x42D410 -- FinalizeCameraProjectionMatrices
 *
 * Copies primary basis to working copy, scales all three matrix sets
 * by projection factors, and computes integer depth range.
 *
 * [ARCH-DIVERGENCE: D3D11 billboard snapshot + FPU rounding; L5 sweep 2026-05-21]
 *   Math sequence is byte-equivalent to orig (12-float memcpy → 18 multiplies
 *   by inv_proj → ROUND of depth → 18 multiplies by fov_factor). Two
 *   intentional divergences:
 *   1. Port snapshots g_cameraSecondary into g_cameraSecondaryUnscaled (9
 *      floats) before scaling — feeds the D3D11 billboard path so its rotation
 *      lives in the unscaled basis space.
 *   2. Integer depth uses `(int)(x + 0.5f)` instead of FISTP/RNE; tracked
 *      under the broader chassis-snap FPU rounding cleanup
 *      (todo_chassis_snap_fix_2026-05-16). Visual-only path; does not enter
 *      sim cascade.
 * ======================================================================== */

void FinalizeCameraProjectionMatrices(void)
{
    float fov_factor = (float)g_depthFovFactor * g_projFovScale;
    float inv_proj   = g_projConst1 / g_projectionScale;
    int i;

    /* Copy primary basis + position to working area (12 floats) */
    memcpy(g_cameraBasisWork, g_cameraBasis, 12 * sizeof(float));

    /* Source-port snapshot: yaw-stripped basis BEFORE projection scaling.
     * The td5_render.c billboard branch uses this so its rotation lives
     * in the same coordinate space as s_camera_basis (also unscaled). */
    memcpy(g_cameraSecondaryUnscaled, g_cameraSecondary, 9 * sizeof(float));

    /* Scale all three 3x3 matrices by 1/projectionScale */
    for (i = 0; i < 9; i++) {
        g_cameraBasisWork[i] *= inv_proj;
        g_cameraSecondary[i] *= inv_proj;
        g_cameraTertiary[i]  *= inv_proj;
    }

    /* Compute integer depth range */
    g_projDepthInt = (int)(g_projectionScale * g_projDepthScale + 0.5f);

    /* Scale first 6 elements of working and secondary, and first 6 of tertiary by fov_factor */
    for (i = 0; i < 6; i++) {
        g_cameraBasisWork[i] *= fov_factor;
        g_cameraSecondary[i] *= fov_factor;
        g_cameraTertiary[i]  *= fov_factor;
    }
}

/* ========================================================================
 * 0x42D5B0 -- OrientCameraTowardTarget
 *
 * Constructs a look-at camera basis from the current camera position
 * toward the given target position. Applies yaw offset rotation and
 * coordinate system flip.
 *
 * [CONFIRMED @ 0x0042D5B0 OrientCameraTowardTarget; L5 sweep 2026-05-21]
 *   Byte-faithful port. Side-by-side decompile match:
 *   - dir_x/y/z = (target * g_fixedPointToFloatScale) - g_currentViewWorldOrigin
 *   - dist_sq early-out on `(int)dist_sq <= 0`
 *   - inv_dist = _DAT_0046737c / sqrt(dist_sq) (g_invDistScale)
 *   - degenerate-case branch on horiz_len <= g_audioDopplerZeroSentinel
 *     (sentinel reuses near-zero compare const)
 *   - non-degenerate branch writes secondary matrix DAT_004ab070..090
 *     identically (right.x, right.z = -fwd.x/h, up.x = -fy*fx/h, up.y = h,
 *     up.z = fz*fy/h, secondary row1/2 mirrors)
 *   - yaw_rot uses (sin,cos) swap matching orig's `local_60=cos local_5c=-sin`
 *   - three MultiplyRotationMatrices3x3 calls + FinalizeCameraProjectionMatrices
 *     in identical order
 * ======================================================================== */

void OrientCameraTowardTarget(int *target_pos, unsigned int yaw_offset)
{
    if (s_cam_capture_view >= 0) {
        TD5_CamPose *P = &s_cam_pose_cur[s_cam_capture_view];
        P->target[0] = target_pos[0]; P->target[1] = target_pos[1]; P->target[2] = target_pos[2];
        P->yaw_offset = (int)yaw_offset; P->build_mode = 0;
    }
    float dir_x, dir_y, dir_z;
    float dist_sq, inv_dist, horiz_len;
    float flip[9];
    float yaw_rot[9];
    float s, c;

    dir_x = (float)target_pos[0] * g_worldToRenderScale - g_cameraPos[0];
    dir_y = (float)target_pos[1] * g_worldToRenderScale - g_cameraPos[1];
    dir_z = (float)target_pos[2] * g_worldToRenderScale - g_cameraPos[2];

    dist_sq = dir_x * dir_x + dir_y * dir_y + dir_z * dir_z;
    if ((int)dist_sq <= 0) return;

    inv_dist = g_invDistScale / sqrtf(dist_sq);

    /* Normalized forward direction stored in secondary matrix row 2 */
    g_cameraBasis[6] = inv_dist * dir_x;    /* forward X */
    g_cameraBasis[7] = inv_dist * dir_y;    /* forward Y */
    g_cameraBasis[8] = inv_dist * dir_z;    /* forward Z */

    /* Horizontal length */
    horiz_len = g_cameraBasis[6] * g_cameraBasis[6] + g_cameraBasis[8] * g_cameraBasis[8];

    if (horiz_len <= g_nearZeroThreshold) {
        /* Degenerate case: looking straight up or down */
        g_cameraBasis[0] = 1.0f;
        g_cameraBasis[2] = 0.0f;
        g_cameraBasis[3] = 0.0f;
        g_cameraBasis[4] = 0.0f;
        g_cameraBasis[6] = 0.0f;
        g_cameraBasis[8] = 0.0f;
        g_cameraBasis[5] = g_cameraBasis[7];  /* up.Y = forward.Y sign */
    } else {
        horiz_len = sqrtf(horiz_len);

        g_cameraSecondary[0] = 1.0f;
        g_cameraSecondary[1] = 0.0f;
        g_cameraSecondary[2] = 0.0f;
        g_cameraSecondary[3] = 0.0f;
        g_cameraSecondary[6] = 0.0f;

        /* Right vector = cross(up_world, forward) normalized in XZ */
        g_cameraBasis[0] = g_cameraBasis[8] / horiz_len;               /* right X */
        g_cameraBasis[2] = -(g_cameraBasis[6] / horiz_len);            /* right Z */

        /* Up vector */
        g_cameraBasis[3] = -((g_cameraBasis[7] * g_cameraBasis[6]) / horiz_len); /* up X */
        g_cameraBasis[4] = horiz_len;                                             /* up Y */
        g_cameraBasis[5] = (g_cameraBasis[8] * g_cameraBasis[7]) / horiz_len;    /* up Z */

        g_cameraSecondary[5] = -g_cameraBasis[7];     /* secondary row1 Z = -forward.Y */
        g_cameraSecondary[4] = horiz_len;              /* secondary row1 Y = horiz_len */
        g_cameraSecondary[7] = g_cameraBasis[7];       /* secondary row2 Y = forward.Y */
        g_cameraSecondary[8] = horiz_len;              /* secondary row2 Z = horiz_len */
    }

    g_cameraBasis[5] = -g_cameraBasis[5];
    g_cameraBasis[1] = 0.0f;

    /* Apply yaw offset rotation (sin/cos swapped — see BuildRotationMatrixFromAngles) */
    s = CosFloat12bit(yaw_offset);
    c = SinFloat12bit((int)yaw_offset);
    yaw_rot[0] = s;  yaw_rot[1] = -c;  yaw_rot[2] = 0.0f;
    yaw_rot[3] = 0.0f; yaw_rot[4] = 0.0f; yaw_rot[5] = 0.0f;
    yaw_rot[6] = 0.0f; yaw_rot[7] = 0.0f; yaw_rot[8] = 1.0f;
    yaw_rot[3] = c;  yaw_rot[4] = s;
    MultiplyRotationMatrices3x3(yaw_rot, g_cameraBasis, g_cameraBasis);

    /* Coordinate system flip: {-1,0,0, 0,1,0, 0,0,-1} */
    flip[0] = -1.0f; flip[1] = 0.0f; flip[2] = 0.0f;
    flip[3] = 0.0f;  flip[4] = 1.0f; flip[5] = 0.0f;
    flip[6] = 0.0f;  flip[7] = 0.0f; flip[8] = -1.0f;

    /* Apply yaw rotation + flip to secondary */
    MultiplyRotationMatrices3x3(yaw_rot, g_cameraSecondary, g_cameraSecondary);
    MultiplyRotationMatrices3x3(g_cameraSecondary, flip, g_cameraSecondary);

    /* Tertiary = yaw_rot * flip */
    MultiplyRotationMatrices3x3(yaw_rot, flip, g_cameraTertiary);

    FinalizeCameraProjectionMatrices();
}

/* ========================================================================
 * 0x401450 -- LoadCameraPresetForView
 *
 * Reads preset data from the camera preset table and populates per-view
 * camera state. Preset index is taken from g_raceCameraPresetId[view].
 * ======================================================================== */

void LoadCameraPresetForView(int actor, int force_reload, int view, int save_state)
{
    int preset_idx = g_raceCameraPresetId[view];
    TD5_CameraPreset *p = &g_cameraPresets[preset_idx];

    short mode_val    = p->mode;
    short elev        = p->elevation_angle;
    short radius_raw  = p->orbit_radius_raw;
    short height_raw  = p->height_target_raw;

    /* Original binary (0x401481) multiplies both by g_const256 (256.0).
     * This keeps the orient vector balanced (X/Z ≈ ±2100, Y ≈ 600) so
     * terrain rotation doesn't leak the large elevation into X/Z and
     * destabilize the radius spring. We divide by 256 when producing
     * actual world-space orbit offsets (see UpdateChaseCamera). */
    float radius_f = (float)(int)radius_raw * g_const256;
    float height_f = (float)(int)height_raw * g_const256;

    /* Store offset vector from preset */
    g_camOffsetVec[view][0] = (short)(p->extra_param_1 & 0xFFFF);
    g_camOffsetVec[view][1] = (short)(p->extra_param_1 >> 16);
    /* Actually: extra_param_1 and extra_param_2 are stored as two ints */
    *(int *)&g_camOffsetVec[view][0] = p->extra_param_1;
    *(int *)&g_camOffsetVec[view][2] = p->extra_param_2;

    g_camOrbitRadiusScale[view] = radius_f;
    g_camTargetHeight[view]     = height_f;
    g_camElevationAngleFP[view] = (int)elev << 8;

    /* If force_reload or mode changed, reset interpolation targets */
    if (force_reload != 0 || g_raceCameraPresetMode[view] != (int)mode_val) {
        short actor_yaw = *(short *)(actor + 2);  /* actor angles offset +0x02 from passed ptr */
        g_camCurrentRadius[view] = radius_f;
        g_camSmoothedHeight[view] = height_f;
        g_camOrbitAngleFP[view] =
            (g_camRotationSlot[view] * 0x800 + (int)actor_yaw) * 0x100;
        g_camStoredPitch[view] = (int)elev << 8;
    }

    g_camPresetChangeFlag[view] = 0;
    g_camReserved78[view]       = 0;
    g_raceCameraPresetMode[view] = (int)mode_val;
    g_camFlyInCounter[view]     = g_flyInThreshold;

    /* The spring just (re)seeded to target above; re-anchor the per-render
     * smoothing for this view so the next finalize doesn't glide from the
     * stale pre-reload snapshot (avoids a 1-frame snap on preset change). */
    td5_camera_snap_smoothing_view(view);

    if (save_state != 0) {
        g_camPackedSave[0] = (unsigned char)((g_raceCameraPresetId[0] & 0x7F) |
                                              (g_raceCameraPresetMode[0] << 7));
        g_camPackedSave[1] = (unsigned char)((g_raceCameraPresetId[1] & 0x7F) |
                                              (g_raceCameraPresetMode[1] << 7));
    }
}

/* ========================================================================
 * 0x401590 -- UpdateChaseCamera
 *
 * Main chase camera: orbit around vehicle with terrain-following,
 * damped angle tracking, height smoothing, and radius spring.
 *
 * [L5 promotion sweep audit 2026-05-18 — byte-equivalent (with documented
 *  ARCH-DIVERGENCEs)]
 *   Decompile of 0x00401590 verified semantically:
 *     - Fly-in counter behaviour (ramp/decrement around g_flyInThreshold,
 *       inverted clamp to 6 when actor+0x379 != 0) reproduces orig.
 *     - Orbit visual angle = g_camYawOffset[v] - (g_camOrbitAngleFP[v]>>8)
 *       matches DAT_00482f70 - (DAT_00482f18>>8).
 *     - Orbit-offset sin/cos*radius (writes [0]/[2]; [1] = stored pitch)
 *       matches orig sin/cos writes to DAT_00482ed8/edc/ee0.
 *     - Heading integrator: 20-bit shortest-wrap, abs/5 + 5 base speed,
 *       clamp >= 15 unless fly-in completed, integrate
 *       (effective_speed * heading_delta) >> 8 — line-by-line match.
 *     - Three-point terrain probe (forward-right, forward-left, backward)
 *       uses pos_x/z + (sin/cos)*0x20 deltas — match orig local_3c..local_24.
 *     - Look-angle = (g_camOrbitAngleFP>>8) - actor.display_angle_yaw +
 *       g_camYawOffset — match orig uVar8 computation.
 *     - Stored-pitch smoother = (orient[1]<<8 - stored_pitch + (>>31 & 7))
 *       >> 3 + stored_pitch — match.
 *     - Radius spring (sqrt(fx² + fz²) * _DAT_0045d5dc - _DAT_004749c8 *
 *       current) + height smoother — match.
 *   Race-start spring-reset (one-shot at g_td5.paused 1→0) lives in the
 *   wrapper (td5_camera.c:2149-2160), not this routine — recent
 *   fb60d3d-class fix verified by this sweep.
 *
 * [ARCH-DIVERGENCE — DAT_*-bank globals → named C-arrays]
 *   Orig writes to fixed DAT addresses (DAT_00482ed8/edc/ee0/f18/f50/f54/
 *   f60/f70/fa0/ef8/f00/f10). Port collects all per-view state into named
 *   arrays (g_camOrbitOffset, g_camOrbitAngleFP, g_camOrientShort,
 *   g_camHeadingDelta20, g_camYawOffset, g_camStoredPitch, g_camRotationSlot,
 *   g_camCurrentRadius, g_camTargetHeight, g_camSmoothedHeight) indexed by
 *   view. Wire-equivalent; only addressing differs.
 *
 * [PARITY — terrain-derived pitch/roll] Orig computes pitch/roll via
 *   AngleFromVector12 over the three terrain-contact normals so the chase
 *   cam follows the road over hills/jumps. Port now mirrors this exactly
 *   (cam_angles[0]=pitch, [2]=roll); the point construction +
 *   ComputeActorTrackContactNormal pipeline is byte-faithful 24.8 FP, so
 *   the earlier "forced to 0" workaround was unnecessary and is removed.
 *
 * [ARCH-DIVERGENCE — FPU rounding mode (ROUND vs +0.5f)]
 *   Same FISTP-RNE vs nearest-up rounding gap as
 *   UpdateVehicleRelativeCamera. Cosmetic only; does not feed sim.
 *
 * [ARCH-DIVERGENCE — split into UpdateChaseCamera + finalize_chase_pos]
 *   Port factors the final SetCameraWorldPosition call out into
 *   td5_camera_finalize_chase_pos so the chase position can be re-pinned
 *   per render frame against the interpolated car mesh under 60fps render
 *   / 30Hz sim. Orig wrote camera world position inline; identity result
 *   when sub-tick fraction is 1.0 (per-sim-tick callsite).
 * ======================================================================== */

/* [TRACE 2026-05-25 terrain-pitch-roll-zeroed] CSV mirror of the orig
 * 3-point terrain probe in UpdateChaseCamera @ 0x00401590. Pairs with
 * tools/_probes/terrain_probe_capture.js (orig). Confirms whether the
 * port's ComputeActorTrackContactNormal output (norm_a/b/c_y, all 24.8
 * FP) matches the orig stack-locals local_38/local_2c/local_20 at the
 * AngleFromVector12 call sites (0x004017EC pitch / 0x0040180C roll).
 *
 * If the values agree the OVERSIGHT row upgrades to FIX_APPLIED and we
 * restore the AngleFromVector12 derivation at td5_camera.c:752-754.
 * Otherwise the rows pinpoint the unit / coord-system mismatch.
 *
 * Gated by g_td5.ini.trace_terrain_cam_probe ([Trace] TerrainCamProbe=1).
 * When disabled the trace is a single compare-and-bail at the top of
 * terrain_probe_trace_emit. */
static FILE *s_terrain_probe_trace_fp = NULL;

static void terrain_probe_trace_ensure_open(void)
{
    if (s_terrain_probe_trace_fp) return;
    s_terrain_probe_trace_fp = fopen("tools/frida_csv/terrain_probe_port.csv", "w");
    if (s_terrain_probe_trace_fp) {
        fprintf(s_terrain_probe_trace_fp,
            "sim_tick,view,actor_slot,actor_span,actor_sub_lane,"
            "pos_x,pos_y,pos_z,combined_angle,"
            "point_ax,point_az,point_bx,point_bz,point_cx,point_cz,"
            "norm_a_y,norm_b_y,norm_c_y,"
            "pitch_input,roll_input,orient1_y,stored_pitch,cam_y_offset\n");
        fflush(s_terrain_probe_trace_fp);
    }
}

/* Emit one row per chase-camera tick. All coordinate values are in 24.8
 * fixed-point (matches orig stack-local layout). pitch_input/roll_input
 * are the post-shift, post-negate raw operands to AngleFromVector12.
 *
 * Matches columns from tools/_probes/terrain_probe_capture.js. */
static void terrain_probe_trace_emit(int view, int actor_addr,
                                     int pos_x, int pos_z,
                                     unsigned int combined_angle,
                                     int point_ax, int point_az,
                                     int point_bx, int point_bz,
                                     int point_cx, int point_cz,
                                     int norm_a_y, int norm_b_y, int norm_c_y,
                                     int pitch_input, int roll_input)
{
    if (!g_td5.ini.trace_terrain_cam_probe) return;
    terrain_probe_trace_ensure_open();
    if (!s_terrain_probe_trace_fp) return;

    /* Derive slot from actor's per-tick slot_index field at +0x375
     * (uint8_t — see td5_vfx.c:2240 for the same idiom against the orig
     * actor base, and td5_pilot_trace_004370A0.h:47 for the typed field).
     * Frida side derives slot from g_raceActors base; we mirror via the
     * actor's own +0x375 since the port uses heap-allocated actors. */
    int actor_slot = actor_addr ? (int)*(unsigned char *)(actor_addr + 0x375) : -1;
    int actor_span = actor_addr ? (int)*(short *)(actor_addr + 0x80) : -1;
    int actor_sub  = actor_addr ? (int)*(signed char *)(actor_addr + 0x8C) : -1;
    int actor_pos_y = actor_addr ? *(int *)(actor_addr + 0x200) : 0;

    fprintf(s_terrain_probe_trace_fp,
        "%u,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        (unsigned)g_td5.simulation_tick_counter,
        view, actor_slot, actor_span, actor_sub,
        pos_x, actor_pos_y, pos_z, combined_angle,
        point_ax, point_az, point_bx, point_bz, point_cx, point_cz,
        norm_a_y, norm_b_y, norm_c_y,
        pitch_input, roll_input,
        /* diag: does terrain pitch reach orient[1] / stored pitch / cam-Y offset? */
        (int)g_camOrientShort[view][1],
        g_camStoredPitch[view],
        g_camOrbitOffset[view][1]);
    fflush(s_terrain_probe_trace_fp);
}

/* Force the next snapshot to re-seed (prev := cur) for one view. */
static void td5_camera_snap_smoothing_view(int view)
{
    if (view >= 0 && view < TD5_MAX_VIEWPORTS) {
        s_camSmoothInit[view] = 0;
        s_cam_pose_init[view] = 0;   /* [CAMERA REWRITE] re-seed prev=cur on next solve */
    }
}

/* Public: re-anchor smoothing for every view (race start / resume-from-pause). */
void td5_camera_snap_smoothing(void)
{
    for (int v = 0; v < TD5_MAX_VIEWPORTS; v++) {
        s_camSmoothInit[v] = 0;
        s_cam_pose_init[v] = 0;      /* [CAMERA REWRITE] re-seed prev=cur on next solve */
    }
}

/* Producer — capture this sub-tick's spring output + car vertical pose.
 * Called once per UpdateChaseCamera (= once per sim sub-tick, countdown AND
 * race). finalize_chase_pos interpolates prev->cur by g_subTickFraction. */
static void td5_camera_snapshot_spring(uintptr_t actor, int v)
{
    int pos_y = *(int *)(actor + 0x200);

    if (!s_camSmoothInit[v]) {
        s_camSmoothInit[v] = 1;
        for (int k = 0; k < 3; k++)
            s_camOffPrev[v][k] = s_camOffCur[v][k] = g_camOrbitOffset[v][k];
        s_camHPrev[v] = s_camHCur[v] = g_camSmoothedHeight[v];
        s_camYPrev[v] = s_camYCur[v] = pos_y;
        return;
    }

    for (int k = 0; k < 3; k++) {
        s_camOffPrev[v][k] = s_camOffCur[v][k];
        s_camOffCur[v][k]  = g_camOrbitOffset[v][k];
    }
    s_camHPrev[v] = s_camHCur[v];  s_camHCur[v] = g_camSmoothedHeight[v];
    s_camYPrev[v] = s_camYCur[v];  s_camYCur[v] = pos_y;

    /* Teleport / respawn / preset-snap guard: a large per-tick jump in the
     * car Y or the orbit offset is a discontinuity, not motion to glide
     * across — collapse prev onto cur so finalize shows it instantly. */
    {
        int dyj = s_camYCur[v] - s_camYPrev[v];
        if (dyj > 0x40000 || dyj < -0x40000)
            s_camYPrev[v] = s_camYCur[v];
        int dxo = s_camOffCur[v][0] - s_camOffPrev[v][0];
        int dzo = s_camOffCur[v][2] - s_camOffPrev[v][2];
        if (dxo > 0x40000 || dxo < -0x40000 || dzo > 0x40000 || dzo < -0x40000) {
            for (int k = 0; k < 3; k++) s_camOffPrev[v][k] = s_camOffCur[v][k];
            s_camHPrev[v] = s_camHCur[v];
        }
    }
}

/* ========================================================================
 * Invisible-car (vehicle_mode != 0) ground floor [PORT ENHANCEMENT — NOT in
 * original; the original clamps camera Y in NO mode]
 *
 * In the scripted/recovery "invisible car" mode the chase camera can dip below
 * terrain on slopes. A blanket ground clamp was tried before and reverted (see
 * the comment in finalize_chase_pos) because: (1) the chassis-span lane probe
 * is unreliable several spans behind the car (branch / ring-wrap → garbage
 * altitude → camera reset), and (2) on flat road the per-heading probe variance
 * made the camera bob with the car's facing.
 *
 * This floor avoids both: it walks a DEDICATED probe from the car's span to the
 * camera XZ (so it lands on the span that actually contains the camera, not the
 * chassis span extrapolated), VALIDATES the result (reject far-span / ring-wrap
 * walks and implausible heights — on failure it returns 0 = "no floor", never a
 * reset), and the caller applies it ONE-SIDED to the eye Y only (raise, never
 * lower; never touches the look-at target). With a small offset the eye on flat
 * road already sits far above the floor, so it never triggers there — no bob.
 *
 * Returns 1 and writes *out_floor_y (24.8 FP eye-Y floor) when a trustworthy
 * ground height was found; 0 otherwise.
 * ======================================================================== */

/* Small lift above terrain, 24.8 FP world units (~24 world units). */
#define TD5_CAM_GROUND_OFFSET   0x1800
/* Max spans the camera XZ may be from the chassis span before we distrust the
 * walk (camera sits ~4 spans behind; allow slack). */
#define TD5_CAM_GROUND_MAX_SPAN_GAP  10
/* Reject a probed ground height further than this from the car's Y (24.8 FP);
 * a real slope over the ~4-span camera gap stays well inside this band, while a
 * branch/ring-wrap walk to a far altitude blows past it. ~3072 world units. */
#define TD5_CAM_GROUND_SANE_DY  0xC0000

static int td5_camera_probe_ground_floor(uintptr_t actor, int cam_x, int cam_z,
                                         int *out_floor_y)
{
    TD5_TrackProbeState probe;
    int car_span = (int)(*(short *)(actor + 0x80));
    int car_y    = *(int *)(actor + 0x200);

    memset(&probe, 0, sizeof(probe));
    probe.span_index     = (int16_t)car_span;
    probe.sub_lane_index = *(signed char *)(actor + 0x8C);

    /* Single-step walker toward the camera XZ; bounded so a runaway walk can't
     * spin. ~4 spans of gap needs only a few steps; stop early on convergence. */
    for (int i = 0; i < TD5_CAM_GROUND_MAX_SPAN_GAP + 2; i++) {
        int prev = probe.span_index;
        td5_track_update_probe_position(&probe, cam_x, cam_z);
        if (probe.span_index == prev) break;
    }

    /* Validate: the landed span must be near the chassis span (else the walk
     * crossed a branch / ring boundary into unrelated geometry). */
    {
        int gap = (int)probe.span_index - car_span;
        if (gap < 0) gap = -gap;
        if (gap > TD5_CAM_GROUND_MAX_SPAN_GAP)
            return 0;
    }

    int ground_y = (int)td5_track_compute_contact_height_bestlane(
                       probe.span_index, cam_x, cam_z, NULL);

    /* Validate the height: reject implausible (far-altitude) reads. */
    {
        int dy = ground_y - car_y;
        if (dy < 0) dy = -dy;
        if (dy > TD5_CAM_GROUND_SANE_DY)
            return 0;
    }

    *out_floor_y = ground_y + TD5_CAM_GROUND_OFFSET;
    return 1;
}

/* ========================================================================
 * [ITEM #18 camera] TD5RE_REPLAY_CAM_FIX (default 1) — in REPLAY mode some
 * cinematic trackside cameras render BELOW the road surface (you see the
 * underside of the track). The authored trackside profiles drive the eye Y
 * directly: the static cam (UpdateStaticTracksideCamera / case-0 inline) reads
 * a strip vertex + height_param, the spline cam (UpdateSplineTracksideCamera)
 * evaluates authored control points, and the orbit cam (UpdateTracksideOrbit-
 * Camera) adds a stored-pitch Y offset to the car Y. On dipping terrain (or a
 * negative stored-pitch / low height_param) that authored eye can land under
 * the ground, so the camera looks up through the track.
 *
 * Fix: after UpdateTracksideCamera on the replay paths, CLAMP the trackside
 * EYE world Y up to the track surface height at the camera's XZ (plus a small
 * clearance). We reuse the chase invisible-car floor probe
 * (td5_camera_probe_ground_floor → td5_track_compute_contact_height_bestlane):
 * it self-validates the walk and returns 0 on an untrustworthy result (we then
 * leave the eye untouched). One-sided RAISE only — never lower the eye, never
 * move the look-at target. Applied ONLY on the replay/cinematic trackside path
 * (the trackside updaters' eye XZ); the vehicle-relative replay cams (behaviour
 * 1/2/7/8) follow the car and are not affected. "0" reverts.
 * ======================================================================== */
static int td5_camera_replay_cam_fix(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_REPLAY_CAM_FIX");   /* default ON */
    }
    return s_mode;
}

/* Raise eye[1] to the track surface (+clearance) at eye XZ if the trackside
 * replay camera dipped below it. Returns 1 when the eye was raised, 0 when it
 * was left untouched (fix disabled, no trustworthy floor, or already above).
 * One-sided: never lowers the eye. Does NOT touch the look-at target. */
static int td5_camera_replay_eye_floor_clamp(uintptr_t actor, int view, int *eye)
{
    if (!td5_camera_replay_cam_fix()) return 0;
    if (!actor || !eye) return 0;

    int floor_y;
    if (!td5_camera_probe_ground_floor(actor, eye[0], eye[2], &floor_y))
        return 0;                       /* untrustworthy walk — leave eye as-is */
    if (eye[1] >= floor_y) return 0;    /* already at/above surface — no change */

    static uint32_t s_replay_floor_log_ctr;
    if ((s_replay_floor_log_ctr++ % 120u) == 0u)
        TD5_LOG_D(LOG_TAG,
                  "replay trackside eye floor v%d: eye Y %d -> %d (behavior=%d)",
                  view, eye[1], floor_y, g_camBehaviorType[view]);

    eye[1] = floor_y;
    return 1;
}

void UpdateChaseCamera(int actor, int do_track_heading, int view)
{
    int fly_in_threshold = g_flyInThreshold;
    int v = view;
    int fly_in;
    float current_radius;
    int orbit_visual_angle;
    unsigned int combined_angle;
    int cos_val, sin_val;
    int point_ax, point_az, point_bx, point_bz, point_cx, point_cz;
    TD5_TrackProbeState probe_a, probe_b, probe_c;
    int norm_a_y, norm_b_y, norm_c_y;
    int pos_x, pos_z;
    float terrain_matrix[12];
    short cam_angles[4];
    short transformed_angles[4]; /* needs 3 for ConvertFloatVec3ToShortAngles */
    unsigned int look_angle;
    int height_delta;
    short fx, fz;
    float magnitude;

    /* --- Fly-in counter --- */
    if (*(char *)(actor + 0x379) == 0) {
        /* Normal mode: ramp up toward threshold */
        if (g_camFlyInCounter[v] < fly_in_threshold) {
            g_camFlyInCounter[v]++;
        }
        fly_in = g_camFlyInCounter[v];
        if (fly_in <= fly_in_threshold) goto after_flyin;
    } else {
        /* Special mode: decrement toward 6 */
        fly_in = g_camFlyInCounter[v];
        if (fly_in < 6) goto after_flyin;
    }
    g_camFlyInCounter[v] = fly_in - 1;

after_flyin:
    current_radius = g_camCurrentRadius[v];
    orbit_visual_angle = g_camYawOffset[v] - (g_camOrbitAngleFP[v] >> 8);

    g_camRotationSlot[v] = 0;

    /* Camera orbit position = sin/cos of visual angle * radius.
     * All values are in 24.8 fixed-point scale (matching actor positions).
     * SetCameraWorldPosition divides by 256 to produce float world coords.
     * NOTE: photo-booth framing/angle is handled by the camera-rotation
     * mechanism (branch fix-1780448829), not hacked here. */
    g_camOrbitOffset[v][0] = (int)(SinFloat12bit(orbit_visual_angle) * current_radius + 0.5f);
    g_camOrbitOffset[v][1] = g_camStoredPitch[v];
    g_camOrbitOffset[v][2] = (int)(-(CosFloat12bit((unsigned int)orbit_visual_angle) * current_radius) + 0.5f);

    /* --- Heading tracking / orbit angle integrator --- */
    if (do_track_heading != 0) {
        unsigned int heading_delta;
        int abs_delta, speed, effective_speed;

        heading_delta = ((unsigned int)*(short *)(actor + 0x20A) * 0x100 -
                         g_camOrbitAngleFP[v]) & 0xFFFFF;
        if (heading_delta > 0x7FFFF) {
            heading_delta -= 0x100000;
        }

        abs_delta = (((int)heading_delta >> 0x0B) ^ ((int)heading_delta >> 0x1F)) -
                    ((int)heading_delta >> 0x1F);
        speed = abs_delta + 5;
        g_camHeadingDelta20[v] = (int)heading_delta;

        effective_speed = 15;
        if (speed > 14) {
            effective_speed = speed;
        }

        fly_in = g_camFlyInCounter[v];
        if (effective_speed <= fly_in) {
            /* Fly-in complete: use raw speed (possibly clamped to 15) */
            effective_speed = speed;
            if (speed < 15) {
                effective_speed = 15;
            }
        }

        g_camOrbitAngleFP[v] += (int)((int)(effective_speed * (int)heading_delta) >> 8);
    }

    /* --- Terrain-following (3-point normal sampling) --- */
    combined_angle = (unsigned int)((g_camOrbitAngleFP[v] >> 8) +
                                     g_camRotationSlot[v] * 0x800);

    cos_val = CosFixed12bit(combined_angle);
    sin_val = SinFixed12bit(combined_angle);

    pos_x = *(int *)(actor + 0x1FC);
    pos_z = *(int *)(actor + 0x204);

    /* Point A: forward-right */
    point_ax = pos_x + sin_val * 0x20;
    point_az = pos_z + cos_val * 0x20;

    /* Point B: forward-left */
    point_bx = pos_x + cos_val * -0x20;
    point_bz = pos_z + sin_val * 0x20;

    /* Point C: backward */
    point_cx = pos_x + cos_val * 0x20;
    point_cz = pos_z + sin_val * -0x20;

    /* Seed all three camera terrain probes from the actor's current span +
     * sub-lane, then let each walk to the span its own XZ lands in (below).
     * The previous port used short[6] arrays and wrote sub_lane at byte 10 but
     * ComputeActorTrackContactNormal reads it at byte 12 (OOB into the next
     * probe). TD5_TrackProbeState puts span_index at +0x00 and sub_lane_index
     * at +0x0C, so the walker and the contact-normal resolver agree on the
     * layout and the sub-lane is read from the byte it was written to. The
     * walker reads only span_index + sub_lane_index, so zero-init + these two
     * fields matches the orig camera's span+sub_lane-only seed. (2026-05-28) */
    memset(&probe_a, 0, sizeof(probe_a));
    probe_a.span_index     = *(short *)(actor + 0x80);
    probe_a.sub_lane_index = *(signed char *)(actor + 0x8C);
    probe_b = probe_a;
    probe_c = probe_a;

    {
        int pa[2], pb[2], pc[2];
        pa[0] = point_ax; pa[1] = point_az;
        pb[0] = point_bx; pb[1] = point_bz;
        pc[0] = point_cx; pc[1] = point_cz;

        /* Walk each probe to the span that actually contains its XZ — orig
         * UpdateActorTrackPosition (0x4440F0), one step per call (single_step),
         * the same walker physics uses for laterally-offset body corners. The
         * prior port stub (td5_track.c UpdateActorTrackPosition) only CLAMPED
         * the actor's span, so points outside the actor's triangle (forward-
         * left B / backward C on a slope) resolved to a sea-level default
         * (~256 FP) while A read real altitude → a phantom ~44° downhill →
         * the (byte-faithful) camera-Y arithmetic drove the chase camera below
         * the ground. Walking lands all three on real geometry so pitch/roll
         * reflect the true slope. (2026-05-28) */
        td5_track_update_probe_position(&probe_a, point_ax, point_az);
        ComputeActorTrackContactNormal((short *)&probe_a, pa, &norm_a_y);

        td5_track_update_probe_position(&probe_b, point_bx, point_bz);
        ComputeActorTrackContactNormal((short *)&probe_b, pb, &norm_b_y);

        td5_track_update_probe_position(&probe_c, point_cx, point_cz);
        ComputeActorTrackContactNormal((short *)&probe_c, pc, &norm_c_y);
    }

    /* Compute pitch and roll from the three terrain-contact normals and feed
     * them into the camera rotation matrix, so the chase cam pitches/rolls
     * to follow the road over hills and jumps (orig 0x004017CA..0x0040180C).
     * The point construction above and ComputeActorTrackContactNormal are
     * byte-faithful with orig (all 24.8 FP), so norm_a/b/c_y match the orig
     * stack-locals local_38/local_2c/local_20. The trace below (INI [Trace]
     * TerrainCamProbe, default 0) remains available for orig-vs-port CSV
     * verification but is no longer a precondition for the derivation. */
    {
        /* [2026-05-28 — terrain probe sanity fallback]
         * The backward terrain probe (point_c) occasionally returns a Y-down
         * surface normal (norm_c_y ≈ -58M while norm_b_y ≈ +9.6M, opposite sign,
         * 6× magnitude) at certain spans. That single garbage tick produces
         * pitch_input ≈ -96k and roll_input ≈ -266k → the camera kicks violently.
         * Cross-check signs: a Y-down surface is meaningless for chase-cam tilt
         * (the road is Y-up). When any normal's Y has the opposite sign from
         * the others, treat it as a probe miss and fall back to the median sign
         * sample. Empirically the wobble disappears with this single guard. */
        if (norm_b_y != 0) {
            /* From the orig decomp of ComputeActorTrackContactNormal (0x445450):
             * the value written is a contact HEIGHT in 24.8 FP at the probe XZ
             * (= bary + span_origin_y << 8). All three probe points are
             * spaced ~512 world units around the car so on a real slope the
             * height delta between them is at most ~512·tan(slope) (a few
             * hundred world units), i.e. ≤ 1% of |norm_b|.
             *
             * In practice the port's forward/backward probe occasionally walks
             * into a span at a completely different altitude (track branch /
             * ring wrap → origin_y from a far span). That gives a sane sign
             * but a wildly different magnitude → pitch_input still spikes,
             * and the earlier sign-only fallback didn't catch it.
             *
             * Treat any probe whose value differs from norm_b by more than
             * 50% of |norm_b| as a probe miss and fall back to norm_b. This
             * keeps real slope deltas (<1%) untouched. */
            /* ABSOLUTE threshold (not relative to norm_b): on a Honolulu-altitude
             * track norm_b ≈ 9.6M and a 50% relative threshold works, but on
             * tracks like Newcastle where surface Y is much smaller, that
             * threshold caught legitimate slopes and zeroed the tilt.
             *
             * Probe spacing is ≤ 512 world units per axis (0x20 × 0x1000 from
             * sin/cos table, scaled). Steepest real slope is ~60°, giving a
             * legitimate height delta ≤ 512·tan(60°) ≈ 887 world units (=
             * ~227072 in 24.8 FP). The observed probe-wander deltas were
             * millions of world units. 500000 FP (~1950 world units) sits
             * cleanly between them. */
            const int WANDER_FP = 500000;
            int sbn = (norm_b_y > 0) ? 1 : -1;
            int sa = (norm_a_y > 0) ? 1 : ((norm_a_y < 0) ? -1 : sbn);
            int diff_a = norm_a_y - norm_b_y; if (diff_a < 0) diff_a = -diff_a;
            if (sa != sbn || diff_a > WANDER_FP) norm_a_y = norm_b_y;
            int sc = (norm_c_y > 0) ? 1 : ((norm_c_y < 0) ? -1 : sbn);
            int diff_c = norm_c_y - norm_b_y; if (diff_c < 0) diff_c = -diff_c;
            if (sc != sbn || diff_c > WANDER_FP) norm_c_y = norm_b_y;
        }
        /* Match orig pitch_input formula at 0x004017CA..0x004017E4:
         *   EAX = local_38;  EAX -= ((local_28 + local_1c) + ((local_28+local_1c)>>31 & 1)) >> 1
         *   EAX = -(EAX >> 8)
         * which is equivalent to:
         *   pitch_input = -((norm_a_y - (norm_b_y + norm_c_y)/2) >> 8)
         * This is the first operand to AngleFromVector12 (z=0x200). */
        int avg_bc = norm_b_y + norm_c_y;
        avg_bc = (avg_bc + ((avg_bc >> 31) & 1)) >> 1;  /* signed /2 round-toward-zero */
        int pitch_input = -((norm_a_y - avg_bc) >> 8);

        /* Match orig roll_input formula at 0x004017F8..0x00401800:
         *   EAX = local_28; EAX -= local_1c; EAX = -(EAX >> 8)
         *   = -((norm_b_y - norm_c_y) >> 8) */
        int roll_input = -((norm_b_y - norm_c_y) >> 8);

        terrain_probe_trace_emit(view, actor,
                                  pos_x, pos_z, combined_angle,
                                  point_ax, point_az,
                                  point_bx, point_bz,
                                  point_cx, point_cz,
                                  norm_a_y, norm_b_y, norm_c_y,
                                  pitch_input, roll_input);

        /* Feed terrain-derived pitch/roll through AngleFromVector12 exactly
         * as orig 0x004017EC (pitch, z=0x200) / 0x0040180C (roll, z=0x400).
         * The point construction + ComputeActorTrackContactNormal pipeline is
         * byte-faithful with orig (24.8 FP), so the normals are correct — the
         * earlier zeroing was the only divergence (camera failed to follow
         * slope; orig pitches/rolls the chase cam over hills and jumps). */
        /* [2026-05-28 — softened TILT_DIV/CLAMP after PM-12 matrix-order fix]
         * The matrix fix removed part of the over-amplification but the port's
         * look-at + orient-vector architecture still amplifies somewhat (raw
         * inputs make the camera swing violently up on the spawn transient).
         * Halved the divisor (was /4) and doubled the clamp (was 90) — the
         * tilt follows the slope visibly without lurching at race start. */
        /* Honolulu showed |pitch_input| ~6000 with ±100k spikes; Newcastle's real
         * slope is only ~80-130 — DIV=64 (calibrated against Honolulu) killed
         * Newcastle slopes entirely (cam tilt ~0.2°). The Honolulu spikes are
         * already caught upstream by the magnitude/sign fallback, so the
         * divisor doesn't need to defend against them. Run raw inputs like the
         * orig (DIV=1) and let CLAMP catch only the rare > ~45° excursion. */
        const int TILT_DIV   = 1;
        const int TILT_CLAMP = 512;   /* caps cam tilt at atan2(512,512) = 45° */
        int pin = pitch_input / TILT_DIV;
        int rin = roll_input  / TILT_DIV;
        if (pin >  TILT_CLAMP) pin =  TILT_CLAMP;
        if (pin < -TILT_CLAMP) pin = -TILT_CLAMP;
        if (rin >  TILT_CLAMP) rin =  TILT_CLAMP;
        if (rin < -TILT_CLAMP) rin = -TILT_CLAMP;
        /* IIR smoothing: state ← 3/4·state + 1/4·new (≈4-tick time constant).
         * Reset to the current sample on the first ticks of a race so a previous
         * race's residual (e.g. a stale spike from an OOB event) can't bleed
         * into the next race's camera. */
        static int s_pin_prev[TD5_MAX_VIEWPORTS] = {0}, s_rin_prev[TD5_MAX_VIEWPORTS] = {0};
        int vidx = view;  /* [PORT: N-way] per-view camera state (was view & 1) */
        if ((unsigned)g_td5.simulation_tick_counter < 30u) {
            s_pin_prev[vidx] = pin;
            s_rin_prev[vidx] = rin;
        } else {
            pin = (s_pin_prev[vidx] * 3 + pin) / 4;
            rin = (s_rin_prev[vidx] * 3 + rin) / 4;
            s_pin_prev[vidx] = pin;
            s_rin_prev[vidx] = rin;
        }
        /* [RESTORED 2026-05-27] Feed the scaled/clamped terrain pitch/roll
         * through AngleFromVector12 (orig 0x004017EC z=0x200 / 0x0040180C
         * z=0x400). The extra negate vs orig accounts for the port's inverted
         * tilt basis (see comment above; direction + magnitude confirmed by
         * drive-test 2026-05-26). The temporary 2026-05-27 diagnostic that
         * forced these to 0 — to isolate car-physics tilt from camera tilt —
         * is removed now that the attitude pipeline is verified byte-faithful
         * (orig 0x00446030 / 0x00405E80 / 0x00403A20); the chase cam follows
         * the road over hills/jumps again. */
        /* [RE-ENABLED 2026-05-27 PM-12 — terrain tilt RESTORED after matrix fix]
         * Was disabled in PM-11 as a symptomatic workaround for "camera bobs
         * up/down with heading on flat road" and "over-swings below the road
         * on slopes". Root cause turned out to be the SAME matrix-construction
         * bug we just fixed in BuildRotationMatrixFromAngles: the terrain_matrix
         * was being built as Rz·Rx·Ry instead of Ry·Rx·Rz, so the orient vector
         * transformed through it swung wildly off-axis. With the matrix correct,
         * the tilt should follow the slope smoothly without bobbing or clipping. */
        /* [2026-05-28 — negation removed after PM-12 matrix-order fix]
         * Was AngleFromVector12(-pin/-rin, …); the negation compensated for
         * "the port's inverted tilt basis", which was a side effect of the
         * same Rz·Rx·Ry vs Ry·Rx·Rz matrix-order bug. With the matrix correct,
         * the basis matches the orig → the negation was flipping the tilt the
         * wrong way (cam pitched UP on uphills, hiding the road). Match the
         * orig's formula (0x004017EC z=0x200 / 0x0040180C z=0x400). */
        cam_angles[0] = (short)AngleFromVector12(pin, 0x200);
        cam_angles[1] = (short)combined_angle;
        cam_angles[2] = (short)AngleFromVector12(rin, 0x400);

        BuildRotationMatrixFromAngles(terrain_matrix, cam_angles);
    }

    /* --- Camera look-direction --- */
    look_angle = (unsigned int)((g_camOrbitAngleFP[v] >> 8) -
                                 (int)*(short *)(actor + 0x20A) +
                                 g_camYawOffset[v]);

    /* [#10 NEAR-WALL ZOOM 2026-06-19] Pull the chase camera in while this view's
     * car is clipping a wall, so the wall geometry can't hide it. The TTL is
     * armed in td5_physics_wall_response (a deep wheel clip) and decays one tick
     * per solve here; the radius spring below smooths the move in and back out.
     * Cosmetic / camera-only, so safe in netplay. */
    float wall_zoom = 1.0f;
    {
        extern int16_t g_actor_near_wall[];
        int wslot = g_actorSlotForView[v];
        if (wslot >= 0 && wslot < TD5_MAX_RACER_SLOTS && g_actor_near_wall[wslot] > 0) {
            wall_zoom = 0.55f;          /* ~45% closer while pinned to a wall */
            g_actor_near_wall[wslot]--;
        }
    }

    {
        short *orient = g_camOrientShort[v];
        orient[0] = (short)((unsigned int)(int)(SinFloat12bit(look_angle) *
                    (g_camOrbitRadiusScale[v] * wall_zoom) + 0.5f) >> 8);
        orient[1] = (short)((unsigned int)g_camElevationAngleFP[v] >> 8);
        orient[2] = -(short)((unsigned int)(int)(CosFloat12bit(look_angle) *
                    (g_camOrbitRadiusScale[v] * wall_zoom) + 0.5f) >> 8);

        /* Transform through terrain rotation matrix.
         * BuildRotationMatrixFromAngles now uses the same swapped sin/cos
         * convention (s=CosFloat12bit, c=SinFloat12bit) as the inline
         * camera basis code, so this transform is correct. */
        LoadRenderRotationMatrix(terrain_matrix);
        ConvertFloatVec3ToShortAngles(orient, transformed_angles);

        orient[0] = transformed_angles[0];
        orient[1] = transformed_angles[1];
        orient[2] = transformed_angles[2];
    }

    /* --- Height/pitch angle smoother --- */
    {
        short *orient = g_camOrientShort[v];
        height_delta = (int)orient[1] * 0x100 - g_camStoredPitch[v];
        g_camStoredPitch[v] += (height_delta + ((height_delta >> 31) & 7)) >> 3;
    }

    /* --- Radius spring --- */
    {
        short *orient = g_camOrientShort[v];
        fx = orient[0];
        fz = orient[2];
        magnitude = sqrtf((float)((int)fx * (int)fx + (int)fz * (int)fz));

        g_camCurrentRadius[v] = g_camCurrentRadius[v] +
            magnitude * g_const32 -
            g_dampWeight * g_camCurrentRadius[v];
    }

    /* --- Height smoothing --- */
    g_camSmoothedHeight[v] = g_camSmoothedHeight[v] +
        (g_camTargetHeight[v] - g_camSmoothedHeight[v]) * g_dampWeight;

    /* Snapshot this sub-tick's spring output (orbit offset + smoothed height)
     * and car vertical pose so the per-render-frame finalize can interpolate
     * prev->cur and not staircase the spring at render rates above 30 Hz, nor
     * bob during the (tick-counter-frozen) countdown. Producer runs here, once
     * per sub-tick; consumer is td5_camera_finalize_chase_pos. */
    td5_camera_snapshot_spring((uintptr_t)actor, v);

    /* Final chase position is written by td5_camera_finalize_chase_pos(),
     * which also runs per-render-frame so the camera stays pinned to the
     * interpolated car mesh even when the fixed-tick sim loop runs 0 times
     * in a render frame (60fps render / 30Hz sim). See fn comment below. */
    td5_camera_finalize_chase_pos((TD5_Actor *)(uintptr_t)actor, v);
}

/* ========================================================================
 * Camera wall-clip raycast (port enhancement — NOT in original)
 *
 * The original UpdateChaseCamera + SetCameraWorldPosition (0x401590 +
 * 0x42CE50) does NO ray-vs-wall test — the camera will happily pass through
 * walls when the player reverses into one. This is a cosmetic-only port
 * enhancement that pulls the camera forward to stay outside wall geometry.
 *
 * Algorithm:
 *   - Walk a small window of spans around actor->track_span_raw (chassis
 *     span). For each span of standard type (1, 2, 5), build the LEFT
 *     and RIGHT rails (NW→SW and NE→SE corners) and perform a 2D segment-
 *     vs-segment intersection against the ray (car_xz → desired_cam_xz).
 *   - Return the smallest hit ratio t in (0, 1] across all spans+rails.
 *   - Caller multiplies the chase-offset by t * SAFETY to clip camera in.
 *
 * 2D-only: chase camera always sits "above" the car so vertical walls are
 * what matter. Rail tops/bottoms are never the failure mode.
 *
 * Coordinate space: all rail vertex coords are 24.8 fixed-point world
 * units (matching actor->world_pos.x/z and probe coords). The ray inputs
 * to the helper are also 24.8 fixed-point so the comparison is consistent.
 *
 * Damping state lives in g_camWallClipRatio[2] — last-frame's hit ratio is
 * eased toward this frame's value at g_camWallClipDamp per render frame so
 * transitions are smooth (no abrupt camera snap when entering/exiting wall
 * proximity).
 * ======================================================================== */

/* Per-view damping state. 1.0 = no clip. */
static float g_camWallClipRatio[TD5_MAX_VIEWPORTS] =
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

/* [TASK 19] Separate per-view damping state for the new-pipeline wall clip in
 * td5_camera_apply_view (kept distinct from the legacy chase clip above so the
 * two never fight over the same state). 1.0 = no clip. */
static float g_camWallClipRatioApply[TD5_MAX_VIEWPORTS] =
    { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };

/* Spans to check on each side of the chassis span. The chase camera sits
 * roughly 1500-2100 world units behind the car; 4 spans @ ~500-1000 units
 * each covers the worst-case envelope. */
#define TD5_CAM_WALL_SPAN_RADIUS  4

/* Safety margin: place clipped camera at 90% of the wall distance so it
 * sits just inside the wall plane, not exactly on it. */
#define TD5_CAM_WALL_SAFETY       0.9f

/* Damping factor toward the new ratio each render frame.
 * 0.0 = no damping (snap), 1.0 = ignore new value. */
#define TD5_CAM_WALL_DAMP         0.35f

/* Minimum normalized hit ratio. Below this the camera would be inside
 * the car mesh; clamp it. */
#define TD5_CAM_WALL_MIN_RATIO    0.15f

/* 2D segment-vs-segment intersection in 24.8 FP world coords.
 * Returns hit parameter t in (0, 1] on ray (P0→P1) if the rail (Q0→Q1)
 * blocks it; returns 1.0 if no hit. */
static float wall_clip_segments_2d(int32_t p0x, int32_t p0z,
                                   int32_t p1x, int32_t p1z,
                                   int32_t q0x, int32_t q0z,
                                   int32_t q1x, int32_t q1z)
{
    /* Cast to double early to avoid int64 overflow during cross terms.
     * Coords are 24.8 fixed-point world units so magnitudes are well
     * within double precision. */
    double rdx = (double)(p1x - p0x);
    double rdz = (double)(p1z - p0z);
    double sdx = (double)(q1x - q0x);
    double sdz = (double)(q1z - q0z);
    double denom = rdx * sdz - rdz * sdx;
    if (denom > -1.0 && denom < 1.0) return 1.0f;  /* parallel */
    double qpx = (double)(q0x - p0x);
    double qpz = (double)(q0z - p0z);
    double t = (qpx * sdz - qpz * sdx) / denom;
    double u = (qpx * rdz - qpz * rdx) / denom;
    if (t <= 0.0 || t > 1.0) return 1.0f;
    if (u < 0.0 || u > 1.0)  return 1.0f;
    return (float)t;
}

/* Compute the closest wall-hit ratio on a ray from car_xz to cam_xz.
 * Returns 1.0 if no hit, else the smallest t in (0, 1]. */
static float td5_camera_raycast_to_wall(int32_t car_x, int32_t car_z,
                                        int32_t cam_x, int32_t cam_z,
                                        int chassis_span)
{
    int span_count = td5_track_get_span_count();
    if (span_count <= 0 || chassis_span < 0 || chassis_span >= span_count)
        return 1.0f;

    float best_t = 1.0f;

    int lo = chassis_span - TD5_CAM_WALL_SPAN_RADIUS;
    int hi = chassis_span + TD5_CAM_WALL_SPAN_RADIUS;
    if (lo < 0) lo = 0;
    if (hi >= span_count) hi = span_count - 1;

    for (int si = lo; si <= hi; si++) {
        TD5_StripSpan *sp = td5_track_get_span(si);
        if (!sp) continue;

        /* Only test standard types — transition/junction/sentinel types
         * have irregular vertex layouts (see td5_track_resolve_wall_contacts
         * for the same gate). */
        int type = sp->span_type;
        if (type != 1 && type != 2 && type != 5) continue;

        int lane_count = td5_track_get_span_lane_count(si);
        if (lane_count < 1) lane_count = 1;

        int li = (int)sp->left_vertex_index;
        int ri = (int)sp->right_vertex_index;

        TD5_StripVertex *vL_near = td5_track_get_vertex(li + 0);
        TD5_StripVertex *vL_far  = td5_track_get_vertex(ri + 0);
        TD5_StripVertex *vR_near = td5_track_get_vertex(li + lane_count);
        TD5_StripVertex *vR_far  = td5_track_get_vertex(ri + lane_count);
        if (!vL_near || !vL_far || !vR_near || !vR_far) continue;

        /* Strip vertices are 16-bit local coords; lift to 24.8 FP world by
         * adding the span origin (which is already 24.8 FP). The factor of
         * 256 (<<8) matches the strip layout — see td5_track.c probe path
         * (probe.x >> 8 vs vertex coords). */
        int32_t ox = sp->origin_x;
        int32_t oz = sp->origin_z;

        int32_t lLnx = ((int32_t)vL_near->x << 8) + ox;
        int32_t lLnz = ((int32_t)vL_near->z << 8) + oz;
        int32_t lLfx = ((int32_t)vL_far->x  << 8) + ox;
        int32_t lLfz = ((int32_t)vL_far->z  << 8) + oz;

        int32_t lRnx = ((int32_t)vR_near->x << 8) + ox;
        int32_t lRnz = ((int32_t)vR_near->z << 8) + oz;
        int32_t lRfx = ((int32_t)vR_far->x  << 8) + ox;
        int32_t lRfz = ((int32_t)vR_far->z  << 8) + oz;

        /* LEFT rail: NW → SW (near-left to far-left) */
        float tL = wall_clip_segments_2d(car_x, car_z, cam_x, cam_z,
                                         lLnx, lLnz, lLfx, lLfz);
        if (tL < best_t) best_t = tL;

        /* RIGHT rail: NE → SE (near-right to far-right) */
        float tR = wall_clip_segments_2d(car_x, car_z, cam_x, cam_z,
                                         lRnx, lRnz, lRfx, lRfz);
        if (tR < best_t) best_t = tR;
    }

    return best_t;
}

/* ========================================================================
 * td5_camera_finalize_chase_pos
 *
 * Writes g_camWorldPos[view] and re-orients the camera from the current
 * chase-orbit state (g_camOrbitOffset, g_camSmoothedHeight) + the CURRENT
 * actor pose + the CURRENT g_subTickFraction velocity extrapolation. Must
 * match the velocity extrapolation applied by the car-mesh render path
 * (td5_render.c:1530-1537 — render_pos = world_pos + vel * subtick / 256)
 * so the car stays pinned in camera space as subtick advances between
 * sim ticks. Without this extrapolation, at render rates above the 30Hz
 * sim tick the car mesh extrapolates forward every frame while the camera
 * stays locked to the last tick's world_pos, producing a sawtooth shake
 * whose amplitude scales with velocity (hence "shake when accelerating").
 *
 * Called BOTH from inside UpdateChaseCamera (per tick, to keep existing
 * ordering for any tick-level consumers) AND from td5_camera_finalize_all
 * at the top of the render frame so the final g_camWorldPos uses the same
 * g_subTickFraction the car mesh will be drawn with.
 *
 * After computing the desired camera position, applies a 2D wall-clip
 * raycast (port enhancement) so the camera pulls forward when reversing
 * into a wall. The clip only pulls the camera TOWARD the car, never
 * pushes it back. Damped per-view to avoid abrupt snaps. See
 * td5_camera_raycast_to_wall above for details.
 * ======================================================================== */
void td5_camera_finalize_chase_pos(TD5_Actor *actor_p, int view)
{
    if (!actor_p) return;
    int v = view;  /* [PORT: N-way] per-view camera state (was view & 1) */
    uintptr_t actor = (uintptr_t)actor_p;

    int target[3];

    int pos_x = *(int *)(actor + 0x1FC);
    int pos_y = *(int *)(actor + 0x200);
    int pos_z = *(int *)(actor + 0x204);

    /* Velocity extrapolation — matches td5_render.c:1530-1537 and the
     * UpdateTracksideOrbitCamera pattern at td5_camera.c:834-846. */
    int vel_x_interp = (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
    int vel_y_interp = (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
    int vel_z_interp = (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

    /* [PORT ENHANCEMENT — sub-tick smoothing of the chase-spring output]
     * Rationale, producer, and the prev/cur snapshot arrays are documented at
     * td5_camera_snapshot_spring() above. The chase spring (radius/height/pitch
     * lerp) is byte-faithful and runs once per fixed 30 Hz sim sub-tick. Here
     * we interpolate its prev->cur snapshot by g_subTickFraction so the camera
     * glides smoothly at ANY render rate, instead of:
     *   - staircasing the spring once per tick (the high-MS / low-FPS jitter), and
     *   - bobbing during the countdown, where simulation_tick_counter is FROZEN
     *     at 0 so the old tick-counter-keyed interpolation never re-anchored and
     *     base_y oscillated with the per-frame g_subTickFraction beat.
     * base_y likewise interpolates the car Y: the chassis-settle SNAP
     * (0x00406300) moves world_pos.y WITHOUT a matching vertical velocity (see
     * td5_physics.c:798-810), so vel_y_interp alone cannot smooth it.
     * <=1 tick (33 ms) latency. Render-only — never feeds sim/AI/replay.
     * Toggle off with TD5RE_SMOOTH_CAM=0 to A/B against the faithful path. */
    static int s_smoothCam = -1;
    if (s_smoothCam < 0) {
        s_smoothCam = td5_env_flag_on("TD5RE_SMOOTH_CAM");   /* default ON */
    }

    int off0, off1, off2, smoothed_h, base_y;
    /* During the new pipeline's per-tick capture (s_cam_capture_view==v,
     * g_subTickFraction forced to 0) take the raw spring output — the prev->cur
     * interpolation now happens in td5_camera_apply_view, not here. */
    if (s_smoothCam && s_camSmoothInit[v] && s_cam_capture_view < 0) {
        float f = g_subTickFraction;
        off0 = (int)((float)s_camOffPrev[v][0]
                     + (float)(s_camOffCur[v][0] - s_camOffPrev[v][0]) * f + 0.5f);
        off1 = (int)((float)s_camOffPrev[v][1]
                     + (float)(s_camOffCur[v][1] - s_camOffPrev[v][1]) * f + 0.5f);
        off2 = (int)((float)s_camOffPrev[v][2]
                     + (float)(s_camOffCur[v][2] - s_camOffPrev[v][2]) * f + 0.5f);
        smoothed_h = (int)(s_camHPrev[v] + (s_camHCur[v] - s_camHPrev[v]) * f + 0.5f);
        base_y = (int)((float)s_camYPrev[v]
                       + (float)(s_camYCur[v] - s_camYPrev[v]) * f + 0.5f);
    } else {
        /* Faithful path: consume the current spring output directly. */
        off0 = g_camOrbitOffset[v][0];
        off1 = g_camOrbitOffset[v][1];
        off2 = g_camOrbitOffset[v][2];
        smoothed_h = (int)(g_camSmoothedHeight[v] + 0.5f);
        base_y = pos_y + vel_y_interp;
    }

    /* Desired (unclipped) camera position from existing chase logic. */
    int cam_x_desired = pos_x + off0 + vel_x_interp;
    int cam_y_desired = base_y + off1;
    int cam_z_desired = pos_z + off2 + vel_z_interp;

    /* Wall-clip raycast (port enhancement, see comment block above). [TASK 19]
     * Ray origin = car center (with vel extrapolation); end = desired
     * camera. If a wall blocks the line, t < 1 and we pull camera in to
     * t * SAFETY along the chase offset. Y is left at desired since the
     * test is 2D and the camera already sits above wall tops in practice.
     *
     * Skip here while CAPTURING for the new pipeline (s_cam_capture_view >= 0):
     * td5_camera_apply_view does the single authoritative per-frame clip then,
     * so clipping the captured (sub-tick 0) eye too would double-pull it.
     * Disabled entirely when TD5RE_CAM_WALL_AVOID=0. */
    int chassis_span = (int)(*(short *)(actor + 0x80));
    float clip;
    if (s_cam_capture_view < 0 && td5_camera_wall_avoid()) {
        int32_t car_x_ray = pos_x + vel_x_interp;
        int32_t car_z_ray = pos_z + vel_z_interp;
        float raw_t = td5_camera_raycast_to_wall(car_x_ray, car_z_ray,
                                                 cam_x_desired, cam_z_desired,
                                                 chassis_span);
        if (raw_t < TD5_CAM_WALL_MIN_RATIO) raw_t = TD5_CAM_WALL_MIN_RATIO;
        if (raw_t > 1.0f) raw_t = 1.0f;

        /* Damp toward the new ratio. Use a snappier response when pulling IN
         * (raw_t < current) so the camera doesn't lag a fast wall-poke, but a
         * slower release when going back out — this matches typical chase-cam
         * conventions and prevents the camera from oscillating against a wall. */
        float cur = g_camWallClipRatio[v];
        float damp = (raw_t < cur) ? (TD5_CAM_WALL_DAMP * 2.0f) : TD5_CAM_WALL_DAMP;
        if (damp > 1.0f) damp = 1.0f;
        cur = cur + (raw_t - cur) * damp;
        if (cur < TD5_CAM_WALL_MIN_RATIO) cur = TD5_CAM_WALL_MIN_RATIO;
        if (cur > 1.0f) cur = 1.0f;
        g_camWallClipRatio[v] = cur;

        /* Apply the (damped) clip. Only the X/Z (lateral) offset is shrunk —
         * Y stays at desired so the camera doesn't dive when it pulls forward. */
        clip = cur * TD5_CAM_WALL_SAFETY;
        if (clip > 1.0f) clip = 1.0f;
        if (clip < 0.0f) clip = 0.0f;
    } else {
        clip = 1.0f;   /* capturing for new pipeline, or avoidance disabled */
    }

    g_camWorldPos[v][0] = pos_x + vel_x_interp +
                          (int)((float)off0 * clip + 0.5f);
    g_camWorldPos[v][1] = cam_y_desired;
    g_camWorldPos[v][2] = pos_z + vel_z_interp +
                          (int)((float)off2 * clip + 0.5f);

    /* [REVERTED 2026-05-27 PM-10] A BLANKET ground-clamp here (probe terrain at
     * the camera XZ, raise cam Y above it, for ALL modes) traded the slope clip
     * for worse bugs: the chassis-span lane probe is unreliable at the camera's
     * orbiting XZ (several spans behind), returning garbage/zero → camera
     * reset/stop-follow, and on flat road the per-heading probe variance made
     * the camera bob with the car's facing. For NORMAL chase (vehicle_mode == 0)
     * that diagnosis stands — no clamp; the slope-tilt over-swing is fixed at
     * the source (UpdateChaseCamera ~L860). The targeted floor below applies
     * ONLY to the invisible-car mode and dodges both failure modes (walked +
     * validated probe; one-sided eye-only raise with a small offset, so it
     * never triggers on flat road). See td5_camera_probe_ground_floor above. */

    /* [PORT ENHANCEMENT — invisible-car (vehicle_mode != 0) ground floor] */
    if (*(char *)(actor + 0x379) != 0) {
        int floor_y;
        if (td5_camera_probe_ground_floor(actor, g_camWorldPos[v][0],
                                          g_camWorldPos[v][2], &floor_y) &&
            g_camWorldPos[v][1] < floor_y) {
            static uint32_t s_floor_log_ctr;
            if ((s_floor_log_ctr++ % 120u) == 0u)
                TD5_LOG_D(LOG_TAG, "invis-cam ground floor v%d: eye Y %d -> %d",
                          v, g_camWorldPos[v][1], floor_y);
            g_camWorldPos[v][1] = floor_y;
        }
    }

    target[0] = pos_x + vel_x_interp;
    target[1] = base_y + smoothed_h;   /* smoothed base (see high-FPS note above) */
    target[2] = pos_z + vel_z_interp;

    SetCameraWorldPosition(g_camWorldPos[v]);
    OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);
}

/* Per-render-frame finalization entry point. Called once per render frame
 * from td5_game.c after the fixed-tick sim loop (and AFTER g_subTickFraction
 * has been recomputed from the post-drain accumulator remainder). */
static TD5_Actor *camera_actor_for_view(int v);  /* forward decl — defined below near td5_camera_tick */

void td5_camera_finalize_all(void)
{
    if (td5_camera_use_new_pipeline()) return;  /* new path finalizes per-viewport in td5_camera_apply_view */
    if (td5_game_get_total_actor_count() <= 0) return;
    int view_count = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
    for (int v = 0; v < view_count; v++) {
        TD5_Actor *actor = camera_actor_for_view(v);
        if (!actor) continue;
        td5_camera_finalize_chase_pos(actor, v);
    }
}

/* ========================================================================
 * [CAMERA REWRITE 2026-06-07] Unified per-tick solve + per-frame present.
 * ======================================================================== */

static int cam_lerp_i(int a, int b, float f)
{
    float d = (float)(b - a) * f;
    return a + (int)(d + (d >= 0.0f ? 0.5f : -0.5f));
}

/* Shortest-path interpolation of a 12-bit (0x1000) angle. */
static short cam_lerp_angle(short a, short b, float f)
{
    int d = ((int)b - (int)a) & 0xFFF;
    if (d > 0x800) d -= 0x1000;
    float fd = (float)d * f;
    int r = (int)a + (int)(fd + (fd >= 0.0f ? 0.5f : -0.5f));
    return (short)(r & 0xFFF);
}

/* Re-seed prev := cur on the next solve for every view (race start, preset
 * change, resume-from-pause) so apply_view does not glide across a teleport. */
void td5_camera_snap_poses(void)
{
    for (int v = 0; v < TD5_MAX_VIEWPORTS; v++)
        s_cam_pose_init[v] = 0;
}

/* [CAR DAMAGE 2026-06-28] When this view's car is done for the race (finished or
 * wrecked) but the race is still running (the post-finish hold), spin the chase
 * camera's yaw offset so it ORBITS the stationary car — letting the player see
 * the accumulated damage before results. Chase-cam paths call this just before
 * UpdateChaseCamera (which reads g_camYawOffset[v]). No-op unless CarDamage +
 * the finish-orbit knob are on. */
static void cam_finish_orbit_step(int v, TD5_Actor *actor)
{
    if (!td5_damage_finish_orbit_enabled() || !actor) return;
    if (v < 0 || v >= TD5_MAX_VIEWPORTS) return;
    int slot = (int)actor->slot_index;
    if (slot < 0) return;
    if (!td5_game_slot_is_finished(slot) && !td5_damage_slot_knocked_out(slot)) return;

    static int s_orbit_logged[TD5_MAX_VIEWPORTS] = {0};
    if (!s_orbit_logged[v]) {
        s_orbit_logged[v] = 1;
        TD5_LOG_I(LOG_TAG, "car_damage finish-orbit ENGAGED view=%d slot=%d "
                  "(finished=%d wrecked=%d)", v, slot,
                  td5_game_slot_is_finished(slot), td5_damage_slot_knocked_out(slot));
    }
    g_camYawOffset[v] = (g_camYawOffset[v] + td5_damage_finish_orbit_speed()) & 0xFFF;
}

/* Solve one view's desired tick pose by running its existing mode updater with
 * g_subTickFraction == 0 and capturing the eye/target/angles it emits. */
static void cam_solve_view(int v)
{
    TD5_Actor *actor = camera_actor_for_view(v);
    if (!actor) return;

    if (s_cam_pose_init[v])
        s_cam_pose_prev[v] = s_cam_pose_cur[v];     /* roll prev <- last tick */

    TD5_CamPose *C = &s_cam_pose_cur[v];
    int eye_lock = 1, tgt_lock = 1;

    /* Fly-in spring-reset one-shot (mirrors td5_camera_update_transition_state). */
    if (!s_flyin_preset_reloaded[v] && !g_td5.paused) {
        s_flyin_preset_reloaded[v] = 1;
        g_raceCameraPresetId[v]   = 0;
        g_raceCameraPresetMode[v] = 0;
        TD5_CameraPreset *p = &g_cameraPresets[0];
        g_camOrbitRadiusScale[v] = (float)(int)p->orbit_radius_raw  * g_const256;
        g_camTargetHeight[v]     = (float)(int)p->height_target_raw * g_const256;
        g_camElevationAngleFP[v] = (int)p->elevation_angle << 8;
    }

    float save = g_subTickFraction;
    g_subTickFraction = 0.0f;     /* kill vel-extrapolation; integrators use cam_integ_step() */
    s_cam_capture_view = v;

    if (g_cameraTransitionActive > 0 && !s_flyin_preset_reloaded[v]) {
        /* Race-start fly-in — now advances once per TICK (was per frame/viewport). */
        UpdateChaseCamera((int)actor, 1, v);
        UpdateRaceCameraTransitionState((int)actor, v);
        td5_camera_finalize_chase_pos(actor, v);
        eye_lock = 1; tgt_lock = 1;
    } else if (g_replay_mode && td5_camera_replay_trackside_ready()) {
        SelectTracksideCameraProfile((int)actor, v);
        UpdateTracksideCamera((int)actor, v);
        eye_lock = 0; tgt_lock = 1;     /* trackside eye is world-fixed; look-at tracks car */
        /* [ITEM #18 camera] Keep the cinematic trackside eye above the road
         * surface (one-sided raise). The captured eye lives in C->eye; the
         * basis is rebuilt downstream in td5_camera_apply_view from this eye +
         * the (untouched) captured look-at target, so raising C->eye[1] here is
         * sufficient — no re-orient needed. Vehicle-relative behaviours (1/2/
         * 7/8) leave build_mode==1 and follow the car, so this only affects the
         * look-at trackside cams (static / spline / orbit). */
        if (C->build_mode == 0)
            td5_camera_replay_eye_floor_clamp((uintptr_t)actor, v, C->eye);
    } else if (g_raceCameraPresetMode[v] != 0 && !g_td5.paused) {
        UpdateVehicleRelativeCamera((int)actor, v);   /* bumper / in-car (euler basis) */
        eye_lock = 1; tgt_lock = 1;
    } else {
        cam_finish_orbit_step(v, actor);   /* [CAR DAMAGE] end-of-race damage orbit */
        UpdateChaseCamera((int)actor, 1, v);
        td5_camera_finalize_chase_pos(actor, v);
        eye_lock = 1; tgt_lock = 1;
    }

    s_cam_capture_view = -1;
    g_subTickFraction = save;

    C->anchor[0] = actor->world_pos.x;
    C->anchor[1] = actor->world_pos.y;
    C->anchor[2] = actor->world_pos.z;
    C->anchor_valid   = 1;
    C->eye_car_locked = (unsigned char)eye_lock;
    C->tgt_car_locked = (unsigned char)tgt_lock;
    C->fov_factor     = g_depthFovFactor;
    C->valid          = 1;

    if (!s_cam_pose_init[v]) {
        s_cam_pose_prev[v] = s_cam_pose_cur[v];   /* seed prev=cur on first solve (no glide) */
        s_cam_pose_init[v] = 1;
    }

    /* [CAMERA REWRITE] FPS-independence harness: TD5RE_CAM_LOG=1 dumps the solved
     * pose once per sim tick. Keyed on simulation_tick_counter (NOT frame), the
     * stream must be byte-identical at 30/60/180 fps to prove tick-locked dynamics. */
    {
        static int s_cam_log = -1;
        if (s_cam_log < 0) s_cam_log = td5_env_flag_off("TD5RE_CAM_LOG");
        if (s_cam_log) {
            TD5_LOG_I(LOG_TAG,
                "campose tick=%d v=%d eye=(%d,%d,%d) tgt=(%d,%d,%d) ang=(%d,%d,%d) mode=%d eyelock=%d fov=%d transA=0x%X",
                (int)g_td5.simulation_tick_counter, v,
                C->eye[0], C->eye[1], C->eye[2],
                C->target[0], C->target[1], C->target[2],
                (int)C->angles[0], (int)C->angles[1], (int)C->angles[2],
                (int)C->build_mode, (int)C->eye_car_locked, C->fov_factor,
                g_cameraTransitionActive);
        }
    }
}

/* Per-sim-tick: solve every active view's camera pose (FPS-independent). */
void td5_camera_solve_tick_all(void)
{
    if (!td5_camera_use_new_pipeline()) { td5_camera_update_chase_all(); return; }
    if (td5_game_get_total_actor_count() <= 0) {
        /* No race actors yet: debug fly-around (already tick-keyed). Capture it. */
        int v = 0;
        if (s_cam_pose_init[v]) s_cam_pose_prev[v] = s_cam_pose_cur[v];
        TD5_CamPose *C = &s_cam_pose_cur[v];
        float save = g_subTickFraction;
        g_subTickFraction = 0.0f;
        s_cam_capture_view = v;
        update_debug_race_camera(v);
        s_cam_capture_view = -1;
        g_subTickFraction = save;
        C->anchor_valid = 0; C->eye_car_locked = 0; C->tgt_car_locked = 0;
        C->fov_factor = g_depthFovFactor; C->valid = 1;
        if (!s_cam_pose_init[v]) { s_cam_pose_prev[v] = s_cam_pose_cur[v]; s_cam_pose_init[v] = 1; }
        return;
    }

    int view_count = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
    if (view_count > TD5_MAX_VIEWPORTS) view_count = TD5_MAX_VIEWPORTS;
    for (int v = 0; v < view_count; v++)
        cam_solve_view(v);
}

/* Per-render-frame, per-viewport: interpolate the solved pose and re-pin the
 * car-locked components to the body-mesh extrapolation, then build the basis. */
void td5_camera_apply_view(int view)
{
    if (!td5_camera_use_new_pipeline()) { td5_camera_update_transition_state(view, view); return; }
    int v = view;
    if (v < 0 || v >= TD5_MAX_VIEWPORTS) return;
    TD5_CamPose *C = &s_cam_pose_cur[v];
    if (!C->valid) return;
    TD5_CamPose *P = s_cam_pose_init[v] ? &s_cam_pose_prev[v] : C;

    float f = g_subTickFraction;
    if (f < 0.0f) f = 0.0f; else if (f > 1.0f) f = 1.0f;

    /* Body-matching anchor: velocity-extrapolate ALL THREE axes so the followed
     * car is pixel-pinned in frame, matching the body-mesh render position
     * world_pos + linear_velocity*g_subTickFraction (RenderRaceActorForView @
     * 0x40C164 / td5_render.c:3955-3957).
     * [WOBBLE FIX 2026-06-29] Y previously sub-tick INTERPOLATED prev->cur while
     * X/Z (and the body mesh) EXTRAPOLATED — the car was pinned horizontally but
     * bobbed vertically against its own mesh once per 30 Hz tick. On high-refresh
     * monitors (render >> 30 Hz) that beat reads as a fast vertical jitter at
     * race start (suspension settling moves Y) that scales with motion and stops
     * when the car is stationary (handbrake). Extrapolating Y like the body pins
     * all three axes. TD5RE_CAM_PIN_Y=0 reverts to the old interpolation. */
    int ax = C->anchor[0], ay = C->anchor[1], az = C->anchor[2];
    if (C->anchor_valid) {
        TD5_Actor *a = camera_actor_for_view(v);
        if (a) {
            ax = a->world_pos.x + (int)((float)a->linear_velocity_x * f);
            az = a->world_pos.z + (int)((float)a->linear_velocity_z * f);
            if (td5_camera_pin_anchor_y()) {
                ay = a->world_pos.y + (int)((float)a->linear_velocity_y * f);
            } else {
                int dy = C->anchor[1] - P->anchor[1];
                if (dy > 0x40000 || dy < -0x40000) ay = C->anchor[1];
                else ay = P->anchor[1] + (int)((float)dy * f + (dy >= 0 ? 0.5f : -0.5f));
            }
        }
    }

    int eye[3], target[3];
    if (C->eye_car_locked && C->anchor_valid) {
        eye[0] = ax + cam_lerp_i(P->eye[0]-P->anchor[0], C->eye[0]-C->anchor[0], f);
        eye[1] = ay + cam_lerp_i(P->eye[1]-P->anchor[1], C->eye[1]-C->anchor[1], f);
        eye[2] = az + cam_lerp_i(P->eye[2]-P->anchor[2], C->eye[2]-C->anchor[2], f);
    } else {
        eye[0] = cam_lerp_i(P->eye[0], C->eye[0], f);
        eye[1] = cam_lerp_i(P->eye[1], C->eye[1], f);
        eye[2] = cam_lerp_i(P->eye[2], C->eye[2], f);
    }
    if (C->tgt_car_locked && C->anchor_valid) {
        target[0] = ax + cam_lerp_i(P->target[0]-P->anchor[0], C->target[0]-C->anchor[0], f);
        target[1] = ay + cam_lerp_i(P->target[1]-P->anchor[1], C->target[1]-C->anchor[1], f);
        target[2] = az + cam_lerp_i(P->target[2]-P->anchor[2], C->target[2]-C->anchor[2], f);
    } else {
        target[0] = cam_lerp_i(P->target[0], C->target[0], f);
        target[1] = cam_lerp_i(P->target[1], C->target[1], f);
        target[2] = cam_lerp_i(P->target[2], C->target[2], f);
    }

    g_depthFovFactor = cam_lerp_i(P->fov_factor, C->fov_factor, f);
    s_cam_capture_view = -1;          /* build live, not capture */

    /* [ITEM #12] Decaying crash shake for this view's player slot. Computed
     * once here (per render frame) and applied to the finished eye below;
     * never captured into the per-tick pose, so determinism is untouched. */
    int   shake_eye[3] = {0,0,0};
    short shake_ang[3] = {0,0,0};
    int   shake_active = td5_camera_compute_crash_shake(v, shake_eye, shake_ang);

    if (C->build_mode == 1) {
        /* In-car / bumper ("ground") cam + trackside offset replay cams.
         * [TASK 22] Lift the eye to a usable hood/window height and re-apply
         * the D3D11 handedness flip that BuildCameraBasisFromAngles lacks (the
         * new pipeline rebuilds the basis here, discarding the flip baked into
         * UpdateVehicleRelativeCamera at solve time). */
        if (td5_camera_ground_cam_fix())
            eye[1] += td5_camera_ground_cam_lift();
        if (shake_active) {
            eye[0] += shake_eye[0]; eye[1] += shake_eye[1]; eye[2] += shake_eye[2];
        }
        SetCameraWorldPosition(eye);
        short ang[3];
        ang[0] = cam_lerp_angle(P->angles[0], C->angles[0], f);
        ang[1] = cam_lerp_angle(P->angles[1], C->angles[1], f);
        ang[2] = cam_lerp_angle(P->angles[2], C->angles[2], f);
        if (shake_active) {
            /* Tiny roll "rattle" — the in-car basis is built straight from
             * these euler angles, so a small roll term reads as a jolt. */
            ang[2] = (short)((int)ang[2] + (int)shake_ang[2]);
        }
        BuildCameraBasisFromAngles(ang);
        if (td5_camera_ground_cam_fix())
            cam_apply_incar_handedness_flip();
    } else {
        /* Look-at cameras (chase + trackside orbit/static/spline). [TASK 19]
         * Pull the eye in before it clips a wall/rail, covering BOTH gameplay
         * chase and replay trackside (this is the only per-frame eye-write for
         * the new pipeline). Ray = look-at target -> eye; if a wall blocks it,
         * shrink the eye-target vector by the (damped) hit ratio. */
        if (td5_camera_wall_avoid() && C->anchor_valid) {
            TD5_Actor *wa = camera_actor_for_view(v);
            if (wa) {
                int chassis_span = (int)(*(short *)((uintptr_t)wa + 0x80));
                float raw_t = td5_camera_raycast_to_wall(
                    target[0], target[2], eye[0], eye[2], chassis_span);
                if (raw_t < TD5_CAM_WALL_MIN_RATIO) raw_t = TD5_CAM_WALL_MIN_RATIO;
                if (raw_t > 1.0f) raw_t = 1.0f;
                /* Snappier when pulling IN, slower releasing out (avoids
                 * oscillation against a wall) — same policy as the chase clip. */
                float cur = g_camWallClipRatioApply[v];
                float damp = (raw_t < cur) ? (TD5_CAM_WALL_DAMP * 2.0f) : TD5_CAM_WALL_DAMP;
                if (damp > 1.0f) damp = 1.0f;
                cur = cur + (raw_t - cur) * damp;
                if (cur < TD5_CAM_WALL_MIN_RATIO) cur = TD5_CAM_WALL_MIN_RATIO;
                if (cur > 1.0f) cur = 1.0f;
                g_camWallClipRatioApply[v] = cur;
                float clip = cur * TD5_CAM_WALL_SAFETY;
                if (clip < 0.0f) clip = 0.0f; else if (clip > 1.0f) clip = 1.0f;
                if (clip < 1.0f) {
                    /* Pull the whole eye-target vector in (X/Y/Z) so a clipped
                     * eye also rises toward the car instead of dropping low. */
                    eye[0] = target[0] + (int)((float)(eye[0] - target[0]) * clip + 0.5f);
                    eye[1] = target[1] + (int)((float)(eye[1] - target[1]) * clip + 0.5f);
                    eye[2] = target[2] + (int)((float)(eye[2] - target[2]) * clip + 0.5f);
                }
            }
        }
        /* [ITEM #12] Jitter the eye AFTER the wall-avoid clip so the rattle
         * rides on the clipped position. The shake is small (~tens of world
         * units) so it won't re-clip into geometry; the look-at target is left
         * untouched so the followed car stays centred while the frame shakes. */
        if (shake_active) {
            eye[0] += shake_eye[0]; eye[1] += shake_eye[1]; eye[2] += shake_eye[2];
        }
        SetCameraWorldPosition(eye);
        OrientCameraTowardTarget(target, (unsigned int)C->yaw_offset);
    }
}

/* ========================================================================
 * 0x401950 -- UpdateTracksideOrbitCamera
 *
 * Orbiting camera around a vehicle, used for trackside replay.
 * Smooths orbit counter using subTickFraction interpolation.
 *
 * [CONFIRMED @ 0x00401950 UpdateTracksideOrbitCamera; L5 sweep 2026-05-21]
 *   Decompile verified line-by-line. Active branch matches orig:
 *     - abs(yaw_delta)>>11 + 5 effective speed clamp; fly-in inversion
 *     - orbit_angle_fp accumulator update via ROUND(delta*eff>>8 * subTick)
 *     - vis_angle = orbit>>8; heading = vis_angle - actor.display_yaw +
 *       g_cameraYawOffsetView[v]
 *     - g_cameraOrbitAngleShortVec writes (sin/-cos)*radius + stored_pitch
 *     - LoadRenderRotationMatrix + ConvertFloatVec3ToShortAngles
 *     - orbit_offset[0..2] = (sin, stored_pitch, -cos) * (yawOff - vis_angle)
 *     - world_pos additions of actor xyz + vel*subTick
 *     - OrientCameraTowardTarget(target, g_tracksideYawOffset)
 *
 * [ARCH-DIVERGENCE — FPU rounding mode]
 *   Orig uses ROUND() (FISTP / RNE). Port uses (int)(x + 0.5f). Per-call
 *   1-LSB drift possible; visual-only output, not part of sim cascade.
 *
 * NOTE: Lines 1104-1117 retain dead scribble code (overwritten by 1118-1119).
 *       Final state is correct; recommend cleanup pass to remove cruft.
 * ======================================================================== */

void UpdateTracksideOrbitCamera(int actor, int is_active, int view)
{
    int orbit_angle_fp;
    int v = view;
    float radius_scaled;
    unsigned int heading;
    int vel_x_interp, vel_y_interp, vel_z_interp;
    int target[3];

    /* Update orbit angle if active and alive */
    if (*(char *)(actor + 0x379) == 0 && is_active != 0) {
        int delta = g_camHeadingDelta20[v];
        int abs_d = ((delta >> 0x0B) ^ (delta >> 0x1F)) - (delta >> 0x1F);
        int speed = abs_d + 5;
        int effective = 15;
        if (speed > 14) effective = speed;

        int fly_in = g_camFlyInCounter[v];
        if (effective <= fly_in) {
            effective = speed;
            if (speed < 15) effective = 15;
        }

        /* Smooth by subTickFraction */
        orbit_angle_fp = (int)((float)((delta * effective) >> 8) * cam_integ_step() + 0.5f) +
                         g_camOrbitAngleFP[v];
    } else {
        orbit_angle_fp = g_camOrbitAngleFP[v];
    }

    /* Compute camera offset */
    radius_scaled = g_worldToRenderScale * g_camOrbitRadiusScale[v];
    unsigned int vis_angle = (unsigned int)(orbit_angle_fp >> 8);

    /* Look direction for orientation */
    heading = vis_angle - (unsigned int)*(short *)(actor + 0x20A) + g_camYawOffset[v];

    {
        short orient[4];
        orient[0] = (short)(int)(SinFloat12bit(heading) * radius_scaled + 0.5f);
        orient[1] = (short)((unsigned int)g_camElevationAngleFP[v] >> 8);
        orient[2] = (short)(int)(-(CosFloat12bit(heading) * radius_scaled) + 0.5f);

        /* Transform orient through actor's rotation matrix.
         * [CONFIRMED @ 0x401950]: original calls LoadRenderRotationMatrix
         * with actor+0x120 then ConvertFloatVec3ToShortAngles. */
        LoadRenderRotationMatrix((float *)(actor + 0x120));
        {
            short out_ang[4];
            ConvertFloatVec3ToShortAngles(orient, out_ang);
            g_camOrientShort[v][0] = out_ang[0];
            g_camOrientShort[v][1] = out_ang[1];
        }
    }

    /* Smooth height */
    {
        float h_delta = (g_camTargetHeight[v] - g_camSmoothedHeight[v]) * g_dampWeight;
        int smoothed_h = (int)(h_delta * cam_integ_step() + g_camSmoothedHeight[v] + 0.5f);

        /* Orbit position from Ghidra 0x00401950:
         *   X =  sin(orbit_vis) * radius
         *   Y =  stored pitch (no orbit component on vertical)
         *   Z = -cos(orbit_vis) * radius
         * (REG-8 cleanup 2026-05-22: removed 4 stale authoring-draft overwrites
         *  of [v][0]/[v][2]; final writes preserved identically.) */
        unsigned int orbit_vis = (unsigned int)(g_camYawOffset[v] - (int)vis_angle);
        float radius = g_camCurrentRadius[v];

        g_camOrbitOffset[v][0] = (int)(SinFloat12bit((int)orbit_vis) * radius + 0.5f);
        g_camOrbitOffset[v][1] = g_camStoredPitch[v];
        g_camOrbitOffset[v][2] = (int)(-(CosFloat12bit(orbit_vis) * radius) + 0.5f);

        /* Camera world pos = vehicle pos + orbit offset + velocity interpolation */
        g_camWorldPos[v][0] = *(int *)(actor + 0x1FC) + g_camOrbitOffset[v][0];
        g_camWorldPos[v][1] = g_camOrbitOffset[v][1] + *(int *)(actor + 0x200);
        g_camWorldPos[v][2] = *(int *)(actor + 0x204) + g_camOrbitOffset[v][2];

        /* Add velocity * subTickFraction */
        vel_x_interp = (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
        g_camWorldPos[v][0] += vel_x_interp;

        vel_y_interp = (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
        g_camWorldPos[v][1] += vel_y_interp;

        vel_z_interp = (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);
        g_camWorldPos[v][2] += vel_z_interp;

        /* Target = vehicle pos + smoothed height + velocity interpolation */
        target[0] = *(int *)(actor + 0x1FC) + vel_x_interp;
        target[1] = *(int *)(actor + 0x200) + smoothed_h + vel_y_interp;
        target[2] = *(int *)(actor + 0x204) + vel_z_interp;

        SetCameraWorldPosition(g_camWorldPos[v]);
        OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);
    }

    {
        static uint32_t s_chase_log_ctr;
        if ((s_chase_log_ctr++ % 30u) == 0u) {
            TD5_LOG_D(LOG_TAG,
                      "chase view %d: pos=(%.2f, %.2f, %.2f) orbit_radius=%.2f target_actor=%d",
                      v,
                      g_cameraPos[0], g_cameraPos[1], g_cameraPos[2],
                      g_camCurrentRadius[v], g_actorSlotForView[v]);
        }
    }
}

/* ========================================================================
 * 0x401C20 -- UpdateVehicleRelativeCamera
 *
 * Camera rigidly attached to vehicle with smoothed orientation.
 * Interpolates Euler angles via shortest-path wrapping.
 *
 * [L5 promotion sweep audit 2026-05-18 — byte-equivalent]
 *   Decompile of 0x00401C20 verified line-by-line:
 *     - First angle target  = display_angles.roll  + 0x800   (+0x208)
 *     - Second angle target = 0x800 - display_angles.yaw     (+0x20A)
 *     - Third  angle target = -display_angles.pitch          (+0x20C)
 *     [variable names in port read target_pitch/yaw/roll but the math
 *      maps roll/yaw/pitch in the actor struct convention — math identity
 *      preserved]
 *   Per-axis shortest-path wrap delta (`((target-cached-0x800)&0xFFF) -
 *   0x800) * g_subTickFraction`) reproduced exactly. cam_angles[1] (yaw)
 *   gets the g_camYawOffset[v] addition, matching uStack_a addition with
 *   DAT_00482f70[viewIndex*4] in the original.
 *   Offset transform through actor rotation matrix + 0x100-scaling +
 *   world_pos + (linear_velocity * g_subTickFraction) is identical.
 *   BuildCameraBasisFromAngles called twice (pre- and post-position set)
 *   matches orig's redundant rebuild pattern.
 *
 * [ARCH-DIVERGENCE — rotation matrix indirection]
 *   Original transforms the offset vector through `g_raceRotationMatrixPtr`
 *   (pointer-deref to the per-actor render matrix at 0x4aaeec). Port reaches
 *   the same data via `g_renderBasisMatrix` (direct 12-float buffer kept in
 *   sync). Value identity preserved; only the addressing differs.
 *
 * [ARCH-DIVERGENCE — FPU rounding mode]
 *   Original uses ROUND() (FISTP / round-to-nearest-even). Port uses
 *   `(int)(x + 0.5f)` (round-toward-positive-infinity for non-negative,
 *   away-from-zero). For half-LSB-class values the two differ by 1 unit.
 *   Tracked under the broader FISTP-RNE vs floorf/+0.5 cleanup
 *   (todo_chassis_snap_fix_2026-05-16.md). Camera position output is
 *   visual-only — divergence does not enter the sim cascade.
 * ======================================================================== */

void UpdateVehicleRelativeCamera(int actor, int view)
{
    int v = view;
    short *cached = g_camCachedAngles[v];
    short target_pitch, target_yaw, target_roll;
    short delta;
    short cam_angles[3];
    int cam_pos[3];

    /* Target angles from vehicle (with 180-degree offset for chase view) */
    target_pitch = *(short *)(actor + 0x208) + 0x800;
    target_yaw   = 0x800 - *(short *)(actor + 0x20A);
    target_roll  = -*(short *)(actor + 0x20C);

    /* Smooth pitch toward target via shortest-path wrapping */
    {
        int wrapped = td5_angle12_delta(target_pitch, cached[0]);
        delta = (short)(int)((float)wrapped * cam_integ_step() + 0.5f);
    }
    cam_angles[0] = cached[0] + delta;

    /* Smooth yaw */
    {
        int wrapped = td5_angle12_delta(target_yaw, cached[1]);
        delta = (short)(int)((float)wrapped * cam_integ_step() + 0.5f);
    }
    cam_angles[1] = cached[1] + delta;

    /* Smooth roll */
    {
        int wrapped = td5_angle12_delta(target_roll, cached[2]);
        delta = (short)(int)((float)wrapped * cam_integ_step() + 0.5f);
    }
    cam_angles[2] = delta + cached[2];

    /* Add yaw offset for look-left/right */
    cam_angles[1] = (cam_angles[1] + (short)g_camYawOffset[v]) & 0xFFF;

    BuildCameraBasisFromAngles(cam_angles);

    /* Transform offset vector through vehicle's rotation matrix */
    {
        /* DAT_004aaeec is a pointer to some global matrix; in the original this is
           passed as a float* from a fixed address. We use the actor's rotation matrix. */
        extern float g_renderBasisMatrix[12]; /* 0x4AAEEC points to this */
        /* [FIX 2026-05-24 OVERSIGHT: case_1_2_basis_transform; orig 0x00401C20]
         * Orig UpdateVehicleRelativeCamera calls ConvertFloatVec3ToIntVec3
         * @ 0x0042DB40 (short-clamped), not TransformVector3ByBasis. */
        ConvertFloatVec3ToIntVec3(g_renderBasisMatrix, &g_camOffsetVec[v][0], cam_pos);
    }

    /* Scale offset and add vehicle position + velocity interpolation */
    cam_pos[0] = cam_pos[0] * 0x100 + *(int *)(actor + 0x1FC) +
                 (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
    cam_pos[1] = cam_pos[1] * 0x100 + *(int *)(actor + 0x200) +
                 (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
    cam_pos[2] = cam_pos[2] * 0x100 + *(int *)(actor + 0x204) +
                 (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

    SetCameraWorldPosition(cam_pos);

    /* Rebuild basis a second time (the original does this) */
    BuildCameraBasisFromAngles(cam_angles);

    /* Port D3D11 handedness compensation for the in-car (bumper) camera.
     *
     * The original's in-car Euler basis (BuildCameraBasisFromAngles @ 0x42d0b0,
     * which applies NO coordinate flip) renders upright in the original's
     * software projector. The port's D3D11 transform consumes g_cameraBasis
     * directly as s_camera_basis (td5_render.c:1534-1536, rows = right/up/fwd)
     * with the opposite up/right handedness, so the un-flipped Euler basis
     * comes out rolled 180° about forward — i.e. upside down. The CHASE camera
     * never hits this because it finalizes through OrientCameraTowardTarget
     * (0x42d5b0), which applies its OWN coordinate flip ([5]-negate +
     * {-1,0,0,0,1,0,0,0,-1}); BuildCameraBasisFromAngles has no such flip.
     *
     * Measured at runtime: in-car Up came out (0,-1,0) for a level car. Apply the
     * matching correction — a 180° roll about forward: negate right (row 0) and
     * up (row 1), keep forward (row 2). That makes Up = (0,+1,0) without
     * touching the look direction (no left/right mirror, determinant preserved).
     * g_cameraBasisWork (FinalizeCameraProjectionMatrices output) is unused by
     * the port renderer, so negating g_cameraBasis here is sufficient.
     * [Port-specific render-compat divergence; not in orig 0x401c20.] */
    g_cameraBasis[0] = -g_cameraBasis[0];   /* right.x */
    g_cameraBasis[1] = -g_cameraBasis[1];   /* right.y */
    g_cameraBasis[2] = -g_cameraBasis[2];   /* right.z */
    g_cameraBasis[3] = -g_cameraBasis[3];   /* up.x */
    g_cameraBasis[4] = -g_cameraBasis[4];   /* up.y */
    g_cameraBasis[5] = -g_cameraBasis[5];   /* up.z */
}

/* ========================================================================
 * 0x401E10 -- UpdateRaceCameraTransitionState
 *
 * Camera transition state machine during race start fly-in.
 * Divides transition timer by 0x2800 to get HUD indicator level,
 * then selects camera preset based on level.
 *
 * [CONFIRMED @ 0x00401E10 UpdateRaceCameraTransitionState; L5 sweep 2026-05-21]
 *   Decompile matches branch-by-branch:
 *     - Early dispatch: alive-check -> UpdateCameraTransitionHudIndicator
 *     - g_uiInputFreezeOverlayActive_PROVISIONAL (port: g_camTransitionGate)
 *     - Transition-complete branch: lookback selects 0 vs 0x800 yaw offset
 *     - level = g_cameraTransitionActive / 0x2800
 *     - level==0: preset 0xD, orbit_radius -= __ftol(delta*3584.0)
 *     - level==1: preset 0xC, orbit_angle += __ftol(delta*1024.0)
 *     - level==2: preset 0xB, orbit_angle += __ftol(delta*1024.0)
 *     - else:     preset 10,  orbit_angle += __ftol(delta*256.0), force_reload=1
 *     - LoadCameraPresetForView(actor+0x208, force_reload, view, 0) + yaw=0
 *     - store_prev: g_cameraPrevPresetId[v] = g_raceCameraPresetId[v]
 *   Port intentionally uses frame_delta=1.0 (orig has no subTickFraction
 *   factor; was a port-side stale interpolation that caused fps-dependent
 *   orbit speed). The const lifts {3584.0, 1024.0, 256.0} match orig FILD
 *   constants.
 * ======================================================================== */

void UpdateRaceCameraTransitionState(int actor, int view)
{
    int v = view;
    int actor_idx = *(unsigned char *)(actor + 0x375);

    /* [#11 (a)/(b) FIX 2026-06-16 — slot 8/9 fly-in OOB] g_actorAliveTable is a
     * flat per-slot byte table sized [TD5_MAX_TOTAL_ACTORS] (32). The original
     * binary read the alive flag with a 4-byte stride because it addressed a
     * per-actor *record* array (DAT-bank addressing); the port collapsed that to
     * one byte per slot but kept the `* 4`, so the index runs 4x too far:
     *   slot 0->0, slot 7->28 (last in-bounds), slot 8->32 (OOB), slot 9->36 (OOB).
     * Viewport 8 ("Player 9") follows racer slot 8 (InitRace identity map) whose
     * +0x375 slot_index is 8, so this read overflows into the adjacent globals
     * (g_lookLeftRight[]). When that garbage byte is non-zero the fly-in takes the
     * "alive -> HUD indicator only" early-return and SKIPS the cinematic countdown
     * camera move + spring advance for that pane — exactly the missing slot-9
     * countdown pan, and an undefined read that also feeds the slot-8/9 jitter.
     * Index by slot directly (stride 1) — the faithful flat-table lookup — and
     * bound-guard so no actor slot can ever index past the table again. The flag
     * is never set in the port (kept as a static 0 table), so for the in-bounds
     * slots this is behaviour-identical; it only removes the OOB for slot >= 8.
     * Gate: TD5RE_FIX_FLYIN_BOUND=0 restores the (buggy) `* 4` for A/B. */
    static int s_flyin_bound_fix = -1;
    if (s_flyin_bound_fix < 0) {
        s_flyin_bound_fix = td5_env_flag_on("TD5RE_FIX_FLYIN_BOUND");   /* default ON */
    }
    int alive_idx = s_flyin_bound_fix ? actor_idx : (actor_idx * 4);

    /* If actor is alive, delegate to HUD indicator update */
    if (alive_idx >= 0 && alive_idx < TD5_MAX_TOTAL_ACTORS &&
        g_actorAliveTable[alive_idx] != 0) {
        UpdateCameraTransitionHudIndicator(v, (unsigned int)actor_idx);
        return;
    }

    if (g_camTransitionGate != 0) return;

    if (g_cameraTransitionActive < 1) {
        /* Transition complete */
        if (g_lookLeftRight[v] == 0) {
            g_camYawOffset[v] = 0;
        } else {
            g_camYawOffset[v] = 0x800;
        }
        return;
    }

    /* Compute level from timer: timer / 0x2800 gives 4 levels (0,1,2,3+) */
    {
        int level = g_cameraTransitionActive / 0x2800;
        int new_preset;
        int force_reload;
        /* In the original binary, this function ran once per sim tick via
           the callback system at 0x429790, so the effective delta was 1.0
           per invocation.  Using g_subTickFraction here caused frame-rate-
           dependent orbit speed (too fast at high fps). */
        float frame_delta = 1.0f;

        if (level == 0) {
            /* Level 0: preset 13, orbit radius shrinks by frameDelta * 3584.0 */
            new_preset = 0x0D;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitRadiusScale[v] -= (float)(int)(frame_delta * 3584.0f);

            if (g_cameraPrevPresetId[v] == 0x0D) goto store_prev;
            force_reload = 0;
        } else if (level == 1) {
            /* Level 1: preset 12, orbit angle advances by frameDelta * 1024.0 */
            new_preset = 0x0C;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += (int)(frame_delta * 1024.0f);
            if (g_cameraPrevPresetId[v] == 0x0C) goto store_prev;
            force_reload = 0;
        } else if (level == 2) {
            /* Level 2: preset 11, orbit angle advances by frameDelta * 1024.0 */
            new_preset = 0x0B;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += (int)(frame_delta * 1024.0f);
            if (g_cameraPrevPresetId[v] == 0x0B) goto store_prev;
            force_reload = 0;
        } else {
            /* Level 3+: preset 10, orbit angle advances by frameDelta * 256.0 */
            new_preset = 10;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += (int)(frame_delta * 256.0f);
            if (g_cameraPrevPresetId[v] == 10) goto store_prev;
            force_reload = 1;
        }

        LoadCameraPresetForView(actor + 0x208, force_reload, v, 0);
        g_camYawOffset[v] = 0;

    store_prev:
        g_cameraPrevPresetId[v] = g_raceCameraPresetId[v];
        {
            static int s_flyin_log_ctr = 0;
            if ((s_flyin_log_ctr++ % 30) == 0)
                TD5_LOG_I(LOG_TAG, "fly-in: level=%d preset=%d orbitFP=0x%X radius=%.0f delta=%.3f",
                          level, g_raceCameraPresetId[v], g_camOrbitAngleFP[v],
                          g_camOrbitRadiusScale[v], frame_delta);
        }
    }
}

/* ========================================================================
 * 0x402000 -- ResetRaceCameraSelectionState
 *
 * Restores or resets camera preset selection for both views.
 *
 * [CONFIRMED @ 0x00402000 ResetRaceCameraSelectionState; L5 promotion sweep
 *  audit 2026-05-18] -- Byte-faithful port (NOT ARCH-DIVERGENCE).
 *  Two-branch dispatch matches orig exactly:
 *    param == 0: restore from packed save (DAT_00482f48 / +0x82fd4):
 *                  preset[0] = packed[0] & 0x7F
 *                  mode[0]   = (packed[0] & 0xFF) >> 7
 *                  preset[1] = packed[1] & 0x7F
 *                  mode[1]   = packed[1] >> 7
 *    param != 0: zero all four fields.
 *  Then two LoadCameraPresetForView calls with identical view indices
 *  (0 and 1) and identical actor base (slot * 0x388 + 0x4ab310 in orig
 *  / g_actorBaseAddr + g_actorSlotForView[v] * 0x388 in port).
 *  Note: orig's only caller is UpdateRaceCameraTransitionTimer at the
 *  countdown timer-zero crossing; that call is currently NOT wired in
 *  the port -- see todo_countdown_reset_camera_preset_call_2026-05-18.md.
 *  This function itself is byte-faithful and ready for the wiring.
 * ======================================================================== */

void ResetRaceCameraSelectionState(int clear_or_restore)
{
    if (clear_or_restore == 0) {
        /* Restore from packed save bytes */
        unsigned short packed = *(unsigned short *)g_camPackedSave;
        g_raceCameraPresetId[0]   = packed & 0x7F;
        g_raceCameraPresetMode[0] = (packed & 0xFF) >> 7;

        unsigned char byte1 = g_camPackedSave[1];
        g_raceCameraPresetId[1]   = byte1 & 0x7F;
        g_raceCameraPresetMode[1] = (unsigned int)(byte1 >> 7);
    } else {
        /* Reset to defaults */
        g_raceCameraPresetId[1]   = 0;
        g_raceCameraPresetId[0]   = 0;
        g_raceCameraPresetMode[1] = 0;
        g_raceCameraPresetMode[0] = 0;
    }

    /* Reload presets for both views */
    LoadCameraPresetForView(
        (int)((char *)&g_actorBaseAddr + g_actorSlotForView[0] * 0x388),
        0, 0, 0);
    LoadCameraPresetForView(
        (int)((char *)&g_actorBaseAddr + g_actorSlotForView[1] * 0x388),
        0, 1, 0);
}

/* ========================================================================
 * 0x4020B0 -- InitializeTracksideCameraProfiles
 *
 * Counts valid profiles, seeds initial camera positions from track
 * geometry, and randomizes per-view timers.
 *
 * [CONFIRMED @ 0x004020B0 InitializeTracksideCameraProfiles; L5 sweep 2026-05-21]
 *   Byte-faithful match:
 *     - count loop scans `gTracksideCameraProfiles[i*8] != -1` with stride 8
 *     - anchor strip index/offset read from profile[3] and profile[4]
 *     - X/Z anchor = (vtx[(strip[+4] + ofs)*6 {+0|+4}] + strip[+0xC|+0x14]) * 0x100
 *     - Y-bias = profile[5]
 *     - Copy view 0 -> view 1 for span/ofs/anchor/Y/index
 *     - Two _rand() % 10000 + 10000 calls for per-view timers
 *     - Spline adv rate seeded to 8, spline params zeroed
 *     - Circuit/<2 profiles override forces behavior type 4 (orbit)
 *   Note: port's `while (... != -1) count++` and orig's `do++; check;` produce
 *   identical final count.
 * ======================================================================== */

void InitializeTracksideCameraProfiles(void)
{
    int count = 0;
    int span_addr, vtx_addr;
    int anchor_span, span_ofs;

    g_cameraProfileIndex[0] = 0;
    g_cameraProfileIndex[1] = 0;
    g_cameraLastProjScale[0] = -1;
    g_cameraLastProjScale[1] = -1;

    /* Defensive: no profile table bound for this track/direction (e.g. a track
     * without authored trackside cameras). Leave count 0 so the replay camera
     * dispatch falls back to chase instead of dereferencing NULL. */
    if (g_tracksideCameraProfiles == NULL) {
        g_tracksideCameraProfileCount = 0;
        return;
    }

    /* Count valid profiles (scan until -1 sentinel) */
    while (g_tracksideCameraProfiles[count * 8] != -1) {
        count++;
    }
    g_tracksideCameraProfileCount = count;

    /* Seed from first profile's anchor data */
    anchor_span = (unsigned short)g_tracksideCameraProfiles[3];
    span_ofs    = (unsigned short)g_tracksideCameraProfiles[4];

    g_camAnchorSpan[0] = anchor_span;
    g_camTrackSpanOfs[0] = span_ofs;

    /* Compute camera world position from track geometry */
    span_addr = g_spanTable + anchor_span * 0x18;
    vtx_addr  = g_vertexTable;

    g_camAnchorX[0] = ((int)*(short *)(vtx_addr +
        ((unsigned short)*(short *)(span_addr + 4) + span_ofs) * 6) +
        *(int *)(span_addr + 0x0C)) * 0x100;

    g_camAnchorZ[0] = ((int)*(short *)(vtx_addr + 4 +
        ((unsigned short)*(short *)(span_addr + 4) + span_ofs) * 6) +
        *(int *)(span_addr + 0x14)) * 0x100;

    g_camHeightParam[0] = (unsigned short)g_tracksideCameraProfiles[5];

    g_camBehaviorType[1] = 0;
    g_camBehaviorType[0] = 0;

    /* Copy to view 1 */
    g_camTrackSpanOfs[1] = g_camTrackSpanOfs[0];
    g_camAnchorSpan[1]   = g_camAnchorSpan[0];
    g_camAnchorX[1]      = g_camAnchorX[0];
    g_camHeightParam[1]  = g_camHeightParam[0];
    g_camAnchorZ[1]      = g_camAnchorZ[0];

    /* Randomize timers */
    g_tracksideTimer[0] = rand() % 10000 + 10000;

    g_camSplineAdvRate[0] = 8;
    g_camSplineAdvRate[1] = 8;
    g_camSplineParam[0] = 0;
    g_camSplineParam[1] = 0;

    g_tracksideTimer[1] = rand() % 10000 + 10000;

    /* If fewer than 2 profiles or circuit track: force orbit mode */
    if (g_tracksideCameraProfileCount < 2 || g_trackType != 0) {
        g_camBehaviorType[0] = 4;
        g_camBehaviorType[1] = 4;
    }
}

/* Bind the active per-track trackside-camera profile table. Mirrors the orig
 * LoadTrackRuntimeData @0x42fd4b: gTracksideCameraProfiles =
 * g_perTrackTracksideCameraProfilePtrs[track_pool_index - 1]. pool_index_1based
 * is the 1-based selector (orig world_x = gTrackPoolSpanCountTable[...]); an
 * out-of-range / unavailable index (0, or a reverse track marked -1, or an
 * unused 64 sentinel) clears the pointer so the replay camera falls back to
 * chase. */
void td5_camera_bind_trackside_profiles(int pool_index_1based)
{
    if (pool_index_1based >= 1 &&
        pool_index_1based <= TD5_PER_TRACK_CAMERA_PROFILE_COUNT) {
        /* cast away const: SelectTracksideCameraProfile / Initialize only READ. */
        g_tracksideCameraProfiles =
            (short *)s_per_track_camera_profiles[pool_index_1based - 1];
    } else {
        g_tracksideCameraProfiles = NULL;
    }
    g_tracksideCameraProfileCount = 0;  /* (re)counted by InitializeTracksideCameraProfiles */
}

/* True when the cinematic trackside replay camera has usable per-track profile
 * data (bound by td5_camera_bind_trackside_profiles + counted by
 * InitializeTracksideCameraProfiles). When false the replay path falls back to
 * the chase camera so the played-back car always stays on screen. */
int td5_camera_replay_trackside_ready(void)
{
    return (g_tracksideCameraProfiles != NULL) && (g_tracksideCameraProfileCount > 0);
}

/* ========================================================================
 * 0x402200 -- SelectTracksideCameraProfile
 *
 * Checks if the followed vehicle's track span falls within a different
 * profile's range. If so, initializes behavior-specific camera state.
 *
 * [CONFIRMED @ 0x00402200 SelectTracksideCameraProfile; L5 sweep 2026-05-21]
 *   Byte-faithful port. Verified:
 *     - profile range scan: prof[1] <= actor.span <= prof[2]
 *     - early-out when profile index unchanged
 *     - anchor X/Z recomputed identically (vtx + strip[+C/+14]) * 0x100
 *     - 0..10 switch dispatch matches orig:
 *         0: yaw=0, timer=20000
 *         1: yaw=0, offset=(0x200, 0xE2, 0xFB9C), depth_bias=0x1000
 *         2: yaw=0, offset=(0xFE00, 0xE2, 0xFB9C), depth_bias=0x1000
 *         3: yaw=0, depth_bias=0x1000
 *         4/9, 5/10: depth_bias=0x1000, height_param = prof[5]
 *         6: yaw=0, spline_param=0, timer=20000, adv=prof[7], nodes=prof[6]
 *         7/8: depth_bias=0x1000, offset=(0, 0xFF38, 0), mode=1
 *           (orig falls through into shared fall-through block; port
 *            replicates writes per-case but produces identical state)
 *     - prof stride = 0x10 (orig) = 8 shorts (port array) — same
 * ======================================================================== */

void SelectTracksideCameraProfile(int actor, int view)
{
    int v = view;
    int prev_profile = g_cameraProfileIndex[v];
    int i;
    short actor_span = *(short *)(actor + 0x80);

    /* Find matching profile by span range */
    for (i = 0; i < g_tracksideCameraProfileCount; i++) {
        short *prof = g_tracksideCameraProfiles + i * 8;
        int range_start = (unsigned short)prof[1];
        int range_end   = (unsigned short)prof[2];

        if (range_start <= (int)actor_span && (int)actor_span <= range_end) {
            g_cameraProfileIndex[v] = i;
            break;
        }
    }

    if (prev_profile == g_cameraProfileIndex[v]) return;

    /* Profile changed -- initialize from new profile */
    {
        short *prof = g_tracksideCameraProfiles + g_cameraProfileIndex[v] * 8;
        unsigned short anchor = (unsigned short)prof[3];
        unsigned int ofs      = (unsigned int)(unsigned short)prof[4];
        int span_addr, vtx_addr;

        g_camAnchorSpan[v] = (unsigned int)anchor;

        span_addr = g_spanTable + (unsigned int)anchor * 0x18;
        vtx_addr  = g_vertexTable;

        g_camTrackSpanOfs[v] = (int)ofs;

        g_camAnchorX[v] = ((int)*(short *)(vtx_addr +
            ((unsigned short)*(short *)(span_addr + 4) + ofs) * 6) +
            *(int *)(span_addr + 0x0C)) * 0x100;

        g_camAnchorZ[v] = ((int)*(short *)(vtx_addr + 4 +
            ((unsigned short)*(short *)(span_addr + 4) + ofs) * 6) +
            *(int *)(span_addr + 0x14)) * 0x100;

        g_camHeightParam[v] = (unsigned int)(unsigned short)prof[5];
        g_raceCameraPresetMode[v] = 0;

        /* Switch on behavior type */
        {
            unsigned int btype = (unsigned int)(unsigned short)prof[0];
            g_camBehaviorType[v] = (int)btype;

            switch (btype) {
            case 0:
                g_camYawOffset[v] = 0;
                g_tracksideTimer[v] = 20000;
                break;
            case 1:
                g_camYawOffset[v] = 0;
                g_camOffsetVec[v][0] = 0x200;
                g_camOffsetVec[v][1] = 0xE2;
                g_depthFovFactor = 0x1000;
                g_camOffsetVec[v][2] = (short)0xFB9C;
                break;
            case 2:
                g_camYawOffset[v] = 0;
                g_camOffsetVec[v][0] = (short)0xFE00;
                g_camOffsetVec[v][1] = 0xE2;
                g_depthFovFactor = 0x1000;
                g_camOffsetVec[v][2] = (short)0xFB9C;
                break;
            case 3:
                g_camYawOffset[v] = 0;
                g_depthFovFactor = 0x1000;
                break;
            case 4:
            case 9:
                g_depthFovFactor = 0x1000;
                g_camHeightParam[v] = (unsigned int)(unsigned short)prof[5];
                break;
            case 5:
            case 10:
                g_depthFovFactor = 0x1000;
                g_camHeightParam[v] = (unsigned int)(unsigned short)prof[5];
                break;
            case 6:
                g_camYawOffset[v] = 0;
                g_camSplineParam[v] = 0;
                g_tracksideTimer[v] = 20000;
                g_camSplineAdvRate[v] = (int)(unsigned short)prof[7];
                g_camSplineNodeCount[v] = (int)(unsigned short)prof[6];
                break;
            case 7:
                g_depthFovFactor = 0x1000;
                g_camYawOffset[v] = 0;
                g_camOffsetVec[v][0] = 0;
                g_raceCameraPresetMode[v] = 1;
                g_camOffsetVec[v][1] = (short)0xFF38;
                g_camOffsetVec[v][2] = 0;
                break;
            case 8:
                g_depthFovFactor = 0x1000;
                g_camOffsetVec[v][0] = 0;
                g_raceCameraPresetMode[v] = 1;
                g_camOffsetVec[v][1] = (short)0xFF38;
                g_camOffsetVec[v][2] = 0;
                break;
            default:
                break;
            }
        }
    }
}

/* ========================================================================
 * 0x402950 -- UpdateStaticTracksideCamera
 *
 * Fixed camera position from track geometry, with look-at toward
 * the followed vehicle.
 *
 * [CONFIRMED @ 0x00402950 UpdateStaticTracksideCamera; REG-3 verdict 2026-05-22]
 *   Byte-faithful Y baseline. Orig reads
 *     vtx[(strip[+4] + g_cameraProfileVertOffset[v]) * 6 + 2]
 *   from the per-profile g_cameraProfileVertOffset @ DAT_00482eb8. Cross-ref
 *   audit (mcp__ghidra__reference_to 0x00482eb8) shows ONLY two refs to that
 *   global — both READS at 0x004024dc (UpdateTracksideCamera case 0) and
 *   0x0040299a (this function). NO ORIG WRITER exists; the variable remains
 *   at .bss default 0 throughout the orig binary.
 *
 *   Port mirrors this exactly: g_camHeightSampleOfs[2] = {0} (line 70) and
 *   stays 0 — both orig and port sample vertex[strip_base + 0]. Earlier
 *   suspected-regression note (2026-05-21) was a false alarm based on the
 *   assumption that the variable was supposed to be written by
 *   SelectTracksideCameraProfile; in fact neither orig nor port writes it.
 * ======================================================================== */

void UpdateStaticTracksideCamera(int actor, int view)
{
    int v = view;
    int span_addr, vtx_addr;
    int target[3];

    span_addr = g_spanTable + g_camAnchorSpan[v] * 0x18;
    vtx_addr  = g_vertexTable + (unsigned short)*(short *)(span_addr + 4) * 6;

    /* Camera X and Z from pre-computed anchor */
    g_camWorldPos[v][0] = g_camAnchorX[v];

    /* Camera Y from terrain vertex + height parameter */
    g_camWorldPos[v][1] = ((int)*(short *)(vtx_addr + 2 +
        g_camHeightSampleOfs[v] * 6) +
        g_camHeightParam[v] + *(int *)(span_addr + 0x10)) * 0x100;

    g_camWorldPos[v][2] = g_camAnchorZ[v];

    /* Target = vehicle pos + velocity * subTickFraction */
    target[0] = *(int *)(actor + 0x1FC) +
                (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
    target[1] = *(int *)(actor + 0x200) +
                (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
    target[2] = *(int *)(actor + 0x204) +
                (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

    SetCameraWorldPosition(g_camWorldPos[v]);
    OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);
}

/* ========================================================================
 * 0x402A80 -- CacheVehicleCameraAngles
 *
 * Snapshots the vehicle's current orientation into per-view cache,
 * with 180-degree yaw offset for chase view.
 *
 * [CONFIRMED @ 0x00402A80 CacheVehicleCameraAngles; L5 sweep 2026-05-21]
 *   Byte-faithful: 3 short writes per view, identical offsets/signs
 *   (display_angle_roll+0x800, 0x800-display_angle_yaw, -display_angle_pitch)
 *   mapped to g_camCachedAngles[view][0..2]. Field offsets 0x208/0x20A/0x20C
 *   verified against Ghidra decomp.
 * ======================================================================== */

void CacheVehicleCameraAngles(int actor, int view)
{
    g_camCachedAngles[view][0] = *(short *)(actor + 0x208) + 0x800;
    g_camCachedAngles[view][1] = 0x800 - *(short *)(actor + 0x20A);
    g_camCachedAngles[view][2] = -*(short *)(actor + 0x20C);
}

/* ========================================================================
 * 0x402AD0 -- UpdateSplineTracksideCamera
 *
 * Evaluates a cubic spline camera path, adjusts projection scale
 * (FOV) based on distance to target.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4, suspected regression]
 *   Orig builds spline templates as inline stack array `local_5e[39]` indexed
 *   by `param_3 * 8`. Effective per-type template (as 4 pairs
 *   (span_delta, ofs_delta)):
 *     type 0: (-1, 0), (0, 0), (40, 0), (41, 0)
 *     type 1: (-1, 0), (0, 0), (40, 0), (41, 0)  [same template, distinct
 *             ofs delta of 0]  -- need careful re-read of local_5e[8..15]
 *     type 2 also reuses (-1, 0) start; types 3-5 advance into 0x28/0x29
 *     range with offset -4 (0xfffc) at specific slots.
 *
 *   Port table at td5_camera.c:163:
 *     type 0: { 0, 0, 0, -1, 0, 0, 0, -1 }
 *     type 1: { 0, 0, 40, 0, 41, 0, 0, -1 }
 *     ...
 *   Indexed as `tmpl[i*2]` (span_delta), `tmpl[i*2+1]` (ofs_delta).
 *
 *   Mismatch: orig type-0 pairs (-1,0),(0,0),(40,0),(41,0) vs port type-0
 *   (0,0),(0,-1),(0,0),(0,-1). Span/ofs roles swapped AND span values
 *   wrong (port flat-anchor vs orig forward-span sweep). Visual: spline
 *   camera path no longer covers the orig 40-41-span flyby arc.
 *
 *   Suggested fix: re-derive all 6 template rows from orig's local_5e
 *   stack pattern (see decomp at 0x00402AD0 lines 21..55). Note orig
 *   reads `psVar2[-1]` for span (off-by-one trick via local_60=0xffff
 *   sentinel sitting at local_5e[-1]).
 *
 *   Also note: orig terrain Y uses g_cameraTracksideAnchorYBias bias.
 *   Port mirrors this via g_camHeightParam (OK), but the underlying
 *   `g_camHeightSampleOfs` vs `g_cameraProfileVertOffset` issue from
 *   UpdateStaticTracksideCamera does NOT apply here (spline uses ofs
 *   delta direct, not VertOffset).
 * ======================================================================== */

void UpdateSplineTracksideCamera(int actor, int view, int spline_type)
{
    int v = view;
    const short *tmpl;
    int control_points[12];  /* 4 points x {X, Y, Z} */
    int i;
    int target[3];
    int dist_scaled, proj_scale;

    /* Advance spline parameter */
    if (g_camSplineParam[v] < 0xFFF - g_camSplineAdvRate[v]) {
        g_camSplineParam[v] += g_camSplineAdvRate[v];
    } else {
        g_camSplineParam[v] = 0xFFF;
    }

    /* Build 4 control points from spline template + track geometry */
    tmpl = s_splineTemplates[spline_type];

    for (i = 0; i < 4; i++) {
        int span_delta = (int)tmpl[i * 2];
        int ofs_delta  = (int)tmpl[i * 2 + 1];
        int span_idx   = span_delta + g_camAnchorSpan[v];
        int ofs        = ofs_delta + g_camTrackSpanOfs[v];
        int sa         = g_spanTable + span_idx * 0x18;
        int va         = g_vertexTable;
        int vidx       = (unsigned short)*(short *)(sa + 4) + ofs;

        control_points[i * 3 + 0] =
            ((int)*(short *)(va + vidx * 6) + *(int *)(sa + 0x0C)) * 0x100;
        control_points[i * 3 + 1] =
            ((int)*(short *)(va + vidx * 6 + 2) +
             *(int *)(sa + 0x10) + g_camHeightParam[v]) * 0x100;
        control_points[i * 3 + 2] =
            ((int)*(short *)(va + vidx * 6 + 4) + *(int *)(sa + 0x14)) * 0x100;
    }

    /* Build and evaluate cubic spline */
    BuildCubicSpline3D(g_camSplineState[v], (int)control_points);
    EvaluateCubicSpline3D(g_camWorldPos[v], g_camSplineState[v], g_camSplineParam[v]);

    /* Target = vehicle pos + velocity * subTickFraction */
    target[0] = *(int *)(actor + 0x1FC) +
                (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
    target[1] = *(int *)(actor + 0x200) +
                (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
    target[2] = *(int *)(actor + 0x204) +
                (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

    SetCameraWorldPosition(g_camWorldPos[v]);
    OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);

    /* Dynamic FOV adjustment based on distance */
    {
        /* __ftol of distance computation -- simplified from original */
        /* dist = distance between camera and target, scaled */
        float dx = (float)(target[0] - g_camWorldPos[v][0]);
        float dz = (float)(target[2] - g_camWorldPos[v][2]);
        float dy = (float)(target[1] - g_camWorldPos[v][1]);
        float dist_f = sqrtf(dx * dx + dy * dy + dz * dz);
        dist_scaled = (int)dist_f * 4;
    }

    g_cameraProjDist[v] = dist_scaled;

    proj_scale = g_tracksideTimer[v] * dist_scaled;
    proj_scale = (proj_scale + ((proj_scale >> 31) & 0xFFF)) >> 12;

    g_cameraProjScaleComp[v] = proj_scale;

    if (proj_scale < 0x1000) {
        g_cameraProjScaleComp[v] = 0x1000;
    } else if (proj_scale > 20000) {
        g_cameraProjScaleComp[v] = 20000;
    }

    g_depthFovFactor = g_cameraProjScaleComp[v];
    RecomputeTracksideProjectionScale();

    TD5_LOG_I(LOG_TAG,
              "trackside view %d: profile=%d anchor=%u behavior=%d",
              v, g_cameraProfileIndex[v],
              (unsigned int)g_camAnchorSpan[v], g_camBehaviorType[v]);
}

/* ========================================================================
 * 0x402480 -- UpdateTracksideCamera
 *
 * Master dispatcher for all 11 trackside camera behavior types.
 *
 * [L5-AUDIT 2026-05-21 — LEFT AT L4, mostly faithful but inherits two
 *  documented latent divergences]
 *   - case 0 terrain Y sampling: same `g_camHeightSampleOfs` vs
 *     `g_cameraProfileVertOffset` issue as UpdateStaticTracksideCamera.
 *   - case 1/2 offset transform: orig calls ConvertFloatVec3ToIntVec3
 *     @ 0x0042DB40 which __ftols + clamps to short range. Port calls
 *     TransformVector3ByBasis @ 0x0042DBD0 which keeps full int32 range.
 *     For very-near-zero or near-saturation offsets the two diverge by
 *     up to 1 LSB; for offsets within ±32767 (the normal case) the
 *     observable values match.
 *   - Dispatch structure (cases 0..10 + default), preset increments
 *     (+8/-8/+0/+0x800 for orbit variants), preset reload semantics,
 *     and trailing projection-scale update all match orig precisely.
 *   - case 6 uses g_camSplineNodeCount[v] (port name) vs
 *     g_cameraSplineTemplateIndex (orig name) — same field, different
 *     port-side name (verify alias is OK).
 * ======================================================================== */

void UpdateTracksideCamera(int actor, int view)
{
    int v = view;
    int btype = g_camBehaviorType[v];

    TD5_LOG_D(LOG_TAG,
              "trackside update view %d: profile=%d behavior=%d(%s)",
              view, g_cameraProfileIndex[v], btype, trackside_behavior_name(btype));

    switch (btype) {
    case 0: {
        /* Static with dynamic FOV */
        int span_addr = g_spanTable + g_camAnchorSpan[v] * 0x18;
        int vtx_base  = g_vertexTable + (unsigned short)*(short *)(span_addr + 4) * 6;
        int target[3];
        int dist_scaled, proj_scale;

        g_camWorldPos[v][0] = g_camAnchorX[v];
        g_camWorldPos[v][1] = ((int)*(short *)(vtx_base + 2 +
            g_camHeightSampleOfs[v] * 6) +
            g_camHeightParam[v] + *(int *)(span_addr + 0x10)) * 0x100;
        g_camWorldPos[v][2] = g_camAnchorZ[v];

        target[0] = *(int *)(actor + 0x1FC) +
                    (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
        target[1] = *(int *)(actor + 0x200) +
                    (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
        target[2] = *(int *)(actor + 0x204) +
                    (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

        SetCameraWorldPosition(g_camWorldPos[v]);
        OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);

        /* Dynamic FOV */
        {
            float dx = (float)(target[0] - g_camWorldPos[v][0]);
            float dy = (float)(target[1] - g_camWorldPos[v][1]);
            float dz = (float)(target[2] - g_camWorldPos[v][2]);
            dist_scaled = (int)sqrtf(dx*dx + dy*dy + dz*dz) * 4;
        }
        g_cameraProjDist[v] = dist_scaled;
        proj_scale = g_tracksideTimer[v] * dist_scaled;
        proj_scale = (proj_scale + ((proj_scale >> 31) & 0xFFF)) >> 12;
        g_cameraProjScaleComp[v] = proj_scale;
        if (proj_scale < 0x1000) g_cameraProjScaleComp[v] = 0x1000;
        else if (proj_scale > 20000) g_cameraProjScaleComp[v] = 20000;
        g_depthFovFactor = g_cameraProjScaleComp[v];
        RecomputeTracksideProjectionScale();
        break;
    }

    case 1:
    case 2: {
        /* Vehicle-relative with smoothed angles (same code as cases 1,2 in original) */
        short target_pitch = *(short *)(actor + 0x208) + 0x800;
        short target_yaw   = 0x800 - *(short *)(actor + 0x20A);
        short target_roll  = -*(short *)(actor + 0x20C);
        short *cached = g_camCachedAngles[v];
        short cam_angles[3];
        int *cam_world;
        short delta_p, delta_y, delta_r;

        {
            int w = td5_angle12_delta(target_pitch, cached[0]);
            delta_p = (short)(int)((float)w * cam_integ_step() + 0.5f);
        }
        cam_angles[0] = cached[0] + delta_p;

        {
            int w = td5_angle12_delta(target_yaw, cached[1]);
            delta_y = (short)(int)((float)w * cam_integ_step() + 0.5f);
        }
        cam_angles[1] = cached[1] + delta_y;

        {
            int w = td5_angle12_delta(target_roll, cached[2]);
            delta_r = (short)(int)((float)w * cam_integ_step() + 0.5f);
        }
        cam_angles[2] = cached[2] + delta_r;

        BuildCameraBasisFromAngles(cam_angles);

        cam_world = g_camWorldPos[v];
        /* [FIX 2026-05-24 OVERSIGHT: case_1_2_basis_transform; orig 0x00402480]
         * Orig case 1/2 calls ConvertFloatVec3ToIntVec3 @ 0x0042DB40 (__ftol
         * + (int)(short) clamp). Port previously called the unclamped
         * TransformVector3ByBasis variant. */
        ConvertFloatVec3ToIntVec3((float *)(actor + 0x120), &g_camOffsetVec[v][0], cam_world);

        cam_world[0] = cam_world[0] * 0x100 + *(int *)(actor + 0x1FC);
        cam_world[1] = *(int *)(actor + 0x200) + cam_world[1] * 0x100;
        cam_world[2] = cam_world[2] * 0x100 + *(int *)(actor + 0x204);

        cam_world[0] += (int)((float)*(int *)(actor + 0x1CC) * g_subTickFraction + 0.5f);
        cam_world[1] += (int)((float)*(int *)(actor + 0x1D0) * g_subTickFraction + 0.5f);
        cam_world[2] += (int)((float)*(int *)(actor + 0x1D4) * g_subTickFraction + 0.5f);

        SetCameraWorldPosition(cam_world);
        BuildCameraBasisFromAngles(cam_angles);

        g_camAnchorSpan[v] = (int)*(short *)(actor + 0x80);
        break;
    }

    case 3:
        UpdateStaticTracksideCamera(actor, v);
        break;

    case 4:
        g_camYawOffset[v] += 8;
        UpdateTracksideOrbitCamera(actor, 1, v);
        break;

    case 5:
        g_camYawOffset[v] -= 8;
        UpdateTracksideOrbitCamera(actor, 1, v);
        break;

    case 6:
        UpdateSplineTracksideCamera(actor, v, g_camSplineNodeCount[v]);
        break;

    case 7:
        UpdateVehicleRelativeCamera(actor, v);
        break;

    case 8:
        g_camYawOffset[v] = 0x800;
        UpdateVehicleRelativeCamera(actor, v);
        break;

    case 9:
        g_camYawOffset[v] = 0x800;
        UpdateTracksideOrbitCamera(actor, 1, v);
        break;

    case 10:
        g_camYawOffset[v] = 0;
        UpdateTracksideOrbitCamera(actor, 1, v);
        break;

    default:
        break;
    }

    /* Update projection scale if changed */
    if (g_cameraLastProjScale[v] != g_depthFovFactor) {
        RecomputeTracksideProjectionScale();
        g_cameraLastProjScale[v] = g_depthFovFactor;
    }
}

/* ========================================================================
 * 0x402E00 -- CycleRaceCameraPreset
 *
 * Advances the camera preset index by delta, wrapping modulo 7.
 * Returns the number of complete cycles (preset / 7).
 *
 * [CONFIRMED @ 0x00402E00 CycleRaceCameraPreset; L5 sweep 2026-05-21]
 *   Byte-faithful: read g_raceCameraPresetId[view], store (old+delta)%7,
 *   return (old+delta)/7. Matches Ghidra decomp exactly.
 * ======================================================================== */

int CycleRaceCameraPreset(int view, int delta)
{
    int old = g_raceCameraPresetId[view];
    int new_val = old + delta;
    g_raceCameraPresetId[view] = new_val % 7;
    return new_val / 7;
}

/* ========================================================================
 * High-level module API wrappers
 * ======================================================================== */

static int s_active_preset;

static const char *camera_mode_name_for_view(int view, int has_actor)
{
    if (!has_actor) return "debug";
    if (g_replay_mode) return "trackside";

    switch (g_raceCameraPresetMode[view]) {
    case 5:
    case 6:
        return "bumper";
    case 1:
        return "trackside";
    default:
        return "chase";
    }
}

static const char *trackside_behavior_name(int btype)
{
    switch (btype) {
    case 0: return "static_fov";
    case 1: return "offset_left";
    case 2: return "offset_right";
    case 3: return "static_orbit";
    case 4: return "anchor_height";
    case 5: return "anchor_pan";
    case 6: return "spline";
    case 7: return "vehicle_relative";
    case 8: return "vehicle_relative_alt";
    case 9: return "anchor_height_alt";
    case 10: return "anchor_pan_alt";
    default: return "unknown";
    }
}

static void sample_span_center_world(int span_index, int *out_pos)
{
    TD5_StripSpan *sp;
    TD5_StripVertex *vl0;
    TD5_StripVertex *vl1;
    TD5_StripVertex *vr0;
    TD5_StripVertex *vr1;
    int span_count = td5_track_get_span_count();

    if (!out_pos)
        return;
    out_pos[0] = 0;
    out_pos[1] = 0;
    out_pos[2] = 0;

    if (span_count <= 0)
        return;

    if (span_index < 0)
        span_index = 0;
    if (span_index >= span_count)
        span_index %= span_count;

    sp = td5_track_get_span(span_index);
    if (!sp)
        return;

    vl0 = td5_track_get_vertex(sp->left_vertex_index);
    vl1 = td5_track_get_vertex(sp->left_vertex_index + 1);
    vr0 = td5_track_get_vertex(sp->right_vertex_index);
    vr1 = td5_track_get_vertex(sp->right_vertex_index + 1);
    if (!vl0 || !vl1 || !vr0 || !vr1)
        return;

    out_pos[0] = sp->origin_x + (vl0->x + vl1->x + vr0->x + vr1->x) / 4;
    out_pos[1] = sp->origin_y + (vl0->y + vl1->y + vr0->y + vr1->y) / 4;
    out_pos[2] = sp->origin_z + (vl0->z + vl1->z + vr0->z + vr1->z) / 4;
}

static void update_debug_track_camera(void)
{
    int span_count = td5_track_get_span_count();
    int focus[3];
    int look[3];
    int cam[3];
    int next_index;
    int dir_x;
    int dir_z;
    float len;
    float inv_len;
    float side_x;
    float side_z;
    float phase;

    if (span_count <= 0)
        return;

    sample_span_center_world((g_td5.simulation_tick_counter / 2) % span_count, focus);
    next_index = ((g_td5.simulation_tick_counter / 2) + 12) % span_count;
    sample_span_center_world(next_index, look);

    dir_x = look[0] - focus[0];
    dir_z = look[2] - focus[2];
    len = sqrtf((float)(dir_x * dir_x + dir_z * dir_z));
    if (len < 1.0f) {
        dir_x = 0;
        dir_z = 4096;
        len = 4096.0f;
    }

    inv_len = 1.0f / len;
    side_x = -(float)dir_z * inv_len;
    side_z = (float)dir_x * inv_len;
    phase = (float)g_td5.simulation_tick_counter * 0.03f;

    cam[0] = focus[0] - (int)((float)dir_x * inv_len * 768.0f) + (int)(side_x * 256.0f * sinf(phase));
    cam[1] = focus[1] + 256 + (int)(sinf(phase * 0.7f) * 64.0f);
    cam[2] = focus[2] - (int)((float)dir_z * inv_len * 768.0f) + (int)(side_z * 256.0f * sinf(phase));

    g_depthFovFactor = 0x1000;
    SetCameraWorldPosition(cam);
    OrientCameraTowardTarget(look, 0x400);
}
static unsigned int s_debug_camera_frame[TD5_MAX_VIEWPORTS];

static int resolve_view_actor_ptr(int view, int *out_actor)
{
    uintptr_t actor_base;
    int slot;

    if (!out_actor || view < 0 || view >= 2)
        return 0;

    slot = g_actorSlotForView[view];
    if (slot < 0 || slot >= g_td5.total_actor_count)
        return 0;

    if (g_actorBaseAddr == 0)
        return 0;

    actor_base = (uintptr_t)(uint32_t)g_actorBaseAddr;
    if (actor_base < 0x10000u)
        return 0;

    *out_actor = (int)(actor_base + (uintptr_t)slot * 0x388u);
    return 1;
}

static void update_debug_race_camera(int view)
{
    int v = view;  /* [PORT: N-way] per-view camera state (was view & 1) */

    update_debug_track_camera();

    g_camWorldPos[v][0] = (int)(g_cameraPos[0] * 256.0f);
    g_camWorldPos[v][1] = (int)(g_cameraPos[1] * 256.0f);
    g_camWorldPos[v][2] = (int)(g_cameraPos[2] * 256.0f);
    g_cameraLastProjScale[v] = g_depthFovFactor;
    g_cameraProjScaleComp[v] = g_depthFovFactor;
    g_cameraProjDist[v] = 0x1000;
}

int td5_camera_init(void)
{
    s_active_preset = 0;
    memset(s_debug_camera_frame, 0, sizeof(s_debug_camera_frame));
    TD5_LOG_I(LOG_TAG, "camera init: active_mode=%s preset=%d",
              camera_mode_name_for_view(0, 0), s_active_preset);
    /* [TASK 22 / TASK 19] one-time knob report. */
    TD5_LOG_I(LOG_TAG,
              "camera knobs: GROUND_CAM_FIX=%d (lift=%d FP) CAM_WALL_AVOID=%d",
              td5_camera_ground_cam_fix(), td5_camera_ground_cam_lift(),
              td5_camera_wall_avoid());
    return 1;
}

void td5_camera_shutdown(void) {}

/* Shared viewport actor lookup used by both camera-tick phases. */
static TD5_Actor *camera_actor_for_view(int v)
{
    TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[v]);
    if (!actor && g_actor_table_base) {
        int slot = g_actorSlotForView[v];
        int total = td5_game_get_total_actor_count();
        if (slot >= 0 && slot < total)
            actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
    }
    return actor;
}

/* Phase 1: snapshot vehicle orientation BEFORE physics integrates this tick.
   Matches CacheVehicleCameraAngles(primary,0)+CacheVehicleCameraAngles(secondary,1)
   at the top of the sim-tick loop inside RunRaceFrame (0x0042B580).
   The cached angles become the "previous-frame" state that
   UpdateVehicleRelativeCamera / chase-modes 1,2 smooth FROM this tick. */
void td5_camera_cache_angles(void)
{
    if (td5_game_get_total_actor_count() <= 0) return;
    int view_count = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
    for (int v = 0; v < view_count; v++) {
        TD5_Actor *actor = camera_actor_for_view(v);
        if (!actor) continue;
        CacheVehicleCameraAngles((int)actor, v);
    }
}

/* Phase 2: run the chase camera AFTER physics has updated actor pose.
   Matches UpdateChaseCamera(actor,1,view) near the tail of RunRaceFrame's
   sim-tick loop (0x0042B580, after UpdateRaceActors/ResolveVehicleContacts/
   UpdateRaceOrder). Reading post-physics actor angles against the
   pre-physics cached angles is what makes the orbit smoothing and the
   shortest-path angle lerp converge instead of snapping. */
void td5_camera_update_chase_all(void)
{
    if (td5_game_get_total_actor_count() <= 0) {
        update_debug_track_camera();
        return;
    }
    int view_count = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
    for (int v = 0; v < view_count; v++) {
        TD5_Actor *actor = camera_actor_for_view(v);
        if (!actor) continue;
        /* Orig RunRaceFrame sim-tick loop (0x0042b9c9) gates the chase cam on
         * gRaceCameraPresetMode[v] == 0. In bumper/in-car mode (mode != 0) the
         * chase orbit math must NOT run, or it overwrites the heading-locked
         * basis that UpdateVehicleRelativeCamera builds in the per-frame
         * dispatch — which is why the in-car view didn't rotate with the car.
         *
         * Gate also on !paused: during the pre-race fly-in/countdown the saved
         * camera mode (g_camPackedSave, loaded at race init) may be the bumper
         * preset (mode 1), but the original forces the orbit/fly-in camera then
         * (RunRaceFrame's "mode==0 OR flyInFlag>0" clause). So while paused,
         * always run the chase/orbit path regardless of mode; only suppress it
         * for the in-car cam once the race is actually running. */
        if (!g_td5.paused && g_raceCameraPresetMode[v] != 0)
            continue;

        cam_finish_orbit_step(v, actor);   /* [CAR DAMAGE] end-of-race damage orbit (legacy path) */
        UpdateChaseCamera((int)actor, 1, v);
    }
}

/* Legacy single-call entry point: still used by non-race paths (frontend
   transitions, etc.). In-race code must call cache_angles + update_chase_all
   around the physics tick instead. */
void td5_camera_tick(void)
{
    if (td5_game_get_total_actor_count() <= 0) {
        update_debug_track_camera();
        return;
    }
    td5_camera_cache_angles();
    td5_camera_update_chase_all();
}

void td5_camera_update_chase(TD5_Actor *a, int p, int vi)
{
    UpdateChaseCamera((int)a, p, vi);
}

void td5_camera_set_preset(int pi)
{
    s_active_preset = pi;
    memset(s_debug_camera_frame, 0, sizeof(s_debug_camera_frame));
    /* [PORT: N-way] reset preset / fly-in state for EVERY viewport (was 0/1). */
    for (int rv = 0; rv < TD5_MAX_VIEWPORTS; rv++) {
        s_flyin_preset_reloaded[rv] = 0;
        g_raceCameraPresetId[rv]    = 0;
        g_raceCameraPresetMode[rv]  = 0;
    }

    /* Seed per-view chase-camera state from the spawned actors so the first
       UpdateChaseCamera frame starts with correct radius/height/orbit values
       rather than stale or zero-initialized garbage.  actor+0x208 is the
       display_angles base used by LoadCameraPresetForView (+2 = yaw). */
    if (g_actor_table_base) {
        int views = (g_td5.viewport_count > 0) ? g_td5.viewport_count : 1;
        if (views > TD5_MAX_VIEWPORTS) views = TD5_MAX_VIEWPORTS;
        for (int v = 0; v < views; v++) {
            TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[v]);

            if (!actor) {
                int slot = g_actorSlotForView[v];
                int total = td5_game_get_total_actor_count();
                if (slot >= 0 && slot < total) {
                    actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
                }
            }

            if (actor) {
                LoadCameraPresetForView((int)((uint8_t *)actor + 0x208), 1, v, 0);
            }
        }
    }
}

void td5_camera_set_rear_view(int view, int active)
{
    /* Rear view = 180° yaw offset (0x800 in 4096-unit circle).
     * Only applied in chase mode (presetMode 0); bumper/trackside
     * paths set their own g_camYawOffset. */
    if (g_raceCameraPresetMode[view] == 0) {
        g_camYawOffset[view] = active ? 0x800 : 0;
    }
}

void td5_camera_update_trackside(TD5_Actor *a, int vi)
{
    UpdateTracksideCamera((int)a, vi);
}

void td5_camera_update_transition_state(int p, int vi)
{
    TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[vi]);
    int v = vi;  /* [PORT: N-way] per-view camera state (was vi & 1) */

    if (!actor && g_actor_table_base) {
        int slot = g_actorSlotForView[vi];
        int total = td5_game_get_total_actor_count();
        if (slot >= 0 && slot < total) {
            actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        }
    }

    s_debug_camera_frame[v]++;

    if (!actor) {
        /* No actor data yet — fall back to debug camera */
        TD5_LOG_I(LOG_TAG, "transition view %d: path=debug preset=%d", vi, p);
        update_debug_race_camera(vi);
        TD5_LOG_D(LOG_TAG,
                  "camera update view %d: pos=(%.2f, %.2f, %.2f) forward=(%.3f, %.3f, %.3f)",
                  vi, g_cameraPos[0], g_cameraPos[1], g_cameraPos[2],
                  g_cameraBasis[6], g_cameraBasis[7], g_cameraBasis[8]);
        return;
    }

    /* One-shot: as soon as the race starts (g_td5.paused → 0 at fly-in level
     * 0 per 630d797), restore the chase camera's spring targets (orbit
     * radius scale and height target) from preset 0 so the radius spring
     * converges to the chase distance (~600 wu) instead of preset 13's
     * fly-in pull-back distance (~3800 wu) for the remaining 40 sub-ticks
     * of the visible "1" countdown indicator.
     *
     * Pre-630d797 the one-shot fired at g_cameraTransitionActive<=0, which
     * coincided with the paused flip. Post-630d797 paused flips ~40 sub-ticks
     * earlier (at level==0 entry) while the timer keeps shrinking; the
     * fly-in level-0 path was pulling g_camOrbitRadiusScale toward 972800
     * during all 40 racing sub-ticks, leaving the chase camera 6.3× too far
     * back (visible as "camera not following / catching up slowly" right
     * after GO).
     *
     * DON'T use LoadCameraPresetForView with force_reload — that resets
     * orbit angle, current radius, and smoothed height simultaneously,
     * causing a jarring visual jump. Instead, only update the spring
     * TARGETS and let the existing spring smooth the transition. */
    if (!s_flyin_preset_reloaded[v] && !g_td5.paused) {
        s_flyin_preset_reloaded[v] = 1;
        g_raceCameraPresetId[v] = 0;
        g_raceCameraPresetMode[v] = 0;
        /* Restore spring targets from preset 0 (far chase) */
        TD5_CameraPreset *p = &g_cameraPresets[0];
        g_camOrbitRadiusScale[v] = (float)(int)p->orbit_radius_raw * g_const256;
        g_camTargetHeight[v]     = (float)(int)p->height_target_raw * g_const256;
        g_camElevationAngleFP[v] = (int)p->elevation_angle << 8;
        TD5_LOG_I(LOG_TAG, "race start view %d: restored chase targets (radius=%.0f height=%.0f, transitionActive=0x%X)",
                  v, g_camOrbitRadiusScale[v], g_camTargetHeight[v], g_cameraTransitionActive);
    }

    /* During race-start countdown, run the transition state machine (0x401E10).
     * Once the spring-reset one-shot above has fired (paused=0), skip the
     * fly-in preset machine so it doesn't overwrite our preset-0 targets
     * with preset-13 fly-in values during the remaining level=0 sub-ticks.
     * The HUD countdown indicator is independently updated by
     * tick_race_countdown() in td5_game.c, so skipping here is purely a
     * camera-state concern. */
    if (g_cameraTransitionActive > 0 && !s_flyin_preset_reloaded[v]) {
        UpdateRaceCameraTransitionState((int)actor, vi);
        /* [PORT: N-way split] Re-pin THIS view's fly-in camera too. Like the
         * chase path below, td5_camera_update_chase_all() leaves only the LAST
         * view's camera in the active g_cameraPos/g_cameraBasis, so during the
         * race-start countdown every pane would otherwise render from the last
         * view's player. (UpdateRaceCameraTransitionState only advances the
         * per-view fly-in state; it does not apply the camera.) */
        td5_camera_finalize_chase_pos(actor, vi);
        return;
    }

    /* Route to appropriate camera based on mode.
     *
     * Replay uses the cinematic trackside cameras (orig RunRaceFrame @0x42BA70:
     * when g_inputPlaybackActive!=0 it runs SelectTracksideCameraProfile +
     * UpdateTracksideCamera instead of chase). This requires the per-track
     * profile table to have been loaded (td5_camera_bind_trackside_profiles) and
     * InitializeTracksideCameraProfiles to have set a non-zero profile count at
     * race init. If neither holds (no profile data for this track/direction —
     * including reverse tracks, whose forward-authored span ranges never match
     * the car, the original's own "shows nothing" case), fall back to the chase
     * camera so the replay is always VISIBLE. [Documented deviation: the original
     * shows nothing on reverse-track replay; the port shows chase instead.] */
    if (g_replay_mode && td5_camera_replay_trackside_ready()) {
        TD5_LOG_D(LOG_TAG, "transition view %d: path=trackside(replay) actor_slot=%d",
                  vi, g_actorSlotForView[vi]);
        SelectTracksideCameraProfile((int)actor, vi);
        UpdateTracksideCamera((int)actor, vi);

        /* [ITEM #18 camera] Legacy per-frame path: the trackside eye is in
         * g_camWorldPos[vi] and the basis was already built from it. Raise the
         * eye above the road surface if a look-at trackside cam (static / spline
         * / orbit, behaviours 0/3/4/5/6/9/10) dipped under it, then re-pin the
         * position + re-orient from the raised eye toward the SAME look-at the
         * trackside cases use (car pos + velocity*subTick). The look-at target
         * is reconstructed identically — not moved. Vehicle-relative cams
         * (1/2/7/8) follow the car, so they are skipped. */
        {
            int bt = g_camBehaviorType[vi];
            int is_lookat = (bt == 0 || bt == 3 || bt == 4 || bt == 5 ||
                             bt == 6 || bt == 9 || bt == 10);
            if (is_lookat &&
                td5_camera_replay_eye_floor_clamp((uintptr_t)actor, vi,
                                                  g_camWorldPos[vi])) {
                uintptr_t ap = (uintptr_t)actor;
                int car_target[3];
                car_target[0] = *(int *)(ap + 0x1FC) +
                    (int)((float)*(int *)(ap + 0x1CC) * g_subTickFraction + 0.5f);
                car_target[1] = *(int *)(ap + 0x200) +
                    (int)((float)*(int *)(ap + 0x1D0) * g_subTickFraction + 0.5f);
                car_target[2] = *(int *)(ap + 0x204) +
                    (int)((float)*(int *)(ap + 0x1D4) * g_subTickFraction + 0.5f);
                SetCameraWorldPosition(g_camWorldPos[vi]);
                OrientCameraTowardTarget(car_target, g_tracksideYawOffset[vi]);
            }
        }
    } else if (g_replay_mode) {
        /* Replay fallback (no trackside profiles): behave like the chase/bumper
         * path below so the played-back car stays on screen. */
        int mode = g_raceCameraPresetMode[v];
        if (mode != 0 && !g_td5.paused) {
            UpdateVehicleRelativeCamera((int)actor, vi);
        }
        /* Chase (mode 0) updated per-sim-tick in td5_camera_update_chase_all(). */
    } else {
        /* Orig RunRaceFrame per-view dispatch (0x0042bca0): runs the orbit/chase
         * cam when gRaceCameraPresetMode[view] == 0 (or the race-start fly-in
         * flag is set, which the port already handles via the
         * g_cameraTransitionActive early-return above), and the in-car/bumper
         * cam otherwise (mode != 0).
         *
         * g_raceCameraPresetMode holds the preset MODE (0 = chase, 1 = bumper),
         * NOT the preset ID. The old `preset == 6 || preset == 5` test compared
         * the mode against preset IDs, so it never fired —
         * UpdateVehicleRelativeCamera never ran and the bumper view kept the
         * (per-sim-tick) chase orientation instead of rotating with the car. */
        int mode = g_raceCameraPresetMode[v];
        if (mode != 0 && !g_td5.paused) {
            /* Bumper / in-car camera (orig UpdateVehicleRelativeCamera
             * @ 0x00401c20): orientation is taken straight from the car's
             * display angles (roll +0x208, yaw +0x20A, pitch +0x20C), so the
             * view rotates with the vehicle's heading.
             *
             * !paused: during the pre-race fly-in/countdown the loaded camera
             * mode may be bumper (saved in g_camPackedSave), but the original
             * forces the orbit cam then; only run the in-car cam once the race
             * is actually running. After GO the spring-reset one-shot above
             * sets mode 0 (chase), so the in-car cam only appears when the
             * player cycles to it with the view button during the race. */
            TD5_LOG_I(LOG_TAG, "transition view %d: path=bumper actor_slot=%d mode=%d",
                      vi, g_actorSlotForView[vi], mode);
            UpdateVehicleRelativeCamera((int)actor, vi);
        } else {
            /* [PORT: N-way split] Re-pin THIS view's chase camera into the
             * active g_cameraPos/g_cameraBasis before the viewport renders.
             * td5_camera_update_chase_all() runs once per sim-tick and leaves
             * only the LAST view's camera active, so without this every
             * split-screen pane would render from the last view's player.
             * finalize_chase_pos only re-applies the already-computed per-view
             * position + look-at (no orbit integration), so it is safe to call
             * once per viewport each render frame. */
            td5_camera_finalize_chase_pos(actor, vi);
        }
    }

    TD5_LOG_D(LOG_TAG,
              "camera update view %d: pos=(%.2f, %.2f, %.2f) forward=(%.3f, %.3f, %.3f)",
              vi, g_cameraPos[0], g_cameraPos[1], g_cameraPos[2],
              g_cameraBasis[6], g_cameraBasis[7], g_cameraBasis[8]);
}

void td5_camera_update_transition_timer(void)
{
    /* Handled by UpdateRaceCameraTransitionState */
}

void td5_camera_cache_vehicle_angles(TD5_Actor *a, int vi)
{
    CacheVehicleCameraAngles((int)a, vi);
}

void td5_camera_get_position(float *x, float *y, float *z)
{
    *x = g_cameraPos[0];
    *y = g_cameraPos[1];
    *z = g_cameraPos[2];
}

void td5_camera_get_basis(float *right, float *up, float *forward)
{
    int i;
    for (i = 0; i < 3; i++) right[i]   = g_cameraBasis[i];
    for (i = 0; i < 3; i++) up[i]      = g_cameraBasis[3 + i];
    for (i = 0; i < 3; i++) forward[i] = g_cameraBasis[6 + i];
}

/* ========================================================================
 * Spline Functions (migrated from td5re_stubs.c)
 * ======================================================================== */

static void BuildCubicSpline3D(int *spline_state, int control_points) {
    /*
     * Catmull-Rom spline builder (0x441F90).
     * Input: 4 control points at control_points, each 3 ints (X,Y,Z in 8.8 fixed).
     * Output: 15 ints at spline_state:
     *   [0..2]  = P1 (base point)
     *   [3..5]  = cubic coefficients (a)
     *   [6..8]  = quadratic coefficients (b)
     *   [9..11] = linear coefficients (c)
     *   [12..14]= constant offset (d)
     *
     * Catmull-Rom basis matrix (scaled by 2):
     *   -1  3 -3  1
     *    2 -5  4 -1
     *   -1  0  1  0
     *    0  2  0  0
     *
     * [CONFIRMED @ 0x00441F90 BuildCubicSpline3D; L5 sweep 2026-05-21]
     *   Decompile verified line-by-line:
     *     - spline_state[0..2] = P[1].xyz (control points 1's coords)
     *     - delta loop scans 4 control points, computes (P[i] - P[1]) >> 8
     *       for each xyz component (12 deltas total in local_30[12])
     *     - Matrix multiplication loop over DAT_00474bc0 (4x4 int basis matrix)
     *       writes spline_state[3..14] (12 coefficients = 4 rows x 3 components)
     *     - Final loop divides all 12 of spline_state[3..14] by 2
     *   Port inlines the basis values directly (-1,3,-3,1)/(2,-5,4,-1)/(-1,0,1,0)/
     *   (0,2,0,0). The `(2*d1) / 2` line equals d1 — matches orig's `>> 8`
     *   delta + later `/ 2` two-step combination.
     *
     * [ARCH-DIVERGENCE — basis-matrix indirection]
     *   Orig reads DAT_00474bc0 (16 int constants in .rdata). Port inlines.
     *   Semantically identical.
     */
    int *P = (int *)(intptr_t)control_points;
    int delta[4][3];
    int i;

    if (!spline_state || !P) return;

    /* Copy base point P1 */
    spline_state[0] = P[3];  /* P1.x */
    spline_state[1] = P[4];  /* P1.y */
    spline_state[2] = P[5];  /* P1.z */

    /* Compute deltas relative to P1, scaled down by 256 (>>8) */
    for (i = 0; i < 4; i++) {
        delta[i][0] = (P[i*3+0] - P[3]) >> 8;
        delta[i][1] = (P[i*3+1] - P[4]) >> 8;
        delta[i][2] = (P[i*3+2] - P[5]) >> 8;
    }

    /* Multiply by Catmull-Rom basis matrix, then divide by 2 */
    for (i = 0; i < 3; i++) {
        int d0 = delta[0][i], d1 = delta[1][i];
        int d2 = delta[2][i], d3 = delta[3][i];
        spline_state[3+i]  = (-d0 + 3*d1 - 3*d2 + d3) / 2;  /* a (cubic) */
        spline_state[6+i]  = (2*d0 - 5*d1 + 4*d2 - d3) / 2;  /* b (quadratic) */
        spline_state[9+i]  = (-d0 + d2) / 2;                   /* c (linear) */
        spline_state[12+i] = (2*d1) / 2;                        /* d (constant) */
    }
}

/* [CONFIRMED @ 0x00442090 EvaluateCubicSpline3D; L5 sweep 2026-05-21]
 *   Byte-faithful: orig applies signed-rounding correction
 *   `(x + (x>>31 & 0xFFFu)) >> 12` at FOUR sites (t2, t3, and each of 3 axes).
 *   That adds 0xFFF when x<0 before the SAR12, rounding toward zero rather
 *   than toward negative-infinity (standard MSVC SAR pattern for div-by-pow2
 *   that preserves /4096 semantics for negative inputs).
 *
 *   Prior port used plain `>> 12` which round-toward-neg-inf for negatives:
 *   off-by-one error on negative Catmull-Rom coefficients during trackside
 *   spline cam evaluation. */
static int sar12_rz(int x) {
    /* Orig pattern: (x + (x>>31 & 0xFFFu)) >> 12
     * Adds 0xFFF when x<0 before the arithmetic shift, rounding toward zero
     * rather than toward negative-infinity. Matches MSVC's /4096 codegen. */
    int corr = ((int)((unsigned)x >> 31)) & 0xFFF;  /* 0xFFF if x<0, else 0 */
    return (x + corr) >> 12;
}
static void EvaluateCubicSpline3D(int *out_pos, int *spline_state, int t) {
    /*
     * Catmull-Rom spline evaluator (0x442090).
     * t is 12-bit fixed-point: 0 = start, 0xFFF = end.
     * result[axis] = ((a*t^3 + b*t^2 + c*t) >> 12 + d) * 256 + base[axis]
     */
    int t2, t3, i;

    if (!out_pos) return;
    if (!spline_state) {
        out_pos[0] = 0; out_pos[1] = 0; out_pos[2] = 0;
        return;
    }

    t2 = sar12_rz(t * t);
    t3 = sar12_rz(t2 * t);

    for (i = 0; i < 3; i++) {
        int a = spline_state[3+i];
        int b = spline_state[6+i];
        int c = spline_state[9+i];
        int d = spline_state[12+i];
        out_pos[i] = (sar12_rz(a * t3 + b * t2 + c * t) + d) * 256 + spline_state[i];
    }
}

/* RecomputeTracksideProjectionScale @ 0x0043E900
 * Recomputes frustum plane normals using current g_depthFovFactor.
 * Original also calls SetProjectionCenterOffset(0,0) here, but that is
 * a per-frame prep step in the original's RunRaceFrame loop; in the port
 * s_center_x/y persists at screen center and must not be zeroed. */
static void RecomputeTracksideProjectionScale(void) {
    td5_render_recompute_frustum_for_trackside();
}

/* UpdateCameraTransitionHudIndicator @ 0x0040A260
 * Single-race: indicator = actor race_position+0x383 + 2; other modes: 0.
 * [CONFIRMED @ 0x0040A260] */
static void UpdateCameraTransitionHudIndicator(int view, int actor_index) {
    char *actor;
    if (g_actorBaseAddr == 0) return;
    actor = (char *)(uintptr_t)(uint32_t)g_actorBaseAddr +
            (size_t)(unsigned int)actor_index * 0x388u;
    if (g_td5.game_type == TD5_GAMETYPE_SINGLE_RACE) {
        td5_hud_set_indicator_state(view, (int)*(uint8_t *)(actor + 0x383) + 2);
    } else {
        td5_hud_set_indicator_state(view, 0);
    }
}

/* td5_compute_heading_delta / ComputeActorRouteHeadingDelta @ 0x00434040
 * Computes 12-bit heading delta between the actor's current yaw and the
 * route heading at its span.  route_entry is a pointer to the int32_t[]
 * route_state block for this actor (RS_SLOT_INDEX=0x35, RS_ROUTE_TABLE_PTR=0x00).
 * [CONFIRMED @ 0x00434040] */
uint32_t td5_compute_heading_delta(void *route_entry) {
    int32_t *rs = (int32_t *)route_entry;
    int slot;
    char *actor;
    int32_t actor_yaw;
    int16_t span_norm;
    const uint8_t *route_table;
    uint32_t rb, t;
    int diff;

    if (!rs || !g_actorBaseAddr) return 0;
    slot = rs[0x35];   /* RS_SLOT_INDEX [CONFIRMED @ 0x00434040] */
    actor = (char *)(uintptr_t)(uint32_t)g_actorBaseAddr +
            (size_t)(unsigned int)slot * 0x388u;
    actor_yaw  = *(int32_t *)(actor + 0x1F4); /* yaw accumulator [CONFIRMED @ 0x00434040] */
    span_norm  = *(int16_t *)(actor + 0x82);  /* span_normalized [CONFIRMED @ 0x00434040] */
    route_table = (const uint8_t *)(intptr_t)rs[0x00]; /* RS_ROUTE_TABLE_PTR [CONFIRMED @ 0x00434040] */
    if (!route_table) return 0;
    rb   = route_table[(uint16_t)span_norm * 3u + 1u]; /* byte lookup [CONFIRMED @ 0x00434040] */
    diff = (actor_yaw >> 8) - (int)((rb * 0x102Cu) >> 8u);
    t    = ((uint32_t)diff - 0x800u) & 0xFFFu;
    t    = (t - 0x800u) & 0xFFFu;
    return (uint32_t)(0u - t);  /* 12-bit negation; formula: -(...) [CONFIRMED @ 0x00434040] */
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
 *   0x0040A480  InitializeRaceCameraTransitionDuration
 *     [ARCH-DIVERGENCE: trivial 10-byte setter inlined into the port's
 *      reset_race_countdown() helper in td5_game.c:3283. Orig single
 *      caller is InitializeRaceSession; the port's static initializer
 *      (g_cameraTransitionActive=0xA000 at td5_camera.c:117) plus the
 *      per-race reset_race_countdown() reach the same observable state.
 *      L5 sweep 2026-05-21]
 *
 *   0x0042DB40  ConvertFloatVec3ToIntVec3
 *     [ARCH-DIVERGENCE: matrix*vec converter with short clamp; L5 sweep 2026-05-21]
 *     Orig: 3x3 matrix * vec3 -> three __ftol -> (int)(short)... -> int[3].
 *     Result clamped to short range via the sign-extension chain.
 *     Port substitutes TransformVector3ByBasis @ 0x0042DBD0 at the
 *     UpdateTracksideCamera case-1/2 and UpdateVehicleRelativeCamera
 *     call sites — same matrix*vec math but stores full int32 (no
 *     short clamp). For offsets within ±32767 (the normal case) results
 *     are identical; near saturation the port retains higher precision.
 *
 *   0x0042DC30  ConvertFloatVec3ToIntVec3B
 *     [ARCH-DIVERGENCE: shared helper, no port surface; L5 sweep 2026-05-21]
 *     Same shape as 0x0042DB40 (matrix * vec -> __ftol -> short-clamp to
 *     int[3]). Decompile confirms identical inner body. Port-side this
 *     helper isn't broken out — math is inlined in TransformVector3ByBasis
 *     callers. No observable divergence vs sibling 0x0042DB40.
 *
 *   0x0042E030  ExtractEulerAnglesFromMatrix
 *     [ARCH-DIVERGENCE: recovery-mode Euler decomp not ported; L5 sweep 2026-05-21]
 *     Orig extracts (yaw, pitch, roll) from rotation matrix via
 *     AngleFromVector12 (atan2-12bit) at 3 sites, with a gimbal-lock
 *     branch when |pitch|=0x400. Sole caller is RefreshScriptedVehicleTransforms
 *     @ 0x00409BF0, which is itself NOT ported (vehicle_mode==1 scripted
 *     recovery — see td5_physics.c:996-1009). When port wires that branch,
 *     this function must come with it. Not currently observable.
 */
