---
batch: 31
area: track_lut_inflate_actor
tier: T6
target_todos: []
ghidra_session: TD5_pool3
analyzed_addresses: 0x00444000, 0x00445000, 0x00437600, 0x00474e40, 0x00475000, 0x00476f80
agent: tier1-a-t6
date: 2026-05-22
---

# Globals enumeration — track-contact LUTs, inflate tables, actor track-script (T6)

## Summary

- Functions analyzed: UpdateActorTrackPosition, ComputeActorTrackContactNormal[Extended], ComputeActorHeadingFromTrackSegment, InitActorTrackSegmentPlacement, AdvanceActorTrackScript, InflateProcessFixedHuffmanBlock, InflateProcessDynamicHuffmanBlock, InflateBuildDecodeTable, InflateDecodeHuffmanCodes
- Unnamed DAT_* targeted: 16
- Already-named neighbors noted: g_inflateBitCount
- Proposals — high: 9
- Proposals — medium: 4
- Proposals — comment-only: 3

## Methodology

Two clusters: (1) actor track-contact LUTs around 0x00474e28..0x00474e40 (accessed via `[reg*2 + base]` = u16 lookup tables for span-type contact normals); (2) inflate static decode tables 0x00475000..0x00476f84 (read by Inflate*).

For the track-script entries 0x00473cc8..0x00473d20: each 8-byte entry has the form `(opcode_u32, param_u32)` and is stored into `actor->script_ptr` (+0xec) by AdvanceActorTrackScript. Reading raw bytes: 0x00473cec=`(0x08, 0x02, 0x40, 0x06)` looks like (cmd, p1, p2, p3) records.

## Proposals (globals)

| address | size | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00474e28 | u8[24] | `g_trackContactNormalXLut` | high | `[reg*2 + 0x474e40]` reads u8 — actually a u8[24] table at 0x474e28 holding 12 sin-like values; 0x474e40 is the SECOND half (cos values). 4 callers UpdateActorTrackPosition / ComputeActorTrackContactNormal[Extended] / ComputeActorHeadingFromTrackSegment / InitActorTrackSegmentPlacement (8 refs) | `(none)` |
| 0x00474e40 | u8[24] | `g_trackContactNormalYLut` | high | Same 4 callers (11 refs); cos-paired with 0x474e28 | `(none)` |
| 0x00474e29 | u8 | (alt label of g_trackContactNormalXLut+1) | low | Ghidra-auto secondary label; same table | `(none)` |
| 0x00474e41 | u8 | (alt label of g_trackContactNormalYLut+1) | low | Ghidra-auto secondary label | `(none)` |
| 0x00474ea0 | u32[24] | `g_inflateLengthBaseTable` | high | InflateDecodeHuffmanCodes/InflateBuildDecodeTable/InflateProcessDynamicHuffmanBlock readers (7 refs); values `0,1,3,7,...` are standard DEFLATE length-base | `(none)` |
| 0x00475560 | u8[24] | `g_inflateLengthExtraBitsTable` | medium | InflateBuildDecodeTable (5 refs) — match-length extra bits | `(none)` |
| 0x00475564 | u8[24] | `g_inflateDistanceExtraBitsTable` | medium | InflateBuildDecodeTable (5 refs) | `(none)` |
| 0x00475570 | u8[16] | `g_inflateCodeLengthReorder` | medium | InflateBuildDecodeTable (4 refs) — code-length permutation table | `(none)` |
| 0x0047557c | u8[??] | `g_inflateFixedHuffmanLitTable` | high | InflateProcessFixedHuffmanBlock + InflateProcessDynamicHuffmanBlock readers (14 refs) — fixed lit/length huffman table | `(none)` |
| 0x00475a7c | u8[??] | `g_inflateFixedHuffmanDistTable` | high | Same callers (6 refs) — fixed distance huffman table | `(none)` |
| 0x00475b04 | u8[??] | `g_inflateDistanceBaseTable` | medium | InflateBuildDecodeTable (5 refs) — distance-base values | `(none)` |
| 0x00475f84 | u8[~4096] | `g_inflateDecodeWorkBuffer` | high | InflateDecodeHuffmanCodes + InflateProcessFixedHuffmanBlock + InflateProcessDynamicHuffmanBlock (15 refs) — sliding-window/decode buffer | `(none)` |
| 0x00476f84 | u8[~256] | `g_inflateBitReverseTable` | medium | Same 3 inflate callers (6 refs) — bit-reverse LUT | `(none)` |
| 0x0047b1c4 | u32 | `g_inflateScratchBufferPtr` | medium | InflateProcessFixedHuffmanBlock + InflateBuildDecodeTable + InflateProcessDynamicHuffmanBlock (5 refs) | `(none)` |
| 0x00473cec | u32[2] | `g_trackScriptDefaultEntry0` | high | AdvanceActorTrackScript stores into actor->script_ptr (+0xec). Bytes (0x08, 0x02) — default script opcode pair (8 refs) | `(none)` |
| 0x00473cf0 | u32[2] | `g_trackScriptDefaultEntry1` | high | AdvanceActorTrackScript stores; bytes (0x02, 0x40, 0x06, 0x00) (10 refs) — second default cmd | `(none)` |
| 0x00473cf4 | u32 | `g_trackScriptDefaultEntry1_p2` | low | Auto-secondary at +4 of above (3 refs) | `(none)` |
| 0x00473d00 | u32[2] | `g_trackScriptDefaultEntry2` | medium | AdvanceActorTrackScript stores (0x08, 0x02, 0xffffffe0, 0x06) — 3rd default cmd (6 refs) | `(none)` |
| 0x00473d18 | u32[2] | `g_trackScriptDefaultEntry3` | medium | AdvanceActorTrackScript stores (0x08, 0x02, 0x40, 0x05) — 4th (7 refs) | `(none)` |
| 0x00473cc8 | u32[2] | `g_trackScriptInitEntry` | medium | (3 refs) AdvanceActorTrackScript only — entry at index 0 | `(none)` |

## Key discoveries

- The track-contact contact-normal LUT at 0x00474e28/0x00474e40 is a **paired sine/cosine micro-LUT** for span-type to contact-normal mapping. It has only 12 entries (24 bytes), but is indexed via `[reg*2 + base]` so each entry is 2 bytes. Worth typing as `int16_t g_trackContactNormalLut[12][2]` in Wave 2 to expose the (x,y) pairs.
- Track-script default entries 0x00473cec..0x00473d20 are a **fallback opcode chain** baked into AdvanceActorTrackScript — when an actor has no script, it cycles through these defaults. 4-5 fallback opcodes, each stored as 2 dwords (op, param). Important for future AI rubber-band investigations: any AI behavior that "falls back" to defaults goes through this table.
- The inflate-table cluster (0x00475560..0x00476f84) is a standard zlib/RFC1951 static table set. Wave 2 may want to consolidate them via Type definitions matching zlib's `cplens[]` / `cplext[]` etc. for cross-binary triage.

## Out-of-scope finds

| address | brief note | probable area |
|---|---|---|
| 0x00474ce8 | u32 — LoadTrafficVehicleSkinTexture/LoadRaceVehicleAssets/GetTrafficVehicleVariantType | T6 asset loader |
| 0x004749c8/d0/e0/e4 | u32 — track parser scratch | T6 track parser |
| 0x00474858/54/50 | u32 — texture parser scratch | T6 asset loader |
| 0x004749c8 | u32 | track/inflate edge |

## TODO impact

- No direct closures. Names support future audits of track-contact mathematics and inflate-state divergence (if any cluster-fix cascade implicates spans).
