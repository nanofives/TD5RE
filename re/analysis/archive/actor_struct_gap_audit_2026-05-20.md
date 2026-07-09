# TD5_Actor Struct — 18-Gap Audit (2026-05-20)

**Method:** Cross-reference of byte-faithful port `td5mod/src/td5re/*.c` (which mirrors every offset the orig touches at the same offset) with 30+ pilot audit docs under `re/analysis/pilot_*.md` and the 587-name consolidated globals header. Ghidra pool slot `TD5_pool0` was acquired read-only and cleaned up; pyghidra-mcp was not available in the agent's deferred-tool set, so live xref scans against the orig DB were not performed — the byte-faithful port and pilot corpus served as the evidence base.

## Headline structural corrections applied to header

| # | Was | Is now | Size | Reason |
|---|---|---|---|---|
| 1 | gap_088 (4B) | `track_contact_vertex_A` (0x088, int16) + `track_contact_vertex_B` (0x08A, int16) | 4B | Block 0x080-0x08F mirrors `TD5_TrackProbeState` layout; written by `ComputeActorHeadingFromTrackSegment` @ 0x00445B90. |
| 2 | gap_08D (3B) | `_pad_main_probe_0D` (1B) + `_pad_main_probe_0E` (int16) | 3B | TrackProbeState trailing-pad pattern. |
| 3 | gap_174 (4B) + gap_178 (8B) | `saved_render_pos` (TD5_Vec3_Float) | 12B | Translation tail of the 48-byte saved-orientation transform. `ClampVehicleAttitudeLimits` @ 0x00405B40 memcpys rotation_matrix → saved_orientation **and** render_pos → saved_render_pos in one 48-byte block. |
| 4 | gap_1A4 (12B) | `collision_spin_translation` (TD5_Vec3_Float) | 12B | Translation tail of the 48-byte collision-spin transform; same 0x00405B40 writer. |
| 5 | gap_1B4 (4B) | `actor_aux_ptr_1B4` (void*) | 4B | INFERRED 4th pointer slot in the 16-byte ptr block at 0x1B0-0x1BF (td5_trace_replay preserves all 16 bytes). Writer unidentified; possible LOD/hi-detail-model alt. |
| 6 | gap_270 (32B) | `wheel_contact_delta` (uint8[32]; logical int16[4][4]) | 32B | Pre-snap per-wheel `(new - old) >> 8` delta. Written by `RefreshVehicleWheelContactFrames` @ 0x00403720; read by `UpdateVehicleSuspensionResponse` @ 0x004057F0 in landing-impulse dot product. **Storage kept as uint8[32]** — promoting to typed struct caused a documented +7-field convergence regression (likely strict-aliasing codegen). |
| 7 | gap_377 (1B) | `light_zone_index` (uint8) | 1B | Cached per-track lighting-zone index. Written by `ApplyTrackLightingForVehicleSegment` @ 0x00430150 (zone walk based on track_span_raw vs zone [span_lo, span_hi]). Port mirrors via file-static `s_actor_light_zone[slot]`. |

## Confirmed-padding renames (gap_* → _pad_*)

10 gaps re-verified as having no observed writer/reader after the 587-name consolidation and 30+ pilot audits:

| Offset | Size | New name | Position |
|---|---|---|---|
| 0x1D8 | 24 | `_pad_1D8` | between linear_velocity_z and euler_accum |
| 0x20E | 2 | `_pad_20E` | between display_angles.pitch and wheel_display_angles |
| 0x296 | 2 | `_pad_296` | between heading_normal.z and wheel_world_positions_hires |
| 0x2D4 | 4 | `_pad_2D4` | between center_suspension_vel and prev_frame_y_position |
| 0x346 | 2 | `_pad_346` | between pending_finish_timer and timing_frame_counter |
| 0x348 | 4 | `_pad_348` | before timing_frame_counter |
| 0x362 | 9 | `_pad_362` | between airborne_frame_counter and current_gear (subsystem docs speculated 0x366/0x367 but not corroborated) |
| 0x37A | 1 | `_pad_37A` | between vehicle_mode and track_contact_flag |
| 0x37F | 1 | `_pad_37F` | between ghost_flag and grip_reduction |
| 0x382 | 1 | `_pad_382` | between prev_race_position and race_position |

## Net result

- **7 previously-unnamed fields recovered** as real struct members
- **10 gaps confirmed as padding** (renamed to `_pad_*` with `[CONFIRMED 2026-05-20]` rationale)
- **1 INFERRED** (`actor_aux_ptr_1B4` — kept as `void *` placeholder pending writer identification)
- **Zero gap_* fields remain** in `re/include/td5_actor_struct.h`
- 6 new `_Static_assert` lines added for the named-field offsets
- Build verified: `td5re.exe` 1841961 bytes, no struct-related errors

## Follow-up opportunities

1. **gap_270 / wheel_contact_delta strict-aliasing regression** — the convergence-regression note from the older audit means a future precise-port pass should investigate whether `-fno-strict-aliasing` (or per-file `__attribute__((may_alias))`) lets us promote to `int16_t[4][4]` storage without losing byte-exactness.
2. **actor_aux_ptr_1B4 writer** — needs live pyghidra-mcp xref against the orig DB; was not accessible to the audit agent. Until identified, port code will simply not write the field (matches current behavior).
3. **gap_362 (9B)** — two prior subsystem docs (`ai-routing-and-track-geometry.md:533`, `remaining-systems.md:244`) speculated meanings for 0x366/0x367 but neither was corroborated. If those docs are revisited, this region may yield 1-2 more named fields.

## Session P6 reconciliation (2026-06-01)

RE-doc + Ghidra-naming hygiene pass; **no port code change**. Ghidra xrefs re-verified
read-only against the master `TD5` project. Three of this audit's open items are now closed:

- **`wheel_contact_normals` (+0x230) — RESOLVED.** Writer identified:
  `ConvertFloatVec3ToShortAngles` @ 0x0042E2E0, called in the 4-wheel loop of
  `IntegrateVehiclePoseAndContacts` @ 0x00405E80 (`LEA [ESI+0x230]` @ 0x0040621D feeds its
  vec arg; all 3 shorts written via `__ftol`, pointer advances +8/iter). The port already
  writes all 3 components at `td5_physics.c:6964-6967`, consumed by suspension response.
  Header field demoted from `[INFERRED]` → `[CONFIRMED 2026-06-01]`.

- **`actor_aux_ptr_1B4` (+0x1B4) — DEAD-VESTIGIAL (closes Follow-up #2).** A binary-wide
  scan for `[reg+0x1b4]` yields ONLY `[ESP+0x1b4]` stack hits (plus an unrelated immediate
  `0x1b4e81b5` @ 0x004059FB) — there is NO actor-relative reader or writer. The field is
  only ever touched by `InitializeRaceActor` @ 0x0042F140's block-zero on init. It is
  confirmed unwritten/unread and kept as a zeroed pad/placeholder. The "writer not yet
  identified — possible LOD/hi-detail-model alt" note is withdrawn: there is no writer to
  find. **Do NOT implement this field** — adding a writer would be invention, not fidelity.

- **`_pad_362` (+0x362, 9B) — DEAD-VESTIGIAL (closes Follow-up #3).** A binary-wide scan
  finds ZERO `+0x362` displacement accesses. Confirmed unwritten/unread, kept as a zeroed
  pad. The +0x366/+0x367 subsystem speculation remains uncorroborated and is dropped.

Header comments in `re/include/td5_actor_struct.h` updated to match (comment-only; fields,
offsets, and all `_Static_assert`s unchanged).

## Key file references

- `re/include/td5_actor_struct.h` — header (now zero gap_* fields)
- `td5mod/src/td5re/td5_track.c:3038-3078, 3880-3947` — contact_vertex writer
- `td5mod/src/td5re/td5_physics.c:4685-4810, 7020-7085, 7400-7440` — wheel_contact_delta + 48-byte transform memcpys
- `td5mod/src/td5re/td5_render.c:357-361, 3258, 3302` — light_zone_index
- `td5mod/src/td5re/td5_trace_replay.c:274-291` — 1B0-1BF 4-pointer block evidence
- `re/analysis/pilot_00405B40_audit.md:215-230` — saved_orientation + collision_spin 48-byte range evidence
- `re/analysis/pilot_00403720_audit.md:30-33, 71-83` — gap_270 = wheel_contact_delta evidence
- `re/analysis/heading_normal_y_writer_audit.md` — 0x290-0x295 heading_normal layout
