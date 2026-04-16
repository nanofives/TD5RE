# Indirect Calls and Dispatch Tables in TD5_d3d.exe

Comprehensive analysis of all indirect call sites, function pointer tables,
switch/jump tables, callback patterns, and COM vtable dispatch in the binary.

---

## 1. Function Pointer Dispatch Tables

### 1.1 Screen Function Table (0x4655C4) -- KNOWN

30 valid code pointers indexed by `DAT_0049635c` (screen/game-mode ID).
Used throughout the frontend and game state machine.

| Index | Address    | Purpose (from context)         |
|-------|------------|--------------------------------|
|  0    | 0x004269D0 | Menu navigation handler        |
|  1    | 0x00415030 | Screen mode 1                  |
|  2    | 0x004275A0 | Screen mode 2                  |
|  3    | 0x00427290 | Screen mode 3                  |
|  4    | 0x004274A0 | Screen mode 4                  |
|  5    | 0x00415490 | Screen mode 5                  |
|  6    | 0x004168B0 | Screen mode 6                  |
|  7    | 0x004213D0 | Screen mode 7                  |
|  8    | 0x00418D50 | Screen mode 8                  |
|  9    | 0x00419CF0 | Screen mode 9                  |
| 10    | 0x0041A7B0 | Screen mode 10                 |
| 11    | 0x0041C330 | Screen mode 11                 |
| 12    | 0x0041D890 | Screen mode 12                 |
| 13    | 0x0041F990 | Screen mode 13                 |
| 14    | 0x0041DF20 | Screen mode 14                 |
| 15    | 0x0041EA90 | Screen mode 15                 |
| 16    | 0x00420400 | Screen mode 16                 |
| 17    | 0x00420C70 | Screen mode 17                 |
| 18    | 0x0040FE00 | Screen mode 18                 |
| 19    | 0x00418460 | Screen mode 19                 |
| 20    | 0x0040DFC0 | Screen mode 20                 |
| 21    | 0x00427630 | Screen mode 21                 |
| 22    | 0x00417D50 | Screen mode 22                 |
| 23    | 0x00413580 | Screen mode 23                 |
| 24    | 0x00422480 | Screen mode 24                 |
| 25    | 0x00413BC0 | Screen mode 25                 |
| 26    | 0x004237F0 | Screen mode 26                 |
| 27    | 0x00423A80 | Screen mode 27                 |
| 28    | 0x00415370 | Screen mode 28                 |
| 29    | 0x0041D630 | Screen mode 29                 |

Entries [30] = 0x7C00 and [31] = 0x3E0 are data values (RGB bitmasks), not pointers.

### 1.2 Screen Init Table (0x464104) -- NEW

Indexed by `DAT_0049635c` (game mode) in FUN_00410ca0. Called as
`(**(code **)(&DAT_00464104 + DAT_0049635c * 4))()` when `DAT_00490ba8 == 99`.

Entry [0] = 0x63 (value 99, sentinel/skip marker).

| Index | Address    | Purpose                         |
|-------|------------|---------------------------------|
|  1    | 0x00410F60 | Screen-mode-1 init (short stub) |
|  2    | 0x00410FA0 | Screen-mode-2 init              |
|  3    | 0x00410FF0 | Screen-mode-3 init              |
|  4    | 0x00411030 | Screen-mode-4 init              |
|  5    | 0x00411070 | Screen-mode-5 init              |
|  6    | 0x004110A0 | Screen-mode-6 init              |

Each is a small initialization stub that sets game-state globals for its mode.
Instruction at 0x410EB4: `CALL dword ptr [ECX*0x4 + 0x464104]`.

### 1.3 Translucent Dispatch Table (0x473B9C) -- KNOWN

7-entry rendering dispatch table for translucent/transparent polygon types.
Indexed by polygon type field from display list entries.

| Index | Address    | Function                       |
|-------|------------|--------------------------------|
|  0    | 0x00431750 | Translucent render type 0      |
|  1    | 0x00431750 | Same as type 0 (duplicate)     |
|  2    | 0x004316F0 | Translucent render type 2      |
|  3    | 0x00431690 | Translucent render type 3      |
|  4    | 0x0043E3B0 | Translucent render type 4      |
|  5    | 0x00431730 | Translucent render type 5      |
|  6    | 0x004316D0 | Translucent render type 6      |

Called at:
- 0x4314D8: `CALL dword ptr [EAX*0x4 + 0x473B9C]` in FUN_004314b0 (display list processor)
- 0x431380: `CALL dword ptr [ECX*0x4 + 0x473B9C]`
- 0x4315BA: `CALL dword ptr [ECX*0x4 + 0x473B9C]`

---

## 2. Switch/Jump Tables

100 compiler-generated jump tables found (switch statement dispatch).
Format: `JMP dword ptr [REG*4 + table_addr]`.

### Key Jump Tables (by functional area)

| JMP Instruction  | Table Address | Containing Function | Context                    |
|------------------|---------------|---------------------|----------------------------|
| 0x402306         | 0x40244C      | Game logic          | Early game-state switch    |
| 0x40249C         | 0x402920      | Game logic          | State dispatch             |
| 0x403ECB         | 0x404014      | Physics/control     | Control-mode switch        |
| 0x405A63         | 0x405B00      | Collision           | Collision-type switch      |
| 0x405FDE         | 0x406364      | Collision           | Response switch            |
| 0x406469         | 0x406618      | Rendering           | Render-type switch         |
| 0x408D48         | 0x408EC8      | AI system           | AI state machine           |
| 0x409FD0         | 0x40A24C      | AI system           | AI behavior dispatch       |
| 0x40B079         | 0x40B0EC      | D3D render states   | Render-state type switch   |
| 0x40B388         | 0x40B518      | D3D texturing       | Texture-mode switch        |
| 0x40B713         | 0x40B818      | D3D texturing       | Texture-filter switch      |
| 0x40E022         | 0x40F85C      | Track rendering     | Geometry-type switch       |
| 0x40FE39         | 0x410C34      | Screen handler      | Screen-mode switch         |
| 0x4100D7         | 0x410C78      | Screen handler      | Screen sub-mode            |
| 0x410D1B         | 0x410F30      | FUN_00410ca0        | Game-mode init switch      |
| 0x4132EA         | 0x41356C      | Frontend            | Menu-item switch           |
| 0x4135AC         | 0x413B90      | Frontend            | Options-menu switch        |
| 0x413C11         | 0x4145DC      | Frontend            | Extended options switch     |
| 0x415399         | 0x415474      | Frontend draw       | HUD-element type switch    |
| 0x4154C5         | 0x416824      | Frontend draw       | Large HUD switch           |
| 0x4168E2         | 0x417CD4      | Cup/championship    | Cup-state switch           |
| 0x416CDF         | 0x417D28      | Cup/championship    | Cup sub-state switch       |
| 0x417D65         | 0x418390      | Save/load           | Save-menu switch           |
| 0x41848F         | 0x418C34      | Save/load           | Extended save switch       |
| 0x418DA1         | 0x419B00      | Configuration       | Config-screen switch       |
| 0x419D22         | 0x41A508      | Configuration       | Input-config switch        |
| 0x41A7EC         | 0x41B32C      | Multiplayer         | MP-screen switch           |
| 0x41B6D7         | 0x41BCD4      | Multiplayer         | MP sub-state switch        |
| 0x41C382         | 0x41D5E8      | Multiplayer         | MP lobby switch            |
| 0x41D8BC         | 0x41DEDC      | Results/standings   | Results-screen switch      |
| 0x41E00C         | 0x41EA64      | Results/standings   | Extended results switch    |
| 0x41EABF         | 0x41F964      | Replay              | Replay-mode switch         |
| 0x41F9BD         | 0x4203B8      | Replay              | Replay sub-mode switch     |
| 0x42042D         | 0x420C38      | Car selection       | Car-select switch          |
| 0x420C9E         | 0x4213A8      | Track selection     | Track-select switch        |
| 0x421404         | 0x421D78      | Track selection     | Track sub-state switch     |
| 0x421F2B         | 0x422468      | 4-player setup      | 4P-setup switch            |
| 0x4224AF         | 0x423750      | Menu/frontend       | Main-menu switch           |
| 0x428A7A         | 0x428C8C      | Force feedback      | FF effect-type switch      |
| 0x42E9D9         | 0x42EA58      | Car physics         | Surface-type switch        |
| 0x4301F5         | 0x430A7C      | Rendering           | Render-pass switch         |
| 0x433D59         | 0x433F98      | 3D rendering        | Polygon-type switch        |
| 0x4343D0         | 0x434588      | 3D rendering        | Vertex-format switch       |
| 0x4355AB         | 0x4358FC      | 3D rendering        | Draw-command switch        |
| 0x437416         | 0x437780      | Track geometry      | Track-section switch       |
| 0x4397DD         | 0x439B44      | Weather/effects     | Weather-type switch        |
| 0x43C7F4         | 0x43C8F8      | Movie/FMV           | Movie player state switch  |
| 0x4411A4         | 0x441A60      | Sound system        | Sound-event switch         |
| 0x44218E         | 0x442550      | Sound system        | Sound-channel switch       |
| 0x442E63         | 0x4431A0      | HUD/minimap         | HUD-draw switch            |
| 0x444345         | 0x445420      | Physics             | Vehicle-dynamics switch    |
| 0x44BD05+        | 0x44BE18      | CRT library         | C runtime switches         |
| 0x44AF96         | 0x44B649      | CRT library         | strtol/atoi switch         |
| 0x44F5B5+        | 0x44F6C8      | CRT library         | printf format switch       |

---

## 3. COM/DirectX Vtable Indirect Calls

The binary makes extensive use of DirectX6 COM interfaces through vtable dispatch.
All follow the pattern: `(**(code **)(*obj + offset))(obj, ...)`.

### 3.1 IDirect3DDevice (via d3d_exref + 0x38)

The D3D device pointer is at `*(int **)(d3d_exref + 0x38)`.
Vtable offsets used (IUnknown base = 0, then IDirect3DDevice methods):

| Vtable Offset | D3D Method                | Call Sites (sample)                  |
|---------------|---------------------------|--------------------------------------|
| +0x08         | Release                   | 0x411E84, 0x411ED0                   |
| +0x34         | Restore (WaitForVBlank?)  | 0x411E68, 0x411E77, 0x411EB3         |
| +0x58         | SetRenderState            | 0x40AF75, 0x40B042..0x40B7B2 (many) |
| +0x64         | SetTransform / GetCaps    | 0x40CE5E, 0x411829, many rendering  |
| +0x6C         | SetTexture?               | 0x414B74, 0x42540B                   |
| +0x74         | DrawPrimitive             | 0x4122C3, 0x412588, 0x431415+        |
| +0x80         | DrawIndexedPrimitive      | 0x40D0EE, 0x411A0C, many rendering  |
| +0x98         | SetTexture (stage)        | 0x40B80E (in FUN_0040b660)           |

**Total vtable call sites through D3D device:** ~80+

### 3.2 IDirectDrawSurface (via DAT_00496260)

| Vtable Offset | DD Method            | Call Sites                              |
|---------------|----------------------|-----------------------------------------|
| +0x08         | Release              | 0x43C412, 0x43C8D5                      |
| +0x14         | SetDisplayMode/Blt   | 0x423E32, 0x423EC2, 0x425E44+           |
| +0x1C         | Flip / BltFast       | 0x40D9CE, 0x42435A, 0x424279, many      |
| +0x34         | WaitForVerticalBlank | 0x424EC9, 0x424FA4                      |

### 3.3 IDirectDrawPalette / IDirectSound (via DAT_004bea98 / DAT_004beaac)

Used in the movie player (FUN_0043c440):
- `(**(code **)(*DAT_004bea98 + 0x14))()` -- DDraw overlay create
- `(**(code **)(*DAT_004beaac + 0x14))()` -- DSound buffer create
- `(**(code **)(*DAT_004beaac + 0x08))()` -- DSound Release
- `(**(code **)(*DAT_004bea98 + 0x08))()` -- DDraw Release

### 3.4 IDirectSoundBuffer (at js_exref + offset)

Force feedback / joystick effects accessed via:
- `CALL dword ptr [EDX + 0x48]` at 0x4286B5, 0x428751, 0x4287AA, 0x42885B

### 3.5 COM/IMediaPlayer (at DAT_00452xxx range)

In the EA TGQ multimedia engine (FUN_004529d0):
- `(**(code **)(*piVar3 + 0x24))(piVar3, ...)` -- QueryStatus
- `(**(code **)(**(int **)(... + 0x5c) + 0x30))(...)` -- IOverlay::Advise
- `(*(code **)(*piVar1 + 0xB0))` -- Callback function pointer

### 3.6 Additional COM vtable patterns

Low-address COM calls in DirectDraw/overlay subsystem (0x452xxx-0x456xxx range):
- +0x0C, +0x10, +0x14, +0x18, +0x24, +0x2C, +0x30, +0x3C, +0x44, +0x4C,
  +0x54, +0x58, +0x60, +0x64, +0x6C, +0x74, +0x7C, +0x80

These represent IDirectDraw, IDirectDrawSurface, IDirectDrawClipper,
IOverlay, and IVideoPort interfaces.

---

## 4. Callback / Function Pointer Registration Patterns

### 4.1 CRT Callback Globals

| Global Address | Type           | Set By / Used At                     |
|----------------|----------------|--------------------------------------|
| PTR_FUN_0047B200 | void(*)()    | CRT init callback; checked at 0x44E6F1 |
| PTR_FUN_0047B204 | void(*)()    | CRT thread-init callback; 0x45C08A   |
| DAT_004CF200   | code*          | CRT exit callback; called at 0x44EE3B |
| DAT_004CEFF8   | code*          | Signal handler callback; 0x449B4B     |
| DAT_004CF270   | FARPROC        | Lazy-loaded MessageBoxA; 0x450C2A     |
| DAT_004CF274   | FARPROC        | Lazy-loaded GetActiveWindow; 0x450C3C  |
| DAT_004CF278   | FARPROC        | Lazy-loaded GetLastActivePopup        |

### 4.2 WndProc / App Callback

| Global Address         | Type       | Purpose                              |
|------------------------|------------|--------------------------------------|
| app_exref + 0x138      | WNDPROC    | Window procedure; set to LAB_0043C3C0 in movie player |
| _DAT_004BCC40          | code*      | Movie event callback; set to LAB_0043C970 |

### 4.3 Function Pointer Parameters (passed on stack/register)

- FUN_00406CC0 (`param_2`): Rendering callback passed as function pointer.
  Called as `(*(code *)param_2)(puVar5)` at 0x406E07, 0x406F27.
  This is a per-polygon rendering dispatch for the collision/track system.

- FUN_00406CC0 sibling at 0x4070A7, 0x407237: Same pattern, callback via
  `[ESP+0x54]` / `[ESP+0x58]`.

### 4.4 Dynamic API Loading (GetProcAddress pattern)

FUN_00449581 at 0x4495A2:
```c
hModule = GetModuleHandleA("KERNEL32");
pFVar1 = GetProcAddress(hModule, "IsProcessorFeaturePresent");
(*pFVar1)(0);  // CALL EAX at 0x4495A2
```

FUN_00450BD2 at 0x450C2A/0x450C3C:
```c
// Lazy-loads user32.dll MessageBox and friends
DAT_004cf270 = GetProcAddress(hModule, "MessageBoxA");
DAT_004cf274 = GetProcAddress(hModule, "GetActiveWindow");
DAT_004cf278 = GetProcAddress(hModule, "GetLastActivePopup");
(*DAT_004cf274)();   // CALL EAX at 0x450C2A
(*DAT_004cf278)();   // CALL EAX at 0x450C3C
(*DAT_004cf270)();   // CALL EAX at 0x450C2A
```

### 4.5 EA TGQ Multimedia Callback

FUN_004529D0 (multimedia streaming thread):
- `pcVar2 = *(code **)(*piVar1 + 0xB0)` -- callback function pointer stored
  in the multimedia player structure at offset +0xB0.
- Called as `(*pcVar2)()` at 0x452B43 if non-null.

### 4.6 Play_exref Callback (Menu Sound)

FUN_00426580 (menu navigation):
- `(*pcVar6)(2)` / `(*pcVar6)(1)` / `(*pcVar6)(3)` at 0x426873, 0x4268E4, etc.
- `pcVar6` is loaded from `Play_exref` (DXSound::Play function pointer).
- Used to play navigation/click sounds from the menu system.

---

## 5. M2DX.DLL Import Table (IAT at 0x45D3C8-0x45D5C0+)

All `CALL dword ptr [0x0045Dxxx]` instructions are calls through the Import
Address Table to M2DX.DLL (the game's DirectX wrapper library).

### Most-referenced IAT slots:

| IAT Address  | Import                        | Call Count |
|--------------|-------------------------------|------------|
| 0x0045D528   | DXD3D::SetRenderState         | ~30+       |
| 0x0045D568   | DXD3D::??? (rendering)        | ~30+       |
| 0x0045D56C   | DXD3D::??? (rendering)        | ~20+       |
| 0x0045D560   | DXD3DTexture::???             | ~10+       |
| 0x0045D558   | DXDraw::ClearBuffers?         | ~8         |
| 0x0045D540   | DXSound::???                  | ~6         |
| 0x0045D544   | DXD3D::TextureClamp?          | ~3         |
| 0x0045D4CC   | DXPlay::???                   | ~10+       |
| 0x0045D5C0   | DXWin::CleanUpAndPostQuit     | ~6         |
| 0x0045D3C8   | DXD3D::BeginScene             | 1 (0x40ADE0) |
| 0x0045D570   | DXDraw::Flip                  | 1 (0x40AE00) |
| 0x0045D578   | DXD3D::InitializeMemoryMgmt   | 2          |
| 0x0045D580   | DXD3D::FullScreen             | 1          |
| 0x0045D52C   | DXD3D::GetMaxTextures?        | ~5         |
| 0x0045D520   | DXErrorToString?              | 1          |
| 0x0045D51C   | DXD3DTexture::LoadRGB?        | ~4         |

Total distinct IAT slots used: ~40 (out of ~112 imports from M2DX.DLL).
Total IAT indirect call sites: 200+ (from CALLIND p-code search, capped at limit).

---

## 6. Register Indirect Calls Summary

### CALL EAX (14 sites)
- 0x42B61F, 0x42B6BF: In main game loop FUN_0042B580 (no callback, likely inlined)
- 0x4495A2: GetProcAddress("IsProcessorFeaturePresent") dispatch
- 0x449B4B: Signal handler callback `(*DAT_004CEFF8)(param_1)`
- 0x44E6F1, 0x44E78E, 0x44E7FF: CRT init/exit callbacks
- 0x44EE3B: CRT exit callback `(*DAT_004CF200)()`
- 0x450C2A, 0x450C3C: Lazy-loaded user32.dll functions
- 0x452B43: EA TGQ multimedia callback
- 0x45C08A, 0x45C0DB, 0x45C15D: CRT thread-start callback

### CALL EBX (33 sites)
- 0x411EC4: COM Release in DirectDraw cleanup
- 0x4170E5+: Frontend drawing (menu item render callbacks)
- 0x424F40+: DirectDraw surface operations (WaitForVBlank in loop)
- 0x4266D8+: Menu navigation (DXSound::Play indirect)
- 0x42C506: Input processing
- 0x430B86: Rendering pipeline
- 0x43157A: Translucent rendering
- 0x441C8F: Sound processing
- 0x44E91C, 0x44E92B: CRT atexit chain
- 0x452263+: Multimedia streaming
- 0x452639+: Multimedia streaming
- 0x45295F: Multimedia streaming
- 0x455D6B+: DirectDraw overlay
- 0x4560EF: Video port

### CALL ECX (0 sites) -- none found

### CALL EDX (0 sites) -- none found

### CALL ESI (50+ sites)
Concentrated in:
- 0x40AF91-0x40B052: D3D fog render-state setting (repeated pattern with EDI)
- 0x40BAF9-0x40BB62: D3D texture state
- 0x40E539, 0x40E53D: Render callback
- 0x410AE9: Screen handler callback
- 0x414749+: Frontend rendering
- 0x428EB6-0x4293FE: Force feedback / DXInput (~25 calls -- COM interface dispatch)
- 0x43C7B6: Movie player
- 0x442525+: Sound system
- 0x449FF6: CRT

### CALL EDI (47 sites)
- 0x40AF89-0x40B04A: Paired with ESI in D3D render-state functions
- 0x41D257: Results screen
- 0x428AE8-0x428C80: Force feedback effect dispatch
- 0x42C500: Input handler
- 0x430B61+: Rendering
- 0x43C7C8: Movie player
- 0x440BA2+: Sound engine (4 consecutive calls at 0x440C01-0x440C0D)
- 0x441D5E+: Sound system
- 0x442326+: Sound mixing
- 0x448A73+: CPU detection
- 0x44E5E9+: CRT locale
- 0x44ED71+: CRT environment
- 0x44FE00+: CRT file I/O
- 0x4509C6+: CRT heap
- 0x452873+: Multimedia
- 0x452950+: Multimedia streaming
- 0x452EF6+: Video port
- 0x4560DB+: DirectDraw overlay

### CALL EBP (28 sites)
- 0x411EBB: DirectDraw surface cleanup
- 0x414701, 0x414E35: Frontend render callbacks
- 0x41A9F4-0x41B637: Multiplayer screen handlers (~6 calls)
- 0x4265B7-0x42664F: Menu navigation (Play_exref callback)
- 0x42C92D-0x42C9BB: Input configuration
- 0x430B91: Rendering
- 0x43C866: Movie player
- 0x44A5C1+: CRT exception
- 0x44ECF9+: CRT environment
- 0x451C8F+: CRT file
- 0x452A87-0x452C1F: Multimedia streaming

---

## 7. Notable Patterns and Observations

### 7.1 No vtable-like structures (C code confirmed)
The binary is compiled from C (not C++). No constructor/destructor patterns or
vtable pointer initialization was found. All "vtable" patterns are COM interface
dispatch through DirectX.

### 7.2 Dominant indirect call pattern
The overwhelming majority of indirect calls (~200+ from IAT, ~180+ COM vtable)
are either:
1. M2DX.DLL import calls (IAT at 0x45D3C8-0x45D5C0)
2. DirectX COM vtable dispatch (IDirect3DDevice, IDirectDraw, IDirectSound)

Only ~20-30 indirect calls are actual game-logic function pointer dispatch.

### 7.3 Function pointer callback parameter pattern
FUN_00406CC0 uses a callback parameter (`param_2`) for per-polygon rendering.
This is the only function found that takes a function pointer as a parameter
for game-specific logic (as opposed to Win32 API callbacks like WndProc).

### 7.4 Register call conventions
- ESI/EDI pairs: Used in D3D render-state setting where ESI and EDI hold two
  different function pointers called in alternation (e.g., SetRenderState +
  Error handler).
- EBP: Used for stable callback references across loops (menu navigation,
  multiplayer screens).
- EBX: Used for COM interface methods in loops (WaitForVBlank retry, atexit chain).

### 7.5 Lazy-loaded APIs
Only two cases of runtime GetProcAddress:
1. KERNEL32::IsProcessorFeaturePresent (CPU feature detection)
2. user32.dll MessageBoxA/GetActiveWindow/GetLastActivePopup (error dialogs)

---

## 8. Complete Dispatch Table Index

| Table Address | Size  | Type       | Caller(s)                    |
|---------------|-------|------------|------------------------------|
| 0x004655C4    | 30x4  | Code ptrs  | Screen function table        |
| 0x00464104    | 6x4   | Code ptrs  | Screen init table            |
| 0x00473B9C    | 7x4   | Code ptrs  | Translucent render dispatch  |
| 0x0040244C    | var   | Jump table | switch at 0x402306           |
| 0x00402920    | var   | Jump table | switch at 0x40249C           |
| 0x00404014    | var   | Jump table | switch at 0x403ECB           |
| ... (100 total jump tables, see Section 2)                          |

---

## 9. Callback Registration Sites

| Address Written | What                  | Where Set              |
|-----------------|-----------------------|------------------------|
| app+0x138       | WndProc               | FUN_0043C440 (movie)   |
| DAT_004BCC40    | Movie event callback  | FUN_0043C440           |
| DAT_004CF200    | CRT exit callback     | CRT init code          |
| DAT_004CF270    | MessageBoxA FARPROC   | FUN_00450BD2 (lazy)    |
| DAT_004CF274    | GetActiveWindow       | FUN_00450BD2 (lazy)    |
| DAT_004CF278    | GetLastActivePopup    | FUN_00450BD2 (lazy)    |
| DAT_004CEFF8    | Signal handler        | CRT signal setup       |
| PTR_FUN_0047B200| CRT init callback     | CRT startup            |
| PTR_FUN_0047B204| CRT thread callback   | CRT thread startup     |
| piVar1+0xB0     | TGQ stream callback   | Multimedia init        |
