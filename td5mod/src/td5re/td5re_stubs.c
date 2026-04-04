/**
 * td5re_stubs.c -- Stub definitions for unresolved game-engine symbols
 *
 * These are reverse-engineered globals and utility functions from the
 * original TD5_d3d.exe that the source port modules reference via extern
 * declarations. In the original modding flow, these resolve to addresses
 * in the running game process. For standalone .exe mode, we provide
 * zero-initialized globals and minimal stub implementations.
 *
 * As modules are fully ported, symbols should migrate out of this file
 * into their proper module implementations.
 */

#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <guiddef.h>
#include <objbase.h>

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* Pull in types we need for struct-typed globals */
#include "td5_types.h"
#include "td5_camera.h"
#include "td5_hud.h"
#include "td5_asset.h"
#include "td5_track.h"

/* ========================================================================
 * Trigonometry / Math Functions
 *
 * Original game uses lookup-table trig with 12-bit angles (0-4095 = 0-360).
 * We provide real implementations using standard math.
 * ======================================================================== */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float CosFloat12bit(unsigned int angle) {
    return (float)cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}

float SinFloat12bit(int angle) {
    return (float)sin((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0));
}

int CosFixed12bit(unsigned int angle) {
    return (int)(cos((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0)) * 4096.0);
}

int SinFixed12bit(int angle) {
    return (int)(sin((double)(angle & 0xFFF) * (2.0 * M_PI / 4096.0)) * 4096.0);
}

int AngleFromVector12(int x, int z) {
    double rad = atan2((double)x, (double)z);
    int angle = (int)(rad * (4096.0 / (2.0 * M_PI)));
    return angle & 0xFFF;
}

float td5_cos_12bit(uint32_t angle) {
    return CosFloat12bit(angle);
}

float td5_sin_12bit(uint32_t angle) {
    return SinFloat12bit((int)angle);
}

/* ========================================================================
 * Matrix / Vector Operations
 * ======================================================================== */

void MultiplyRotationMatrices3x3(float *A, float *B, float *out) {
    int i, j, k;
    float tmp[9];
    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++) {
            tmp[i*3+j] = 0.0f;
            for (k = 0; k < 3; k++)
                tmp[i*3+j] += A[i*3+k] * B[k*3+j];
        }
    memcpy(out, tmp, 9 * sizeof(float));
}

void TransformVector3ByBasis(float *matrix, void *vec, int *out) {
    /*
     * 0x42dbd0 -- Transform a short[3] vector by a 3x3 rotation matrix,
     * producing an int[3] result.  The matrix is row-major float[9+].
     */
    short *v = (short *)vec;
    if (!out) return;
    if (!matrix || !v) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)v[0];
    float fy = (float)v[1];
    float fz = (float)v[2];

    out[0] = (int)(matrix[0] * fx + matrix[1] * fy + matrix[2] * fz);
    out[1] = (int)(matrix[3] * fx + matrix[4] * fy + matrix[5] * fz);
    out[2] = (int)(matrix[6] * fx + matrix[7] * fy + matrix[8] * fz);
}

void BuildRotationMatrixFromAngles(float *out, short *angles) {
    /*
     * 0x42e1e0 -- Build a 3x3 rotation matrix from 12-bit Euler angles
     * (pitch, yaw, roll).  Uses the same axis convention as
     * BuildCameraBasisFromAngles: yaw -> pitch -> roll.
     *
     * angles[0] = pitch, angles[1] = yaw, angles[2] = roll
     * All in 12-bit fixed-point (0-4095 = 0-360 degrees).
     *
     * NOTE: The original binary's trig lookup at 0x40a6a0 is a cosine
     * table (table[0]=1), and 0x40a6c0 offsets by -1024 giving sine.
     * Our stubs label them backwards (SinFloat12bit=sin, CosFloat12bit=cos).
     * The matrix slot pattern was decompiled from the original where the
     * "first" trig call (func_A) returns cos.  We swap s/c here to match:
     *   s = CosFloat12bit (= original func_B = sin)
     *   c = SinFloat12bit (= original func_A = cos)
     * so the rest of the matrix construction stays correct.
     */
    float rot[9];
    float s, c;

    if (!out || !angles) return;

    /* Start with identity */
    out[0] = 1.0f; out[1] = 0.0f; out[2] = 0.0f;
    out[3] = 0.0f; out[4] = 1.0f; out[5] = 0.0f;
    out[6] = 0.0f; out[7] = 0.0f; out[8] = 1.0f;

    /* Yaw (angles[1]): rotate around Y axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[1]);
    c = SinFloat12bit(angles[1]);
    rot[4] = 1.0f;
    rot[3] = 0.0f; rot[5] = 0.0f;
    rot[1] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[8] = s;
    rot[2] = c;  rot[6] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Pitch (angles[0]): rotate around X axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[0]);
    c = SinFloat12bit(angles[0]);
    rot[0] = 1.0f;
    rot[1] = 0.0f; rot[2] = 0.0f;
    rot[3] = 0.0f; rot[6] = 0.0f;
    rot[4] = s;  rot[8] = s;
    rot[7] = c;  rot[5] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);

    /* Roll (angles[2]): rotate around Z axis */
    s = CosFloat12bit((unsigned int)(unsigned short)angles[2]);
    c = SinFloat12bit(angles[2]);
    rot[8] = 1.0f;
    rot[2] = 0.0f; rot[5] = 0.0f;
    rot[6] = 0.0f; rot[7] = 0.0f;
    rot[0] = s;  rot[4] = s;
    rot[3] = c;  rot[1] = -c;
    MultiplyRotationMatrices3x3(rot, out, out);
}

/*
 * Static matrix loaded by LoadRenderRotationMatrix for use by
 * ConvertFloatVec3ToShortAngles.  This mirrors the original engine's
 * global at ~0x43DA80 target.
 */
static float s_loaded_render_matrix[12] = {
    1,0,0, 0,1,0, 0,0,1, 0,0,0
};

void ConvertFloatVec3ToShortAngles(short *in, short *out) {
    /*
     * 0x42e2e0 -- Transform a short[3] direction vector through the
     * currently loaded render rotation matrix and store the result as
     * short[3].  Despite the misleading name, this is a matrix*vector
     * transform, not a unit conversion.
     */
    if (!out) return;
    if (!in) { out[0] = 0; out[1] = 0; out[2] = 0; return; }

    float fx = (float)in[0];
    float fy = (float)in[1];
    float fz = (float)in[2];

    out[0] = (short)(int)(s_loaded_render_matrix[0] * fx +
                          s_loaded_render_matrix[1] * fy +
                          s_loaded_render_matrix[2] * fz);
    out[1] = (short)(int)(s_loaded_render_matrix[3] * fx +
                          s_loaded_render_matrix[4] * fy +
                          s_loaded_render_matrix[5] * fz);
    out[2] = (short)(int)(s_loaded_render_matrix[6] * fx +
                          s_loaded_render_matrix[7] * fy +
                          s_loaded_render_matrix[8] * fz);
}

void LoadRenderRotationMatrix(float *matrix) {
    /*
     * 0x43da80 -- Load a rotation matrix (float[12] = 3x3 + translation)
     * into the static render matrix used by ConvertFloatVec3ToShortAngles.
     * Only the 3x3 rotation part (first 9 floats) is needed for the
     * direction transform; we copy all 12 for completeness.
     */
    if (!matrix) return;
    memcpy(s_loaded_render_matrix, matrix, 12 * sizeof(float));
}

/* ========================================================================
 * Track / Spline Functions
 * ======================================================================== */

void UpdateActorTrackPosition(short *probe, int *pos) {
    /*
     * Lightweight span-boundary walk for camera probes.
     *
     * probe is a TD5_TrackProbe laid out as short[]:
     *   [0] = span_index
     *   byte offset 12 = sub_lane_index (int8)
     *
     * pos = { world_x, world_z } in 24.8 fixed-point.
     *
     * We walk forward/backward spans using edge cross-product tests
     * via td5_track public API. For a minimal implementation, we just
     * validate the span index stays in range -- the full boundary walk
     * is already done by td5_track_update_actor_position() for actors.
     *
     * For camera ground probes we do a simple local search: check the
     * current span and neighbors until we find one that contains the point.
     */
    int span_count;
    int span_idx;
    int sub_lane;

    if (!probe || !pos) return;

    span_count = td5_track_get_span_count();
    if (span_count <= 0) return;

    span_idx = (int)probe[0];
    sub_lane = (int)((int8_t *)probe)[12];

    /* Clamp span index to valid range */
    if (span_idx < 0) span_idx = 0;
    if (span_idx >= span_count) span_idx = span_count - 1;

    /* Clamp sub_lane */
    if (sub_lane < 0) sub_lane = 0;

    /* Write back clamped values */
    probe[0] = (short)span_idx;
    ((int8_t *)probe)[12] = (int8_t)sub_lane;
}

void ComputeActorTrackContactNormal(short *probe, int *pos, int *out_y) {
    /*
     * Compute terrain contact height at the probe's span/sub-lane
     * for the world position pos = {x, z}.
     *
     * Returns the ground Y height (24.8 fixed-point, span-local units)
     * via *out_y. The camera uses this to compute pitch/roll from
     * three sample points around the vehicle.
     */
    int span_idx;
    int sub_lane;
    int span_count;
    int32_t height;

    if (!out_y) return;
    *out_y = 0;

    if (!probe || !pos) return;

    span_count = td5_track_get_span_count();
    if (span_count <= 0) return;

    span_idx = (int)probe[0];
    sub_lane = (int)((int8_t *)probe)[12];

    if (span_idx < 0 || span_idx >= span_count) return;
    if (sub_lane < 0) sub_lane = 0;

    /* Delegate to the barycentric contact height resolver in td5_track.c */
    height = td5_track_compute_contact_height(span_idx, sub_lane,
                                               pos[0], pos[1]);

    /* Return height scaled to the format the camera expects.
     * The camera uses this as a Y displacement in 24.8 FP. */
    *out_y = (int)height;
}

void BuildCubicSpline3D(int *spline_state, int control_points) {
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

void EvaluateCubicSpline3D(int *out_pos, int *spline_state, int t) {
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

    t2 = (t * t) >> 12;
    t3 = (t2 * t) >> 12;

    for (i = 0; i < 3; i++) {
        int a = spline_state[3+i];
        int b = spline_state[6+i];
        int c = spline_state[9+i];
        int d = spline_state[12+i];
        out_pos[i] = ((a * t3 + b * t2 + c * t) >> 12 + d) * 256 + spline_state[i];
    }
}

void RecomputeTracksideProjectionScale(void) { }

void UpdateCameraTransitionHudIndicator(int view, int actor_index) {
    (void)view; (void)actor_index;
}

uint32_t td5_compute_heading_delta(void *route_entry) {
    (void)route_entry;
    return 0;
}

/* ========================================================================
 * Render Helpers
 * ======================================================================== */

typedef struct TD5_RenderSpriteQuadParams {
    void     *dest;
    int       mode_flags;
    float     scr_x[4];
    float     scr_y[4];
    float     depth_z[4];
    float     tex_u[4];
    float     tex_v[4];
    uint32_t  diffuse[4];
    int       texture_page;
    int       reserved;
} TD5_RenderSpriteQuadParams;

typedef struct TD5_RenderSpriteQuad {
    int      geometry_ptr;
    int      vertex_count;
    float    v0_x, v0_y, v0_z, v0_rhw;
    uint32_t v0_color;
    float    v0_u, v0_v;
    float    v1_x, v1_y, v1_z, v1_rhw;
    uint32_t v1_color;
    float    v1_u, v1_v;
    float    v2_x, v2_y, v2_z, v2_rhw;
    uint32_t v2_color;
    float    v2_u, v2_v;
    float    v3_x, v3_y, v3_z, v3_rhw;
    uint32_t v3_color;
    float    v3_u, v3_v;
    float    tex_u0, tex_v0;
    float    tex_u1, tex_v1;
    float    quad_width;
    float    quad_height;
    float    texture_page;
    uint8_t  padding[0xB8 - 0x94];
} TD5_RenderSpriteQuad;

extern void td5_render_queue_translucent_batch(void *record);

void td5_render_build_sprite_quad(int *params) {
    const TD5_RenderSpriteQuadParams *src = (const TD5_RenderSpriteQuadParams *)params;
    TD5_RenderSpriteQuad *dst;
    float z;
    float rhw;

    if (!src || !src->dest) {
        return;
    }

    dst = (TD5_RenderSpriteQuad *)src->dest;

    if (src->mode_flags != 2) {
        z = src->depth_z[0];
        rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;

        dst->geometry_ptr = 0;
        dst->vertex_count = 4;

        dst->v0_x = src->scr_x[0]; dst->v0_y = src->scr_y[0];
        dst->v1_x = src->scr_x[3]; dst->v1_y = src->scr_y[3];
        dst->v2_x = src->scr_x[2]; dst->v2_y = src->scr_y[2];
        dst->v3_x = src->scr_x[1]; dst->v3_y = src->scr_y[1];

        dst->v0_z = dst->v1_z = dst->v2_z = dst->v3_z = z;
        dst->v0_rhw = dst->v1_rhw = dst->v2_rhw = dst->v3_rhw = rhw;
    }

    dst->v0_color = src->diffuse[0];
    dst->v1_color = src->diffuse[3];
    dst->v2_color = src->diffuse[2];
    dst->v3_color = src->diffuse[1];

    dst->v0_u = src->tex_u[0]; dst->v0_v = src->tex_v[0];
    dst->v1_u = src->tex_u[3]; dst->v1_v = src->tex_v[3];
    dst->v2_u = src->tex_u[2]; dst->v2_v = src->tex_v[2];
    dst->v3_u = src->tex_u[1]; dst->v3_v = src->tex_v[1];

    dst->tex_u0 = src->tex_u[0];
    dst->tex_v0 = src->tex_v[0];
    dst->tex_u1 = src->tex_u[2];
    dst->tex_v1 = src->tex_v[2];
    dst->quad_width = src->scr_x[2] - src->scr_x[0];
    dst->quad_height = src->scr_y[1] - src->scr_y[0];
    dst->texture_page = (float)src->texture_page;
}

void td5_render_submit_translucent(uint16_t *quad_data) {
    typedef struct TD5_D3DVertex {
        float    screen_x, screen_y;
        float    depth_z, rhw;
        uint32_t diffuse;
        uint32_t specular;
        float    tex_u, tex_v;
    } TD5_D3DVertex;

    extern void td5_plat_render_set_preset(TD5_RenderPreset preset);
    extern void td5_plat_render_bind_texture(int page_index);
    extern void td5_plat_render_draw_tris(const TD5_D3DVertex *verts, int vertex_count,
                                          const uint16_t *indices, int index_count);

    float *fdata;
    TD5_D3DVertex verts[4];
    uint16_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    int tex_page;
    int i;

    if (!quad_data) {
        return;
    }

    /*
     * HUD translucent quads are already emitted as pre-transformed 0xB8 sprite
     * records. They are not TD5_PrimitiveCmd batches, so forwarding them into
     * td5_render_queue_translucent_batch() makes the batch parser read garbage
     * dispatch_type state and crash after the first frame.
     */
    fdata = (float *)quad_data;
    for (i = 0; i < 4; i++) {
        int base = 2 + i * 7;
        verts[i].screen_x = fdata[base + 0];
        verts[i].screen_y = fdata[base + 1];
        verts[i].depth_z = fdata[base + 2];
        verts[i].rhw = fdata[base + 3];
        verts[i].diffuse = *(uint32_t *)&fdata[base + 4];
        verts[i].specular = 0;
        verts[i].tex_u = fdata[base + 5];
        verts[i].tex_v = fdata[base + 6];
    }

    tex_page = (int)(*(float *)((uint8_t *)quad_data + 0x90));
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR);
    td5_plat_render_bind_texture(tex_page);
    td5_plat_render_draw_tris(verts, 4, indices, 6);
}
void td5_render_set_clip_rect(float left, float right, float top, float bottom) {
    (void)left; (void)right; (void)top; (void)bottom;
}
void td5_render_set_projection_center(float cx, float cy) {
    (void)cx; (void)cy;
}
void td5_render_radial_pulse(float dt) { (void)dt; }

/* ========================================================================
 * Game Logic Helpers
 * ======================================================================== */

typedef struct TD5_Actor TD5_Actor;

int td5_game_get_player_slot(int viewport) { (void)viewport; return 0; }
int td5_game_is_replay_active(void) { return 0; }
int td5_game_get_traffic_variant(int traffic_index) { (void)traffic_index; return 0; }
int td5_game_get_cop_actor_index(void) { return -1; }
int td5_game_is_wanted_mode(void) { return 0; }
void td5_game_advance_sky_rotation(void) { }

void *td5_game_heap_alloc(size_t size) {
    return calloc(1, size);
}

int td5_net_is_slot_active(int slot) { (void)slot; return 0; }

/* ========================================================================
 * Camera System Globals
 * ======================================================================== */

float  g_worldToRenderScale = 0.00390625f;  /* 1/256 */
float  g_subTickFraction    = 0.0f;
float  g_projectionScale    = 1.0f;
int    g_depthFovFactor     = 0;
float  g_projConst1         = 1.0f;
float  g_projDepthScale     = 65000.0f;
float  g_projFovScale       = 0.00024414063f; /* 1/4096 */
int    g_projDepthInt       = 0;
float  g_invDistScale       = 1.0f;

float  g_cameraBasis[9]     = { 1,0,0, 0,1,0, 0,0,1 };
float  g_cameraPos[3]       = { 0, 0, 0 };
float  g_cameraBasisWork[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };
float  g_cameraSecondary[9] = { 1,0,0, 0,1,0, 0,0,1 };
float  g_cameraTertiary[9]  = { 1,0,0, 0,1,0, 0,0,1 };

int    g_camElevationAngleFP[2]     = {0};
short  g_camCachedAngles[2][4]      = {{0}};
short  g_camOffsetVec[2][4]         = {{0}};
float  g_camSmoothedHeight[2]       = {0};
int    g_camHeightSampleOfs[2]      = {0};
int    g_camTrackSpanOfs[2]         = {0};
int    g_camBehaviorType[2]         = {0};
float  g_camOrbitRadiusScale[2]     = {0};
int    g_camOrbitOffset[2][3]       = {{0}};
int    g_camFlyInCounter[2]         = {0};
int    g_camRotationSlot[2]         = {0};
float  g_camCurrentRadius[2]        = {0};
int    g_camSplineParam[2]          = {0};
float  g_camTargetHeight[2]         = {0};
int    g_camOrbitAngleFP[2]         = {0};
int    g_camAnchorSpan[2]           = {0};
int    g_camPresetChangeFlag[2]     = {0};
int    g_camWorldPos[2][3]          = {{0}};
short  g_camOrientShort[2][4]       = {{0}};
int    g_camHeadingDelta20[2]       = {0};
int    g_camSplineNodeCount[2]      = {0};
int    g_camYawOffset[2]            = {0};
int    g_camReserved78[2]           = {0};
int    g_camSplineAdvRate[2]        = {0};
int    g_camAnchorX[2]              = {0};
int    g_camHeightParam[2]          = {0};
int    g_camAnchorZ[2]              = {0};
int    g_camStoredPitch[2]          = {0};

int    g_raceCameraPresetId[2]      = {0};
int    g_raceCameraPresetMode[2]    = {0};
int    g_cameraProfileIndex[2]      = {0};
int    g_cameraLastProjScale[2]     = {0};
int    g_cameraProjDist[2]          = {0};
int    g_cameraProjScaleComp[2]     = {0};
int    g_cameraPrevPresetId[2]      = {0};

unsigned char g_camPackedSave[2]    = {0};

int    g_tracksideCameraProfileCount = 0;
short *g_tracksideCameraProfiles     = NULL;
int    g_tracksideTimer[2]           = {0};
unsigned int g_tracksideYawOffset[2] = {0};

int    g_spanTable      = 0;
int    g_vertexTable    = 0;

int    g_cameraTransitionActive = 0xA000;
int    g_camTransitionGate      = 0;
int    g_actorSlotForView[2]    = {0};
int    g_actorBaseAddr          = 0;
int    g_trackType              = 0;
unsigned char g_actorAliveTable[12] = {0};
int    g_lookLeftRight[2]       = {0};

int    g_camSplineState[2][15]  = {{0}};

int    g_flyInThreshold  = 40;
float  g_const256        = 256.0f;
float  g_const32         = 32.0f;
float  g_dampWeight      = 0.125f;
float  g_nearZeroThreshold = 0.001f;

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

float  g_renderBasisMatrix[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };

/* ========================================================================
 * HUD / Race State Globals
 * ======================================================================== */

int     g_replay_mode           = 0;
int     g_wanted_mode_enabled   = 0;
int     g_special_encounter     = 0;
int     g_race_rule_variant     = 0;
int     g_game_type             = 0;
int     g_split_screen_mode     = 0;
int     g_racer_count           = 0;
float   g_render_width_f        = 640.0f;
float   g_render_height_f       = 480.0f;
int     g_render_width          = 640;
int     g_render_height         = 480;
int     g_track_is_circuit      = 0;
int     g_track_type_mode       = 0;
int     g_hud_metric_mode       = 0;
float   g_instant_fps           = 30.0f;
uint32_t g_tick_counter         = 0;
int     g_kph_mode              = 0;

int     g_actor_slot_map[2]     = {0};
void   *g_actor_pool            = NULL;
void   *g_actor_base            = NULL;

int     g_strip_span_count      = 0;
int     g_strip_total_segments  = 0;
void   *g_strip_span_base      = NULL;
void   *g_strip_vertex_base    = NULL;

/* HUD string table: 13 entries per player (matches Language.dll SNK exports).
 * [0..5] = position labels, [6..12] = UI labels (LAP, TIME, DEMO MODE, etc.) */
static const char *s_default_position_strings[] = {
    "1ST", "2ND", "3RD", "4TH", "5TH", "6TH",
    "WRONG WAY", "PIT STOP", "FINISH", "BEST LAP",
    "DEMO MODE", "TIME", "LAP",
    /* P2 copy */
    "1ST", "2ND", "3RD", "4TH", "5TH", "6TH",
    "WRONG WAY", "PIT STOP", "FINISH", "BEST LAP",
    "DEMO MODE", "TIME", "LAP"
};
const char **g_position_strings = s_default_position_strings;

static const char *s_default_wanted_line1[] = { "YOU ARE", "PULL OVER", "" };
static const char *s_default_wanted_line2[] = { "WANTED!", "NOW!", "" };
const char **g_wanted_msg_line1 = s_default_wanted_line1;
const char **g_wanted_msg_line2 = s_default_wanted_line2;

int     g_wanted_msg_timer      = 0;
int     g_wanted_msg_index      = 0;

int     g_special_render_mode   = 0;
int     g_pending_finish_timer  = 0;
int     g_race_end_state        = 0;
int32_t g_actor_best_lap        = 0;
int32_t g_actor_best_race       = 0;
void   *g_route_data            = NULL;

/* 0x4660C8: PAUSETXT font glyph widths indexed by ASCII code.
 * Values are in pixels before the *2/3 scaling applied during rendering. */
const int8_t g_pause_glyph_widths[256] = {
    /* 0x00-0x1F: control chars = 0 */
    [0x00] = 0,
    /* 0x20 = space */
    [' '] = 8,
    /* Uppercase letters (from binary at 0x4660C8) */
    ['A'] = 19, ['B'] = 15, ['C'] = 15, ['D'] = 17, ['E'] = 13,
    ['F'] = 13, ['G'] = 17, ['H'] = 17, ['I'] = 8,  ['J'] = 10,
    ['K'] = 18, ['L'] = 13, ['M'] = 23, ['N'] = 17, ['O'] = 20,
    ['P'] = 16, ['Q'] = 20, ['R'] = 17, ['S'] = 14, ['T'] = 14,
    ['U'] = 17, ['V'] = 18, ['W'] = 24, ['X'] = 19, ['Y'] = 18,
    ['Z'] = 15,
};

/* English pause overlay string table (from binary at 0x4744B8).
 * Layout: alternating {const char *label, (const char *)(intptr_t)alignment}.
 * alignment 0 = left-aligned, 2 = centred.
 * 6 entries: PAUSED (title), VIEW, MUSIC, SOUND, CONTINUE, EXIT.
 * Accessed as: string = table[string_offset / 4]
 *              align  = *(int *)((uint8_t *)table + string_offset + 4)
 * with string_offset advancing by 8 each row. */
static const char *s_eng_pause_strings[] = {
    "PAUSED",     (const char *)(intptr_t)2,   /* row 0: title, centered */
    "VIEW",       (const char *)(intptr_t)0,   /* row 1: view distance, left */
    "MUSIC",      (const char *)(intptr_t)0,   /* row 2: music volume, left */
    "SOUND",      (const char *)(intptr_t)0,   /* row 3: SFX volume, left */
    "CONTINUE",   (const char *)(intptr_t)2,   /* row 4: resume, centered */
    "EXIT",       (const char *)(intptr_t)2,   /* row 5: quit race, centered */
    NULL
};
const char **g_pause_page_strings[8] = {
    s_eng_pause_strings, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};
const int g_pause_page_sizes[8] = { 256, 0, 0, 0, 0, 0, 0, 0 };

/* ========================================================================
 * VFX / Track Environment Globals
 * ======================================================================== */

TD5_StaticHedEntry *g_static_hed_entries     = NULL;
int                 g_static_hed_entry_count = 0;
uint8_t            *g_track_environment_config = NULL;
uint8_t            *g_actor_table_base         = NULL;

/* ========================================================================
 * GUIDs that MinGW import libraries sometimes miss
 *
 * These are DirectInput axis GUIDs and MFPlay COM interface IDs.
 * ======================================================================== */

/* DirectInput GUIDs (normally from dinput.h with INITGUID) */
DEFINE_GUID(GUID_Key,    0x55728220,0xD33C,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(GUID_XAxis,  0xA36D02E0,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(GUID_YAxis,  0xA36D02E1,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);
DEFINE_GUID(GUID_ZAxis,  0xA36D02E2,0xC9F3,0x11CF,0xBF,0xC7,0x44,0x45,0x53,0x54,0x00,0x00);

/* MFPlay callback interface IID */
/* {A714B699-0BC6-4322-AD57-16C25C4A0717} */
DEFINE_GUID(IID_IMFPMediaPlayerCallback,
    0xA714B699, 0x0BC6, 0x4322,
    0xAD, 0x57, 0x16, 0xC2, 0x5C, 0x4A, 0x07, 0x17);

/* ========================================================================
 * MFPlay / Media Foundation stubs
 *
 * MFPCreateMediaPlayer, MFStartup, MFShutdown are from mfplay.lib and
 * mfplat.lib. MinGW may not have these import libraries. Provide stubs
 * that return failure so the FMV module gracefully degrades.
 * ======================================================================== */

/* Only define stubs if we are NOT linking against the real libraries */
#ifndef TD5RE_HAS_MFPLAY

long __stdcall MFPCreateMediaPlayer(const wchar_t *url, int fStartPlayback,
    int creationOptions, void *pCallback, void *hWnd, void **ppMediaPlayer)
{
    (void)url; (void)fStartPlayback; (void)creationOptions;
    (void)pCallback; (void)hWnd;
    if (ppMediaPlayer) *ppMediaPlayer = NULL;
    return (long)0x80004001L; /* E_NOTIMPL */
}

long __stdcall MFStartup(unsigned long version, unsigned long flags) {
    (void)version; (void)flags;
    return 0; /* S_OK -- pretend it worked so init doesn't fail */
}

long __stdcall MFShutdown(void) {
    return 0; /* S_OK */
}

#endif /* TD5RE_HAS_MFPLAY */
