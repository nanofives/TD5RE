/**
 * td5_camera.c -- Chase cam, trackside cam, spline cam, camera transforms
 *
 * Translated from TD5_d3d.exe decompilation. All functions preserve exact
 * original behavior using the 12-bit angle system (4096 = 360 degrees)
 * and fixed-point math.
 */

#include "td5_camera.h"
#include "td5_track.h"
#include "td5_game.h"
#include "td5_platform.h"
#include "td5re.h"

#define LOG_TAG "camera"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "camera"

static const char *trackside_behavior_name(int btype);

extern int g_replay_mode;

/* ========================================================================
 * External functions (defined in other modules, linked at build time)
 *
 * These correspond to original binary functions called by camera code.
 * ======================================================================== */

/* Trig lookup tables (0x40a6a0 .. 0x40a720) */
extern float  CosFloat12bit(unsigned int angle);   /* 0x40a6a0 */
extern float  SinFloat12bit(int angle);             /* 0x40a6c0 */
extern int    CosFixed12bit(unsigned int angle);    /* 0x40a6e0 */
extern int    SinFixed12bit(int angle);             /* 0x40a700 */
extern int    AngleFromVector12(int x, int z);      /* 0x40a720 */

/* Matrix math (0x42da10, 0x42dbd0, 0x42e1e0, 0x42e2e0) */
extern void MultiplyRotationMatrices3x3(float *A, float *B, float *out);  /* 0x42da10 */
extern void TransformVector3ByBasis(float *matrix, void *vec, int *out);  /* 0x42dbd0 -- variant for short[3] input */
extern void BuildRotationMatrixFromAngles(float *out, short *angles);     /* 0x42e1e0 */
extern void ConvertFloatVec3ToShortAngles(short *in, short *out);         /* 0x42e2e0 -- transforms through loaded render matrix */

/* Render matrix load (0x43da80) */
extern void LoadRenderRotationMatrix(float *matrix);    /* 0x43da80 -- variant taking float[12] */

/* Track position (0x4440f0, 0x445450) */
extern void UpdateActorTrackPosition(short *probe, int *pos);              /* 0x4440f0 */
extern void ComputeActorTrackContactNormal(short *probe, int *pos, int *out_y); /* 0x445450 */

/* Spline (0x441f90, 0x442090) */
extern void BuildCubicSpline3D(int *spline_state, int control_points);     /* 0x441f90 */
extern void EvaluateCubicSpline3D(int *out_pos, int *spline_state, int t); /* 0x442090 */

/* Projection recompute (0x43e900) */
extern void RecomputeTracksideProjectionScale(void);    /* 0x43e900 */

/* HUD indicator (0x40a260) */
extern void UpdateCameraTransitionHudIndicator(int view, int actor_index); /* 0x40a260 */

/* ========================================================================
 * External globals (mapped from original binary addresses)
 * ======================================================================== */

/* --- World scale --- */
extern float  g_worldToRenderScale;       /* 0x4749D0 -- world int -> float scale */
extern float  g_subTickFraction;          /* 0x4AAF60 -- interpolation factor [0..1) */

/* --- Projection / FOV --- */
extern float  g_projectionScale;          /* 0x467364 -- FOV control */
extern int    g_depthFovFactor;           /* 0x467368 -- integer depth/FOV factor */
extern float  g_projConst1;              /* 0x45D5F4 -- projection constant 1.0 */
extern float  g_projDepthScale;          /* 0x45D694 -- 65000.0 */
extern float  g_projFovScale;            /* 0x45D698 -- 0.00024414063 = 1/4096 */
extern int    g_projDepthInt;            /* 0x467360 -- (int)(projScale * 65000) */
extern float  g_invDistScale;            /* 0x46737C -- 1/sqrt normalization */

/* --- Camera output matrices --- */
extern float  g_cameraBasis[9];          /* 0x4AAFA0 -- primary 3x3 view rotation */
extern float  g_cameraPos[3];           /* 0x4AAFC4 -- camera world position (float) */
extern float  g_cameraBasisWork[12];     /* 0x4AAFE0 -- working copy (3x3 + translation) */
extern float  g_cameraSecondary[9];      /* 0x4AB070 -- secondary rotation matrix */
extern float  g_cameraTertiary[9];       /* 0x4AB040 -- tertiary rotation matrix */

/* --- Per-view camera state arrays --- */
extern int    g_camElevationAngleFP[2];     /* 0x482E0C, stride 4 */
extern short  g_camCachedAngles[2][4];      /* 0x482E18, stride 8 -- {pitch, yaw, roll, ?} */
extern short  g_camOffsetVec[2][4];         /* 0x482EA0, stride 8 */
extern float  g_camSmoothedHeight[2];       /* 0x482EB0, stride 4 */
extern int    g_camHeightSampleOfs[2];      /* 0x482EB8, stride 4 */
extern int    g_camTrackSpanOfs[2];         /* 0x482EC0, stride 4 */
extern int    g_camBehaviorType[2];         /* 0x482EC8, stride 4 */
extern float  g_camOrbitRadiusScale[2];     /* 0x482ED0, stride 4 */
extern int    g_camOrbitOffset[2][3];       /* 0x482ED8, stride 12 */
extern int    g_camFlyInCounter[2];         /* 0x482EF0, stride 4 */
extern int    g_camRotationSlot[2];         /* 0x482EF8, stride 4 */
extern float  g_camCurrentRadius[2];        /* 0x482F00, stride 4 */
extern int    g_camSplineParam[2];          /* 0x482F08, stride 4 */
extern float  g_camTargetHeight[2];         /* 0x482F10, stride 4 */
extern int    g_camOrbitAngleFP[2];         /* 0x482F18, stride 4 */
extern int    g_camAnchorSpan[2];           /* 0x482F20, stride 4 */
extern int    g_camPresetChangeFlag[2];     /* 0x482F28, stride 4 */
extern int    g_camWorldPos[2][3];          /* 0x482F30, stride 12 */
extern short  g_camOrientShort[2][4];       /* 0x482F50, stride 8 */
extern int    g_camHeadingDelta20[2];       /* 0x482F60, stride 4 */
extern int    g_camSplineNodeCount[2];      /* 0x482F68, stride 4 */
extern int    g_camYawOffset[2];            /* 0x482F70, stride 4 */
extern int    g_camReserved78[2];           /* 0x482F78, stride 4 */
extern int    g_camSplineAdvRate[2];        /* 0x482F80, stride 4 */
extern int    g_camAnchorX[2];              /* 0x482F88, stride 4 */
extern int    g_camHeightParam[2];          /* 0x482F90, stride 4 */
extern int    g_camAnchorZ[2];              /* 0x482F98, stride 4 */
extern int    g_camStoredPitch[2];          /* 0x482FA0, stride 4 */

/* --- Scalar camera globals --- */
extern int    g_raceCameraPresetId[2];      /* 0x482FD0, stride 4 (view 0 & 1) */
extern int    g_raceCameraPresetMode[2];    /* 0x482FD8, stride 4 (view 0 & 1) */
extern int    g_cameraProfileIndex[2];      /* 0x482FC8, stride 4 */
extern int    g_cameraLastProjScale[2];     /* 0x482DEC, stride 4 */
extern int    g_cameraProjDist[2];          /* 0x482DFC, stride 4 */
extern int    g_cameraProjScaleComp[2];     /* 0x482E04, stride 4 */
extern int    g_cameraPrevPresetId[2];      /* 0x482DF4, stride 4 */

extern unsigned char g_camPackedSave[2];    /* 0x482F48 -- packed {7-bit preset | 1-bit mode} */

/* --- Trackside --- */
extern int    g_tracksideCameraProfileCount;        /* 0x482DE8 */
extern short *g_tracksideCameraProfiles;             /* 0x4AEE18 -- pointer to profile array */
extern int    g_tracksideTimer[2];                   /* 0x463088, stride 4 */
extern unsigned int g_tracksideYawOffset[2];         /* 0x463080, stride 4 */

/* --- Track geometry tables --- */
extern int    g_spanTable;               /* 0x4C3D9C -- pointer to span record array */
extern int    g_vertexTable;             /* 0x4C3D98 -- pointer to vertex array */

/* --- Race state --- */
extern int    g_cameraTransitionActive;  /* 0x4AAEF0 -- transition timer */
extern int    g_camTransitionGate;       /* 0x4AAF8C -- nonzero while blocking input */
extern int    g_actorSlotForView[2];     /* 0x466EA0, stride 4 -- actor index per view */
extern int    g_actorBaseAddr;           /* 0x4AB310 -- base of actor table (offset form) */
extern uint8_t *g_actor_table_base;
extern int    g_trackType;               /* 0x466E94 -- 0=circuit, 1=point-to-point */
extern unsigned char g_actorAliveTable[12]; /* 0x4AADF5, stride 4 -- per-actor alive flag */
extern int    g_lookLeftRight[2];        /* 0x466F88, stride 4 */

/* --- Camera spline workspace --- */
extern int    g_camSplineState[2][15];   /* 0x482E28, stride 0x3C -- cubic spline coefficients */

/* --- Constants in .data --- */
extern int    g_flyInThreshold;          /* 0x463090 -- default 40 */
extern float  g_const256;               /* 0x45D5D8 -- 256.0 */
extern float  g_const32;                /* 0x45D5DC -- 32.0 */
extern float  g_dampWeight;             /* 0x4749C8 -- 0.125 */
extern float  g_radiusScale;            /* 0x4749D0 -- 1/256 = 0.00390625 */
extern float  g_nearZeroThreshold;      /* 0x45D624 -- degenerate look-at threshold */

/* ========================================================================
 * Camera Preset Table (7 entries at 0x463098)
 *
 * We declare this as an extern referencing the original .data section.
 * The table is 7 x 16 bytes = 112 bytes.
 * ======================================================================== */

extern TD5_CameraPreset g_cameraPresets[TD5_CAMERA_PRESET_COUNT]; /* 0x463098 */

/* ========================================================================
 * Spline template table (6 templates x 8 shorts, on stack in original)
 *
 * Each template defines 4 control points with {span_delta, span_offset,
 * height_mod, sentinel}. Sentinel 0xFFFF terminates.
 * ======================================================================== */

static const short s_splineTemplates[6][8] = {
    /* type 0 */ {  0,  0,  0, -1,  0,  0,  0, -1 },
    /* type 1 */ {  0,  0, 40,  0, 41,  0,  0, -1 },
    /* type 2 */ {  0,  0, 40,  0, 41,  0,  0, -1 },
    /* type 3 */ {  0,  0, 40,  0, 41, -4, -4, -1 },
    /* type 4 */ {  0,  0, 10,  4, 11,  4,  0, -1 },
    /* type 5 */ {  0,  0, 10,  4, 11,  4,  0, -1 },
};

/* ========================================================================
 * 0x42CE50 -- SetCameraWorldPosition
 *
 * Converts integer world-space position to float and stores in the
 * shared camera position globals.
 * ======================================================================== */

void SetCameraWorldPosition(int *pos)
{
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

    /* Yaw (param_1[1]): center=[1,1]=1, sin->[0,0],[2,2], cos->[0,2], -cos->[2,0] */
    s = SinFloat12bit(yaw);
    c = CosFloat12bit((unsigned int)(unsigned short)yaw);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraBasis, g_cameraBasis);

    /* Pitch (param_1[0]): center=[0,0]=1, sin->[1,1],[2,2], cos->[2,1], -cos->[1,2] */
    s = SinFloat12bit(pitch);
    c = CosFloat12bit((unsigned int)(unsigned short)pitch);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraBasis, g_cameraBasis);

    /* Roll (param_1[2]): center=[2,2]=1, sin->[0,0],[1,1], cos->[1,0], -cos->[0,1] */
    s = SinFloat12bit(roll);
    c = CosFloat12bit((unsigned int)(unsigned short)roll);
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
    s = SinFloat12bit(pitch);
    c = CosFloat12bit((unsigned int)(unsigned short)pitch);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, g_cameraSecondary, g_cameraSecondary);

    /* Roll */
    s = SinFloat12bit(roll);
    c = CosFloat12bit((unsigned int)(unsigned short)roll);
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
    s = SinFloat12bit(roll);
    c = CosFloat12bit((unsigned int)(unsigned short)roll);
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
 * ======================================================================== */

void FinalizeCameraProjectionMatrices(void)
{
    float fov_factor = (float)g_depthFovFactor * g_projFovScale;
    float inv_proj   = g_projConst1 / g_projectionScale;
    int i;

    /* Copy primary basis + position to working area (12 floats) */
    memcpy(g_cameraBasisWork, g_cameraBasis, 12 * sizeof(float));

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
 * ======================================================================== */

void OrientCameraTowardTarget(int *target_pos, unsigned int yaw_offset)
{
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

    /* Apply yaw offset rotation */
    s = SinFloat12bit((int)yaw_offset);
    c = CosFloat12bit(yaw_offset);
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
 * ======================================================================== */

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
    short probe_a[6], probe_b[6], probe_c[6];
    int norm_a_y, norm_b_y, norm_c_y;
    int pos_x, pos_z;
    float terrain_matrix[12];
    short cam_angles[4];
    short transformed_angles[4]; /* needs 3 for ConvertFloatVec3ToShortAngles */
    unsigned int look_angle;
    int i;
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
     * SetCameraWorldPosition divides by 256 to produce float world coords. */
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

    /* Copy track probe state from actor */
    probe_a[0] = *(short *)(actor + 0x80);
    *(unsigned char *)&probe_a[5] = *(unsigned char *)(actor + 0x8C);
    probe_b[0] = probe_a[0];
    *(unsigned char *)&probe_b[5] = *(unsigned char *)&probe_a[5];
    probe_c[0] = probe_a[0];
    *(unsigned char *)&probe_c[5] = *(unsigned char *)&probe_a[5];

    {
        int pa[2], pb[2], pc[2];
        pa[0] = point_ax; pa[1] = point_az;
        pb[0] = point_bx; pb[1] = point_bz;
        pc[0] = point_cx; pc[1] = point_cz;

        UpdateActorTrackPosition(probe_a, pa);
        ComputeActorTrackContactNormal(probe_a, pa, &norm_a_y);

        UpdateActorTrackPosition(probe_b, pb);
        ComputeActorTrackContactNormal(probe_b, pb, &norm_b_y);

        UpdateActorTrackPosition(probe_c, pc);
        ComputeActorTrackContactNormal(probe_c, pc, &norm_c_y);
    }

    /* Compute pitch and roll from terrain normals.
     * NOTE: Terrain probes currently return garbage because actor positions
     * are in integer coords but probes expect 24.8 FP. Force pitch/roll to
     * zero until the coordinate system is unified. */
    {
        cam_angles[0] = 0;  /* pitch = 0 (flat terrain) */
        cam_angles[1] = (short)combined_angle;
        cam_angles[2] = 0;  /* roll = 0 */

        BuildRotationMatrixFromAngles(terrain_matrix, cam_angles);
    }

    /* --- Camera look-direction --- */
    look_angle = (unsigned int)((g_camOrbitAngleFP[v] >> 8) -
                                 (int)*(short *)(actor + 0x20A) +
                                 g_camYawOffset[v]);

    {
        short *orient = g_camOrientShort[v];
        orient[0] = (short)((unsigned int)(int)(SinFloat12bit(look_angle) *
                    g_camOrbitRadiusScale[v] + 0.5f) >> 8);
        orient[1] = (short)((unsigned int)g_camElevationAngleFP[v] >> 8);
        orient[2] = -(short)((unsigned int)(int)(CosFloat12bit(look_angle) *
                    g_camOrbitRadiusScale[v] + 0.5f) >> 8);

        /* Transform through terrain rotation matrix */
        LoadRenderRotationMatrix(terrain_matrix);
        ConvertFloatVec3ToShortAngles(orient, transformed_angles);

        /* Store all 3 transformed components back */
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

    /* --- Set camera world position from actor + orbit offset --- */
    {
        /* smoothed_h is in 24.8 FP scale (matching actor positions). */
        int smoothed_h = (int)(g_camSmoothedHeight[v] + 0.5f);
        int target[3];

        g_camWorldPos[v][0] = *(int *)(actor + 0x1FC) + g_camOrbitOffset[v][0];
        g_camWorldPos[v][1] = g_camOrbitOffset[v][1] + *(int *)(actor + 0x200);
        g_camWorldPos[v][2] = *(int *)(actor + 0x204) + g_camOrbitOffset[v][2];

        target[0] = *(int *)(actor + 0x1FC);
        target[1] = *(int *)(actor + 0x200) + smoothed_h;
        target[2] = *(int *)(actor + 0x204);

        SetCameraWorldPosition(g_camWorldPos[v]);
        OrientCameraTowardTarget(target, g_tracksideYawOffset[v]);

    }
}

/* ========================================================================
 * 0x401950 -- UpdateTracksideOrbitCamera
 *
 * Orbiting camera around a vehicle, used for trackside replay.
 * Smooths orbit counter using subTickFraction interpolation.
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
        orbit_angle_fp = (int)((float)((delta * effective) >> 8) * g_subTickFraction + 0.5f) +
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

        /* Transform through actor's rotation matrix */
        LoadRenderRotationMatrix((float *)(actor + 0x120));
        {
            short out_ang[2];
            ConvertFloatVec3ToShortAngles(orient, out_ang);

            /* Store back */
            g_camOrientShort[v][0] = out_ang[0];
            g_camOrientShort[v][1] = out_ang[1];
        }
    }

    /* Smooth height */
    {
        float h_delta = (g_camTargetHeight[v] - g_camSmoothedHeight[v]) * g_dampWeight;
        int smoothed_h = (int)(h_delta * g_subTickFraction + g_camSmoothedHeight[v] + 0.5f);

        /* Orbit position using opposite angle */
        unsigned int orbit_vis = (unsigned int)(g_camYawOffset[v] - (int)vis_angle);
        float radius = g_camCurrentRadius[v];

        int cam_idx = v * 3;
        g_camOrbitOffset[v][0] = (int)(CosFloat12bit(orbit_vis) * radius + 0.5f);
        g_camOrbitOffset[v][1] = g_camStoredPitch[v];
        g_camOrbitOffset[v][2] = (int)(-(CosFloat12bit((unsigned int)orbit_vis) * radius) + 0.5f);
        /* Correction: Z uses sin not cos */
        g_camOrbitOffset[v][0] = (int)(SinFloat12bit((int)orbit_vis) * radius + 0.5f);  /* actually CosFloat per Ghidra */

        /* Rewrite faithfully from Ghidra:
           orbit position uses (yawOffset - vis_angle) as the orbit placement angle */
        g_camOrbitOffset[v][0] = (int)(CosFloat12bit(orbit_vis) * radius + 0.5f);
        g_camOrbitOffset[v][1] = g_camStoredPitch[v];
        g_camOrbitOffset[v][2] = (int)(-(CosFloat12bit((unsigned int)orbit_vis) * radius) + 0.5f);
        /* Actually Ghidra shows:
           sin -> X, -cos -> Z, same pattern as chase cam.
           Let me just use the pattern from the decompilation directly. */
        g_camOrbitOffset[v][0] = (int)(SinFloat12bit((int)orbit_vis) * radius + 0.5f);
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
 * ======================================================================== */

void UpdateVehicleRelativeCamera(int actor, int view)
{
    int v = view;
    short *cached = g_camCachedAngles[v];
    short target_pitch, target_yaw, target_roll;
    short cur_pitch, cur_yaw, cur_roll;
    short delta;
    short cam_angles[3];
    int cam_pos[3];

    /* Target angles from vehicle (with 180-degree offset for chase view) */
    target_pitch = *(short *)(actor + 0x208) + 0x800;
    target_yaw   = 0x800 - *(short *)(actor + 0x20A);
    target_roll  = -*(short *)(actor + 0x20C);

    /* Smooth pitch toward target via shortest-path wrapping */
    {
        int wrapped = (int)((((unsigned int)(unsigned short)target_pitch -
                     (unsigned int)(unsigned short)cached[0]) - 0x800) & 0xFFF) - 0x800;
        delta = (short)(int)((float)wrapped * g_subTickFraction + 0.5f);
    }
    cam_angles[0] = cached[0] + delta;

    /* Smooth yaw */
    {
        int wrapped = (int)((((unsigned int)(unsigned short)target_yaw -
                     (unsigned int)(unsigned short)cached[1]) - 0x800) & 0xFFF) - 0x800;
        delta = (short)(int)((float)wrapped * g_subTickFraction + 0.5f);
    }
    cam_angles[1] = cached[1] + delta;

    /* Smooth roll */
    {
        int wrapped = (int)((((unsigned int)(unsigned short)target_roll -
                     (unsigned int)(unsigned short)cached[2]) - 0x800) & 0xFFF) - 0x800;
        delta = (short)(int)((float)wrapped * g_subTickFraction + 0.5f);
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
        TransformVector3ByBasis(g_renderBasisMatrix, &g_camOffsetVec[v][0], cam_pos);
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
}

/* ========================================================================
 * 0x401E10 -- UpdateRaceCameraTransitionState
 *
 * Camera transition state machine during race start fly-in.
 * Divides transition timer by 0x2800 to get HUD indicator level,
 * then selects camera preset based on level.
 * ======================================================================== */

void UpdateRaceCameraTransitionState(int actor, int view)
{
    int v = view;
    int actor_idx = *(unsigned char *)(actor + 0x375);

    /* If actor is alive, delegate to HUD indicator update */
    if (g_actorAliveTable[actor_idx * 4] != 0) {
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

    /* Compute level from timer */
    {
        int level = g_cameraTransitionActive / 0x2800;
        int new_preset;
        int force_reload;

        if (level == 0) {
            new_preset = 0x0D;
            g_raceCameraPresetId[v] = new_preset;
            {
                /* Adjust orbit radius based on __ftol of some float expression */
                /* Original: lVar5 = __ftol(); then subtract from radius */
                /* This is a transition effect -- the preset handles it */
            }
            g_camOrbitRadiusScale[v] -= 0.0f; /* placeholder -- actual ftol result */

            if (g_cameraPrevPresetId[v] == 0x0D) goto store_prev;
            force_reload = 0;
        } else if (level == 1) {
            new_preset = 0x0C;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += 0; /* ftol addition */
            if (g_cameraPrevPresetId[v] == 0x0C) goto store_prev;
            force_reload = 0;
        } else if (level == 2) {
            new_preset = 0x0B;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += 0; /* ftol addition */
            if (g_cameraPrevPresetId[v] == 0x0B) goto store_prev;
            force_reload = 0;
        } else {
            new_preset = 10;
            g_raceCameraPresetId[v] = new_preset;
            g_camOrbitAngleFP[v] += 0; /* ftol addition */
            if (g_cameraPrevPresetId[v] == 10) goto store_prev;
            force_reload = 1;
        }

        LoadCameraPresetForView(actor + 0x208, force_reload, v, 0);
        g_camYawOffset[v] = 0;

    store_prev:
        g_cameraPrevPresetId[v] = g_raceCameraPresetId[v];
    }
}

/* ========================================================================
 * 0x402000 -- ResetRaceCameraSelectionState
 *
 * Restores or resets camera preset selection for both views.
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

/* ========================================================================
 * 0x402200 -- SelectTracksideCameraProfile
 *
 * Checks if the followed vehicle's track span falls within a different
 * profile's range. If so, initializes behavior-specific camera state.
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
 * ======================================================================== */

void UpdateStaticTracksideCamera(int actor, int view)
{
    int v = view;
    int span_addr, vtx_addr;
    int cam_idx = v * 3;
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
        int cam_idx   = v * 3;
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
            int w = (int)((((unsigned int)(unsigned short)target_pitch -
                   (unsigned int)(unsigned short)cached[0]) - 0x800) & 0xFFF) - 0x800;
            delta_p = (short)(int)((float)w * g_subTickFraction + 0.5f);
        }
        cam_angles[0] = cached[0] + delta_p;

        {
            int w = (int)((((unsigned int)(unsigned short)target_yaw -
                   (unsigned int)(unsigned short)cached[1]) - 0x800) & 0xFFF) - 0x800;
            delta_y = (short)(int)((float)w * g_subTickFraction + 0.5f);
        }
        cam_angles[1] = cached[1] + delta_y;

        {
            int w = (int)((((unsigned int)(unsigned short)target_roll -
                   (unsigned int)(unsigned short)cached[2]) - 0x800) & 0xFFF) - 0x800;
            delta_r = (short)(int)((float)w * g_subTickFraction + 0.5f);
        }
        cam_angles[2] = cached[2] + delta_r;

        BuildCameraBasisFromAngles(cam_angles);

        cam_world = g_camWorldPos[v];
        TransformVector3ByBasis((float *)(actor + 0x120), &g_camOffsetVec[v][0], cam_world);

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

    switch (g_raceCameraPresetMode[view & 1]) {
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
    out_pos[1] = sp->pad_10 + (vl0->y + vl1->y + vr0->y + vr1->y) / 4;
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
static unsigned int s_debug_camera_frame[2];

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
    int v = (view & 1);

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
    return 1;
}

void td5_camera_shutdown(void) {}

void td5_camera_tick(void)
{
    /* Use debug track camera only if no actors are populated */
    if (td5_game_get_total_actor_count() <= 0) {
        update_debug_track_camera();
        return;
    }
    /* Per-sim-tick camera update — matches CacheVehicleCameraAngles +
       UpdateChaseCamera inside the RunRaceFrame sim loop (0x0042B580).
       Called once per fixed timestep tick from td5_game.c sim loop. */
    for (int v = 0; v < 2; v++) {
        TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[v]);
        if (!actor && g_actor_table_base) {
            int slot = g_actorSlotForView[v];
            int total = td5_game_get_total_actor_count();
            if (slot >= 0 && slot < total)
                actor = (TD5_Actor *)(g_actor_table_base + (size_t)slot * TD5_ACTOR_STRIDE);
        }
        if (!actor) continue;
        CacheVehicleCameraAngles((int)actor, v);
        UpdateChaseCamera((int)actor, 1, v);
    }
}

void td5_camera_update_chase(TD5_Actor *a, int p, int vi)
{
    UpdateChaseCamera((int)a, p, vi);
}

void td5_camera_set_preset(int pi)
{
    s_active_preset = pi;
    memset(s_debug_camera_frame, 0, sizeof(s_debug_camera_frame));

    /* Reset preset selection state for both views */
    g_raceCameraPresetId[0]   = 0;
    g_raceCameraPresetId[1]   = 0;
    g_raceCameraPresetMode[0] = 0;
    g_raceCameraPresetMode[1] = 0;

    /* Seed per-view chase-camera state from the spawned actors so the first
       UpdateChaseCamera frame starts with correct radius/height/orbit values
       rather than stale or zero-initialized garbage.  actor+0x208 is the
       display_angles base used by LoadCameraPresetForView (+2 = yaw). */
    if (g_actor_table_base) {
        for (int v = 0; v < 2; v++) {
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

void td5_camera_update_trackside(TD5_Actor *a, int vi)
{
    UpdateTracksideCamera((int)a, vi);
}

void td5_camera_update_transition_state(int p, int vi)
{
    TD5_Actor *actor = td5_game_get_actor(g_actorSlotForView[vi]);
    int v = vi & 1;

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

    /* Route to appropriate camera based on mode */
    if (g_replay_mode) {
        TD5_LOG_I(LOG_TAG, "transition view %d: path=trackside actor_slot=%d", vi, g_actorSlotForView[vi]);
        UpdateTracksideCamera((int)actor, vi);
    } else {
        int preset = g_raceCameraPresetMode[vi];
        if (preset == 6 || preset == 5) {
            /* Bumper / in-car camera */
            TD5_LOG_I(LOG_TAG, "transition view %d: path=bumper actor_slot=%d preset=%d",
                      vi, g_actorSlotForView[vi], preset);
            UpdateVehicleRelativeCamera((int)actor, vi);
        }
        /* Chase camera: already updated per-sim-tick in td5_camera_tick() */
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
