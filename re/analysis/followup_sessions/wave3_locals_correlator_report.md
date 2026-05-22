# Wave 3 Locals Correlator — Report

**Date:** 2026-05-22
**Source CSV:** `re/analysis/followup_sessions/wave1_f_local_naming_targets.csv` (199 rows, 42 functions)
**Output CSV:** `re/analysis/followup_sessions/wave3_locals_correlator.csv` (199 rows + header, all original columns + `ghidra_local_name`, `match_reason`, `match_kind`)
**Pool slot used:** TD5_pool0 (read-only). Cleanup confirmed at end.

## Summary

- **Matched:** 11 / 199 (5.5%) — all HIGH confidence
- **Unmatched:** 188 / 199 (94.5%)
- **Functions with any match:** 4 / 42 (0x004057F0, 0x00435940, 0x00404030, 0x00401590)

## Why Wave 2G failed (root cause)

Wave 2G assumed each port-suggested rename targets a Ghidra **stack local**. This is wrong for ~95% of the CSV rows. Three structural reasons:

1. **Port readability variables don't exist in Ghidra.** The port introduces named locals like `racer_count`, `traffic_max`, `front_weight`, `total_weight` as readable wrappers around inline expressions. The original assembly computes the same value but holds it in a CPU register (Ghidra `iVar4`, `sVar3`, etc.), **not** a stack-allocated `local_XX` slot. There is no target for the rename.

2. **CSV port_line offsets are stale.** Many rows reference line numbers that no longer point to the suggested name (file shifted since CSV was generated). My script re-resolves to the nearest matching declaration before reporting.

3. **CSV `orig_address` is sometimes wrong.** Example: rows tagged `0x00405d70` (ResetVehicleActorState) actually describe locals in `td5_ai_update_special_encounter` (port mirror of `0x00434DA0`). The original function `0x00405d70` has zero locals — nothing to rename.

## High-confidence matches (11 rows, ready to apply)

| orig_address | suggested_name | ghidra_local | match_reason |
|---|---|---|---|
| `0x004057F0` | `loni_grav` | `local_50` | comment-mapping (port `/* local_50 — ... */`) |
| `0x004057F0` | `lat_grav` | `local_5c` | comment-mapping |
| `0x004057F0` | `loni_spr` | `local_64` | comment-mapping |
| `0x004057F0` | `lat_spr` | `local_60` | comment-mapping |
| `0x00435940` | `local_18` | `local_18` | literal-name (port keeps Ghidra name verbatim) |
| `0x00435940` | `local_c` | `local_c` | literal-name |
| `0x00404030` | `surface_center` | `local_88` | semantic match: `local_88 = (uchar)iVar6; actor->surface_type_chassis = local_88;` |
| `0x00401590` | `current_radius` | `local_10` | semantic match: `local_10 = *(float *)(&g_cameraSpringRadiusCurrent + iVar8);` |
| `0x00401590` | `orbit_visual_angle` | `local_18` | semantic match: `local_18 = (short *)(g_cameraYawOffsetView[v] - ...);` |
| `0x00401590` | `terrain_matrix` | `local_9c` | semantic match: `float local_9c[12]` (declared as array) |
| `0x00401590` | `cam_angles` | `local_14` | semantic match: `BuildRotationMatrixFromAngles(local_9c, &local_14)` (3-short cluster local_14/12/10) |

## What kinds of mappings worked vs failed

### Worked (HIGH confidence)
- **Literal Ghidra-name port locals** — port keeps a `local_XX` name (e.g. `local_18`, `local_c`). Trivial rename.
- **Inline `/* local_XX */` comments** — port author already annotated the mapping in a comment adjacent to the declaration.
- **Mapping-block comments** — port has a `Ghidra name mapping:` line listing `local_X = name1, local_Y = name2`.
- **Singleton semantic matches** — when one Ghidra `local_XX` and one port-named local share an unambiguous defining assignment (same struct field write, same constant initializer pattern), I matched by direct decomp inspection.

### Failed
- **Port-only readability locals** — port introduces a name for a value that Ghidra keeps register-only. Examples: `racer_count` (port int, Ghidra register `iVar5`), `front_weight` / `total_weight` (port int32, Ghidra registers), `surface_wheel[4]` (port array, Ghidra holds 4 separate `iVar`s).
- **Port refactors split helpers** — e.g. `RunRaceFrame` (0x0042B580) becomes 6+ port helpers; locals don't map 1:1.
- **Port-only state vars** — `resolved_surface_valid`, `t2_old_disp_roll` save snapshots port introduces for diagnostics that aren't present in orig.
- **Stale CSV addresses** — e.g. rows for 0x00405d70 describe code that lives in 0x00434DA0.

## Per-function match counts

| Address | Total rows | Matched | Notes |
|---|---|---|---|
| 0x004057F0 | 6 | 4 | suspension response — comment-mapping block covered loni_/lat_ pairs. `lock`/`prev_air` are register-byte vars (`bVar1`/`bVar2`). |
| 0x00435940 | 6 | 2 | calibration example. Two literal `local_XX` matches; other 4 (`racer_count`, `racer_cap`, `queue_span`, `queue_byte2`) are port-only readability names. |
| 0x00404030 | 6 | 1 | UpdatePlayerVehicleDynamics. Only `surface_center` → `local_88` is a clean Ghidra-stack-local target. `grip[4]` would correspond to 4 separate Ghidra locals (`local_5c, local_58, local_10, local_3c`) — a 1:N mapping that the rename infrastructure can't express; flagged unmatched. |
| 0x00401590 | 6 | 4 | UpdateChaseCamera. `current_radius`, `orbit_visual_angle`, `terrain_matrix`, `cam_angles` all match. `fly_in_threshold`, `fly_in` are register-only. |
| All others (38 funcs) | 175 | 0 | port-only locals or register-only Ghidra vars or stale CSV references. |

## Recommendations for Wave 3

1. **Apply the 11 high-confidence matches immediately.** These are ready for `variable_rename` calls and should land cleanly.

2. **Pivot Wave 2G's failed 188 rows: do NOT attempt to rename as Ghidra stack locals.** Instead, treat them as **port-source-only documentation** — the port has already named them for human readability. The Ghidra side may benefit from EOL comments on the *callsite line* (e.g., comment `actor->surface_type_chassis = local_88;` with `// surface_center (port td5_physics.c:1275)`) but not local renames.

3. **For functions where Wave 2G truly wants register-vars renamed** (e.g., the 4 grip slots in 0x00404030), use targeted per-function manual sessions. The semantic mapping requires reading both decomps side-by-side; a CSV-driven batch tool is the wrong shape.

4. **CSV-format upgrade for any future passes:** add columns `ghidra_local_name` (mandatory before apply) and `ghidra_target_kind` (one of `stack_local | register_var | port_only | n_to_1`). This forces the analyst to commit to a concrete Ghidra-side target during CSV authoring, not at apply time.

## Cleanup confirmation

- Ghidra session `6164fb09b5b44864b665ee2fe12eabae` opened read-only on TD5_pool0
- No writes were made to any program
- Pool slot will be released by `ghidra_pool.sh cleanup`

## Top 10 high-confidence rename batches ready for apply

(All 11 matches fit in a single batch — listed above.)

```
# Ghidra rename calls (read-write Ghidra session required — DO NOT use pool slot 0; reopen with read_only=false in dedicated apply session)
variable_rename(function_start=0x004057F0, name="local_50", new_name="loni_grav")
variable_rename(function_start=0x004057F0, name="local_5c", new_name="lat_grav")
variable_rename(function_start=0x004057F0, name="local_64", new_name="loni_spr")
variable_rename(function_start=0x004057F0, name="local_60", new_name="lat_spr")
variable_rename(function_start=0x00435940, name="local_18", new_name="slot_counter")
variable_rename(function_start=0x00435940, name="local_c",  new_name="per_slot_offset")
variable_rename(function_start=0x00404030, name="local_88", new_name="surface_center")
variable_rename(function_start=0x00401590, name="local_10", new_name="current_radius")
variable_rename(function_start=0x00401590, name="local_18", new_name="orbit_visual_angle")
variable_rename(function_start=0x00401590, name="local_9c", new_name="terrain_matrix")
variable_rename(function_start=0x00401590, name="local_14", new_name="cam_angles")
```

Note: `0x00435940` already has these renamed in TD5_pool0 (Wave 2G's 2 successful applies); verify before re-applying.
