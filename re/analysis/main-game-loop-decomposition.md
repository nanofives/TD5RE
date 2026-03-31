# Main Game Loop Decomposition

Complete analysis of the TD5 execution flow from CRT entry through the main game loop state machine, covering initialization, the message pump, state dispatch, frame timing, and shutdown.

---

## 1. Complete Execution Flow: entry -> GameWinMain -> RunMainGameLoop

### 1.1 CRT Entry Point (`entry` -- 0x4493E0)

The PE entry point performs standard MSVC CRT initialization before calling into game code:

```
entry (0x4493E0)
  |-- GetVersion()                  -- capture Windows version info
  |-- InitProcessHeap(1)           -- CRT heap init (fatal exit 0x1C on failure)
  |-- InitPrimaryTlsSlot()         -- TLS init (fatal exit 0x10 on failure)
  |-- _ioinit()                    -- stdio init
  |-- GetCommandLineA()            -- store at DAT_004d0d28
  |-- _crt_setenv() / _setargv() / _setenvp() / _cinit()
  |-- GetStartupInfoA()            -- read STARTUPINFO for nCmdShow
  |-- GetModuleHandleA(NULL)       -- hInstance
  |-- GameWinMain(hInstance, NULL, lpCmdLine, nCmdShow)
  |-- _exit(returnValue)           -- does not return
```

No game-specific code runs before `GameWinMain`. The entry point is clean CRT boilerplate.

### 1.2 GameWinMain (0x430A90)

**Signature:** `int __stdcall GameWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)`

This is the top-level game bootstrap and main loop driver. It initializes the DXWin framework, runs the Windows message pump, and calls `RunMainGameLoop()` once per frame.

#### Initialization Sequence (pre-loop)

```
GameWinMain
  |
  |-- DXWin::Environment(lpCmdLine)      -- parse command line, detect HW
  |     (returns 0 = fatal, skip everything)
  |
  |-- Configure g_appExref fields:
  |     +0xBC = 0x280 (640)              -- default render width
  |     +0xC0 = 0x1E0 (480)             -- default render height
  |     +0xC4 = 0x10  (16)              -- default color depth (16-bit)
  |     +0x20 = &DAT_0045d6b0           -- callback/vtable pointer
  |     +0x04 = hInstance                -- app instance handle
  |     +0xC8 = nCmdShow                -- window show command
  |     +0x188 = 1                       -- bootstrap-active / loop-enabled flag
  |
  |-- DXWin::Initialize()                -- create window, init DirectDraw/D3D
  |     (returns 0 = fatal, skip loop)
  |     Also checks: ErrorN_exref == 0
  |
  |-- [ENTER MAIN LOOP]
  |
  |-- DXWin::Uninitialize()              -- teardown on exit
  |-- DestroyWindow(*(HWND*)g_appExref)  -- destroy main window
  |-- return DAT_004aee30                -- exit code (from WM_QUIT wParam)
```

#### The Main Loop (message pump + frame dispatch)

The main loop runs `while (g_appExref+0x180 == 0)` -- i.e., until the global quit latch is set:

```
while (g_appExref->quitFlag == 0) {
    //--- MESSAGE PUMP ---
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {      // 0x12
            DXWin::CleanUpAndPostQuit();
            break;
        }
        if (hWnd == NULL || !TranslateAcceleratorA(hWnd, hAccel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }

    //--- FRAME DISPATCH ---
    if (DAT_00473b6c == 0) {
        // Normal path: run game frame
        if (bootstrapActive && !auxGate184 && !startupGate168) {
            if (quitFlag) break;
            if (ErrorN_exref == 0)
                RunMainGameLoop();       // <--- THE GAME FRAME
            else
                DXWin::CleanUpAndPostQuit();
            if (quitFlag) break;
        }
    } else {
        // DX6 confirmation path (display mode change recovery)
        if (DXDraw::ConfirmDX6()) {
            DAT_00473b6c = 0;
            if (bootstrapActive && !auxGate184 && !startupGate168) {
                if (quitFlag) break;
                DXWin::DXInitialize();
                InitializeRaceVideoConfiguration();
            }
        }
    }
}
```

**Key control flags in `g_appExref`:**

| Offset | Name | Purpose |
|--------|------|---------|
| +0x000 | HWND | Main window handle |
| +0x004 | hInstance | Application instance |
| +0x008 | HACCEL | Accelerator table handle |
| +0x020 | callback ptr | Points to DAT_0045d6b0 |
| +0x0BC | render width | 640 default |
| +0x0C0 | render height | 480 default |
| +0x0C4 | color depth | 16 default |
| +0x0C8 | nCmdShow | Window show command |
| +0x108 | mouse buttons | Current mouse button state |
| +0x100 | mouse Y | Current mouse Y position |
| +0x104 | mouse X | Current mouse X position |
| +0x138 | shutdown callback | Set to `RequestIntroMovieShutdown` during movie |
| +0x14C | movie capable flag | Non-zero if intro movie can play |
| +0x150 | fullscreen flag | Gates fullscreen restore behavior |
| +0x15C | vsync flag | Used for flip sync parameter |
| +0x168 | startup gate | Auxiliary startup-transition gate |
| +0x16C | frontend active flag | 1 during frontend, 0 during race |
| +0x174 | input/backend mode | Used by intro movie system |
| +0x180 | quit latch | Global shutdown flag (0=running, non-0=quit) |
| +0x184 | aux gate | Auxiliary startup gate |
| +0x188 | bootstrap active | Loop-enabled flag (set to 1 before loop) |
| +0x12C | app callback (0x12C) | Display mode state callback |

**Gate conditions for `RunMainGameLoop()` to execute:**
1. `g_appExref+0x188 != 0` (bootstrap complete)
2. `g_appExref+0x184 == 0` (no auxiliary gate active)
3. `g_appExref+0x168 == 0` (startup transition complete)
4. `g_appExref+0x180 == 0` (quit not requested)
5. `ErrorN_exref == 0` (no fatal error)
6. `DAT_00473b6c == 0` (not in DX6 recovery path)

---

## 2. RunMainGameLoop State Machine (0x442170)

**Signature:** `void __stdcall RunMainGameLoop(void)`

This function is called once per frame from `GameWinMain`. It implements a 4-state machine controlled by `g_gameState` (address `0x4C3CE8`).

### 2.1 State Machine Diagram

```
                    +------------------+
                    |   GAME START     |
                    +--------+---------+
                             |
                             v
               +-------------+-------------+
               |   GAMESTATE_INTRO (0)     |
               |                           |
               | 1. Play intro movie       |
               |    (if pending & capable) |
               | 2. InitializeMemoryMgmt   |
               | 3. SetRenderState         |
               | 4. ShowLegalScreens       |
               |                           |
               | (FALLS THROUGH to MENU)   |
               +-------------+-------------+
                             |
                             v
               +-------------+-------------+
               |   GAMESTATE_MENU (1)      |<-----------------------+
               |                           |                        |
               | 1. Init frontend resources|                        |
               |    (if pending)           |                        |
               | 2. RunFrontendDisplayLoop |                        |
               | 3. Check race request     |                        |
               |                           |                        |
               | appExref+0x16C = 1        |                        |
               +---+-------------------+---+                        |
                   |                   |                             |
                   | race requested    | no race (break,            |
                   |                   | return to pump)            |
                   v                   |                             |
      +------------+------+           |                             |
      | InitializeRace    |           |                             |
      | Session           |           |                             |
      +------------+------+           |                             |
                   |                   |                             |
                   v                   |                             |
      +------------+------+           |                             |
      | GAMESTATE_RACE (2)|           |                             |
      |                   |           |                             |
      | RunRaceFrame()    |           |                             |
      | appExref+0x16C = 0|           |                             |
      +---+--------+------+           |                             |
          |        |                   |                             |
          |        | returns 0         |                             |
          |        | (continue)        |                             |
          |        v                   |                             |
          |   [break, return           |                             |
          |    to message pump]        |                             |
          |                            |                             |
          | returns non-0 (race over)  |                             |
          v                            |                             |
    +-----+----------+                 |                             |
    | Post-race:     |                 |                             |
    | dd+0x1730 = 0  |                 |                             |
    | FullScreen     |                 |                             |
    |   (if needed)  |                 |                             |
    | DXPlay::UnSync |                 |                             |
    +-----+----+-----+                |                             |
          |    |                       |                             |
          |    +-- benchmark? ---------+--------> GAMESTATE_BENCHMARK (3)
          |                            |                             |
          +-- normal ------------------+---------+                   |
                                                 |                   |
                                                 +-------------------+
                                                 (back to MENU)

               +---------------------------+
               | GAMESTATE_BENCHMARK (3)   |
               |                           |
               | 1. Clear benchmark flag   |
               | 2. Load FPS image (TGA)   |
               |    from FPSName_exref     |
               | 3. Decode to pixel buffer |
               | 4. Lock surface, blit     |
               | 5. Flip                   |
               | 6. Wait for keypress      |
               | 7. Deallocate + -> MENU   |
               +---------------------------+
```

### 2.2 State Transitions Table

| From | To | Trigger | Code |
|------|----|---------|------|
| INTRO (0) | MENU (1) | Legal screens shown | `g_gameState = GAMESTATE_MENU` (fallthrough) |
| MENU (1) | RACE (2) | `g_startRaceRequestFlag` or `g_startRaceConfirmFlag` set | `g_gameState = GAMESTATE_RACE` |
| RACE (2) | MENU (1) | `RunRaceFrame()` returns non-0, benchmark inactive | `g_gameState = GAMESTATE_MENU` |
| RACE (2) | BENCHMARK (3) | `RunRaceFrame()` returns non-0, `g_benchmarkModeActive != 0` | `g_gameState = (-(benchmarkActive != 0) & 2) + 1` = 3 |
| BENCHMARK (3) | MENU (1) | Keypress, file error, or lock failure | `g_gameState = GAMESTATE_MENU` |

**State transition formula (race exit):** `g_gameState = (-(uint)(g_benchmarkModeActive != 0) & 2) + GAMESTATE_MENU`
- If benchmark inactive: `(-0 & 2) + 1 = 0 + 1 = 1` (MENU)
- If benchmark active: `(-1 & 2) + 1 = 2 + 1 = 3` (BENCHMARK)

### 2.3 Enum Values

```c
typedef enum GameState {
    GAMESTATE_INTRO     = 0,   // Intro movie + legal screens
    GAMESTATE_MENU      = 1,   // Frontend / menu system
    GAMESTATE_RACE      = 2,   // Active gameplay
    GAMESTATE_BENCHMARK = 3    // Benchmark result display
} GameState;
```

---

## 3. Detailed State Breakdown

### 3.1 GAMESTATE_INTRO (0)

Runs only once at startup. The `switch` uses fallthrough from case 0 to case 1.

**Step 1 -- Intro Movie (conditional):**
```c
if (g_introMoviePendingFlag != 0) {
    if (g_appExref+0x14C != 0 && g_appExref+0x13C == 0) {
        LogReport("Playing Movie");
        PlayIntroMovie();     // plays Movie/intro.tgq via EA TGQ engine
        if (quitFlag) return; // movie can trigger quit
    }
    g_introMoviePendingFlag = 0;
}
```

`PlayIntroMovie()` (0x43C440):
- Captures DirectSound/DirectDraw objects from DXWin
- Installs `RequestIntroMovieShutdown` at `g_appExref+0x138`
- Opens `Movie/intro.tgq` via `OpenAndStartMediaPlayback`
- Runs its own nested message pump with `PeekMessageA`/`GetMessageA`
- Skippable via Enter, Shift, Escape, or Space (VK 0x0D, 0x10, 0x1B, 0x20)
- Volume adjustable with +/- keys during playback
- Sleeps 30ms per iteration (`Sleep(0x1E)`)

**Step 2 -- Memory and Render Init:**
```c
DXD3D::InitializeMemoryManagement();   // set up 3D memory pools
DXD3D::SetRenderState();               // configure initial D3D state
```

**Step 3 -- Legal Screens:**

`ShowLegalScreens()` (0x42C8E0) is a blocking function:
- Loads `legal1.tga` from `LEGALS.ZIP`, displays it
- Waits up to 5 seconds or any keypress (with 400ms grace period before accepting input)
- Loads `legal2.tga` from `LEGALS.ZIP`, displays it
- Same 5-second timeout / keypress dismiss

**Step 4 -- Transition:** Sets `g_gameState = GAMESTATE_MENU` and falls through.

### 3.2 GAMESTATE_MENU (1)

**Frontend Resource Initialization (once per entry):**

When `g_frontendResourceInitPending != 0`:

`InitializeFrontendResourcesAndState()` (0x414740):
1. Seeds random number generator with `timeGetTime()`
2. If not replaying: `SerializeRaceStatusSnapshot()` (saves race state)
3. Resets frontend state variables (redraw count, frame toggle, flags)
4. Captures back surface pointer from `dd_exref+8`
5. Sets CD audio volume from options
6. Records timestamp for attract mode idle timer
7. Installs `InitializeFrontendDisplayModeState` callback at `g_appExref+0x12C`
8. Clears surface registry
9. Detects pixel format (15-bit, 16-bit, or other) and selects color masks
10. Sets frontend virtual resolution: 640x480
11. Builds dither offset table, resets overlay/selection/string state
12. Sets initial screen function pointer: `g_currentScreenFnPtr = g_initialScreenFnPtr` (ScreenStartupInit, entry 28)
13. Creates two 640x480 work surfaces for double-buffered frontend rendering
14. Loads shared UI assets from `Front_End/FrontEnd.zip`:
    - ButtonBits.tga, ArrowButtonz.tga, ArrowExtras.tga
    - snkmouse.tga (cursor), BodyText.tga or SmallText2.tga (locale-dependent)
    - SmallText.tga, SmallTextb.tga, MenuFont.tga
15. Loads frontend sound effects
16. Loads extras gallery images
17. Selects and plays a random CD track (avoiding recently played)

**Per-Frame Frontend:**

`RunFrontendDisplayLoop()` (0x414B50):
1. **Surface recovery:** Checks for lost DirectDraw surface (`DDERR_SURFACELOST = 0x887601C2`), restores if needed, forces 3 redraws
2. Early return if `g_startRaceRequestFlag` is set
3. **Input polling:**
   - `DXInputGetKBStick(0)` for keyboard/joystick
   - `DXInput::GetMouse()` for mouse
   - Computes edge-triggered bits for both keyboard and mouse
   - Detects mouse movement (threshold > 8 pixels Manhattan distance)
4. **Client origin update:** `UpdateFrontendClientOrigin()`
5. **Screen dispatch:** `(*g_currentScreenFnPtr)()` -- calls the active screen's state machine function from the 30-entry `g_frontendScreenFnTable` (see frontend-screen-table.md)
6. Early return if `g_startRaceConfirmFlag` is set
7. **Cursor overlay:** Queues mouse cursor sprite if visible
8. **Render flush:** `RenderFrontendUiRects()`, `FlushFrontendSpriteBlits()`
9. **Presentation:**
   - Hardware flip path: `DXDraw::Flip(1)`, then locks back surface for screen dump
   - Software path: `PresentFrontendBufferSoftware()`
10. **Display mode update:** `UpdateFrontendDisplayModeSelection()`
11. **Escape key handling:** Maps Esc to the designated back button
12. **Cheat code detection:** On the Options Hub screen, scans for multi-key cheat sequences that toggle NPC racer unlock flags
13. **Frame counter increment:** `g_frontendAnimFrameCounter++`
14. **Attract mode timeout:** If on Main Menu and idle > 50 seconds with `g_attractModeIdleCounter < -15`:
    - Selects random unlocked track
    - Switches screen to `RunAttractModeDemoScreen` (entry 2)

**Race Request Handling:**

After `RunFrontendDisplayLoop` returns:
```c
if (g_startRaceRequestFlag == 0 && g_startRaceConfirmFlag == 0) {
    // No race requested -- stay in MENU
} else {
    g_frontendResourceInitPending = 1;   // re-init frontend on return
    g_startRaceConfirmFlag = 0;
    g_startRaceRequestFlag = 0;
    InitializeRaceSession();             // heavy race bootstrap
    g_gameState = GAMESTATE_RACE;
    return;
}
g_appExref+0x16C = 1;  // mark frontend as active
```

### 3.3 GAMESTATE_RACE (2)

**Per-Frame:**
```c
result = RunRaceFrame();           // full simulation + render frame
g_appExref+0x16C = 0;             // mark frontend as inactive
if (result != 0) {
    // Race is over
    dd_exref+0x1730 = 0;          // clear race-active render flag
    if (g_appExref+0x150 != 0) {
        DXD3D::FullScreen(dd_exref+0x1690);  // restore fullscreen mode
    }
    g_gameState = benchmark ? 3 : 1;
    DXPlay::UnSync();              // release DirectPlay sync
    return;
}
// result == 0: race continues, break back to message pump
```

`RunRaceFrame()` (0x42B580) is the core gameplay loop body (see Section 5 for details). It returns:
- **0** = race in progress, continue next frame
- **1** = race complete (fade-out finished, resources released)

### 3.4 GAMESTATE_BENCHMARK (3)

Entered only when `g_benchmarkModeActive` was set during the race. Displays the benchmark results image.

1. Clears `g_benchmarkModeActive = 0`
2. If `g_benchmarkImageLoadPending`:
   - Opens `FPSName_exref` (the benchmark results filename)
   - Allocates 0x50000 bytes for raw data + 0xA0000 for decoded pixels
   - Reads file, configures `Image_exref` struct for 16-bit RGB565 TGA
   - Calls `DX::ImageProTGA()` to decode
3. Locks the primary DirectDraw surface
4. Blits decoded pixel data (640x480 at 16bpp) line by line
5. Unlocks surface, calls `DXDraw::Flip()`
6. Polls `DXInputGetKBStick(0)` for any keypress
7. On keypress: deallocates buffers, sets `g_benchmarkImageLoadPending = 1`, returns to MENU

---

## 4. Initialization Sequence Summary

Complete ordered initialization from cold start to first interactive frame:

```
 1.  CRT: GetVersion, heap, TLS, stdio, env, argv
 2.  GameWinMain: DXWin::Environment(cmdLine)     -- HW detection, cmd parse
 3.  GameWinMain: Configure g_appExref             -- resolution, depth, callbacks
 4.  GameWinMain: DXWin::Initialize()              -- window creation, DDraw/D3D
 5.  RunMainGameLoop INTRO: PlayIntroMovie()       -- EA TGQ movie playback
 6.  RunMainGameLoop INTRO: InitializeMemoryMgmt   -- 3D memory pools
 7.  RunMainGameLoop INTRO: SetRenderState          -- initial D3D state
 8.  RunMainGameLoop INTRO: ShowLegalScreens()      -- legal1.tga, legal2.tga
 9.  RunMainGameLoop MENU: InitFrontendResources    -- surfaces, fonts, UI assets
10.  RunMainGameLoop MENU: ScreenStartupInit(28)    -- first frontend screen
11.  RunMainGameLoop MENU: ScreenLocalizationInit(0)-- language DLL, config, modes
12.  RunMainGameLoop MENU: ScreenMainMenu(5)        -- user-interactive main menu
```

**On race start (from any menu path):**
```
13.  InitializeRaceSession: Display loading screen
14.  InitializeRaceSession: Reset game heap
15.  InitializeRaceSession: Configure slot states (player/AI/disabled)
16.  InitializeRaceSession: Load track runtime data
17.  InitializeRaceSession: Load car assets and configure vehicles
18.  InitializeRaceSession: Bind track strip pointers
19.  InitializeRaceSession: Load 3D models (MODELS.DAT from level ZIP)
20.  InitializeRaceSession: Load track textures
21.  InitializeRaceSession: Parse model data, get span ring length
22.  InitializeRaceSession: Load sky mesh (SKY.PRR from STATIC.ZIP)
23.  InitializeRaceSession: Initialize vehicles, actors, wheels, shadows
24.  InitializeRaceSession: Position all actors on track grid
25.  InitializeRaceSession: Open input recording/playback
26.  InitializeRaceSession: Load ambient sounds
27.  InitializeRaceSession: Initialize particles, smoke, tire tracks, weather
28.  InitializeRaceSession: Configure force feedback
29.  InitializeRaceSession: Configure input mapping
30.  InitializeRaceSession: Start CD audio track
31.  InitializeRaceSession: Initialize 3D render state + viewport layout
32.  InitializeRaceSession: Load race texture pages
33.  InitializeRaceSession: Initialize pause menu overlay
```

---

## 5. RunRaceFrame Body (0x42B580)

The race frame function handles simulation, rendering, and timing in a single call.

### 5.1 Fixed-Timestep Simulation Loop

```c
// Accumulate frame time into simulation budget
g_simTickBudget += g_smoothedFrameDt;

// Run simulation ticks until budget exhausted
while (g_simTimeAccumulator > 0xFFFF) {
    PollRaceSessionInput();
    CacheVehicleCameraAngles(primary, 0);
    CacheVehicleCameraAngles(secondary, 1);

    // Per-slot player control update
    for each slot with state == ACTIVE:
        UpdatePlayerVehicleControlState(slot);

    // Core simulation
    UpdateRaceActors();          // AI + physics step
    ResolveVehicleContacts();    // collision resolution
    UpdateTireTrackPool();       // tire marks
    UpdateRaceOrder();           // position sorting

    // Per-viewport camera update
    for each viewport:
        UpdateChaseCamera(primaryActor, 1, viewIdx);

    // Particles
    UpdateRaceParticleEffects(0);
    UpdateRaceParticleEffects(1);
    UpdateAmbientParticleDensityForSegment(primary, 0);
    UpdateAmbientParticleDensityForSegment(secondary, 1);

    UpdateRaceCameraTransitionTimer();

    g_simTickBudget -= TICK_DECREMENT;      // 0x0045d5f4
    g_simTimeAccumulator -= 0x10000;        // one tick consumed
    g_simulationTickCounter++;
    g_raceSimSubTickCounter++;

    // Normalize track wrap state for all active actors
    for each active actor:
        NormalizeActorTrackWrapState(actor);
}
```

The simulation runs at a fixed tick rate (each tick = 0x10000 accumulator units). Frame time is normalized and accumulated, then drained in discrete steps. This ensures deterministic physics regardless of frame rate.

### 5.2 Race Completion Detection

Before the simulation loop, each frame checks `CheckRaceCompletionState()`:
- Returns 0: race continues
- Returns 1: all actors finished (or 2-second cooldown after finish expired)

On completion:
1. Sets `g_raceEndFadeState = 1`
2. Optionally activates radial pulse HUD effect (single-player win)
3. Selects fade direction based on viewport layout:
   - Single player: alternates horizontal/vertical (`g_fadeDirectionAlternator`)
   - Horizontal split: always horizontal (direction 0)
   - Vertical split: always vertical (direction 1)

### 5.3 Fade-Out and Resource Release

The fade accumulates per frame. When it reaches 255.0:
```c
ReleaseRaceSoundChannels();
DXInput::StopEffects();
DXInput::ResetConfiguration();
ReleaseRaceRenderResources();
DXInput::WriteClose() or DXInput::ReadClose();

if (benchmark) WriteBenchmarkResultsTgaReport();

// Post-process: accumulate per-lap split times into total finish time
// Fix up any actors whose display position was unset
return 1;  // signals race over to RunMainGameLoop
```

### 5.4 Rendering Pipeline (per frame)

```
BeginRaceScene()
  |
  +-- Primary Viewport:
  |     UpdateRaceCameraTransitionState(primary, 0)
  |     Camera update (chase / trackside / orbit)
  |     Update listener position (3D audio)
  |     AdvanceWorldBillboardAnimations()
  |     Render sky mesh (with fog if capable)
  |     Render track spans from display list
  |     RenderRaceActorsForView(0)
  |     RenderTireTrackPool()
  |     RenderAmbientParticleStreaks(primary)
  |     DrawRaceParticleEffects(0)
  |     FlushQueuedTranslucentPrimitives()
  |     FlushProjectedPrimitiveBuckets()
  |     DrawRaceStatusText(primary, 0)
  |
  +-- Secondary Viewport (if split-screen):
  |     [Same pipeline for player 2]
  |
  +-- HUD:
  |     RenderRaceHudOverlays(simTickBudget)
  |     RunAudioOptionsOverlay() (if active)
  |
  +-- Frame Timing:
  |     g_frameEndTimestamp = timeGetTime()
  |     frameDeltaMs = end - prev
  |     g_instantFPS = 1000000.0 / frameDeltaMs * scale
  |     g_normalizedFrameDt = frameDeltaMs * timeScale * tickScale
  |     g_simTickBudget += smoothedDt (clamped to 4.0 max)
  |
  +-- Audio:
  |     UpdateVehicleLoopingAudioState(each slot)
  |     UpdateVehicleAudioMix()
  |
  +-- Finalize:
        EndRaceScene()
        FCount_exref++    // global frame counter
        return 0
```

### 5.5 Frame Timing Constants

| Address | Value | Description |
|---------|-------|-------------|
| 0x45D5F4 | tick decrement | Subtracted from `g_simTickBudget` per sim tick |
| 0x45D650 | 4.0 | Max sim tick budget (prevents spiral of death) |
| 0x45D658 | tick scale | Frame dt normalization factor |
| 0x45D660 | time scale | Converts ms to internal time units |
| 0x45D668 | FPS multiplier | Converts reciprocal frame time to FPS |
| 0x45D670 | 1000000.0 | Microsecond constant for FPS calculation |
| 0x45D680 | fade scale | Controls fade-out speed |
| 0x45D684 | 255.0 | Target fade value (full black) |
| 0x45D68C | sub-tick fraction | Converts accumulator to 0.0-1.0 interpolant |

Benchmark mode forces `g_simTickBudget = 3.0` each frame for consistent simulation speed.

---

## 6. Global State Variables

### 6.1 Core State Machine

| Address | Name | Type | Description |
|---------|------|------|-------------|
| 0x4C3CE8 | `g_gameState` | int (enum) | 0=INTRO, 1=MENU, 2=RACE, 3=BENCHMARK |
| 0x45D53C | `g_appExref` | pointer | DX::app object base (static, resolved at link) |

### 6.2 Intro/Startup Flags

| Address | Name | Type | Description |
|---------|------|------|-------------|
| (appExref+0x14C) | movie capable flag | int | Non-zero if system supports intro movie |
| (appExref+0x13C) | movie skip flag | int | Non-zero to skip movie |
| g_introMoviePendingFlag | intro pending | int | Set at startup, cleared after movie plays |

### 6.3 Frontend/Menu Control

| Address | Name | Type | Description |
|---------|------|------|-------------|
| g_frontendResourceInitPending | resource init flag | int | Set to 1 when frontend needs re-initialization |
| g_startRaceRequestFlag | race request | int | Set by menu screens to request race start |
| g_startRaceConfirmFlag | race confirm | int | Secondary race start trigger |
| g_currentScreenFnPtr | screen function | code* | Active frontend screen state machine |
| g_frontendInnerState | screen state | int | Sub-state within current screen |
| g_frontendAnimFrameCounter | anim counter | int | Frame counter for frontend animations |
| g_frontendFrameToggle | frame toggle | int | Alternates 0/1 each frontend frame |
| g_frontendRedrawCount | redraw count | int | Forces N full redraws (surface recovery) |
| g_frontendHardwareFlipEnabled | hw flip | int | 1=hardware flip, 0=software present |
| g_frontendScreenTransitionFlag | transition flag | int | Signals screen transition in progress |
| g_attractModeIdleCounter | idle counter | int | Counts down during main menu idle |
| g_attractModeDemoActive | demo active | int | Non-zero during attract mode playback |

### 6.4 Race Session Control

| Address | Name | Type | Description |
|---------|------|------|-------------|
| g_raceEndFadeState | fade state | int | 0=racing, 1+=fade accumulator |
| g_raceEndRadialPulseEnabled | pulse enabled | int | Radial pulse effect on win |
| g_raceEndTimerStart | timer start | DWORD | timeGetTime() when replay end timer starts |
| g_fadeDirection | fade dir | int | 0=horizontal, 1=vertical |
| g_fadeDirectionAlternator | fade alt | int | Toggles between 0 and 1 |
| g_simTickBudget | tick budget | float | Accumulated simulation time to process |
| g_simTimeAccumulator | time accum | uint | Raw accumulator (drained by 0x10000 per tick) |
| g_simulationTickCounter | tick count | int | Total sim ticks since race start |
| g_raceSimSubTickCounter | sub tick | int | Sub-tick counter |
| g_normalizedFrameDt | frame dt | float | Normalized frame delta time |
| g_instantFPS | fps | float | Current frame rate |
| g_framePrevTimestamp | prev time | DWORD | timeGetTime() of previous frame |
| g_frameEndTimestamp | end time | DWORD | timeGetTime() of current frame |
| g_gamePaused | paused | int | Set to 1 at race init (countdown) |
| g_benchmarkModeActive | benchmark | int | Non-zero during benchmark mode |
| g_benchmarkFirstFrameSkipped | skip first | int | Skips first frame for timing accuracy |
| g_inputPlaybackActive | playback | int | Non-zero during replay playback |
| g_replayModeFlag | replay | int | Non-zero during any replay mode |
| g_replayOrBenchmarkActive | replay/bench | int | Combined flag |
| g_networkRaceActive | network | int | Non-zero during network race |

### 6.5 Benchmark Display

| Address | Name | Type | Description |
|---------|------|------|-------------|
| g_benchmarkImageLoadPending | load pending | int | Set to 1 when image needs loading |
| g_benchmarkImageDataPtr | image data | void* | Allocated raw TGA data (0x50000 bytes) |
| g_benchmarkDecodedPixelPtr | pixel data | int | Allocated decoded pixels (0xA0000 bytes) |
| g_benchmarkImageInfoStruct | info struct | struct | TGA decode parameters |
| FPSName_exref | filename | char* | Benchmark results TGA filename |
| Image_exref | image obj | struct | DX image processing structure |

### 6.6 Quit/Shutdown Control

| Address | Name | Type | Description |
|---------|------|------|-------------|
| (appExref+0x180) | quit latch | int | Global quit flag -- set to non-zero to exit |
| ErrorN_exref | error flag | int | Non-zero = fatal error, triggers cleanup |
| DAT_004aee30 | exit code | int | Process exit code from WM_QUIT.wParam |

---

## 7. Shutdown / Cleanup Sequence

### 7.1 Normal Quit (user exits from menu)

```
User selects "Exit" in Main Menu
  -> PostQuitMessage(0)              -- queues WM_QUIT
  -> Message pump receives WM_QUIT (0x12)
  -> DXWin::CleanUpAndPostQuit()     -- sets quit latch
  -> Main loop breaks
  -> DXWin::Uninitialize()           -- release DirectDraw/D3D/Sound/Input
  -> DestroyWindow(hWnd)             -- destroy main window
  -> return exit code
  -> _exit(exitCode)                 -- CRT exit
```

### 7.2 Error Quit

```
ErrorN_exref set to non-zero (e.g., 3D init failure)
  -> GameWinMain detects ErrorN_exref != 0
  -> DXWin::CleanUpAndPostQuit()
  -> Same teardown as normal quit
```

### 7.3 Race-to-Frontend Transition

```
RunRaceFrame returns 1 (race complete)
  -> Clear dd_exref+0x1730 (race render flag)
  -> Restore fullscreen mode if needed
  -> DXPlay::UnSync()
  -> g_gameState = GAMESTATE_MENU
  -> (next frame) g_frontendResourceInitPending triggers re-init
  -> Frontend picks up at results screen (screen 24)
```

### 7.4 Intro Movie Shutdown

The intro movie installs `RequestIntroMovieShutdown` at `g_appExref+0x138`. If the application needs to quit during movie playback, this callback is invoked. After movie completion or skip:
- Releases DirectDraw overlay surfaces
- Clears the callback: `g_appExref+0x138 = 0`
- Sets `DAT_004beacc = 1` (movie played flag, prevents replay)

---

## 8. DX6 Recovery Path

A secondary path exists in `GameWinMain` for DirectX 6 display mode recovery:

```c
if (DAT_00473b6c != 0) {
    if (DXDraw::ConfirmDX6()) {
        DAT_00473b6c = 0;
        DXWin::DXInitialize();
        InitializeRaceVideoConfiguration();
        // Resume normal loop
    }
    // else: retry next frame
}
```

`InitializeRaceVideoConfiguration()` (thunk at 0x442160):
- Derives render detail level from language DLL metadata
- Calls `InitializeRaceRenderGlobals()`
- Sets up 640x480 projection parameters
- Loads static track texture header
- Installs texture page loader and force feedback effect creator

This handles Alt-Tab recovery and display mode switching gracefully without restarting the application.

---

## 9. Message Pump Details

The message pump in `GameWinMain` uses a non-blocking `PeekMessageA` loop:

```c
while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
        DXWin::CleanUpAndPostQuit();
        break;
    }
    if (hWnd == NULL || !TranslateAcceleratorA(hWnd, hAccel, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}
```

Key characteristics:
- **Non-blocking:** Uses `PM_REMOVE` flag, drains all queued messages before processing a game frame
- **Accelerator support:** `TranslateAcceleratorA` handles keyboard shortcuts defined in the accelerator table
- **WM_QUIT intercept:** Detected before dispatch, triggers immediate cleanup
- **One game frame per pump cycle:** After draining all messages, exactly one call to `RunMainGameLoop()` occurs
- **No frame rate limiting in the pump:** Frame pacing is handled by the simulation tick budget system inside `RunRaceFrame` (and implicitly by `DXDraw::Flip` vsync during frontend)

The message is stored at a static global `DAT_004aee28` (the `MSG` structure), with `DAT_004aee2c` being the message ID field and `DAT_004aee30` being the wParam (used as exit code).

---

## 10. Cross-References to Related Analysis

- **Frontend screen table (30 screens):** See [frontend-screen-table.md](frontend-screen-table.md)
- **Race progression (laps, checkpoints, scoring):** See [race-progression-system.md](race-progression-system.md)
- **Frame timing and tick rate:** See [tick-rate-dependent-constants.md](tick-rate-dependent-constants.md) and [frontend-framerate-timers.md](frontend-framerate-timers.md)
- **Race frame hook points:** See [race-frame-hook-points.md](race-frame-hook-points.md)
- **Render pipeline:** See [render-pipeline-and-scene-setup.md](render-pipeline-and-scene-setup.md)
- **Input system:** See [input-and-configuration.md](input-and-configuration.md)
- **Sound system:** See [sound-system-deep-dive.md](sound-system-deep-dive.md)

---

## Key Function Address Summary

| Address | Function | Role |
|---------|----------|------|
| 0x4493E0 | `entry` | CRT entry point |
| 0x430A90 | `GameWinMain` | Game bootstrap + main loop driver |
| 0x442170 | `RunMainGameLoop` | 4-state game state machine |
| 0x414740 | `InitializeFrontendResourcesAndState` | Frontend resource setup |
| 0x414B50 | `RunFrontendDisplayLoop` | Frontend per-frame update |
| 0x42AA10 | `InitializeRaceSession` | Race bootstrap (heavy) |
| 0x42B580 | `RunRaceFrame` | Race per-frame simulation + render |
| 0x42C8E0 | `ShowLegalScreens` | Legal splash display |
| 0x43C440 | `PlayIntroMovie` | EA TGQ movie playback |
| 0x442160 | `InitializeRaceVideoConfiguration` | DX6 recovery / video init |
