# TD5_d3d.exe -- Global Variable Catalog

> Comprehensive catalog of all significant global variables, data tables, and function pointer
> dispatch tables in the TD5 Direct3D executable.
>
> Generated: 2026-03-28 | Binary: TD5_d3d.exe | Image base: 0x00400000

---

## 1. Memory Map

| Segment    | Start      | End        | Size (bytes) | Perms | Description |
|------------|------------|------------|--------------|-------|-------------|
| Headers    | 0x00400000 | 0x00400FFF |        4,096 | R--   | PE headers |
| .text      | 0x00401000 | 0x0045CFFF |      376,832 | R-X   | Executable code |
| .rdata     | 0x0045D000 | 0x00462FFF |       24,576 | R--   | Read-only data (strings, import tables, float constants) |
| .data      | 0x00463000 | 0x004D0D2B |      449,836 | RW-   | Initialized + uninitialized global data (includes BSS) |
| IDCT_DAT   | 0x004D1000 | 0x004D2FFF |        8,192 | RW-   | IDCT coefficient tables (TGQ video decoder) |
| UVA_DATA   | 0x004D3000 | 0x004D7FFF |       20,480 | RW-   | UV animation / texture coordinate data |
| .rsrc      | 0x004D8000 | 0x004D9FFF |        8,192 | R--   | PE resources (icon, version info) |

### .data Segment Internal Layout (estimated)

| Range              | Approximate Size | Content |
|--------------------|------------------|---------|
| 0x463000-0x463B00  | ~2,816           | Initialized constants: camera presets, particle defs, fog strings |
| 0x463B00-0x46CF00  | ~37,632          | Static data tables: screen assets, NPC names, car/track strings, level tables |
| 0x46CF00-0x474F00  | ~32,768          | Per-track checkpoint data, environment tables, surface types, AI constants |
| 0x474F00-0x482000  | ~28,928          | DirectDraw error tables, sound file paths, HUD strings |
| 0x482000-0x49A000  | ~98,304          | Large runtime state: camera state, save data, input config, race state |
| 0x49A000-0x4C4000  | ~172,032         | Actor arrays, viewport layouts, render transforms, display lists |
| 0x4C4000-0x4D0D2B  | ~52,524          | Texture cache, render buffers, misc runtime |

---

## 2. Named Globals by Subsystem

### 2.1 Game State Machine

| Address    | Type     | Name | Description | Xrefs |
|------------|----------|------|-------------|-------|
| 0x4C3CE8   | dword    | `g_gameState` | Top-level game state (0=frontend, 1=loading, 2=racing, 3=results) | R:0x442170, W:0x4421ED/0x44226D/0x4422D6/0x44242B/0x442446/0x44253C |
| 0x473B6C   | dword    | `g_dxConfirmGate` | DX6 confirmation gate (non-zero = display mode recovery path) | Main loop condition |
| 0x4C3D04   | dword    | `g_textureQualityFlag` | Texture quality (affects mip selection) | 3D asset loader |
| 0x4C3D44   | dword    | `g_splitscreenPlayerCount` | Active player count (1 or 2) | Race init, model render |

### 2.2 Frontend Screen System

| Address    | Type     | Name | Description | Xrefs |
|------------|----------|------|-------------|-------|
| 0x4655C4   | ptr[30]  | `g_frontendScreenFnTable` | Screen function dispatch table (30 entries) | R:0x415447, DATA:0x414620 |
| 0x4654A0   | dword    | `g_frontendButtonCount` | Active button count for current screen | Car/track selection |
| 0x4962A0   | dword    | `g_twoPlayerState` | 2P mode: 0=1P, 1=P1 selecting, 2=P2 selecting | Throughout frontend |
| 0x4962D4   | dword    | `g_flowContext` | Flow origin: 1=RaceMenu, 2=QuickRace, 3=2P, 4=NetPlay | Screen transitions |
| 0x49635C   | dword    | `g_selectedGameType` | Active game type (0=Single, 1-6=Cup, 7=TimeTrial, 8=CopChase, 9=Drag) | Race init |
| 0x496298   | dword    | `g_cheatUnlockFlag` | Cheat mode / network flag (bypasses lock checks when nonzero) | Unlock system |
| 0x4962A8   | dword    | `g_cupUnlockTier` | Cup progression tier (0-2+) | Cup race flow |
| 0x4962BC   | dword    | `g_networkConnectionState` | DirectPlay connection state | Network flow |
| 0x4962B0   | dword    | `g_secretUnlockFlag` | Set by winning cup 18 with easter egg car | Cup progression |
| 0x4A2C8C   | dword    | `g_postRaceReturnCode` | Post-race screen selector (2 = high score entry) | ScreenLocalizationInit |

### 2.3 Car and Track Selection

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x48F31C   | dword      | `g_selectedCarIndex` | Currently selected car index (0-36) |
| 0x48F308   | dword      | `g_selectedPaintJob` | Paint scheme (0-3) |
| 0x48F334   | dword      | `g_selectedConfigPreset` | Car config preset (0-3) |
| 0x48F338   | dword      | `g_selectedTransmission` | 0=auto, 1=manual |
| 0x48F360   | dword      | `g_carPreviewMode` | 0=car image, 1=config sheet, 2=info sheet |
| 0x4A2C90   | dword      | `g_selectedTrackScheduleIndex` | Track schedule selection index |
| 0x4A2C98   | dword      | `g_trackDirection` | 0=forwards, 1=backwards |
| 0x497268   | dword[6]   | `g_slotCarIds` | Per-slot car ID assignment (set in frontend) |
| 0x463E0C   | dword      | `g_totalCarCount` | Total available cars (default 23) |
| 0x466840   | dword      | `g_totalTrackCount` | Total available tracks (default 16) |
| 0x463E4C   | byte[37]   | `g_carLockTable` | Per-car lock state (0=unlocked, 1=locked) |
| 0x4668B0   | byte[26]   | `g_trackLockTable` | Per-track lock state (0=unlocked, 1=locked) |

### 2.4 Game Options (Config.td5 persistent)

| Address    | Type   | Name | Range | Description |
|------------|--------|------|-------|-------------|
| 0x466000   | dword  | `g_circuitLaps` | 0-3 | Number of laps for circuit races |
| 0x466004   | dword  | `g_checkpointTimers` | 0-1 | On/off |
| 0x466008   | dword  | `g_trafficEnabled` | 0-1 | On/off |
| 0x46600C   | dword  | `g_copsEnabled` | 0-1 | Encounters on/off |
| 0x466010   | dword  | `g_difficulty` | 0-2 | Easy/Medium/Hard |
| 0x466014   | dword  | `g_dynamics` | 0-1 | Arcade/Simulation |
| 0x466018   | dword  | `g_3dCollisions` | 0-1 | On/off |
| 0x466020   | dword  | `g_displayModeOrdinal` | - | M2DX display mode index |
| 0x466024   | dword  | `g_foggingEnabled` | 0-1 | On/off |
| 0x466028   | dword  | `g_speedReadout` | 0-1 | 0=MPH, 1=KPH |
| 0x46602C   | dword  | `g_cameraDamping` | 0-9 | Chase cam smoothing factor |
| 0x465FE8   | dword  | `g_sfxMode` | 0-2 | Mono/Stereo/3D |
| 0x465FEC   | dword  | `g_sfxVolume` | 0-100 | Percentage |
| 0x465FF0   | dword  | `g_musicVolume` | 0-100 | Percentage |
| 0x465FF4   | dword  | `g_p2InputSource` | 0-7 | P2 device slot index |
| 0x465FF8   | byte   | `g_2pCatchupAssist` | 0-9 | 2P catch-up assist level |
| 0x497A58   | dword  | `g_p1InputSource` | 0-7 | P1 device slot index |
| 0x497A5C   | byte   | `g_splitScreenMode` | 0-1 | 0=Horizontal, 1=Vertical |

### 2.5 Input and Controllers

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x463FC4   | dword[18]  | `g_controllerBindingTables` | Key/button mappings (9 actions x 2 players) |
| 0x464054   | dword      | `g_ffConfigA` | Force feedback config dword A |
| 0x464058   | dword      | `g_ffConfigC` | Force feedback config dword C |
| 0x46405C   | dword      | `g_ffConfigB` | Force feedback config dword B |
| 0x464060   | dword      | `g_ffConfigD` | Force feedback config dword D |
| 0x465660   | byte[32]   | `g_p1DeviceDescriptor` | P1 active controller device descriptor |
| 0x465680   | byte[32]   | `g_p2DeviceDescriptor` | P2 active controller device descriptor |
| 0x4978C0   | byte[392]  | `g_p1CustomBindingMap` | P1 controller custom binding map (0x188 bytes) |
| 0x497330   | byte[392]  | `g_p2CustomBindingMap` | P2 controller custom binding map (0x188 bytes) |

### 2.6 AI and Rubber-Banding

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x473D9C   | dword      | `g_rbBehindScale` | Catch-up intensity when AI trails player |
| 0x473DA0   | dword      | `g_rbAheadScale` | Slow-down intensity when AI leads player |
| 0x473DA4   | dword      | `g_rbBehindRange` | Max span distance (behind) for full effect |
| 0x473DA8   | dword      | `g_rbAheadRange` | Max span distance (ahead) for full effect |
| 0x473D64   | dword[6+]  | `g_defaultThrottleTable` | Per-slot baseline throttle (race-init constant) |
| 0x473D2C   | dword[6+]  | `g_liveThrottleTable` | Copied from default each tick, then rubber-band modified |
| 0x473DB0   | byte[128]  | `g_aiPhysicsTemplate` | AI physics template copied per AI slot at race init |
| 0x46317C   | short      | `g_p1SteeringWeight` | Player 1 steering weight (increases when behind) |
| 0x46317E   | short      | `g_p2SteeringWeight` | Player 2 steering weight (increases when behind) |
| 0x48301C   | dword      | `g_maxSteeringWeightDelta` | Clamps maximum weight adjustment |
| 0x4B08B0   | dword      | `g_lateralAvoidanceDir` | AI lateral avoidance direction |
| 0x466F90   | byte[108]  | `g_perCarTorqueTriplets` | 9 cars x 12 bytes: torque_shape, top_speed, traction |
| 0x474900   | short[32]  | `g_surfaceFriction` | Per surface type friction values |
| 0x4748C0   | short[32]  | `g_surfaceGrip` | Per-wheel surface grip multiplier (player only) |

### 2.7 Track Geometry (STRIP.DAT runtime pointers)

| Address    | Type     | Name | Description |
|------------|----------|------|-------------|
| 0x4C3DA0   | ptr      | `g_stripDatHeader` | Raw STRIP.DAT blob pointer |
| 0x4C3D9C   | ptr      | `g_spanRecordTable` | Span record table (24 bytes/record) |
| 0x4C3D98   | ptr      | `g_vertexCoordTable` | Vertex coordinate table (6 bytes/vertex = 3x short XYZ) |
| 0x4C3D90   | dword    | `g_totalSpanCount` | Total span count (ring length) |
| 0x4C3D94   | dword    | `g_auxiliaryCount` | Auxiliary geometry count |
| 0x4C3D8C   | dword    | `g_secondaryCount` | Secondary geometry count |
| 0x4631A0   | ptr      | `g_trackStripPtrA` | Track strip pointer A (LEFT.TRK) |
| 0x4631A4   | ptr      | `g_trackStripPtrB` | Track strip pointer B (RIGHT.TRK) |
| 0x473C6C   | ptr      | `g_trackStripPtrC` | Track strip pointer C (aux strip data) |

Cross-references for track strip pointers:
- 0x4631A0: DATA refs from 0x406E3D, 0x4075D0
- 0x4631A4: DATA refs from 0x406E5E, 0x4075F8
- 0x473C6C: DATA refs from 0x4345E4, 0x4346A3, 0x434764

### 2.8 Camera System

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x463098   | struct[6+] | `g_cameraPresetTable` | Camera preset parameters (see Section 4) |
| 0x463090   | dword      | `g_flyInThreshold` | Fly-in threshold frame count (40) |
| 0x4AAFA0   | float[9]   | `g_primaryCameraBasis` | Primary camera view rotation matrix |
| 0x4AAFC4   | float[3]   | `g_cameraWorldPos` | Camera world position (float) |
| 0x4AAFE0   | float[9]   | `g_cameraBasisScaled` | Working copy (scaled for projection) |
| 0x4AB040   | float[9]   | `g_tertiaryRotation` | Tertiary rotation matrix (projection path) |
| 0x4AB070   | float[9]   | `g_secondaryRotation` | Secondary rotation matrix (look-at construction) |
| 0x482E0C   | dword      | `g_cameraPitchAngle` | Pitch angle (20-bit, from preset) |
| 0x482E18   | short[3]   | `g_vehicleOrientAngles` | Cached vehicle orientation angles |
| 0x482EA0   | short[4]   | `g_cameraOffsetVector` | Camera offset vector (from preset) |
| 0x482EB0   | float      | `g_interpolatedHeightTarget` | Interpolated height target |
| 0x482EB8   | dword      | `g_heightSampleOffset` | Height sample offset |
| 0x482EC0   | dword      | `g_trackSpanOffsetProfile` | Track span offset (profile) |
| 0x482EC8   | dword      | `g_cameraBehaviorType` | Behavior type (profile) |
| 0x482ED0   | float      | `g_cameraDistance` | Distance (scaled from preset) |
| 0x482ED8   | int[3]     | `g_orbitOffsetVector` | Orbit offset vector |
| 0x482EF0   | dword      | `g_orbitBaseParam` | Smoothing/orbit base parameter |
| 0x482F00   | float      | `g_interpolatedDistance` | Interpolated distance |
| 0x482F08   | dword      | `g_splineParameter` | Spline parameter (0..0xFFF) |
| 0x482F10   | float      | `g_targetHeight` | Target height (from preset) |
| 0x482F18   | dword      | `g_headingAngle` | Heading angle (20-bit) |
| 0x482F20   | dword      | `g_trackSpanAnchor` | Track span index (profile anchor) |
| 0x482F30   | int[3]     | `g_cameraWorldPosInt` | Camera world position (integer) |
| 0x482F48   | byte       | `g_cameraUnknownByte` | Unknown camera-related byte (saved to config) |
| 0x482F50   | short[4]   | `g_transformedOffset` | Transformed offset (short) |
| 0x482F60   | dword      | `g_orbitDistAccum` | Orbit distance accumulator |
| 0x482F68   | dword      | `g_splineNodeCount` | Spline node count |
| 0x482F70   | dword      | `g_yawOffset` | Yaw offset (look-left/right) |
| 0x482F80   | dword      | `g_splineAdvanceRate` | Spline advance rate |
| 0x482FA0   | dword      | `g_storedPitchAngle` | Stored pitch angle |

### 2.9 Rendering and 3D Pipeline

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x4BF6B8   | ptr        | `g_currentRenderTransform` | Active 3x4 float matrix |
| 0x4C36C8   | float[12]  | `g_renderTransformBackup` | Push/pop backup slot |
| 0x48DA00   | dword      | `g_currentTexPageSortKey` | Current texture page sort key |
| 0x48DA04   | dword      | `g_prevTexPageSortKey` | Previous texture page sort key |
| 0x48DC3C   | ptr        | `g_texCacheControlBlock` | Texture cache control block pointer |
| 0x48DC40   | ptr        | `g_texCacheDescArray` | Texture cache descriptor array base |
| 0x48DC48   | float      | `g_billboardYOffset` | Y-offset for billboard/overlay quads |
| 0x48DBA0   | dword      | `g_raceRenderActive` | Race render state active flag (0/1) |
| 0x48DBA8   | byte       | `g_renderGlobalsInit` | Render globals initialized flag |
| 0x48DB90   | dword      | `g_savedFpuCtrlClean` | Saved FPU control word (clean) |
| 0x48DB94   | dword      | `g_savedFpuCtrlSingle` | Saved FPU control word (single-precision) |
| 0x4AEE68   | uint[1024] | `g_luminanceToARGB_LUT` | Luminance-to-ARGB color lookup table |
| 0x4AF26C   | dword      | `g_translucentBatchPoolBase` | Translucent batch linked-list pool base |
| 0x4AF270   | dword      | `g_translucentBatchCount` | Active translucent batch count |
| 0x4AF314   | ushort[]   | `g_immediateDrawIndexBuf` | Immediate draw index buffer |
| 0x4AFB14   | byte[]     | `g_immediateDrawVertexBuf` | Immediate draw vertex buffer |
| 0x4AFB4C   | dword      | `g_immediateDrawVtxCount` | Immediate draw vertex count |
| 0x4AFB50   | dword      | `g_immediateDrawIdxCount` | Immediate draw index count |
| 0x4AAE14   | struct[3]  | `g_viewportLayoutTable` | Viewport layout table (0x40B bytes/entry) |
| 0x473B70   | int[10]    | `g_mipLevelDimTable` | Mip level dimension table: 1,2,4,8,16,32,64,128,256,512,1024 |
| 0x4749D0   | float      | `g_worldToTexelScale` | World-to-texel coordinate scale |
| 0x463B70   | float      | `g_billboardScale` | Track-relative billboard scale constant |
| 0x463B6C   | float      | `g_billboardMidpointAvg` | Billboard midpoint averaging factor (0.5) |
| 0x4BF6C8   | ptr[]      | `g_depthSortBuckets` | 4096-bucket depth sort array for translucent primitives |
| 0x4AB0B0   | float      | `g_hFovTangent` | Horizontal half-angle tangent |
| 0x4AB0B8   | float      | `g_hDepthScale` | Horizontal depth scale |
| 0x4AB0A4   | float      | `g_vFovTangent` | Vertical half-angle tangent |
| 0x4AB0A8   | float      | `g_vDepthScale` | Vertical depth scale |
| 0x4AB0D0   | float[9]   | `g_lightCoefficients` | 3 directions x 3 components light model |
| 0x4BF6A8   | float      | `g_ambientOffset` | Ambient lighting offset |
| 0x48DC34   | dword      | `g_boundTextureSlot` | Currently bound texture slot (avoid re-bind) |
| 0x482DD0   | byte[]     | `g_actorBrightness` | Per-actor brightness byte array (0-255) |
| 0x4B11D4   | float      | `g_halfScreenWidth` | Half screen width (for centered overlay layout) |
| 0x4B1368   | dword      | `g_pauseMenuQuadCount` | Pause menu total quad count |

### 2.10 Race Runtime State

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x4AD288   | struct[]   | `g_actorRuntimeArray` | Actor runtime state array (per-slot, large struct ~0x1900 each) |
| 0x4A31AC   | byte[]     | `g_perViewActorState` | Per-view actor state base (param_1 * 0x1900 stride) |
| 0x4AACA0   | struct[]   | `g_raceSlotStates` | Per-slot race state (position, laps, etc.) |
| 0x4AAF58   | dword      | `g_benchmarkModeActive` | Benchmark mode flag |
| 0x4A2CF4   | ptr        | `g_benchmarkSampleBuffer` | Frame timing sample array |
| 0x4A2CF8   | dword      | `g_benchmarkSampleCount` | Number of recorded samples |
| 0x4A2CFC   | char[256]  | `g_benchmarkScratch` | FormatBenchmarkReportText output buffer |
| 0x4AAF40   | dword      | `g_benchmarkFirstFrame` | Skip first frame flag |
| 0x48F378   | dword      | `g_escapeKeyFlagP1` | P1 escape key pressed |
| 0x48F37C   | dword      | `g_escapeKeyFlagP2` | P2 escape key pressed |
| 0x4AAF98   | dword[]    | `g_inputCooldownArray` | Per-player input repeat cooldown |
| 0x497A64   | dword      | `g_lastResultsButton` | Last button choice in results screen |
| 0x497A6C   | dword      | `g_replayAvailable` | Non-zero if replay data exists |
| 0x497A70   | dword      | `g_cupComplete` | Set when no more cup races available |
| 0x497A74   | dword      | `g_skipResults` | Bypasses score display |
| 0x497A78   | dword      | `g_reRaceFlag` | "Race Again" snapshot restore state |
| 0x494BB0   | dword      | `g_unlockedCarInfo` | Non-zero if car was just unlocked |
| 0x494BB4   | dword      | `g_unlockedTrackInfo` | Non-zero if track was just unlocked |

### 2.11 Pause Menu and In-Race Overlay

| Address    | Type     | Name | Description |
|------------|----------|------|-------------|
| 0x474498   | int[8]   | `g_pauseThemeHeightTable` | Height values per pause menu theme |
| 0x4744B8   | ptr[]    | `g_pauseStringPtrTable` | Pause menu string/asset pointer table |
| 0x4B135C   | float    | `g_viewDistSlider` | View distance slider value |
| 0x4B1360   | float    | `g_musicVolumeFloat` | Music volume (0.0-1.0) |
| 0x4B1364   | float    | `g_sfxVolumeFloat` | SFX volume (0.0-1.0) |
| 0x474640   | dword    | `g_pauseMenuCursor` | Current menu cursor position (0-4) |
| 0x4BCB78   | dword    | `g_pauseRepeatGuard` | Held-key rapid scroll guard |
| 0x466EA8   | float    | `g_viewDistanceRaw` | Raw view distance value |
| 0x474638   | float    | `g_musicVolumeRaw` | Music volume float (writeback) |
| 0x47463C   | float    | `g_sfxVolumeRaw` | SFX volume float (writeback) |

### 2.12 Encounter / Cop Chase System

| Address    | Type   | Name | Description |
|------------|--------|------|-------------|
| 0x4B0630   | dword  | `g_encounterRefSlot` | Encounter reference slot index for heading comparison |
| 0x4B064C   | dword  | `g_encounterCooldown` | Cooldown timer (counts down from 300) |
| 0x4B05E4   | dword  | `g_encounterPhase` | 0 = acquisition phase, nonzero = tracking phase |
| 0x4B0658   | dword  | `g_encounterDespawnFlag` | Cleared on despawn |
| 0x4B05C4   | dword  | `g_encounterTeardownFlag` | Cleared on encounter teardown |
| 0x4B0568   | dword  | `g_encounterRouteSelector` | Required route table selector for encounter spawn |

### 2.13 Frontend Rendering / Music Test

| Address    | Type     | Name | Description |
|------------|----------|------|-------------|
| 0x465E14   | dword    | `g_musicTestTrackIndex` | Current music test track index (0-11) |
| 0x465E18   | dword    | `g_musicTestPlayingIndex` | Currently playing track index |
| 0x465E1C   | ptr[12]  | `g_bandNamePtrTable` | 12 pointers to band name strings |
| 0x465E58   | ptr[12]  | `g_trackNamePtrTable` | 12 pointers to track/song name strings |
| 0x4951DC   | dword    | `g_galleryMode` | 0=normal, 1=credits, 2=band photos |
| 0x48F300   | dword    | `g_galleryImageCount` | Number of loaded gallery images |
| 0x48F2FC   | dword    | `g_galleryCrossfade` | Crossfade position for gallery transitions |
| 0x496360   | dword    | `g_creditsTextIndex` | Current line in SNK_CreditsText |
| 0x496364   | dword    | `g_creditsSubLine` | Sub-line counter (0-6) for mugshot entries |
| 0x496354   | dword    | `g_creditsSectionCounter` | Counts `*` separators in credits (exits at 11) |

### 2.14 Debug / Positioner

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x4951F8   | dword      | `g_positionerInputBitmask` | Positioner input bitmask |
| 0x49521C   | dword      | `g_positionerSelectedRow` | Positioner selected row |
| 0x495260   | int[37x2]  | `g_positionerXOffsets` | Positioner character X positions |
| 0x495264   | int[37x2]  | `g_positionerYOffsets` | Positioner character Y positions |

### 2.15 3D Asset Management

| Address    | Type       | Name | Description |
|------------|------------|------|-------------|
| 0x4AEE54   | ptr        | `g_meshDataStart` | Pointer past end of MODELS.DAT entry table (mesh data start) |
| 0x4AED90   | ptr        | `g_stripDatAllocation` | STRIP.DAT raw blob allocation |

### 2.16 Viewport / Splitscreen

| Address    | Type     | Name | Description |
|------------|----------|------|-------------|
| 0x4B1134   | dword    | `g_viewCount` | Number of active viewports (1, 2, or 4) |
| 0x4B1138   | float    | `g_scaleX` | Horizontal scale factor |
| 0x4B113C   | float    | `g_scaleY` | Vertical scale factor |
| 0x4B1148   | rect     | `g_viewport0Rect` | View 0 bounds (left, top, right, bottom) |
| 0x4B1180   | rect     | `g_viewport1Rect` | View 1 bounds |
| 0x4AAE48   | struct   | `g_viewportLayoutConfig` | Per-layout view_count at mode*0x40 offset |
| 0x4AAC60   | ptr[]    | `g_spanVisibilityArray` | Track span visibility per-view (cull_window * viewIndex + spanIndex) |

---

## 3. Function Pointer Tables / Dispatch Tables

### 3.1 Frontend Screen Function Table (0x4655C4)

30-entry table indexed by `g_frontendScreenIndex`. Each entry is a function pointer to a screen state machine.

| Index | Address    | Function Name | Purpose |
|-------|------------|---------------|---------|
| 0     | 0x004269D0 | ScreenLocalizationInit | Bootstrap/localization init |
| 1     | 0x00415030 | ScreenPositionerDebugTool | Dev-only UI positioner |
| 2     | 0x004275A0 | RunAttractModeDemoScreen | Attract mode demo |
| 3     | 0x00427290 | ScreenLanguageSelect | Language flag selection |
| 4     | 0x004274A0 | ScreenLegalCopyright | Legal splash screen |
| 5     | 0x00415490 | ScreenMainMenuAnd1PRaceFlow | Main menu hub |
| 6     | 0x004168B0 | RaceTypeCategoryMenuStateMachine | Race type selection |
| 7     | 0x004213D0 | ScreenQuickRaceMenu | Quick race setup |
| 8     | 0x00418D50 | RunFrontendConnectionBrowser | Network connection browser |
| 9     | 0x00419CF0 | RunFrontendSessionPicker | Network session browser |
| 10    | 0x0041A7B0 | RunFrontendCreateSessionFlow | Session create/join |
| 11    | 0x0041C330 | RunFrontendLobby | Network lobby |
| 12    | 0x0041D890 | ScreenOptionsHub | Options menu hub |
| 13    | 0x0041F990 | ScreenControlOptions | Controller configuration |
| 14    | 0x0041DF20 | ScreenSoundOptions | Sound options |
| 15    | 0x0041EA90 | ScreenGameOptions | Game options |
| 16    | 0x00420400 | ScreenGraphicsOptions | Graphics/display options |
| 17    | 0x00420C70 | ScreenTwoPlayerOptions | 2P split-screen options |
| 18    | 0x0040FE00 | ScreenInfoDisplay | System info display |
| 19    | 0x00418460 | ScreenControllerDevicePicker | Controller device picker |
| 20    | 0x0040DFC0 | ScreenCarSelection | Car selection (generic) |
| 21    | 0x00427630 | ScreenTrackSelection | Track selection |
| 22    | 0x00417D50 | ScreenMusicTest | Music test / jukebox |
| 23    | 0x00413580 | ScreenHighScores | High score display |
| 24    | 0x00422480 | ScreenPostRaceResults | Post-race results |
| 25    | 0x00413BC0 | ScreenCreditsGallery | Credits / mugshots gallery |
| 26    | 0x004237F0 | ScreenCupWon | Cup won celebration |
| 27    | 0x00423A80 | ScreenBandPhotos | Band photo gallery |
| 28    | 0x00415370 | ScreenStartupInit | Initial startup bootstrap |
| 29    | 0x0041D630 | ScreenNoControllerError | No controller error screen |

### 3.2 Translucent Dispatch Table (0x473B9C)

7-entry function pointer table for translucent/alpha primitive rendering.

| Index | Address    | Function Name | Primitive Type |
|-------|------------|---------------|----------------|
| 0     | 0x00431750 | EmitTranslucentTriangleStrip | Triangle strip |
| 1     | 0x00431750 | EmitTranslucentTriangleStrip | (duplicate of 0) |
| 2     | 0x004316F0 | SubmitProjectedTrianglePrimitive | Single triangle |
| 3     | 0x00431690 | SubmitProjectedQuadPrimitive | Single quad |
| 4     | 0x0043E3B0 | InsertBillboardIntoDepthSortBuckets | Billboard |
| 5     | 0x00431730 | EmitTranslucentTriangleStripDirect | Direct tri strip |
| 6     | 0x004316D0 | EmitTranslucentQuadDirect | Direct quad |

Callers: 0x431380 (FlushQueuedTranslucentPrimitives), 0x4314D8 (RenderPreparedMeshResource), 0x4315BA (SubmitImmediateTranslucentPrimitive)

### 3.3 Mip Level Dimension Table (0x473B70)

11-entry power-of-two table used for texture mip level selection.

```
[0]=1, [1]=2, [2]=4, [3]=8, [4]=16, [5]=32, [6]=64, [7]=128, [8]=256, [9]=512, [10]=1024
```

### 3.4 Pause Menu Theme Height Table (0x474498)

8-entry table mapping theme index to overlay height:

```
[0]=256, [1]=320, [2]=298, [3]=268, [4]=320, [5]=301, [6]=327, [7]=332
```

---

## 4. Camera Preset Table (0x463098)

Structure: 6+ entries, 20 bytes each (10 x uint16_t). Encodes per-mode camera position parameters
(radius, height, pitch, FOV offsets). Accessed by `UpdateChaseCamera` via
`DAT_004014D2` and `DAT_0040151D`.

| Entry | Words (hex) | Notes |
|-------|-------------|-------|
| 0     | 0000 0258 0834 01FE 0000 0000 0000 0000 0000 0226 | Default chase (radius=600, height=2100, pitch=510) |
| 1     | 06AE 006E 0000 0000 0000 0000 0000 01DB 05DC 0136 | Close follow |
| 2     | 0000 0000 0000 0000 0000 0190 0546 006E 0000 0000 | Bumper cam |
| 3     | 0000 0000 0000 0145 04B0 00F0 0000 0000 0000 0000 | Hood cam |
| 4     | 0000 00F0 060E 006E 0000 0000 0000 0000 0001 0000 | Far chase |
| 5     | 0000 0000 0000 0000 0038 FF00 0000 0000 0000 0000 | Special / disabled |

---

## 5. Key String Table Regions

| Start      | End        | Content | Count |
|------------|------------|---------|-------|
| 0x0045C114 | 0x0045D800 | PE import DLL names + API names | ~20 |
| 0x0045D800 | 0x0045DE70 | MSVC runtime strings (error messages, locale data) | ~80 |
| 0x0045FD7E | 0x00462350 | M2DX.DLL mangled export names | ~120 |
| 0x00462350 | 0x00462350 | Win32 API names (KERNEL32, USER32, WINMM, DSOUND) | ~80 |
| 0x00463070 | 0x00463B90 | Game data strings (BRAKED, smoke, fog error messages) | ~30 |
| 0x00463B90 | 0x00464250 | Frontend asset paths (TGA files, ZIP archives) | ~40 |
| 0x004642B4 | 0x00464370 | Frontend menu sprite names (ResultsText, MainMenuText, etc.) | ~15 |
| 0x0046437C | 0x004655C0 | NPC racer names (237 entries: Frank, Raymond, Jeffrey, ...) | ~237 |
| 0x00465460 | 0x00465960 | HUD format strings (%dKPH, %dMPH, time formats) | ~10 |
| 0x00465960 | 0x00465AE4 | Character set mapping table (384-byte ASCII remap) | 1 |
| 0x00465AE4 | 0x00465E10 | Extras gallery paths (mugshots, legals) | ~30 |
| 0x00465E88 | 0x00465F74 | Music track/band names (FEAR FACTORY, KMFDM, etc.) | ~15 |
| 0x004660D0 | 0x00466140 | In-race HUD format strings (%3dKPH, %3dMPH) | ~5 |
| 0x00466620 | 0x004667F0 | Lock/fade error strings, language config filenames | ~20 |
| 0x004668CC | 0x004669D8 | Track ID strings (trak0000-trak0019) + paths | 20 |
| 0x00466B7C | 0x00466D40 | Force feedback errors, FPS stats, system info labels | ~20 |
| 0x00466FF0 | 0x00467240 | Car ZIP filenames (cars\*.zip, 37 entries) | ~37 |
| 0x00467240 | 0x00467310 | Level loading strings (MODELS.DAT, level%03d.zip, etc.) | ~15 |
| 0x00473980 | 0x00473B18 | Surface type names (tarmac, gravel, cobblestone, etc.) | ~15 |
| 0x00473B18 | 0x00473B50 | Track data filenames (LEVELINF.DAT, CHECKPT.NUM, environs.zip) | ~5 |
| 0x00474080 | 0x00474168 | Cop chase arrest reason strings | ~8 |
| 0x00474168 | 0x004743B4 | Localized HUD text (7 languages: EN, DE, FR, NL, ES, SE, IT) | ~80 |
| 0x004743B4 | 0x00474840 | In-race texture names (COLOURS, SPEEDO, numbers, etc.) + pause strings | ~40 |
| 0x00474840 | 0x00474EA0 | Movie path, police light names, damage sprites, sound filenames | ~50 |
| 0x004814D8 | 0x00481EC0 | DirectDraw error code string table (DDERR_*) | ~100 |

Total defined strings in binary: **1,107**

---

## 6. Static Data Tables Summary

| Address    | Size        | Name | Description |
|------------|-------------|------|-------------|
| 0x463098   | ~120 bytes  | Camera preset table | Per-mode camera params (6 entries x 20 bytes) |
| 0x4631A0   | 8 bytes     | Track strip pointer pair | LEFT.TRK / RIGHT.TRK raw data pointers |
| 0x463E0C   | 4 bytes     | Total car count | Default 23 |
| 0x463E4C   | 37 bytes    | Car lock table | Per-car unlock state |
| 0x463FC4   | 72 bytes    | Controller bindings | 18 dwords (9 actions x 2 players) |
| 0x4640A4   | variable    | Cup schedule table | Per-cup track sequence, terminated by 0x63 |
| 0x464120   | variable    | Cup save filename | "CupData.td5" |
| 0x4643BC   | ~800 bytes  | High score table | 5 entries x 0x20 bytes per category |
| 0x466000   | 28 bytes    | Game options block | 7 dwords (laps, timers, traffic, cops, difficulty, dynamics, collisions) |
| 0x466840   | 4 bytes     | Total track count | Default 16 |
| 0x466894   | 20 bytes    | Schedule-to-pool map | `gScheduleToPoolIndex[20]` |
| 0x4668B0   | 26 bytes    | Track lock table | Per-track unlock state |
| 0x466D50   | 76 bytes    | Pool-to-level-ZIP map | int32[19]: pool index -> level%03d.zip number |
| 0x466F90   | 108 bytes   | Per-car torque triplets | 9 cars x 12 bytes |
| 0x46CF6C   | variable    | Per-track checkpoint ptrs | Pointer table indexed by level ZIP number |
| 0x473B9C   | 28 bytes    | Translucent dispatch table | 7 function pointers |
| 0x473D2C   | 56 bytes    | Live AI throttle table | 14 dwords (6 slots + padding) |
| 0x473D64   | 56 bytes    | Default AI throttle table | Baseline values per slot |
| 0x473D9C   | 16 bytes    | Rubber-band parameters | 4 dwords: behind_scale, ahead_scale, behind_range, ahead_range |
| 0x473DB0   | 128 bytes   | AI physics template | Copied to each AI actor at race init |
| 0x4748C0   | 64 bytes    | Surface grip table | Per-wheel surface grip (player only) |
| 0x474900   | 64 bytes    | Surface friction table | Per surface type friction |
| 0x4749C8   | float       | Camera damping weight | 0.125 |
| 0x4749D0   | float       | Radius orbit scale | 0.00390625 (1/256) |
| 0x474BC0   | 64 bytes    | Catmull-Rom basis matrix | 4x4 int32 Catmull-Rom spline coefficients |
| 0x4655C4   | 120 bytes   | Screen function table | 30 function pointers |
| 0x4AEE68   | 4096 bytes  | Luminance-to-ARGB LUT | 1024 uint entries |

---

## 7. Important Unnamed Globals Needing Investigation

These addresses appear in the codebase without clear purpose. Further reverse engineering is needed.

| Address    | Type   | Context | Notes |
|------------|--------|---------|-------|
| 0x4D0D28   | ptr    | CRT entry | Command line pointer from GetCommandLineA |
| 0x4AEE30   | dword  | GameWinMain | Exit code (from WM_QUIT wParam) |
| 0x473B6C   | dword  | Main loop | DX6 confirm gate -- needs naming in Ghidra |
| 0x4C3CE8+4..+60 | dwords | Near g_gameState | Adjacent to game state, likely related game loop flags |
| 0x48301C   | dword  | Steering weight | Max steering weight delta -- ROM default 0, meaning inactive |
| 0x4B08B0   | dword  | AI avoidance | Lateral avoidance direction -- transient per-tick |
| 0x4962C0   | dword  | Frontend | Referenced in race type menu (context flag) |
| 0x473C6C+4..+60 | dwords | Near track strip C | Contains -1 sentinel pattern, may be track segment boundary table |
| 0x4C3D8C   | dword  | STRIP.DAT | Secondary count -- purpose unclear vs auxiliary |
| 0x48DBA0-0x48DC48 | block | Rendering | Cluster of render state flags, likely a struct |
| 0x4A31AC   | base   | Actor views | Per-view actor state (0x1900 stride) -- needs struct layout |
| 0x482DD0   | array  | Rendering | Per-actor brightness array -- size unknown |
| 0x4BF6C8   | array  | Rendering | Depth sort buckets -- 4096 entries, element size unknown |
| 0x46A080-0x46BB00 | block | Environment | TREE.TGA / BRIDGE.TGA string patterns suggest environment object table |

---

## 8. M2DX.DLL External Symbols

The game imports ~120 functions from M2DX.DLL across these namespaces:

| Namespace    | Function Count | Key Functions |
|--------------|----------------|---------------|
| DX           | ~10 | FOpen, FRead, FClose, Allocate, DeAllocate, FCount, info, Image, ImageProTGA, TGACompress |
| DXD3D        | ~10 | BeginScene, EndScene, ClearAll, CanFog, TextureClamp, FullScreen, GetMaxTextures, SetRenderState, InitializeMemoryManagement, bClearScreen, d3d |
| DXD3DTexture | ~5  | LoadRGB, LoadRGBS24, LoadRGBS32, GetMask, Set, Texture |
| DXDraw       | ~8  | ClearBuffers, Flip, ConfirmDX6, GetDDObject, PrintTGA, FPSName, FPSCaption |
| DXInput      | ~20 | CheckKey, GetJS, GetMouse, GetKB, Read, Write, ReadOpen, ReadClose, WriteOpen, WriteClose, SetConfiguration, ResetConfiguration, Configure, GetString, SetAnsi, EnumerateEffects, SetEffect, PlayEffect, StopEffects, CreateEffects, js, JoystickC, JoystickButtonC, JoystickType |
| DXPlay       | ~12 | Destroy, ConnectionEnumerate, ConnectionPick, EnumSessionTimer, SendMessageA, JoinSession, NewSession, ReceiveMessage, SealSession, HandlePadHost, HandlePadClient, UnSync, dpu |
| DXSound      | ~12 | Play, Stop, LoadBuffer, Remove, Modify, ModifyOveride, Status, SetVolume, GetVolume, CDPlay, CDSetVolume, CDGetVolume, MuteAll, UnMuteAll, SetPlayback, CanDo3D, GetDSObject |
| DXWin        | ~4  | Initialize, Uninitialize, DXInitialize, Environment, CleanUpAndPostQuit |
| (global)     | ~5  | DXErrorToString, Msg, LogReport, Report, CheckOut, ErrorN, ReleaseMsg, DXDecimal, ExtractScreenInfo, GetStateString, DXInputGetJS, DXInputGetKBStick |

---

## 9. Cross-Reference Summary for Key Globals

### g_gameState (0x4C3CE8) -- 7 xrefs
- READ: 0x442170
- WRITE: 0x4421ED, 0x44226D, 0x4422D6, 0x44242B, 0x442446, 0x44253C

### g_frontendScreenFnTable (0x4655C4) -- 2 xrefs
- READ: 0x415447 (dispatch call)
- DATA: 0x414620 (table reference)

### g_cameraPresetTable (0x463098) -- 2 xrefs
- DATA: 0x4014D2, 0x40151D

### Rubber-band globals (0x473D9C-0x473DA8) -- 50+ xrefs total
- All four globals have 11 WRITE refs (from `InitializeRaceActorRuntime` difficulty branches)
- Each has 1-2 READ refs (from `ComputeAIRubberBandThrottle`)

### Translucent dispatch table (0x473B9C) -- 3 xrefs
- DATA: 0x431380, 0x4314D8, 0x4315BA (all three translucent rendering paths)

### Track strip pointers -- 2-3 xrefs each
- 0x4631A0: DATA from 0x406E3D, 0x4075D0
- 0x4631A4: DATA from 0x406E5E, 0x4075F8
- 0x473C6C: DATA from 0x4345E4, 0x4346A3, 0x434764
