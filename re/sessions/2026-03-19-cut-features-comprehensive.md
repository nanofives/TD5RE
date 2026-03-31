# Comprehensive Cut Features Analysis

**Date:** 2026-03-19
**Binary:** TD5_d3d.exe (Ghidra port 8193)

---

## 1. SNOW RENDERING -- CUT FEATURE (Partially Implemented)

### What Exists
- `InitializeWeatherOverlayParticles` (0x446240): Weather type is read from `gTrackEnvironmentConfig + 0x28` into `DAT_004c3de8`.
- **Rain path (type 0):** Allocates 2x 0x6400-byte buffers, looks up "RAINDROP" sprite, extracts UV coords (+0x2c/+0x30/+0x34/+0x38/+0x3c), seeds 128 particles with `BuildSpriteQuadTemplate` per particle.
- **Snow path (type 1):** Allocates identical buffers, looks up "SNOWDROP" sprite (string "4CSNOWDROP" at 0x474e76, lookup starts at +2 = "SNOWDROP"), seeds 128 particles with wider scatter (X:[-8000,8000] Y:[-8000,4000] Z:[200,8000]) -- **but NEVER calls BuildSpriteQuadTemplate**.
- `UpdateAmbientParticleDensityForSegment` (0x4464b0): Updates particle density per track segment. Works identically for both weather types.

### What Is Missing
- `RenderAmbientParticleStreaks` (0x446560): At instruction `0x4465f4`, `JNZ 0x446a61` jumps directly to the function epilogue when `DAT_004c3de8 != 0`. The entire rendering body (0x4465FA through 0x446A60 = 1127 bytes of rendering code) is skipped for snow. No UV extraction, no `BuildSpriteQuadTemplate`, no `QueueTranslucentPrimitiveBatch`.
- Snow particles are initialized with positions but have no renderable geometry (no quad templates built).

### Restoration Feasibility: MEDIUM
**Approach A -- In-function UV patch (simpler):**
The snow init at 0x4463E5-0x44649A needs ~45 extra bytes injected to:
1. Read UV fields from the SNOWDROP archive entry: `entry+0x2c, +0x30, +0x34, +0x38, +0x3c`
2. Set up the local `BuildSpriteQuadTemplate` parameter block (same as rain path)
3. Call `BuildSpriteQuadTemplate` (0x432BD0) inside the inner particle loop

The snow init inner loop currently runs from 0x44642A to 0x44648D. It needs a `CALL 0x432BD0` with the same setup as the rain path (which does `LEA EDX,[ESI+0x10]`, stores to local, `LEA EAX,[ESP+0x14]`, `PUSH EAX`, `CALL BuildSpriteQuadTemplate`, `ADD ESP,4`).

**Approach B -- Code cave trampoline (preferred):**
1. Hijack the snow inner loop at 0x44648D (JNZ back to 0x44642A) with a JMP to cave at 0x45C380
2. In the cave: set up UV locals from the archive entry (already in `_DAT_004c3da8`), `LEA` quad pointer from `ESI+0x10`, call `BuildSpriteQuadTemplate`, then loop back
3. Cave space needed: ~60 bytes. Available: 0x45C380-0x45CFFF = hundreds of bytes free.

**Approach C -- Render path fix (also needed):**
The rendering skip at `0x4465F4` (`JNZ 0x446A61`) must be changed to fall through for snow. Since the rain rendering code does UV-based streak rendering (not just point sprites), snow might need its own simpler rendering -- or the rain renderer can be reused if BuildSpriteQuadTemplate was called during init.

**Minimal patch:**
1. Init fix: ~60B code cave + 5B JMP in snow init loop
2. Render fix: NOP the `JNZ` at 0x4465F4 (6 bytes -> 6x NOP), or change to `JMP` over only the rain-specific respawn code
3. Caveat: Snow particles have different scatter ranges and no streak direction -- the rain renderer might produce odd visuals. May need snow-specific rendering.

---

## 2. VEHICLE DAMAGE RULES -- CUT FEATURE (Stub System)

### What Exists
- `GetDamageRulesStub` (0x42C8D0): A 3-byte function (`XOR EAX,EAX; RET`) that always returns 0.
- Called from **22 sites** across 4 functions:
  - `ApplyVehicleCollisionImpulse` (0x407660): 12 calls
  - `ApplyRandomWheelJitterHighSpeed` (0x43D830): 4 calls
  - `ApplyRandomWheelJitterLowSpeed` (0x43D910): 4 calls
  - `ApplyRandomWheelJitterSynchronized` (0x43D9F0): 2 calls

### What Would Happen If It Returned Nonzero

**In ApplyVehicleCollisionImpulse:**
The return value is used as: `result % impact_magnitude - impact_magnitude/2`. Since `0 % anything = 0` and `0 - magnitude/2 = -magnitude/2`, the current behavior always applies a fixed negative offset to the vehicle's rotation/velocity vectors.

If `GetDamageRulesStub` returned a random value (likely its intended purpose -- it's used exactly like `rand()`), the collision response would add *random* perturbation to:
- Actor +0x1C0/+0x1C4/+0x1C8 (euler angles / orientation) for racer slots <6
- Actor +0x180-0x1A0 (rotation matrix) for traffic/NPC slots >=6
- Actor +0x1D0 (impact timer at +0x1D0) = `impact_magnitude / 6`

This would create **visible post-collision wobble/damage effects**: cars would spin/rotate randomly after big hits proportional to impact force, and traffic vehicles would have their orientation matrices randomized.

**In Wheel Jitter functions:**
The return value is masked with `& 0x7` (3 bits, range 0-7) then `result - 4` gives range [-4,+3]. This value multiplied by speed is added to per-wheel forces at actor+0x2EC/+0x2F0/+0x2F4/+0x2F8. Since `0 - 4 = -4` always, the stub produces a constant negative jitter bias. A proper random would produce chaotic wheel shimmy at speed -- a **"damage causes handling degradation"** effect.

**Wheel jitter dispatch table** at 0x474948:
- 20 entries mapping wheel-state indices to one of 3 jitter functions
- HighSpeed: used for indices 0,2-9,11-19 (most common)
- LowSpeed: index 1
- Synchronized: index 8

### Was This a Health/Damage System?
**Yes, partially.** The wanted-mode damage system (DAMAGE/DAMAGEB1 sprites, `UpdateWantedDamageIndicator`, `AwardWantedDamageScore`) IS fully implemented for cop chase mode. It has per-actor health stored as shorts at `DAT_004bead4 + slot*2`, starting at 0x1000 (4096), decremented by 0x200 (small hit) or 0x400 (big hit >20000 force), with visual health bar overlay.

The `GetDamageRulesStub` was intended to be a shared random number source for collision/handling damage effects in ALL modes, not just cop chase. Replacing it with `_rand` (CALL to 0x448157) would restore the intended behavior.

### Restoration Feasibility: TRIVIAL (single 5-byte patch)
The function at 0x42C8D0 is 3 bytes (`33 C0 C3`) followed by 13 NOP padding bytes (0x42C8D3-0x42C8DF). This gives 16 bytes of writable space.

**Recommended patch:** Replace 0x42C8D0 with `JMP _rand`:
```
E9 82 B8 01 00   ; JMP 0x448157 (_rand)
```
This is a 5-byte patch. The remaining 11 NOPs are untouched. All 22 call sites continue calling 0x42C8D0, which now trampolines to `_rand`. _rand returns in EAX, exactly as the callers expect.

**Alternative:** Replace each of the 22 `CALL 0x42C8D0` instructions with `CALL 0x448157` (same 5-byte size). More invasive but avoids touching the stub function.

---

## 3. GAME TYPES 10 AND 11 -- NOT TRULY CUT (Preview Markers)

### What They Are
In `RaceTypeCategoryMenuStateMachine` (0x416900), state 4 (description panel update):
- `g_frontendButtonIndex == 1` -> `g_selectedGameType = 10`
- `g_frontendButtonIndex == 2` -> `g_selectedGameType = 11`

These are the **"Cup Race"** and **"Continue Cup"** entries in the first-level race type menu. When highlighted (not selected), they set g_selectedGameType to 10 or 11 to index into `SNK_RaceTypeText` for the description panel text.

### Are They Playable?
In `ConfigureGameTypeFlags` (0x410CA0), the switch covers cases 1-9. Types 10 and 11 have **no case** and fall through to default, which returns 0 without setting any flags. But types 10/11 are **never used to start a race** -- they only exist as preview descriptions. When the player actually selects "Cup Race", the menu transitions to a sub-screen (state 6 -> state 7/8) where specific cup types 1-6 are chosen.

### Verdict: NOT CUT
Types 10 and 11 are UI-only markers for displaying description text in the race type menu. They are functioning exactly as designed -- they provide hover-over descriptions for the "Cup Race" and "Continue Cup" menu buttons before the player drills into the sub-menu. They were never intended to be playable game modes.

---

## 4. HIDDEN CHEAT CODE SYSTEM -- ACTIVE (Undocumented)

### Discovery
In `RunFrontendDisplayLoop` (0x414B50), when the current screen is `ScreenOptionsHub`, the game monitors 6 DirectInput key sequences. Each sequence is a phrase typed on the keyboard. The key tables are at 0x4654A4 (6 sequences, stride 0x28 = 40 bytes each, FF-terminated).

### Cheat Codes (typed on Options screen)

| # | Key Sequence | Target Address | XOR Mask | Effect |
|---|---|---|---|---|
| 0 | **I HAVE THE KEY** | 0x496298 | ^= 1 | Unlock flag A -- used with cheat 2 to unlock all cars |
| 1 | **CUP OF CHOICE** | 0x4962A8 | ^= 8 | Unlock all cup tiers (Championship through Ultimate) |
| 2 | **I CARRY A BADGE** | 0x4962AC | ^= 2 | Unlock flag B -- used with cheat 0 to unlock all cars |
| 3 | **LONE CRUSADER IN A DANGEROUS WORLD** | 0x4AAF7C | ^= 1 | Nitro boost/jump: horn button doubles speed + adds Y velocity |
| 4 | **REMOTE BRAKING** | 0x49629C | ^= 1 | Freeze all opponents: zeroes velocity of all non-player actors |
| 5 | **THAT TAKES ME BACK** | 0x4962B4 | ^= 1 | Modifies NPC racer group table unlock bits |

### Detailed Cheat Effects

**Cheats 0+2 ("I HAVE THE KEY" + "I CARRY A BADGE"):**
In `CarSelectionScreenStateMachine` (0x40E020), at 0x40E07B: `if (DAT_004962ac != 0 && DAT_00496298 != 0)` bypasses the car count restriction, setting maximum selectable cars to `DAT_00463E0C` (full roster count) instead of the per-tier limit. Both cheats must be active simultaneously.

**Cheat 1 ("CUP OF CHOICE"):**
`DAT_004962A8` is used in `RaceTypeCategoryMenuStateMachine` at 0x41721D to decide which cup tier buttons are shown as active vs preview (locked). The XOR with 8 unlocks all tiers. Persisted in Config.td5 via `WritePackedConfigTd5`.

**Cheat 3 ("LONE CRUSADER IN A DANGEROUS WORLD"):**
In `UpdatePlayerVehicleControlState` (0x402E60) at 0x403422: when `DAT_004AAF7C != 0` and the horn/siren button is pressed (bit 21 of control word), the code:
- Adds 0x6400 to actor Y velocity (vertical launch)
- Doubles forward (X) and lateral (Z) velocity
- Caps velocity at +/- 0x61A80
- Sets a 15-frame cooldown at actor+0x37C

**Cheat 4 ("REMOTE BRAKING"):**
In `UpdatePlayerVehicleControlState` at 0x4034DD: when `DAT_0049629C != 0` and the horn button is pressed, iterates all 6 actor slots (stride 0x388 starting at 0x4AB108), skipping the player's own slot, and zeroes out their forward velocity (actor+0x1CC) and speed fields (actor+0x1D4).

**Cheat 5 ("THAT TAKES ME BACK"):**
On cheat activation in RunFrontendDisplayLoop, iterates all NPC racer groups (26 groups, stride 0xA4, base gNpcRacerGroupTable). For groups where byte[0] == 0, sets bit 1 in `DAT_004A2C9C[i]`. This likely unlocks retro/classic car variants.

### All cheats are toggles (XOR) and produce a sound effect (DXSound::Play 4 or 5) on activation.

---

## 5. DEAD SYMBOLS AND UNREFERENCED FEATURES

### Dead Mangled C++ Symbols (Zero xrefs)

| Address | Symbol | Implication |
|---|---|---|
| 0x4611E0 | `SNK_MusicTest_MT` | Cut music test screen menu title |
| 0x4616AE | `SNK_MusicTestButTxt` | Cut music test button text |
| 0x4611C0 | `SNK_CreditsText` | Cut credits scroll screen |
| 0x460F94 | `SNK_TimeDemoButTxt` | Cut timedemo button in main menu |
| 0x460F40 | `SNK_HiScoreButTxt` | Cut high score table button |
| 0x461B5E | `SNK_TracksUnlocked` | Cut track unlock notification string |
| 0x461B7C | `SNK_CarsUnlocked` | Cut car unlock notification string |
| 0x46121C | `SNK_NowPlayingTxt` | Cut "now playing" text for music test |

### Dead Data References (Zero xrefs)

| Address | String | Implication |
|---|---|---|
| 0x465910 | `// Created by SNK_Positioner\n\n` | Cut developer UI layout tool (writes positioner.txt) |
| 0x465930 | `positioner.txt` | Output filename for cut positioner tool |

### Active But Vestigial

| Address | String | Used By | Notes |
|---|---|---|---|
| 0x465944 | `Front End\\Positioner.tga` | RunFrontendDisplayLoop (0x415068) | Loaded as overlay background in frontend -- developer positioning grid |
| 0x4658A4 | `Lock failed in SNK_ScreenDump` | RunFrontendDisplayLoop (0x414D49) | Screen capture via surface lock -- active but locks/unlocks without saving |
| 0x466704 | `Lock failed in SNK_HalfBriteSurface` | CreateFrontendDisplayModeButton | 50% brightness effect for locked menu buttons |

---

## 6. NOOP HOOK STUB -- CUT FEATURE HOOK

`NoOpHookStub` (0x418450): Empty `RET` called from 13 sites:
- `RenderRaceActorForView`, `InitializeFrontendResourcesAndState`, `RunFrontendDisplayLoop`, `RunFrontendConnectionBrowser` (5 sites), `RenderFrontendSessionBrowser`, `RenderFrontendCreateSessionNameInput`, `RenderFrontendLobbyChatInput`, `RunRaceFrame`, `LoadEnvironmentTexturePages`

This was likely a debug/instrumentation hook point. In a debug build, it probably performed frame-by-frame logging, texture page validation, or developer overlay rendering. In the release build, all 13 call sites invoke a no-op.

---

## 7. BENCHMARK / FPS REPORT -- VESTIGIAL (State 3)

Game state 3 in `RunMainGameLoop` (0x442170) is an FPS benchmark image viewer:
- `WriteBenchmarkResultsTgaReport` (0x428D80): Generates a 640x480 TGA with system info (CPU, memory, OS, screen mode, texture memory), min/max/avg FPS graph, and driver version
- `RecordBenchmarkFrameRateSample` (0x428D40): Per-frame capture
- `InitializeBenchmarkFrameRateCapture` (0x428D20): Allocates sample buffer
- Output: `FPSName_exref` (configurable .tga filename)
- Activation: `g_benchmarkModeActive` flag, transitions to state 3 after race ends if flag is set
- The image viewer in state 3 loads and displays the TGA, waiting for any key to exit

This is a **developer/review tool** left in the release binary. The `SNK_TimeDemoButTxt` dead symbol suggests it was originally accessible from the main menu.

---

## 8. AUTO-DEMO / ATTRACT MODE -- ACTIVE

In `RunFrontendDisplayLoop` at the end: if the current screen is `ScreenMainMenuAnd1PRaceFlow` and 50 seconds of idle time elapses (50000ms) AND `DAT_0048f2fc < -15`, the game randomly selects a track and starts an automated race demo. This is a standard arcade-style attract mode. It IS functional.

---

## 9. IDLE POSITIONER TGA OVERLAY -- DEVELOPER TOOL REMNANT

`Positioner.tga` is loaded (0x415068) inside `RunFrontendDisplayLoop` as a frontend resource. Combined with the dead `positioner.txt` and `// Created by SNK_Positioner` strings, this was a developer tool for visually laying out menu element coordinates. The TGA was a grid/ruler overlay, and the tool would output coordinate data to `positioner.txt`. The output function was stripped but the TGA asset loading remained.

---

## Restoration Priority Summary

| Feature | Difficulty | Impact | Priority |
|---|---|---|---|
| Damage rules (rand() fix) | TRIVIAL (22 x 5-byte patches) | Visual collision effects | P0 |
| Snow rendering init | MEDIUM (60B cave + 5B hijack) | Snowfall particles on snow tracks | P1 |
| Snow rendering enable | EASY (6B NOP at 0x4465F4) | Required for snow to be visible | P1 |
| Benchmark via menu | EASY (set g_benchmarkModeActive) | FPS benchmarking tool | P2 |
| Music test screen | HARD (entire screen function missing) | Dev-only feature | P3 |
| Credits screen | HARD (entire screen function missing) | Low value | P3 |
| High score table | HARD (no save infrastructure) | Would need full implementation | P3 |
