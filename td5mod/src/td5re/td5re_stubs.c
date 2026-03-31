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
    /* Stub: transforms short[3] input through 3x3 matrix to int[3] output */
    if (out) { out[0] = 0; out[1] = 0; out[2] = 0; }
}

void BuildRotationMatrixFromAngles(float *out, short *angles) {
    /* Stub: build identity matrix */
    if (out) {
        memset(out, 0, 9 * sizeof(float));
        out[0] = 1.0f; out[4] = 1.0f; out[8] = 1.0f;
    }
}

void ConvertFloatVec3ToShortAngles(short *in, short *out) {
    if (out) { out[0] = 0; out[1] = 0; out[2] = 0; }
}

void LoadRenderRotationMatrix(float *matrix) {
    /* Stub: copies matrix into global render basis -- no-op for now */
    (void)matrix;
}

/* ========================================================================
 * Track / Spline Functions
 * ======================================================================== */

void UpdateActorTrackPosition(short *probe, int *pos) {
    (void)probe; (void)pos;
}

void ComputeActorTrackContactNormal(short *probe, int *pos, int *out_y) {
    (void)probe; (void)pos;
    if (out_y) *out_y = 0;
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

void td5_render_build_sprite_quad(int *params) { (void)params; }
void td5_render_submit_translucent(uint16_t *quad_data) { (void)quad_data; }
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

TD5_AtlasEntry *td5_asset_find_atlas_entry(void *context, const char *name) {
    (void)context; (void)name;
    static TD5_AtlasEntry s_stub = {0};
    return &s_stub;
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

int    g_cameraTransitionActive = 0;
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

TD5_CameraPreset g_cameraPresets[TD5_CAMERA_PRESET_COUNT] = {{0}};

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

const char **g_position_strings = NULL;
const char **g_wanted_msg_line1 = NULL;
const char **g_wanted_msg_line2 = NULL;

int     g_wanted_msg_timer      = 0;
int     g_wanted_msg_index      = 0;

int     g_special_render_mode   = 0;
int     g_pending_finish_timer  = 0;
int     g_race_end_state        = 0;
int32_t g_actor_best_lap        = 0;
int32_t g_actor_best_race       = 0;
void   *g_route_data            = NULL;

const int8_t g_pause_glyph_widths[256] = {0};
const char **g_pause_page_strings[8]   = {NULL};
const int    g_pause_page_sizes[8]     = {0};

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
