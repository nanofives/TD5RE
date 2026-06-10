# re/analysis INDEX

> Generated 2026-06-10. One line per file (filename + first-heading hook) so a future session can find the right doc without reading all ~324 files. Regenerate by re-running an inventory pass (list `*.md`, print first heading per file); keep one line per file.

## Formats & file layouts

- [dat_retirement_formats.md](dat_retirement_formats.md) — DAT retirement: editable source formats (JSON/glTF/PNG) packed back to original binary layout at load
- [actor_struct_gap_audit_2026-05-20.md](actor_struct_gap_audit_2026-05-20.md) — TD5_Actor struct 18-gap audit (0x388-stride field resolution)
- [3d-asset-formats.md](formats/3d-asset-formats.md) — MODELS.DAT, TEXTURES.DAT, tpage%d.dat, himodel.dat, SKY.PRR layouts
- [archive-and-asset-loading.md](formats/archive-and-asset-loading.md) — ZIP archive system + vehicle asset loading pipeline
- [car-zip-and-static-zip-contents.md](formats/car-zip-and-static-zip-contents.md) — complete file inventory of car ZIPs and static.zip
- [carparam-physics-table.md](formats/carparam-physics-table.md) — carparam.dat physics table layout
- [custom-pe-sections.md](formats/custom-pe-sections.md) — custom PE sections IDCT_DAT and UVA_DATA
- [data-structure-gaps-filled.md](formats/data-structure-gaps-filled.md) — data-structure gaps filled (items 8-17)
- [data-tables-decoded.md](formats/data-tables-decoded.md) — decoded misc data tables in TD5_d3d.exe
- [ghidra-types-created.md](formats/ghidra-types-created.md) — Ghidra data types created (2026-03-28)
- [global-variable-catalog.md](formats/global-variable-catalog.md) — TD5_d3d.exe global variable catalog
- [indirect-calls-and-dispatch-tables.md](formats/indirect-calls-and-dispatch-tables.md) — indirect calls + dispatch table inventory
- [patchable-limits-catalog.md](formats/patchable-limits-catalog.md) — catalog of patchable engine limits
- [type-propagation-results.md](formats/type-propagation-results.md) — TD5_Actor type propagation results (Ghidra)
- [type-propagation-wave2.md](formats/type-propagation-wave2.md) — type propagation wave 2: TD5_Actor* parameter typing
- [unnamed-globals-resolved.md](formats/unnamed-globals-resolved.md) — formerly-unnamed globals, resolved
- [wrapper-dlls-analysis.md](formats/wrapper-dlls-analysis.md) — original ddraw.dll / winmm.dll wrapper DLLs
- [actor-struct-first-128-bytes.md](subsystems/actor-struct-first-128-bytes.md) — Actor bytes 0x000-0x07F (contact-probe track state)
- [actor-struct-remaining-gaps.md](subsystems/actor-struct-remaining-gaps.md) — TD5_Actor remaining gap analysis
- [game-data-inventory.md](subsystems/game-data-inventory.md) — complete game data inventory (all files on disc)
- [m2dxfx-dll-analysis.md](subsystems/m2dxfx-dll-analysis.md) — M2DXFX.dll = non-3D-accelerated build of M2DX (same API minus DXD3D), NOT an FMV lib
- [m2dxfx-last-three-functions.md](subsystems/m2dxfx-last-three-functions.md) — last three unnamed M2DXFX.dll functions
- [modding_strategy.md](modding_strategy.md) — TD5RE modding strategy draft (2026-06-02, user-confirmed decisions)
- [file-reorganization-plan.md](tickets/file-reorganization-plan.md) — plan to move game data into subfolders

## Physics/AI

- [da_t2_update_actor_track_pos.md](wave4_deep_audits/da_t2_update_actor_track_pos.md) — deep audit UpdateActorTrackPosition 0x004440F0 (extends pilot D1-D7 with D8-D10)
- [da_t2_fix1_case1_fwd_staged.md](wave4_deep_audits/da_t2_fix1_case1_fwd_staged.md) — DA-T2 fix 1 case 1 (FWD) secondary tests, STAGED not applied
- [vehicle-dynamics-complete.md](subsystems/vehicle-dynamics-complete.md) — complete vehicle dynamics & physics pipeline
- [vehicle-dynamics-advanced.md](subsystems/vehicle-dynamics-advanced.md) — suspension cross-coupling, weight transfer, yaw torque, tire model
- [collision-system.md](subsystems/collision-system.md) — collision detection & response system
- [ai-system-deep-dive.md](subsystems/ai-system-deep-dive.md) — AI system deep dive (racer brains)
- [ai-rubber-banding-deep-dive.md](subsystems/ai-rubber-banding-deep-dive.md) — AI rubber-banding / catch-up system
- [traffic-ai-system.md](subsystems/traffic-ai-system.md) — traffic AI complete analysis
- [encounter-scripting-system.md](subsystems/encounter-scripting-system.md) — special encounters + actor track-script VM
- [force-feedback-system.md](subsystems/force-feedback-system.md) — force feedback effect system
- [npc-group-table.md](formats/npc-group-table.md) — gNpcRacerGroupTable NPC racer group table
- [tick-rate-dependent-constants.md](formats/tick-rate-dependent-constants.md) — tick-rate-dependent physics constants
- [physics-validation-testcases.md](tickets/physics-validation-testcases.md) — physics validation test cases

## Track/strip data

- [ai-routing-and-track-geometry.md](subsystems/ai-routing-and-track-geometry.md) — STRIP spans, LEFT/RIGHT.TRK routing, segment walking — deep analysis
- [level-zip-file-formats.md](formats/level-zip-file-formats.md) — level%03d.zip contents and per-file formats (STRIP, TRK, MODELS, ...)
- [levelinf-dat-format.md](formats/levelinf-dat-format.md) — LEVELINF.DAT file format

## Rendering

- [tire_marks_burnout_investigation_handoff_2026-05-29.md](tire_marks_burnout_investigation_handoff_2026-05-29.md) — tire marks / burnout smoke investigation handoff
- [advance_texture_streaming_scheduler_reaudit_2026-05-24.md](advance_texture_streaming_scheduler_reaudit_2026-05-24.md) — AdvanceTextureStreamingScheduler (0x0040b830) re-audit
- [da_t1_clip_and_submit.md](wave4_deep_audits/da_t1_clip_and_submit.md) — deep audit ClipAndSubmitProjectedPolygon (0x004317F0)
- [render-pipeline-and-scene-setup.md](subsystems/render-pipeline-and-scene-setup.md) — render pipeline, scene setup, translucent primitives
- [track-3d-rendering-pipeline.md](subsystems/track-3d-rendering-pipeline.md) — track 3D rendering pipeline (cull window, mesh dispatch)
- [environment-map-system.md](subsystems/environment-map-system.md) — environment map / env-texture system
- [visual-effects-system.md](subsystems/visual-effects-system.md) — VFX systems deep dive (particles, billboards)
- [weather-particle-system.md](subsystems/weather-particle-system.md) — weather / ambient particle system
- [camera-and-math-utilities.md](subsystems/camera-and-math-utilities.md) — camera system + math/transform utilities
- [chase-camera-full-decompilation.md](subsystems/chase-camera-full-decompilation.md) — UpdateChaseCamera (0x401590) full decomp + time-delta scaling plan
- [m2dx-dxdraw-class-deep-dive.md](subsystems/m2dx-dxdraw-class-deep-dive.md) — M2DX DXDraw class deep dive
- [m2dx-resolution-hardcodes.md](subsystems/m2dx-resolution-hardcodes.md) — M2DX.dll resolution hardcodes
- [dxd3d-class-internals.md](formats/dxd3d-class-internals.md) — DXD3D class internals (M2DX.dll D3D layer)
- [translucent-dispatch-table.md](formats/translucent-dispatch-table.md) — translucent dispatch table PTR_LAB_00473b9c
- [hardcoded-resolution-audit.md](formats/hardcoded-resolution-audit.md) — hardcoded 640x480 audit in TD5_d3d.exe
- [surface-viewport-hud-rendering.md](ui/surface-viewport-hud-rendering.md) — surface creation, viewport setup, HUD & font rendering
- [4player-splitscreen-plan.md](tickets/4player-splitscreen-plan.md) — 4-player split-screen implementation plan
- [widescreen-640x480-audit.md](tickets/widescreen-640x480-audit.md) — widescreen hardcoded-constants audit
- [widescreen-patch-guide.md](tickets/widescreen-patch-guide.md) — widescreen patch guide (M2DX.dll + TD5_d3d.exe)

## Frontend/HUD

- [frontend_fade_sfx_universal_s03.md](frontend_fade_sfx_universal_s03.md) — transition fade SFX universal coverage (S03, 2026-06-04)
- [frontend_master_fix_plan.md](frontend_master_fix_plan.md) — master faithfulness fix plan (2026-06-01)
- [frontend_diff_blindspot_postmortem.md](frontend_diff_blindspot_postmortem.md) — DIFF blind-spot post-mortem (2026-06-01)
- [frontend_fix_plan.md](frontend_fix_plan.md) — faithfulness master fix plan, first cut (2026-05-31)
- [frontend_re_methodology_postmortem.md](frontend_re_methodology_postmortem.md) — frontend RE methodology post-mortem (2026-05-30)
- [frontend_complete_spec.md](frontend_complete_spec.md) — complete element & behavior spec (corrected-methodology read)
- [frontend_screen_layout_spec.md](frontend_screen_layout_spec.md) — exact element positions from TD5_d3d.exe
- [frontend_behavior_parity_report.md](frontend_behavior_parity_report.md) — button & element behavior parity report
- [frontend_flow_model.md](frontend_flow_model.md) — flow / dispatch / state / persistence behavior model
- [frontend_rendering_model.md](frontend_rendering_model.md) — frontend rendering model (behavior-level RE)
- [frontend_call_graph_closure.md](frontend_call_graph_closure.md) — provably-complete call-graph closure of the frontend subsystem
- [frontend_assumption_register.md](frontend_assumption_register.md) — assumption register for the frontend port
- [frontend_font_atlas_resolution.md](frontend_font_atlas_resolution.md) — font atlas resolution
- [frontend_snk_strings.md](frontend_snk_strings.md) — Language.dll SNK_* frontend label string extraction
- [plan_screen24_race_results_parity.md](plan_screen24_race_results_parity.md) — Screen [24] RaceResults parity plan
- [screens_00.md](frontend_screens/screens_00.md) — per-screen element + behavior spec, screens 0-4
- [screens_05.md](frontend_screens/screens_05.md) — per-screen element + behavior spec, screens 5-9
- [screens_10.md](frontend_screens/screens_10.md) — per-screen element + behavior spec, screens 10-14
- [screens_15.md](frontend_screens/screens_15.md) — per-screen element + behavior spec, screens 15-19
- [screens_20.md](frontend_screens/screens_20.md) — per-screen element + behavior spec, screens 20-24
- [screens_25.md](frontend_screens/screens_25.md) — per-screen element + behavior spec, screens 25-29
- [part_00.md](frontend_layout/part_00.md) — layout harvest, screen-table indices 0-4
- [part_05.md](frontend_layout/part_05.md) — layout harvest, indices 5-9
- [part_10.md](frontend_layout/part_10.md) — layout harvest, indices 10-14
- [part_15.md](frontend_layout/part_15.md) — layout harvest, indices 15-19
- [part_20.md](frontend_layout/part_20.md) — layout harvest, indices 20-24
- [part_25.md](frontend_layout/part_25.md) — layout harvest, indices 25-29
- [behavior_00.md](frontend_layout/behavior_00.md) — behavior + parity harvest, indices 0-4
- [behavior_05.md](frontend_layout/behavior_05.md) — behavior harvest, indices 5-9
- [behavior_10.md](frontend_layout/behavior_10.md) — behavior harvest, indices 10-14
- [behavior_15.md](frontend_layout/behavior_15.md) — behavior harvest, indices 15-19
- [behavior_20.md](frontend_layout/behavior_20.md) — behavior + port-parity harvest, indices 20-24
- [behavior_25.md](frontend_layout/behavior_25.md) — behavior harvest, indices 25-29
- [diff_00.md](frontend_diff/diff_00.md) — PORT-vs-ORIGINAL diff, screens 0-4
- [diff_05.md](frontend_diff/diff_05.md) — PORT-vs-ORIGINAL diff, screens 5-9
- [diff_10.md](frontend_diff/diff_10.md) — PORT-vs-ORIGINAL diff, screens 10-14
- [diff_15.md](frontend_diff/diff_15.md) — PORT-vs-ORIGINAL diff, screens 15-19
- [diff_20.md](frontend_diff/diff_20.md) — PORT-vs-ORIGINAL diff, screens 20-24
- [diff_25.md](frontend_diff/diff_25.md) — PORT-vs-ORIGINAL diff, screens 25-29
- [fix_00.md](frontend_fixlist/fix_00.md) — 3-layer faithfulness fix list, screens 0-4
- [fix_05.md](frontend_fixlist/fix_05.md) — 3-layer faithfulness fix list, screens 5-9
- [fix_10.md](frontend_fixlist/fix_10.md) — 3-layer faithfulness fix list, screens 10-14
- [fix_15.md](frontend_fixlist/fix_15.md) — 3-layer faithfulness fix list, screens 15-19
- [fix_20.md](frontend_fixlist/fix_20.md) — 3-layer faithfulness fix list, screens 20-24
- [fix_25.md](frontend_fixlist/fix_25.md) — 3-layer faithfulness fix list, screens 25-29
- [screen-init-dispatch-table.md](formats/screen-init-dispatch-table.md) — screen init dispatch table at 0x464104
- [language-dll-full-analysis.md](formats/language-dll-full-analysis.md) — Language.dll localization DLL full analysis
- [language-dll-xref-table.md](formats/language-dll-xref-table.md) — Language.dll SNK_ import cross-reference table
- [frontend-screen-table.md](ui/frontend-screen-table.md) — g_frontendScreenFnTable complete 30-entry reference
- [frontend-rendering-internals.md](ui/frontend-rendering-internals.md) — frontend rendering internals deep dive
- [frontend-screens-remaining.md](ui/frontend-screens-remaining.md) — remaining frontend screens deep dive
- [frontend-framerate-timers.md](ui/frontend-framerate-timers.md) — frame-rate-dependent frontend timers/counters (FPS-decoupling reference)
- [car-track-selection-screens.md](ui/car-track-selection-screens.md) — car selection, track selection, race-type menu deep analysis
- [hud-minimap-overlay-system.md](ui/hud-minimap-overlay-system.md) — HUD & minimap overlay system
- [loading-screen-pipeline.md](ui/loading-screen-pipeline.md) — loading screen pipeline
- [text-rendering-pipeline.md](ui/text-rendering-pipeline.md) — text rendering pipeline complete analysis
- [gdi-text-rendering-implementation.md](ui/gdi-text-rendering-implementation.md) — GDI text rendering implementation notes
- [fun-00414f40-analysis.md](subsystems/fun-00414f40-analysis.md) — FUN_00414f40 = RenderPositionerGlyphStrip (glyph strip rendering)
- [da_t3_race_hud_layout.md](wave4_deep_audits/da_t3_race_hud_layout.md) — deep audit InitializeRaceHudLayout (0x00437BA0), ARCH-DIVERGENCE promotion
- [da_t4_race_type_category.md](wave4_deep_audits/da_t4_race_type_category.md) — deep audit RaceTypeCategoryMenuStateMachine (0x004168B0)
- [da_t5_radial_pulse.md](wave4_deep_audits/da_t5_radial_pulse.md) — deep audit RenderHudRadialPulseOverlay (0x00439E60)

## Audio

- [sound-system-deep-dive.md](subsystems/sound-system-deep-dive.md) — sound system deep dive (DXSound, vehicle audio, CD)
- [da_m3_dxsound_polyphony.md](wave4_deep_audits/da_m3_dxsound_polyphony.md) — M2DX DXSound polyphony deep audit
- [ea-tgq-multimedia-engine.md](subsystems/ea-tgq-multimedia-engine.md) — EA TGQ/TGV FMV + audio streaming engine analysis

## Netplay

- [network-multiplayer-protocol.md](subsystems/network-multiplayer-protocol.md) — network multiplayer protocol deep dive (DXPTYPE)
- [multiplayer-lobby-state-machine.md](subsystems/multiplayer-lobby-state-machine.md) — multiplayer lobby state machine complete analysis
- [da_m2_dxplay_lockstep.md](wave4_deep_audits/da_m2_dxplay_lockstep.md) — DXPlay lockstep / pad-replication wire protocol audit

## Save system

- [save-file-formats.md](formats/save-file-formats.md) — Config.td5 / CupData.td5 save file formats
- [cup-progression-save-system.md](subsystems/cup-progression-save-system.md) — cup progression, race-series scheduling, save/restore
- [race-progression-system.md](subsystems/race-progression-system.md) — race progression system (unlocks)
- [replay-recording-system.md](subsystems/replay-recording-system.md) — replay/demo recording & playback system
- [da_m1_replay_codec.md](wave4_deep_audits/da_m1_replay_codec.md) — M2DX replay codec deep audit
- [input-and-configuration.md](subsystems/input-and-configuration.md) — input system, controller binding, configuration deep dive
- [settings-exe-analysis.md](ui/settings-exe-analysis.md) — Settings.exe reverse-engineering analysis

## TD6 migration

- [audit_stage7_junction_remap.md](audit_stage7_junction_remap.md) — Stage 7 peer-cmp polarity + junction-remap no-match sentinel audit (TD6 strip pipeline)
- [td6_track_migration_plan.md](td6_track_migration_plan.md) — TD6 → TD5 track migration infrastructure plan
- [td6_car_perf_normalization.md](td6_car_perf_normalization.md) — TD6 → TD5 car performance normalization (S07)

## Performance/profiling

- [split_screen_perf_diagnostic_2026-06-08.md](split_screen_perf_diagnostic_2026-06-08.md) — split-screen render perf: per-draw Map(WRITE_DISCARD) GPU-serialization stall
- [multithreading_architecture_plan_2026-06-08.md](multithreading_architecture_plan_2026-06-08.md) — multithreading architecture design plan (job pool, parallel decode, render re-entrancy)
- [pageheap_findings_2026-05-20.md](pageheap_findings_2026-05-20.md) — PageHeap audit on TD5_d3d.exe, first finding
- [ttd_mcp_workflow.md](ttd_mcp_workflow.md) — TTD + WinDbg MCP time-travel debugging workflow
- [main-game-loop-decomposition.md](ui/main-game-loop-decomposition.md) — main game loop decomposition (frame loop / FSM / pacing)
- [debug-diagnostics-system.md](subsystems/debug-diagnostics-system.md) — debug & diagnostics system deep dive
- [debug-hook-stubs.md](subsystems/debug-hook-stubs.md) — debug hook stubs in TD5_d3d.exe
- [hidden-debug-features.md](ui/hidden-debug-features.md) — hidden, debug, and cut features catalog

## Audits & confidence maps

Precise-port program (orig-vs-port byte-exactness):

- [cannot_determine_investigation_2026-05-24.md](cannot_determine_investigation_2026-05-24.md) — CANNOT_DETERMINE verdict rows re-investigated
- [intentional_resampling_2026-05-24.md](intentional_resampling_2026-05-24.md) — re-sample of 25/310 INTENTIONAL verdicts for lazy classification
- [not_ported_triage_2026-05-24.md](not_ported_triage_2026-05-24.md) — NOT_PORTED verdict triage
- [oversight_triage_2026-05-24.md](oversight_triage_2026-05-24.md) — OVERSIGHT triage: 10 new bugs
- [orig_vs_port_agent_prompt.md](orig_vs_port_agent_prompt.md) — orig-vs-port verdict agent procedure
- [orig_vs_port_intentional_divergences.md](orig_vs_port_intentional_divergences.md) — pre-seeded intentional divergences for the verdict sweep
- [permanent_l4_residual.md](permanent_l4_residual.md) — permanent L4 residual after Phase 5 (2026-05-21)
- [precise_port_scope.md](precise_port_scope.md) — must-be-byte-exact simulation core scope
- [precise_port_workflow.md](precise_port_workflow.md) — precise-port batch workflow
- [whole_state_diff_design.md](whole_state_diff_design.md) — whole-state per-tick diff harness design (V1)
- [whole_state_diff_first5_inventory.md](whole_state_diff_first5_inventory.md) — first-5-ticks whole-state diff inventory (2026-05-15)
- [fpu_control_word_audit.md](fpu_control_word_audit.md) — FPU control word audit, orig vs port
- [fpu_pc64_fwrapv_ab_measurement_2026-05-20.md](fpu_pc64_fwrapv_ab_measurement_2026-05-20.md) — A/B measurement of FPU PC=64 + -fwrapv on whole-state diff
- [imul_wrapv_audit_2026-05-20.md](imul_wrapv_audit_2026-05-20.md) — IMUL signed-overflow audit of port build flags
- [sar_audit_2026-05-20.md](sar_audit_2026-05-20.md) — SAR vs SHR semantic audit
- [heading_normal_y_writer_audit.md](heading_normal_y_writer_audit.md) — heading_normal.y (actor+0x292) writer audit
- [race-trace-harness.md](tickets/race-trace-harness.md) — port-side race_trace.csv parity harness
- [race-frame-hook-points.md](tickets/race-frame-hook-points.md) — RunRaceFrame hook point map
- [rng-restoration-patch.md](tickets/rng-restoration-patch.md) — RNG restoration patch (FUN_0042C8D0)

Pilot byte-exactness audits (per original function):

- [pilot_00403720_audit.md](pilot_00403720_audit.md) — RefreshVehicleWheelContactFrames (0x00403720)
- [pilot_00403720_runbook.md](pilot_00403720_runbook.md) — RefreshVehicleWheelContactFrames runbook
- [pilot_00403A20_audit.md](pilot_00403A20_audit.md) — IntegrateWheelSuspensionTravel (0x00403A20)
- [pilot_00403D90_audit.md](pilot_00403D90_audit.md) — UpdateVehicleState0fDamping (0x00403D90)
- [pilot_00403D90_runbook.md](pilot_00403D90_runbook.md) — UpdateVehicleState0fDamping runbook
- [pilot_00404030_audit.md](pilot_00404030_audit.md) — UpdatePlayerVehicleDynamics (0x00404030)
- [pilot_00404EC0_audit.md](pilot_00404EC0_audit.md) — UpdateAIVehicleDynamics (0x00404EC0)
- [pilot_004057F0_audit.md](pilot_004057F0_audit.md) — UpdateVehicleSuspensionResponse (0x004057F0)
- [pilot_00405B40_audit.md](pilot_00405B40_audit.md) — ClampVehicleAttitudeLimits (0x00405B40)
- [pilot_00405D70_audit.md](pilot_00405D70_audit.md) — ResetVehicleActorState (0x00405D70)
- [pilot_00405E80_audit.md](pilot_00405E80_audit.md) — IntegrateVehiclePoseAndContacts (0x00405E80)
- [pilot_004063A0_audit.md](pilot_004063A0_audit.md) — UpdateVehiclePoseFromPhysicsState (0x004063A0)
- [pilot_00406650_audit.md](pilot_00406650_audit.md) — UpdateVehicleActor (0x00406650)
- [pilot_00406980_audit.md](pilot_00406980_audit.md) — ApplyTrackSurfaceForceToActor / WallResponse (0x00406980)
- [pilot_00409150_audit.md](pilot_00409150_audit.md) — ResolveVehicleContacts (0x00409150)
- [pilot_0040A720_audit.md](pilot_0040A720_audit.md) — AngleFromVector12 (0x0040A720)
- [pilot_0042EB10_audit.md](pilot_0042EB10_audit.md) — TransformShortVec3ByRenderMatrixRounded (0x0042EB10)
- [pilot_0042EBF0_audit.md](pilot_0042EBF0_audit.md) — ComputeVehicleSurfaceNormalAndGravity (0x0042EBF0)
- [pilot_0042F030_audit.md](pilot_0042F030_audit.md) — ComputeDriveTorqueFromGearCurve (0x0042F030)
- [pilot_00432D60_audit.md](pilot_00432D60_audit.md) — ComputeAIRubberBandThrottle (0x00432D60)
- [pilot_00432E60_audit.md](pilot_00432E60_audit.md) — InitializeRaceActorRuntime (0x00432E60)
- [pilot_004340C0_audit.md](pilot_004340C0_audit.md) — UpdateActorSteeringBias (0x004340C0)
- [pilot_00434350_audit.md](pilot_00434350_audit.md) — InitializeActorTrackPose (0x00434350)
- [pilot_00434FE0_audit.md](pilot_00434FE0_audit.md) — UpdateActorTrackBehavior (0x00434FE0)
- [pilot_00436A70_audit.md](pilot_00436A70_audit.md) — UpdateRaceActors (0x00436A70)
- [pilot_004370A0_audit.md](pilot_004370A0_audit.md) — AdvanceActorTrackScript (0x004370A0)
- [pilot_004370A0_runbook.md](pilot_004370A0_runbook.md) — AdvanceActorTrackScript runbook
- [pilot_004440F0_audit.md](pilot_004440F0_audit.md) — UpdateActorTrackPosition (0x004440F0)
- [pilot_spline_target_audit.md](pilot_spline_target_audit.md) — ComputeSignedTrackOffset (0x00434670) + SampleTrackTargetPoint (0x00434800)
- [pilot_traffic_audit.md](pilot_traffic_audit.md) — ApplyDampedSuspensionForce (0x004437C0) + IntegrateVehicleFrictionForces (0x004438F0)
- [pilot_trig_audit.md](pilot_trig_audit.md) — CosFloat12bit (0x0040A6A0) + SinFloat12bit (0x0040A6C0)
- [pilot_v2v_contact_audit.md](pilot_v2v_contact_audit.md) — V2V contact pair (0x00408570 + 0x00408A60)
- [pilot_v2v_impulse_audit.md](pilot_v2v_impulse_audit.md) — V2V impulse branches (0x004079C0)

Confidence maps & validation packs (audits/, 2026-04):

- [texture-audit-2026-04-14.md](audits/texture-audit-2026-04-14.md) — texture audit follow-up
- [texture-audit-2026-04-13.md](audits/texture-audit-2026-04-13.md) — texture audit + missing-feature TODO
- [v2v-impulse-rewrite-2026-04-11.md](audits/v2v-impulse-rewrite-2026-04-11.md) — V2V impulse rewrite
- [deterministic-validation-suite-2026-04-09.md](audits/deterministic-validation-suite-2026-04-09.md) — deterministic validation suite
- [race-fix-tickets-2026-04-09.md](audits/race-fix-tickets-2026-04-09.md) — race fix tickets
- [race-function-confidence-map-2026-04-09.md](audits/race-function-confidence-map-2026-04-09.md) — race function confidence map
- [race-function-evidence-dossier-2026-04-09.md](audits/race-function-evidence-dossier-2026-04-09.md) — race function evidence dossier
- [race-path-arithmetic-fidelity-audit-2026-04-09.md](audits/race-path-arithmetic-fidelity-audit-2026-04-09.md) — race-path arithmetic fidelity audit
- [race-path-contradiction-ledger-2026-04-09.md](audits/race-path-contradiction-ledger-2026-04-09.md) — race path contradiction ledger
- [race-re-quality-pack-2026-04-09.md](audits/race-re-quality-pack-2026-04-09.md) — race RE quality pack
- [race-ticket-validation-2026-04-09.md](audits/race-ticket-validation-2026-04-09.md) — race ticket validation
- [race-top25-evidence-dossier-2026-04-09.md](audits/race-top25-evidence-dossier-2026-04-09.md) — race top-25 evidence dossier
- [race-validation-pack-2026-04-09.md](audits/race-validation-pack-2026-04-09.md) — race validation pack
- [re-sync-audit-workflow-2026-04-09.md](audits/re-sync-audit-workflow-2026-04-09.md) — RE sync audit workflow
- [runtime-struct-global-ownership-pass-2026-04-09.md](audits/runtime-struct-global-ownership-pass-2026-04-09.md) — runtime struct & global ownership pass
- [source-port-known-deviations-2026-04-09.md](audits/source-port-known-deviations-2026-04-09.md) — source port known deviations
- [function-mapping-confidence-audit-2026-04-08.md](audits/function-mapping-confidence-audit-2026-04-08.md) — function mapping confidence audit
- [low-confidence-function-pass-2026-04-08.md](audits/low-confidence-function-pass-2026-04-08.md) — low-confidence function pass
- [gap-functions-naming-wave2.md](audits/gap-functions-naming-wave2.md) — gap functions naming wave 2
- [m2dx-bulk-rename-results.md](audits/m2dx-bulk-rename-results.md) — M2DX.dll bulk dead-code rename results

L5 port-quality audits (l5_audit_reports/, 2026-05-21):

- [check_agent_phase5_report.md](l5_audit_reports/check_agent_phase5_report.md) — Phase 5 check + finalization verification
- [check_agent_report.md](l5_audit_reports/check_agent_report.md) — L4→L5 parallel sweep verification
- [agent_5c_frontend.md](l5_audit_reports/agent_5c_frontend.md) — td5_frontend.c L5 audit (Phase 5c)
- [agent_5d_render.md](l5_audit_reports/agent_5d_render.md) — td5_render.c/.h L4→L5 audit (Phase 5d)
- [agent_5e1_cam_vfx.md](l5_audit_reports/agent_5e1_cam_vfx.md) — td5_camera.c + td5_vfx.c L4→L5 audit (Phase 5e)
- [agent_5e2_misc.md](l5_audit_reports/agent_5e2_misc.md) — HUD / asset / input / sound L5 audit (Phase 5e pt2)
- [agent_a_frontend.md](l5_audit_reports/agent_a_frontend.md) — td5_frontend.c L5 audit (Agent A)
- [agent_b_render.md](l5_audit_reports/agent_b_render.md) — render module L4→L5 audit (Agent B)
- [agent_c_visual.md](l5_audit_reports/agent_c_visual.md) — camera / vfx / hud L5 audit (Agent C)
- [agent_d_misc.md](l5_audit_reports/agent_d_misc.md) — misc modules L4→L5 audit (Agent D)

Function-naming audit-comment sync (function_naming/, Wave 2B, 60 addresses per batch):

- [batch_50_wave2b_audit_004011c0_0040ade0.md](function_naming/batch_50_wave2b_audit_004011c0_0040ade0.md) — addresses 0x004011c0-0x0040ade0
- [batch_51_wave2b_audit_0040ae00_00413bc0.md](function_naming/batch_51_wave2b_audit_0040ae00_00413bc0.md) — addresses 0x0040ae00-0x00413bc0
- [batch_52_wave2b_audit_00413c0b_00424110.md](function_naming/batch_52_wave2b_audit_00413c0b_00424110.md) — addresses 0x00413c0b-0x00424110
- [batch_53_wave2b_audit_004241e0_0042e130.md](function_naming/batch_53_wave2b_audit_004241e0_0042e130.md) — addresses 0x004241e0-0x0042e130
- [batch_54_wave2b_audit_0042e2e0_00435f6a.md](function_naming/batch_54_wave2b_audit_0042e2e0_00435f6a.md) — addresses 0x0042e2e0-0x00435f6a
- [batch_55_wave2b_audit_0043646a_00446560.md](function_naming/batch_55_wave2b_audit_0043646a_00446560.md) — addresses 0x0043646a-0x00446560
- [batch_56_wave2b_audit_00446a70_00453c60.md](function_naming/batch_56_wave2b_audit_00446a70_00453c60.md) — addresses 0x00446a70-0x00453c60
- [batch_57_wave2b_audit_00453f70_0045a84c.md](function_naming/batch_57_wave2b_audit_00453f70_0045a84c.md) — addresses 0x00453f70-0x0045a84c
- [batch_58_wave2b_audit_0045a98b_0048d988.md](function_naming/batch_58_wave2b_audit_0045a98b_0048d988.md) — addresses 0x0045a98b-0x0048d988 (28 addresses)

Global-naming sweep (global_naming/, T1-T6 tiers):

- [T1_SUMMARY.md](global_naming/T1_SUMMARY.md) — T1 sweep summary
- [T2_SUMMARY.md](global_naming/T2_SUMMARY.md) — T2 summary (ARCH-DIVERGENCE writers)
- [T3_SUMMARY.md](global_naming/T3_SUMMARY.md) — T3 summary (per-frame orchestrator globals)
- [T4_SUMMARY.md](global_naming/T4_SUMMARY.md) — T4 summary (config + asset loaders)
- [T5_SUMMARY.md](global_naming/T5_SUMMARY.md) — T5 summary (long tail: debug, attract, replay, network)
- [CONSOLIDATION_LOG.md](global_naming/CONSOLIDATION_LOG.md) — consolidation log (2026-05-20)
- [BATCH_TEMPLATE.md](global_naming/BATCH_TEMPLATE.md) — batch template
- [batch_01_transmission_reverse.md](global_naming/batch_01_transmission_reverse.md) — transmission / reverse latch
- [batch_02_wanted_mode.md](global_naming/batch_02_wanted_mode.md) — wanted_mode / police chase
- [batch_03_traffic_init.md](global_naming/batch_03_traffic_init.md) — traffic init & route planning
- [batch_04_checkpoint_config.md](global_naming/batch_04_checkpoint_config.md) — checkpoint config (timer + circuit gate)
- [batch_05_replay_and_results.md](global_naming/batch_05_replay_and_results.md) — replay save/playback + race-results FSM
- [batch_06_main_game_loop.md](global_naming/batch_06_main_game_loop.md) — main game loop + race init
- [batch_07_frontend_orchestrators.md](global_naming/batch_07_frontend_orchestrators.md) — frontend orchestrators (loop / FMV / legal screens)
- [batch_08_render_hud_wheels.md](global_naming/batch_08_render_hud_wheels.md) — HUD overlays + wheel billboards + minimap
- [batch_09_track_lighting.md](global_naming/batch_09_track_lighting.md) — track lighting family
- [batch_10_small_divergences.md](global_naming/batch_10_small_divergences.md) — small AI/track divergences catch-all
- [batch_11_per_frame_state.md](global_naming/batch_11_per_frame_state.md) — per-frame top-level state
- [batch_12_camera_view.md](global_naming/batch_12_camera_view.md) — camera & view state
- [batch_13_sound_audio.md](global_naming/batch_13_sound_audio.md) — sound / audio orchestrator state
- [batch_14_input_controller.md](global_naming/batch_14_input_controller.md) — input / controller
- [batch_15_vfx_render_frame.md](global_naming/batch_15_vfx_render_frame.md) — VFX + render frame setup
- [batch_16_config_save_state.md](global_naming/batch_16_config_save_state.md) — Config.td5 / CupData.td5 / cheat state / INI mirrors
- [batch_17_asset_loaders.md](global_naming/batch_17_asset_loaders.md) — asset loaders (ZIP, TGA, mesh prepare)
- [batch_18_track_parsers.md](global_naming/batch_18_track_parsers.md) — track data parsers (STRIP / LEVELINF / LIGHT / MODELS)
- [batch_19_carparam_ai_tuning.md](global_naming/batch_19_carparam_ai_tuning.md) — carparam + AI tuning tables
- [batch_20_audio_loaders.md](global_naming/batch_20_audio_loaders.md) — audio asset loaders (DXSound pool, per-vehicle banks)
- [batch_21_debug_overlays.md](global_naming/batch_21_debug_overlays.md) — debug overlays + diagnostics
- [batch_22_attract_mode.md](global_naming/batch_22_attract_mode.md) — attract mode / intro / demo
- [batch_23_replay_record_telemetry.md](global_naming/batch_23_replay_record_telemetry.md) — replay record + results slot table + telemetry
- [batch_24_network_multiplayer.md](global_naming/batch_24_network_multiplayer.md) — network multiplayer (DXPlay session/lockstep/lobby)
- [batch_25_long_tail_misc.md](global_naming/batch_25_long_tail_misc.md) — long-tail miscellany
- [batch_26_t5_stragglers.md](global_naming/batch_26_t5_stragglers.md) — T5 stragglers (TGA config, weather, lighting/FF false alarms)
- [batch_27_conflict_triage.md](global_naming/batch_27_conflict_triage.md) — T1-T5 cross-batch name reconciliation
- [batch_28_t6_frontend_screens.md](global_naming/batch_28_t6_frontend_screens.md) — frontend screen FSMs
- [batch_29_t6_render_pipeline.md](global_naming/batch_29_t6_render_pipeline.md) — render/clipper/translucent pipeline
- [batch_30_t6_particle_pool.md](global_naming/batch_30_t6_particle_pool.md) — particle pool state
- [batch_31_t6_track_lut_inflate.md](global_naming/batch_31_t6_track_lut_inflate.md) — track-contact LUTs, inflate tables, actor track-script
- [batch_32_t6_traffic_camera_audio.md](global_naming/batch_32_t6_traffic_camera_audio.md) — traffic queue, trackside camera, vehicle audio
- [batch_33_t6_save_state_snapshot.md](global_naming/batch_33_t6_save_state_snapshot.md) — race-status snapshot + packed-config buffer
- [batch_34_t6_frontend_display_modes.md](global_naming/batch_34_t6_frontend_display_modes.md) — frontend display-mode list + selection
- [batch_35_t6_texture_mesh_camera.md](global_naming/batch_35_t6_texture_mesh_camera.md) — texture cache, mesh transform constants, camera basis tail
- [batch_36_t6_audio_codec_chat_misc.md](global_naming/batch_36_t6_audio_codec_chat_misc.md) — ADPCM tables, chat tokenizer, AI rubber-band, misc
- [batch_37_t6_math_audio_fmv_final.md](global_naming/batch_37_t6_math_audio_fmv_final.md) — sin/cos LUT, font glyphs, ADPCM state, FMV, inline strings
- [batch_38_t6_arch_collapsed_exclusions.md](global_naming/batch_38_t6_arch_collapsed_exclusions.md) — ARCH-COLLAPSED CRT/vendor exclusions
- [batch_39_p6_struct_globals_reconcile.md](global_naming/batch_39_p6_struct_globals_reconcile.md) — P6 struct/global/format reconciliation

Coverage / recovery sweeps:

- [call-graph-analysis.md](subsystems/call-graph-analysis.md) — TD5_d3d.exe call graph analysis
- [gap-region-function-recovery.md](subsystems/gap-region-function-recovery.md) — gap-region function recovery
- [m2dx-low-range-investigation.md](subsystems/m2dx-low-range-investigation.md) — M2DX.dll low address range investigation
- [remaining-systems.md](subsystems/remaining-systems.md) — remaining undocumented systems deep dive
- [td5mod-framework-analysis.md](subsystems/td5mod-framework-analysis.md) — review of the td5mod/ source-port framework modules

## Session follow-ups

- [README.md](followup_sessions/README.md) — eight self-contained follow-up session briefs (2026-05-22), index
- [session_01_ai_stuck_span_137.md](followup_sessions/session_01_ai_stuck_span_137.md) — AI stuck at Edinburgh span 137-147
- [session_02_rotation_matrix_fpu.md](followup_sessions/session_02_rotation_matrix_fpu.md) — FPU-faithful rotation matrix rebuild (root of countdown cascade)
- [session_03_rand_call_sequence.md](followup_sessions/session_03_rand_call_sequence.md) — InitializeRaceSeriesSchedule rand() call-sequence alignment
- [session_04_visual_capture.md](followup_sessions/session_04_visual_capture.md) — visual side-by-side capture (floating cars + HUD transparency)
- [session_05_suspension_fr_asymmetry.md](followup_sessions/session_05_suspension_fr_asymmetry.md) — F-R suspension asymmetry (car tilted right)
- [session_06_invisible_walls.md](followup_sessions/session_06_invisible_walls.md) — Edinburgh span 350 + Newcastle span 216 invisible walls
- [session_07_wall_recovery_exit.md](followup_sessions/session_07_wall_recovery_exit.md) — AI wall-collision recovery exit conditions
- [session_08_deferred_triage.md](followup_sessions/session_08_deferred_triage.md) — triage of older deferred items
- [wave1_a_quickwin_audits.md](followup_sessions/wave1_a_quickwin_audits.md) — Wave 1 Agent A: quick-win audits
- [wave1_b_dispatch_tables.md](followup_sessions/wave1_b_dispatch_tables.md) — Wave 1 Agent B: dispatch table inventory
- [wave1_c_switch_orphans.md](followup_sessions/wave1_c_switch_orphans.md) — Wave 1 Agent C: switch-table orphan recovery
- [wave1_d_m2dx_inventory.md](followup_sessions/wave1_d_m2dx_inventory.md) — Wave 1 Agent D: M2DX.dll inventory
- [wave1_e_data_segment_map.md](followup_sessions/wave1_e_data_segment_map.md) — Wave 1 Agent E: .data segment map
- [wave1_f_overview.md](followup_sessions/wave1_f_overview.md) — Wave 1 Agent F: wave-2 worklist outputs
- [wave3_lang_m2dxfx_inventory.md](followup_sessions/wave3_lang_m2dxfx_inventory.md) — Wave 3: Language.dll + M2DXFX.dll inventory
- [wave3_locals_correlator_report.md](followup_sessions/wave3_locals_correlator_report.md) — Wave 3 locals correlator report
- [wave3_m2dx_plan_summary.md](followup_sessions/wave3_m2dx_plan_summary.md) — Wave 3 Agent A: M2DX.dll plate-comment apply plan
- [_index.md](followup_sessions/wave3_structs/_index.md) — Tier1-B struct recovery index (2026-05-22)

## Archive/obsolete

- [ENGINE_DOSSIER.md](archive/ENGINE_DOSSIER.md) — early engine dossier
- [KNOWLEDGE_BASE.md](archive/KNOWLEDGE_BASE.md) — early knowledge base
- [format-notes.md](archive/format-notes.md) — early format notes
- [in-race-d3d-trace.md](archive/in-race-d3d-trace.md) — in-race D3D trace
- [m2dx-widescreen-test.md](archive/m2dx-widescreen-test.md) — M2DX widescreen test
- [modding-workflow.md](archive/modding-workflow.md) — early modding workflow
- [race-input-test.md](archive/race-input-test.md) — race/input test
- [runtime-hooking-notes.md](archive/runtime-hooking-notes.md) — runtime hooking notes
- [td5-active-world-matrix-test.md](archive/td5-active-world-matrix-test.md) — active world matrix test (004BF6B8)
- [td5-exe-widescreen-test.md](archive/td5-exe-widescreen-test.md) — exe widescreen test
- [td5-frustum-matrix-test.md](archive/td5-frustum-matrix-test.md) — frustum matrix test
- [td5-frustum-test.md](archive/td5-frustum-test.md) — frustum/camera basis test
- [td5-horizontal-projection-test.md](archive/td5-horizontal-projection-test.md) — horizontal projection test
- [td5-scale-globals-test.md](archive/td5-scale-globals-test.md) — camera/projection scale globals test
- [td5-view-table-test.md](archive/td5-view-table-test.md) — view mode table test (0x004AAE10)
- [true-widescreen-status.md](archive/true-widescreen-status.md) — true widescreen status
- [viewport-override-test.md](archive/viewport-override-test.md) — viewport and projection override test
- [widescreen-re-candidates.md](archive/widescreen-re-candidates.md) — widescreen RE candidates
- [windows11-baseline.md](archive/windows11-baseline.md) — Windows 11 baseline behavior of original game
