/**
 * td5_enums.h - Enum definitions for Test Drive 5 reverse engineering
 *
 * These enums are applied in Ghidra (TD5_d3d.exe on port 8195,
 * M2DX.dll on port 8193) under the /TD5 category.
 *
 * Created: 2026-03-24
 */

#ifndef TD5_ENUMS_H
#define TD5_ENUMS_H

/* ============================================================
 * Global Address Defines -- Game State & Flow
 *
 * Canonical addresses for key runtime globals.
 * ============================================================ */
#define TD5_ADDR_GAME_STATE             0x004C3CE8  /* dword: g_gameState */
#define TD5_ADDR_SELECTED_GAME_TYPE     0x0049635C  /* dword: g_selectedGameType */ /* FIXED wave3: was 0x4AAF6C, that is a Time Trial boolean */
#define TD5_ADDR_RACE_LAUNCH_TRIGGER    0x00495248  /* dword: g_raceLaunchTrigger */ /* NEW wave3 */
#define TD5_ADDR_RACE_LAUNCH_MODE       0x0049524C  /* dword: g_raceLaunchMode */ /* NEW wave3 */
#define TD5_ADDR_NETWORK_SESSION_ACTIVE 0x004962C0  /* dword: g_networkSessionActive */ /* NEW wave3 */
#define TD5_ADDR_DX_CONFIRM_GATE        0x00473B6C  /* dword: g_dxConfirmGate */ /* NEW wave3 */
#define TD5_ADDR_MOVIE_PLAYBACK_PENDING 0x00474C00  /* dword: g_moviePlaybackPending */ /* NEW wave3 */
#define TD5_ADDR_FRONTEND_INIT_PENDING  0x00474C04  /* dword: g_frontendInitPending */ /* NEW wave3 */
#define TD5_ADDR_FLOW_CONTEXT           0x004962D4  /* dword: g_flowContext (1=RaceMenu,2=QuickRace,3=2P,4=NetPlay) */ /* NEW wave3 */
#define TD5_ADDR_TWO_PLAYER_STATE       0x004962A0  /* dword: g_twoPlayerState */ /* NEW wave3 */
#define TD5_ADDR_WEATHER_TYPE           0x004C3DE8  /* dword: g_weatherType */ /* NEW wave3 */
#define TD5_ADDR_TIME_TRIAL_ACTIVE      0x004AAF6C  /* dword: boolean, 1 when Time Trial mode */ /* NEW wave3: formerly mislabeled as g_selectedGameType */

/* ============================================================
 * TD5_GameState - Main game state machine
 * Global: g_gameState @ 0x004C3CE8
 * Referenced by: RunMainGameLoop (0x442170), ConfigureGameTypeFlags (0x410CA0)
 * ============================================================ */
typedef enum TD5_GameState {
    GAMESTATE_INTRO     = 0,
    GAMESTATE_MENU      = 1,
    GAMESTATE_RACE      = 2,
    GAMESTATE_BENCHMARK = 3
} TD5_GameState;

/* ============================================================
 * TD5_WeatherType - Weather/precipitation mode
 * Global: g_weatherType @ 0x004C3DE8
 * Snow gate: JNZ at 0x4465F4 (6-byte NOP to enable)
 * ============================================================ */
typedef enum TD5_WeatherType {
    WEATHER_RAIN  = 0,
    WEATHER_SNOW  = 1,
    WEATHER_CLEAR = 2
} TD5_WeatherType;

/* ============================================================
 * TD5_GameType - Selected game mode
 * Global: g_selectedGameType @ 0x0049635C
 * FIXED wave3: address corrected from 0x004AAF6C (that is a
 *   Time Trial boolean, NOT the game type selector).
 *   Confirmed via ConfigureGameTypeFlags dispatch at 0x410EB4:
 *     CALL dword ptr [ECX*4 + 0x464104]
 *   ECX = DAT_0049635C.
 * ============================================================ */
typedef enum TD5_GameType {
    GAMETYPE_SINGLE_RACE  = 1,
    GAMETYPE_CHAMPIONSHIP = 2,
    GAMETYPE_DRAG_RACE    = 3,
    GAMETYPE_COP_CHASE    = 4,
    GAMETYPE_PRACTICE     = 5,
    GAMETYPE_MULTIPLAYER  = 6,
    GAMETYPE_TIME_TRIAL   = 7,
    GAMETYPE_SURVIVAL     = 8,
    GAMETYPE_SPEED_TRAP   = 9
} TD5_GameType;

/* ============================================================
 * TD5_NpcGroupType - NPC racer group scoring type      NEW wave3
 *
 * Stored at +0x00 of each NpcRacerGroup record (0x4643B8).
 * Controls high-score display format in DrawHighScorePanel (0x413010).
 * ============================================================ */
typedef enum TD5_NpcGroupType {
    NPC_GROUP_TIME       = 0,   /* Point-to-point: centiseconds MM:SS.cc */
    NPC_GROUP_LAP        = 1,   /* Era cup (lap-based): centiseconds MM:SS.cc */
    NPC_GROUP_POINTS     = 2,   /* Championship/Ultimate: raw integer points */
    /* 3 is unused */
    NPC_GROUP_TIMETRIAL  = 4    /* Time Trial: milliseconds MM:SS.mmm */
} TD5_NpcGroupType;

/* ============================================================
 * TD5_VehicleMode - Actor vehicle control mode
 * ============================================================ */
typedef enum TD5_VehicleMode {
    VMODE_NORMAL   = 0,
    VMODE_SCRIPTED = 1
} TD5_VehicleMode;

/* ============================================================
 * TD5_InputBits - Input bitmask flags (from td5_sdk.h)
 * These are OR'd together in the input DWORD.
 * ============================================================ */
typedef enum TD5_InputBits {
    INPUT_STEER_LEFT    = 0x00000001,
    INPUT_STEER_RIGHT   = 0x00000002,
    INPUT_THROTTLE      = 0x00000200,
    INPUT_BRAKE         = 0x00000400,
    INPUT_GEAR_DOWN     = 0x00080000,
    INPUT_GEAR_UP       = 0x00100000,
    INPUT_HORN          = 0x00200000,
    INPUT_ANALOG_Y_FLAG = 0x08000000,
    INPUT_ANALOG_X_FLAG = 0x80000000
} TD5_InputBits;

/* ============================================================
 * Camera Preset Constants                               NEW wave3
 *
 * 7 presets at g_cameraPresetTable (0x463098), 16 bytes each.
 * Indexed via DAT_00482FD0[player_slot].
 * Consumer: SetCameraPreset (0x401450).
 * ============================================================ */
#define TD5_CAMERA_PRESET_COUNT     7   /* NEW wave3: presets 0-6 (6=bumper cam) */
#define TD5_ADDR_CAMERA_PRESET_TBL  0x00463098  /* NEW wave3 */

/* ============================================================
 * D3DRENDERSTATE - DirectX 6 render state IDs
 * Used in M2DX.dll SetRenderState calls and TD5_d3d.exe rendering.
 * Also created in Ghidra on M2DX.dll (port 8193).
 * ============================================================ */
typedef enum D3DRENDERSTATE {
    D3DRENDERSTATE_TEXTUREHANDLE   =  1,
    D3DRENDERSTATE_ZENABLE         =  7,
    D3DRENDERSTATE_FILLMODE        =  8,
    D3DRENDERSTATE_SHADEMODE       =  9,
    D3DRENDERSTATE_ZWRITEENABLE    = 14,   /* 0x0E */
    D3DRENDERSTATE_ALPHATESTENABLE = 15,   /* 0x0F */
    D3DRENDERSTATE_TEXTUREMAG      = 16,   /* 0x10 - texture stage state */
    D3DRENDERSTATE_TEXTUREMIN      = 17,   /* 0x11 */
    D3DRENDERSTATE_TEXTUREMIPFILTER= 18,   /* 0x12 */
    D3DRENDERSTATE_SRCBLEND        = 19,   /* 0x13 */
    D3DRENDERSTATE_DESTBLEND       = 20,   /* 0x14 */
    D3DRENDERSTATE_TEXTUREMAPBLEND = 21,   /* 0x15 */
    D3DRENDERSTATE_CULLMODE        = 22,   /* 0x16 */
    D3DRENDERSTATE_ZFUNC           = 23,   /* 0x17 */
    D3DRENDERSTATE_DITHERENABLE    = 26,   /* 0x1A */
    D3DRENDERSTATE_ALPHABLENDENABLE= 27,   /* 0x1B */
    D3DRENDERSTATE_FOGENABLE       = 28,   /* 0x1C */
    D3DRENDERSTATE_SPECULARENABLE  = 29,   /* 0x1D */
    D3DRENDERSTATE_STIPPLEDALPHA   = 33,   /* 0x21 */
    D3DRENDERSTATE_FOGCOLOR        = 34,   /* 0x22 */
    D3DRENDERSTATE_FOGTABLEMODE    = 35,   /* 0x23 */
    D3DRENDERSTATE_FOGSTART        = 36,   /* 0x24 */
    D3DRENDERSTATE_FOGEND          = 37,   /* 0x25 */
    D3DRENDERSTATE_FOGDENSITY      = 38,   /* 0x26 */
    D3DRENDERSTATE_COLORKEYENABLE  = 41,   /* 0x29 */
    D3DRENDERSTATE_SRCBLENDALPHA   = 44,   /* 0x2C */
    D3DRENDERSTATE_DESTBLENDALPHA  = 45    /* 0x2D */
} D3DRENDERSTATE;

#endif /* TD5_ENUMS_H */
