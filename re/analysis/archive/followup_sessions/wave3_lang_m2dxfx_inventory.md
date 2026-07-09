# Wave3 follow-up: Language.dll + M2DXFX.dll inventory

Date: 2026-05-22 | Method: Ghidra read-only (temp project, no master mutation) | Time: 25 min

## Headline

- **Language.dll** — pure string-table DLL (172 exports, only `entry`/DllMain is custom code; remaining 52 funcs are MSVC CRT). Used by TD5_d3d.exe (167 `SNK_*` symbols).
- **M2DXFX.dll** — **NOT audio FX**. It is the **3dfx Voodoo2 / Glide variant of M2DX** middleware (163 exports, 406 functions). Linked by `original/VOODOO2.exe`, **not by TD5_d3d.exe**. The "FX" likely refers to "3dfx" or the non-D3D fixed-pipeline build, since this DLL exports the same DX namespaces as M2DX *minus* the D3D-3 (`DXD3D`, `DXD3DTexture`) namespaces.

Net: **Both DLLs are orthogonal to the source port's RE workload.** Language.dll content is already extracted verbatim into td5_frontend.c string literals. M2DXFX.dll only matters if anyone ever wants to port the VOODOO2.exe variant — out of scope for td5re.exe.

---

## A: Language.dll

- **Path:** `original/Language.dll`
- **Size:** 49,152 bytes
- **Image base:** `0x10000000`
- **Sections:** `.text` 12 KB | `.rdata` 8 KB | **`.data` 21 KB** (the actual string pool) | `.reloc` 4 KB
- **Functions:** 53 total (all CRT scaffolding — `__amsg_exit`, `__exit`, `_malloc`, `__nh_malloc`, `_strlen`, `_strncpy`, `__global_unwind2`, `__local_unwind2`, etc.). The only custom function is `entry` (DllMain @ 0x100010b8); it just sets up DllMain state — no logic.
- **Exports:** **172** — all C++-mangled `SNK_*Txt`, `SNK_*MT`, `SNK_*Ex` symbols pointing to `.data` labels (no functions exported).

### Top-level export categories
| Category | Examples | Count (approx) |
|---|---|---|
| Button labels (`*ButTxt`) | `SNK_QuickRaceButTxt`, `SNK_BackButTxt`, `SNK_OkButTxt` | ~50 |
| Menu trees (`*_MT`, `*_Ex`) | `SNK_MainMenu_MT`, `SNK_CarSelect_Ex` | ~14 |
| Result screens (`*ResultsTxt`) | `SNK_CCResultsTxt`, `SNK_DRResultsTxt`, `SNK_ResultsTxt` | ~3 |
| Track / car names | `SNK_TrackNames`, `SNK_CarLongNames` | ~2 |
| Unit suffixes | `SNK_ConfMph`, `SNK_ConfFt`, `SNK_ConfSec`, `SNK_ConfRpm` | ~6 |
| Net error strings | `SNK_NetErrString1..4`, `SNK_NetPlayStatMsg` | ~5 |
| Status messages | `SNK_BlockSavedOK`, `SNK_FailedToSave`, `SNK_CongratsTxt`, `SNK_YouHaveWonTxt`, `SNK_Quit`, `SNK_RaceAgain` | ~30 |
| Misc HUD/credits | `SNK_CreditsText`, `SNK_NowPlayingTxt`, `SNK_SpeedReadTxt`, `SNK_LapTxt` | ~10 |
| Lookup tables | `SNK_Engine_Types`, `SNK_Layout_Types`, `SNK_Info_Values`, `SNK_DifficultyTxt`, `SNK_OnOffTxt` | ~10 |

### Localized variants
- `original/Language.dll` is the English default (size 49,152 bytes).
- `original/Language/English/Language.dll`, `original/Language/German/Language.dll` are alternates (same export table, different `.data` bytes).
- TD5_d3d.exe's installer probably copies the chosen locale over the root Language.dll.

---

## B: M2DXFX.dll

- **Path:** `original/M2DXFX.dll`
- **Size:** 151,552 bytes
- **Image base:** `0x10000000`
- **Sections:** `.text` 90 KB (real code) | `.rdata` 20 KB | `.data` 227 KB | `.reloc` 12 KB
- **Functions:** **406** total
- **Exports:** **163** — full C++-mangled `?Name@Namespace@@signature` set

### Namespace breakdown (M2DXFX exports)
| Namespace | Description | Approx count |
|---|---|---|
| `DXDraw` | 2D blitter, surface, flip, gamma, frame-rate, TGA print | ~25 |
| `DXInput` | Keyboard / joystick / mouse / force-feedback / configure | ~35 |
| `DXSound` | Wave / streaming / CD audio / mute / volume / 3D | ~32 |
| `DXPlay` | DirectPlay session, host/client send/receive, enumeration | ~13 |
| `DXWin` | Window pump, Initialize/Uninitialize, Pause, hWnd | ~6 |
| `DX` (global) | Allocate/DeAllocate, file I/O, image decoders (BMP/TGA/RGB), state strings, system/screen info | ~25 |
| Free-function helpers | `DXErrorToString`, `DXHex`, `DXDecimal`, `DXFDecimal`, `LogReport`, `Msg`, `ReleaseMsg`, `Report`, `ErrorN` | ~12 |
| State globals | `?app@DX@@`, `?dd@DXDraw@@`, `?info@DX@@`, `?dpu@DXPlay@@`, `?js@DXInput@@` etc. | ~15 |

### Key absence vs M2DX.dll
- **M2DXFX has NO `DXD3D` namespace** (no `BeginScene`/`EndScene`/`SetRenderState`/`TextureClamp`/`ChangeDriver`/`FullScreen`/`GetMaxTextures`/`CanFog`/`InitializeMemoryManagement`/`Create@DXD3D`/`Destroy@DXD3D`/`Environment@DXD3D`).
- **M2DXFX has NO `DXD3DTexture` namespace** (no `Load`/`LoadRGB`/`LoadRGBS24/32`/`Manage`/`ClearAll`/`LoseAll`/`RestoreAll`/`GetMask`/`Set@`).

Conclusion: M2DXFX = M2DX with the D3D-3 hardware rendering replaced by something else (Glide/software). Likely 3dfx Voodoo path.

---

## C: TD5_d3d.exe import surface

From TD5_d3d.exe imports (jq-tallied):

| Library | Symbol count |
|---|---|
| **LANGUAGE.DLL** | **167** (all `SNK_*` string labels) |
| KERNEL32.DLL | 71 |
| **M2DX.DLL** | 79 total: `DXInput` 26, `DXSound` 19, `DXPlay` 13, `DX` 13, `DXD3D` 10, `(root)` 10, `DXDraw` 8, `DXD3DTexture` 7, `DXWin` 5 |
| USER32.DLL | 9 |
| WINMM.DLL | 6 |
| DSOUND.DLL | 1 |

**M2DXFX.dll is NOT imported by TD5_d3d.exe.** It is imported by `original/VOODOO2.exe` (the 3dfx variant of the game), confirmed by raw-string scan.

---

## D: Port-side equivalents

| Original DLL surface | Source-port handling | Where |
|---|---|---|
| `LANGUAGE.DLL` `SNK_*` strings | **Strings extracted verbatim as C literals** during RE; no DLL load at runtime. | `td5_frontend.c` (73 references), `td5_game.c` (1) — search `Language.dll` for citation comments |
| `M2DX::DXDraw` (2D surface blit/flip/gamma/print/TGA) | Replaced by `ddraw_wrapper/` (D3D11 emulation of legacy DDraw). 18 modules import the D3D11 surface. | `td5mod/ddraw_wrapper/`, `main.c`, `td5_render.c` |
| `M2DX::DXD3D`, `DXD3DTexture` (D3D-3 immediate-mode) | Replaced wholesale by D3D11 forward renderer in port. Software T&L → `td5_render.c`; texture mgmt → `td5_asset.c`. | `td5_render.c`, `td5_asset.c` |
| `M2DX::DXSound` (DSound wrapper, CD audio, streaming) | Reimplemented in `td5_sound.c` over native `dsound.dll` + `winmm.dll`. CD audio likely stubbed (no source CD). | `td5_sound.c` |
| `M2DX::DXInput` (keyboard/joystick/FF) | Reimplemented in `td5_input.c` over native `dinput8.dll`. | `td5_input.c` |
| `M2DX::DXPlay` (DirectPlay sessions, host/client) | Reimplemented in `td5_net.c` (DXPTYPE protocol re-flattened; wire-incompatible with original per memory entry "arch_dxptype_protocol_divergence_2026-05-20"). | `td5_net.c` |
| `M2DX::DXWin` (window pump, init/uninit) | Replaced by main.c WinMain + WndProc using user32 directly. | `main.c` |
| `M2DX::DX` (Allocate/file/image decoders) | Replaced — heap via msvcrt, file I/O native, TGA decode in `td5_asset.c`, BMP/RGB decoders unused. | `td5_asset.c`, `main.c` |
| **M2DXFX.dll** | **Zero port-side equivalent — not needed.** Port targets D3D11, not 3dfx Voodoo. | N/A |

---

## E: Highest-value follow-up targets

Since both DLLs are essentially closed (Language is byte data already extracted; M2DXFX is unused), the high-value audits are in **M2DX.dll**, not these two. But these are the few items in the inventoried set that could matter:

1. **Language.dll `SNK_NetErrString1` (`@@3PAY0CI@DA` array of 40-char strings)** — used by net error reporter. Port-side `td5_net.c` error pretty-printer should match. ~30 min audit.
2. **Language.dll `SNK_CreditsText` (`@@3PAY0BI@DA` array of 24-char strings)** — credits screen text. Verify port string-table size matches (24 entries). ~15 min.
3. **Language.dll `SNK_ControlText` (`@@3PAY0BA@DA`)** — keyboard-binding row labels. Slot count + width matters for input UI. ~15 min.
4. **Language.dll `SNK_ResultsTxt` / `SNK_CCResultsTxt` / `SNK_DRResultsTxt`** — result-screen layouts. Wave3 has open todo about View-Race-Data flashing back; format strings here could be involved. ~30 min.
5. **M2DXFX diff vs M2DX whole-binary structural compare** — useful only if anyone ever wants to RE the Voodoo2 build for completeness. **DEFERRED** unless TD5RE expands scope to VOODOO2.exe.
6. **M2DXFX vs M2DX DXSound implementation diff** — if `M2DX::DXSound` has 3dfx-aware audio routing (unlikely) it could surface in M2DXFX's variant. Low-probability. **DEFERRED.**
7. **Language.dll `SNK_LangDLL`** (export at 0x10006030) — likely a self-ID string; cheap sanity check (~5 min) that the port's locale-pick logic, if any, has the right tag.

**Recommended action: close out both DLLs as inventoried. No deep audit warranted unless a specific port bug points back to a Language.dll string or VOODOO2 path.**

---

## Constraints honored
- DLLs opened with `read_only=true` from temp Ghidra project (under `%TEMP%\ghidra_headless_mcp-*`); briefly switched to RW on the temp projects only to run auto-analysis. **Master `TD5.gpr` and all `ghidra_pool/TD5_pool*` projects untouched.**
- No port source modified.
- Pool slot 6 used for TD5_d3d.exe import lookup (read-only); released at exit.
