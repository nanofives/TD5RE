# NOT_PORTED triage — 2026-05-24

Triage of the 375 `NOT_PORTED` rows from `re/analysis/orig_vs_port_verdict_2026-05-24.csv`.

## Summary

| Bucket | Count | Action |
|---|---:|---|
| MINGW_CRT | 239 | Skip (MinGW provides) |
| FMV_STUB | 101 | Skip (EA TGQ codec stubbed) |
| D3D11 wrapper (infra) | 5 | Skip (`ddraw_wrapper/` covers) |
| DEAD_CODE / DEAD_WRITE | 5 | Skip (dead in orig too) |
| STDLIB_INLINE | 2 + 4 | Skip (compiler libc inlines) |
| Already inline-folded | 1 | Skip (`MarkMastersRaceSlotCompleted`) |
| Misc skip (M2DX_AUDIO, generic ARCH-DIVERGENCE, etc.) | 8 | Skip |
| **Tier 1 — cop chase visuals** | **3** | **Port** |
| **Tier 2 — recovery animation** | **5** | **Port** |
| **Tier 3 — smoke completion** | **2** | **Port** |
| **Tier 4 — polish/edge** | **5** | **Port** |
| **Tier 5 — benchmark report parity** | **4** | **Port (optional)** |
| Re-audit (`AdvanceTextureStreamingScheduler`) | 1 | Verdict pending |
| **Total addressable** | **20** | |

## Tier 1 — Cop chase visuals (3 funcs, 2667B → td5_render.c + td5_vfx.c + td5_hud.c)

| Addr | Name | Size | Role |
|---|---|---:|---|
| `0x0043c9e0` | `InitializeTrackedActorMarkerBillboards` | 985 | Builds 6 blue/red flashing-light billboards from `PoliceLt_red/blue` archive entries |
| `0x0043cde0` | `RenderTrackedActorMarker` | 1262 | Per-frame render of those markers |
| `0x0043d4e0` | `UpdateWantedDamageIndicator` | 420 | Cop-chase damage HUD overlay |

Closes the visual side of `todo-police-chase-no-audio-2026-05-19` (audio side = separate `wanted_mode_enabled` write-site bug).

## Tier 2 — Vehicle recovery animation (5 funcs, 2743B → td5_physics.c)

| Addr | Name | Size | Role |
|---|---|---:|---|
| `0x004092d0` | `RenderVehicleActorModel` | 587 | Render path for recovery-mode actor |
| `0x00409520` | `CheckAndUpdateActorCollisionAlignment` | 392 | Collision alignment during recovery |
| `0x004096b0` | `ComputeActorWorldBoundingVolume` | 1339 | World AABB computation |
| `0x00409bf0` | `RefreshScriptedVehicleTransforms` | 179 | `vehicle_mode==1` scripted recovery |
| `0x0042e030` | `ExtractEulerAnglesFromMatrix` | 246 | Gimbal-lock Euler decomp (only caller is `RefreshScriptedVehicleTransforms`) |

Currently after a bad crash there's no scripted respawn — porting these = recovery flow works.

## Tier 3 — Smoke completion (2 funcs, 847B → td5_vfx.c)

| Addr | Name | Size | Role |
|---|---|---:|---|
| `0x00429fd0` | `SpawnVehicleSmokePuffAtPoint` | 688 | Low-level point-spawn helper |
| `0x00401370` | `SpawnRandomVehicleSmokePuff` | 159 | Engine-rev gated random spawner |

Closes part of `todo-smoke-render-broken-2026-05-19`.

## Tier 4 — Polish/edge (5 funcs, 836B → mixed modules)

| Addr | Name | Size | Target file |
|---|---|---:|---|
| `0x004036b0` | `UpdatePlayerSteeringWeightBalance` | 101 | `td5_input.c` (2P split-screen) |
| `0x0040d6a0` | `LoadExtrasBandGalleryImages` | 163 | `td5_asset.c` |
| `0x0040d640` | `ReleaseExtrasGalleryImageSurfaces` | 90 | `td5_asset.c` |
| `0x00412e30` | `CreateMenuStringLabelSurface` | 472 | `td5_frontend.c` |
| `0x0043a210` | `ResetHudRadialPulseOverlay` | 10 | `td5_hud.c` |

## Tier 5 — Benchmark report parity (4 funcs, ~2000B → new td5_benchmark.c or td5_game.c)

| Addr | Name | Size | Role |
|---|---|---:|---|
| `0x00428d20` | `InitializeBenchmarkFrameRateCapture` | 28 | Benchmark mode init |
| `0x00428d40` | `RecordBenchmarkFrameRateSample` | 29 | Per-frame sample writer |
| `0x00428d60` | `FormatBenchmarkReportText` | 26 | Report text formatter |
| `0x00428d80` | `WriteBenchmarkResultsTgaReport` | 1921 | Writes orig's benchmark TGA report file |

Only port if benchmark report parity with orig matters (e.g., comparing scores against original game).

## Re-audit (1)

| Addr | Name | Size | Why re-look |
|---|---|---:|---|
| `0x0040b830` | `AdvanceTextureStreamingScheduler` | 441 | Classified D3D11_BACKEND but size (441B) is more than other wrapper-replaced rows; texture-streaming *behavior* (when textures load/unload under VRAM pressure) may differ between orig and port. Worth a manual Ghidra read before committing to skip. |

## Sequencing (chosen)

1. **Tier 2** first (most self-contained — one file)
2. **Tier 1** (cop chase, multiple files but logically cohesive)
3. **Tier 3** (smoke, may touch vfx structs added in Tier 1)
4. **Tier 4** (polish, may touch hud added in Tier 1)
5. **Tier 5** (benchmark, can be a new file)
6. **Re-audit** the streaming scheduler in parallel with Tier 5

Each tier ends with a clean MinGW build before the next starts.
