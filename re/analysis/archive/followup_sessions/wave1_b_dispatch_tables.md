# Wave 1 Agent B — Dispatch Table Inventory (TD5_d3d.exe)

## Summary

| Category | Count | Entries (sum) | Unnamed targets |
|---|---|---|---|
| **Programmer-defined function-pointer tables** | 3 | 43 | 7 (chunks inside `ConfigureGameTypeFlags`) |
| **Compiler-emitted switch JMP tables** | 99 unique | not enumerated | n/a (targets are local labels within parent function, all in `.text`) |
| **Total dispatch tables found** | **102** | ~150+ | 7 |

Methodology: ran 2 cross-validating strategies — (A) Ghidra `search_instructions` for every `CALL`/`JMP dword ptr [reg*4 + IMM]` site in the binary; (B) manual decode of each unique table base reading raw memory and verifying all entries point inside `.text` (`0x00401000..0x0045cfff`). Skipped Strategy C (CALLIND pcode) because it returned too many vtable/COM/struct-method hits to be useful.

`.rdata` at `0x0045d000..0x00462fff` is **almost entirely IAT, GUIDs, and C++ name-decoration strings** — it contains zero static code-pointer tables outside of the IAT itself. All static function-pointer dispatch tables in this binary live in `.data` (`0x00463000..0x004d0d2b`).

### Three confirmed function-pointer tables

#### 1. Primitive opcode dispatch — `0x00473b9c` (7 entries × 4B)

Caller sites: `0x00431380`, `0x004314d8`, `0x004315ba` (all use `CALL [reg*4 + 0x473b9c]`).

| idx | addr | function |
|---|---|---|
| 0 | `0x00431750` | `EmitTranslucentTriangleStrip` |
| 1 | `0x00431750` | `EmitTranslucentTriangleStrip` (duplicate of 0) |
| 2 | `0x004316f0` | `SubmitProjectedTrianglePrimitive` |
| 3 | `0x00431690` | `SubmitProjectedQuadPrimitive` |
| 4 | `0x0043e3b0` | `InsertBillboardIntoDepthSortBuckets` |
| 5 | `0x00431730` | `EmitTranslucentTriangleStripDirect` |
| 6 | `0x004316d0` | `EmitTranslucentQuadDirect` |

Bytes after entry 6 are `3f 80 00 00` (float `1.0f`) — confirms table is exactly 7 entries. Matches the "Primitive opcode dispatch (opcodes 0-6)" line already documented in CLAUDE.md.

#### 2. Frontend screen function table — `g_frontendScreenFnTable @ 0x004655c4` (30 entries × 4B)

Loaded by `SetFrontendScreen @ 0x00414610` into `g_currentScreenFnPtr` (no direct indexed CALL — the table walks through a register, which is why my CALL/JMP scan didn't surface it; found via `decomp_function`).

All 30 entries point inside `.text`. First six: `0x004269d0, 0x00415030, 0x004275a0, 0x00427290, 0x004274a0, 0x00415490`. Last six: `0x00422480, 0x00413bc0, 0x004237f0, 0x00423a80, 0x00415370, 0x0041d630`.

(Full enumeration available via `memory_read` at `0x004655c4` length 120; all 30 targets are named functions in Ghidra.)

#### 3. ConfigureGameTypeFlags dispatch — `0x00464104..0x0046411c` (6 fp entries × 4B, +1 leading literal `0x63`)

**NEW finding** — not in CLAUDE.md's known list.

Caller: `ConfigureGameTypeFlags @ 0x00410ca0`, indexed CALL at `0x00410eb4` (`CALL [ECX*4 + 0x464104]`). The first dword at `0x00464104` is `0x00000063` (literal `99`, a sentinel/count) — the actual function pointers occupy `0x00464108..0x0046411c`.

| idx | addr (via ECX) | function |
|---|---|---|
| 1 | `0x00410f60` | (unnamed code chunk — no Ghidra function header; `MOV CL, [0x004668ba]` is first insn) |
| 2 | `0x00410fa0` | unnamed |
| 3 | `0x00410ff0` | unnamed |
| 4 | `0x00411030` | unnamed |
| 5 | `0x00411070` | unnamed |
| 6 | `0x004110a0` | unnamed |

**6 hidden code paths.** These are likely small game-type-specific initializers (cup vs single race vs benchmark etc.) called once at race startup. Worth a follow-up pass to name them — they would close the gap on game-mode-specific behavior currently inferred only from `ConfigureGameTypeFlags`. All target chunks start with `MOV CL, byte ptr [0x004668ba]` style preambles, so they share the same control-flow shape.

Trailing data at `0x00464120` is the ASCII string `"CupData.td5"` — confirms the table ends at `0x46411c`.

### 99 compiler-emitted switch JMP tables (summary, not enumerated)

Every `JMP dword ptr [reg*4 + IMM]` in the binary corresponds to a C `switch` statement compiled by MSVC. The targets are **always local labels inside the same parent function**, so they are not "hidden code paths" in the usual sense — Ghidra resolves them during analysis and (per `switchD_*` namespace in symbols) they show up as labeled branches in the decompiler output. Examples actually inspected:

- `0x00410c34` (10 entries) and `0x00410c78` (8 entries): switch tables inside `ScreenControllerBindingPage` (matches the "Controller-binding tail fallthrough (10-14 entries)" hint in CLAUDE.md).
- `0x004d7aac` (8 entries, in `UVA_DATA` section): switch table for `IDCTDecodeDCAndAC @ 0x00456f70` (FMV codec).
- `0x0044be18, 0x0044bfb0, 0x0044f6c8, 0x0044f860`: each is shared across 4–5 JMP sites inside the inflate/decompression routines (these are tight inner loops with multiple switch points sharing a common dispatch base).

Full deduplicated list of 99 unique JMP table base addresses is dumped to (transient) `/tmp/td5re_scan/jmp_tables.txt`; the raw `JMP dword ptr` query against the Ghidra MCP reproduces it.

### What I did NOT find (and confirmed absent)

- **Camera mode dispatch table.** Despite CLAUDE.md mentioning "7 chase + trackside/spline/orbit", camera-update code in `UpdateChaseCamera`, `UpdateRaceCameraTransitionState`, `LoadCameraPresetForView`, `CycleRaceCameraPreset` is **all direct branches / `if-else`** — no function-pointer indirection. `g_cameraPresetTable @ 0x00463098` is a struct array (short pitch/yaw/distance fields), not function pointers.
- **Particle/smoke callback tables.** Refs to `LAB_00429950`, `LAB_004297d0` are runtime `MOV [actor+0x4a31ac], &callback` instructions setting per-particle callbacks — they are *callback fields inside struct instances*, not a static table.
- **DXPTYPE network protocol table.** `ReadAndDispatchChunk @ 0x00451d70` (FMV chunk dispatcher) and the network handlers use big `if-else` cascades comparing 4-byte FOURCC values directly — no table.
- **Pointer table at `g_perTrackTracksideCameraProfilePtrs @ 0x00473780`.** This IS a 40-entry pointer table but all entries point into `.data` (`0x46xxxx..0x47xxxx`) — it is a **data** pointer array (per-track camera profile structs), not a function pointer table.
- **No function-pointer tables in `.rdata`.** That section is exclusively IAT + GUIDs + C++ decorated export name strings.

### Follow-up worth doing (out of this agent's scope)

1. **Name the 6 unnamed handlers reached via `0x00464104`** — they're called from `ConfigureGameTypeFlags` and probably correspond to game-type init paths. Currently they have no Ghidra `Function` record (just code labels).
2. **Verify the duplicate entry 0/1 in the primitive opcode table** — both slots point at `EmitTranslucentTriangleStrip`. Either deliberate (e.g. opcodes 0 and 1 share a handler in this game vs the more general M2DX library), or a build-time artifact. Port code should match: if the port has 7 distinct handlers for opcodes 0-6, slot 0 and 1 should both be the same function.

### Cleanup

`bash scripts/ghidra_pool.sh release TD5_pool12` invoked — slot released. Read-only mode used throughout.
