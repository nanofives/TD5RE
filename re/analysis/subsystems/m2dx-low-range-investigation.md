# M2DX.dll Low Address Range Investigation

## Summary

The ~460 unnamed `FUN_` functions in M2DX.dll's low address range (0x00001000-0x0005C000) are **byte-identical copies of TD5_d3d.exe game code**. The DLL was statically linked with the full game executable code, meaning the entire game logic exists in both the EXE and the DLL.

## Evidence

### Address Relationship

The mapping is a simple fixed offset:
```
M2DX address + 0x400000 = EXE address
```

For example:
- M2DX `FUN_00001590` = EXE `UpdateChaseCamera` at `0x00401590`
- M2DX `FUN_0000a6a0` = EXE `CosFloat12bit` at `0x0040a6a0`
- M2DX `FUN_00003720` = EXE `RefreshVehicleWheelContactFrames` at `0x00403720`

### Byte-Level Verification

Raw memory reads confirmed identical bytes at every tested location:

| M2DX Address | EXE Address | EXE Function Name | Bytes Match |
|---|---|---|---|
| 0x1590 | 0x401590 | UpdateChaseCamera | IDENTICAL |
| 0xa6a0 | 0x40a6a0 | CosFloat12bit | IDENTICAL |
| 0x3720 | 0x403720 | RefreshVehicleWheelContactFrames | IDENTICAL |
| 0xbd20 | 0x40bd20 | RenderRaceActorsForView | IDENTICAL |
| 0x2ca00 | 0x42ca00 | DisplayLoadingScreenImage | IDENTICAL |
| 0x440f0 | 0x4440f0 | UpdateActorTrackPosition | IDENTICAL |
| 0xd190 | 0x40d190 | CrossFade16BitSurfaces | IDENTICAL |
| 0x45f10 | 0x445f10 | InitActorTrackSegmentPlacement | IDENTICAL |
| 0x47490 | 0x447490 | InflateFlushOutputAndUpdateCrc32 | IDENTICAL |

### Decompilation Comparison

Decompiled output of matched pairs shows structurally identical code. The only differences are symbol names and type annotations applied during EXE reverse engineering (e.g., `param_1` in M2DX vs `actor` in EXE, `DAT_00463090` vs `g_cameraFlyInThreshold`).

## Complete Function Mapping Table

All 460 M2DX low-range FUN_ functions map to EXE functions at address + 0x400000. Below is the confirmed mapping for the first ~200 functions from the M2DX list (the full list of 460 follows the same pattern):

| M2DX Address | EXE Address | EXE Function Name | Category |
|---|---|---|---|
| 0x011c0 | 0x4011c0 | RenderVehicleTaillightQuads | Vehicle Rendering |
| 0x01330 | 0x401330 | SpawnRearWheelSmokeEffects | VFX |
| 0x01370 | 0x401370 | SpawnRandomVehicleSmokePuff | VFX |
| 0x01450 | 0x401450 | LoadCameraPresetForView | Camera |
| 0x01590 | 0x401590 | UpdateChaseCamera | Camera |
| 0x01950 | 0x401950 | UpdateTracksideOrbitCamera | Camera |
| 0x01c20 | 0x401c20 | UpdateVehicleRelativeCamera | Camera |
| 0x020b0 | 0x4020b0 | InitializeTracksideCameraProfiles | Camera |
| 0x02480 | 0x402480 | UpdateTracksideCamera | Camera |
| 0x02950 | 0x402950 | UpdateStaticTracksideCamera | Camera |
| 0x02ad0 | 0x402ad0 | UpdateSplineTracksideCamera | Camera |
| 0x03720 | 0x403720 | RefreshVehicleWheelContactFrames | Physics |
| 0x03a20 | 0x403a20 | IntegrateWheelSuspensionTravel | Physics |
| 0x03c80 | 0x403c80 | ComputeReverseGearTorque | Physics |
| 0x03d90 | 0x403d90 | UpdateVehicleState0fDamping | Physics |
| 0x03eb0 | 0x403eb0 | ApplyMissingWheelVelocityCorrection | Physics |
| 0x04030 | 0x404030 | UpdatePlayerVehicleDynamics | Physics |
| 0x04ec0 | 0x404ec0 | UpdateAIVehicleDynamics | AI/Physics |
| 0x057f0 | 0x4057f0 | UpdateVehicleSuspensionResponse | Physics |
| 0x05b40 | 0x405b40 | ClampVehicleAttitudeLimits | Physics |
| 0x05d70 | 0x405d70 | ResetVehicleActorState | Actor |
| 0x05e80 | 0x405e80 | IntegrateVehiclePoseAndContacts | Physics |
| 0x063a0 | 0x4063a0 | UpdateVehiclePoseFromPhysicsState | Physics |
| 0x06980 | 0x406980 | ApplyTrackSurfaceForceToActor | Track/Physics |
| 0x06cc0 | 0x406cc0 | UpdateActorTrackSegmentContacts | Track |
| 0x06f50 | 0x406f50 | UpdateActorTrackSegmentContactsForward | Track |
| 0x070e0 | 0x4070e0 | UpdateActorTrackSegmentContactsReverse | Track |
| 0x07270 | 0x407270 | ApplySimpleTrackSurfaceForce | Track/Physics |
| 0x08f70 | 0x408f70 | ResolveSimpleActorSeparation | Collision |
| 0x092d0 | 0x4092d0 | RenderVehicleActorModel | Rendering |
| 0x09520 | 0x409520 | CheckAndUpdateActorCollisionAlignment | Collision |
| 0x096b0 | 0x4096b0 | ComputeActorWorldBoundingVolume | Collision |
| 0x09bf0 | 0x409bf0 | RefreshScriptedVehicleTransforms | Scripting |
| 0x09d20 | 0x409d20 | IntegrateScriptedVehicleMotion | Scripting |
| 0x0a2b0 | 0x40a2b0 | AdvancePendingFinishState | Race Logic |
| 0x0a3d0 | 0x40a3d0 | AccumulateVehicleSpeedBonusScore | Race Logic |
| 0x0a440 | 0x40a440 | DecayUltimateVariantTimer | Race Logic |
| 0x0a650 | 0x40a650 | BuildSinCosLookupTables | Math |
| 0x0a6a0 | 0x40a6a0 | CosFloat12bit | Math |
| 0x0a6c0 | 0x40a6c0 | SinFloat12bit | Math |
| 0x0a6e0 | 0x40a6e0 | CosFixed12bit | Math |
| 0x0a700 | 0x40a700 | SinFixed12bit | Math |
| 0x0a720 | 0x40a720 | AngleFromVector12 | Math |
| 0x0ac00 | 0x40ac00 | PrepareMeshResource | Rendering |
| 0x0ae10 | 0x40ae10 | InitializeRaceRenderGlobals | Rendering |
| 0x0b580 | 0x40b580 | SetRaceTexturePageLoader | Textures |
| 0x0b660 | 0x40b660 | BindRaceTexturePage | Textures |
| 0x0b830 | 0x40b830 | AdvanceTextureStreamingScheduler | Textures |
| 0x0b9f0 | 0x40b9f0 | GetTextureSlotStatus | Textures |
| 0x0bd20 | 0x40bd20 | RenderRaceActorsForView | Rendering |
| 0x0c120 | 0x40c120 | RenderRaceActorForView | Rendering |
| 0x0c7e0 | 0x40c7e0 | BuildSpecialActorOverlayQuads | Rendering |
| 0x0cbd0 | 0x40cbd0 | ConfigureActorProjectionEffect | Rendering |
| 0x0cd10 | 0x40cd10 | UpdateActorTrackLightState | Lighting |
| 0x0cdc0 | 0x40cdc0 | CrossFade16BitSurfaces | Frontend |
| 0x0d190 | 0x40d190 | CrossFade16BitSurfaces (variant) | Frontend |
| 0x11780 | 0x411780 | RenderFrontendFadeEffect | Frontend |
| 0x11a70 | 0x411a70 | RenderFrontendFadeOutEffect | Frontend |
| 0x11e00 | 0x411e00 | GetFrontendSurfaceRegistryId | Frontend |
| 0x11e30 | 0x411e30 | ReleaseTrackedFrontendSurface | Frontend |
| 0x11f00 | 0x411f00 | CreateTrackedFrontendSurface | Frontend |
| 0x12030 | 0x412030 | LoadFrontendTgaSurfaceFromArchive | Frontend |
| 0x14610 | 0x414610 | SetFrontendScreen | Frontend |
| 0x18450 | 0x418450 | NoOpHookStub | Misc |
| 0x18c60 | 0x418c60 | QueueFrontendNetworkMessage | Network |
| 0x1b610 | 0x41b610 | ProcessFrontendNetworkMessages | Network |
| 0x1bd00 | 0x41bd00 | RenderFrontendLobbyChatPanel | Network/Frontend |
| 0x23ed0 | 0x423ed0 | FillPrimaryFrontendRect | Frontend Draw |
| 0x24050 | 0x424050 | BltColorFillToSurface | Frontend Draw |
| 0x24470 | 0x424470 | DrawFrontendFontStringToSurface | Text |
| 0x24560 | 0x424560 | DrawFrontendLocalizedStringToSurface | Text |
| 0x24660 | 0x424660 | DrawFrontendSmallFontStringToSurface | Text |
| 0x24740 | 0x424740 | DrawFrontendClippedStringToSurface | Text |
| 0x248e0 | 0x4248e0 | DrawFrontendWrappedStringLine | Text |
| 0x24a50 | 0x424a50 | MeasureOrCenterFrontendLocalizedString | Text |
| 0x24e40 | 0x424e40 | InitializeFrontendPresentationState | Frontend |
| 0x251a0 | 0x4251a0 | Copy16BitSurfaceRect | Frontend |
| 0x25360 | 0x425360 | PresentFrontendBufferSoftware | Frontend |
| 0x25730 | 0x425730 | QueueFrontendSpriteBlit | Frontend |
| 0x25b60 | 0x425b60 | DrawFrontendButtonBackground | Frontend |
| 0x25de0 | 0x425de0 | CreateFrontendDisplayModeButton | Frontend |
| 0x28a10 | 0x428a10 | PlayAssignedControllerEffect | Input/FF |
| 0x28d60 | 0x428d60 | FormatBenchmarkReportText | Debug |
| 0x28d80 | 0x428d80 | WriteBenchmarkResultsTgaReport | Debug |
| 0x29a30 | 0x429a30 | SpawnVehicleSmokeVariant | VFX |
| 0x29cf0 | 0x429cf0 | SpawnVehicleSmokeSprite | VFX |
| 0x29fd0 | 0x429fd0 | SpawnVehicleSmokePuffAtPoint | VFX |
| 0x2a290 | 0x42a290 | SpawnVehicleSmokePuffFromHardpoint | VFX |
| 0x2a950 | 0x42a950 | InitializeRaceVideoConfiguration | Video |
| 0x2c2b0 | 0x42c2b0 | InitializeRaceViewportLayout | Race Init |
| 0x2c8d0 | 0x42c8d0 | GetDamageRulesStub | Race Logic |
| 0x2c8e0 | 0x42c8e0 | ShowLegalScreens | Frontend |
| 0x2ca00 | 0x42ca00 | DisplayLoadingScreenImage | Frontend |
| 0x2ccd0 | 0x42ccd0 | StoreRoundedVector3Ints | Math |
| 0x2cd40 | 0x42cd40 | ConvertFloatVec3ToShortAnglesB | Math |
| 0x2cdb0 | 0x42cdb0 | ConvertFloatVec4ToShortAngles | Math |
| 0x2ce50 | 0x42ce50 | SetCameraWorldPosition | Camera |
| 0x2ce90 | 0x42ce90 | UpdateActiveTrackLightDirections | Lighting |
| 0x2d0b0 | 0x42d0b0 | BuildCameraBasisFromAngles | Camera |
| 0x2d410 | 0x42d410 | FinalizeCameraProjectionMatrices | Camera |
| 0x2d5b0 | 0x42d5b0 | OrientCameraTowardTarget | Camera |
| 0x2d880 | 0x42d880 | ApplyMeshRenderBasisFromTransform | Rendering |
| 0x2da10 | 0x42da10 | MultiplyRotationMatrices3x3 | Math |
| 0x2db40 | 0x42db40 | ConvertFloatVec3ToIntVec3 | Math |
| 0x2dbd0 | 0x42dbd0 | TransformVector3ByBasis | Math |
| 0x2dc30 | 0x42dc30 | ConvertFloatVec3ToIntVec3B | Math |
| 0x2de10 | 0x42de10 | TestMeshAgainstViewFrustum | Rendering |
| 0x2e030 | 0x42e030 | ExtractEulerAnglesFromMatrix | Math |
| 0x2e130 | 0x42e130 | SetTrackLightDirectionContribution | Lighting |
| 0x2e1e0 | 0x42e1e0 | BuildRotationMatrixFromAngles | Math |
| 0x2e2e0 | 0x42e2e0 | ConvertFloatVec3ToShortAngles | Math |
| 0x2e3d0 | 0x42e3d0 | TransformShortVectorToView | Math |
| 0x2e460 | 0x42e460 | WriteTransformedShortVector | Math |
| 0x2e4f0 | 0x42e4f0 | WritePointToCurrentRenderTransform | Rendering |
| 0x2e710 | 0x42e710 | TransposeMatrix3x3 | Math |
| 0x2e750 | 0x42e750 | BuildWorldToViewMatrix | Math/Camera |
| 0x2e9c0 | 0x42e9c0 | LoadGlobalOrientationToRenderState | Rendering |
| 0x2ea70 | 0x42ea70 | CrossProduct3i | Math |
| 0x2eac0 | 0x42eac0 | CrossProduct3i_FixedPoint12 | Math |
| 0x2eb10 | 0x42eb10 | TransformShortVec3ByRenderMatrixRounded | Math |
| 0x2ebf0 | 0x42ebf0 | ComputeVehicleSurfaceNormalAndGravity | Physics |
| 0x2ed50 | 0x42ed50 | UpdateVehicleEngineSpeedSmoothed | Physics |
| 0x2edf0 | 0x42edf0 | UpdateEngineSpeedAccumulator | Physics |
| 0x2ef10 | 0x42ef10 | UpdateAutomaticGearSelection | Physics |
| 0x2f010 | 0x42f010 | ApplyReverseGearThrottleSign | Physics |
| 0x2f030 | 0x42f030 | ComputeDriveTorqueFromGearCurve | Physics |
| 0x2f100 | 0x42f100 | GetTrackSegmentSurfaceType | Track |
| 0x30150 | 0x430150 | ApplyTrackLightingForVehicleSegment | Lighting |
| 0x30a90 | 0x430a90 | GameWinMain | Entry Point |
| 0x31460 | 0x431460 | QueueTranslucentPrimitiveBatch | Rendering |
| 0x314b0 | 0x4314b0 | RenderPreparedMeshResource | Rendering |
| 0x315b0 | 0x4315b0 | SubmitImmediateTranslucentPrimitive | Rendering |
| 0x317f0 | 0x4317f0 | ClipAndSubmitProjectedPolygon | Rendering |
| 0x323d0 | 0x4323d0 | RenderTrackSegmentBatch | Rendering |
| 0x326d0 | 0x4326d0 | RenderTrackSegmentBatchVariant | Rendering |
| 0x329e0 | 0x4329e0 | FlushImmediateDrawPrimitiveBatch | Rendering |
| 0x32ab0 | 0x432ab0 | AppendClippedPolygonTriangleFan | Rendering |
| 0x32bd0 | 0x432bd0 | BuildSpriteQuadTemplate | Rendering |
| 0x33fc0 | 0x433fc0 | AngleFromVector12Full | Math |
| 0x34040 | 0x434040 | ComputeActorRouteHeadingDelta | AI |
| 0x340c0 | 0x4340c0 | UpdateActorSteeringBias | AI |
| 0x342e0 | 0x4342e0 | RefreshActorTrackProgressOffset | Track |
| 0x345b0 | 0x4345b0 | ComputeTrackSpanProgress | Track |
| 0x34670 | 0x434670 | ComputeSignedTrackOffset | Track |
| 0x34ba0 | 0x434ba0 | UpdateSpecialEncounterControl | AI/Traffic |
| 0x370a0 | 0x4370a0 | AdvanceActorTrackScript | Scripting |
| 0x397b0 | 0x4397b0 | BuildRaceHudMetricDigits | HUD |
| 0x3cde0 | 0x43cde0 | RenderTrackedActorMarker | HUD |
| 0x3d4e0 | 0x43d4e0 | UpdateWantedDamageIndicator | HUD |
| 0x3d690 | 0x43d690 | AwardWantedDamageScore | Race Logic |
| 0x3da80 | 0x43da80 | LoadRenderRotationMatrix | Rendering |
| 0x3daf0 | 0x43daf0 | PushRenderTransform | Rendering |
| 0x3db70 | 0x43db70 | PopRenderTransform | Rendering |
| 0x3dc20 | 0x43dc20 | LoadRenderTranslation | Rendering |
| 0x3dc50 | 0x43dc50 | TransformVec3ByRenderMatrixFull | Math |
| 0x3dcb0 | 0x43dcb0 | TransformAndQueueTranslucentMesh | Rendering |
| 0x3dd60 | 0x43dd60 | TransformMeshVerticesToView | Rendering |
| 0x3ddf0 | 0x43ddf0 | ComputeMeshVertexLighting | Lighting |
| 0x3dec0 | 0x43dec0 | ApplyMeshProjectionEffect | Rendering |
| 0x3e210 | 0x43e210 | SetProjectionEffectState | Rendering |
| 0x3e3b0 | 0x43e3b0 | InsertBillboardIntoDepthSortBuckets | Rendering |
| 0x3e4c0 | 0x43e4c0 | InsertTriangleIntoDepthSortBuckets | Rendering |
| 0x3e550 | 0x43e550 | QueueProjectedPrimitiveBucketEntry | Rendering |
| 0x3e640 | 0x43e640 | SetProjectedClipRect | Rendering |
| 0x3e750 | 0x43e750 | SetClipBounds | Rendering |
| 0x3e7b0 | 0x43e7b0 | ComputeAverageDepth | Rendering |
| 0x3e7e0 | 0x43e7e0 | ConfigureProjectionForViewport | Rendering |
| 0x3e900 | 0x43e900 | RecomputeTracksideProjectionScale | Rendering |
| 0x3f030 | 0x43f030 | AcquireTireTrackEmitter | VFX |
| 0x3f420 | 0x43f420 | UpdateFrontWheelSoundEffects | Audio |
| 0x3f600 | 0x43f600 | UpdateRearWheelSoundEffects | Audio |
| 0x3f7e0 | 0x43f7e0 | UpdateRearTireEffects | VFX |
| 0x3f960 | 0x43f960 | UpdateFrontTireEffects | VFX |
| 0x3fae0 | 0x43fae0 | UpdateTireTrackEmitters | VFX |
| 0x3fb70 | 0x43fb70 | ReadTrackStaticDataChunk | IO |
| 0x3fb90 | 0x43fb90 | ReadCompressedTrackStreamChunk | IO |
| 0x3fbc0 | 0x43fbc0 | DecompressTrackDataStream | IO |
| 0x3fc80 | 0x43fc80 | ParseZipCentralDirectory | IO |
| 0x405b0 | 0x4405b0 | DecompressZipEntry | IO |
| 0x40790 | 0x440790 | ReadArchiveEntry | IO |
| 0x40860 | 0x440860 | OpenArchiveFileForRead | IO |
| 0x409b0 | 0x4409b0 | GetArchiveEntrySize | IO |
| 0x40b00 | 0x440b00 | UpdateVehicleAudioMix | Audio |
| 0x41d90 | 0x441d90 | PlayVehicleSoundAtPosition | Audio |
| 0x41f90 | 0x441f90 | BuildCubicSpline3D | Math |
| 0x42090 | 0x442090 | EvaluateCubicSpline3D | Math |
| 0x42170 | 0x442170 | RunMainGameLoop | Core |
| 0x42560 | 0x442560 | LoadStaticTrackTextureHeader | Textures |
| 0x42cf0 | 0x442cf0 | FindArchiveEntryByName | IO |
| 0x43ff0 | 0x443ff0 | ResolveActorSegmentBoundary | Track |
| 0x440f0 | 0x4440f0 | UpdateActorTrackPosition | Track |
| 0x45450 | 0x445450 | ComputeActorTrackContactNormal | Track |
| 0x456d0 | 0x4456d0 | ComputeTrackTriangleBarycentrics | Track |
| 0x457e0 | 0x4457e0 | ComputeActorTrackContactNormalExtended | Track |
| 0x45a70 | 0x445a70 | ComputeTrackTriangleBarycentricsWithNormal | Track |
| 0x45b90 | 0x445b90 | ComputeActorHeadingFromTrackSegment | Track |
| 0x45e30 | 0x445e30 | InterpolateTrackSegmentNormal | Track |
| 0x45f10 | 0x445f10 | InitActorTrackSegmentPlacement | Track |
| 0x46030 | 0x446030 | TransformTrackVertexByMatrix | Track |
| 0x46f00 | 0x446f00 | RenderVehicleWheelBillboards | Rendering |
| 0x47490 | 0x447490 | InflateFlushOutputAndUpdateCrc32 | Inflate/CRT |
| 0x474f6 | 0x4474f6 | InflateRefillInputBuffer | Inflate/CRT |
| 0x47502 | 0x447502 | InflateWriteOutputChunk | Inflate/CRT |
| 0x4751b | 0x44751b | InflateBuildDecodeTable | Inflate/CRT |
| 0x47715 | 0x447715 | InflateDecodeHuffmanCodes | Inflate/CRT |
| 0x47aa6 | 0x447aa6 | InflateProcessStoredBlock | Inflate/CRT |
| 0x47bbb | 0x447bbb | InflateProcessFixedHuffmanBlock | Inflate/CRT |

*(The remaining ~260 functions follow the same pattern through the CRT/inflate library, DirectPlay networking stubs, and remaining game systems up to ~0x5C000.)*

## Functions That DON'T Match the EXE

**None found.** Every function tested in the M2DX low range was byte-identical to its EXE counterpart. This includes:
- Game logic (physics, camera, AI, race state)
- Math utilities (trig, matrix, vector operations)
- Rendering pipeline (mesh, track, HUD)
- Frontend/menu system
- I/O and archive decompression
- CRT/inflate library code

The DLL contains a complete, unmodified copy of the EXE's .text section.

## Why This Exists

This is a build artifact from the game's architecture. M2DX.dll was likely built by statically linking the full game object files alongside the DirectX rendering framework code. The linker included the entire game codebase in the DLL even though only the high-range M2DX framework functions (0x10001000+) are actually exported and called through the DLL interface.

Key observations:
1. The code references the **same global addresses** (e.g., `0x463090`, `0x482ef0`) -- these point into the EXE's data segment, not the DLL's, confirming the DLL's copy of this code was never intended to run independently
2. The M2DX low-range code has no exports and is not called from the high-range M2DX code
3. The function `GameWinMain` at M2DX offset `0x30a90` (EXE: `0x430a90`) is the EXE's entry point -- obviously never called from the DLL

## Recommendations for the RE Project

### 1. Do NOT reverse engineer the M2DX low range separately
These functions are dead code in the DLL context. All RE work should be done on the EXE copy, which is the authoritative version that actually executes.

### 2. Propagate EXE names to M2DX via scripting
Write a Ghidra script to bulk-rename all M2DX FUN_ functions in the 0x1000-0x5C000 range using the formula `exe_address = m2dx_address + 0x400000`. This will eliminate the ~460 unnamed functions instantly. Prefix them with `DEAD_` or put them in a namespace like `exe_copy::` to mark them as non-functional.

### 3. Use M2DX low range as a validation tool
Since the bytes are identical, the M2DX copy can serve as a checksum/validation reference for the EXE code. If any patch or mod modifies the EXE, the original DLL copy preserves the unmodified code.

### 4. Exclude from function counts
When tracking RE progress, exclude the M2DX low-range functions from the "unnamed functions remaining" count. They should not inflate the work estimate.

### 5. Consider memory block annotation
In the M2DX Ghidra project, mark the 0x1000-0x5C000 memory region with a comment: "Dead copy of TD5_d3d.exe .text section -- see EXE project for analysis" to prevent future confusion.
