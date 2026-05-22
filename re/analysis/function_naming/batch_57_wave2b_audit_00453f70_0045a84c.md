---
batch: 57
area: wave2b_audit_comment_sync
tier: T6
target_todos: []
ghidra_session: (per-batch, ghidra-apply opens master)
analyzed_addresses: 0x00453f70,0x00454020,0x00454170,0x004542c0,0x00454690,0x00454750,0x00454830,0x004548c0,0x004549d0,0x00454bd0,0x00454ce0,0x00454e90,0x00454f30,0x00454fd0,0x00455070,0x004552b0,0x00455580,0x00455770,0x00455870,0x00455950,0x00455ad0,0x00455d60,0x00455de0,0x00455ff0,0x00456080,0x004560c0,0x00456110,0x004561d0,0x00456210,0x00456250,0x004562a0,0x004562c0,0x00456430,0x00456450,0x004564f3,0x00456670,0x0045681d,0x00456926,0x00456b31,0x00456c94,0x00456d9e,0x00456f40,0x00456f70,0x004573f9,0x00457684,0x0045791f,0x004580fe,0x004588e0,0x00458bc9,0x004590bf,0x004592ad,0x0045949b,0x00459689,0x00459877,0x004599d0,0x00459ea0,0x0045a3a0,0x0045a588,0x0045a5cc,0x0045a84c
agent: claude-opus-4-7
date: 2026-05-22
---

# Wave 2B audit-comment sync — batch 57 (60 addresses)

## Summary

- Addresses in this batch: 60
- All proposals are confidence=low (comment-only, no rename)
- Each address receives a consolidated PLATE comment derived from port-source audit headers

## Methodology

Pure derived from `re/analysis/followup_sessions/wave1_f_comment_sync_plan.csv` —
port [CONFIRMED @ ...] / [ARCH-DIVERGENCE: ...] audit-header references extracted
and deduped per address. Comment text combines unique header summaries.

## Proposals (functions)

| address | current_name | proposed_name | confidence | evidence | port_mirror |
|---|---|---|---|---|---|
| 0x00453f70 | _unchanged_ | _unchanged_ | low | 0x00453F70  QuerySurfacePixelFormat         [ARCH-DIVERGENCE: FMV] 0x00454020  ReleaseVideoResources           [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1004 |
| 0x00454020 | _unchanged_ | _unchanged_ | low | 0x00454020  ReleaseVideoResources           [ARCH-DIVERGENCE: FMV] 0x00454170  CreateDDrawSurface32            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1005 |
| 0x00454170 | _unchanged_ | _unchanged_ | low | 0x00454170  CreateDDrawSurface32            [ARCH-DIVERGENCE: FMV] 0x004542C0  DecodeVideoFrame                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1006 |
| 0x004542c0 | _unchanged_ | _unchanged_ | low | 0x004542C0  DecodeVideoFrame                [ARCH-DIVERGENCE: FMV] 0x00454690  BuildDequantizationTable        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1007 |
| 0x00454690 | _unchanged_ | _unchanged_ | low | 0x00454690  BuildDequantizationTable        [ARCH-DIVERGENCE: FMV] 0x00454750  BuildFullQuantTable             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1008 |
| 0x00454750 | _unchanged_ | _unchanged_ | low | 0x00454750  BuildFullQuantTable             [ARCH-DIVERGENCE: FMV] 0x00454830  ComputeBlitGeometry             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1009 |
| 0x00454830 | _unchanged_ | _unchanged_ | low | 0x00454830  ComputeBlitGeometry             [ARCH-DIVERGENCE: FMV] 0x004548C0  DecodeCompressedPixelBlock16    [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1010 |
| 0x004548c0 | _unchanged_ | _unchanged_ | low | 0x004548C0  DecodeCompressedPixelBlock16    [ARCH-DIVERGENCE: FMV] 0x004549D0  WriteBlock16_3to6               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1011 |
| 0x004549d0 | _unchanged_ | _unchanged_ | low | 0x004549D0  WriteBlock16_3to6               [ARCH-DIVERGENCE: FMV] 0x00454BD0  DecodeCompressedPixelBlock32    [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1012 |
| 0x00454bd0 | _unchanged_ | _unchanged_ | low | 0x00454BD0  DecodeCompressedPixelBlock32    [ARCH-DIVERGENCE: FMV] 0x00454CE0  WriteBlock32_3to6               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1013 |
| 0x00454ce0 | _unchanged_ | _unchanged_ | low | 0x00454CE0  WriteBlock32_3to6               [ARCH-DIVERGENCE: FMV] 0x00454E90  DecodeVideoFrameBlocks8x8       [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1014 |
| 0x00454e90 | _unchanged_ | _unchanged_ | low | 0x00454E90  DecodeVideoFrameBlocks8x8       [ARCH-DIVERGENCE: FMV] 0x00454F30  DecodeVideoFrameBlocks16x16     [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1015 |
| 0x00454f30 | _unchanged_ | _unchanged_ | low | 0x00454F30  DecodeVideoFrameBlocks16x16     [ARCH-DIVERGENCE: FMV] 0x00454FD0  DecodeVideoFrameBlocksOverlay   [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1016 |
| 0x00454fd0 | _unchanged_ | _unchanged_ | low | 0x00454FD0  DecodeVideoFrameBlocksOverlay   [ARCH-DIVERGENCE: FMV] 0x00455070  ParseVideoFrameHeader           [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1017 |
| 0x00455070 | _unchanged_ | _unchanged_ | low | 0x00455070  ParseVideoFrameHeader           [ARCH-DIVERGENCE: FMV] 0x004552B0  CreateVideoSurfaces             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1018 |
| 0x004552b0 | _unchanged_ | _unchanged_ | low | 0x004552B0  CreateVideoSurfaces             [ARCH-DIVERGENCE: FMV] 0x00455580  CreateDDrawOverlaySurface       [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1019 |
| 0x00455580 | _unchanged_ | _unchanged_ | low | 0x00455580  CreateDDrawOverlaySurface       [ARCH-DIVERGENCE: FMV] 0x00455770  CreateDDrawSurface16            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1020 |
| 0x00455770 | _unchanged_ | _unchanged_ | low | 0x00455770  CreateDDrawSurface16            [ARCH-DIVERGENCE: FMV] 0x00455870  RestoreLostSurfaces             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1021 |
| 0x00455870 | _unchanged_ | _unchanged_ | low | 0x00455870  RestoreLostSurfaces             [ARCH-DIVERGENCE: FMV] 0x00455950  DecodeEAIMA_Mono                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1022 |
| 0x00455950 | _unchanged_ | _unchanged_ | low | 0x00455950  DecodeEAIMA_Mono                [ARCH-DIVERGENCE: FMV] 0x00455AD0  DecodeEAIMA_Stereo              [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1023 |
| 0x00455ad0 | _unchanged_ | _unchanged_ | low | 0x00455AD0  DecodeEAIMA_Stereo              [ARCH-DIVERGENCE: FMV] 0x00455D60  AudioDecoderThreadFunc          [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1024 |
| 0x00455d60 | _unchanged_ | _unchanged_ | low | 0x00455D60  AudioDecoderThreadFunc          [ARCH-DIVERGENCE: FMV] 0x00455DE0  WriteAudioToDirectSound         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1025 |
| 0x00455de0 | _unchanged_ | _unchanged_ | low | 0x00455DE0  WriteAudioToDirectSound         [ARCH-DIVERGENCE: FMV] 0x00455FF0  GetAudioPlayPosition            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1026 |
| 0x00455ff0 | _unchanged_ | _unchanged_ | low | 0x00455FF0  GetAudioPlayPosition            [ARCH-DIVERGENCE: FMV] 0x00456080  StartAudioDecoderThread         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1027 |
| 0x00456080 | _unchanged_ | _unchanged_ | low | 0x00456080  StartAudioDecoderThread         [ARCH-DIVERGENCE: FMV] 0x004560C0  StopAudioDecoderThread          [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1028 |
| 0x004560c0 | _unchanged_ | _unchanged_ | low | 0x004560C0  StopAudioDecoderThread          [ARCH-DIVERGENCE: FMV] 0x00456110  InitStreamDirectSound           [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1029 |
| 0x00456110 | _unchanged_ | _unchanged_ | low | 0x00456110  InitStreamDirectSound           [ARCH-DIVERGENCE: FMV] 0x004561D0  CreateDirectSoundDevice         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1030 |
| 0x004561d0 | _unchanged_ | _unchanged_ | low | 0x004561D0  CreateDirectSoundDevice         [ARCH-DIVERGENCE: FMV] 0x00456210  DDrawGetSurfaceCaps             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1031 |
| 0x00456210 | _unchanged_ | _unchanged_ | low | 0x00456210  DDrawGetSurfaceCaps             [ARCH-DIVERGENCE: FMV] 0x00456250  ReleaseDirectSoundDevice        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1032 |
| 0x00456250 | _unchanged_ | _unchanged_ | low | 0x00456250  ReleaseDirectSoundDevice        [ARCH-DIVERGENCE: FMV] 0x004562A0  DDrawReleaseSurface             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1033 |
| 0x004562a0 | _unchanged_ | _unchanged_ | low | 0x004562A0  DDrawReleaseSurface             [ARCH-DIVERGENCE: FMV] 0x004562C0  InitAudioPlayback               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1034 |
| 0x004562c0 | _unchanged_ | _unchanged_ | low | 0x004562C0  InitAudioPlayback               [ARCH-DIVERGENCE: FMV] 0x00456430  ReleaseDSStreamBuffer           [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1035 |
| 0x00456430 | _unchanged_ | _unchanged_ | low | 0x00456430  ReleaseDSStreamBuffer           [ARCH-DIVERGENCE: FMV] 0x00456450  DecodeADPCMBlock                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1036 |
| 0x00456450 | _unchanged_ | _unchanged_ | low | 0x00456450  DecodeADPCMBlock                [ARCH-DIVERGENCE: FMV] 0x004564F3  DecodeEAADPCM                   [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1037 |
| 0x004564f3 | _unchanged_ | _unchanged_ | low | 0x004564F3  DecodeEAADPCM                   [ARCH-DIVERGENCE: FMV] 0x00456670  IDCT_DecodeCoefficients         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1038 |
| 0x00456670 | _unchanged_ | _unchanged_ | low | 0x00456670  IDCT_DecodeCoefficients         [ARCH-DIVERGENCE: FMV] 0x0045681D  IDCT_Transform8x8_Caller        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1039 |
| 0x0045681d | _unchanged_ | _unchanged_ | low | 0x0045681D  IDCT_Transform8x8_Caller        [ARCH-DIVERGENCE: FMV] 0x00456926  IDCT_Transform8x8_Block         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1040 |
| 0x00456926 | _unchanged_ | _unchanged_ | low | 0x00456926  IDCT_Transform8x8_Block         [ARCH-DIVERGENCE: FMV] 0x00456B31  IDCT_1D_8pt_Float               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1041 |
| 0x00456b31 | _unchanged_ | _unchanged_ | low | 0x00456B31  IDCT_1D_8pt_Float               [ARCH-DIVERGENCE: FMV] 0x00456C94  IDCT_1D_8pt_Float_B             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1042 |
| 0x00456c94 | _unchanged_ | _unchanged_ | low | 0x00456C94  IDCT_1D_8pt_Float_B             [ARCH-DIVERGENCE: FMV] 0x00456D9E  IDCT_FullBlock8x8               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1043 |
| 0x00456d9e | _unchanged_ | _unchanged_ | low | 0x00456D9E  IDCT_FullBlock8x8               [ARCH-DIVERGENCE: FMV] 0x00456F40  LookupQuantTable                [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1044 |
| 0x00456f40 | _unchanged_ | _unchanged_ | low | 0x00456F40  LookupQuantTable                [ARCH-DIVERGENCE: FMV] 0x00456F70  IDCTDecodeDCAndAC               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1045 |
| 0x00456f70 | _unchanged_ | _unchanged_ | low | 0x00456F70  IDCTDecodeDCAndAC               [ARCH-DIVERGENCE: FMV] 0x004573F9  YCbCrToPackedPixel16            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1046 |
| 0x004573f9 | _unchanged_ | _unchanged_ | low | 0x004573F9  YCbCrToPackedPixel16            [ARCH-DIVERGENCE: FMV] 0x00457684  YCbCrToRGB_Row_16bit            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1047 |
| 0x00457684 | _unchanged_ | _unchanged_ | low | 0x00457684  YCbCrToRGB_Row_16bit            [ARCH-DIVERGENCE: FMV] 0x0045791F  IDCTDecode16x16Block            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1048 |
| 0x0045791f | _unchanged_ | _unchanged_ | low | 0x0045791F  IDCTDecode16x16Block            [ARCH-DIVERGENCE: FMV] 0x004580FE  DecodeJPEGFrame                 [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1049 |
| 0x004580fe | _unchanged_ | _unchanged_ | low | 0x004580FE  DecodeJPEGFrame                 [ARCH-DIVERGENCE: FMV] 0x004588E0  DecodeHuffmanBlock              [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1050 |
| 0x004588e0 | _unchanged_ | _unchanged_ | low | 0x004588E0  DecodeHuffmanBlock              [ARCH-DIVERGENCE: FMV] 0x00458BC9  DecodeJPEGMCU                   [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1051 |
| 0x00458bc9 | _unchanged_ | _unchanged_ | low | 0x00458BC9  DecodeJPEGMCU                   [ARCH-DIVERGENCE: FMV] 0x004590BF  DecodeMCU_YCbCr422_16bpp        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1052 |
| 0x004590bf | _unchanged_ | _unchanged_ | low | 0x004590BF  DecodeMCU_YCbCr422_16bpp        [ARCH-DIVERGENCE: FMV] 0x004592AD  DecodeMCU_YCbCr420_16bpp        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1053 |
| 0x004592ad | _unchanged_ | _unchanged_ | low | 0x004592AD  DecodeMCU_YCbCr420_16bpp        [ARCH-DIVERGENCE: FMV] 0x0045949B  DecodeMCU_YCbCr444_16bpp        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1054 |
| 0x0045949b | _unchanged_ | _unchanged_ | low | 0x0045949B  DecodeMCU_YCbCr444_16bpp        [ARCH-DIVERGENCE: FMV] 0x00459689  DecodeMCU_YCbCr422_16bpp_Fast   [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1055 |
| 0x00459689 | _unchanged_ | _unchanged_ | low | 0x00459689  DecodeMCU_YCbCr422_16bpp_Fast   [ARCH-DIVERGENCE: FMV] 0x00459877  DecodeMCU_YCbCr422_8bpp         [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1056 |
| 0x00459877 | _unchanged_ | _unchanged_ | low | 0x00459877  DecodeMCU_YCbCr422_8bpp         [ARCH-DIVERGENCE: FMV] 0x004599D0  YCbCrToRGB_Row_16bit_444        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1057 |
| 0x004599d0 | _unchanged_ | _unchanged_ | low | 0x004599D0  YCbCrToRGB_Row_16bit_444        [ARCH-DIVERGENCE: FMV] 0x00459EA0  YCbCrToRGB_Row_16bit_420        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1058 |
| 0x00459ea0 | _unchanged_ | _unchanged_ | low | 0x00459EA0  YCbCrToRGB_Row_16bit_420        [ARCH-DIVERGENCE: FMV] 0x0045A3A0  YCbCrToRGB_Row_8bit             [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1059 |
| 0x0045a3a0 | _unchanged_ | _unchanged_ | low | 0x0045A3A0  YCbCrToRGB_Row_8bit             [ARCH-DIVERGENCE: FMV] 0x0045A588  CopyPixelBuffer32               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1060 |
| 0x0045a588 | _unchanged_ | _unchanged_ | low | 0x0045A588  CopyPixelBuffer32               [ARCH-DIVERGENCE: FMV] 0x0045A5CC  YCbCrToRGB_Row_16bit_422        [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1061 |
| 0x0045a5cc | _unchanged_ | _unchanged_ | low | 0x0045A5CC  YCbCrToRGB_Row_16bit_422        [ARCH-DIVERGENCE: FMV] 0x0045A84C  IDCT8ColumnButterfly            [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1062 |
| 0x0045a84c | _unchanged_ | _unchanged_ | low | 0x0045A84C  IDCT8ColumnButterfly            [ARCH-DIVERGENCE: FMV] 0x0045A98B  IDCT8RowButterfly               [ARCH-DIVERGENCE: FMV] | td5_fmv.c:1063 |

## Key discoveries

- (Mechanical comment-sync batch; no new findings.)

## Out-of-scope finds

- (None — this batch only consolidates existing port audit headers.)

## TODO impact

- No TODO closure expected. This batch makes future audits faster by surfacing port-side analysis inside Ghidra.

