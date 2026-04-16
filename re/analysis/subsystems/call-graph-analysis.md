# TD5_d3d.exe Call Graph Analysis

**Date**: 2026-03-28
**Total functions**: 822 (Ghidra-identified)
**Game code range**: 0x401000 - 0x446F00 (~460 game functions)
**CRT/library range**: 0x447000 - 0x45C169 (~362 CRT/runtime functions)

---

## 1. Program Entry Point and Top-Level Call Chain

The main execution spine is clean and linear:

```
entry (0x4493E0) — CRT startup
  -> FUN_0045c140 (0x45C140) — WinMain wrapper
    -> thunk_FUN_0045c140 (0x430A90) — thunk jump
      -> FUN_00430a95 (0x430A95) — GameWinMain (real implementation)
        -> FUN_00442170 (0x442170) — Main game state machine / outer loop
          -> FUN_0042aa10 (0x42AA10) — InitializeRaceSession
          -> FUN_0042b580 (0x42B580) — RunRaceFrame (returns to loop)
          -> FUN_0042c8e0 (0x42C8E0) — post-race cleanup
          -> FUN_0043c440 (0x43C440) — D3D device/mode setup
          -> FUN_00414740 (0x414740) — renderer init
          -> FUN_00414b50 (0x414B50) — texture/asset loading
```

**GameWinMain** (0x430A95) calls only 2 internal functions:
- `thunk_FUN_0042a950` (0x442160) — shutdown/cleanup path
- `FUN_00442170` (0x442170) — the main game state machine

**FUN_00442170** is the true orchestrator, calling 21 functions including external DLL calls (InitializeMemoryManagement, SetRenderState, FOpen, Flip, ImageProTGA, etc.).

---

## 2. Critical Path Depths

### GameWinMain -> deepest leaf
**Path**: GameWinMain(0x430A95) -> FUN_00442170 -> RunRaceFrame(0x42B580) -> FUN_00409150 -> FUN_00408a60 -> FUN_004079c0
- **Depth: 6 levels** from GameWinMain to the deepest traced game function

### RunRaceFrame (0x42B580) — the per-frame workhorse
This function has **56 direct callees** — making it the widest fan-out function in the game. Key sub-chains:

| Sub-path | Depth from RunRaceFrame | Purpose |
|----------|------------------------|---------|
| -> FUN_00409150 -> FUN_00408a60 -> FUN_004079c0 | 3 | Texture/model loading pipeline |
| -> FUN_0040bd20 -> FUN_0040c120 -> FUN_004011c0 | 3 | Rendering (mesh building, with FPU math) |
| -> FUN_0040bd20 -> FUN_0040c120 -> FUN_0040c7e0 | 3 | Additional render pass |
| -> FUN_004388a0 -> FUN_004397b0 | 2 | Physics/car simulation |
| -> FUN_004388a0 -> FUN_0043a220 | 2 | Physics sub-computation |
| -> FUN_00436a70 -> FUN_00406650 -> FUN_00405e80 | 3 | Track/3D object processing |
| -> FUN_00436a70 -> FUN_00434da0 | 2 | Race progression/scoring |
| -> FUN_0042c470 -> FUN_00401450 | 2 | HUD/overlay rendering |
| -> FUN_00428d80 -> FUN_00428d60 | 2 | Benchmark report (WriteBenchmarkResultsTgaReport) |

**Maximum call depth from RunRaceFrame: 4 levels** (to leaf functions like FUN_004011c0, FUN_004079c0).
**Maximum call depth from GameWinMain: 6 levels** to the deepest leaf.

### InitializeRaceSession (0x42AA10)
Has **62 direct callees** — even wider than RunRaceFrame. This is the pre-race setup function that initializes everything:
- External DLL calls: ReadOpen, WriteOpen, SetConfiguration, FullScreen, CDPlay, CanFog, timeGetTime
- Internal: sound init, track loading, car physics setup, AI configuration, renderer state
- Calls into: FUN_00437ba0 (thiscall, likely a class init), FUN_0043c9e0, FUN_0043d2d0, FUN_00432e60

---

## 3. Most-Called Hub Functions (by caller count)

| Function | Address | Callers | Role |
|----------|---------|---------|------|
| `__ftol` | 0x44817C | 31 | Float-to-long conversion (CRT) |
| `_malloc` | 0x4481A3 | 26 | Memory allocation |
| `_rand` | 0x448157 | 22 | Random number generation |
| `FUN_00440790` | 0x440790 | 11 | INI/config file reader (GetPrivateProfileString wrapper) |
| `FUN_00418450` | 0x418450 | 8 | Direct3D rendering primitive (empty body — likely nop/passthrough) |
| `FUN_0042ce90` | 0x42CE90 | 5 | Math utility (vertex/coordinate transforms) |
| `FUN_0042c8d0` | 0x42C8D0 | 4 | Data accessor/getter (tiny function) |
| `FUN_00406980` | 0x406980 | 3 | Texture/surface handling |
| `FUN_00412030` | 0x412030 | 3 | File loading (takes LPCSTR paths) |
| `FUN_0040ac00` | 0x40AC00 | 3 | Sound/audio initialization |

**Key observation**: `FUN_00418450` at 0x418450 has an empty body (body_start == body_end) yet is called 8 times. This is a no-op function placeholder, likely a removed debug function or conditional compile stub.

---

## 4. Orphan Functions (No Direct Callers — Potential Dead Code)

### 4.1 Truly Unreachable (no callers AND no xrefs)

No functions in the game code range were found to be completely without any references at all. Every function checked via `reference_to` has at least one UNCONDITIONAL_CALL reference, even when `function_callers` returned 0. This is because some callers exist in address ranges not recognized as functions by Ghidra (likely inlined code or functions Ghidra split differently).

### 4.2 Functions with 0 callers but reachable via xrefs

These functions have no registered caller in Ghidra's function database but DO have call references from other code:

| Function | Address | Xref Count | Called From (addresses) | Notes |
|----------|---------|-----------|------------------------|-------|
| FUN_0040aad0 | 0x40AAD0 | 1 | 0x422599 | Called from unlisted function range |
| FUN_0040ab80 | 0x40AB80 | 1 | 0x422592 | Called from unlisted function range |
| FUN_0040dac0 | 0x40DAC0 | 14 | 0x41dec2, 0x421d4e, many more | **Heavily used** — rendering subsystem utility |
| FUN_0040ddc0 | 0x40DDC0 | 13 | 0x40e726, 0x40ea90, many more | **Heavily used** — rendering subsystem utility |
| FUN_0040fb60 | 0x40FB60 | 1 | 0x426fdd | D3D setup function |
| FUN_00410ca0 | 0x410CA0 | 3 | 0x422f5d, 0x417596, 0x416d0a | Renderer feature detection |
| FUN_00411780 | 0x411780 | 2 | 0x427575, 0x4275fa | Lighting/material setup |
| FUN_00411a70 | 0x411A70 | 1 | 0x427522 | Lighting setup |
| FUN_00413010 | 0x413010 | 3 | 0x4136e2, 0x41394a, 0x41421f | Asset processing |
| FUN_00419b30 | 0x419B30 | 4 | 0x419e1c, 0x419f06, more | UI/frontend related |
| FUN_0041a530 | 0x41A530 | 4 | 0x413eeb, 0x41a9da, more | UI/asset related |
| FUN_0041b390 | 0x41B390 | 2 | 0x4280f7, 0x40f7d2 | 3D object processing |
| FUN_0041b610 | 0x41B610 | 6 | 0x40f836, 0x41c8ab, more | **Heavily used** — 3D render calls |
| FUN_0041bd00 | 0x41BD00 | 1 | 0x41c9fd | 3D render sub-function |
| FUN_0041c030 | 0x41C030 | 1 | 0x41ca76 | 3D render sub-function |
| FUN_00421da0 | 0x421DA0 | 1 | 0x4234f3 | UI/screen rendering |
| FUN_0042a950 | 0x42A950 | 1 (jump) | 0x442160 (thunk) | **Shutdown/cleanup** — called via thunk only |

**All of these are reachable code.** The reason Ghidra reports 0 callers is that the calling code lives in address ranges where Ghidra has not created function boundaries (gap regions between named functions, or code in the 0x414F40-0x418450 range and 0x41C320-0x41D840 range that lacks function definitions).

### 4.3 WriteBenchmarkResultsTgaReport (0x428D80) — NOT Dead Code

- **Callers**: 1 (RunRaceFrame at 0x42B580)
- **Callees**: FUN_00428d60 (FormatBenchmarkReportText), timeGetTime, malloc, file I/O
- **Status**: Called conditionally during race frames. The benchmark system IS reachable in normal gameplay, likely triggered by a config flag or command-line argument.

### 4.4 FormatBenchmarkReportText (0x428D60) — NOT Dead Code

- **Callers**: 1 (WriteBenchmarkResultsTgaReport at 0x428D80)
- **Status**: Reachable through the benchmark path above.

### 4.5 ScreenPositionerDebugTool (0x415030) — No Function At This Address

Ghidra has no function defined at 0x415030. This address falls in the gap between FUN_00414f34 (ends) and FUN_004183b0 (starts at 0x4183B0). This entire ~13KB region (0x414F40-0x4183AF) likely contains **multiple unlisted functions** including potential debug tools. The callers to addresses like 0x40dac0 and 0x41b610 originate from this region, confirming active code exists here that Ghidra failed to auto-analyze into functions.

---

## 5. Function Pointer Table References

### FUN_00418450 (0x418450) — D3D Render Nop/Dispatch Point

This function has an empty body (0 bytes) and is called 8 times from diverse rendering locations. Referenced from:
- FUN_0040c120 (mesh rendering)
- FUN_00414740 (renderer init)
- FUN_00414b50 (texture loading)
- FUN_00419b30 (UI rendering)
- FUN_0041a530, FUN_0041a670 (UI subsystems)
- FUN_0042b580 (RunRaceFrame)
- FUN_0042f990 (post-race)

This appears to be a **virtual function table dispatch point** or a compiled-out debug hook.

### thunk_FUN_0045c140 (0x430A90) -> FUN_0045c140 (0x45C140)

The WinMain entry is a thunk jump, suggesting the actual WinMain implementation was in a different compilation unit and linked via an import-style pattern.

### thunk_FUN_0042a950 (0x442160) -> FUN_0042a950 (0x42A950)

Shutdown/cleanup is also reached via thunk, called from GameWinMain. This is the orderly shutdown path when the game exits the message loop.

### External DLL Function Pointer Imports

The game heavily uses function pointer tables for its engine DLL (m2dx/dxdraw):
- External calls: StopEffects, ResetConfiguration, ReadOpen, WriteOpen, SetConfiguration, FullScreen, CDPlay, CanFog, InitializeMemoryManagement, SetRenderState, FOpen, FRead, FClose, Flip, ImageProTGA, LogReport, Allocate, DeAllocate, Msg, DXErrorToString, DXInputGetKBStick, Initialize, Uninitialize, Environment, ConfirmDX6, DXInitialize, CleanUpAndPostQuit, ReleaseMsg

These are resolved through the PE import table and represent the m2dx engine API.

---

## 6. Unreachable Code Paths — Evidence

### Gap Region: 0x414F40 - 0x4183AF (~13KB)

This region contains code that calls well-known game functions (0x40dac0, 0x41b610, 0x411780, etc.) but Ghidra has not created function entries for it. This is likely:
- Frontend screen rendering code
- Debug/diagnostic visualization code
- Code compiled with different optimization settings

**Evidence**: Multiple xrefs originate from addresses in this range (0x415c01, 0x416d0a, 0x417596, 0x4175fa, etc.) calling into known rendering functions.

### Gap Region: 0x41C320 - 0x41D840 (~5.5KB)

Similarly contains active code. References from 0x41c8ab, 0x41c974, 0x41c9fd, 0x41ca76 into rendering functions FUN_0041b610, FUN_0041bd00, FUN_0041c030.

### Gap Region: 0x41D890 - 0x421D9F (~17KB)

Large unanalyzed region. References from addresses like 0x41dec2, 0x41ea38, 0x41f944, 0x42039c, 0x420c1c, 0x421388, 0x421d4e call into FUN_0040dac0. This region likely contains the bulk of the **3D scene rendering pipeline**.

### Gap Region: 0x422465 - 0x423DAF (~6.5KB)

References from 0x422592, 0x422599, 0x422f5d into game functions. Likely frontend/menu code.

### Gap Region: 0x426580 - 0x428240 (~7.2KB)

References from 0x426fdd, 0x4275fa, 0x427575, 0x427522, 0x427608, 0x4280f7, 0x42810c. This region calls into D3D setup (FUN_0040fb60), lighting (FUN_00411780, FUN_00411a70), and rendering functions.

---

## 7. Summary Statistics

| Metric | Value |
|--------|-------|
| Total functions | 822 |
| Game logic functions (0x401000-0x446F00) | ~460 |
| CRT/runtime functions (0x447000+) | ~362 |
| Functions with 0 callers (game range, checked) | ~16 |
| Functions with 0 callers but reachable via xrefs | 16/16 (100%) |
| True dead code functions found | **0** (all checked functions have xrefs) |
| RunRaceFrame direct callees | 56 |
| InitializeRaceSession direct callees | 62 |
| Max call depth (GameWinMain to leaf) | 6 |
| Unanalyzed code gap regions | ~49KB across 5+ regions |
| Widest hub (game code) | FUN_00440790 (config reader) — 11 callers |
| Widest hub (CRT) | __ftol — 31 callers |

### Key Findings

1. **No true dead code detected** in the sampled functions. Every function is reachable, either through direct calls or through xrefs from gap regions.

2. **~49KB of unanalyzed code** exists in gap regions that Ghidra did not auto-create functions for. This code IS actively referenced and needs manual function creation in Ghidra.

3. **The benchmark system is live** — WriteBenchmarkResultsTgaReport and FormatBenchmarkReportText are called from RunRaceFrame, not dead code.

4. **FUN_00418450 is suspicious** — an empty-body function called 8 times, likely a compiled-out debug hook or an intentional no-op placeholder.

5. **RunRaceFrame (0x42B580) is the critical hot path** with 56 callees covering rendering, physics, AI, HUD, sound, and race logic — all in a single frame tick.

6. **InitializeRaceSession (0x42AA10) is the widest setup function** with 62 callees handling all pre-race initialization.

7. **The main game state machine (0x442170)** is surprisingly simple — it calls init, then loops RunRaceFrame, with cleanup on exit. The complexity is pushed down into the callee trees.
