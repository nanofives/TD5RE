/**
 * td5_sdk.h - Test Drive 5 Mod SDK
 *
 * Game globals, function typedefs, and struct definitions for hooking
 * TD5_d3d.exe from an injected DLL.
 *
 * All addresses are Virtual Addresses in the loaded process (base 0x00400000).
 * Generated from complete Ghidra reverse engineering (864 named functions).
 */

#ifndef TD5_SDK_H
#define TD5_SDK_H

#include <stdint.h>
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =======================================================================
 * Game State
 * ======================================================================= */

#define g_gameState             (*(int32_t*)0x004C3CE8)   /* 0=intro, 1=menu, 2=race, 3=benchmark */
#define g_selectedGameType      (*(int32_t*)0x004AAF6C)   /* 1-9 game mode */
#define g_dragRaceModeEnabled   (*(int32_t*)0x004AAF48)
#define g_wantedModeEnabled     (*(int32_t*)0x004AAF68)
#define g_benchmarkModeActive   (*(int32_t*)0x004AAF58)
#define g_inputPlaybackActive   (*(int32_t*)0x00466E9C)   /* 1 during replay */
#define g_replayModeFlag        (*(int32_t*)0x004AAF64)   /* 1 during attract demo */

/* =======================================================================
 * Screen / Viewport
 * ======================================================================= */

#define g_renderWidthF          (*(float*)0x004AAF08)     /* Race render width (float) */
#define g_renderHeightF         (*(float*)0x004AAF0C)     /* Race render height (float) */
#define g_frontendCanvasW       (*(int32_t*)0x00495228)   /* Frontend virtual width (640) */
#define g_frontendCanvasH       (*(int32_t*)0x00495200)   /* Frontend virtual height (480) */
#define g_raceViewportLayoutMode (*(int32_t*)0x004AAF89)  /* 0=full, 1=horiz split, 2=vert split */

/* =======================================================================
 * Race State
 * ======================================================================= */

#define g_racerCount            (*(int32_t*)0x004AAF00)   /* Total actors (6 or 12 w/ traffic) */
#define g_humanPlayerCount      (*(int32_t*)0x004AAF89)   /* 0=1P, 1=2P -- NOTE: shares byte w/ viewport! */
#define g_twoPlayerModeState    (*(int32_t*)0x004962A0)   /* 0=1P, 1-2=car select, 5-6=network */
#define g_trafficActorsEnabled  (*(int32_t*)0x004B0FA4)
#define g_specialEncounterEnabled (*(int32_t*)0x004AAF70)
#define g_circuitLapCount       (*(int32_t*)0x004AAF04)
#define g_reverseTrackDirection (*(int32_t*)0x004AACF8)

/* =======================================================================
 * Timing
 * ======================================================================= */

#define g_framePrevTimestamp     (*(uint32_t*)0x004AAF44)  /* timeGetTime() of last frame */
#define g_normalizedFrameDt     (*(float*)0x004AAD70)     /* 1.0 at 30fps */
#define g_simTickBudget         (*(float*)0x00466E88)     /* Accumulated ticks to simulate */
#define g_subTickFraction       (*(float*)0x004AAF60)     /* 0.0-1.0 interpolation fraction */

/* =======================================================================
 * Surfaces / DirectDraw
 * ======================================================================= */

#define g_backBufferSurface     (*(void**)0x00495220)     /* IDirectDrawSurface* back buffer */
#define g_appExref              (*(void**)0x0045D53C)     /* App/window state object pointer */

/* DirectDraw external reference array (used by DXDraw::Flip wrapper):
 *   [0] = IDirectDraw*, [1] = IDirectDrawSurface* front, [2] = IDirectDrawSurface* back */
#define g_ddExrefPtr            (*(void***)0x0045D564)

/* =======================================================================
 * Input
 * ======================================================================= */

#define g_playerControlBits     ((uint32_t*)0x00482FFC)   /* DWORD[2] - P1 at [0], P2 at [1] */
#define g_p1InputSource         (*(int32_t*)0x00497A58)   /* 0=keyboard, 1+=joystick */
#define g_p2InputSource         (*(int32_t*)0x00465FF4)

/* Input bitmask bits */
#define INPUT_STEER_LEFT    0x00000001
#define INPUT_STEER_RIGHT   0x00000002
#define INPUT_THROTTLE      0x00000200
#define INPUT_BRAKE         0x00000400
#define INPUT_HORN          0x00200000
#define INPUT_GEAR_UP       0x00100000
#define INPUT_GEAR_DOWN     0x00080000
#define INPUT_ANALOG_X_FLAG 0x80000000
#define INPUT_ANALOG_Y_FLAG 0x08000000

/* =======================================================================
 * Actor Runtime State
 * ======================================================================= */

#define g_actorRuntimeState     ((uint8_t*)0x004AB108)    /* 6 slots x 0x388 bytes each */
#define ACTOR_STRIDE            0x388
#define ACTOR_MAX_SLOTS         6

/* Actor struct field offsets (see td5_actor_struct.h for full layout) */
#define ACTOR_OFF_SPAN_INDEX    0x082   /* int16: current STRIP.DAT span */
#define ACTOR_OFF_SPAN_COUNTER  0x084   /* int32: monotonic forward span count */
#define ACTOR_OFF_WORLD_POS_X   0x1FC   /* int32: 24.8 fixed-point X */
#define ACTOR_OFF_WORLD_POS_Y   0x200   /* int32: 24.8 fixed-point Y */
#define ACTOR_OFF_WORLD_POS_Z   0x204   /* int32: 24.8 fixed-point Z */
#define ACTOR_OFF_FORWARD_SPEED 0x318   /* int32: forward speed */
#define ACTOR_OFF_LATERAL_SPEED 0x31C   /* int32: lateral speed */
#define ACTOR_OFF_ENGINE_SPEED  0x314   /* int32: engine RPM */
#define ACTOR_OFF_STEERING_CMD  0x33C   /* int16: steering command +/-0x18000 */
#define ACTOR_OFF_GEAR_CURRENT  0x36A   /* int8: current gear index */
#define ACTOR_OFF_RACE_FINISHED 0x380   /* int8: 1 when finished */
#define ACTOR_OFF_PHYSICS_PTR   0x1BC   /* int32: pointer to physics table */
#define ACTOR_OFF_TUNING_PTR    0x1B8   /* int32: pointer to tuning table */

/* Helper to get actor slot pointer */
#define TD5_ACTOR(slot) (g_actorRuntimeState + (slot) * ACTOR_STRIDE)

/* =======================================================================
 * Cheat Flags
 * ======================================================================= */

#define g_cheatNitroEnabled     (*(int32_t*)0x004AAF7C)
#define g_cheatRemoteBraking    (*(int32_t*)0x0049629C)

/* =======================================================================
 * Track Environment
 * ======================================================================= */

#define g_trackEnvironmentConfig (*(uint8_t**)0x004AEE20) /* LEVELINF.DAT loaded data */
#define g_weatherType           (*(int32_t*)0x004C3DE8)   /* 0=rain, 1=snow(cut), 2=clear */

/* =======================================================================
 * NPC / High Scores
 * ======================================================================= */

#define g_npcRacerGroupTable    ((uint8_t*)0x004643B8)    /* 26 groups x 0xA4 bytes */
#define NPC_GROUP_STRIDE        0xA4

/* =======================================================================
 * Display Mode Table
 * ======================================================================= */

#define g_displayModeStringTable ((char*)0x004974BC)      /* 16 entries x 0x20 bytes ("WxHxBPP") */
#define g_configuredDisplayModeOrdinal (*(int32_t*)0x00497498)

/* =======================================================================
 * Frontend
 * ======================================================================= */

#define g_frontendScreenFnTable ((void**)0x004655C4)      /* 30 function pointers */
#define g_frontendInnerState    (*(int32_t*)0x004951E0)
#define g_frontendButtonIndex   (*(int32_t*)0x004951E4)
#define g_frontendAnimTick      (*(int32_t*)0x0049522C)

/* Current screen function pointer (set by SetFrontendScreen, called each frame) */
#define g_currentScreenFnPtr    (*(void (__cdecl **)(void))0x00495238)

/* =======================================================================
 * Sound
 * ======================================================================= */

#define g_cdAudioTrackOffset    (*(int32_t*)0x00465E14)

/* =======================================================================
 * Function Typedefs & Pointers
 *
 * Usage: call Original_FuncName() from hooks, or call game functions directly:
 *   TD5_QueueRaceHudFormattedText(100, 50, "Hello %d", 42);
 * ======================================================================= */

/* --- Core Loop --- */
typedef void (__cdecl *fn_RunRaceFrame)(void);
#define TD5_RunRaceFrame ((fn_RunRaceFrame)0x0042B580)

typedef void (__cdecl *fn_RunFrontendDisplayLoop)(void);
#define TD5_RunFrontendDisplayLoop ((fn_RunFrontendDisplayLoop)0x00414B50)

typedef void (__cdecl *fn_GameStateDispatcher)(void);
#define TD5_GameStateDispatcher ((fn_GameStateDispatcher)0x00442170)

/* --- HUD --- */
typedef void (__cdecl *fn_QueueRaceHudFormattedText)(int x, int y, const char* fmt, ...);
#define TD5_QueueRaceHudFormattedText ((fn_QueueRaceHudFormattedText)0x00428320)

typedef void (__cdecl *fn_InitializeRaceHudLayout)(void);
#define TD5_InitializeRaceHudLayout ((fn_InitializeRaceHudLayout)0x00437BA0)

typedef void (__cdecl *fn_RenderRaceHudOverlays)(void);
#define TD5_RenderRaceHudOverlays ((fn_RenderRaceHudOverlays)0x004388A0)

/* --- Rendering --- */
typedef void (__cdecl *fn_FlushQueuedTranslucentPrimitives)(void);
#define TD5_FlushQueuedTranslucentPrimitives ((fn_FlushQueuedTranslucentPrimitives)0x00431340)

typedef void (__cdecl *fn_FlushProjectedPrimitiveBuckets)(void);
#define TD5_FlushProjectedPrimitiveBuckets ((fn_FlushProjectedPrimitiveBuckets)0x0043E2F0)

typedef void (__cdecl *fn_ConfigureProjectionForViewport)(int width, int height);
#define TD5_ConfigureProjectionForViewport ((fn_ConfigureProjectionForViewport)0x0043E7E0)

/* --- Camera --- */
typedef void (__cdecl *fn_UpdateChaseCamera)(void);
#define TD5_UpdateChaseCamera ((fn_UpdateChaseCamera)0x00401590)

/* --- Input --- */
typedef void (__cdecl *fn_PollRaceSessionInput)(void);
#define TD5_PollRaceSessionInput ((fn_PollRaceSessionInput)0x0042C470)

/* --- Physics --- */
typedef void (__cdecl *fn_UpdatePlayerVehicleDynamics)(uint8_t* actor);
#define TD5_UpdatePlayerVehicleDynamics ((fn_UpdatePlayerVehicleDynamics)0x00404030)

typedef void (__cdecl *fn_UpdateAIVehicleDynamics)(uint8_t* actor);
#define TD5_UpdateAIVehicleDynamics ((fn_UpdateAIVehicleDynamics)0x00404EC0)

/* --- AI --- */
typedef void (__cdecl *fn_ComputeAIRubberBandThrottle)(uint8_t* actor, int slot);
#define TD5_ComputeAIRubberBandThrottle ((fn_ComputeAIRubberBandThrottle)0x00432D60)

/* --- Collision --- */
typedef void (__cdecl *fn_ResolveVehicleContacts)(void);
#define TD5_ResolveVehicleContacts ((fn_ResolveVehicleContacts)0x00409150)

typedef void (__cdecl *fn_ApplyVehicleCollisionImpulse)(uint8_t* actorA, uint8_t* actorB, void* contact);
#define TD5_ApplyVehicleCollisionImpulse ((fn_ApplyVehicleCollisionImpulse)0x004079C0)

/* --- Race Progression --- */
typedef void (__cdecl *fn_UpdateRaceOrder)(void);
#define TD5_UpdateRaceOrder ((fn_UpdateRaceOrder)0x0042F5B0)

typedef void (__cdecl *fn_CheckRaceCompletionState)(void);
#define TD5_CheckRaceCompletionState ((fn_CheckRaceCompletionState)0x00409E80)

/* --- Frontend --- */
typedef void (__cdecl *fn_SetFrontendScreen)(int screenIndex);
#define TD5_SetFrontendScreen ((fn_SetFrontendScreen)0x00414610)

/* --- Save --- */
typedef void (__cdecl *fn_WritePackedConfigTd5)(void);
#define TD5_WritePackedConfigTd5 ((fn_WritePackedConfigTd5)0x0040F8D0)

/* --- NoOpHookStub: 9 lifecycle call sites, ideal hook point --- */
typedef void (__cdecl *fn_NoOpHookStub)(void);
#define TD5_NoOpHookStub ((fn_NoOpHookStub)0x00418450)

/* --- CRT (linked into EXE) --- */
typedef int (__cdecl *fn_rand)(void);
#define TD5_rand ((fn_rand)0x00448157)

/* =======================================================================
 * IAT Entries (resolved import pointers in .rdata)
 * ======================================================================= */

typedef DWORD (__stdcall *pfn_timeGetTime)(void);
#define TD5_IAT_timeGetTime     (*(pfn_timeGetTime*)0x0045D5C0)

/* --- Weather --- */
typedef void (__cdecl *fn_RenderAmbientParticleStreaks)(int actorPtr, float param2, int viewIdx);
#define TD5_RenderAmbientParticleStreaks ((fn_RenderAmbientParticleStreaks)0x00446560)

/* --- Track Loading --- */
typedef void (__cdecl *fn_LoadTrackRuntimeData)(void);
#define TD5_LoadTrackRuntimeData ((fn_LoadTrackRuntimeData)0x0042FB90)

/* =======================================================================
 * Frontend Text Rendering
 * ======================================================================= */

/* DirectDraw surface pointers used by frontend text */
#define g_primaryWorkSurface    (*(void**)0x00496260)   /* primary 640x480 work surface */
#define g_secondaryWorkSurface  (*(void**)0x00496264)   /* secondary work surface */
#define g_menuFontSurface       (*(void**)0x00496278)   /* MenuFont.tga surface */
#define g_smallFontSurface      (*(void**)0x0049627C)   /* active small font surface */

/* Per-character width tables (signed char[128] in .data section) */
#define g_smallFontAdvance      ((signed char*)0x004662D0)
#define g_smallFontDisplay      ((signed char*)0x004662F0)
#define g_smallFontYOffset      ((signed char*)0x004663E4)
#define g_largeFontAdvance      ((signed char*)0x004664F8)
#define g_largeFontDisplay      ((signed char*)0x00466518)

/* Language DLL pointer (for language index check) */
#define g_langDllString         (*(char**)0x0045D1E4)   /* SNK_LangDLL imported ptr */

/* IDirectDrawSurface COM vtable slot indices */
#define VT_DDS_BLTFAST          7    /* +0x1C */
#define VT_DDS_GETDC            17   /* +0x44 */
#define VT_DDS_RELEASEDC        26   /* +0x68 */

/* Frontend text function addresses */
#define VA_DrawLocPrimary       0x004242B0  /* DrawFrontendLocalizedStringPrimary   */
#define VA_DrawLocSecondary     0x00424390  /* DrawFrontendLocalizedStringSecondary  */
#define VA_DrawLocToSurface     0x00424560  /* DrawFrontendLocalizedStringToSurface  */
#define VA_MeasureOrDraw        0x00412D50  /* MeasureOrDrawFrontendFontString       */
#define VA_MeasureOrCenter      0x00424A50  /* MeasureOrCenterFrontendLocalizedStr   */

/* Frontend text function typedefs */
typedef int  (__cdecl *fn_DrawLocString)(const char*, unsigned int, int);
typedef int  (__cdecl *fn_DrawLocToSurface)(const char*, int, int, void*);
typedef int  (__cdecl *fn_MeasureOrDrawFont)(const char*, unsigned int, unsigned int, void*);
typedef int  (__cdecl *fn_MeasureOrCenter)(const char*, int, int);

/* =======================================================================
 * Race HUD Text / Glyph System
 * ======================================================================= */

/* Race HUD glyph atlas globals */
#define g_hudGlyphQueueBuf      (*(void**)0x004A2CBC)   /* 0xB800-byte queued primitive buffer */
#define g_hudGlyphAtlasBase     (*(int*)0x004A2CB8)     /* base of 0x404-byte atlas (per font) */
#define g_hudGlyphQueueCount    (*(int*)0x004A2CC0)     /* queued glyph count (max 0x100) */

/* Race HUD character mapping table (ASCII -> glyph index) */
#define g_hudCharMap            ((unsigned char*)0x004669F4)

/* Race HUD font atlas: 64 glyphs (4 rows x 16 cols), each = 4 floats:
 *   [0] = U start (texture X), [1] = V start (texture Y),
 *   [2] = glyph width,         [3] = glyph height
 * Atlas pointer at g_hudGlyphAtlasBase + fontIndex * 0x404.
 * Texture pointer at offset +0x400 from atlas base. */

typedef void (__cdecl *fn_BuildSpriteQuadTemplate)(int* templateParams);
#define TD5_BuildSpriteQuadTemplate ((fn_BuildSpriteQuadTemplate)0x00432BD0)

typedef void (__cdecl *fn_InitRaceHudFontAtlas)(void);
#define TD5_InitRaceHudFontAtlas ((fn_InitRaceHudFontAtlas)0x00428240)

typedef void (__cdecl *fn_QueueRaceHudText)(int, int, int, int, const char*, ...);
#define TD5_QueueRaceHudFormattedText_VA  0x00428320

typedef void (__cdecl *fn_FlushRaceHudText)(void);
#define TD5_FlushRaceHudText_VA  0x00428570

/* =======================================================================
 * M2DX.DLL Functions (base 0x10000000)
 * ======================================================================= */

/* These need GetProcAddress or ordinal-based lookup at runtime */
#define M2DX_RecordDisplayModeIfUsable_VA  0x10008020
#define M2DX_SetRenderState_VA             0x10001770
#define M2DX_TextureClamp_VA               0x10001700
#define M2DX_FullScreen_VA                 0x10002170

/* =======================================================================
 * Vehicle Physics (~16 functions)
 * ======================================================================= */

/* UpdateVehicleActor -- per-actor dispatcher, selects physics/scripted path */
typedef void (__cdecl *fn_UpdateVehicleActor)(short* actor);
#define TD5_UpdateVehicleActor ((fn_UpdateVehicleActor)0x00406780)

/* IntegrateVehiclePoseAndContacts -- core integration: gravity, angles, track contact */
typedef void (__cdecl *fn_IntegrateVehiclePoseAndContacts)(short* actor);
#define TD5_IntegrateVehiclePoseAndContacts ((fn_IntegrateVehiclePoseAndContacts)0x00405E80)

/* UpdatePlayerVehicleControlState -- decodes input bitmask to steering/throttle/brake */
typedef void (__cdecl *fn_UpdatePlayerVehicleControlState)(int actorSlot);
#define TD5_UpdatePlayerVehicleControlState ((fn_UpdatePlayerVehicleControlState)0x00402E60)

/* RefreshVehicleWheelContactFrames -- builds per-wheel contact frames for suspension */
typedef void (__cdecl *fn_RefreshVehicleWheelContactFrames)(short* actor);
#define TD5_RefreshVehicleWheelContactFrames ((fn_RefreshVehicleWheelContactFrames)0x00403720)

/* IntegrateWheelSuspensionTravel -- spring-damper per wheel + central roll/pitch */
typedef void (__cdecl *fn_IntegrateWheelSuspensionTravel)(int actorPtr, int tuningPtr, int param3, int param4);
#define TD5_IntegrateWheelSuspensionTravel ((fn_IntegrateWheelSuspensionTravel)0x00403A20)

/* UpdateVehicleSuspensionResponse -- aggregates wheel contact into chassis forces */
typedef void (__cdecl *fn_UpdateVehicleSuspensionResponse)(int actorPtr);
#define TD5_UpdateVehicleSuspensionResponse ((fn_UpdateVehicleSuspensionResponse)0x004057F0)

/* ClampVehicleAttitudeLimits -- pitch/roll safety clamping */
typedef void (__cdecl *fn_ClampVehicleAttitudeLimits)(int actorPtr);
#define TD5_ClampVehicleAttitudeLimits ((fn_ClampVehicleAttitudeLimits)0x00405B40)

/* UpdateVehiclePoseFromPhysicsState -- lighter pose refresh callback */
typedef void (__cdecl *fn_UpdateVehiclePoseFromPhysicsState)(short* actor);
#define TD5_UpdateVehiclePoseFromPhysicsState ((fn_UpdateVehiclePoseFromPhysicsState)0x004063A0)

/* UpdateEngineSpeedAccumulator -- slews RPM toward target from speed/gear */
typedef void (__cdecl *fn_UpdateEngineSpeedAccumulator)(int actorPtr, int tuningPtr);
#define TD5_UpdateEngineSpeedAccumulator ((fn_UpdateEngineSpeedAccumulator)0x0042EDF0)

/* UpdateAutomaticGearSelection -- RPM-threshold upshift/downshift */
typedef void (__cdecl *fn_UpdateAutomaticGearSelection)(int actorPtr, int tuningPtr);
#define TD5_UpdateAutomaticGearSelection ((fn_UpdateAutomaticGearSelection)0x0042EF10)

/* ComputeDriveTorqueFromGearCurve -- interpolated torque curve lookup */
typedef int (__cdecl *fn_ComputeDriveTorqueFromGearCurve)(int actorPtr, int tuningPtr);
#define TD5_ComputeDriveTorqueFromGearCurve ((fn_ComputeDriveTorqueFromGearCurve)0x0042F030)

/* ApplySteeringTorqueToWheels -- differential torque for yaw from steering */
typedef void (__cdecl *fn_ApplySteeringTorqueToWheels)(int actorPtr, int tuningPtr);
#define TD5_ApplySteeringTorqueToWheels ((fn_ApplySteeringTorqueToWheels)0x0042EEA0)

/* UpdatePlayerSteeringWeightBalance -- 2P trailing player steering boost */
typedef void (__cdecl *fn_UpdatePlayerSteeringWeightBalance)(void);
#define TD5_UpdatePlayerSteeringWeightBalance ((fn_UpdatePlayerSteeringWeightBalance)0x004036B0)

/* ComputeReverseGearTorque -- reverse gear RPM and drive torque */
typedef int (__cdecl *fn_ComputeReverseGearTorque)(int actorPtr, int tuningPtr, int throttle);
#define TD5_ComputeReverseGearTorque ((fn_ComputeReverseGearTorque)0x00403C80)

/* UpdateVehicleState0fDamping -- state 0x0F damped yaw/velocity decay */
typedef void (__cdecl *fn_UpdateVehicleState0fDamping)(int actorPtr);
#define TD5_UpdateVehicleState0fDamping ((fn_UpdateVehicleState0fDamping)0x00403D90)

/* ApplyReverseGearThrottleSign -- negates throttle in reverse gear */
typedef void (__cdecl *fn_ApplyReverseGearThrottleSign)(int actorPtr);
#define TD5_ApplyReverseGearThrottleSign ((fn_ApplyReverseGearThrottleSign)0x0042F010)

/* =======================================================================
 * AI & Traffic (~20 functions)
 * ======================================================================= */

/* UpdateActorRouteThresholdState -- 3-state throttle/brake writer for AI */
typedef uint32_t (__cdecl *fn_UpdateActorRouteThresholdState)(int actorPtr);
#define TD5_UpdateActorRouteThresholdState ((fn_UpdateActorRouteThresholdState)0x00434AA0)

/* UpdateActorTrackBehavior -- main per-frame AI routing update */
typedef void (__cdecl *fn_UpdateActorTrackBehavior)(int* actorPtr);
#define TD5_UpdateActorTrackBehavior ((fn_UpdateActorTrackBehavior)0x00434FE0)

/* UpdateActorSteeringBias -- core AI steering actuator from heading delta */
typedef void (__cdecl *fn_UpdateActorSteeringBias)(int actorPtr, int steeringWeight);
#define TD5_UpdateActorSteeringBias ((fn_UpdateActorSteeringBias)0x004340C0)

/* UpdateActorTrackOffsetBias -- lateral offset bias for peer avoidance */
typedef void (__cdecl *fn_UpdateActorTrackOffsetBias)(int* actorPtr);
#define TD5_UpdateActorTrackOffsetBias ((fn_UpdateActorTrackOffsetBias)0x00434900)

/* FindActorTrackOffsetPeer -- nearest same-lane forward actor scan */
typedef int* (__cdecl *fn_FindActorTrackOffsetPeer)(int* actorPtr);
#define TD5_FindActorTrackOffsetPeer ((fn_FindActorTrackOffsetPeer)0x004337E0)

/* FindNearestRoutePeer -- nearest same-lane actor within 33 spans */
typedef int* (__cdecl *fn_FindNearestRoutePeer)(int* actorPtr);
#define TD5_FindNearestRoutePeer ((fn_FindNearestRoutePeer)0x00433680)

/* InitializeActorTrackPose -- full track placement + heading + physics reset */
typedef void (__cdecl *fn_InitializeActorTrackPose)(uint32_t actorIdx, short spanIdx, char subSpan, int reverseFlag);
#define TD5_InitializeActorTrackPose ((fn_InitializeActorTrackPose)0x00434350)

/* ComputeTrackSpanProgress -- projects actor XZ onto span forward axis (0-0xFF) */
typedef int64_t (__cdecl *fn_ComputeTrackSpanProgress)(int spanIdx, int* actorPos);
#define TD5_ComputeTrackSpanProgress ((fn_ComputeTrackSpanProgress)0x004345B0)

/* SampleTrackTargetPoint -- generates world-space target point for AI steering */
typedef void (__cdecl *fn_SampleTrackTargetPoint)(int spanIdx, int progress, int* outPos, int offsetBias);
#define TD5_SampleTrackTargetPoint ((fn_SampleTrackTargetPoint)0x00434800)

/* UpdateRaceActors -- master per-frame dispatcher: racers + traffic + encounter */
typedef void (__cdecl *fn_UpdateRaceActors)(void);
#define TD5_UpdateRaceActors ((fn_UpdateRaceActors)0x00436A70)

/* RecycleTrafficActorFromQueue -- per-frame traffic despawn/respawn */
typedef void (__cdecl *fn_RecycleTrafficActorFromQueue)(void);
#define TD5_RecycleTrafficActorFromQueue ((fn_RecycleTrafficActorFromQueue)0x004353B0)

/* InitializeTrafficActorsFromQueue -- initial traffic spawn from TRAFFIC.BUS */
typedef void (__cdecl *fn_InitializeTrafficActorsFromQueue)(void);
#define TD5_InitializeTrafficActorsFromQueue ((fn_InitializeTrafficActorsFromQueue)0x00435940)

/* UpdateTrafficRoutePlan -- traffic AI: routing, steering, peer avoidance */
typedef void (__cdecl *fn_UpdateTrafficRoutePlan)(int actorSlot);
#define TD5_UpdateTrafficRoutePlan ((fn_UpdateTrafficRoutePlan)0x00435E80)

/* UpdateTrafficActorMotion -- traffic motion dispatcher (normal vs recovery) */
typedef void (__cdecl *fn_UpdateTrafficActorMotion)(short* actor);
#define TD5_UpdateTrafficActorMotion ((fn_UpdateTrafficActorMotion)0x00443ED0)

/* IntegrateVehicleFrictionForces -- simplified 2-axle tire model for traffic */
typedef void (__cdecl *fn_IntegrateVehicleFrictionForces)(int actorPtr);
#define TD5_IntegrateVehicleFrictionForces ((fn_IntegrateVehicleFrictionForces)0x004438F0)

/* UpdateTrafficVehiclePose -- traffic physics->render, track contact */
typedef void (__cdecl *fn_UpdateTrafficVehiclePose)(short* actor);
#define TD5_UpdateTrafficVehiclePose ((fn_UpdateTrafficVehiclePose)0x00443CF0)

/* ApplyDampedSuspensionForce -- simple 2-DOF spring-damper for traffic */
typedef void (__cdecl *fn_ApplyDampedSuspensionForce)(int actorPtr, int rollInput, int pitchInput);
#define TD5_ApplyDampedSuspensionForce ((fn_ApplyDampedSuspensionForce)0x004437C0)

/* UpdateSpecialTrafficEncounter -- encounter spawn/despawn/proximity checks */
typedef void (__cdecl *fn_UpdateSpecialTrafficEncounter)(void);
#define TD5_UpdateSpecialTrafficEncounter ((fn_UpdateSpecialTrafficEncounter)0x00434DA0)

/* AdvanceActorTrackScript -- bytecode interpreter (12 opcodes) for AI scripts */
typedef uint32_t (__cdecl *fn_AdvanceActorTrackScript)(int* actorRouteState);
#define TD5_AdvanceActorTrackScript ((fn_AdvanceActorTrackScript)0x004370A0)

/* InitializeRaceActorRuntime -- pre-race actor setup, difficulty scaling */
typedef void (__cdecl *fn_InitializeRaceActorRuntime)(void);
#define TD5_InitializeRaceActorRuntime ((fn_InitializeRaceActorRuntime)0x00432E60)

/* =======================================================================
 * Track & Collision (~15 functions)
 * ======================================================================= */

/* BindTrackStripRuntimePointers -- STRIP.DAT parser, sets span/vertex tables */
typedef void (__cdecl *fn_BindTrackStripRuntimePointers)(void);
#define TD5_BindTrackStripRuntimePointers ((fn_BindTrackStripRuntimePointers)0x00444070)

/* UpdateActorTrackPosition -- cross-product boundary tests, span transitions */
typedef void (__cdecl *fn_UpdateActorTrackPosition)(short* actor);
#define TD5_UpdateActorTrackPosition ((fn_UpdateActorTrackPosition)0x004440F0)

/* NormalizeActorTrackWrapState -- span modulo ring_length for circuit laps */
typedef int (__cdecl *fn_NormalizeActorTrackWrapState)(short* actor);
#define TD5_NormalizeActorTrackWrapState ((fn_NormalizeActorTrackWrapState)0x00443FB0)

/* ComputeActorTrackContactNormal -- master contact-normal resolver for segment */
typedef void (__cdecl *fn_ComputeActorTrackContactNormal)(short* segState, int* worldPos, int* outHeight);
#define TD5_ComputeActorTrackContactNormal ((fn_ComputeActorTrackContactNormal)0x00445450)

/* ComputeTrackTriangleBarycentrics -- barycentric height interpolation on triangle */
typedef int (__cdecl *fn_ComputeTrackTriangleBarycentrics)(short v1, short v2, short v3, int* worldPos);
#define TD5_ComputeTrackTriangleBarycentrics ((fn_ComputeTrackTriangleBarycentrics)0x004456D0)

/* ApplyTrackSurfaceForceToActor -- V2W wall impulse + friction response */
typedef void (__cdecl *fn_ApplyTrackSurfaceForceToActor)(int actorPtr, int* param2, uint32_t heading, int depth, uint32_t param5);
#define TD5_ApplyTrackSurfaceForceToActor ((fn_ApplyTrackSurfaceForceToActor)0x00406980)

/* ApplySimpleTrackSurfaceForce -- simplified wall-bounce for traffic/AI */
typedef void (__cdecl *fn_ApplySimpleTrackSurfaceForce)(int actorPtr, uint32_t heading, int depth);
#define TD5_ApplySimpleTrackSurfaceForce ((fn_ApplySimpleTrackSurfaceForce)0x00407270)

/* CollectVehicleCollisionContacts -- 8-corner OBB narrowphase test */
typedef uint32_t (__cdecl *fn_CollectVehicleCollisionContacts)(int actorA, int actorB, int* param3, int* param4, short* param5);
#define TD5_CollectVehicleCollisionContacts ((fn_CollectVehicleCollisionContacts)0x00408570)

/* ResolveVehicleCollisionPair -- broadphase AABB + binary-search TOI + impulse */
typedef void (__cdecl *fn_ResolveVehicleCollisionPair)(int actorA, int actorB);
#define TD5_ResolveVehicleCollisionPair ((fn_ResolveVehicleCollisionPair)0x00408A60)

/* ComputeActorWorldBoundingVolume -- ground/terrain collision for scripted vehicles */
typedef void (__cdecl *fn_ComputeActorWorldBoundingVolume)(int actorPtr);
#define TD5_ComputeActorWorldBoundingVolume ((fn_ComputeActorWorldBoundingVolume)0x004096B0)

/* RemapCheckpointOrderForTrackDirection -- CHECKPT.NUM swap for reverse tracks */
typedef void (__cdecl *fn_RemapCheckpointOrderForTrackDirection)(void);
#define TD5_RemapCheckpointOrderForTrackDirection ((fn_RemapCheckpointOrderForTrackDirection)0x0042FD70)

/* ComputeActorTrackHeading -- heading angle from span geometry (12-bit angle) */
typedef int (__cdecl *fn_ComputeActorTrackHeading)(uint32_t spanIdx);
#define TD5_ComputeActorTrackHeading ((fn_ComputeActorTrackHeading)0x00435CE0)

/* ComputeActorHeadingFromTrackSegment -- heading from segment vertex pairs */
typedef void (__cdecl *fn_ComputeActorHeadingFromTrackSegment)(short* segState, int* param2, uint16_t* outAngle);
#define TD5_ComputeActorHeadingFromTrackSegment ((fn_ComputeActorHeadingFromTrackSegment)0x00445B90)

/* InitActorTrackSegmentPlacement -- places actor at quad centroid */
typedef void (__cdecl *fn_InitActorTrackSegmentPlacement)(short* segState, int* outPos);
#define TD5_InitActorTrackSegmentPlacement ((fn_InitActorTrackSegmentPlacement)0x00445F10)

/* GetTrackSegmentSurfaceType -- reads surface type from span attribute byte */
typedef uint8_t (__cdecl *fn_GetTrackSegmentSurfaceType)(short* segState);
#define TD5_GetTrackSegmentSurfaceType ((fn_GetTrackSegmentSurfaceType)0x0042F100)

/* =======================================================================
 * Rendering (~14 functions)
 * ======================================================================= */

/* InitializeRaceRenderState -- per-race-start 3D pipeline init */
typedef uint32_t (__cdecl *fn_InitializeRaceRenderState)(void);
#define TD5_InitializeRaceRenderState ((fn_InitializeRaceRenderState)0x0040AE80)

/* ReleaseRaceRenderResources -- teardown at race end */
typedef void (__cdecl *fn_ReleaseRaceRenderResources)(void);
#define TD5_ReleaseRaceRenderResources ((fn_ReleaseRaceRenderResources)0x0040AEC0)

/* BeginRaceScene -- D3D BeginScene + reset texture sort keys */
typedef void (__cdecl *fn_BeginRaceScene)(void);
#define TD5_BeginRaceScene ((fn_BeginRaceScene)0x0040ADE0)

/* EndRaceScene -- D3D EndScene + texture age advancement */
typedef void (__cdecl *fn_EndRaceScene)(void);
#define TD5_EndRaceScene ((fn_EndRaceScene)0x0040AE00)

/* RenderRaceActorsForView -- master per-view actor renderer */
typedef void (__cdecl *fn_RenderRaceActorsForView)(int viewIndex);
#define TD5_RenderRaceActorsForView ((fn_RenderRaceActorsForView)0x0040BD20)

/* ConfigureRaceFogColorAndMode -- set fog color/enable from track data */
typedef void (__cdecl *fn_ConfigureRaceFogColorAndMode)(uint32_t color, int enable);
#define TD5_ConfigureRaceFogColorAndMode ((fn_ConfigureRaceFogColorAndMode)0x0040AF10)

/* ApplyRaceFogRenderState -- applies D3D fog render states (0=off, 1=on) */
typedef void (__cdecl *fn_ApplyRaceFogRenderState)(int enable);
#define TD5_ApplyRaceFogRenderState ((fn_ApplyRaceFogRenderState)0x0040AF50)

/* SetRaceRenderStatePreset -- 4 presets: filter + alpha combinations */
typedef void (__cdecl *fn_SetRaceRenderStatePreset)(int preset);
#define TD5_SetRaceRenderStatePreset ((fn_SetRaceRenderStatePreset)0x0040B070)

/* ParseModelsDat -- parses track mesh, gets span ring length */
typedef void (__cdecl *fn_ParseModelsDat)(void);
#define TD5_ParseModelsDat ((fn_ParseModelsDat)0x00431190)

/* FlushImmediateDrawPrimitiveBatch -- submits vertex buffer to D3D */
typedef void (__cdecl *fn_FlushImmediateDrawPrimitiveBatch)(void);
#define TD5_FlushImmediateDrawPrimitiveBatch ((fn_FlushImmediateDrawPrimitiveBatch)0x004329E0)

/* LoadRenderRotationMatrix -- copies 3x3 rotation into current transform */
typedef void (__cdecl *fn_LoadRenderRotationMatrix)(void* srcMatrix);
#define TD5_LoadRenderRotationMatrix ((fn_LoadRenderRotationMatrix)0x0043DA80)

/* LoadRenderTranslation -- copies translation into current transform */
typedef void (__cdecl *fn_LoadRenderTranslation)(int srcPtr);
#define TD5_LoadRenderTranslation ((fn_LoadRenderTranslation)0x0043DC20)

/* PushRenderTransform -- saves current 3x4 transform to backup slot */
typedef void (__cdecl *fn_PushRenderTransform)(void);
#define TD5_PushRenderTransform ((fn_PushRenderTransform)0x0043DAF0)

/* PopRenderTransform -- restores 3x4 transform from backup slot */
typedef void (__cdecl *fn_PopRenderTransform)(void);
#define TD5_PopRenderTransform ((fn_PopRenderTransform)0x0043DB70)

/* =======================================================================
 * Particles & Weather (~8 functions)
 * ======================================================================= */

/* InitializeRaceParticleSystem -- inits RAINSPL + SMOKE sprites, clears slots */
typedef void (__cdecl *fn_InitializeRaceParticleSystem)(void);
#define TD5_InitializeRaceParticleSystem ((fn_InitializeRaceParticleSystem)0x00429510)

/* UpdateRaceParticleEffects -- unified callback-driven particle pool dispatcher */
typedef void (__cdecl *fn_UpdateRaceParticleEffects)(int viewIndex);
#define TD5_UpdateRaceParticleEffects ((fn_UpdateRaceParticleEffects)0x00429790)

/* SpawnVehicleSmokeVariant -- direct spawn at position with variant type */
typedef void (__cdecl *fn_SpawnVehicleSmokeVariant)(int viewIndex, void* position, uint8_t variant, int actorPtr);
#define TD5_SpawnVehicleSmokeVariant ((fn_SpawnVehicleSmokeVariant)0x00429A30)

/* InitializeWeatherOverlayParticles -- allocates weather particle buffers */
typedef int (__cdecl *fn_InitializeWeatherOverlayParticles)(void);
#define TD5_InitializeWeatherOverlayParticles ((fn_InitializeWeatherOverlayParticles)0x00446240)

/* UpdateAmbientParticleDensityForSegment -- per-tick density ramp from track segment */
typedef void (__cdecl *fn_UpdateAmbientParticleDensityForSegment)(int actorPtr, int viewIndex);
#define TD5_UpdateAmbientParticleDensityForSegment ((fn_UpdateAmbientParticleDensityForSegment)0x004464B0)

/* AcquireTireTrackEmitter -- allocates pooled tire-track emitter slot */
typedef int (__cdecl *fn_AcquireTireTrackEmitter)(void);
#define TD5_AcquireTireTrackEmitter ((fn_AcquireTireTrackEmitter)0x0043F030)

/* UpdateTireTrackEmitters -- dispatches tire-track updates by drivetrain layout */
typedef void (__cdecl *fn_UpdateTireTrackEmitters)(int actorPtr);
#define TD5_UpdateTireTrackEmitters ((fn_UpdateTireTrackEmitters)0x0043FAE0)

/* AdvanceGlobalSkyRotation -- increments sky dome rotation by 0x400/tick */
typedef void (__cdecl *fn_AdvanceGlobalSkyRotation)(void);
#define TD5_AdvanceGlobalSkyRotation ((fn_AdvanceGlobalSkyRotation)0x0043D7C0)

/* =======================================================================
 * Sound (~6 functions)
 * ======================================================================= */

/* UpdateVehicleAudioMix -- per-frame engine/ambient spatial audio mixer */
typedef void (__cdecl *fn_UpdateVehicleAudioMix)(void);
#define TD5_UpdateVehicleAudioMix ((fn_UpdateVehicleAudioMix)0x00440E00)

/* LoadVehicleSoundBank -- loads Drive/Rev/Horn WAVs from car ZIP */
typedef uint32_t (__cdecl *fn_LoadVehicleSoundBank)(const char* archiveName, int vehicleIndex, char* reverbFlag);
#define TD5_LoadVehicleSoundBank ((fn_LoadVehicleSoundBank)0x00441A80)

/* LoadRaceAmbientSoundBuffers -- loads 19 ambient WAVs from SOUND.ZIP */
typedef uint32_t (__cdecl *fn_LoadRaceAmbientSoundBuffers)(void);
#define TD5_LoadRaceAmbientSoundBuffers ((fn_LoadRaceAmbientSoundBuffers)0x00441C60)

/* LoadFrontendSoundEffects -- loads 10 SFX from Front End\Sounds\Sounds.zip */
typedef void (__cdecl *fn_LoadFrontendSoundEffects)(void);
#define TD5_LoadFrontendSoundEffects ((fn_LoadFrontendSoundEffects)0x00414640)

/* PlayVehicleSoundAtPosition -- spatial one-shot with random variant selection */
typedef int (__cdecl *fn_PlayVehicleSoundAtPosition)(int slot, int volume, int frequency, int* position, int pan);
#define TD5_PlayVehicleSoundAtPosition ((fn_PlayVehicleSoundAtPosition)0x00441D90)

/* CreateRaceForceFeedbackEffects -- installs 3 FF effects per joystick */
typedef void (__cdecl *fn_CreateRaceForceFeedbackEffects)(void);
#define TD5_CreateRaceForceFeedbackEffects ((fn_CreateRaceForceFeedbackEffects)0x004285B0)

/* =======================================================================
 * Frontend (~12 functions)
 * ======================================================================= */

/* ScreenMainMenuAnd1PRaceFlow -- main menu + single-player race flow */
typedef void (__cdecl *fn_ScreenMainMenuAnd1PRaceFlow)(void);
#define TD5_ScreenMainMenuAnd1PRaceFlow ((fn_ScreenMainMenuAnd1PRaceFlow)0x00415490)

/* CarSelectionScreenStateMachine -- car selection screen */
typedef void (__cdecl *fn_CarSelectionScreenStateMachine)(void);
#define TD5_CarSelectionScreenStateMachine ((fn_CarSelectionScreenStateMachine)0x0040E020)

/* TrackSelectionScreenStateMachine -- track selection screen */
typedef void (__cdecl *fn_TrackSelectionScreenStateMachine)(void);
#define TD5_TrackSelectionScreenStateMachine ((fn_TrackSelectionScreenStateMachine)0x00427630)

/* RunRaceResultsScreen -- 22-state post-race frontend FSM */
typedef void (__cdecl *fn_RunRaceResultsScreen)(void);
#define TD5_RunRaceResultsScreen ((fn_RunRaceResultsScreen)0x00422480)

/* ScreenOptionsHub -- options hub screen (cheat input monitored here) */
typedef void (__cdecl *fn_ScreenOptionsHub)(void);
#define TD5_ScreenOptionsHub ((fn_ScreenOptionsHub)0x0041D890)

/* BltColorFillToSurface -- draws solid rect onto frontend surface */
typedef uint32_t (__cdecl *fn_BltColorFillToSurface)(uint32_t color, int x, int y, int w, int h, int* surface);
#define TD5_BltColorFillToSurface ((fn_BltColorFillToSurface)0x00424050)

/* ClearBackbufferWithColor -- fills backbuffer with solid color */
typedef void (__cdecl *fn_ClearBackbufferWithColor)(uint32_t color);
#define TD5_ClearBackbufferWithColor ((fn_ClearBackbufferWithColor)0x00423DB0)

/* DrawFrontendFontStringPrimary -- draws string with 12x12 font atlas */
typedef int (__cdecl *fn_DrawFrontendFontStringPrimary)(uint8_t* str, uint32_t x, int y);
#define TD5_DrawFrontendFontStringPrimary ((fn_DrawFrontendFontStringPrimary)0x00424110)

/* LoadFrontendTgaSurfaceFromArchive -- loads TGA from ZIP into frontend surface */
typedef void (__cdecl *fn_LoadFrontendTgaSurfaceFromArchive)(void);
#define TD5_LoadFrontendTgaSurfaceFromArchive ((fn_LoadFrontendTgaSurfaceFromArchive)0x00412030)

/* ScreenLocalizationInit -- first startup screen; reads configs, inits HW */
typedef void (__cdecl *fn_ScreenLocalizationInit)(void);
#define TD5_ScreenLocalizationInit ((fn_ScreenLocalizationInit)0x004269D0)

/* ShowLegalScreens -- displays legal/loading TGA screens at startup */
typedef void (__cdecl *fn_ShowLegalScreens)(void);
#define TD5_ShowLegalScreens ((fn_ShowLegalScreens)0x0042C8E0)

/* CrossFade16BitSurfaces -- MMX-accelerated weighted surface blend */
typedef void (__cdecl *fn_CrossFade16BitSurfaces)(uint32_t weight, int src1, int src2, uint32_t blendMode, int dst);
#define TD5_CrossFade16BitSurfaces ((fn_CrossFade16BitSurfaces)0x0040CDC0)

/* =======================================================================
 * Race Progression (~8 functions)
 * ======================================================================= */

/* BuildRaceResultsTable -- post-finish scoring, AI time estimation */
typedef void (__cdecl *fn_BuildRaceResultsTable)(void);
#define TD5_BuildRaceResultsTable ((fn_BuildRaceResultsTable)0x0040A8C0)

/* AwardCupCompletionUnlocks -- car/track unlock on 1st place cup win */
typedef void (__cdecl *fn_AwardCupCompletionUnlocks)(void);
#define TD5_AwardCupCompletionUnlocks ((fn_AwardCupCompletionUnlocks)0x00421DA0)

/* ConfigureGameTypeFlags -- maps game type to runtime mode flags */
typedef void (__cdecl *fn_ConfigureGameTypeFlags)(void);
#define TD5_ConfigureGameTypeFlags ((fn_ConfigureGameTypeFlags)0x00410CA0)

/* InitializeRaceSeriesSchedule -- generates race schedule for cup */
typedef void (__cdecl *fn_InitializeRaceSeriesSchedule)(void);
#define TD5_InitializeRaceSeriesSchedule ((fn_InitializeRaceSeriesSchedule)0x0040DAC0)

/* AdjustCheckpointTimersByDifficulty -- scales checkpoint time limits */
typedef void (__cdecl *fn_AdjustCheckpointTimersByDifficulty)(int actorPtr);
#define TD5_AdjustCheckpointTimersByDifficulty ((fn_AdjustCheckpointTimersByDifficulty)0x0040A530)

/* SerializeRaceStatusSnapshot -- captures cup state into 12966-byte buffer */
typedef void (__cdecl *fn_SerializeRaceStatusSnapshot)(void);
#define TD5_SerializeRaceStatusSnapshot ((fn_SerializeRaceStatusSnapshot)0x00411120)

/* WriteCupData -- XOR-encrypted mid-cup snapshot to CupData.td5 */
typedef void (__cdecl *fn_WriteCupData)(void);
#define TD5_WriteCupData ((fn_WriteCupData)0x004114F0)

/* IsLocalRaceParticipantSlot -- checks if slot is local player */
typedef uint32_t (__cdecl *fn_IsLocalRaceParticipantSlot)(int slot);
#define TD5_IsLocalRaceParticipantSlot ((fn_IsLocalRaceParticipantSlot)0x0042CBE0)

/* =======================================================================
 * Archive / Asset Loading (~3 functions)
 * ======================================================================= */

/* ReadArchiveEntry -- reads + decompresses a file from a ZIP into memory */
typedef uint32_t (__cdecl *fn_ReadArchiveEntry)(const char* entryName, const char* zipPath, char* destBuf, uint32_t maxSize);
#define TD5_ReadArchiveEntry ((fn_ReadArchiveEntry)0x00440790)

/* GetArchiveEntrySize -- gets uncompressed size of a file inside a ZIP */
typedef uint32_t (__cdecl *fn_GetArchiveEntrySize)(const char* entryName, const char* zipPath);
#define TD5_GetArchiveEntrySize ((fn_GetArchiveEntrySize)0x004409B0)

/* OpenArchiveFileForRead -- opens a ZIP and returns file buffer pointer */
typedef char* (__cdecl *fn_OpenArchiveFileForRead)(const char* entryName, const char* zipPath);
#define TD5_OpenArchiveFileForRead ((fn_OpenArchiveFileForRead)0x00440860)

/* =======================================================================
 * Math (~12 functions)
 * ======================================================================= */

/* BuildSinCosLookupTables -- generates 5120-entry float + fixed cos/sin tables */
typedef void (__cdecl *fn_BuildSinCosLookupTables)(void);
#define TD5_BuildSinCosLookupTables ((fn_BuildSinCosLookupTables)0x0040A650)

/* CosFloat12bit -- float cosine from 12-bit angle lookup */
typedef double (__cdecl *fn_CosFloat12bit)(uint32_t angle);
#define TD5_CosFloat12bit ((fn_CosFloat12bit)0x0040A6A0)

/* SinFloat12bit -- float sine from 12-bit angle lookup (cos shifted -0x400) */
typedef double (__cdecl *fn_SinFloat12bit)(int angle);
#define TD5_SinFloat12bit ((fn_SinFloat12bit)0x0040A6C0)

/* CosFixed12bit -- Q12 fixed-point cosine from 12-bit angle */
typedef int32_t (__cdecl *fn_CosFixed12bit)(uint32_t angle);
#define TD5_CosFixed12bit ((fn_CosFixed12bit)0x0040A6E0)

/* SinFixed12bit -- Q12 fixed-point sine from 12-bit angle */
typedef int32_t (__cdecl *fn_SinFixed12bit)(int angle);
#define TD5_SinFixed12bit ((fn_SinFixed12bit)0x0040A700)

/* AngleFromVector12 -- atan2 equivalent, returns 12-bit angle from 2D vector */
typedef int (__cdecl *fn_AngleFromVector12)(int x, int z);
#define TD5_AngleFromVector12 ((fn_AngleFromVector12)0x0040A720)

/* MultiplyRotationMatrices3x3 -- standard 3x3 matrix multiply: out = A * B */
typedef void (__cdecl *fn_MultiplyRotationMatrices3x3)(float* A, float* B, float* out);
#define TD5_MultiplyRotationMatrices3x3 ((fn_MultiplyRotationMatrices3x3)0x0042DA10)

/* TransposeMatrix3x3 -- dst[row][col] = src[col][row] (NOT in-place safe) */
typedef void (__cdecl *fn_TransposeMatrix3x3)(uint32_t* src, uint32_t* dst);
#define TD5_TransposeMatrix3x3 ((fn_TransposeMatrix3x3)0x0042E710)

/* BuildRotationMatrixFromAngles -- builds 3x3 from Euler angles {pitch,yaw,roll} */
typedef void (__cdecl *fn_BuildRotationMatrixFromAngles)(float* out, short* angles);
#define TD5_BuildRotationMatrixFromAngles ((fn_BuildRotationMatrixFromAngles)0x0042E1E0)

/* ExtractEulerAnglesFromMatrix -- extracts Euler angles from 3x3 float matrix */
typedef void (__cdecl *fn_ExtractEulerAnglesFromMatrix)(int matPtr, uint16_t* angles);
#define TD5_ExtractEulerAnglesFromMatrix ((fn_ExtractEulerAnglesFromMatrix)0x0042E030)

/* CrossProduct3i -- integer cross product: out = a x b */
typedef void (__cdecl *fn_CrossProduct3i)(int* a, int* b, int* out);
#define TD5_CrossProduct3i ((fn_CrossProduct3i)0x0042EA70)

/* TransformVector3ByBasis -- matrix-vector multiply (rotation only, no translation) */
typedef void (__cdecl *fn_TransformVector3ByBasis)(float* matrix, float* vec, float* out);
#define TD5_TransformVector3ByBasis ((fn_TransformVector3ByBasis)0x0042DBD0)

/* =======================================================================
 * Debug / Benchmark (~3 functions)
 * ======================================================================= */

/* WriteBenchmarkResultsTgaReport -- generates 640x480 TGA with system info + FPS graph */
typedef void (__cdecl *fn_WriteBenchmarkResultsTgaReport)(void);
#define TD5_WriteBenchmarkResultsTgaReport ((fn_WriteBenchmarkResultsTgaReport)0x00428D80)

/* RecordBenchmarkFrameRateSample -- stores per-frame FPS to capture buffer */
typedef void (__cdecl *fn_RecordBenchmarkFrameRateSample)(int fpsValue);
#define TD5_RecordBenchmarkFrameRateSample ((fn_RecordBenchmarkFrameRateSample)0x00428D40)

/* ScreenPositionerDebugTool -- developer debug tool (frontend screen entry 1) */
typedef void (__cdecl *fn_ScreenPositionerDebugTool)(void);
#define TD5_ScreenPositionerDebugTool ((fn_ScreenPositionerDebugTool)0x00415030)

/* =======================================================================
 * Additional Globals
 * ======================================================================= */

/* Track geometry */
#define g_trackSpanRingLength           (*(int32_t*)0x004C3D90)   /* Total span count (ring length) */
#define g_surfaceFrictionTable          ((int16_t*)0x004748C0)    /* Friction coefficient per surface type */

/* Render transform */
#define g_currentRenderTransform        (*(float**)0x004BF6B8)    /* 3x4 float matrix (8-byte aligned) */

/* Encounter system */
#define g_specialEncounterTrackedActorHandle (*(int32_t*)0x004B0630)  /* -1 = none, else target actor idx */
#define g_specialEncounterCooldown      (*(int32_t*)0x004B064C)   /* Countdown from 300; must be 0 to spawn */

/* Weather / sky */
#define g_weatherActiveCountView0       (*(int32_t*)0x004C3DE0)   /* Active particle count, view 0 */
#define g_skyRotationAngle              (*(int32_t*)0x004BF500)   /* Fixed-point sky dome rotation */

/* Car / progression */
#define g_selectedCarIndex              (*(int32_t*)0x0048F364)   /* Current car selection index */
#define g_raceDifficultyTier            (*(int32_t*)0x00466010)   /* 0=easy, 1=medium, 2=hard */
#define g_cupUnlockLevel                (*(int32_t*)0x004962A8)   /* Cup tier progression (0-2) */

/* Math tables */
#define g_sinCosFloatTable              ((float*)0x00488984)      /* 5120-entry float cosine table */

#ifdef __cplusplus
}
#endif

#endif /* TD5_SDK_H */
