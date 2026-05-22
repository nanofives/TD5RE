# Wave3 Option-1 Agent A — M2DX.dll Plate-Comment Apply Plan

**Date:** 2026-05-22
**Scope:** Build a CSV plan of plate comments to write back into M2DX.dll in Ghidra,
documenting how each game-facing M2DX export is handled (re-implemented, stubbed,
or ARCH-DIVERGENCE) on the port side.

**Inputs:** W1-D inventory (`wave1_d_m2dx_inventory.md`, 191 exports across 8
namespaces), subsystem deep-dives, and direct grep of `td5mod/src/td5re/`.

**Output:** `wave3_m2dx_comment_plan.csv` — **163 rows**, every comment ≤300 chars,
each with a one-line purpose + port-side handling pointer + `Wave3-O1A 2026-05-22`
tag.

**Constraint compliance:**
- Ghidra opened READ-ONLY through existing pool14 session (no transactions opened
  by this agent).
- No port source modified; no Ghidra program modified.
- All evidence cites either W1-D inventory, a subsystem markdown, project memory,
  or a direct file-line citation in `td5mod/src/td5re/`.

---

## Per-namespace coverage

| Namespace      | Exports in W1-D | Rows in plan | Coverage |
|----------------|-----------------|--------------|----------|
| DX (top)       | 29              | 21           | 72%      |
| DXD3D          | 16              | 14           | 88%      |
| DXD3DTexture   | 14              | 12           | 86%      |
| DXDraw         | 23              | 18           | 78%      |
| DXInput        | 37              | 33           | 89%      |
| DXPlay         | 18              | 17           | 94%      |
| DXSound        | 32              | 32           | 100%     |
| DXWin          | 7               | 6            | 86%      |
| Global stubs   | 15              | 10           | 67%      |
| **Total**      | **191**         | **163**      | **85%**  |

Skipped (~28): pure CRT/internal helpers (`DXDecimal`/`DXHex` already covered
generically; multiple data-label noise like `DXInput::AnsiBuffer`, `DXInput::AnsiP`,
`DXDraw::FPSCaption`, `DXD3DTexture::Texture` — these are pure data symbols
without distinct semantics from their owning class), and ordinal-only stubs
(`CheckOut`, `CheckOutS`) that are import-thunks rather than functional code.

## Confidence distribution

| Confidence | Count | Meaning                                             |
|------------|-------|-----------------------------------------------------|
| high       | 98    | Direct port-source citation OR explicit W1-D claim  |
| medium     | 43    | Subsystem-doc derivation w/ reasonable inference   |
| low        | 22    | Namespace-tag + best-effort summary; no direct evidence |

## Comment-length stats

min=88, max=300, mean=193 chars. Inside the 300-char spec for every row.

---

## 10 highest-value rows to land in Ghidra

These are the rows where the plate comment captures non-trivial port-side
behavior the next reverse-engineer would have to re-derive from scratch:

### 1. `0x1000a660` — `DXInput::Write`
**Why:** Captures the entire replay-codec wire format (entry table at
`0x10033ba8`, max `0x4E1C` entries × `0x7FEE` frames, `param_2==1` strips
`0x44000000`). Closes `[[todo-view-replay-restarts-race]]`. Cross-references
`td5_input.c:1080–1082` and `subsystems/replay-recording-system.md`.

### 2. `0x1000b2a0` — `DXPlay::SendMessageA`
**Why:** Documents the lockstep barrier mechanism (cases 1/3/2/8/4/10) and the
ARCH-DIVERGENCE flag that the port's flattened 13-handler dispatch is
**wire-incompatible** with TD5_d3d.exe peers. Anyone reading orig DXPlay needs
to know the port cannot interop. Cites `td5_net.c:2257` and
`reference_arch_dxptype_protocol_divergence_2026-05-20.md`.

### 3. `0x1000b680` — `DXPlay::HandlePadHost`
**Why:** The per-frame host sync path — 20s timeout on FrameAckEvent, merge into
`controlBits[0..5]`, broadcast type-0 DXPFRAME (0x80 bytes), monotonic
`syncSequence`. Documents the lockstep model. Not ported (single-machine only).

### 4. `0x1000b8b0` — `DXPlay::HandlePadClient`
**Why:** Mirror image of #3 — sends local controlBits, blocks 20s for host
broadcast, unpacks merged frame. Pair with #3 to fully reconstruct lockstep wire
format. Not ported.

### 5. `0x1000ce30` — `DXSound::Create`
**Why:** Calls `InitializeCDAudioBackendFromInstallSource` which decides whether
music-track playback works at all. Understanding the `g_pCDAudioBackend` vtable
is the prerequisite for the deferred Music Tracks implementation. W1-D
highest-value follow-up #4.

### 6. `0x1000d380` + `0x1000d470` — `DXSound::Play` (both overloads)
**Why:** Documents the slot-duplicate fan-out via `g_soundDuplicateCountTable`.
Port likely re-triggers same buffer instead of cloning, causing engine/skid SFX
choppiness. W1-D follow-up #5. Plate comment cites `td5_frontend.c` SFX call
sites (lines 8142, 9153, 9268).

### 7. `0x1000d850` — `DXSound::SetVolume`
**Why:** Documents the `g_soundVolumeAttenuationTable[raw>>9]` indexing
(16 tiers from 0–65535 input range) and the **confirmed** mapping
slider2 "SOUND" (`DAT_004B1364`) → `DXSound::SetVolume(frac*0xFFFF)` per
`td5_game.c:2600` + `td5_hud.c:986-987`.

### 8. `0x1000d8a0` + `0x1000dc30` — `DXSound::CDPlay` + `DXSound::CDSetVolume`
**Why:** Music-tracks API surface. `td5_frontend.c:8256` (orig `0x41864E`
confirmed) calls `CDPlay(g_selectedCdTrackIndex+2, 1)`; `td5_game.c:2599`
maps slider1 "MUSIC" → `CDSetVolume(frac*0xFFFF)`. Port stubs all CD audio
(silent). Marks the music-tracks gap clearly.

### 9. `0x10001770` — `DXD3D::SetRenderState`
**Why:** Multiple port files reference orig `0x10001770` for the specific
`ALPHAREF=0 / D3DCMP_NOTEQUAL` semantics used to gate HUD text rendering
(`td5_hud.c:335`, `td5_render.c:5724`, `td5_platform_win32.c:2317`). The plate
comment captures **why** the port's blend-state mapping has to mirror this exact
value — it's not a generic render-state setter, it's specifically the alpha-test
shortcut used by HUD.

### 10. `0x1005ecd8` — `DXPlay::dpu` (data label)
**Why:** The 0xC10-byte DPU shared block — exported by M2DX, imported by EXE as
`dpu_exref` (IAT `0x0045D4E4`). ARCH-DIVERGENCE: port reproduces the struct but
with subtle layout differences (driver-name stride 0x3C vs 64; lockstep counters
not in data segment per `reference_arch_dxptype_protocol_divergence`). This is
the single most-referenced M2DX data symbol from the EXE.

## Honourable mentions (rows 11–20)

11. `0x1000aed0` — `DXPlay::NewSession` (session descriptor layout, seed via `timeGetTime()`)
12. `0x1000a640` / `0x1000a740` — `DXInput::WriteOpen` / `WriteClose` (replay lifecycle)
13. `0x1000a760` / `0x1000a780` — `DXInput::ReadOpen` / `Read` (replay playback path)
14. `0x10001d40` — `DXD3D::GetMaxTextures` (port maps to fixed D3D11 limits per `td5_render.c:6165`)
15. `0x1000d690` — `DXSound::Remove` (duplicate-chain teardown; ARCH-DIVERGENCE noted at `td5_sound.c:337`)
16. `0x1000bd00` — `DXPlay::UnSync` (race-end barrier release; `td5_game.c:713,719`)
17. `0x1000a1e0` — `DXInput::GetJS` (joystick poll + custom packed-mode tables)
18. `0x1000ded0` — `DXSound::Refresh` (per-frame DSBSTATUS poll for play-handles)
19. `0x10009b10` / `0x10009bb0` — `DXInput::PlayEffect` (FFB; ARCH-DIVERGENCE at `td5_input.c:1586`)
20. `0x1000eed0` — `DX::DecodeTgaToScratchSurface` (TGA codec reproduced in `td5_asset.c`)

---

## Phase B apply guidance

1. **Apply order:** namespaces in descending DXSound > DXPlay > DXInput > DXD3D
   > DXDraw > DXWin > DX > Global. This pairs with the W1-D follow-up priority
   (CD audio + DirectPlay are the largest port gaps).

2. **Existing comments:** Spot-checked `0x1000a660` — Ghidra already has a
   shorter plate comment ("Appends one recorded input frame…"). The Phase B
   apply script should:
   - If existing plate exists: append `\n\n` + this plan's text (preserving
     existing analyst notes).
   - If empty: write directly.

3. **Confidence gating:** apply `high`-confidence rows first (98 entries). Pause
   for spot-check by user before applying `medium`/`low` rows.

4. **Round-trip verification:** for each applied row, re-read via
   `mcp__ghidra__comment_get_all` and confirm the `Wave3-O1A 2026-05-22` tag
   appears.

---

## Cleanup

Pool slot14 session `7112fec2e8e3431787dd82ca5f5507bc` was opened **read-only**
by an earlier agent and is preserved (not closed by this agent — other sessions
may rely on it). This agent did not acquire a new pool slot or open new
transactions.

No port source modified. No Ghidra program modified.

---

## Files produced

- `re/analysis/followup_sessions/wave3_m2dx_comment_plan.csv` — 163 rows
- `re/analysis/followup_sessions/wave3_m2dx_comment_plan_build.py` — generator
  (re-runnable, deterministic)
- `re/analysis/followup_sessions/wave3_m2dx_plan_summary.md` — this file
