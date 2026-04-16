# File Reorganization Plan — Moving Game Data to Subfolders

## Goal

Move all game data files out of the root directory into a `data\` subfolder, leaving only
executables and DLLs in root. This eliminates the flat clutter of 50+ files.

## Target Layout

```
root/
├── TD5_d3d.exe          (main executable)
├── M2DX.dll             (engine DLL — PE IAT import, must stay in root)
├── M2DXFX.dll           (effects DLL — loaded by M2DX, must stay in root)
├── Language.dll          (strings DLL — PE IAT import, must stay in root)
├── ddraw.dll            (dgVoodoo2 wrapper — DLL hijack, must stay in root)
├── dgVoodoo.conf        (dgVoodoo2 config — looked up next to ddraw.dll)
│
└── data/
    ├── Config.td5
    ├── CupData.td5
    ├── level001.zip … level039.zip    (19 track archives)
    ├── static.zip                      (shared models/textures)
    ├── traffic.zip                     (traffic vehicle models)
    ├── environs.zip                    (environment billboards)
    ├── LOADING.ZIP                     (loading screens)
    ├── LEGALS.ZIP                      (legal/credits)
    ├── cars/                           (37 car archives)
    │   ├── day.zip … atp.zip
    │   └── ...
    ├── Front End/                      (frontend UI)
    │   ├── FrontEnd.zip
    │   ├── MenuFont.tga
    │   ├── Extras/ (Extras.zip, Mugshots.zip)
    │   ├── Sounds/ (Sounds.zip)
    │   └── Tracks/ (Tracks.zip)
    ├── sound/
    │   └── SOUND.ZIP
    └── movie/
        └── intro.tgq
```

---

## Critical Finding: What Needs vs Doesn't Need Patching

### DO need patching — Filesystem paths (opened via fopen/CreateFileA/mmioOpen)

These are paths the game passes to file-open APIs to access the filesystem:

| # | VA | String | Len | Xrefs | Used by |
|---|---|---|---|---|---|
| 1 | 0x467294 | `level%03d.zip` | 15 | 4 | LoadTrackRuntimeData, LoadTrackTextureSet, LoadRaceTexturePages |
| 2 | 0x467268 | `static.zip` | 11 | 7 | InitializeRaceSession, LoadStaticTrackTextureHeader, LoadTrackTextureSet, LoadRaceTexturePages |
| 3 | 0x46727C | `STATIC.ZIP` | 11 | 1 | InitializeRaceSession (fallback) |
| 4 | 0x4672A4 | `LOADING.ZIP` | 12 | 2 | InitializeRaceSession |
| 5 | 0x467300 | `LEGALS.ZIP` | 11 | 4 | ShowLegalScreens |
| 6 | 0x473B18 | `environs.zip` | 14 | 2 | LoadEnvironmentTexturePages |
| 7 | 0x474BB0 | `SOUND\SOUND.ZIP` | 16 | 3 | LoadRaceAmbientSoundBuffers |
| 8 | 0x474DD8 | `traffic.zip` | 12 | 3 | LoadTrafficVehicleSkinTexture, LoadRaceVehicleAssets |
| 9 | 0x474840 | `Movie\intro.tgq` | 17 | 1 | PlayIntroMovie (via mmioOpenA, NOT fopen) |
| 10 | 0x463FB0 | `Config.td5` | 12 | 2 | WritePackedConfigTd5, LoadPackedConfigTd5 |
| 11 | 0x464120 | `CupData.td5` | 13 | 4 | WriteCupData, LoadContinueCupData, ValidateCupDataChecksum, ScreenCupWonDialog |
| 12 | 0x463F84 | `Front End\FrontEnd.zip` | 23 | 47 | 20+ Screen* functions, InitializeFrontendResourcesAndState |
| 13 | 0x463D1C | `Front End\Extras\Extras.zip` | 29 | 10 | LoadExtrasGalleryImageSurfaces, LoadExtrasBandGalleryImages |
| 14 | 0x465DF0 | `Front End\Extras\Mugshots.zip` | 31 | 27 | ScreenExtrasGallery, LoadFrontendExtrasGalleryResources |
| 15 | 0x4656F4 | `Front End\Sounds\Sounds.zip` | 29 | 1 | LoadFrontendSoundEffects |
| 16 | 0x4669BC | `Front End\Tracks\Tracks.zip` | 29 | 1 | TrackSelectionScreenStateMachine |
| 17–53 | 0x466FF0–0x467230 | `cars\XXX.zip` (×37) | 13–14 | via ptr table | LoadRaceVehicleAssets (table at 0x466EEC) |

**Total: 53 unique filesystem path strings, 119+ code xrefs**

### Also in M2DX.dll

| VA | String | Used by |
|---|---|---|
| 0x1002414C | `TD5.log` | M2DX.dll log writer |

### DO NOT need patching — Archive entry names

These are filenames looked up INSIDE ZIP archives, not filesystem paths.
Confirmed by code pattern: `OpenArchiveFileForRead(entry_name, archive_path)`:

- All `Front End\*.tga` strings (~65) — entries inside FrontEnd.zip
- All `Front End\Sounds\*.wav` strings (6) — entries inside Sounds.zip
- All `Front End\Extras\*.tga` strings (~25) — entries inside Extras.zip/Mugshots.zip
- All `Front End\Tracks\*.tga` strings — entries inside Tracks.zip
- All internal track data: STRIP.DAT, LEFT.TRK, RIGHT.TRK, TRAFFIC.BUS, CHECKPT.NUM, LEVELINF.DAT, MODELS.DAT, TEXTURES.DAT, FORWSKY.TGA, BACKSKY.TGA
- All car archive entries: carparam.dat, himodel.dat, CARSKIN%d.TGA, CARHUB%d.TGA, CarPic%d.tga, config.eng, config.nfo, Drive.wav, Rev.wav, Reverb.wav, Horn.wav
- All traffic entries: model%d.prr, skin%d.tga
- All shared entries: static.hed, sky.prr, SKY.PRR, tpage%d.dat, wheels, TREE.TGA, BRIDGE.TGA, etc.
- Loading screen entries: load%02d.tga, legal1.tga, legal2.tga

### DO NOT need patching — DLL imports

| String | Mechanism |
|---|---|
| `M2DX.dll` (0x460AB0) | PE IAT — resolved by Windows loader before code runs |
| `Language.dll` (0x461D24) | PE IAT — resolved by Windows loader before code runs |

---

## Approach A: SetCurrentDirectory Trampoline (RECOMMENDED)

**Complexity: LOW — 1 code patch, ~80 bytes total**

### Concept

Inject a `SetCurrentDirectoryA("data")` call at the very start of WinMain. This makes
ALL relative path resolution use `data\` as base directory. Every fopen, mmioOpen, and
CreateFileA call with a relative path will automatically look in `data\`.

### Why it works

- All DLLs (M2DX.dll, Language.dll, ddraw.dll) are loaded by the PE loader BEFORE
  WinMain runs, so they're already resolved from root
- All game file paths are relative (no absolute paths anywhere)
- SetCurrentDirectory affects the entire process uniformly
- dgVoodoo2 uses GetModuleFileName to find its own DLL path for dgVoodoo.conf
  (not affected by CWD change)

### Available resources

- **Code cave in .text**: 0x45C10C – 0x45CFFF = **3828 bytes free**
  (scale cave v4 uses ~150 bytes at 0x45C330 in patched binary; plenty of room)
- **IAT imports already available**:
  - `GetProcAddress` at IAT slot ~0x45D508
  - `GetModuleHandleA` at IAT slot ~0x45D50C
- **WinMain entry**: 0x430A90, first 5 bytes = `8B 44 24 0C 50`
  (`mov eax,[esp+0xC]` + `push eax`) — perfect 5-byte steal for JMP

### Patch details

#### Step 1: Write string data to .text cave

At 0x45C110 (well before scale cave at 0x45C330):
```
0x45C110: "kernel32.dll\0"              (13 bytes)
0x45C11D: "SetCurrentDirectoryA\0"      (21 bytes)
0x45C132: "data\0"                       (5 bytes)
```
Total: 39 bytes of string data.

#### Step 2: Write trampoline code

At 0x45C140:
```asm
; === SetCurrentDirectory("data") trampoline ===
; Preserve all registers
pushad                          ; +0  (1 byte)

; GetModuleHandleA("kernel32.dll")
push 0x0045C110                 ; +1  "kernel32.dll"  (5 bytes)
call [0x0045D50C]               ; +6  GetModuleHandleA  (6 bytes)

; GetProcAddress(hKernel32, "SetCurrentDirectoryA")
push 0x0045C11D                 ; +12 "SetCurrentDirectoryA"  (5 bytes)
push eax                        ; +17 hModule  (1 byte)
call [0x0045D508]               ; +18 GetProcAddress  (6 bytes)

; SetCurrentDirectoryA("data")
push 0x0045C132                 ; +24 "data"  (5 bytes)
call eax                        ; +29 SetCurrentDirectoryA  (2 bytes)

; Restore all registers
popad                           ; +31 (1 byte)

; Execute displaced instructions from WinMain entry
mov eax, [esp+0x0C]             ; +32 (4 bytes)  [stolen]
push eax                        ; +36 (1 byte)   [stolen]

; Jump back to WinMain+5
jmp 0x00430A95                  ; +37 (5 bytes)
```
Total: 42 bytes of code.

#### Step 3: Redirect WinMain entry

At 0x430A90, replace first 5 bytes:
```
Before: 8B 44 24 0C 50           mov eax,[esp+0xC] / push eax
After:  E9 AB B6 02 00           jmp 0x45C140
```
(Offset = 0x45C140 - 0x430A95 = 0x1B6AB)

### Total binary modifications

| Location | Size | What |
|---|---|---|
| 0x430A90 | 5 bytes | JMP to trampoline (replaces `mov eax,[esp+0xC]; push eax`) |
| 0x45C110 | 39 bytes | String data: "kernel32.dll", "SetCurrentDirectoryA", "data" |
| 0x45C140 | 42 bytes | Trampoline code |
| **Total** | **86 bytes** | |

### Side effects

| Effect | Impact | Mitigation |
|---|---|---|
| TD5.log written to data\ | Negligible | None needed, or patch M2DX.dll's TD5.log string |
| Config.td5 read/written in data\ | Desired behavior | Move existing Config.td5 to data\ |
| Benchmark TGA report in data\ | Negligible | None needed |

### Risk assessment

- **dgVoodoo2**: SAFE — uses GetModuleFileName for config, not CWD
- **DLL loading**: SAFE — all DLLs loaded by PE loader before WinMain
- **M2DXFX.dll**: NOT in EXE's or M2DX.dll's import table. Either unused or
  loaded via a mechanism we haven't identified. If loaded via LoadLibrary after
  CWD change, it would need to stay in data\ or root. Test empirically.
- **Existing widescreen patches**: COMPATIBLE — scale cave at 0x45C330 is
  untouched, trampoline at 0x45C140 doesn't overlap

### Verification checklist

After patching:
1. Delete Config.td5 from root (force fresh config in data\)
2. Move all game data to data\ per target layout
3. Test: intro movie plays
4. Test: menu loads (Frontend ZIPs)
5. Test: race loads (level ZIPs, car ZIPs, static, traffic, environs)
6. Test: sound effects work (SOUND.ZIP)
7. Test: config saves persist (Config.td5 in data\)
8. Test: cup progress saves (CupData.td5 in data\)

---

## Approach B: Patch fopen_game Wrapper (Alternative)

**Complexity: MEDIUM — 1 function patch + 1 string patch, ~120 bytes**

### Concept

Patch the central `fopen_game` function (0x44867C) that ALL file I/O funnels through.
Insert a trampoline that prepends "data\" to relative paths before calling the real fopen.

### How it works

`fopen_game(filename, mode)` is called by:
- ParseZipCentralDirectory (archive opening)
- ReadArchiveEntry (archive extraction)
- OpenArchiveFileForRead (archive reading)
- GetArchiveEntrySize (archive size query)
- WritePackedConfigTd5 / LoadPackedConfigTd5 (config)
- WriteCupData / LoadContinueCupData / ValidateCupDataChecksum (cup saves)
- WriteBenchmarkResultsTgaReport (benchmark)

**13 call sites from 8 functions** — covers everything except movie playback.

### Trampoline design

```asm
; At 0x45C180 (in .text cave):
fopen_game_hook:
    push ebp
    mov ebp, esp
    sub esp, 0x110              ; 272-byte local buffer

    ; Build "data\<original_path>" in local buffer
    lea eax, [ebp-0x110]
    push eax
    push "data\%s"              ; format string in cave
    call sprintf_game           ; 0x449A66 or similar
    add esp, 8

    ; Call original fopen with modified path
    push [ebp+0x0C]             ; mode
    lea eax, [ebp-0x110]
    push eax                    ; "data\<path>"
    push 0x40                   ; share flag
    call _fopen                 ; 0x44864B
    add esp, 0x0C

    leave
    ret
```

### Additional patch for movie path

Movie uses mmioOpenA (not fopen_game), so needs separate treatment:
- Relocate `Movie\intro.tgq` (0x474840, 17 bytes) to cave as `data\Movie\intro.tgq` (22 bytes)
- Patch the 1 xref at 0x43C781 to point to new string

### Trade-offs vs Approach A

| Aspect | Approach A (SetCurrentDir) | Approach B (fopen patch) |
|---|---|---|
| Patches | 1 (86 bytes) | 2 (~120 bytes) |
| Movie coverage | Automatic (CWD) | Needs separate string patch |
| M2DX.dll log | Automatically in data\ | Stays in root (pro or con?) |
| Future file I/O | Automatically covered | Only fopen_game paths |
| Risk | Low (well-understood Win32) | Low (controlled wrapper) |

---

## Approach C: Mass String Relocation (Precise but tedious)

**Complexity: HIGH — 53 new strings + 119+ xref fixups**

### Concept

Write all 53 new prefixed strings to the .text cave, then patch every PUSH/LEA
instruction that references a filesystem path to point to the new string.

### Space budget

New strings needed:
- 16 individual paths with "data\" prefix: ~350 bytes
- 37 car paths with "data\" prefix: ~600 bytes
- New pointer table (37 × 4): 148 bytes
- **Total: ~1100 bytes** (fits in 3828-byte .text cave)

### Patch categories

**Category 1: Simple string relocations (16 strings, 36 code xrefs)**

For each string, write new version to cave and patch all PUSH instructions:
```
push 0x00467294  →  push 0x0045Cxxx    (4-byte address change per xref)
```

| String | Xrefs to patch |
|---|---|
| `level%03d.zip` → `data\level%03d.zip` | 4 |
| `static.zip` → `data\static.zip` | 7 |
| `STATIC.ZIP` → `data\STATIC.ZIP` | 1 |
| `LOADING.ZIP` → `data\LOADING.ZIP` | 2 |
| `LEGALS.ZIP` → `data\LEGALS.ZIP` | 4 |
| `environs.zip` → `data\environs.zip` | 2 |
| `SOUND\SOUND.ZIP` → `data\SOUND\SOUND.ZIP` | 3 |
| `traffic.zip` → `data\traffic.zip` | 3 |
| `Movie\intro.tgq` → `data\Movie\intro.tgq` | 1 |
| `Config.td5` → `data\Config.td5` | 2 |
| `CupData.td5` → `data\CupData.td5` | 4 |
| `Front End\FrontEnd.zip` → `data\Front End\FrontEnd.zip` | **47** |
| `Front End\Extras\Extras.zip` → `data\...` | 10 |
| `Front End\Extras\Mugshots.zip` → `data\...` | 27 |
| `Front End\Sounds\Sounds.zip` → `data\...` | 1 |
| `Front End\Tracks\Tracks.zip` → `data\...` | 1 |

**Category 2: Car path pointer table (37 strings, 37 pointer fixups)**

Write 37 new `data\cars\XXX.zip` strings to cave, then overwrite the 37 pointers
in the table at 0x466EEC–0x466F80 to point to the new strings.
No code xref patches needed — the table is in .data (RW), pointer update suffices.

### Total patches

| Type | Count |
|---|---|
| New strings in cave | 53 |
| PUSH immediate fixups | 119 |
| Pointer table entries | 37 |
| **Total individual byte-patches** | **~700+** |

### Trade-off

- **Pro**: Most precise — zero side effects, each path independently controlled
- **Con**: 700+ byte-level patches is error-prone and hard to maintain
- **Con**: Any future discovery of additional file paths requires new patches

---

## Recommendation

**Use Approach A (SetCurrentDirectory trampoline).**

It's 86 bytes, 1 patch site, zero risk of missing a path, and fully compatible
with the existing widescreen patches. The only "side effect" (TD5.log in data\)
is arguably desirable for keeping root clean.

Implementation can be done with a single Python patcher script, similar to the
existing `fix_scale_cave_v4.py`.

---

## Appendix: Section Map & Cave Space

```
.text     0x401000–0x45CFFF  (376 KB, RWX)
  Last code at 0x45C10B
  Scale cave v4 at 0x45C330 (~150 bytes, existing widescreen patch)
  FREE: 0x45C10C–0x45C32F = 548 bytes (before scale cave)
  FREE: 0x45C3E0–0x45CFFF = ~3100 bytes (after scale cave, estimate)

.rdata    0x45D000–0x462FFF  (24 KB, R--)
  FREE: 0x462E00–0x462FFF = 512 bytes

.data     0x463000–0x4D0D2B  (449 KB, RW-)
  Contains ALL game path strings (0x463xxx–0x474xxx)
  Car pointer table at 0x466EEC (37 × 4 bytes)
```

## Appendix: IAT Slots Used

| Function | IAT Slot | Source DLL |
|---|---|---|
| GetProcAddress | ~0x45D508 | KERNEL32.dll |
| GetModuleHandleA | ~0x45D50C | KERNEL32.dll |
| (SetCurrentDirectoryA) | NOT imported | (resolved via GetProcAddress) |

## Appendix: M2DX.dll Considerations

M2DX.dll contains only ONE filesystem path: `TD5.log` (0x1002414C).
It has NO game data paths (no .zip, .tga, .cfg references).
All game data loading is done by the EXE, calling M2DX rendering functions
with already-loaded data.

If TD5.log in data\ is undesirable, patch M2DX.dll separately:
- Option 1: Change `TD5.log` to `..\TD5.log` (same length + 3 bytes — needs cave)
- Option 2: Leave as-is (log goes to data\, no functional impact)
