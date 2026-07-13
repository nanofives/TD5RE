/**
 * td5_track.h -- Track geometry, segment contacts, strip data
 *
 * Original functions:
 *   0x444070  BindTrackStripRuntimePointers
 *   0x42FB90  LoadTrackRuntimeData
 *   0x42FAD0  InitializeTrackStripMetadata
 *   0x42FB40  ApplyTrackStripAttributeOverrides
 *   0x4440F0  UpdateActorTrackPosition
 *   0x430150  ApplyTrackLightingForVehicleSegment
 *   0x42CE90  UpdateActiveTrackLightDirections
 *   0x431260  GetTrackSpanDisplayListEntry
 *   0x431190  ParseModelsDat
 *   0x40AC00  PrepareMeshResource
 *   0x434FE0  UpdateActorTrackBehavior (AI path-following)
 *   0x436A70  UpdateRaceActors
 *   0x435930  InitializeTrafficActorsFromQueue
 *   0x4353B0  RecycleTrafficActorFromQueue (canonical port in td5_ai.c)
 */

#ifndef TD5_TRACK_H
#define TD5_TRACK_H

#include "td5_types.h"

/* Forward declaration for per-probe track update */
struct TD5_TrackProbeState;

/* ========================================================================
 * Track Light Entry (36 bytes, 0x24 stride)
 *
 * Hardcoded per-track in EXE, pointed to by lighting table pointer.
 * Used by ApplyTrackLightingForVehicleSegment.
 * ======================================================================== */

typedef struct TD5_TrackLightEntry {
    int16_t  span_start;        /* +0x00 */
    int16_t  span_end;          /* +0x02 */
    int16_t  light_dir_x;       /* +0x04 */
    int16_t  light_dir_y;       /* +0x06 */
    int16_t  light_dir_z;       /* +0x08 */
    uint8_t  light_color_r;     /* +0x0A */
    uint8_t  light_color_g;     /* +0x0B */
    uint8_t  light_color_b;     /* +0x0C */
    uint8_t  ambient_r;         /* +0x0D */
    uint8_t  ambient_g;         /* +0x0E */
    uint8_t  ambient_b;         /* +0x0F */
    uint32_t fog_color_override; /* +0x10 */
    uint8_t  reserved_14[4];    /* +0x14 */
    uint8_t  light_mode;        /* +0x18: 0=instant, 1=interpolate, 2=multi-dir */
    uint8_t  blend_distance;    /* +0x19 */
    uint8_t  reserved_1A[2];    /* +0x1A */
    uint32_t projection_effect; /* +0x1C */
    uint32_t material_flag;     /* +0x20 */
} TD5_TrackLightEntry;

/* ========================================================================
 * Traffic Bus Entry (4 bytes)
 * ======================================================================== */

typedef struct TD5_TrafficBusEntry {
    int16_t  span_index;    /* +0x00: -1 = end sentinel */
    uint8_t  flags;         /* +0x02: bit 0 = oncoming direction */
    uint8_t  lane;          /* +0x03: sub-lane index */
} TD5_TrafficBusEntry;

/* ========================================================================
 * MODELS.DAT entry (runtime)
 * ======================================================================== */

typedef struct TD5_ModelsDatEntry {
    void    *mesh_ptr;          /* pointer to mesh data */
    int32_t  texture_page_id;   /* texture page reference */
    float    bounding_radius;   /* bounding sphere */
} TD5_ModelsDatEntry;

/* ========================================================================
 * Directional light slot (runtime)
 * ======================================================================== */

#define TD5_TRACK_LIGHT_SLOTS  3

/* ========================================================================
 * Route Data (LEFT.TRK / RIGHT.TRK)
 *
 * Each file is a byte array indexed by span number, giving lateral offset
 * (0x00-0xFF = left-to-right across track width). Three bytes per span:
 *   [0] = lateral offset byte
 *   [1] = heading byte
 *   [2] = reserved
 * ======================================================================== */

/* --- Module lifecycle --- */
int  td5_track_init(void);
void td5_track_shutdown(void);

/* --- STRIP.DAT loading --- */
int  td5_track_load_strip(const void *data, size_t size);
void td5_track_bind_runtime_pointers(void);
void td5_track_apply_attribute_overrides(void);

/* --- Track runtime data --- */
int  td5_track_load_runtime_data(int track_index, int reverse);

/* --- Span access --- */
int              td5_track_get_span_count(void);
int              td5_track_get_ring_length(void);
/* TD6 branch->main span remap (port of TD6.exe FUN_0045d040): a branch corridor
 * span maps to its parallel main-ring span via the segment-remap table; main
 * spans pass through unchanged. */
int              td5_track_branch_to_main_span(int span);
/* [#18] Nearest non-slow (road) lane to `lane` in `span` — keeps traffic off the
 * TD6 sidewalk/shoulder lanes. Returns `lane` if no road lane is found. */
int              td5_track_nearest_road_lane(int span_index, int lane);
/* Inverse: a main-ring span -> the parallel branch-corridor span, or -1 if none. */
int              td5_track_main_to_branch_span(int main_span);
/* [#20 2026-06-18] 1 if the fork that `span` belongs to (corridor span) or sits at
 * (main span) is blacklisted as a dead-end sidewalk branch on the current track.
 * Direction-aware (the flagged list is mirrored for forward vs reverse). Consumed
 * by the traffic spawner so cars are never placed on these corridors. */
int              td5_track_branch_blacklisted(int span);
/* Branch-corridor enumeration (jump-table records) for the traffic spawner:
 * td5_track_corridor_count() -> number of corridors; td5_track_corridor_info()
 * fills the idx-th corridor's BRANCH span range [branch_lo,branch_hi] (displaced,
 * >= ring) and the parallel MAIN range [main_lo,main_hi] it bypasses. Returns 1
 * on success, 0 if idx is out of range / no table / malformed record. */
int              td5_track_corridor_count(void);
int              td5_track_corridor_info(int idx, int *branch_lo, int *branch_hi,
                                         int *main_lo, int *main_hi);
/* Per-main-span branch query (HUD minimap parallel-branch overlay):
 * count of parallel branches off `main_span`, and the which-th branch span. */
int              td5_track_count_branch_corridors(int main_span);
int              td5_track_branch_corridor_span(int main_span, int which);
/* Are TD6 branch corridors currently drivable (walker takes/traverses them)? */
int              td5_track_td6_branches_drivable(void);
/* TD6 per-lane surface grid (header[6]): class at (span, lateral byte 0..255),
 * and the class->grip(8.8 fixed,0x100=1.0)/drag coefficient tables. -1 class /
 * 0x100 grip / 0 drag when no TD6 grid is loaded. */
int              td5_track_td6_surface_class(int span_index, int lateral_byte);
int              td5_track_td6_surface_grip_q8(int cls);
int              td5_track_td6_surface_drag(int cls);
int              td5_track_td6_surface_grid_loaded(void);
/* TD6 breakable props (level<N>.tcl): load/clear the prop table, query count and
 * per-prop world pos (24.8) + collision radius (world units) + anchor span. */
void             td5_track_load_td6_props(const void *data, size_t size);
void             td5_track_append_td6_props(const void *data, size_t size);
int              td5_track_td6_prop_count(void);
int              td5_track_td6_prop_get_mov(int i, int32_t *out_px, int32_t *out_py,
                                            int32_t *out_pz, int *out_model, int *out_angle);
/* [#20 pushable] Mass (MOV byte 6; 0=immovable), add slide velocity (24.8/tick),
 * and the once-per-tick integrator that slides/decays/settles pushed props. */
int              td5_track_td6_prop_mass(int i);
void             td5_track_td6_prop_push(int i, int32_t dvx, int32_t dvz);
void             td5_track_td6_props_tick(void);
int              td5_track_td6_prop_get(int i, int32_t *out_px, int32_t *out_pz,
                                        int *out_radius_w, int *out_span);
int              td5_track_td6_prop_is_broken(int i);
void             td5_track_td6_prop_set_broken(int i);
void             td5_track_td6_props_reset_broken(void);
/* Render-time: 1 if world point (wx,wz) sits on a SMASHED prop (used to hide the
 * static furniture mesh on top of it). Fast no-broken early-out. */
int              td5_track_td6_prop_broken_near(float wx, float wz);
int              td5_track_td6_broken_count(void);   /* diag: # of smashed props */
/* World-float ground Y at prop i's anchor span (stable; fallback if no strip). */
float            td5_track_td6_prop_ground_y(int prop_index, float fallback);
int              td5_track_get_span_lane_count(int span_index);
/* Per-lane surface type for a span (reuses surface_type_for_span_lane).
 * Returns the surface byte: 0x00-0x0F = main-road surface (low nibble of
 * surface_attribute), 0x10-0x1F = alternate/off-road surface (high nibble |
 * 0x10) when the span's lane bitmask (byte +0x02) marks the lane. Returns
 * TD5_SURFACE_DRY_ASPHALT for an out-of-range span/lane. Used by the
 * S20 smart-traffic lane chooser to avoid parking traffic on a slow lane. */
int              td5_track_get_span_lane_surface(int span_index, int lane);
/* 1 if `surface_type` (as returned above) is a "slow"/off-road surface the
 * smart-traffic chooser should avoid: the alternate-surface bit (0x10) is set,
 * or the base surface is DIRT(3)/GRAVEL(4). Asphalt (dry/wet) is not slow. */
int              td5_track_surface_is_slow(int surface_type);
/* [#18] Traffic-drivable road band (contiguous lanes of the centre lane's surface
 * class) — excludes sidewalk/verge edges that surface_is_slow misses. 1 if found. */
int              td5_track_td6_road_band(int span_index, int lane_count, int *out_lo, int *out_hi);
int              td5_track_branch_to_junction(int span_idx);
int              td5_track_get_fwd_sentinel(void);
/* Junction-table lookup for orig UpdateRaceActors @ 0x00436A70 route_table
 * selector logic. Returns 1 if span_norm falls inside any junction entry's
 * main-road range AND the entry's branch_lo - main_target + span_norm != -1
 * (PATH 2a in orig). Returns 0 for PATH 2b (no match → keep ptr unchanged).
 * Mirrors orig 0x00436ADB..0x00436C74 byte-faithfully. */
int              td5_track_route_junction_path2a_match(int span_norm);
/* Junction-table accessors for slot-init route-selector heuristic
 * (td5_ai_initialize_runtime, BlueRidge regression follow-up 2026-05-18). */
int              td5_track_get_junction_count(void);
const uint8_t   *td5_track_get_junction_entries(void);
TD5_StripSpan   *td5_track_get_span(int index);
TD5_StripVertex *td5_track_get_vertex(int index);
int              td5_track_is_valid_mesh_ptr(const void *ptr);
int              td5_track_is_ptr_in_blob(const void *ptr, size_t size);
void            *td5_track_get_display_list(int span_index);
/* Entry-indexed MODELS.DAT lookup. Caller must pre-divide span_index by 4
 * (orig 0x0042BBBC SHR). Replaces per-span iteration's 4× redundant block
 * submission with orig's per-entry walk shape. */
void            *td5_track_get_display_list_entry(int entry_index);

/* --- MODELS.DAT --- */
int  td5_track_parse_models_dat(const void *data, size_t size);
void td5_track_prepare_mesh_resource(TD5_MeshHeader *mesh);

/* Halve per-vertex diffuse of billboard meshes that draw through a
 * type-3 (additive) texture page. Call AFTER td5_asset_load_track_textures
 * so the transparency table is populated. */
void td5_track_dim_additive_billboard_meshes(void);
/* [LIGHT2 P1] One-shot post-load pass: derive flat face normals for every
 * MODELS.DAT mesh without a normals stream (G-buffer feed only — baked vertex
 * lighting is untouched). Derived streams are tagged with bit 0 of
 * normals_offset; call right after td5_track_dim_additive_billboard_meshes. */
void td5_track_derive_missing_normals(void);
/* [LIGHT2] One-shot post-load pass: register every street-lamp glow fixture
 * (type-3 billboard mesh) with the dynamic-light system so the nearest few
 * become real point lights per frame. Call AFTER track textures load. */
void td5_track_register_lamp_lights(void);
/* [NATIVE BANNERS] One-shot post-load pass: flag the texture pages of native
 * (g_active_td6_level==0) double-sided sign panels, detected by finding
 * coincident reverse-wound primitive pairs in the MODELS.DAT geometry. The
 * banner one-sided cull in clip_and_submit_polygon consults this so native
 * track banners stop z-fighting/reading mirrored. TD5RE_BANNER_PAIR_CULL=0
 * disables. Call after td5_track_dim_additive_billboard_meshes. */
void td5_track_scan_banner_pages(void);
int  td5_track_is_native_banner_page(int page);
int  td5_track_get_models_display_list_count(void);
int  td5_track_get_span_display_list_index(int span_index);
const void *td5_track_get_models_display_list_raw(int index, size_t *size_out);

/* --- Actor track position --- */
void td5_track_update_actor_position(TD5_Actor *actor);
void td5_track_update_probe_position(struct TD5_TrackProbeState *probe,
                                     int32_t world_x, int32_t world_z);

/* Write the per-probe contact_vertex_A/B fields from the probe's current
 * span_index + sub_lane_index. Mirrors the prefix of
 * ComputeActorTrackContactNormal[Extended] (0x00445450 / 0x004457E0):
 *   iVar5 = strip[span].left_vertex_index + sub_lane + LUT_E40[type * 2]
 *   iVar7 = strip[span].right_vertex_index + sub_lane + LUT_E41[type * 2]
 * Called after td5_track_update_probe_position so the (possibly walker-
 * advanced) span_index is the one used. */
void td5_track_compute_probe_contact_vertices(struct TD5_TrackProbeState *probe);
int  td5_track_get_surface_type(TD5_Actor *actor, int probe_index);
void td5_track_compute_heading(TD5_Actor *actor);

/* InterpolateTrackSegmentNormal (byte-faithful port of 0x00445E30).
 * Inner helper used by ComputeActorHeadingFromTrackSegment (0x00445B90) to
 * write the int16 surface normal at out_normal[0..2] (caller-side pointer
 * to actor+0x290). Computes cross-product of (va-vb) x (va-vc), >>12,
 * FPU-normalises to length 4096.0f, truncates to int16, applies the
 * post-conversion `if (uny == 0) uny = 1` sentinel so the int16 .y
 * divisor in ApplyMissingWheelVelocityCorrection (0x00403EB0) is never
 * exactly zero. va/vb/vc are vertex indices into the strip vertex pool. */
void td5_track_interpolate_segment_normal(int16_t va_idx, int16_t vb_idx,
                                           int16_t vc_idx, int16_t *out_normal);

/* Byte-faithful port of ComputeActorHeadingFromTrackSegment @ 0x00445B90.
 * Per-tick heading-normal writer. Reads actor's track_state at +0x80
 * (span_idx, sub_lane), looks up the live span record, picks a triangle
 * via a two-level (sub_lane × span_type) dispatch, and writes the
 * normalized surface normal back to actor +0x290 (heading_normal int16[3]).
 *
 * Called from the per-tick pose integrators:
 *   - IntegrateVehiclePoseAndContacts   (player/AI per tick)
 *   - UpdateVehiclePoseFromPhysicsState (player/AI per tick)
 *   - UpdateTrafficVehiclePose          (traffic per tick)
 *
 * This is distinct from td5_track_compute_heading() above, which is the
 * SPAWN-only initializer port of 0x00434350 and writes a different vector
 * (with heading_normal.y hard-coded to 0). */
void td5_track_compute_runtime_heading_normal(TD5_Actor *actor);

/* --- Barycentric contact --- */
int32_t td5_track_compute_contact_height(int span_index, int sub_lane,
                                          int32_t world_x, int32_t world_z);
int32_t td5_track_compute_contact_height_with_normal(int span_index, int sub_lane,
                                                      int32_t world_x, int32_t world_z,
                                                      int16_t *out_normal);
/* Height + normal using the best-fit (geometrically containing) lane within
 * span_index, rather than a caller-supplied sub_lane. */
int32_t td5_track_compute_contact_height_bestlane(int span_index,
                                                  int32_t world_x, int32_t world_z,
                                                  int16_t *out_normal);
/* Height + normal using a BOUNDED (+/-1) containing-lane search around the
 * walker's carried sub_lane. Reproduces the original's containing-lane height
 * while staying continuous across lane-count transitions (no slope-roll). */
int32_t td5_track_compute_contact_height_bounded(int span_index, int carried_sub_lane,
                                                 int32_t world_x, int32_t world_z,
                                                 int16_t *out_normal);
/* [task#4] Stable per-(slot,node) ground height for the vehicle SHADOW grid:
 * keeps a persistent per-node span/lane seed + converging walk so a node does
 * not oscillate between adjacent spans on a slope (uphill shadow flicker).
 * Gated by TD5RE_SHADOW_PROBE_FIX (default on; off = old chassis-seeded
 * single-step behavior). Render-only; never used by physics. */
int32_t td5_track_shadow_probe_height(int slot, int node,
                                      int chassis_span, int chassis_lane,
                                      int32_t world_x, int32_t world_z,
                                      int16_t *out_normal);
/* Returns nonzero if the most recent contact-height probe capped an upward
 * out-of-quad extrapolation (fast-tilt OOB launch fix). The per-wheel contact
 * refresh reads this immediately after its bestlane call to force the wheel
 * airborne instead of letting it "ground" on a fictional extrapolated plane. */
int td5_track_last_contact_was_capped(void);
int  td5_track_probe_height(int world_x, int world_z, int current_span,
                             int *out_y, int *out_surface_type);
void td5_track_get_span_edges(int span_index,
                               int *left_x, int *left_z,
                               int *right_x, int *right_z);
/* Rail frame matching td5_track_sample_target_point's bias axis (track coords).
 * Used by the smart-AI lane brain so its lateral target shares the bias sign. */
int  td5_track_get_span_route_frame(int span_index, int *lx, int *lz,
                                    int *rx, int *rz);
int  td5_track_get_span_center_world(int span_index,
                                      int *out_x, int *out_y, int *out_z);
int  td5_track_get_span_lane_world(int span_index, int sub_lane,
                                    int *out_x, int *out_y, int *out_z);

/* [STUCK RECOVERY 2026-06-15] Resolve a "respawn a few spans back, centred" pose
 * for the player car-recovery feature. Steps `spans_back` spans behind
 * `from_span` (wraps on a closed circuit, clamps at span 0 on point-to-point),
 * skips junction span-types 9/10, picks the target span's CENTER lane, and
 * returns its center-lane world XYZ (24.8 fixed-point) plus the resolved span +
 * sub-lane. Heading is NOT computed here — the caller runs td5_track_compute_
 * heading() on the actor after writing the span (mirrors the spawn-pose path).
 * Returns 1 on success, 0 if no track is loaded / no drivable span resolved.
 * Any out-pointer may be NULL. */
int  td5_track_get_recovery_pose(int from_span, int spans_back,
                                 int *out_span, int *out_sub_lane,
                                 int *out_x, int *out_y, int *out_z);

/* Byte-faithful port of InitActorTrackSegmentPlacement @ 0x00445F10.
 * - actor_at_0x80 must point to actor + 0x80 (span_raw int16); the helper
 *   reads param_1[0] (span), param_1[6]/byte+12 (sub_lane), and writes back
 *   param_1[2] (span_accum), param_1[3] (span_high), and clamped sub_lane.
 * - out_pos receives world position in 24.8 fixed-point at out_pos[0..2]
 *   (laid out to match actor +0x1FC/+0x200/+0x204).
 * Used by traffic spawn (UpdateTrafficActorRecycle / InitializeTrafficActorsFromQueue). */
void td5_track_init_actor_segment_placement(int16_t *actor_at_0x80, int32_t *out_pos);

int  td5_track_span_lane_count_at(int span_index);

/* [DRAG LENGTHEN] 1 if the drag strip was lengthened this load (clean spans
 * inserted mid-strip); insert-span = where they were inserted (ribbon paints +
 * MODELS.DAT scenery is suppressed from here on); finish-span = finish on the
 * inserted clean road. The latter two are -1 when no lengthening occurred. */
int  td5_track_drag_applied_repeats(void);
int  td5_track_drag_insert_span(void);
int  td5_track_drag_finish_span(void);
int  td5_track_drag_tail_end(void);

/* [LANE ASSIST 2026-06-28] Continuous look-ahead lane-centre target line for the
 * optional steering aid (td5_laneassist.c). Walks up to `lookahead` spans forward
 * from (from_span, from_sub_lane) following the SAME fork rule the original walker
 * uses (UpdateActorTrackPosition @ 0x004440F0): at a type-8 forward junction
 * sub_lane < next_lanes stays on the main road (span+1), else commits to the
 * branch via link_next and carries the lane with the confirmed remap
 * sub_lane += branch_lanes - cur_lanes (fork_commit=0 ignores branches and stays
 * on the main line). Each look-ahead span is sampled as the centre of a
 * `lane_band`-wide group of adjacent lanes (1 = single lane; 2 = centre of two
 * lanes, more forgiving for narrow lane changes). Blends the sampled centres
 * (near-weighted) into one aim point in 24.8 fixed-point world XZ.
 * When `fork_diverge` is set (and fork_commit on), an upcoming forward junction is
 * scouted and the approach/fork spans aim at only the committed branch's lanes
 * (the side the walker takes: [0,next_lanes) main / [next_lanes,lc) branch), so the
 * car diverges toward one fork a few spans early instead of aiming at the divider.
 * Returns 1 on success, 0 if no track is loaded or no drivable look-ahead span. */
int  td5_track_laneassist_target(int from_span, int from_sub_lane,
                                 int lookahead, int fork_commit, int fork_diverge,
                                 int lane_band, int *out_x, int *out_z);
/* [LANE ASSIST 2026-06-28] Diagnostic: walk ONE continuous lane forward from
 * `start` for up to `count` spans (carrying the lane across junctions, as the aid
 * does) and log the look-ahead lane-centre target + inter-step delta, flagging
 * fork/merge rows (types 8/9/10/11). Confirms target-line continuity across
 * junctions in a trace run. Env-gated by the caller. */
void td5_track_laneassist_sweep_diag(int start, int count, int lookahead,
                                     int fork_commit, int fork_diverge, int lane_band);

/* --- Lighting ---
 * Per-vehicle zone-driven lighting now lives in td5_render.c as
 * td5_render_apply_track_lighting(); td5_track no longer exposes any
 * lighting helpers. */

/* --- Track wall contact resolution (FUN_00406CC0) ---
 * Checks all 4 wheel probes against span edge boundaries.
 * Calls td5_physics_wall_response for each penetration. */
void td5_track_resolve_wall_contacts(TD5_Actor *actor);

/* --- Debug overlay: emit wall/rail wireframe lines for spans near the
 * given span index. Lines are pushed via td5_render_debug_line_world().
 * Caller is responsible for flushing via td5_render_debug_lines_flush().
 * span_radius is the number of spans before/after `center_span` to draw. */
void td5_track_debug_emit_collision_lines(int center_span, int span_radius);

/* --- Debug overlay: mark TD6 breakable props within max_dist world units of
 * (gx,gz) as magenta poles (grey when broken), at ground_y. Caller flushes. */
void td5_track_debug_emit_prop_markers(float gx, float ground_y, float gz,
                                       float max_dist);

/* --- Forward/Reverse boundary contact resolution ---
 * FUN_00406F50 / FUN_004070E0 — check probes against the track's
 * forward-/reverse-side boundary sentinel strips. The two sentinels
 * are per-level constants from a 40-entry table at original VA
 * 0x00473820, NOT `1` / `s_span_count-2`. Bind via
 * td5_track_bind_boundary_sentinels() before racing on a level. */
void td5_track_resolve_forward_contacts(TD5_Actor *actor);
void td5_track_resolve_reverse_contacts(TD5_Actor *actor);

/* Bind the forward/reverse boundary sentinel pair for the level about
 * to race. level_number is the 1-based level ZIP number (same value
 * td5_asset_level_number() returns). Values outside the populated
 * range disable the boundary handlers for that level. */
void td5_track_bind_boundary_sentinels(int level_number);

/* --- Traffic --- */
void td5_track_init_traffic_from_queue(void);

/* --- Route data (LEFT/RIGHT.TRK) --- */
int  td5_track_load_routes(const void *left_data, size_t left_size,
                            const void *right_data, size_t right_size);

/* --- Wrap normalization --- */
int  td5_track_normalize_actor_wrap(TD5_Actor *actor);

/* --- Segment boundary remap (0x443FF0 ResolveActorSegmentBoundary) --- */
void td5_track_resolve_actor_segment_boundary(TD5_Actor *actor);

/* --- Route heading helpers --- */
int  td5_track_get_primary_route_heading(int span_index);

/* --- Signed spline distance (0x434670) --- */
int32_t td5_track_compute_spline_position(int span_index, int segment_distance,
                                           int route_lane);

/* --- AI target point sampling (0x434800) ---
 * Computes world-space XZ target for AI look-ahead by interpolating between
 * strip vertices using route_byte, then applying perpendicular lateral_bias.
 * Returns 24.8 fixed-point coordinates. Returns 1 on success, 0 on failure. */
int  td5_track_sample_target_point(int span_index, int route_byte,
                                    int *out_x, int *out_z, int lateral_bias);

/* --- ComputeTrackSpanProgress (0x004345B0)
 * Projects actor 24.8 position onto span direction (right_vertex_index base).
 * Returns packed int64: low32 = 8-bit normalised progress (0-255), high32 = rem. */
int64_t td5_track_compute_span_progress(int span_index, const int32_t *actor_pos);

/* --- ComputeSignedTrackOffset (0x00434670)
 * Returns signed world-unit lateral distance between progress and route_byte.
 * Negative when progress < route_byte. */
int32_t td5_track_compute_signed_offset(int span_index, int progress, int route_byte);

/* --- Track globals (defined in td5_track.c) --- */
extern int       g_track_is_circuit;
extern int       g_track_type_mode;
extern int       g_strip_span_count;
extern int       g_strip_total_segments;
extern void     *g_strip_span_base;
extern void     *g_strip_vertex_base;
extern uint16_t *g_checkpoint_array;

/* --- AI target span remap (0x00435180-0x00435260 inside UpdateActorTrackBehavior)
 * Remaps a linear-advanced span index through the STRIP.DAT junction table
 * when the actor is on a non-canonical route (i.e. NOT the LEFT.TRK route).
 * Record layout per [CONFIRMED @ 0x00435218] walker disasm:
 *   +0 u16 remap_dst
 *   +2 u16 remap_end_exclusive
 *   +4 u16 range_lo
 * Match condition: range_lo <= lin <= range_lo + (remap_end_excl - remap_dst) - 1.
 * Remap formula: cand = (remap_dst - range_lo) + lin. If cand == -1 sentinel,
 * remap is suppressed and lin is returned unchanged. Walker exits on first
 * match (break semantics).
 * Pass `is_canonical_route=1` when the actor's route_ptr equals LEFT.TRK (the
 * walker is skipped in that case). */
int  td5_track_apply_target_span_remap(int lin_span, int is_canonical_route);

/* --- Per-probe contact helpers (0x4440F0 region, defined in td5_track.c) --- */
void UpdateActorTrackPosition(short *probe, int *pos);
void ComputeActorTrackContactNormal(short *probe, int *pos, int *out_y);

#endif /* TD5_TRACK_H */
