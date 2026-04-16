# Low-Confidence Function Pass — 2026-04-08

This note is a follow-up to `function-mapping-confidence-audit-2026-04-08.md`.

It has two goals:

1. Record the project-safe Ghidra remediation that restored clean headless startup.
2. Re-bucket the `21` "low-confidence" user-defined functions into:
   - promoted/high-confidence
   - applied exact-address renames backed by stronger contradictory evidence
   - provisional names that still need direct decompilation review

---

## 1. Ghidra Remediation Status

The project itself was not modified during remediation.

### Root cause

Headless Ghidra startup was being poisoned by an invalid user extension manifest at:

`%APPDATA%\ghidra\ghidra_12.0.3_PUBLIC\Extensions\GhydraMCP\Module.manifest`

That extension is unrelated to this repo's `ghidra-headless-mcp` server and caused manifest parse failures during startup.

### Safe fix applied

The broken extension directory was moved completely out of the live Ghidra extensions scan path to:

`temp_ghidra/disabled_extensions/GhydraMCP.disabled-2026-04-08`

No `.rep` project data or Ghidra database files were edited for this fix.

### Verification

- `analyzeHeadless.bat` opens the live `TD5` project cleanly after the extension move.
- The repo MCP config now passes `--ghidra-install-dir` explicitly in `.mcp.json`.
- A manual stdio handshake against `ghidra-headless-mcp.py` succeeded, including `tools/list` and `health.ping`.
- The built-in MCP bridge in this chat session still returns `Transport closed`, which appears to be a stale session transport issue rather than a project or server integrity issue.

Conclusion: Ghidra itself is fixed locally without touching the project database.

---

## 2. Re-bucketing The Prior "Low-Confidence" Set

Original weak set from the audit: `21` functions.

After the applied rename pass:

- `7` have been moved out of the weak bucket
- `14` remain provisional but plausible within the confirmed TGQ/FMVs subsystem
- the former lone auto-name at `0x00414F40` is now resolved as `RenderPositionerGlyphStrip`

---

## 3. Promote To High-Confidence

These are no longer meaningfully "low-confidence". They are directly backed by maintained source-port comments and implementations.

| Address | Live Name | Evidence | Result |
|---|---|---|---|
| `0x00429690` | `ProjectRaceParticlesToView` | Explicitly referenced and implemented in `td5_vfx.c` / `td5_vfx.h` | Promote to high-confidence |
| `0x00439B60` | `SetRaceHudIndicatorState` | Explicitly referenced and implemented in `td5_hud.c` / `td5_hud.h` | Promote to high-confidence |

### Source evidence

- `td5mod/src/td5re/td5_vfx.c` documents `ProjectRaceParticlesToView (0x429690)` and implements the exact per-particle world-to-view projection path.
- `td5mod/src/td5re/td5_hud.c` documents `SetRaceHudIndicatorState (0x439B60)` and implements the corresponding per-view indicator setter.

These two should be removed from any future "weak mapping" backlog.

---

## 4. Applied Exact-Address Renames

These five were not merely under-documented. The previous live Ghidra names were weaker than existing exact-address mappings already captured in `ea-tgq-multimedia-engine.md`.

| Address | Previous Live Name | Applied Name | Basis |
|---|---|---|---|
| `0x004527C0` | `InitVideoDecoderState` | `ReadTaggedParam` | Exact-address match in streaming-core summary; function belongs to SCHl tagged audio-header parsing, not video init |
| `0x00453680` | `PackBitsToSigned` | `PackPixelFromRGB` | Exact-address match in video-init summary; helper used while building color conversion LUTs |
| `0x004549D0` | `InterpolateYCbCrPixel_A` | `WriteBlock16_3to6` | Exact-address match in type-3 compressed block path for 16-bit output |
| `0x00454CE0` | `InterpolateYCbCrPixel_B` | `WriteBlock32_3to6` | Exact-address match in type-3 compressed block path for 32-bit output |
| `0x0045A588` | `DoublePixelRow` | `CopyPixelBuffer32` | Exact-address match in palette/color helper table |

### Status

These five names were applied to the live `TD5` project via headless Ghidra on 2026-04-08. A fresh inventory export confirms the saved project state now uses the dossier-backed names.

---

## 5. Provisional But Plausible

These names are still inside the confirmed TGQ movie/FMV subsystem, and their current semantics fit their address neighborhoods. They are not yet supported by the same exact-address level of durable documentation as the groups above.

| Address | Current Live Name | Status |
|---|---|---|
| `0x004539A0` | `PackYCbCrTableEntry` | Provisional, plausible helper under LUT-builder cluster |
| `0x00456F40` | `LookupQuantTable` | Provisional, plausible helper immediately before `IDCTDecodeDCAndAC` |
| `0x00458BC9` | `DecodeJPEGMCU` | Provisional, plausible large MCU decoder in IDCT/JPEG path |
| `0x004599D0` | `YCbCrToRGB_Row_16bit_444` | Provisional, plausible row converter |
| `0x00459EA0` | `YCbCrToRGB_Row_16bit_420` | Provisional, plausible row converter |
| `0x0045A3A0` | `YCbCrToRGB_Row_8bit` | Provisional, plausible row converter |
| `0x0045AA75` | `IDCT_1D_8pt_Float_C` | Provisional, plausible IDCT helper near confirmed butterfly stages |
| `0x0045B1C0` | `BitstreamRefillBits` | Provisional, plausible small entropy/bitstream helper |
| `0x0045B750` | `BuildBitplaneDeinterleaveTable` | Provisional, plausible palette/bitmap helper |
| `0x0045B7D0` | `DecodeVQ_Block` | Provisional, plausible palette-frame block decoder |
| `0x0045BD00` | `SetVGAPalette` | Provisional, plausible palette helper |
| `0x0045BD3C` | `DecompressLZData` | Provisional, plausible bitmap/palette decompressor |
| `0x0045BE5F` | `UnpackBitplaneToPixels` | Provisional, plausible bitmap unpacker |
| `0x0045BF08` | `DecodeBitmapRLERun` | Provisional, plausible RLE helper |

### Why these stay provisional

The subsystem placement is strong:

- `0x4539E0` is confirmed as `InitializeVideoDecoder`
- `0x4542C0` is confirmed as `DecodeVideoFrame`
- `0x456F70` is confirmed as `IDCTDecodeDCAndAC`
- `0x45791F` is confirmed as `IDCTDecode16x16Block`
- `0x45A5CC` is confirmed as `YCbCrToRGB_Row_16bit_422`
- `0x45B2B0` is confirmed as `DecodeMotionCompensatedFrame`
- `0x45B420` is confirmed as `DecodeType2DeltaPalette`
- `0x45B8D0` is confirmed as `DecodeType1PaletteFrame`

So the unresolved names above are not "unknown subsystem" functions. They are specifically unresolved helpers inside an already identified FMV decode pipeline.

What is missing is direct function-level proof that each current name is the best final semantic name.

---

## 6. Net Result

The old `21`-function low-confidence bucket should now be interpreted as:

- `2` promoted to high-confidence from maintained source-port evidence
- `5` corrected to stronger exact-address names and removed from the weak bucket
- `14` provisional helpers in a confirmed FMV/TGQ subsystem

This is a materially better state than "21 weak names", because the remaining uncertainty is now localized to `14` FMV codec helpers rather than spread across unrelated gameplay systems.

---

## 7. Next Pass

Priority order:

1. Review the `14` provisional FMV helpers by direct decompilation in address order.
2. Refresh the stale `exe_func_*.json` snapshots from the now-correct live `TD5` project.
3. Re-audit any remaining exact-address mismatches between the live project and `ea-tgq-multimedia-engine.md`, because that dossier still contains a few internal inconsistencies outside the original weak bucket.

Once those are done, the project should be close to having no meaningful weakly mapped gameplay functions left; the remaining uncertainty will be almost entirely codec-helper nomenclature.
