---
batch: 58
area: wave2b_audit_comment_sync
tier: T6
target_todos: []
ghidra_session: (per-batch, ghidra-apply opens master)
analyzed_addresses: 0x0045a98b,0x0045aa75,0x0045ab80,0x0045ac60,0x0045acd0,0x0045ad90,0x0045af50,0x0045b1c0,0x0045b220,0x0045b250,0x0045b2b0,0x0045b420,0x0045b750,0x0045b7d0,0x0045b8d0,0x0045ba30,0x0045bae0,0x0045bd00,0x0045bd3c,0x0045be5f,0x0045bf08,0x0045d6d8,0x004631a0,0x00465f74,0x004667a8,0x00466808,0x004743f4,0x0048d988
agent: claude-opus-4-7
date: 2026-05-22
---

# Wave 2B audit-comment sync — batch 58 (28 addresses)

## Summary

- Addresses in this batch: 28
- All proposals are confidence=low (comment-only, no rename)
- Each address receives a consolidated PLATE comment derived from port-source audit headers

## Methodology

Pure derived from `re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv` —
port [CONFIRMED @ ...] / [ARCH-DIVERGENCE: ...] audit-header references extracted
and deduped per address. Comment text combines unique header summaries.

## Proposals (functions)

| address | current_name | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x0045a98b | _unchanged_ | _unchanged_ | low | 0x0045A98B  IDCT8RowButterfly               [ARCH-DIVERGENCE: FMV] 0x0045AA75  IDCT_1D_8pt_Float_C             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1064 |
| 0x0045aa75 | _unchanged_ | _unchanged_ | low | 0x0045AA75  IDCT_1D_8pt_Float_C             [ARCH-DIVERGENCE: FMV] 0x0045AB80  DequantizeIDCT_Block            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1065 |
| 0x0045ab80 | _unchanged_ | _unchanged_ | low | 0x0045AB80  DequantizeIDCT_Block            [ARCH-DIVERGENCE: FMV] 0x0045AC60  DequantizeIDCT_Block_Half       [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1066 |
| 0x0045ac60 | _unchanged_ | _unchanged_ | low | 0x0045AC60  DequantizeIDCT_Block_Half       [ARCH-DIVERGENCE: FMV] 0x0045ACD0  InitMotionVectorDecoder         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1067 |
| 0x0045acd0 | _unchanged_ | _unchanged_ | low | 0x0045ACD0  InitMotionVectorDecoder         [ARCH-DIVERGENCE: FMV] 0x0045AD90  BuildHuffmanDecodeTables        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1068 |
| 0x0045ad90 | _unchanged_ | _unchanged_ | low | 0x0045AD90  BuildHuffmanDecodeTables        [ARCH-DIVERGENCE: FMV] 0x0045AF50  ParseJPEGHeaders                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1069 |
| 0x0045af50 | _unchanged_ | _unchanged_ | low | 0x0045AF50  ParseJPEGHeaders                [ARCH-DIVERGENCE: FMV] 0x0045B1C0  BitstreamRefillBits             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1070 |
| 0x0045b1c0 | _unchanged_ | _unchanged_ | low | 0x0045B1C0  BitstreamRefillBits             [ARCH-DIVERGENCE: FMV] 0x0045B220  BitstreamReadHuffmanCode        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1071 |
| 0x0045b220 | _unchanged_ | _unchanged_ | low | 0x0045B220  BitstreamReadHuffmanCode        [ARCH-DIVERGENCE: FMV] 0x0045B250  FillBlock_DC                    [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1072 |
| 0x0045b250 | _unchanged_ | _unchanged_ | low | 0x0045B250  FillBlock_DC                    [ARCH-DIVERGENCE: FMV] 0x0045B2B0  DecodeMotionCompensatedFrame    [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1073 |
| 0x0045b2b0 | _unchanged_ | _unchanged_ | low | 0x0045B2B0  DecodeMotionCompensatedFrame    [ARCH-DIVERGENCE: FMV] 0x0045B420  DecodeType2DeltaPalette         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1074 |
| 0x0045b420 | _unchanged_ | _unchanged_ | low | 0x0045B420  DecodeType2DeltaPalette         [ARCH-DIVERGENCE: FMV] 0x0045B750  BuildBitplaneDeinterleaveTable  [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1075 |
| 0x0045b750 | _unchanged_ | _unchanged_ | low | 0x0045B750  BuildBitplaneDeinterleaveTable  [ARCH-DIVERGENCE: FMV] 0x0045B7D0  DecodeVQ_Block                  [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1076 |
| 0x0045b7d0 | _unchanged_ | _unchanged_ | low | 0x0045B7D0  DecodeVQ_Block                  [ARCH-DIVERGENCE: FMV] 0x0045B8D0  DecodeType1PaletteFrame         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1077 |
| 0x0045b8d0 | _unchanged_ | _unchanged_ | low | 0x0045B8D0  DecodeType1PaletteFrame         [ARCH-DIVERGENCE: FMV] 0x0045BA30  ConvertPaletteToPixelFormat     [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1078 |
| 0x0045ba30 | _unchanged_ | _unchanged_ | low | 0x0045BA30  ConvertPaletteToPixelFormat     [ARCH-DIVERGENCE: FMV] 0x0045BAE0  BlitDecodedToSurface            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1079 |
| 0x0045bae0 | _unchanged_ | _unchanged_ | low | 0x0045BAE0  BlitDecodedToSurface            [ARCH-DIVERGENCE: FMV] 0x0045BD00  SetVGAPalette                   [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1080 |
| 0x0045bd00 | _unchanged_ | _unchanged_ | low | 0x0045BD00  SetVGAPalette                   [ARCH-DIVERGENCE: FMV] 0x0045BD3C  DecompressLZData                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1081 |
| 0x0045bd3c | _unchanged_ | _unchanged_ | low | 0x0045BD3C  DecompressLZData                [ARCH-DIVERGENCE: FMV] 0x0045BE5F  UnpackBitplaneToPixels          [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1082 |
| 0x0045be5f | _unchanged_ | _unchanged_ | low | 0x0045BE5F  UnpackBitplaneToPixels          [ARCH-DIVERGENCE: FMV] 0x0045BF08  DecodeBitmapRLERun              [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1083 |
| 0x0045bf08 | _unchanged_ | _unchanged_ | low | 0x0045BF08  DecodeBitmapRLERun              [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1084 |
| 0x0045d6d8 | _unchanged_ | _unchanged_ | low | Inter-digit gap is 2.0 [CONFIRMED @ 0x45d6d8]. */ float sf_gw = sx * 15.0f; | td5_hud.c:2193 |
| 0x004631a0 | _unchanged_ | _unchanged_ | low | DAT_004631a4 is 0 for all 12 span types [CONFIRMED @ 0x004631A0]. DAT_004631a0 values [CONFIRMED @ 0x004631A0]: | DAT_004631a0 values [CONFIRMED @ 0x004631A0]: span_type: 0  1  2  3  4  5  6  7  8  9 10 11 */ | td5_physics.c:5380 |
| 0x00465f74 | _unchanged_ | _unchanged_ | low | Format: track-label  = "%d. %s" (track# 1-based + band name)   [CONFIRMED @ 0x465f74] | td5_frontend.c:173 |
| 0x004667a8 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x4667A8] English archive entry = "config.eng"; token 0 = display name. | English entry name = "config.eng" [CONFIRMED @ 0x4667A8]. The original reads "config.eng" per car ZIP, sscanf's 17 tokens into | td5_frontend.c:1063 |
| 0x00466808 | _unchanged_ | _unchanged_ | low | Copyright string [CONFIRMED @ 0x00466808] */ static const char k_copyright[] = "TEST DRIVE 5 COPYRIGHT 1998"; | Original renders "TEST DRIVE 5 COPYRIGHT 1998" [CONFIRMED @ 0x00466808] at x=canvasW/10, y=0x20 (32px) and repeats each row down the screen. | td5_frontend.c:5133 |
| 0x004743f4 | _unchanged_ | _unchanged_ | low | - [CONFIRMED @ 0x4743F4] Texture "SEMICOL" (not "SKIDMARK" which doesn't exist). | td5_vfx.c:1669 |
| 0x0048d988 | _unchanged_ | _unchanged_ | low | [CONFIRMED @ 0x0048d988] s_results mirrors original ResultsTable at that addr [CONFIRMED @ 0x40AAD0 / 0x40AB80] sort functions populate final_position | td5_game.c:966 |

## Key discoveries

- (Mechanical comment-sync batch; no new findings.)

## Out-of-scope finds

- (None — this batch only consolidates existing port audit headers.)

## TODO impact

- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.

