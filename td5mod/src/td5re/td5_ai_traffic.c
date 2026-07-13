/**
 * td5_ai_traffic.c -- Traffic subsystem (S5 module split, see REFACTOR_PLAN.md)
 *
 * Extracted verbatim from td5_ai.c's "Traffic System" section (background
 * traffic lifecycle, dynamic ambient traffic spawner, smart-traffic reactive
 * driving, jam/collision escape, cop-chase collision helpers). Pure code
 * motion -- same statements, same order; shared state/macros come from
 * td5_ai_internal.h.
 */

#include "td5_ai.h"
#include "td5_ai_internal.h"
#include "td5_bytes.h"
#include "td5_math_util.h"
#include "td5_track.h"
#include "td5_physics.h"
#include "td5_platform.h"
#include "td5_sound.h"
#include "td5_trace.h"
#include "td5_race_state.h"  /* read-only game queries (was td5_game.h) */
#include "td5re.h"
#include "td5_config.h"
#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>

#define LOG_TAG "ai"

/* Forward decl -- traffic_lane_is_clear is used by traffic_escape_pick_side,
 * defined further up in this file (moved verbatim from td5_ai.c). */
static int traffic_lane_is_clear(int self_slot, int self_span,
                                 int target_lane, int polarity);

/* ========================================================================
 * Traffic System
 *
 *   0x4353B0  RecycleTrafficActorFromQueue
 *   0x435940  InitializeTrafficActorsFromQueue
 *   0x435E80  UpdateTrafficRoutePlan
 *   0x433680  FindNearestRoutePeer
 *
 * Traffic actors occupy slots 6-11. They use:
 *   - Constant speed: encounter_steering_override = 0x3C (60)
 *   - 7-stage FSM: recycle -> heading -> edge -> normal -> target -> steer -> yield
 * ======================================================================== */

/**
 * FindNearestRoutePeer (precise-00433680): byte-faithful port of the original
 * at 0x00433680. Replaces the prior simplified-by-active-count scan.
 *
 * [CONFIRMED @ disassembly 0x00433680-0x004337D1 — full pool6 audit.]
 *
 * Two near-identical loops gated on `route_state[+0xfc]` (the dword that
 * Ghidra symbolises as `gActorRouteDirectionPolarity`, i.e. RS dword index
 * 0x3F — NOT the field already known in this port as `RS_DIRECTION_POLARITY`
 * which is index 0x25 and indexes a separate byte at RS+0x94 used elsewhere
 * for the traffic heading-mirror branch). The +0xfc field controls direction
 * sense for the peer scan: 0 → look ahead (peer ahead of self); nonzero →
 * look behind (peer behind self).
 *
 * Both loops iterate the FULL 12-slot actor pool (slots 0..11), gated by the
 * EBX/EBP < 0x4b08bc terminator at 0x0043371a/0x004337b3 — i.e. the
 * route-state stride times 12 from base 0x004afb6c. The prior port gated on
 * `g_active_actor_count`; the original does not.
 *
 * Same-actor skip: `i == self_slot` skips the slot whose RS_SLOT_INDEX
 * (dword 0x35 = `gActorTrackReferenceSlot`) matches the caller's iVar2.
 *
 * Lane match: byte at actor+0x8c == self.field_0x8c.
 *
 * Route-table identity: dword at RS[0x00] (`gActorRouteTableSelector` =
 * the route-table base POINTER, NOT the +0x0c selector at index 0x03)
 * compared between self.RS[0] and peer.RS[0]. The prior port compared
 * peer.RS[0x03] to self_selector — that was the wrong field.
 *
 * Distance gate: `0 < dist <= 0x20` for the final accept (TEST/JZ at
 * 0x00433726 + CMP 0x20/JG at 0x0043372e). Prior port used `< 0x22`.
 *
 * Direction-relative distance:
 *   path 1 (`*+0xfc == 0`): keep peers with peer_span >= self_span;
 *                            dist = peer_span - self_span
 *   path 2 (`*+0xfc != 0`): keep peers with peer_span <= self_span;
 *                            dist = self_span - peer_span
 *
 * Return: `local_4` = best peer slot if (0 < best_dist <= 0x20), else iVar2
 * (=self_slot). The original returns either iVar2 directly or via the
 * load-from-stack at 0x00433737 / 0x004337c8; both paths share the trailing
 * "if (best_dist == 0 || best_dist > 0x20) preserve iVar2" semantic.
 */

/* Route-state direction-polarity dword [CONFIRMED via Ghidra symbol
 * `gActorRouteDirectionPolarity` @ 0x004afc5c; route-state base 0x004afb60;
 * delta 0xfc bytes = dword index 0x3F]. Same field as RS_ROUTE_DIRECTION_POLARITY
 * declared at the top of this file. FindNearestRoutePeer uses this to switch
 * ahead/behind peer-scan direction; UpdateTrafficRoutePlan uses the same dword
 * for the route-heading mirror branch. Both names alias the same field. */
#define RS_PEER_SCAN_REVERSE      RS_ROUTE_DIRECTION_POLARITY


/* ===== SECTION: traffic actor lifecycle: recycle, queue, init ===== */

int td5_ai_find_nearest_route_peer(int *route_state_ptr) {
    int self_slot = route_state_ptr[RS_SLOT_INDEX];
    int32_t scan_reverse = route_state_ptr[RS_PEER_SCAN_REVERSE];
    int32_t self_route_ptr;
    int16_t self_span;
    uint8_t self_lane;
    int32_t best_dist = 0x2ee00;   /* CONFIRMED @ 0x0043369b */
    int best_slot = self_slot;     /* CONFIRMED: local_4 = iVar2 @ 0x00433697 */
    int i;

    /* Bounds: required because route_state(slot) returns the storage array;
     * disassembly bypasses bounds because EAX is read from a struct field. */
    if (self_slot < 0 || self_slot >= TD5_MAX_TOTAL_ACTORS) {
        return self_slot;
    }

    {
        char *self = actor_ptr(self_slot);
        self_span = ACTOR_I16(self, ACTOR_SPAN_RAW);
        self_lane = ACTOR_U8(self, ACTOR_SUB_LANE_INDEX);
        self_route_ptr = route_state_ptr[RS_ROUTE_TABLE_PTR];
    }

    if (scan_reverse == 0) {
        /* ----------------------------------------------------------------
         * Path 1 [CONFIRMED @ 0x004336a9-0x00433722]: forward scan.
         * Keep peers ahead: dist = peer_span - self_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EBP,EAX / JZ skip @ 0x004336b5 */
            if (i == self_slot) continue;
            /* [PER-VIEWPORT TRAFFIC] ignore cars from other partitions (the twin
             * sits at the same span/lane and would read as a 0-distance peer ->
             * phantom brake + desync). No-op when per-viewport is off. */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV SI,[EDI] @ 0x004336b9 → peer.field_0x80 = peer_span.
             * MOV DX,[ECX + 0x4ab188] @ 0x004336cd → self.field_0x80 = self_span.
             * CMP DX,SI / JG skip @ 0x004336d4 → skip if self_span > peer_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (self_span > peer_span) continue;

            /* MOV CL,[ECX + 0x4ab194] @ 0x004336d9 → self.field_0x8c = self_lane.
             * CMP CL,[EDI + 0xc] @ 0x004336df → compare against peer.field_0x8c.
             * JNZ skip @ 0x004336e2 → lane must match. */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV ECX,[ECX*4 + 0x4afb6c] @ 0x004336ec → self.RS[0] (route ptr).
             * CMP ECX,[EBX] @ 0x004336f3 → compare against peer.RS[0].
             * JNZ skip @ 0x004336f5 → route-table pointers must match. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,SI / SUB ECX,EDX @ 0x004336f7-0x004336fd
             * → dist = (int32)peer_span - (int32)self_span.
             * CMP ECX,best_dist / JGE skip @ 0x004336ff-0x00433703
             * → strict less-than: skip if dist >= best_dist. */
            dist = (int32_t)peer_span - (int32_t)self_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x00433726-0x00433728: best_dist == 0 → return self
         * CMP ECX,0x20 / JG @ 0x0043372e-0x00433731: best_dist > 0x20 → return self
         * MOV EAX,best_slot / RET @ 0x00433737-0x00433740: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    } else {
        /* ----------------------------------------------------------------
         * Path 2 [CONFIRMED @ 0x00433741-0x004337c1]: reverse scan.
         * Keep peers behind: dist = self_span - peer_span, < best_dist.
         * ---------------------------------------------------------------- */
        for (i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
            char *peer_actor;
            int32_t *peer_rs;
            int16_t peer_span;
            uint8_t peer_lane;
            int32_t dist;

            /* CMP EDI,EAX / JZ skip @ 0x0043374d */
            if (i == self_slot) continue;
            /* [PER-VIEWPORT TRAFFIC] skip other partitions' cars (gated; no-op off). */
            if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;

            peer_actor = actor_ptr(i);
            peer_rs = route_state(i);

            /* MOV DL,[ESI + 0xc] @ 0x00433751 → peer.field_0x8c = peer_lane.
             * CMP DL,[ECX + 0x4ab194] @ 0x00433765 → compare against self.field_0x8c.
             * JNZ skip @ 0x0043376b → lane must match.
             * (Note path 2 tests lane BEFORE span, opposite of path 1; semantics same.) */
            peer_lane = ACTOR_U8(peer_actor, ACTOR_SUB_LANE_INDEX);
            if (peer_lane != self_lane) continue;

            /* MOV DX,[ESI] @ 0x0043376d → peer.field_0x80 = peer_span.
             * MOV CX,[ECX + 0x4ab188] @ 0x00433770 → self.field_0x80 = self_span.
             * CMP DX,CX / JG skip @ 0x00433777-0x0043377a → skip if peer_span > self_span. */
            peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
            if (peer_span > self_span) continue;

            /* MOV EBX,[EBX*4 + 0x4afb6c] @ 0x00433784 → self.RS[0].
             * CMP EBX,[EBP] @ 0x0043378b → compare against peer.RS[0].
             * JNZ skip @ 0x0043378e. */
            if (self_route_ptr != peer_rs[RS_ROUTE_TABLE_PTR]) continue;

            /* MOVSX EDX,DX / MOVSX ECX,CX / SUB ECX,EDX @ 0x00433790-0x00433796
             * → dist = (int32)self_span - (int32)peer_span. */
            dist = (int32_t)self_span - (int32_t)peer_span;
            if (dist >= best_dist) continue;

            best_dist = dist;
            best_slot = i;
        }

        /* TEST ECX,ECX / JZ @ 0x004337bf-0x004337c1: best_dist == 0 → preserve self.
         * CMP ECX,0x20 / JG @ 0x004337c3-0x004337c6: best_dist > 0x20 → preserve self.
         * MOV EAX,best_slot @ 0x004337c8: otherwise return best_slot. */
        if (best_dist == 0)   return self_slot;
        if (best_dist > 0x20) return self_slot;
        return best_slot;
    }
}

/**
 * RecycleTrafficActorFromQueue — byte-faithful port of 0x004353B0.
 *
 * [CONFIRMED @ 0x004353B0] L5 promotion sweep audit (2026-05-18).
 *
 * Verbatim 10-step translation per disassembly listing (1352 bytes /
 * ~340 instructions). Each step in the comment block below is anchored
 * to its orig listing range:
 *   1.  Bail g_racerCount <= 6           [0x004353B0-C1].
 *   2.  Cursor pre-scan over queue       [0x004353C9-FD].
 *   3.  Linear scan slots 6..min(N,12)   [0x004353FE-465].
 *   4.  best_dist <= 0x28 early-return.
 *   5.  *cursor == -1 early-return.
 *   6.  Slot-9 + special-encounter gate.
 *   7.  RS direction polarity + table_ptr_index write.
 *   8.  LEFT/RIGHT branch dispatch (queue_byte vs strip_byte):
 *       - LEFT: InitActorTrackSegmentPlacement + geometry + angle +
 *         polarity + ResetVehicleActorState + RefreshActorTrack
 *         ProgressOffset + NormalizeActorTrackWrapState.
 *       - RIGHT: inline jump-table scan + remap + Init + geometry +
 *         RefreshActorTrackProgressOffset + ResolveActorSegmentBoundary.
 *   9.  Advance g_activeTrafficBusCursor +4.
 *  10.  LAB_0043588d post-call zero block.
 *
 * ARCHITECTURAL DIVERGENCES (documented in code below):
 *   - RS_DIRECTION_POLARITY: the port writes ONLY 0x3F (dword index 0x3F =
 *     gActorRouteDirectionPolarity), matching the original. The earlier
 *     defensive dual-write to 0x25 was removed (0x25 has no readers in orig).
 *   - Recycle heading dispatch collapsed into td5_track_compute_heading
 *     (see memory/reference_arch_recycle_heading_collapse.md).
 *
 * KNOWN DIVERGENCES: none beyond the two documented architectural ones.
 *
 * Effective level: L5 (byte-faithful per static audit; arch-divergences
 * are equivalence-preserving by design).
 *
 * Verbatim translation of TD5_d3d.exe @ 0x004353B0 disassembly (pool3
 * 2026-05-14). Replaces the prior semantically-named port (which used
 * wrong route-state offsets, wrote +0x82/+0x84/+0x86 directly instead
 * of letting InitActorTrackSegmentPlacement / RSB / NormalizeWrap do
 * it, and called helpers in the wrong direction).
 *
 * Listing flow:
 *   1. Bail if g_racerCount <= 6 (no traffic).
 *   2. Pre-scan g_traffic_queue cursor forward over entries that are
 *      either == -1 sentinel, behind player.span_norm (+0x82), or within
 *      0x28 spans ahead. Advance g_activeTrafficBusCursor over rejected entries.
 *   3. Linear scan slots 6..min(g_racerCount,12)-1: find the slot with
 *      max (player.span_norm - traffic[i].span_norm) — store as
 *      (best_slot, best_dist). NOTE: gate (>=0x29) is applied AFTER the
 *      scan, not inside it. Slot 9 is NOT excluded from the scan.
 *   4. If best_dist <= 0x28: write cursor and return.
 *   5. If *cursor == -1: write cursor and return (queue exhausted at the
 *      currently-pointed entry — no recyclable spawn).
 *   6. If best_slot == 9 AND gSpecialEncounterTrackedActorHandle != -1:
 *      write cursor and return. (This is the slot-9 protection — but
 *      only fires when slot 9 was actually selected as best.)
 *   7. Set route_state[best_slot] direction polarity (dword +0xFC),
 *      route_table_ptr_index (dword +0x68) = best_slot*4+0x10.
 *   8. Compare queue.byte_3 (sub_lane) vs g_trackStripRecords[queue_span * 0x18 + 3] & 0xf:
 *        - queue_byte > strip_byte: LEFT branch (selector=0)
 *            * actor+0x80 = queue_span; actor+0x8c = queue_byte (raw)
 *            * Call InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC)
 *            * Geometry-derive heading from strip vertex deltas (case 1-7)
 *            * Call AngleFromVector12Full; store (angle+0x800)*0x100 to +0x1f4
 *            * If polarity flag, add 0x80000 to yaw
 *            * Call ResetVehicleActorState(actor)
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call NormalizeActorTrackWrapState(actor)
 *        - queue_byte <= strip_byte: RIGHT branch (selector=1)
 *            * Inline jump-table scan: queue_span in [third, third+(high-low)-1]
 *              → remapped = queue_span + (low - third); else -1 sentinel.
 *            * actor+0x80 = remapped; actor+0x8c = queue_byte - (strip_byte & 0xf)
 *            * Call InitActorTrackSegmentPlacement; geometry; angle; polarity; reset.
 *            * Call RefreshActorTrackProgressOffset(best_slot)
 *            * Call ResolveActorSegmentBoundary(actor) (0x00443FF0)
 *   9. Advance g_activeTrafficBusCursor by 4 bytes (past consumed entry).
 *  10. Zero post-call state fields (LAB_0043588d) including velocities,
 *      steering, encounter_handle = -1, etc.
 *
 * NOTE on RS_DIRECTION_POLARITY: the disassembly writes to
 * gActorRouteDirectionPolarity (0x004afc5c), which is at route_state
 * base+0xFC = dword index 0x3F. The port writes ONLY this field, via
 * rs[RS_ROUTE_DIRECTION_POLARITY] (see the live write below), matching the
 * original. An earlier port also wrote dword 0x25, but that field has no
 * readers in the original listing, so the defensive dual-write was removed —
 * no discrepancy remains.
 *
 * NOTE on ResolveActorSegmentBoundary: 0x00443FF0 is ported in unmerged
 * branch precise-00443FF0 (SHA b99e36a) as td5_track_resolve_actor_segment_boundary.
 * Declared __attribute__((weak)) so the port links cleanly in master
 * (with NormalizeActorTrackWrapState as fallback) and uses the real RSB
 * once b99e36a merges.
 */

/* Forward decl, weakly linked: real symbol comes from b99e36a's
 * td5_track.c port of ResolveActorSegmentBoundary @ 0x00443FF0.
 * If unresolved at link time, the address is NULL and the fallback runs. */
extern void td5_track_resolve_actor_segment_boundary(TD5_Actor *actor)
    __attribute__((weak));

/* ARCHITECTURAL DIVERGENCE — see
 *   memory/reference_arch_recycle_heading_collapse.md
 *
 * The original 0x004353B0 inlines a span_type → vertex-offset case dispatch
 * (cases 1/2/5, 3/4, 6/7) twice — once per LEFT/RIGHT branch — feeding
 * AngleFromVector12Full to derive the spawn heading. In the port we instead
 * route through td5_track_compute_heading, which implements the same
 * geometry-from-strip atan2 contract for the actor's current span and
 * writes the result into actor+0x1F4. The case dispatch is therefore
 * absorbed into that helper; do not re-implement it here.
 *
 * Static byte-equivalence audit (cases 1/2/5, 3/4, 6/7, default):
 *  - span source: actor+0x80 (SPAN_RAW) is written with q_span (LEFT) or
 *    remapped (RIGHT) before each helper call, matching what the original's
 *    inline dispatch keys off.
 *  - vertex offsets, signed div-by-4 (>>2 with sign bias), and the
 *    (angle+0x800)<<8 writeback to +0x1F4 all match td5_track.c:3139.
 *  - helper additionally writes actor+0x290 (heading_normal) which the
 *    inline original may not — benign because nothing in the post-call
 *    zero block (LAB_0043588d) reads +0x290.
 *
 * If you ever need to byte-diff this against the original, prototype was
 * `recycle_compute_heading_angle_12` (mentioned in ca6e5bb commit body)
 * but was removed pending byte-diff validation against the existing helper.
 */

/* [PORT: N-way split] Span extremes across the LOCAL human players (slots
 * 0..num_human_players-1, clamped to the racer range). span = span_normalized
 * (+0x82). For a single player both return slot 0's span, so legacy traffic
 * recycling stays byte-identical to the original. Used so traffic is recycled
 * only once the TRAILING player has passed it and respawns ahead of the LEAD
 * player. */
/* [PER-VIEWPORT TRAFFIC 2026-06-22] When >= 0, the dynamic-traffic spawner /
 * manager is SCOPED to a single viewport's player: the anchor + proximity helpers
 * below consider ONLY this racer slot, so each split-screen viewport's traffic set
 * is placed relative to its own player. -1 (the default) restores the original
 * all-players behaviour, so every non-scoped caller is byte-identical. */
static int s_trf_scope_slot = -1;

static int ai_player_span_lead(void)
{
    int humans = g_td5.num_human_players;
    if (s_trf_scope_slot >= 0)
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    int ext = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    for (int s = 1; s < humans; s++) {
        int sp = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        if (sp > ext) ext = sp;
    }
    return ext;
}
static int ai_player_span_trailing(void)
{
    int humans = g_td5.num_human_players;
    if (s_trf_scope_slot >= 0)
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    int ext = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    for (int s = 1; s < humans; s++) {
        int sp = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        if (sp < ext) ext = sp;
    }
    return ext;
}

/* [traffic one-way] Forward decl — defined next to the other trf_* knobs below.
 * When on, every traffic lane is forced to head down-track (polarity 0) so all
 * traffic flows the SAME direction (user: "the orientation should make the
 * traffic all head the same way"). Used by the legacy queue spawn paths here
 * AND the dynamic spawner's lane-direction / cross-traffic helpers. */
static int trf_oneway_traffic(void);

void td5_ai_recycle_traffic_actor(void) {
    int       racer_count;
    int       best_slot = 0;
    int32_t   best_dist = 0;
    int       i;
    int       loop_max;
    char     *p0;
    int16_t   player_span_norm;
    const uint8_t *qp;
    int16_t   q_span;
    uint8_t   q_flags;
    uint8_t   q_byte_3;
    char     *a;
    int32_t  *rs;
    uint8_t  *rs_bytes;

    /* [0x004353b0-c1] EAX = g_racerCount; bail if <= 6. The original reads
     * `dword ptr [0x004aaf00]` which is g_racerCount (also exposed in port
     * as g_active_actor_count for the runtime active-slot count).
     * [PORT: N-way] "6" is really the racer/traffic boundary — only recycle
     * once traffic actors exist beyond the racer slots. */
    racer_count = g_active_actor_count;
    if (racer_count <= g_traffic_slot_base) return;

    if (!g_actor_base || !g_route_state_base) return;
    if (!g_traffic_queue_ptr || !g_traffic_queue_base) return;

    /* [0x004353c9-fd] Pre-scan: advance queue cursor over entries that are
     * not-yet-overtaken by the player. The match condition for STOPPING the
     * scan is:
     *   q_span == -1  (sentinel; loop exits)
     *   OR q_span < player.span_norm   (entry is behind player → stop)
     *   OR q_span - player.span_norm >= 0x28   (≥40 spans ahead → stop)
     *
     * Otherwise (q_span >= player AND ahead < 0x28): advance.
     *
     * NOTE: player slot's span_norm at byte offset 0x82 — read as 16-bit
     * sign-extended value (MOV BP, word ptr [0x004ab18a]). Equivalent to
     * actor[0]+0x82, i.e. the span_norm field. The pre-2026-05-14 port
     * used ACTOR_SPAN_RAW (+0x80) which is the wrong field for tracks
     * with junction remaps. */
    p0 = actor_ptr(0);
    (void)p0;
    /* [PORT: N-way] Pre-scan/respawn reference = the LEAD local player, so a
     * recycled traffic car respawns ahead of the front-runner. (1 player =>
     * slot 0 span, byte-identical to the original.) */
    player_span_norm = (int16_t)ai_player_span_lead();

    qp = g_traffic_queue_ptr;
    q_span = td5_read_le16s(qp);
    if (q_span != -1) {
        for (;;) {
            int sx = (int)q_span;
            int px = (int)player_span_norm;
            if (sx < px) break;           /* behind player → stop */
            if ((sx - px) >= 0x28) break; /* ≥40 ahead → stop */
            /* fall through: too close — advance */
            qp += 4;
            q_span = td5_read_le16s(qp);
            if (q_span == -1) break;
        }
    }

    /* [0x00435401-46] Scan slots 6..min(g_racerCount,12)-1; find slot with
     * MAX (player.span_norm - traffic[i].span_norm). The gate (>=0x29) is
     * applied AFTER the scan, NOT inside it. The disassembly explicitly
     * does NOT exclude slot 9 here — slot 9 is only protected later, IF
     * it was actually selected as the winner.
     *
     * The original's EBX walks the actor table at slot[6]+0x82 (raw byte
     * address 0x004ac6ba); the inner read is signed 16-bit (MOVSX). */
    loop_max = racer_count;
    if (loop_max > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
        loop_max = g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS;
    {
        int psn_loop;
        /* [PORT: N-way] scan the TRAFFIC region [g_traffic_slot_base, loop_max),
         * not the legacy 6..12, so big fields recycle their own traffic slots
         * (16..21) instead of mistakenly teleporting racer slots 6..15. */
        for (i = g_traffic_slot_base; i < loop_max; i++) {
            int16_t ts;
            int32_t dist;
            /* [PORT: N-way] Recycle reference = the TRAILING local player, so a
             * traffic car is only retired once EVERY player has passed it by
             * >40 spans (1 player => slot 0 span, byte-identical to original).
             * Orig reloaded slot 0's span_norm here each iteration (0x00435438). */
            psn_loop = ai_player_span_trailing();
            ts = ACTOR_I16(actor_ptr(i), ACTOR_SPAN_NORMALIZED);
            dist = psn_loop - (int)ts;
            if (dist > best_dist) {
                best_slot = i;
                best_dist = dist;
            }
        }
    }

    /* [0x00435448-4b] If best_dist <= 0x28: commit cursor and return.
     * Original writes g_activeTrafficBusCursor unconditionally just before bail. */
    if (best_dist <= 0x28) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x00435451-55] If queue cursor entry is -1 sentinel: commit cursor
     * and return. The pre-scan may have left q_span==-1 OR may have left a
     * valid entry; this check fires only when entry IS sentinel. */
    if (td5_read_le16s(qp) == -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043545b-67] Slot 9 protection: only blocks if slot 9 was chosen
     * AND a special-encounter handle is active. */
    if (best_slot == 9 && g_encounter_tracked_handle != -1) {
        g_traffic_queue_ptr = qp;
        return;
    }

    /* [0x0043546d-9c] uVar11 = best_slot * 0x11c. Write polarity and a
     * derived field at uVar11 + 0x4afbc8. The +0x68 dword (route_state
     * index 0x1A) holds best_slot*4+0x10 — purpose unclear; copied verbatim.
     * The polarity field is at route_state base+0xFC (dword index 0x3F)
     * per `gActorRouteDirectionPolarity = 0x004afc5c`. */
    rs = route_state(best_slot);
    rs_bytes = (uint8_t *)rs;
    /* Refresh q_span and qp[2..3] from cursor (may have been advanced by
     * pre-scan, then validated above). */
    q_span   = td5_read_le16s(qp);
    q_flags  = qp[2];
    q_byte_3 = qp[3];

    /* dword at rs+0xFC = polarity (q_flags & 1) [0x0043548a]. The original
     * writes ONLY this dword (= gActorRouteDirectionPolarity, dword index 0x3F). */
    /* [traffic one-way] Force polarity 0 so this car follows the route in the
     * down-track direction; otherwise honour the authored TRAFFIC.BUS bit. */
    rs[RS_ROUTE_DIRECTION_POLARITY] =
        trf_oneway_traffic() ? 0 : (int32_t)(q_flags & 1u);
    (void)rs_bytes;

    /* dword at rs+0x68 = best_slot*4 + 0x10 [0x00435497]. Purpose unclear
     * (the original uses it as some kind of slot-derived index/offset).
     * Port verbatim. */
    *(int32_t *)(rs_bytes + 0x68) = best_slot * 4 + 0x10;

    /* [0x0043549d-af] Compare queue.byte_3 (raw sub_lane) vs strip_records
     * [q_span * 0x18 + 3] & 0xf (strip's lane_count nibble). */
    a = actor_ptr(best_slot);
    {
        const uint8_t *strip = (const uint8_t *)g_strip_span_base;
        uint8_t strip_lane_count;
        int     left_branch; /* 1 = LEFT (queue_byte > strip_lane), 0 = RIGHT */

        if (!strip) {
            /* Without strip records we cannot proceed; commit cursor and bail. */
            g_traffic_queue_ptr = qp + 4;
            return;
        }
        strip_lane_count = strip[(size_t)q_span * 0x18 + 3] & 0x0Fu;
        left_branch = ((unsigned)strip_lane_count < (unsigned)q_byte_3) ? 1 : 0;

        if (left_branch) {
            /* ====== LEFT branch (0x004354b5-0x004356cd, selector=0) ====== */
            int32_t yaw_pack;

            /* [0x004354bb] selector = 0 */
            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* [0x00435541-55] actor+0x80 = queue_span; actor+0x8c = queue_byte (raw) */
            ACTOR_I16(a, ACTOR_SPAN_RAW)       = q_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX)  = q_byte_3;

            /* [0x0043555b-67] InitActorTrackSegmentPlacement(actor+0x80, actor+0x1FC).
             * Byte-faithful port @ 0x00445F10 — writes world_pos[0..2] in 24.8 FP
             * AND seeds actor +0x84 (span_accum) + +0x86 (span_high) from span_raw,
             * AND clamps +0x8C (sub_lane) to (lane_count - 1) if out of range. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* [0x00435568-0x00435690] geometry-derive heading angle from strip,
             * then AngleFromVector12Full, then (angle+0x800)<<8 → actor+0x1f4.
             * For tighter fidelity, recycle_compute_heading_angle_12 walks the
             * span_type case dispatch; td5_track_compute_heading is the
             * port's existing equivalent path. We prefer the existing helper
             * here because it shares the same atan2 sign/wrap convention with
             * the rest of the port; the angle field +0x1f4 is then written
             * with the +0x800 bias as the original does. */
            td5_track_compute_heading((TD5_Actor *)a);

            /* [0x004356a6-bb] If polarity flag set, add 0x80000 to yaw (180°
             * flip = oncoming heading). Suppressed in one-way mode so the car
             * faces down-track like all other traffic. */
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if (!trf_oneway_traffic() && (q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x004356bb-c2] ResetVehicleActorState(actor) at 0x00405d70 —
             * port equivalent zeroes velocities & integrates pose. */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* [0x004356c7-c8] RefreshActorTrackProgressOffset(best_slot) at 0x004342e0 */
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x004356cd-ce] NormalizeActorTrackWrapState(actor) at 0x00443fb0 */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* [0x004356d3-de] Commit g_activeTrafficBusCursor += 4 (consume entry) */
            g_traffic_queue_ptr = qp + 4;

        } else {
            /* ====== RIGHT branch (0x004356ea-0x004358a6, selector=1) ====== */
            int32_t yaw_pack;
            int     remapped;

            /* [0x004356ea-f3] selector = 1 (RIGHT route / alt-segment) */
            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            /* [0x004354c5-0x0043550f + 0x004355e2-00] Inline jump-table scan:
             *   Find entry i where queue_span ∈ [entry.third, entry.third + (entry.high - entry.low) - 1].
             *   Result = queue_span + (entry.low - entry.third) (queue→main decode).
             *   No match: remapped = -1 (sentinel).
             *
             * The port's td5_track_apply_target_span_remap(lin_span,
             * is_canonical=0) implements the IDENTICAL math: entry[+0] read
             * as "remap_dst" (=low), entry[+2] as "remap_end_exc" (=high),
             * entry[+4] as "range_lo" (=third). Match condition:
             *   range_lo <= lin <= range_lo + (remap_end_exc - remap_dst) - 1
             * Result: lin + (remap_dst - range_lo)
             * — which is exactly queue_span + (low - third).
             *
             * Behavior contract: returns lin_span unchanged when no entry
             * matches; the original writes -1 in that case. We restore the
             * -1 sentinel by detecting "unchanged" outcome here. */
            remapped = td5_track_apply_target_span_remap((int)q_span, 0);
            if (remapped == (int)q_span) {
                /* No remap happened — either no jump entries or no match.
                 * Original stores -1 as the sentinel (OR EBP, 0xffffffff at
                 * 0x0043550f). Treat as -1. */
                remapped = -1;
            }

            /* [0x00435541-55] actor+0x80 = remapped; actor+0x8c = q_byte_3 - (strip_lane & 0xf). */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = (int16_t)remapped;
            ACTOR_I8(a, ACTOR_SUB_LANE_INDEX) =
                (int8_t)((int)(int8_t)q_byte_3 - (int)(strip_lane_count & 0xFu));

            /* [0x0043555b-67] InitActorTrackSegmentPlacement byte-faithful port.
             * Seeds actor +0x84/+0x86 (span_accum/high), clamps +0x8C if sub_lane
             * exceeds lane_nibble, writes world_pos[0..2] in 24.8 FP. */
            if (remapped >= 0) {
                td5_track_init_actor_segment_placement(
                    (int16_t *)(a + ACTOR_SPAN_RAW),
                    (int32_t *)(a + ACTOR_WORLD_POS_X));
            }

            /* [0x00435568-0x00435690 mirror] heading + yaw + polarity.
             * One-way mode suppresses the 180° oncoming flip. */
            td5_track_compute_heading((TD5_Actor *)a);
            yaw_pack = ACTOR_I32(a, ACTOR_YAW_ACCUM);
            if (!trf_oneway_traffic() && (q_flags & 1u) != 0u) {
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_pack + 0x80000;
            }

            /* [0x00435865-72] ResetVehicleActorState; RefreshActorTrackProgressOffset. */
            td5_physics_reset_actor_state((TD5_Actor *)a);
            td5_ai_seed_actor_track_progress_offset(best_slot);

            /* [0x00435877-78] ResolveActorSegmentBoundary(actor) at 0x00443ff0.
             * Real symbol from b99e36a (precise-00443FF0). Weakly-linked
             * forward decl; fall back to NormalizeWrap if unavailable. */
            if (&td5_track_resolve_actor_segment_boundary != NULL) {
                td5_track_resolve_actor_segment_boundary((TD5_Actor *)a);
            } else {
                /* FIXME[precise-00443FF0]: NormalizeWrap is the closest in-tree
                 * helper but does NOT handle the jump-table-decode case for
                 * out-of-ring raw spans (>ring_length). Once b99e36a is
                 * merged, the weak-linked RSB above takes over and the
                 * fallback dies. */
                td5_track_normalize_actor_wrap((TD5_Actor *)a);
            }

            /* [0x00435882-88] Commit g_activeTrafficBusCursor += 4. */
            g_traffic_queue_ptr = qp + 4;
        }
    }

    /* ====== LAB_0043588d: shared post-call state zero (both branches) ======
     * Original writes (all dwords unless marked):
     *   slot+0x314 = 0   (LONGITUDINAL_SPEED)
     *   slot+0x30c = 0   (STEERING_CMD)
     *   slot+0x379 = 0   (byte: VEHICLE_MODE)
     *   slot+0x1f0 = 0
     *   slot+0x1f8 = 0
     *   slot+0x1c0 = 0
     *   slot+0x1c4 = 0
     *   slot+0x1c8 = 0
     *   slot+0x1cc = 0   (LIN_VEL_X)
     *   route_state[+0x88] = 0   (RS_RECOVERY_STAGE at dword 0x22)
     *   slot+0x1d0 = 0
     *   route_state[+0xF0] = 0   (dword index 0x3C; port macro RS_SCRIPT_SPEED_PARAM
     *                              — but here it's used as a generic clear)
     *   slot+0x1d4 = 0   (LIN_VEL_Z)
     *   route_state[+0x7C] = -1  (RS_ENCOUNTER_HANDLE at dword 0x1F)
     *   word slot+0x338 = 0      (16-bit) — UNNAMED slot field
     */
    {
        char *aa = actor_ptr(best_slot);
        if (aa) {
            *(int32_t *)(aa + 0x314) = 0;
            *(int32_t *)(aa + 0x30C) = 0;
            *(int8_t  *)(aa + 0x379) = 0;
            *(int32_t *)(aa + 0x1F0) = 0;
            *(int32_t *)(aa + 0x1F8) = 0;
            *(int32_t *)(aa + 0x1C0) = 0;
            *(int32_t *)(aa + 0x1C4) = 0;
            *(int32_t *)(aa + 0x1C8) = 0;
            *(int32_t *)(aa + 0x1CC) = 0;
            *(int32_t *)(aa + 0x1D0) = 0;
            *(int32_t *)(aa + 0x1D4) = 0;
            *(int16_t *)(aa + 0x338) = 0;
        }
        /* route_state byte-direct writes match the original's
         * `[uVar11 + literal]` style. */
        *(int32_t *)((uint8_t *)rs + 0x88) = 0;
        *(int32_t *)((uint8_t *)rs + 0xF0) = 0;
        *(int32_t *)((uint8_t *)rs + 0x7C) = -1;
        /* Mirror into port's semantic macros for consistency. */
        rs[RS_RECOVERY_STAGE]   = 0;
        rs[RS_ENCOUNTER_HANDLE] = -1;
        /* Track recovery stage shadow used by other port code: clear too. */
        if (best_slot >= 0 && best_slot < TD5_MAX_TOTAL_ACTORS) {
            g_traffic_recovery_stage[best_slot] = 0;
        }
    }

    /* [#5 2026-06-20] A recycled slot is a fresh car: clear any (permanent) wreck
     * state so the reused slot drives again and the traffic pool is never starved
     * by totalled cars left behind the player. */
    if (best_slot >= 0 && best_slot < TD5_MAX_TOTAL_ACTORS) {
        g_actor_broken_down[best_slot]  = 0;
        g_actor_broken_ticks[best_slot] = 0;
    }

    TD5_LOG_I(LOG_TAG,
              "recycle_traffic: slot=%d player_span=%d q_span=%d q_lane=%u q_flags=0x%02X dist=%d",
              best_slot, (int)player_span_norm, (int)q_span, q_byte_3, q_flags, best_dist);
}

/**
 * InitializeTrafficActorsFromQueue: fill slots 6-11 from the head
 * of the TRAFFIC.BUS queue at race start.
 */
void td5_ai_set_traffic_queue(const uint8_t *data, int size) {
    /* Store the raw pointer — caller owns the backing buffer for the race
     * lifetime. `size` is informational; the queue is actually terminated
     * by a span == -1 sentinel record. */
    g_traffic_queue_base = data;
    g_traffic_queue_ptr  = data;
    TD5_LOG_I(LOG_TAG, "traffic queue bound: data=%p size=%d records=%d",
              (const void *)data, size, size / 4);
}

/* Byte-faithful port of InitializeTrafficActorsFromQueue @ 0x00435940.
 *
 * Original signature: void __stdcall InitializeTrafficActorsFromQueue(void)
 * Spawns ambient traffic actors into slots [6, min(g_racerCount, 12)).
 * Source: Ghidra disassembly listing 0x00435940-0x00435CB7 (271 instructions).
 *
 * [CONFIRMED @ 0x00435940] L5 audit 2026-05-18 (TD5_pool0 read-only) — byte-
 * faithful with original for all in-loop logic:
 *   - Outer gate `if (6 < g_racerCount)` matches 0x0043595D.
 *   - racer_cap = min(racer_count, 12) matches 0x0043597E.
 *   - Per-slot initial local_18=6, local_c=0x28 match 0x0043595B-70.
 *   - Branch condition (lane_count > queue.sub_lane) matches JA at 0x004359C1.
 *   - NORMAL path span_type switch (cases 1/2/5, 3/4, 6/7) byte-faithful.
 *   - REMAP path inlined ComputeActorTrackHeading switch byte-faithful.
 *   - Yaw computation `(angle + 0x800) << 8` matches 0x435C44 / 0x00435A77.
 *   - Polarity flip +0x80000 matches 0x435C58 / 0x00435A88.
 *   - Per-slot advance: qp+=4, slot++, local_c+=4 matches 0x00435C8B-CA5.
 *
 * [ARCH-DIVERGENCE] Three intentional port-side divergences kept post-audit.
 * See `reference_arch_init_traffic_actors_2026-05-18.md` for full rationale.
 *   D1. REMAP-miss returns input span vs orig -1 sentinel; port re-checks
 *       equality and substitutes -1 to match orig externally.
 *   D2. Trailing common ops (ACTOR_SLOT_INDEX mirror + RS_SLOT_INDEX +
 *       RS_ENCOUNTER_HANDLE = -1) are port-only bookkeeping; orig achieves
 *       the same via EBX address-mode addressing + ResetVehicleActorState
 *       side-effects.
 *   D3. RS_DIRECTION_POLARITY (legacy alias 0x25) dual-write removed
 *       2026-05-14 after confirming no original readers. Orig writes
 *       dword 0x3F = gActorRouteDirectionPolarity only.
 *
 * Per-slot algorithm:
 *   1. Read 4-byte queue record (span, polarity_byte, sub_lane).
 *   2. Compute strip-record lane_count = strip[queue.span].byte3 & 0xF.
 *   3. If queue.sub_lane < lane_count: NORMAL path (route selector = 0).
 *      Else: REMAP path (route selector = 1) — search junction-remap table
 *      and adjust span/sub_lane.
 *   4. Common: place actor, build yaw, ResetVehicleActorState,
 *      RefreshActorTrackProgressOffset (NORMAL) or ComputeTrackSpanProgress
 *      + ComputeSignedTrackOffset (REMAP), NormalizeActorTrackWrapState.
 *   5. Advance queue pointer 4 bytes.
 *
 * Divergences from existing port v0 (replaced 2026-05-14):
 *   - Adds ResetVehicleActorState + NormalizeActorTrackWrapState calls per
 *     original behavior (v0 skipped these).
 *   - Inline switch geometry matches ComputeActorTrackHeading @ 0x00435CE0
 *     (REMAP path) and an inline AngleFromVector12Full (NORMAL path).
 *   - REMAP path now seeds RS_TRACK_PROGRESS + RS_TRACK_OFFSET_BIAS via
 *     ComputeTrackSpanProgress + ComputeSignedTrackOffset (per original
 *     LAB_00435a96-bd).
 *   - REMAP path sets actor.span_normalized to ORIGINAL queue.span
 *     (post-call, per 0x00435ad1), not the remapped span.
 *   - REMAP path subtracts lane_count from sub_lane (per 0x00435a59 SUB).
 *
 * [ARCH-DIVERGENCE D3] direction-polarity-macro: the original writes polarity
 * at byte 0xFC of route_state slot (= dword index 0x3F =
 * gActorRouteDirectionPolarity). The port previously had
 * RS_DIRECTION_POLARITY = 0x25 (byte 0x94) — an established port-wide macro
 * mismatch. The dual-write defence was removed 2026-05-14 after confirming
 * no original readers of the 0x25 alias. See
 * reference_arch_init_traffic_actors_2026-05-18.md for catalogue.
 */
void td5_ai_init_traffic_actors(void) {
    int local_18;       /* slot counter (original local_18, starts at 6) */
    int local_c;        /* per-slot small constant (slot*4 + 0x10), starts at 0x28 */
    const uint8_t *qp;  /* g_activeTrafficBusCursor queue cursor */
    int racer_count;
    int racer_cap;

    /* 0x00435940-50: gate on g_racerCount > 6 */
    racer_count = g_active_actor_count;
    if (racer_count <= g_traffic_slot_base)
        return;

    /* [PORT ENHANCEMENT dynamic-traffic] GTA-style spawner replaces the queue
     * fill entirely (and works on tracks WITHOUT a TRAFFIC.BUS, e.g. TD6
     * conversions — hence this gate sits before the queue NULL check).
     * [Traffic] Dynamic=0 falls through to the byte-faithful queue path. */
    if (g_td5.ini.traffic_dynamic && g_td5.traffic_enabled) {
        td5_ai_traffic_dynamic_race_init();
        return;
    }

    qp = g_traffic_queue_ptr ? g_traffic_queue_ptr : g_traffic_queue_base;
    if (!qp) {
        TD5_LOG_W(LOG_TAG, "init_traffic_actors: g_traffic_queue_ptr is NULL");
        return;
    }

    /* [DIAG fix-1780404735 upstream-remap] track how often this race-start fill
     * runs and how far the queue cursor has advanced. The original calls
     * InitializeTrafficActorsFromQueue ONCE per race; if the port calls it
     * repeatedly the cursor walks deep into the queue into branch-entries that
     * miss the junction remap (-> span=-1 -> origin placement -> stuck). */
    {
        static int s_init_traffic_calls = 0;
        s_init_traffic_calls++;
        TD5_LOG_I(LOG_TAG,
                  "init_traffic_ENTER: call#=%d cursor_off=%ld span_count=%d",
                  s_init_traffic_calls,
                  (long)(g_traffic_queue_base ? (qp - g_traffic_queue_base) : -1),
                  td5_track_get_span_count());
    }

    /* 0x00435975-7e: cap iteration at min(racer_count, traffic_base+6 traffic).
     * [PORT] traffic base is g_traffic_slot_base (6 legacy / 16 big fields). */
    racer_cap = (racer_count > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
                ? (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) : racer_count;

    /* Initial loop state (0x4359 5b/68/63/70) — traffic actors start right
     * after the racer slots (g_traffic_slot_base: 6 legacy, 16 big fields). */
    local_18 = g_traffic_slot_base;             /* 6 legacy */
    local_c  = g_traffic_slot_base * 4 + 0x10;  /* 0x28 legacy */

    while (local_18 < racer_cap) {
        char *a = actor_ptr(local_18);
        int32_t *rs = route_state(local_18);
        int16_t queue_span;
        uint8_t queue_byte2;          /* polarity in bit 0 */
        uint8_t queue_byte3;          /* sub_lane */
        int polarity_bit;
        int lane_count;
        int orig_queue_span;          /* preserved for REMAP path post-call */

        queue_span  = td5_read_le16s(qp);
        queue_byte2 = qp[2];
        queue_byte3 = qp[3];
        orig_queue_span = (int)queue_span;
        /* [traffic one-way] Force forward heading; otherwise honour TRAFFIC.BUS
         * bit 0. Zeroing here propagates to the polarity store, both yaw-flip
         * sites, and the diagnostic log below. */
        polarity_bit = trf_oneway_traffic() ? 0 : ((int)queue_byte2 & 1);

        /* 0x435989-9d: common writes — polarity, local_c, RECOVERY_STAGE=0,
         * g_actor_traffic_recovery_stage=0. Polarity goes to dword 0x3F (= gActorRouteDirectionPolarity
         * @ 0x004afc5c). Prior port also wrote dword 0x25 (legacy macro) but
         * that field has no references in the original — the dual-write was
         * unnecessary and is now removed.
         * `local_c` (slot*4+0x10) writes the field at byte 0x68 within
         * route_state (dword 0x1A) — original semantics unknown; mirror raw. */
        rs[RS_ROUTE_DIRECTION_POLARITY] = polarity_bit;
        rs[0x1A] = local_c;                         /* DAT_004afbc8[slot] */
        rs[RS_RECOVERY_STAGE] = 0;                  /* dword 0x22 */
        rs[RS_SCRIPT_SPEED_PARAM] = 0;              /* dword 0x3C = g_actor_traffic_recovery_stage[slot] */
        g_traffic_recovery_stage[local_18] = 0;     /* port-side mirror */

        /* 0x004359a0-bf: compute lane_count for branching */
        {
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            int span_idx = (int)queue_span;
            uint8_t strip_byte3 = 0;

            if (strip_base && span_idx >= 0 && span_idx < g_strip_span_count) {
                strip_byte3 = ((const uint8_t *)&strip_base[span_idx])[3];
            }
            lane_count = (int)(strip_byte3 & 0x0F);
        }

        /* Branch on (lane_count > queue.sub_lane) — JA at 0x004359c1.
         * JA-taken (NORMAL path) means lane_count strictly greater. */
        if (lane_count > (int)queue_byte3) {
            /* ---------- NORMAL path (selector = 0, 0x00435b0d) ---------- */
            const TD5_StripSpan *strip_base =
                (const TD5_StripSpan *)g_strip_span_base;
            const TD5_StripVertex *vpool =
                (const TD5_StripVertex *)g_strip_vertex_base;
            const TD5_StripSpan *sp;
            const int16_t *psVar2;   /* left vertex base shorts */
            const int16_t *psVar3;   /* right vertex base shorts */
            int32_t local_10 = 0;    /* dx component */
            int32_t local_14 = 0;    /* dz component */
            int has_geom = 0;
            int angle_full;
            int32_t yaw_stored;

            rs[RS_ROUTE_TABLE_SELECTOR] = 0;

            /* 0x435b10-2b: actor.span_raw = queue.span; actor.sub_lane = queue.byte3 */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = queue_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = queue_byte3;

            /* 0x435b2e: InitActorTrackSegmentPlacement(&actor.span_raw, &actor.world_pos).
             * Byte-faithful port @ 0x00445F10. Seeds:
             *   actor+0x84 (span_accum) = span_raw
             *   actor+0x86 (span_high)  = span_raw
             *   actor+0x8C (sub_lane)   = clamp if >= lane_nibble
             *   actor+0x1FC/+0x200/+0x204 = 4-vertex barycenter (24.8 FP). */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x435b33-72: load strip[span] + first/second vertex pointers */
            sp = NULL;
            psVar2 = psVar3 = NULL;
            if (strip_base && vpool &&
                queue_span >= 0 && queue_span < g_strip_span_count) {
                sp = &strip_base[queue_span];
                psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];
                has_geom = 1;
            }

            /* Switch on strip type byte 0 (sp->span_type).
             * Original disassembles to a jump table at 0x00435cb8 covering
             * type-1 in [0,6]; type 0 or type > 7 falls through to the
             * default which leaves local_10/local_14 as their stack values.
             * For first iteration these are uninitialized. To produce
             * deterministic-port behavior on default-type spans, leave both
             * components at 0 (since stack residue is non-deterministic). */
            if (has_geom) {
                switch (sp->span_type) {
                case 1: case 2: case 5: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                       - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[5])
                            - (int32_t)psVar3[2];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 3: case 4: {
                    int32_t dx, dz_part;
                    dx = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                       - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz_part = ((int32_t)psVar2[5] - (int32_t)psVar3[8])
                            - (int32_t)psVar3[5];
                    {
                        int32_t dz = dz_part + (int32_t)psVar2[2];
                        local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    }
                    break;
                }
                case 6: case 7: {
                    int32_t dx, dz;
                    dx = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                       + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                    local_10 = (dx + ((dx >> 31) & 3)) >> 2;
                    dz = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                       - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                    local_14 = (dz + ((dz >> 31) & 3)) >> 2;
                    break;
                }
                default:
                    /* Original default: skip recompute, retain stack values.
                     * Port treats as 0/0 for determinism. */
                    break;
                }
            }

            /* 0x435c37-44: yaw = (AngleFromVector12Full(dx, dz) + 0x800) << 8 */
            angle_full = ai_angle_from_vector(local_10, local_14);
            yaw_stored = (angle_full + 0x800) << 8;
            ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

            /* 0x435c47-58: polarity flip — add 0x80000 */
            if (polarity_bit) {
                yaw_stored += 0x80000;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
            }

            /* 0x435c5c-60: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x435c69-6a: RefreshActorTrackProgressOffset(slot) */
            td5_ai_seed_actor_track_progress_offset(local_18);

            /* 0x435c6f-70: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);
        }
        else {
            /* ---------- REMAP path (selector = 1, 0x004359c7) ----------
             *
             * Original walks the junction-remap table at g_trackStripBlobAliasJunction+0x18,
             * matching queue.span into [range_lo, range_lo + (B - A) - 1].
             * On match: remapped = (A - C) + queue.span. On miss: -1.
             *
             * Port reuses td5_track_apply_target_span_remap which performs
             * the same walker but returns the input span (not -1) on miss.
             * [ARCH-DIVERGENCE D1] queue records whose span is outside every
             * junction range will have remapped_span = queue.span here, vs
             * -1 in original. Local re-check below substitutes -1 to match
             * orig externally. See
             * reference_arch_init_traffic_actors_2026-05-18.md. */
            int remapped_int;
            int16_t remapped_span;

            rs[RS_ROUTE_TABLE_SELECTOR] = 1;

            remapped_int = td5_track_apply_target_span_remap((int)queue_span, 0);

            if (remapped_int == (int)queue_span) {
                /* Junction-remap MISS. This fires on the 2nd (down-track) traffic
                 * fill when a queue record asks for a BRANCH lane
                 * (sub_lane >= lane_count) off a span that has no matching
                 * junction-remap entry on this track. The original sets a -1
                 * sentinel and then indexes strip[-1] (garbage); the port's
                 * range-guarded placement zeroed that to the world ORIGIN,
                 * stranding a visible, immovable traffic car off-track
                 * (user-reported "traffic standing still" / dead car at spawn).
                 *
                 * [SOURCE-PORT FIX S20 2026-06-05] Instead of stranding it, fall
                 * back to a NORMAL main-road placement at the queued span on the
                 * canonical (LEFT) route, so the car spawns on-track and cruises
                 * like the rest. Selector + route ptr are set to canonical so the
                 * Stage-2 heading check stays aligned (a branch-route car on the
                 * main road would otherwise trip the recovery brake). Strictly
                 * better than the origin-strand; only changes the miss case. */
                remapped_span = (int16_t)queue_span;
                rs[RS_ROUTE_TABLE_SELECTOR] = 0;
                if (g_route_tables[0])
                    rs[RS_ROUTE_TABLE_PTR] = (int32_t)(intptr_t)g_route_tables[0];
                TD5_LOG_I(LOG_TAG,
                          "init_remap_MISS: slot=%d queue_span=%d sub_lane=%d "
                          "lane_count=%d -> main-road fallback (was origin-strand)",
                          local_18, (int)queue_span, (int)queue_byte3, lane_count);
            } else {
                remapped_span = (int16_t)remapped_int;
            }

            /* 0x00435a2b-5b: actor.span_raw = remapped; actor.sub_lane =
             * queue.byte3 - lane_count. */
            ACTOR_I16(a, ACTOR_SPAN_RAW) = remapped_span;
            ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)((int)queue_byte3 - lane_count);

            /* 0x00435a5e: InitActorTrackSegmentPlacement(&span_raw, &world_pos).
             * Byte-faithful port @ 0x00445F10 — defensive guard: original has none
             * but our table-driven path can index into NULL span pool. The helper
             * itself early-returns with zeroed out_pos when track isn't bound. */
            td5_track_init_actor_segment_placement(
                (int16_t *)(a + ACTOR_SPAN_RAW),
                (int32_t *)(a + ACTOR_WORLD_POS_X));

            /* 0x00435a67: ComputeActorTrackHeading(actor) → 12-bit angle.
             * Inline the original switch + AngleFromVector12 to avoid the
             * extra +0x290 heading_normal write that td5_track_compute_heading
             * (port of 0x00434350) performs. */
            {
                const TD5_StripSpan *strip_base =
                    (const TD5_StripSpan *)g_strip_span_base;
                const TD5_StripVertex *vpool =
                    (const TD5_StripVertex *)g_strip_vertex_base;
                const TD5_StripSpan *sp = NULL;
                const int16_t *psVar2 = NULL;
                const int16_t *psVar3 = NULL;
                int32_t uVar9 = 0, param_1 = 0;
                int angle12;
                int32_t yaw_stored;
                int rs_span = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);

                if (strip_base && vpool &&
                    rs_span >= 0 && rs_span < g_strip_span_count) {
                    sp = &strip_base[rs_span];
                    psVar2 = (const int16_t *)&vpool[sp->left_vertex_index];
                    psVar3 = (const int16_t *)&vpool[sp->right_vertex_index];

                    switch (sp->span_type) {
                    case 1: case 2: case 5: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[3])
                                      - (int32_t)psVar3[0] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[5]
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 3: case 4: {
                        int32_t iVar6 = ((int32_t)psVar2[3] - (int32_t)psVar3[6])
                                      - (int32_t)psVar3[3] + (int32_t)psVar2[0];
                        int32_t iVar5 = (int32_t)psVar2[5] - (int32_t)psVar3[8]
                                      - (int32_t)psVar3[5] + (int32_t)psVar2[2];
                        iVar6 = iVar6 + ((iVar6 >> 31) & 3);
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = iVar6 >> 2;
                        break;
                    }
                    case 6: case 7: {
                        int32_t iVar6 = ((int32_t)psVar2[6] - (int32_t)psVar3[3])
                                      + (int32_t)psVar2[3] - (int32_t)psVar3[0];
                        int32_t iVar5 = ((int32_t)psVar2[8] - (int32_t)psVar3[5])
                                      - (int32_t)psVar3[2] + (int32_t)psVar2[5];
                        param_1 = (iVar5 + ((iVar5 >> 31) & 3)) >> 2;
                        uVar9 = (iVar6 + ((iVar6 >> 31) & 3)) >> 2;
                        break;
                    }
                    default:
                        /* default leaves uVar9 = param_1 = 0 (initial). */
                        break;
                    }
                }

                /* 0x00435dd1-44: quadrant-fold dispatch onto AngleFromVector12.
                 * Use the existing port helper ai_angle_from_vector for the
                 * full-circle equivalent. */
                angle12 = ai_angle_from_vector(uVar9, param_1);
                yaw_stored = (angle12 + 0x800) << 8;
                ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;

                if (polarity_bit) {
                    yaw_stored += 0x80000;
                    ACTOR_I32(a, ACTOR_YAW_ACCUM) = yaw_stored;
                }
            }

            /* 0x00435a91: ResetVehicleActorState(actor) */
            td5_physics_reset_actor_state((TD5_Actor *)a);

            /* 0x00435a96-b8: ComputeTrackSpanProgress + ComputeSignedTrackOffset
             * — seed RS_TRACK_PROGRESS and RS_TRACK_OFFSET_BIAS. The original
             * indexes the route table by actor.span_normalized (which at this
             * point is still zero/uninit); the port helper does likewise. */
            {
                int span_raw = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW);
                int span_norm = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
                int32_t pos[3];
                int64_t prog64;
                int32_t progress;
                int route_byte = 0;
                pos[0] = ACTOR_I32(a, ACTOR_WORLD_POS_X);
                pos[1] = *(int32_t *)(a + 0x200);
                pos[2] = ACTOR_I32(a, ACTOR_WORLD_POS_Z);
                prog64 = td5_track_compute_span_progress(span_raw, pos);
                progress = (int32_t)(uint32_t)(prog64 & 0xFFFFFFFF);
                rs[RS_TRACK_PROGRESS] = progress;

                {
                    const uint8_t *rb =
                        (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
                    if (rb && span_norm >= 0) {
                        route_byte = (int)rb[(size_t)(unsigned)span_norm * 3u];
                    }
                }
                rs[RS_TRACK_OFFSET_BIAS] =
                    td5_track_compute_signed_offset(span_raw, progress, route_byte);
            }

            /* 0x00435abe-c1: NormalizeActorTrackWrapState(actor) */
            td5_track_normalize_actor_wrap((TD5_Actor *)a);

            /* 0x00435ac6-d1: actor.span_normalized = ORIGINAL queue.span
             * (the un-remapped one), as final write. */
            *(int16_t *)(a + 0x082) = (int16_t)orig_queue_span;
        }

        /* ---------- common trailing ops (port-side bookkeeping) ----------
         * [ARCH-DIVERGENCE D2] The original 0x00435940 does NOT zero
         * steering_cmd / vehicle_mode / etc — those resets happen in
         * ResetVehicleActorState. We also mirror the slot index here for
         * AI dispatcher consumption (port-only). Orig achieves equivalent
         * state via EBX address-mode addressing. See
         * reference_arch_init_traffic_actors_2026-05-18.md. */
        ACTOR_U8(a, ACTOR_SLOT_INDEX) = (uint8_t)local_18;
        rs[RS_SLOT_INDEX] = local_18;
        rs[RS_ENCOUNTER_HANDLE] = -1;

        TD5_LOG_I(LOG_TAG,
                  "init_traffic: slot=%d span=%d remapped=%d lane=%d "
                  "polarity=%d sel=%d pos=(%d,%d,%d)",
                  local_18, orig_queue_span,
                  (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_RAW),
                  (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX),
                  polarity_bit, (int)rs[RS_ROUTE_TABLE_SELECTOR],
                  ACTOR_I32(a, ACTOR_WORLD_POS_X),
                  *(int32_t *)(a + 0x200),
                  ACTOR_I32(a, ACTOR_WORLD_POS_Z));

        /* 0x00435c8b-ca5: advance queue pointer, slot, local_c, racer_cap reload */
        qp += 4;
        local_18 += 1;
        local_c  += 4;

        /* 0x00435c85-78: reload g_racerCount each iteration (in case it
         * changed; mirror original even if our port doesn't mutate it
         * mid-loop). [PORT: N-way] re-cap to the traffic region end
         * (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) so big fields fill all
         * traffic slots, not just one — must match the top cap at 0x435975.
         * Legacy (base 6) => 12, byte-identical. */
        racer_count = g_active_actor_count;
        racer_cap = (racer_count > g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS)
                    ? (g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS) : racer_count;
    }

    g_traffic_queue_ptr = qp;
}

/**
 * UpdateTrafficRoutePlan: 7-stage FSM per traffic slot.
 *
 * Stage 1 (recycle):   Call RecycleTrafficActorFromQueue
 * Stage 2 (heading):   Heading misalignment check -> enter recovery if > 90 deg
 * Stage 3 (edge):      Edge-of-track / recovery bail-out -> brake and return
 * Stage 4 (normal):    Set constant speed (encounter_steering_override = 0x3C)
 * Stage 5 (target):    Compute next target span (accounting for direction polarity)
 * Stage 6 (steer):     Call UpdateActorSteeringBias with weight 0x8000
 * Stage 7 (yield):     FindNearestRoutePeer -> brake if closing on peer
 *
 * [precise-00435E80] Byte-faithful port from 0x00435E80 disassembly listing.
 *
 * [CONFIRMED @ 0x00435E80] L5 promotion sweep audit (2026-05-18).
 *   - Stages 1-7 enumerated above match orig FSM block layout
 *     (0x00435E80..0x00436A6F, 2142 bytes / 521 instructions).
 *   - Stage 2 ref_slot via rs[RS_SLOT_INDEX], not param_1 — CONFIRMED at
 *     0x00435EA6-0x00435FA8 EBX+0xD4 dispatch.
 *   - Stage 2 polarity reads RS_ROUTE_DIRECTION_POLARITY (dword 0x3F),
 *     not the prior incorrectly-aliased 0x25 — CONFIRMED at 0x00435EF4.
 *   - Stage 2 strict hdelta band (0x400, 0xC00) — CONFIRMED at
 *     0x00435F2B JLE / 0x00435F32 JGE pair.
 *   - Stage 2 special-encounter audio cleanup asymmetric — orig only
 *     clears g_encounter_tracked_handle on polarity==0 path.
 *   - Stage 5 target span calculation accounts for polarity sign flip.
 *   - Stage 7 yield brake — FindNearestRoutePeer dispatch verified.
 *
 * KNOWN DIVERGENCES: none documented (port commits document the four
 * shipped fixes vs prior approximate port).
 *
 * Effective level: L5 (byte-faithful end-to-end).
 *
 * Notable fixes vs prior approximate port:
 *   - Stage 2 reads heading/yaw_accum from the REFERENCE actor
 *     (actor_ptr(rs[RS_SLOT_INDEX])), not from the param_1 actor. Original
 *     dereferences EBX+0xD4 = rs[RS_SLOT_INDEX] before every per-actor read.
 *   - Stage 2 random-recovery seed reads actor[0].world_pos_z (DAT_004ab30c),
 *     not the local g_ai_frame_counter. Same source the original uses.
 *   - Stage 2 special-encounter cleanup: slot==9 + !wanted_mode in
 *     polarity==0 path stops audio AND clears handle. polarity!=0 path
 *     stops audio only — DOES NOT clear the handle. Original asymmetric.
 *   - Stage 3 bail-out checks rs[RS_SCRIPT_SPEED_PARAM] (g_actor_traffic_recovery_stage) and
 *     g_traffic_recovery_stage[slot] (DAT_004afbe8). Prior port duplicated
 *     the recovery check and never read the script-speed-param sentinel.
 *   - Stage 2 hdelta band check uses strict bounds (0x400, 0xC00) per the
 *     JLE/JGE pair at 0x435F2B/0x435F32, not >= and <=.
 */

/* ========================================================================
 * SOURCE-PORT ENHANCEMENT — Smart Traffic (S20, 2026-06-05)
 *
 * The original background traffic is deliberately simple: a flat 0x3c cruise
 * command [CONFIRMED @ 0x00435E80], a fully deterministic junction route
 * (a linear remap-table walk, NO RNG — confirmed _rand @ 0x00448157 is never
 * called from the route path), and no active lateral wall avoidance (traffic
 * slots >= 6 are even skipped by the lateral wall-contact synthesis, see
 * td5_track.c:1035). This block layers three OPTIONAL, individually-gateable
 * behaviours on top, applied ONLY to traffic slots (>= g_traffic_slot_base);
 * racing AI (slots 0..5, a different code path entirely) is untouched. With
 * [Traffic] TrafficSmart=0 the traffic is byte-faithful again.
 *
 *   1. WallAvoid     — bias an edge-lane car's lateral target toward the lane
 *                      interior so it stops scraping the rail.
 *   2. AvoidSlowLane — prefer the asphalt lane over an off-road shoulder lane
 *                      (the only per-lane attribute the track exposes is the
 *                      lane bitmask + surface_attribute; there is NO native
 *                      lane-speed field, so this is a clearly-marked heuristic).
 *   3. Lookahead     — when a car is close ahead in our lane, change to a clear
 *                      adjacent lane (and ease the hard brake) instead of just
 *                      ramming/braking. Falls back to the faithful TTC brake.
 *
 * All three operate purely on the traffic car's lateral target / chosen sub-lane
 * — they do NOT touch route_state, the route table, or the actor's yaw, so they
 * neither trip the heading-recovery brake nor perturb racer routing.
 *
 * REMOVED (was behaviour "RandomBranch"): assigning each traffic car a random
 * route table to vary branches at forks. TD5 traffic STRICTLY follows one
 * prescribed route, enforced by the Stage-2 heading-misalignment recovery brake
 * (UpdateTrafficRoutePlan @ 0x00435E80): re-pointing a live car at a different
 * route table desynced its yaw from the new route's heading bytes, tripping the
 * recovery brake -> the car froze in place until recycled (visible as "traffic
 * standing still"). It also fed the faithful racer peer-scan
 * td5_ai_find_offset_peer (which reads traffic route_state) and shifted racer
 * race-lines. A faithful, stable random-branch lever does not exist on top of
 * the route-following + recovery design, so the behaviour was dropped. */

/* Per-actor smart-traffic state (indexed by actor slot). */
int8_t   s_traffic_lane_bias[TD5_MAX_TOTAL_ACTORS];     /* situational lane offset (-1/0/+1), 1-tick latency */
static int      s_traffic_stuck_frames[TD5_MAX_TOTAL_ACTORS];  /* AntiFreeze: consecutive recovery-frozen ticks */
/* [#3 COLLISION-DEADLOCK ESCAPE 2026-06-19] */
static int      s_traffic_stall_frames[TD5_MAX_TOTAL_ACTORS];  /* consecutive near-stopped ticks */
static int16_t  s_traffic_escape_ticks[TD5_MAX_TOTAL_ACTORS];  /* remaining escape-burst ticks */
static int8_t   s_traffic_escape_side[TD5_MAX_TOTAL_ACTORS];   /* current escape steer side (-1/+1) */
/* [R3-10 ANTI-PILEUP STRENGTHEN 2026-06-19] Forward-PROGRESS jam detection +
 * persistent escape lane. The old escape only fired on |speed|<thresh for 45
 * ticks, so a car that keeps BUMPING (grinding forward a few units, never fully
 * stopping) never qualified and ground against its neighbour forever. These
 * track real along-track progress via ACTOR_SPAN_ACCUM (monotonic, remap-immune)
 * over a sliding window: too few spans of progress over the window == jammed,
 * regardless of instantaneous speed. After a burst, an escape LANE bias + a
 * cooldown make the car commit to a clear lane instead of re-braking into the
 * same peer and re-jamming. All deterministic (replicated span accum + tick
 * counters + slot parity, no rand) -> MP-lockstep safe. */
static int16_t  s_traffic_prog_accum[TD5_MAX_TOTAL_ACTORS];    /* span-accum at window start */
static int16_t  s_traffic_prog_window[TD5_MAX_TOTAL_ACTORS];   /* ticks elapsed in this window */
static int16_t  s_traffic_lowprog[TD5_MAX_TOTAL_ACTORS];       /* consecutive low-progress ticks */
static uint8_t  s_traffic_lowprog_latch[TD5_MAX_TOTAL_ACTORS]; /* 1 = last window showed no progress */
static int16_t  s_traffic_escape_cooldown[TD5_MAX_TOTAL_ACTORS];/* post-escape re-arm suppression */
/* s_traffic_escape_lane / s_traffic_escape_lane_ttl are declared near the top
 * globals (td5_ai_smart_traffic_lane, defined earlier, reads them). */

/* [AI UNSTICK 2026-06-26] Per-RACER collision-escape state. Racer slots
 * (0..g_traffic_slot_base) never had the jam-escape that traffic and cops do,
 * so two wedged racers — or a racer vs the player — pushed at full throttle
 * forever until an external force broke them apart. These mirror the traffic
 * escape exactly (no-progress detection -> short steer-away burst) and are
 * fully deterministic (span-accum + tick counters + slot parity, no rand) so
 * MP lockstep is preserved. */
static int16_t  s_racer_escape_ticks[TD5_MAX_TOTAL_ACTORS];    /* remaining brake-yield burst ticks */
static int8_t   s_racer_escape_side[TD5_MAX_TOTAL_ACTORS];     /* committed steer-around side (-1/+1) */
static int16_t  s_racer_escape_cooldown[TD5_MAX_TOTAL_ACTORS]; /* post-burst re-arm suppression */
static uint8_t  s_racer_has_moved[TD5_MAX_TOTAL_ACTORS];       /* gate: don't act at the start line */
static uint8_t  s_racer_stall_active[TD5_MAX_TOTAL_ACTORS];    /* 1 = sustained low-speed steer-around running */
static int16_t  s_racer_recover_ticks[TD5_MAX_TOTAL_ACTORS];   /* ticks back up to speed (release the steer) */
/* V2V grind detection: physics sets the flag each tick this racer touches a car;
 * the leaky load climbs while grinding and decays when clear (catches an
 * intermittent side-by-side rub that a speed/progress test misses). */
static uint8_t  s_racer_contact_flag[TD5_MAX_TOTAL_ACTORS];    /* set by physics, consumed by AI */
static int8_t   s_racer_contact_peer[TD5_MAX_TOTAL_ACTORS];    /* slot most recently collided with */
static int16_t  s_racer_contact_load[TD5_MAX_TOTAL_ACTORS];    /* leaky accumulator of recent contacts */

/* Per-race diagnostic counters — observe which behaviours actually trigger
 * (the rate-limited per-event logs can miss firings). Dumped periodically. */
static struct {
    int react_calls;          /* react_to_peer entered with a real peer       */
    int ease_diff_lane;       /* eased: nearest peer in a different lane       */
    int ease_lane_change;     /* eased: same-lane, changed to a clear lane     */
    int blocked_single_lane;  /* couldn't act: span had <=1 lane (brake stays) */
    int no_clear_lane;        /* same-lane peer but no clear adjacent (brake)   */
    int slow_lane_change;     /* choose_lane stepped off a slow/off-road lane  */
    int wall_nudges;          /* edge-lane targets nudged toward lane centre   */
} s_smart_stat;

/* Reset all smart-traffic per-actor state. Called once per race from
 * td5_ai_init_race_actor_runtime(). */
void td5_traffic_smart_reset(void) {
    for (int i = 0; i < TD5_MAX_TOTAL_ACTORS; i++) {
        s_traffic_lane_bias[i] = 0;
        s_traffic_stuck_frames[i] = 0;
        s_traffic_stall_frames[i] = 0;
        s_traffic_escape_ticks[i] = 0;
        s_traffic_escape_side[i]  = 0;
        s_traffic_prog_accum[i]   = 0;
        s_traffic_prog_window[i]  = 0;
        s_traffic_lowprog[i]      = 0;
        s_traffic_lowprog_latch[i] = 0;
        s_traffic_escape_cooldown[i] = 0;
        s_traffic_escape_lane[i]  = 0;
        s_traffic_escape_lane_ttl[i] = 0;
        /* [AI UNSTICK] racer collision-escape state (per race). */
        s_racer_escape_ticks[i]    = 0;
        s_racer_escape_side[i]     = 0;
        s_racer_escape_cooldown[i] = 0;
        s_racer_has_moved[i]       = 0;
        s_racer_stall_active[i]    = 0;
        s_racer_recover_ticks[i]   = 0;
        s_racer_contact_flag[i]    = 0;
        s_racer_contact_peer[i]    = -1;
        s_racer_contact_load[i]    = 0;
    }
    memset(&s_smart_stat, 0, sizeof(s_smart_stat));
}

/* AntiFreeze (source-port enhancement): the faithful traffic recovery-brake only
 * clears via RecycleTrafficActorFromQueue, which requires the player to advance
 * (~41 spans) — so a heading-misaligned traffic car freezes PERMANENTLY when the
 * player is parked/slow. This un-sticks a car that has been recovery-frozen for
 * `traffic_antifreeze_frames` consecutive ticks: clear the recovery flag and
 * re-align its heading to the road geometry (so Stage-2 below does not just
 * re-arm it), reset its velocity, and re-seed track progress. Independent of the
 * "smart" gate; default ON. Skips the active special-encounter cop (slot 9). */
static void traffic_smart_antifreeze(int slot, char *actor, int32_t *rs) {
    if (!g_td5.ini.traffic_antifreeze)
        return;
    if (slot < g_traffic_slot_base || slot >= TD5_MAX_TOTAL_ACTORS)
        return;
    if (slot == 9 && g_encounter_tracked_handle != -1) {
        s_traffic_stuck_frames[slot] = 0;
        return;
    }
    if (g_traffic_recovery_stage[slot] == 0) {
        s_traffic_stuck_frames[slot] = 0;   /* not frozen — reset the counter */
        return;
    }
    if (++s_traffic_stuck_frames[slot] < g_td5.ini.traffic_antifreeze_frames)
        return;                              /* not stuck long enough yet */

    /* Stuck too long — un-stick in place. */
    s_traffic_stuck_frames[slot] = 0;
    g_traffic_recovery_stage[slot] = 0;
    /* Re-align heading to the road at the current span (same helper the faithful
     * recycle uses), then flip 180 deg for reverse-polarity (oncoming) traffic,
     * matching the spawn/recycle convention. This keeps Stage-2's heading delta
     * small so it does not immediately re-arm the recovery brake. */
    td5_track_compute_heading((TD5_Actor *)actor);
    if ((rs[RS_ROUTE_DIRECTION_POLARITY] & 1) != 0)
        ACTOR_I32(actor, ACTOR_YAW_ACCUM) += 0x80000;
    td5_physics_reset_actor_state((TD5_Actor *)actor);
    td5_ai_seed_actor_track_progress_offset(slot);
    td5_track_normalize_actor_wrap((TD5_Actor *)actor);
    if ((g_ai_frame_counter % 30u) == 0u)
        TD5_LOG_I(LOG_TAG,
                  "traffic_antifreeze: slot=%d unstuck at span=%d (cleared recovery + realigned)",
                  slot, (int)ACTOR_I16(actor, ACTOR_SPAN_RAW));
}

/* [FIX 2026-07-07] Universal stuck backstop. AntiFreeze only clears the
 * recovery-latch (recovery_stage!=0) and collision-escape only the jam-speed
 * case; the diag showed the DOMINANT real stall is the low-speed wedge
 * (recovery_stage==0, ~zero speed) at specific track spots — neither net fired
 * once in an 80s run. This resets ANY traffic car that has made no span progress
 * for the caller's window (clear recovery, realign heading, reset velocity,
 * reseed progress) so it rejoins the flow. The caller gates on stuck-duration and
 * excludes an actively-chasing cop. TD5RE_TRAFFIC_UNSTICK=0 disables. */
static int traffic_unstick_enabled(void) {
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_TRAFFIC_UNSTICK"); }
    return s;
}
static void traffic_force_unstick(int slot, char *actor, int32_t *rs) {
    g_traffic_recovery_stage[slot] = 0;
    td5_track_compute_heading((TD5_Actor *)actor);
    if ((rs[RS_ROUTE_DIRECTION_POLARITY] & 1) != 0)
        ACTOR_I32(actor, ACTOR_YAW_ACCUM) += 0x80000;
    td5_physics_reset_actor_state((TD5_Actor *)actor);
    td5_ai_seed_actor_track_progress_offset(slot);
    td5_track_normalize_actor_wrap((TD5_Actor *)actor);
}

/* [#3 COLLISION-DEADLOCK ESCAPE 2026-06-19] Safety net for traffic cars that pile
 * into each other (or geometry) and cannot free themselves. When a traffic car
 * has been effectively stopped for ~1.5 s — and it is NOT a deliberate stop
 * (broken-down / parked-despawn / cop) — force a brief "steer-away" burst: brake
 * off, throttle on, and a hard steering command to one side, so it peels off
 * whatever it is jammed against. The steer side alternates on each engagement
 * (seeded by slot parity so adjacent jammed cars peel opposite ways), so a car
 * that jams a wall on one try escapes the other way next time. It drives the same
 * control fields normal traffic uses, applied as a POST-override at the end of the
 * per-slot update. Deterministic (speed + tick counters + slot parity, no rand)
 * -> MP-lockstep safe. The existing 45-tick stuck despawn stays as the last-resort
 * backstop. Knob TD5RE_TRAFFIC_ESCAPE (default on). */
static int traffic_escape_enabled(void) {
    static int s = -1;
    if (s < 0) {
        s = td5_env_flag_on("TD5RE_TRAFFIC_ESCAPE");
    }
    return s;
}

/* [R3-10] Anti-pileup tuning. JAM = low FORWARD progress (not just low speed):
 * fewer than JAM_MIN_SPANS of span-accum gained over a JAM_WINDOW-tick window
 * counts the window as "no progress"; LOWPROG_TICKS such ticks in a row arm the
 * escape. This catches the grind-bumpers the speed-only test missed. After the
 * burst the car commits to a clear lane for ESCAPE_LANE_TTL ticks and suppresses
 * re-arming for ESCAPE_COOLDOWN ticks so it does not instantly re-jam. */
#define TRAFFIC_JAM_WINDOW       30   /* ~1s progress-sampling window            */
#define TRAFFIC_JAM_MIN_SPANS     1   /* < this many spans/window == stalled     */
#define TRAFFIC_LOWPROG_TICKS    45   /* ~1.5s of no progress -> escape          */
#define TRAFFIC_ESCAPE_BURST     36   /* ~1.2s steer-away burst (was 24)         */
#define TRAFFIC_ESCAPE_COOLDOWN  30   /* ~1s after a burst before re-arming      */
#define TRAFFIC_ESCAPE_LANE_TTL  90   /* ~3s commit to the cleared lane          */

/* [R3-10] Pick the side (-1/+1) with more lateral room for `slot` to escape
 * toward: compare lane occupancy just ahead on each side via the existing
 * lane-clear scan. Falls back to slot-parity-alternation (deterministic) when
 * both sides are equally (un)clear, so adjacent jammed cars peel opposite ways.
 * Pure function of replicated lane/span state -> lockstep-safe. */
static int8_t traffic_escape_pick_side(int slot, char *actor) {
    int span  = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int lane  = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    int pol   = (int)(route_state(slot)[RS_ROUTE_DIRECTION_POLARITY] & 1);
    int left_clear  = traffic_lane_is_clear(slot, span, lane - 1, pol);
    int right_clear = traffic_lane_is_clear(slot, span, lane + 1, pol);
    if (left_clear && !right_clear) return (int8_t)-1;
    if (right_clear && !left_clear) return (int8_t)+1;
    /* Tie: alternate off the previous side (seeded by parity). */
    {
        int8_t prev = s_traffic_escape_side[slot];
        if (prev == 0) prev = (slot & 1) ? (int8_t)1 : (int8_t)-1;
        return (int8_t)-prev;
    }
}

void traffic_collision_escape(int slot) {
    if (!traffic_escape_enabled()) return;
    if (slot < g_traffic_slot_base || slot >= TD5_MAX_TOTAL_ACTORS) return;
    if (g_cop_is_cop[slot]) return;                         /* cops run their own driver */

    char *actor = actor_ptr(slot);

    if (g_actor_broken_down[slot] || td5_ai_traffic_dynamic_parked(slot)) {
        s_traffic_stall_frames[slot]    = 0;
        s_traffic_escape_ticks[slot]    = 0;
        s_traffic_lowprog[slot]         = 0;
        s_traffic_lowprog_latch[slot]   = 0;
        s_traffic_escape_cooldown[slot] = 0;
        s_traffic_escape_lane[slot]     = 0;
        s_traffic_escape_lane_ttl[slot] = 0;
        s_traffic_prog_window[slot]     = 0;
        return;                                             /* deliberate stop, not a jam */
    }

    /* Persistent escape-lane commit: count down the TTL. Both lane choosers
     * (td5_ai_smart_traffic_lane and traffic_smart_choose_lane) read
     * s_traffic_escape_lane/_ttl DIRECTLY to bias toward the cleared side while
     * this is live, so the car heads into a different lane instead of re-braking
     * into the same peer. (Not routed through s_traffic_lane_bias — the route
     * plan zeroes that every tick before the chooser runs.) */
    if (s_traffic_escape_lane_ttl[slot] > 0)
        s_traffic_escape_lane_ttl[slot]--;
    if (s_traffic_escape_cooldown[slot] > 0)
        s_traffic_escape_cooldown[slot]--;

    /* Active escape burst: override the route plan's brake with a forward
     * steer-away so the car physically peels off the obstacle. */
    if (s_traffic_escape_ticks[slot] > 0) {
        s_traffic_escape_ticks[slot]--;
        ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)     = 0;
        ACTOR_U8(actor,  ACTOR_THROTTLE_STATE) = 1;
        ACTOR_I32(actor, ACTOR_STEERING_CMD)   =
            (int32_t)s_traffic_escape_side[slot] * 0x12000;  /* near full-lock to one side */
        return;
    }

    /* Jam detection. TWO signals, either one arms the escape:
     *  (1) near-zero SPEED for LOWPROG_TICKS straight (the classic full-stop jam);
     *  (2) low forward PROGRESS — < JAM_MIN_SPANS of span-accum gained over a
     *      JAM_WINDOW-tick window — which catches a car that keeps BUMPING and
     *      grinding forward a hair at a time but never actually advances.
     * Both feed the same s_traffic_lowprog counter. */
    int32_t spd  = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    int32_t aspd = (spd < 0) ? -spd : spd;

    /* Sliding progress window on the monotonic span accumulator. The result is
     * LATCHED until the next window completes, so a grind-bumper (speed never
     * fully zero) is flagged jammed on EVERY tick of the window, not just on the
     * single tick the window closes — otherwise the per-tick lowprog counter
     * would barely advance and the escape would take ~45s to arm. */
    if (s_traffic_prog_window[slot] == 0)
        s_traffic_prog_accum[slot] = ACTOR_I16(actor, ACTOR_SPAN_ACCUM);
    if (++s_traffic_prog_window[slot] >= TRAFFIC_JAM_WINDOW) {
        int prog = (int)(int16_t)(ACTOR_I16(actor, ACTOR_SPAN_ACCUM) -
                                  s_traffic_prog_accum[slot]);
        if (prog < 0) prog = -prog;                          /* oncoming counts down */
        s_traffic_lowprog_latch[slot] = (prog < TRAFFIC_JAM_MIN_SPANS) ? 1 : 0;
        s_traffic_prog_window[slot] = 0;                     /* restart the window */
    }

    if (aspd < 0x1800 || s_traffic_lowprog_latch[slot]) {
        /* Don't re-arm immediately after a burst (let the steer-away land). */
        if (s_traffic_escape_cooldown[slot] == 0 &&
            ++s_traffic_lowprog[slot] >= TRAFFIC_LOWPROG_TICKS) {
            s_traffic_lowprog[slot]      = 0;
            s_traffic_escape_side[slot]  = traffic_escape_pick_side(slot, actor);
            s_traffic_escape_ticks[slot] = TRAFFIC_ESCAPE_BURST;
            /* Commit to the cleared lane after the burst so the route plan heads
             * there instead of re-braking into the same neighbour. */
            s_traffic_escape_lane[slot]     = s_traffic_escape_side[slot];
            s_traffic_escape_lane_ttl[slot] = TRAFFIC_ESCAPE_LANE_TTL;
            s_traffic_escape_cooldown[slot] = TRAFFIC_ESCAPE_BURST + TRAFFIC_ESCAPE_COOLDOWN;
            if ((g_ai_frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG,
                    "traffic_escape: slot=%d JAM (spd=%d prog_latch=%d) -> burst side=%d "
                    "lane_commit=%d", slot, aspd, (int)s_traffic_lowprog_latch[slot],
                    s_traffic_escape_side[slot], s_traffic_escape_lane[slot]);
        }
        /* legacy near-stopped counter (diagnostic; the actual stuck-despawn
         * backstop keys off s_traffic_stuck_frames in the AntiFreeze layer). */
        s_traffic_stall_frames[slot]++;
    } else {
        s_traffic_stall_frames[slot] = 0;                     /* moving fine */
        if (s_traffic_lowprog[slot] > 0) s_traffic_lowprog[slot]--;
    }
}

/* [dynamic-traffic] forward decls — defined in the dynamic-traffic module
 * below; used here so the SmartAI lane-changer never moves a car into a lane
 * of the opposite drive direction (lanes carry a direction when Dynamic=1). */
static int trf_dyn_lane_direction(int lane_count, int lane);
static int trf_dyn_lane_change_blocked(int slot, int lane_count, int cand_lane);

/* Scan active actors for one occupying `target_lane` near `self_span` (a car
 * beside or just ahead). Returns 1 if the lane is clear. */
static int traffic_lane_is_clear(int self_slot, int self_span,
                                 int target_lane, int polarity) {
    int n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int i = 0; i < n; i++) {
        char *a;
        int lane, span, diff;
        if (i == self_slot) continue;
        /* [PER-VIEWPORT TRAFFIC] only consider same-partition cars (+ own player):
         * a partition must not see another partition's twin when picking a clear
         * lane, or the placements desync. */
        if (td5_ai_traffic_pair_blocked(self_slot, i)) continue;
        if (!ai_peer_is_present(i)) continue;   /* [R3-8] skip phantom/absent slots */
        a = actor_ptr(i);
        if (!a) continue;
        lane = (int)ACTOR_U8(a, ACTOR_SUB_LANE_INDEX);
        if (lane != target_lane) continue;
        span = (int)ACTOR_I16(a, ACTOR_SPAN_RAW);
        diff = (polarity == 0) ? (span - self_span) : (self_span - span);
        if (diff >= -2 && diff <= 6) return 0;   /* occupied */
    }
    return 1;
}

/* ===================================================================
 * [AI UNSTICK 2026-06-26] Racer collision-escape
 *
 * Racers have the faithful LATERAL dodge (find_offset_peer -> steering offset)
 * but NO throttle/brake reaction to a blocking car — confirmed in the original
 * @0x00432D60, where AI throttle is field-position rubber-band only, with no
 * other-actor proximity read. So when the lateral dodge can't separate two
 * cars they wedge and grind at full throttle indefinitely until physics shoves
 * them apart. This gives racers the same jam-escape that traffic
 * (traffic_collision_escape) and cops (cop_ram_stuck) already have: detect no
 * forward progress, then a short steer-away burst that physically peels the car
 * off the obstacle (the codebase deliberately avoids AI reverse — see the cop
 * driver note at the ram path — so we steer-away like traffic instead).
 * Port-only, AI-racer-only, default ON (TD5RE_AI_UNSTICK=0 to disable).
 * =================================================================== */
#define RACER_ESCAPE_BURST     14    /* ~0.5s brake-yield burst (fast grind)      */
#define RACER_ESCAPE_COOLDOWN  30    /* ~1s after a brake burst before re-arming  */
#define RACER_STUCK_SPEED   0x1800   /* |speed| below this == slow / blocked       */
#define RACER_RECOVER_SPEED 0x3000   /* |speed| above this (held) == moving again  */
#define RACER_RECOVER_HOLD     8     /* ticks at speed before releasing the steer  */
#define RACER_STALL_STEER   0x12000  /* full-lock steer-around for a LOW-SPEED block
                                      * (safe at low speed; no high-speed yank)    */
#define RACER_ESCAPE_THROTTLE 0x50   /* throttle once a gap opens (not touching)    */
#define RACER_NUDGE_THROTTLE  0x18   /* barely-feathered throttle WHILE touching the
                                      * obstacle, so we redirect around it instead of
                                      * powering in and burying into the other car  */
#define RACER_CONTACT_GAIN     6     /* load added per tick a V2V contact happened */
#define RACER_CONTACT_DECAY    1     /* load shed per clear tick                   */
#define RACER_CONTACT_CAP     60     /* load ceiling (hysteresis)                  */
#define RACER_GRIND_LOAD      30     /* fast same-speed grind -> brake-yield        */
#define RACER_BLOCK_LOAD      12     /* slow + this much contact -> blocked, steer around */

/* Default ON; TD5RE_AI_UNSTICK=0 disables (restores the faithful no-escape AI). */
static int racer_unstick_enabled(void) {
    static int v = -1;
    if (v < 0) { v = td5_env_flag_on("TD5RE_AI_UNSTICK"); }
    return v;
}

/* Pick the side (-1 left / +1 right) with a clear adjacent lane to peel into.
 * Reuses the traffic lane-occupancy scan. When only one side is clear, take it.
 * On a tie (both clear / both blocked) COMMIT to the side already chosen for
 * this episode so the car decisively goes one way around instead of wiggling
 * back and forth; first time, pick a stable side by slot parity. */
static int8_t racer_escape_pick_side(int slot, char *actor) {
    int span_raw = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
    int lane     = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);
    int lc       = td5_track_get_span_lane_count((int)ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED));
    int can_lo, can_hi, lo_clear, hi_clear, center, interior;
    if (lc < 1) lc = 1;
    /* Only consider sides that stay ON the track: -1 steers toward the lower
     * lane index, +1 toward the higher (matches the traffic escape's sign). */
    can_lo = (lane - 1) >= 0;
    can_hi = (lane + 1) <= (lc - 1);
    if (!can_lo && !can_hi) return 0;                 /* single lane -> nowhere safe to go */
    lo_clear = can_lo && traffic_lane_is_clear(slot, span_raw, lane - 1, 0);
    hi_clear = can_hi && traffic_lane_is_clear(slot, span_raw, lane + 1, 0);
    if (lo_clear && !hi_clear) return (int8_t)-1;
    if (hi_clear && !lo_clear) return (int8_t)+1;
    /* Tie (both clear, or both blocked but on-track): steer toward the track
     * INTERIOR (toward the centre lane), never toward an off-track edge. This is
     * what keeps a blocked car from steering itself out of bounds. */
    center   = (lc - 1) / 2;
    interior = (lane > center) ? -1 : +1;
    if (interior < 0 && !can_lo) interior = +1;
    if (interior > 0 && !can_hi) interior = -1;
    return (int8_t)interior;
}

/* [AI UNSTICK] Physics V2V hook — records that `slot` touched `peer` this tick.
 * Cheap (two writes); range-guarded so physics can call it unconditionally. */
void td5_ai_note_v2v_contact(int slot, int peer) {
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return;
    s_racer_contact_flag[slot] = 1;
    s_racer_contact_peer[slot] = (int8_t)peer;
}

/* Per-tick racer unstick. Runs AFTER td5_ai_update_track_behavior so it can
 * override the throttle/steer the normal racing-line driver just set. Split by
 * SPEED: SLOW + touching a car -> sustained steer-around until moving again
 * (handles being blocked behind a stopped/slow car, or a dead wedge); FAST +
 * grinding a same-speed car -> the car behind brakes briefly to drop back. */
void racer_collision_escape(int slot) {
    char   *actor;
    int32_t spd, aspd;
    int     contacted, peer;

    if (!racer_unstick_enabled()) return;
    if (slot < 0 || slot >= g_traffic_slot_base) return;   /* AI RACER slots only */
    if (td5_ai_actor_is_cop(slot)) return;                 /* cops run their own driver */
    if (g_actor_broken_down[slot]) return;

    actor = actor_ptr(slot);
    if (!actor) return;

    /* Consume this tick's contact flag (set by physics last tick) into a leaky
     * load accumulator: climbs while repeatedly colliding, bleeds off when clear. */
    contacted = s_racer_contact_flag[slot];
    peer      = s_racer_contact_peer[slot];
    s_racer_contact_flag[slot] = 0;
    if (contacted) {
        s_racer_contact_load[slot] += RACER_CONTACT_GAIN;
        if (s_racer_contact_load[slot] > RACER_CONTACT_CAP)
            s_racer_contact_load[slot] = RACER_CONTACT_CAP;
    } else if (s_racer_contact_load[slot] > 0) {
        s_racer_contact_load[slot] -= RACER_CONTACT_DECAY;
        if (s_racer_contact_load[slot] < 0) s_racer_contact_load[slot] = 0;
    }

    spd  = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
    aspd = (spd < 0) ? -spd : spd;
    if (aspd > RACER_STUCK_SPEED) s_racer_has_moved[slot] = 1;

    if (s_racer_escape_cooldown[slot] > 0) s_racer_escape_cooldown[slot]--;

    /* ---- 1. SUSTAINED low-speed steer-around (blocked behind a car / wedge). ----
     * Blocked = has moved this race, is now slow, and is touching a car. At low
     * speed a firm steer is safe (no high-speed yank), and SUSTAINING it (rather
     * than a burst+cooldown that lets the car drift back into the obstacle) is
     * what actually gets it past a stopped/slow car instead of sitting there. */
    if (s_racer_has_moved[slot] && aspd < RACER_STUCK_SPEED &&
        s_racer_contact_load[slot] >= RACER_BLOCK_LOAD) {
        if (!s_racer_stall_active[slot]) {
            int8_t side = racer_escape_pick_side(slot, actor);
            if (side != 0) {              /* only escape if there's a safe on-track way around */
                s_racer_stall_active[slot] = 1;
                s_racer_escape_side[slot]  = side;
                TD5_LOG_I(LOG_TAG,
                    "racer_unstick: slot=%d BLOCKED peer=%d spd=%d -> steer-around side=%d",
                    slot, peer, (int)aspd, (int)side);
            }
        }
        s_racer_recover_ticks[slot] = 0;
    }
    if (s_racer_stall_active[slot]) {
        /* Re-pick the side EVERY tick so it tracks the (edge-aware, interior-
         * biased) safe direction as the car moves — never letting a committed
         * side walk it off the track. side==0 means we've reached an edge / run
         * out of safe room, so abandon the maneuver. */
        int8_t side = racer_escape_pick_side(slot, actor);
        if (side != 0) s_racer_escape_side[slot] = side;
        /* Release when back up to speed for a few ticks (got around), OR as soon
         * as we're no longer touching anything (pulled clear) — the latter stops
         * the car steering in a circle out in open space. */
        if (aspd > RACER_RECOVER_SPEED) s_racer_recover_ticks[slot]++;
        else                            s_racer_recover_ticks[slot] = 0;
        if (side == 0 ||
            s_racer_recover_ticks[slot] >= RACER_RECOVER_HOLD ||
            s_racer_contact_load[slot] == 0) {
            s_racer_stall_active[slot]  = 0;
            s_racer_recover_ticks[slot] = 0;
            s_racer_escape_side[slot]   = 0;
        } else {
            /* Steer toward the open side. While still TOUCHING the obstacle,
             * barely feather the throttle so we redirect AROUND it instead of
             * powering INTO it — driving hard into a stopped car built up deep
             * penetration that snapped the other car away ("teleport"). Once a
             * gap opens (not touching this tick) give it a bit more to slot in. */
            ACTOR_I32(actor, ACTOR_STEERING_CMD)    =
                (int32_t)s_racer_escape_side[slot] * RACER_STALL_STEER;
            ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)      = 0;
            ACTOR_U8(actor,  ACTOR_THROTTLE_STATE)  = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) =
                (int16_t)(contacted ? RACER_NUDGE_THROTTLE : RACER_ESCAPE_THROTTLE);
            return;
        }
    }

    /* ---- 2. FAST same-speed grind: the car behind brakes briefly to drop back. ----
     * Longitudinal only (no steering at racing speed -> no weird turns). */
    if (s_racer_escape_ticks[slot] > 0) {
        s_racer_escape_ticks[slot]--;
        ACTOR_U8(actor,  ACTOR_BRAKE_FLAG)      = 1;
        ACTOR_U8(actor,  ACTOR_THROTTLE_STATE)  = 1;
        ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00;   /* brake — drop back */
        return;
    }
    if (s_racer_escape_cooldown[slot] > 0) return;   /* settle before re-arming */

    /* Arm a brake-yield only for a fast, sustained grind against a car keeping
     * pace, and only for the car that's behind (lower span-accum; higher slot on
     * a tie). A slower/stopped peer is handled by section 1 (steer-around), never
     * by braking — so a car never sits and waits behind a stopped car. */
    if (aspd >= RACER_STUCK_SPEED &&
        s_racer_contact_load[slot] >= RACER_GRIND_LOAD &&
        peer >= 0 && peer < TD5_MAX_TOTAL_ACTORS) {
        char *pa = actor_ptr(peer);
        int32_t peer_spd = pa ? ACTOR_I32(pa, ACTOR_LONGITUDINAL_SPEED) : 0;
        int my_acc   = (int)(int16_t)ACTOR_I16(actor, ACTOR_SPAN_ACCUM);
        int peer_acc = pa ? (int)(int16_t)ACTOR_I16(pa, ACTOR_SPAN_ACCUM) : my_acc;
        int gap      = my_acc - peer_acc;
        int behind   = (gap < -1) ? 1 : (gap > 1) ? 0 : (slot > peer);
        if (peer_spd < 0) peer_spd = -peer_spd;
        if (peer_spd >= (aspd >> 1) && behind) {
            s_racer_escape_ticks[slot]    = RACER_ESCAPE_BURST;
            s_racer_escape_cooldown[slot] = RACER_ESCAPE_BURST + RACER_ESCAPE_COOLDOWN;
            TD5_LOG_I(LOG_TAG,
                "racer_unstick: slot=%d GRIND peer=%d load=%d spd=%d -> brake-yield",
                slot, peer, (int)s_racer_contact_load[slot], (int)aspd);
        }
        s_racer_contact_load[slot] = 0;
    }
}

/* (4) Lookahead: when a peer sits in our lane just ahead, pick a clear adjacent
 * lane to move into (preferring the interior side and never a slow lane), set
 * the 1-tick-latency lane bias consumed by traffic_smart_choose_lane next tick,
 * and return 1 to tell the caller to EASE the hard brake. Returns 0 (and clears
 * the bias) when disabled, single-lane, no same-lane peer, or no clear lane —
 * in which case the faithful TTC brake stands. */
static int traffic_smart_react_to_peer(int slot, int self_span, int self_sub_lane,
                                        int lane_count, int peer_slot, int polarity) {
    char *peer;
    int peer_lane, dir_first, dirs[2], k;
    if (!g_td5.ini.traffic_smart || !g_td5.ini.traffic_lookahead ||
        peer_slot < 0 || peer_slot >= TD5_MAX_TOTAL_ACTORS || peer_slot == slot) {
        s_traffic_lane_bias[slot] = 0;
        return 0;
    }
    peer = actor_ptr(peer_slot);
    if (!peer) { s_traffic_lane_bias[slot] = 0; return 0; }
    s_smart_stat.react_calls++;
    if (lane_count <= 1) {
        /* Single-lane span: nowhere to go around — the faithful brake is the
         * correct response. */
        s_smart_stat.blocked_single_lane++;
        s_traffic_lane_bias[slot] = 0;
        return 0;
    }
    peer_lane = (int)ACTOR_U8(peer, ACTOR_SUB_LANE_INDEX);
    if (peer_lane != self_sub_lane) {
        s_smart_stat.ease_diff_lane++;
        /* The nearest peer is in a DIFFERENT lane on a multi-lane span — not an
         * actual collision course. The faithful TTC brakes purely on span
         * distance (lane-agnostic) and would needlessly stop us behind a car in
         * the next lane. Ease (keep rolling, no lane change) so parallel-lane
         * traffic flows past instead of stacking up. */
        s_traffic_lane_bias[slot] = 0;
        if ((g_ai_frame_counter % 30u) == 0u)
            TD5_LOG_I(LOG_TAG,
                      "traffic_smart_avoid: slot=%d peer=%d in lane %d (self %d) "
                      "-> ease (different lane, no brake)",
                      slot, peer_slot, peer_lane, self_sub_lane);
        return 1;
    }

    dir_first = (self_sub_lane < lane_count / 2) ? +1 : -1;  /* prefer interior */
    dirs[0] = dir_first;
    dirs[1] = -dir_first;
    for (k = 0; k < 2; k++) {
        int cand = self_sub_lane + dirs[k];
        if (cand < 0 || cand >= lane_count) continue;
        if (trf_dyn_lane_change_blocked(slot, lane_count, cand))
            continue;   /* [dynamic-traffic] never swerve into an opposite-direction lane */
        if (g_td5.ini.traffic_avoid_slow_lane &&
            td5_track_surface_is_slow(td5_track_get_span_lane_surface(self_span, cand)))
            continue;
        if (traffic_lane_is_clear(slot, self_span, cand, polarity)) {
            s_traffic_lane_bias[slot] = (int8_t)dirs[k];
            s_smart_stat.ease_lane_change++;
            if ((g_ai_frame_counter % 30u) == 0u)
                TD5_LOG_I(LOG_TAG,
                          "traffic_smart_avoid: slot=%d peer=%d lane %d->%d (ease brake)",
                          slot, peer_slot, self_sub_lane, cand);
            return 1;
        }
    }
    s_smart_stat.no_clear_lane++;
    s_traffic_lane_bias[slot] = 0;
    return 0;
}

/* (2)+(3) Choose the target sub-lane: apply the situational lane bias (set last
 * tick by react_to_peer) then avoid-slow-lane. Returns base_sub_lane unchanged
 * when disabled or single-lane. Result is clamped to [0, lane_count-1]. */
static int traffic_smart_choose_lane(int slot, int target_span,
                                     int lane_count, int base_sub_lane) {
    int lane;
    if (!g_td5.ini.traffic_smart || lane_count <= 1)
        return base_sub_lane;
    lane = base_sub_lane;
    if (lane < 0) lane = 0;
    if (lane >= lane_count) lane = lane_count - 1;

    /* situational lane change (1-tick latency) */
    if (g_td5.ini.traffic_lookahead && s_traffic_lane_bias[slot] != 0) {
        int cand = lane + s_traffic_lane_bias[slot];
        if (cand >= 0 && cand < lane_count &&
            !trf_dyn_lane_change_blocked(slot, lane_count, cand))
            lane = cand;
    }

    /* [R3-10] Anti-pileup lane commit: while a recent jam-escape's lane bias is
     * live, step toward the cleared side so the car leaves the lane it kept
     * re-jamming in. Read directly (not via s_traffic_lane_bias, which the route
     * plan clears every tick). Deterministic (replicated escape state). */
    if (s_traffic_escape_lane_ttl[slot] > 0 && s_traffic_escape_lane[slot] != 0) {
        int cand = lane + (int)s_traffic_escape_lane[slot];
        if (cand >= 0 && cand < lane_count &&
            !trf_dyn_lane_change_blocked(slot, lane_count, cand))
            lane = cand;
    }

    /* avoid a slow / off-road lane: step to the nearest faster lane (that
     * also matches the car's drive direction when Dynamic=1) */
    if (g_td5.ini.traffic_avoid_slow_lane &&
        td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, lane))) {
        int toward_centre = (lane < lane_count / 2) ? +1 : -1;
        int picked = lane, d;
        for (d = 1; d < lane_count; d++) {
            int c1 = lane + toward_centre * d;
            int c2 = lane - toward_centre * d;
            if (c1 >= 0 && c1 < lane_count &&
                !trf_dyn_lane_change_blocked(slot, lane_count, c1) &&
                !td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, c1))) {
                picked = c1; break;
            }
            if (c2 >= 0 && c2 < lane_count &&
                !trf_dyn_lane_change_blocked(slot, lane_count, c2) &&
                !td5_track_surface_is_slow(td5_track_get_span_lane_surface(target_span, c2))) {
                picked = c2; break;
            }
        }
        if (picked != lane) s_smart_stat.slow_lane_change++;
        lane = picked;
    }

    if (lane != base_sub_lane && (g_ai_frame_counter % 60u) == 0u)
        TD5_LOG_I(LOG_TAG, "traffic_smart_lane: slot=%d base=%d -> %d (lanes=%d span=%d)",
                  slot, base_sub_lane, lane, lane_count, target_span);
    return lane;
}

/* (2) WallAvoid: blend an edge-lane car's lateral target (24.8 FP) toward the
 * interior neighbour lane so it stops scraping the rail. Interior lanes are
 * left exactly centred (preserves lane separation). No-op when disabled or
 * single-lane. */
static void traffic_smart_wall_nudge(int target_span, int target_sub_lane,
                                     int lane_count, int32_t *tx,
                                     int32_t *ty, int32_t *tz) {
    int bias, inward, ix = 0, iy = 0, iz = 0;
    if (!g_td5.ini.traffic_smart || !g_td5.ini.traffic_wall_avoid || lane_count <= 1)
        return;
    bias = g_td5.ini.traffic_wall_avoid_bias;
    if (bias <= 0) return;
    if (target_sub_lane <= 0)
        inward = 1;
    else if (target_sub_lane >= lane_count - 1)
        inward = lane_count - 2;
    else
        return;   /* interior lane already away from both rails */
    if (!td5_track_get_span_lane_world(target_span, inward, &ix, &iy, &iz))
        return;
    {
        int32_t dx_move = (int32_t)(((int64_t)(ix - *tx) * bias) >> 8);
        int32_t dz_move = (int32_t)(((int64_t)(iz - *tz) * bias) >> 8);
        *tx += dx_move;
        *ty += (int32_t)(((int64_t)(iy - *ty) * bias) >> 8);
        *tz += dz_move;
        s_smart_stat.wall_nudges++;
        /* Rate-limited so the before/after trace can count edge-lane nudges. */
        if ((g_ai_frame_counter % 60u) == 0u)
            TD5_LOG_I(LOG_TAG,
                      "traffic_smart_wallnudge: span=%d edge_lane=%d->interior=%d "
                      "moved(dx=%d dz=%d) [24.8 FP]",
                      target_span, target_sub_lane, inward, dx_move, dz_move);
    }
}

/* ========================================================================
 * [PORT ENHANCEMENT 2026-06-11] Dynamic (GTA-style) ambient traffic
 *
 * Replaces the TRAFFIC.BUS fixed-spawn queue with a distance-driven
 * spawn/despawn state machine, gated by [Traffic] Dynamic (default 1).
 * Dynamic=0 leaves the byte-faithful queue init (InitializeTrafficActors-
 * FromQueue @ 0x00435940) + recycle (RecycleTrafficActorFromQueue @
 * 0x004353B0) paths completely untouched.
 *
 * Per traffic slot state machine:
 *   INACTIVE  — parked at its last pose; skipped by AI route plan, traffic
 *               physics, V2V broadphase, render (body/shadow/wheels/brakes),
 *               minimap dot and engine audio.
 *   FADE_IN   — just (re)placed on the road; render alpha ramps 0→255 over
 *               [Traffic] FadeTicks. Fully simulated from tick 0.
 *   ACTIVE    — normal traffic.
 *   FADE_OUT  — every local player is > DespawnDistance spans away (or the
 *               car is recovery-latched out of sight); alpha ramps 255→0,
 *               then the slot parks (INACTIVE).
 *
 * Spawning: every SpawnPeriod-ish ticks (volume-scaled + jittered) one
 * INACTIVE slot is placed at a random span [SpawnAheadMin..SpawnAheadMax]
 * ahead of the LEAD local player, on a random CLEAR, NON-SLOW lane
 * (td5_track_surface_is_slow — dirt/gravel/alternate-surface shoulders are
 * never picked). Direction polarity (oncoming bit, +0x80000 heading like
 * the queue's flags bit0 [CONFIRMED @ 0x00435786]) is rolled against the
 * track's own TRAFFIC.BUS oncoming ratio so per-track direction character
 * is preserved. Placement reuses the exact recycle placement chain
 * (selector 0 / main ring only).
 *
 * Multiplayer: despawn requires EVERY local player to be out of range and
 * spawn validates the window against ALL players — unlike the original
 * recycle which only ever reads slot 0's span (word @ 0x004AB18A
 * [CONFIRMED]). Netplay is unaffected (traffic is forced off there).
 *
 * Police: traffic slot 9 is the speeding-pursuit cop donor
 * (UpdateSpecialTrafficEncounter @ 0x00434DA0). The spawner PREFERS slot 9
 * when picking a slot to (re)spawn so the cop-capable car is usually on the
 * road, and a slot-9 car is never despawned while the encounter handle is
 * live (mirrors the recycle's slot-9 guard @ 0x0043545B).
 *
 * Determinism: a private LCG seeded from the track index — deliberately NOT
 * the CRT rand(), whose sequence is consumed at render rate by the audio
 * mix (td5_sound.c traffic pitch) and must stay untouched for the fixed-
 * seed A/B trace harness.
 * ======================================================================== */

enum {
    TRF_DYN_INACTIVE = 0,
    TRF_DYN_FADE_IN  = 1,
    TRF_DYN_ACTIVE   = 2,
    TRF_DYN_FADE_OUT = 3
};

static uint8_t  s_trf_dyn_state[TD5_MAX_TOTAL_ACTORS];
static int16_t  s_trf_dyn_alpha[TD5_MAX_TOTAL_ACTORS];   /* 0..255 draw alpha */
static int      s_trf_dyn_cooldown;                       /* ticks to next spawn attempt */
static uint32_t s_trf_dyn_rng;                            /* private LCG state */
static int      s_trf_dyn_oncoming_pct;                   /* 0..100 from TRAFFIC.BUS mix (diagnostic) */
static int      s_trf_dyn_seeded;                         /* race_init ran for this race */

/* ---- [PER-VIEWPORT TRAFFIC 2026-06-22] ----------------------------------------
 * Split-screen TIME TRIAL (removed 2026-07-04): each viewport got its OWN traffic
 * partition (a disjoint sub-range of the traffic slots), spawned with an IDENTICAL
 * RNG seed but scoped to its own player, so the sets landed in matching places yet
 * were simulated / collided / rendered independently — one player perturbing a
 * traffic car never showed in another player's pane. That was the only mode that
 * ever set s_trf_per_vp, so trf_per_viewport_setup() now always leaves it at 0 and
 * every path below permanently takes the original shared-spawner branch. */
int             g_traffic_slot_owner_vp[TD5_MAX_TOTAL_ACTORS]; /* viewport owner; -1 = shared */
static int      s_trf_per_vp     = 0;     /* per-viewport traffic active this race      */
static int      s_trf_vp_count   = 1;     /* number of viewports partitioned across     */
static int      s_trf_vp_k       = TD5_MAX_TRAFFIC_SLOTS;  /* traffic slots per viewport */
static uint32_t s_trf_dyn_rng_vp[TD5_MAX_VIEWPORTS];      /* per-viewport spawn RNG      */
static int      s_trf_dyn_cooldown_vp[TD5_MAX_VIEWPORTS]; /* per-viewport spawn cooldown */
/* The partition currently being spawned (>=0) — the existing-traffic density gate
 * counts ONLY this partition's cars so each partition's placement is identical
 * (a partition must not "see" another partition's twins when deciding clumping).
 * -1 = count all traffic (default / non-per-vp). */
static int      s_trf_spawn_partition = -1;


/* ===== SECTION: dynamic ambient traffic (trf_dyn_*) spawner + lane logic ===== */

/* Viewport index whose player occupies racer `slot` (-1 if none). Split-screen is
 * an identity map (vp == slot) but resolve it via the game table to be safe. */
static int trf_viewport_of_slot(int slot)
{
    int vc = s_trf_vp_count;   /* the partition count (== num_human_players) */
    if (vc > TD5_MAX_VIEWPORTS) vc = TD5_MAX_VIEWPORTS;
    for (int v = 0; v < vc; v++)
        if (td5_game_get_player_slot(v) == slot) return v;
    return -1;
}

int td5_ai_traffic_per_viewport_active(void) { return s_trf_per_vp; }

int td5_ai_traffic_slot_owner_vp(int slot)
{
    if (!s_trf_per_vp || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return -1;
    return g_traffic_slot_owner_vp[slot];
}

/* 1 if a collision between these two slots must be SUPPRESSED for per-viewport
 * isolation: a traffic car only interacts with its owning viewport's player, and
 * two traffic cars from DIFFERENT partitions (which sit at matching positions)
 * must not collide with each other. Returns 0 (allow) whenever per-vp is off. */
int td5_ai_traffic_pair_blocked(int slot_a, int slot_b)
{
    int base, a_traf, b_traf, owner_a, owner_b, vp;
    if (!s_trf_per_vp) return 0;
    base    = g_traffic_slot_base;
    a_traf  = (slot_a >= base);
    b_traf  = (slot_b >= base);
    owner_a = (a_traf && slot_a >= 0 && slot_a < TD5_MAX_TOTAL_ACTORS) ? g_traffic_slot_owner_vp[slot_a] : -1;
    owner_b = (b_traf && slot_b >= 0 && slot_b < TD5_MAX_TOTAL_ACTORS) ? g_traffic_slot_owner_vp[slot_b] : -1;
    if (a_traf && b_traf)
        return (owner_a >= 0 && owner_b >= 0 && owner_a != owner_b);
    if (a_traf) { vp = trf_viewport_of_slot(slot_b); return (owner_a >= 0 && vp >= 0 && vp != owner_a); }
    if (b_traf) { vp = trf_viewport_of_slot(slot_a); return (owner_b >= 0 && vp >= 0 && vp != owner_b); }
    return 0;
}

/* Reset per-viewport traffic partitioning state. [MP TIME TRIAL removed
 * 2026-07-04] This subsystem existed solely to give split-screen TIME TRIAL
 * ghost semantics an isolated traffic partition per player; with that mode
 * gone there is no remaining caller that can ever enable it, so this now only
 * resets to the inactive defaults and every gated call site below permanently
 * takes the original shared-traffic path (s_trf_per_vp stays 0). */
static void trf_per_viewport_setup(void)
{
    int slot;
    for (slot = 0; slot < TD5_MAX_TOTAL_ACTORS; slot++)
        g_traffic_slot_owner_vp[slot] = -1;
    s_trf_per_vp   = 0;
    s_trf_vp_count = 1;
    s_trf_vp_k     = TD5_MAX_TRAFFIC_SLOTS;
}

/* Per-track lane→direction map learned from the authored TRAFFIC.BUS records:
 * each 4-byte record couples (span, lane, polarity), and the span's strip
 * lane-count nibble keys which road layout the lane index refers to. Indexed
 * [lane_count][lane]: -1 = no authored data, 0 = forward, 1 = oncoming
 * (majority vote). s_trf_dyn_oncoming_left summarizes which HALF of the road
 * carries oncoming traffic on this track (fallback for unseen combos and for
 * tracks without a TRAFFIC.BUS, e.g. TD6 conversions). */
#define TRF_DYN_MAX_LANES 15
static int8_t   s_trf_dyn_lane_dir[TRF_DYN_MAX_LANES + 1][TRF_DYN_MAX_LANES];
static int      s_trf_dyn_oncoming_left;                  /* 1 = oncoming on left half */

/* Direction a freshly spawned car should drive in `lane` of a
 * `lane_count`-lane span: authored data first, side-half heuristic second.
 * Single-lane spans always drive forward. */
/* [TRAFFIC BATTLE 2026-06-28] Force ALL traffic to drive ONCOMING (toward the
 * players) — head-on demolition fodder. Default ON in battle mode; the
 * TD5RE_TRAFFIC_ONCOMING knob overrides either way (1 = on, 0 = off) so it can be
 * forced on/off in any mode for testing. Reads process config + replicated mode,
 * so it is lockstep-deterministic. */
static int trf_force_oncoming(void)
{
    const char *e = getenv("TD5RE_TRAFFIC_ONCOMING");
    if (e && e[0]) return atoi(e) != 0;
    if (td5_game_battle_mode_active()) return 1;
    /* [DRAG RACE 2026-06-30] Oncoming traffic when the drag TRAFFIC option is on. */
    if (td5_game_drag_mp_active() && g_td5.mp_mode_config.drag_traffic) return 1;
    return 0;
}

static int trf_dyn_lane_direction(int lane_count, int lane)
{
    /* [TRAFFIC BATTLE] Every lane heads oncoming — takes precedence over the
     * one-way (all-forward) default so battle traffic comes at the players. */
    if (trf_force_oncoming())
        return 1;
    /* [traffic one-way] Every lane heads down-track — no oncoming lanes. This is
     * the single chokepoint for the dynamic spawner: it sets both the spawn
     * polarity and trf_dyn_lane_change_blocked()'s direction test. */
    if (trf_oneway_traffic())
        return 0;
    if (lane_count >= 1 && lane_count <= TRF_DYN_MAX_LANES &&
        lane >= 0 && lane < lane_count &&
        s_trf_dyn_lane_dir[lane_count][lane] >= 0)
        return (int)s_trf_dyn_lane_dir[lane_count][lane];
    if (lane_count <= 1)
        return 0;
    {
        int left_half = (lane * 2 < lane_count);
        return s_trf_dyn_oncoming_left ? left_half : !left_half;
    }
}

/* Span of the 1ST-PLACE RACE CAR — humans AND AI racers (user rule: traffic
 * appears ahead of whoever leads the race; a last-place human otherwise sees
 * traffic materializing around themselves on the minimap). Skips decoration/
 * absent slots (state 3) and slots with no car bound. Falls back to the lead
 * human when no racer qualifies (e.g. degenerate solo states). Note: on
 * circuits the comparison is ring-relative like every other span comparison
 * in this module, so right at the lap seam the anchor may briefly trail. */
static int trf_dyn_race_span_lead(void)
{
    int n = g_traffic_slot_base;
    int best = -0x7FFFFFFF;
    if (s_trf_scope_slot >= 0)   /* per-viewport: anchor on this viewport's player */
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (n > g_active_actor_count) n = g_active_actor_count;
    if (n > TD5_MAX_TOTAL_ACTORS) n = TD5_MAX_TOTAL_ACTORS;
    for (int s = 0; s < n; s++) {
        char *a = actor_ptr(s);
        void *cdef = NULL;
        int sp;
        if (!a) continue;
        if (g_slot_state[s] == 3) continue;          /* decoration / absent */
        memcpy(&cdef, a + 0x1B8, sizeof(cdef));      /* car_definition_ptr */
        if (!cdef) continue;
        sp = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
        if (sp > best) best = sp;
    }
    return (best == -0x7FFFFFFF) ? ai_player_span_lead() : best;
}

/* 1 when moving `slot` into `cand_lane` would put it in a lane of the
 * OPPOSITE drive direction. Inert (0) in faithful mode, where lanes carry
 * no direction and the SmartAI lane-changer keeps its original freedom. */
static int trf_dyn_lane_change_blocked(int slot, int lane_count, int cand_lane)
{
    int32_t *rs;
    if (!td5_ai_traffic_dynamic_active()) return 0;
    /* [DRAG RACE 2026-06-30] Drag traffic stays in its spawn lane — block every
     * candidate lane change (covers the choose-lane + react-to-peer paths). */
    if (td5_game_drag_mp_active()) return 1;
    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    rs = route_state(slot);
    if (!rs) return 0;
    return trf_dyn_lane_direction(lane_count, cand_lane) !=
           (rs[RS_ROUTE_DIRECTION_POLARITY] & 1);
}

static uint32_t trf_dyn_rand(void)
{
    /* Numerical Recipes LCG; top bits are the usable ones. */
    s_trf_dyn_rng = s_trf_dyn_rng * 1664525u + 1013904223u;
    return s_trf_dyn_rng >> 8;
}

int td5_ai_traffic_dynamic_active(void)
{
    return g_td5.ini.traffic_dynamic && g_td5.traffic_enabled && s_trf_dyn_seeded;
}

int td5_ai_traffic_dynamic_parked(int slot)
{
    if (!td5_ai_traffic_dynamic_active()) return 0;
    if (slot < g_traffic_slot_base ||
        slot >= g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS ||
        slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return s_trf_dyn_state[slot] == TRF_DYN_INACTIVE;
}

int td5_ai_traffic_get_draw_alpha(int slot)
{
    if (!td5_ai_traffic_dynamic_active()) return 255;
    if (slot < g_traffic_slot_base ||
        slot >= g_traffic_slot_base + TD5_MAX_TRAFFIC_SLOTS ||
        slot >= TD5_MAX_TOTAL_ACTORS) return 255;
    return (int)s_trf_dyn_alpha[slot];
}

/* [task#12 2026-06-15] A/B knob for the consistent-density fixes (proximity
 * recycle + per-volume retune + VERY HIGH tier). TD5RE_TRAFFIC_DENSITY=0 restores
 * the prior behaviour (corridor-only despawn, old caps/periods); default on.
 * Logged once on first read. Independent of TD5RE_TRAFFIC_BRANCHES / _SPAWN_DIST
 * (those keep their own knobs and stay in effect). */
static int trf_dyn_density_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        s = td5_env_flag_on("TD5RE_TRAFFIC_DENSITY");
        TD5_LOG_I(LOG_TAG, "traffic_density knob: TD5RE_TRAFFIC_DENSITY=%d "
                  "(proximity recycle + retune + VERY HIGH %s)",
                  s, s ? "ON" : "OFF");
    }
    return s;
}

/* Resolved traffic volume 0..4 (Off/Low/Medium/High/Very-High). Code paths that
 * only set the legacy boolean (cup save restore, forced-on game types) leave
 * traffic_volume at 0 — treat enabled+no-volume as High (the classic 6-car
 * density). The frontend selector currently masks the option to 0..3; value 4
 * (VERY HIGH) is wired here and in main.c's clamp so the frontend can be extended
 * to emit it without further engine changes. */
static int trf_dyn_volume(void)
{
    int v = g_td5.traffic_volume;
    if (v <= 0) v = g_td5.traffic_enabled ? 3 : 0;
    if (v > 4) v = 4;
    return v;
}

/* Concurrency cap from the track-select Traffic volume row.
 *   Off=0  Low=2  Medium=4  High=6  Very-High=16
 * The hard ceiling is TD5_MAX_TRAFFIC_SLOTS (16 — see td5_types.h). HIGH keeps the
 * faithful 6; VERY HIGH fills all 16 traffic slots (the extra cars reuse each
 * track's 6 car models + 6 skin pages — normal for traffic) and packs them near
 * the player via the tighter spawn window / faster respawn below. */
static int trf_dyn_cap(void)
{
    static const int k_cap[5] = { 0, 2, 4, 6, 16 };
    int v = trf_dyn_volume();
    int cap;
    if (!trf_dyn_density_enabled()) {
        /* legacy 0..3 caps */
        static const int k_old[4] = { 0, 2, 4, 6 };
        int ov = v > 3 ? 3 : v;
        cap = k_old[ov];
    } else {
        cap = k_cap[v];
    }
    return (cap > TD5_MAX_TRAFFIC_SLOTS) ? TD5_MAX_TRAFFIC_SLOTS : cap;
}

/* Volume also paces the spawner: Low is sparse, High busy, Very-High relentless.
 * The period is the gap (ticks @30Hz) between spawn attempts when below the cap;
 * a shorter period refills emptied slots faster, which (with the proximity
 * recycle below) is the main lever that keeps the road consistently full. */
static int trf_dyn_spawn_period(void)
{
    int p = g_td5.ini.traffic_dyn_period;
    int v = trf_dyn_volume();
    if (!trf_dyn_density_enabled()) {
        /* legacy pacing */
        if (v == 1) p *= 2;
        else if (v >= 3) p = (p * 2) / 3;
    } else {
        /* Retuned so the tiers are clearly distinct:
         *   Low      ×2.0   (sparse — a car every few seconds)
         *   Medium   ×1.0   (stock cadence)
         *   High     ×0.5   (busy — refills fast)
         *   VeryHigh ×0.3   (relentless — slots refill almost immediately) */
        if (v == 1)      p = (p * 2);
        else if (v == 2) p = p;
        else if (v == 3) p = (p * 1) / 2;
        else if (v >= 4) p = (p * 3) / 10;
    }
    if (p < 4) p = 4;
    /* +/-50% jitter so spawns don't metronome. */
    return p / 2 + (int)(trf_dyn_rand() % (uint32_t)p);
}

/* [PER-VIEWPORT TRAFFIC] Per-viewport on-road cap: a normal per-player density,
 * bounded by the partition size so it never overflows a viewport's slot range. */
static int trf_per_viewport_cap(void)
{
    int c = trf_dyn_cap();
    if (c > s_trf_vp_k) c = s_trf_vp_k;
    if (c < 1) c = 1;
    return c;
}

/* Wrap-aware |span distance| from `span_norm` to the NEAREST local player.
 * Wrapping only applies on circuits (the ring); point-to-point distances are
 * plain differences on the normalized span axis. */
static int trf_dyn_min_player_dist(int span_norm)
{
    int humans = g_td5.num_human_players;
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int best = 0x7FFFFFFF;
    int s_lo = 0;
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    if (s_trf_scope_slot >= 0) {     /* per-viewport: only this viewport's player */
        s_lo = s_trf_scope_slot;
        humans = s_trf_scope_slot + 1;
    }
    for (int s = s_lo; s < humans; s++) {
        int ps = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        int d = span_norm - ps;
        if (is_circuit && ring > 0) {
            int half = ring / 2;
            while (d > half)  d -= ring;
            while (d < -half) d += ring;
        }
        if (d < 0) d = -d;
        if (d < best) best = d;
    }
    return best;
}

/* Wrap-aware |span distance| between two normalized spans (circuit ring-aware). */
static int trf_dyn_span_dist(int a, int b)
{
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int d = a - b;
    if (is_circuit && ring > 0) {
        int half = ring / 2;
        while (d > half)  d -= ring;
        while (d < -half) d += ring;
    }
    return d < 0 ? -d : d;
}

/* [item#10 2026-06-15] Count ACTIVE/FADING traffic cars within `radius` spans of
 * `player_span`. Branch-corridor cars (span >= ring) are normalized to their
 * parallel main span first so they count toward whichever player is near that
 * stretch of road. Used to find the human who currently has the least traffic. */
static int trf_dyn_count_traffic_near(int player_span, int radius)
{
    int t_base = g_traffic_slot_base;
    int t_end  = g_active_actor_count;
    int ring   = td5_track_get_ring_length();
    int n = 0;
    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS) t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;
    for (int slot = t_base; slot < t_end; slot++) {
        int sp;
        /* [PER-VIEWPORT TRAFFIC] count only the partition being spawned, so each
         * partition's clump gate behaves identically (ignores other partitions'
         * twins sitting at the same spans). */
        if (s_trf_spawn_partition >= 0 &&
            g_traffic_slot_owner_vp[slot] != s_trf_spawn_partition)
            continue;
        if (s_trf_dyn_state[slot] != TRF_DYN_ACTIVE &&
            s_trf_dyn_state[slot] != TRF_DYN_FADE_IN)
            continue;
        sp = (int)(int16_t)ACTOR_I16(actor_ptr(slot), ACTOR_SPAN_NORMALIZED);
        if (ring > 0 && sp >= ring) {
            int m = td5_track_branch_to_main_span(sp);   /* fold branch -> main */
            if (m >= 0) sp = m;
        }
        if (trf_dyn_span_dist(sp, player_span) <= radius) n++;
    }
    return n;
}

/* [proximity gate 2026-06-18] Stop traffic from spawning in a knot. A fresh
 * spawn is rejected when the stretch of road around the candidate span already
 * holds >= cluster_max ACTIVE/FADING cars within +/- cluster_radius spans (all
 * lanes; branch-corridor cars fold to their parallel main span via
 * trf_dyn_count_traffic_near). This complements the per-lane clearance
 * (traffic_lane_is_clear), which only blocks the SAME lane within a few spans
 * and so let cars pile up across adjacent lanes / nearby spans (user: London
 * reverse ~span 223 spawned a clump). Both knobs are tunable; MAX=0 disables.
 *   TD5RE_TRAFFIC_CLUSTER_SPANS  radius in spans (default 8 = the clearance reach)
 *   TD5RE_TRAFFIC_CLUSTER_MAX    cars allowed in that window before rejecting
 *                                (default auto: 2, or 4 on VERY HIGH so dense
 *                                 modes still fill; 0 = gate off) */
static int trf_dyn_cluster_radius(void)
{
    static int r = -1;
    if (r < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_CLUSTER_SPANS");
        r = e ? atoi(e) : 8;
        if (r < 1) r = 1;
    }
    return r;
}

static int trf_dyn_cluster_max(void)
{
    static int cached = -2;   /* -2 = unread; -1 = auto; >=0 = explicit (0 = off) */
    if (cached == -2) {
        const char *e = getenv("TD5RE_TRAFFIC_CLUSTER_MAX");
        cached = (e && e[0]) ? atoi(e) : -1;
        TD5_LOG_I(LOG_TAG, "traffic_cluster gate: max=%s radius=%d "
                  "(TD5RE_TRAFFIC_CLUSTER_MAX/_SPANS; 0 = off)",
                  (e && e[0]) ? e : "auto", trf_dyn_cluster_radius());
    }
    if (cached >= 0) return cached;
    return (trf_dyn_volume() >= 4) ? 4 : 2;   /* VERY HIGH packs tighter */
}

/* [item#10 2026-06-15] Live-spawn anchor for the consistent-density goal. In a
 * multi-human (split-screen) race the players can spread far apart; anchoring all
 * spawns on the FRONT-MOST human (ai_player_span_lead) leaves the trailing
 * player(s) with little/no nearby traffic. This returns the span of the human who
 * currently has the FEWEST traffic cars within `radius`, tie-breaking toward the
 * TRAILING (lowest-span) player, so fresh spawns are steered to whoever is
 * starved and every human ends up with a comparable amount of traffic. With a
 * single human this is just that human's span (== ai_player_span_lead), so
 * single-player behaviour is unchanged. Gated by the caller on
 * TD5RE_TRAFFIC_DENSITY + num_human_players > 1. */
static int trf_dyn_starved_player_span(int radius)
{
    int humans = g_td5.num_human_players;
    int best_span;
    int best_cnt;
    if (s_trf_scope_slot >= 0)   /* per-viewport: only this viewport's player */
        return (int)(int16_t)ACTOR_I16(actor_ptr(s_trf_scope_slot), ACTOR_SPAN_NORMALIZED);
    if (humans < 1) humans = 1;
    if (humans > g_traffic_slot_base) humans = g_traffic_slot_base;
    best_span = (int)(int16_t)ACTOR_I16(actor_ptr(0), ACTOR_SPAN_NORMALIZED);
    best_cnt  = trf_dyn_count_traffic_near(best_span, radius);
    for (int s = 1; s < humans; s++) {
        int sp  = (int)(int16_t)ACTOR_I16(actor_ptr(s), ACTOR_SPAN_NORMALIZED);
        int cnt = trf_dyn_count_traffic_near(sp, radius);
        /* Strictly fewer wins; on a tie prefer the TRAILING (lower-span) player so
         * the player who is behind is favoured for fresh traffic. */
        if (cnt < best_cnt || (cnt == best_cnt && sp < best_span)) {
            best_cnt  = cnt;
            best_span = sp;
        }
    }
    return best_span;
}

/* [task#8 2026-06-14] A/B knob for spawning ambient traffic on branch corridors
 * (both sides of a fork), not just the main/right route. TD5RE_TRAFFIC_BRANCHES=0
 * restores the prior main-ring-only spawn (TD6 branches stay empty). Default on.
 * Logged once on first read. */
static int trf_dyn_branches_enabled(void)
{
    static int s = -1;
    if (s < 0) {
        /* Default ON: branches are connected drivable roads with route tables, so
         * traffic spawns on and drives them like the main ring. =0 forces all
         * traffic onto the main ring only. */
        s = td5_env_flag_on("TD5RE_TRAFFIC_BRANCHES");
        TD5_LOG_I(LOG_TAG, "traffic_branches knob: TD5RE_TRAFFIC_BRANCHES=%d "
                  "(spawn on branch corridors %s)", s, s ? "ON" : "OFF");
    }
    return s;
}

/* Branch traffic spawning is allowed when the knob is on AND the track has
 * branch corridors (jump table populated). Works for BOTH TD6 city tracks and
 * TD5 tracks (e.g. Moscow) — now that td5_track_normalize_actor_wrap folds a
 * branch car's SPAN_NORMALIZED to its parallel main span on ALL tracks (not
 * just TD6), TD5 branch cars get valid route guidance + despawn distances and
 * drive the corridor like any other car. Previously gated TD6-only, so TD5
 * forks never got branch traffic (user: "traffic not spawning on the right
 * track of a branch"). */
static int trf_dyn_branch_spawn_ok(void)
{
    return trf_dyn_branches_enabled() && td5_track_corridor_count() > 0;
}

/* [#16 cross-traffic 2026-06-17] A/B knob for CROSS-TRAFFIC AT INTERSECTIONS.
 * When a dynamic-traffic car spawns on a TD6 BRANCH corridor (the drivable
 * alternate route taken at a junction), a fraction of those spawns are forced to
 * the OPPOSITE polarity (oncoming) so they drive AGAINST the player's flow
 * through the junction — giving the read of cross-traffic at intersections.
 * DEFAULT ON at a moderate fraction (#16 implemented 2026-06-19). Disable with
 * TD5RE_CROSS_TRAFFIC=0; set an explicit fraction (e.g. 0.5) to tune it.
 *
 * Returns the cross-traffic FRACTION as N/16 (0 = off). When unset it defaults
 * to a believable minority; "1" maps to the default fraction; an explicit
 * decimal sets the fraction directly. Clamped so a minority (never all) of
 * branch spawns are oncoming, keeping a two-way mix rather than a wall of
 * head-on cars. Logged once. */
static int trf_cross_traffic_x16(void)
{
    static int x16 = -2;
    /* [traffic one-way] One-way mode means no oncoming cars at all, including
     * the junction-branch cross-traffic — behaves exactly like
     * TD5RE_CROSS_TRAFFIC=0 (same lockstep-RNG consumption). */
    if (trf_oneway_traffic()) return 0;
    if (x16 == -2) {
        const char *e = getenv("TD5RE_CROSS_TRAFFIC");
        double f;
        if (e && e[0]) {
            f = strtod(e, NULL);                  /* explicit value; "0" disables */
            /* "1" (the common on-switch) means "enabled at the default fraction",
             * NOT 100% oncoming. Any other value is read as the literal fraction. */
            if (f == 1.0) f = 0.5;
        } else {
            f = 0.35;                             /* [#16] default ON: an occasional
                                                   * oncoming car on junction branches */
        }
        if (f < 0.0) f = 0.0;
        if (f > 0.75) f = 0.75;                   /* never more than 3/4 oncoming */
        x16 = (int)(f * 16.0 + 0.5);
        TD5_LOG_I(LOG_TAG, "cross_traffic knob: TD5RE_CROSS_TRAFFIC -> %d/16 "
                  "(oncoming fraction on branch corridors; 0 = off)", x16);
    }
    return x16;
}

/* True when cross-traffic is enabled at all (any non-zero fraction). */
static int trf_cross_traffic_enabled(void)
{
    return trf_cross_traffic_x16() > 0;
}

/* [#18] Width (in spans) of the traffic emergency-brake "proximity gate": a peer
 * this close AHEAD in-lane brakes the car immediately; farther peers are handled by
 * the closing-rate TTC formula. Default 4 (the faithful comment's "within 4 spans").
 * Env TD5RE_TRAFFIC_BRAKE_NEAR. */
static int trf_brake_near_spans(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_BRAKE_NEAR");
        /* 8 spans (was 4): 4 left too little stopping distance, so cars closed up and
         * OVERLAPPED before the brake took hold; 8 keeps a visible following gap while
         * still far below the old unbounded behaviour (which braked for cars ~30 spans
         * ahead and chain-stopped). The TTC closing-rate formula handles anything
         * beyond this. Env TD5RE_TRAFFIC_BRAKE_NEAR. */
        s = e ? atoi(e) : 8;
        if (s < 1) s = 1;
    }
    return s;
}

/* [traffic one-way] Make ALL traffic head the SAME direction (down-track). When
 * on, every lane's drive direction is forced forward (polarity 0): no oncoming
 * lanes, no junction cross-traffic, and no 180° heading flip on queue spawns.
 * DEFAULT ON (user: "the orientation should make the traffic all head the same
 * way"). TD5RE_TRAFFIC_ONEWAY=0 restores the authored two-way / cross-traffic. */
static int trf_oneway_traffic(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_ONEWAY");
        s = (e && e[0]) ? (atoi(e) != 0) : 1;   /* default ON */
        TD5_LOG_I(LOG_TAG, "traffic_oneway knob: TD5RE_TRAFFIC_ONEWAY -> %d "
                  "(1 = all traffic heads down-track; 0 = two-way/cross-traffic)", s);
    }
    return s;
}

/* [traffic pile-up] Hard minimum following gap (in spans). A same-lane peer
 * directly AHEAD within this many spans forces an unconditional brake — even
 * when the SmartAI lane-changer found a clear lane to ease into — because the
 * lane change has a 1-tick latency and easing here would roll the car onto the
 * peer this tick. Guarantees traffic "keeps some distance" instead of stacking
 * nose-to-tail. Default 3; TD5RE_TRAFFIC_MIN_GAP tunes it (>=1). */
static int trf_min_gap_spans(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_MIN_GAP");
        s = e ? atoi(e) : 3;
        if (s < 1) s = 1;
    }
    return s;
}

/* [task#13 2026-06-14] Spawn-distance multiplier (×16 fixed-point). The per-tick
 * spawner places traffic [SpawnAheadMin..SpawnAheadMax] spans ahead of the race
 * leader; the stock 25..50 window pops cars in close to the player. This knob
 * scales that window so traffic appears further away. TD5RE_TRAFFIC_SPAWN_DIST is
 * a decimal multiplier (e.g. "2.0"); "0" restores the stock 1.0x window. Default
 * 2.0x. Clamped 1.0x..6.0x. Logged once. */
static int trf_dyn_spawn_dist_x16(void)
{
    static int x16 = -1;
    if (x16 < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_SPAWN_DIST");
        double m;
        if (e && e[0] == '0' && e[1] == '\0') {
            m = 1.0;                         /* explicit "0" = stock window */
        } else if (e && e[0]) {
            m = strtod(e, NULL);
            if (m < 1.0) m = 1.0;            /* never shrink below stock */
        } else {
            m = 2.0;                         /* default: twice as far away */
        }
        if (m > 6.0) m = 6.0;
        x16 = (int)(m * 16.0 + 0.5);
        TD5_LOG_I(LOG_TAG, "traffic_spawn_dist knob: TD5RE_TRAFFIC_SPAWN_DIST x%d/16 "
                  "(spawn window scaled %d%%)", x16, (x16 * 100) / 16);
    }
    return x16;
}

/* [traffic-front-despawn 2026-06-24] Minimum forward-keep distance, in spans. A
 * traffic car must NEVER fade out while it could still be on-screen AHEAD of a
 * human — that is the "traffic disappears right in front of me instead of far
 * away after I passed them" report. The forward despawn bounds (ahead-of-leader
 * and far-from-all) are normally despawn + eff_hi (default 65 + 100 = 165 spans,
 * comfortably past the ~88-128-span actor render cull). But on SHORT tracks
 * eff_hi is capped at ring/3 (see trf_dyn_effective_spawn_window), which can pull
 * that bound BELOW the render distance so a car fades while still visible ahead —
 * worst in split-screen, where the players spread apart and the far-from-all test
 * (min distance to ANY human) becomes the active despawn condition. This floor
 * keeps the forward-keep distance past the render cull on every track. The
 * rear/"behind" despawn is left untouched (a car retiring far behind after you've
 * passed it is the intended, wanted behaviour). Default 150 spans (render cull +
 * margin); TD5RE_TRAFFIC_FRONT_KEEP overrides (>=0; 0 disables the floor). */
static int trf_dyn_front_keep_floor(void)
{
    static int s = -1;
    if (s < 0) {
        const char *e = getenv("TD5RE_TRAFFIC_FRONT_KEEP");
        if (e && e[0]) {
            s = atoi(e);            /* explicit override always wins */
        } else {
            /* [traffic-view-dist 2026-06-29] Default tracks the extended actor
             * render cull (td5_render reads TD5RE_TRAFFIC_VIEW_DIST, default 1.6x)
             * so a car always fades out BEYOND the visible window, never on-screen.
             * Faithful single-screen max render = 128 spans * view-dist mult; keep
             * +24 spans of margin above that. Was a fixed 150. Reading the same env
             * var here (not a cross-module call) keeps the two in lockstep. */
            const char *ev = getenv("TD5RE_TRAFFIC_VIEW_DIST");
            float vm = (ev && ev[0]) ? (float)atof(ev) : 1.6f;
            if (vm < 1.0f) vm = 1.0f;
            if (vm > 2.5f) vm = 2.5f;
            s = (int)(128.0f * vm + 0.5f) + 24;
            if (s < 150) s = 150;   /* never below the prior faithful floor */
        }
        if (s < 0) s = 0;
        TD5_LOG_I(LOG_TAG, "traffic_front_keep floor: %d spans "
                  "(cars stay until this far ahead of every human)", s);
    }
    return s;
}

/* Effective per-tick spawn window after the distance scale, clamped so it never
 * grows past a safe fraction of the ring (a window wider than ~1/3 of the loop
 * would wrap onto the player from behind and could starve placement). Returns the
 * scaled [*lo, *hi]; the despawn cleanup bound below uses the same *hi so a car
 * spawned far ahead is not immediately faded out. */
static void trf_dyn_effective_spawn_window(int *lo, int *hi)
{
    int base_lo = g_td5.ini.traffic_dyn_spawn_min;
    int base_hi = g_td5.ini.traffic_dyn_spawn_max;
    int s16 = trf_dyn_spawn_dist_x16();
    int elo = (int)(((int64_t)base_lo * s16) >> 4);
    int ehi = (int)(((int64_t)base_hi * s16) >> 4);
    int ring = td5_track_get_ring_length();
    if (elo < base_lo) elo = base_lo;       /* never closer than stock min */
    /* [task#12 2026-06-15] Busy tiers also fill the NEAR road. _SPAWN_DIST scales
     * the FAR edge so cars appear in the distance; on HIGH/VERY HIGH we pull the
     * NEAR edge back down to (near) the stock min so the window spans near→far and
     * the six slots cover the whole stretch instead of clustering at one distance.
     * Low/Medium keep the _SPAWN_DIST window unchanged. */
    if (trf_dyn_density_enabled()) {
        int v = trf_dyn_volume();
        if (v >= 4)      elo = base_lo;              /* Very-High: from the stock min */
        else if (v == 3) elo = (elo + base_lo) / 2; /* High: halfway back toward min */
        if (elo < base_lo) elo = base_lo;
    }
    if (ehi < elo + 1) ehi = elo + 1;
    if (ring > 0) {
        /* Keep the FAR edge within ~1/3 of the ring so the spawn point stays
         * genuinely ahead and there is always placeable road. The NEAR edge
         * follows it down (but never below the stock min) so the window stays a
         * valid [lo<hi] range on small loops. */
        int cap = ring / 3;
        if (cap < base_hi) cap = base_hi;   /* tiny rings: never below stock */
        if (ehi > cap) ehi = cap;
        if (elo > ehi - 1) elo = (ehi - 1 > base_lo) ? ehi - 1 : base_lo;
        if (ehi < elo + 1) ehi = elo + 1;
    }
    if (lo) *lo = elo;
    if (hi) *hi = ehi;
}

/* Place `slot` at (span, lane, polarity) on the canonical main ring.
 * Same placement chain as the faithful recycle LEFT branch
 * [CONFIRMED @ 0x004354B5-0x004356CE + shared zero block LAB_0043588D]. */
static void trf_dyn_place(int slot, int span, int lane, int polarity)
{
    char    *a  = actor_ptr(slot);
    int32_t *rs = route_state(slot);
    uint8_t *rsb = (uint8_t *)rs;

    rs[RS_ROUTE_DIRECTION_POLARITY] = polarity ? 1 : 0;
    *(int32_t *)(rsb + 0x68) = slot * 4 + 0x10;   /* DAT_004afbc8 mirror */
    rs[RS_ROUTE_TABLE_SELECTOR] = 0;

    ACTOR_I16(a, ACTOR_SPAN_RAW)      = (int16_t)span;
    ACTOR_U8(a, ACTOR_SUB_LANE_INDEX) = (uint8_t)lane;

    td5_track_init_actor_segment_placement(
        (int16_t *)(a + ACTOR_SPAN_RAW),
        (int32_t *)(a + ACTOR_WORLD_POS_X));
    td5_track_compute_heading((TD5_Actor *)a);
    if (polarity)
        ACTOR_I32(a, ACTOR_YAW_ACCUM) += 0x80000;  /* oncoming: 180 deg flip */
    td5_physics_reset_actor_state((TD5_Actor *)a);
    td5_ai_seed_actor_track_progress_offset(slot);
    td5_track_normalize_actor_wrap((TD5_Actor *)a);

    /* Shared post-placement state zero (mirrors recycle LAB_0043588D). */
    *(int32_t *)(a + 0x314) = 0;
    *(int32_t *)(a + 0x30C) = 0;
    *(int8_t  *)(a + 0x379) = 0;
    *(int32_t *)(a + 0x1F0) = 0;
    *(int32_t *)(a + 0x1F8) = 0;
    *(int32_t *)(a + 0x1C0) = 0;
    *(int32_t *)(a + 0x1C4) = 0;
    *(int32_t *)(a + 0x1C8) = 0;
    *(int32_t *)(a + 0x1CC) = 0;
    *(int32_t *)(a + 0x1D0) = 0;
    *(int32_t *)(a + 0x1D4) = 0;
    *(int16_t *)(a + 0x338) = 0;
    *(int32_t *)(rsb + 0x88) = 0;
    *(int32_t *)(rsb + 0xF0) = 0;
    *(int32_t *)(rsb + 0x7C) = -1;
    rs[RS_RECOVERY_STAGE]   = 0;
    rs[RS_ENCOUNTER_HANDLE] = -1;
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
        g_traffic_recovery_stage[slot] = 0;
        s_traffic_stuck_frames[slot]   = 0;
        s_traffic_lane_bias[slot]      = 0;
        s_traffic_stall_frames[slot]   = 0;
        s_traffic_escape_ticks[slot]   = 0;
        s_traffic_escape_side[slot]    = 0;
        /* [R3-10] clear anti-pileup progress/lane-commit state on (re)placement */
        s_traffic_lowprog[slot]         = 0;
        s_traffic_lowprog_latch[slot]   = 0;
        s_traffic_prog_window[slot]     = 0;
        s_traffic_escape_cooldown[slot] = 0;
        s_traffic_escape_lane[slot]     = 0;
        s_traffic_escape_lane_ttl[slot] = 0;
        /* [#1 WRECK 2026-06-21] A (re)placed slot is a FRESH car — clear any stale
         * broken-down/wreck state so a recycled wreck slot does NOT spawn already
         * wrecked (anchored + smoking). The legacy recycle path clears this
         * (td5_ai_recycle_traffic_actor, ~6479); the dynamic spawner missed it,
         * which is the "lots of cars spawning wrecked" the user saw. (My
         * s_wreck_push_ticks in td5_physics.c is only read while broken_down is
         * set, so clearing the flag here makes any stale slide-window inert too.) */
        g_actor_broken_down[slot]  = 0;
        g_actor_broken_ticks[slot] = 0;
    }
}

/* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21] Keep traffic on a fork's branch road.
 * Two failure modes (verified via the traffic_dev trace on Moscow: ring=2789,
 * 11 forks) peg traffic steering to ±0x18000 and fishtail it into the walls:
 *   A) span_norm lags span_raw on a branch, so the norm-based remap target
 *      stays at the car's OWN span and flips behind it -> ~180deg error.
 *   B) at a branch end the target lands on an ORPHAN boundary span (the dead
 *      spans between the main road and the branch dst ranges, e.g. ring_length)
 *      whose lane geometry is garbage -> the target is ~285000 units away.
 * Default ON; TD5RE_BRANCH_TRAFFIC_FIX=0 restores the old behaviour for A/B. */
static int branch_traffic_fix_enabled(void) {
    static int s = -1;
    if (s < 0) { s = td5_env_flag_on("TD5RE_BRANCH_TRAFFIC_FIX"); }
    return s;
}

/* [task#18 2026-06-12] TD6 drivable lane band from the route tables.
 * On wide TD6 city strips only the CENTRAL lanes are paved road; the outer
 * lanes are sidewalk. The strip lane bitmask does NOT mark them (London's is
 * all zero), so td5_track_surface_is_slow can't gate them and traffic happily
 * drives on the pavement. The route tables DO encode the road: LEFT.TRK's lane
 * byte is the left racing line and RIGHT.TRK's the right line, each a lateral
 * position 0..255 across the full rail-to-rail width. Convert both to lane
 * indices -> the drivable band [lo,hi]; clamp traffic into it so cars keep off
 * the kerb. Native TD5 (g_active_td6_level==0) is untouched — the whole width
 * is road there. Branch-region spans (>= ring) have no route coverage, so the
 * helper returns 0 and the caller leaves the lane unclamped. Returns 1 when a
 * band narrower than the full lane range was derived.
 * `route_span` is a normalized (main-ring) span index == the route-table row. */
static int td5_ai_td6_drivable_band(int route_span, int lane_count,
                                    int *out_lo, int *out_hi)
{
    int lo = 0, hi = (lane_count > 0) ? lane_count - 1 : 0;
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    if (g_active_td6_level <= 0 || lane_count <= 2 || route_span < 0)
        return 0;
    {
        /* [#18 2026-06-16] BRANCH corridors have no route-table coverage. Derive the
         * band straight from the SURFACE GRID: the contiguous road-class lanes about
         * the centre (excludes the sidewalk/verge edge lanes that surface_is_slow
         * misses). This keeps branch traffic on the actual two-way road — both
         * directions, off the kerb — instead of the median-only or full-width
         * heuristics. Falls through to the central-half fallback if the grid is flat. */
        int ringL = td5_track_get_ring_length();
        if (ringL > 0 && route_span >= ringL) {
            int rl, rh;
            if (td5_track_td6_road_band(route_span, lane_count, &rl, &rh)) {
                if (out_lo) *out_lo = rl;
                if (out_hi) *out_hi = rh;
                return 1;
            }
            /* grid is flat (no distinct kerb) -> central half keeps cars centred */
            {
                int m = lane_count / 4;
                lo = m; hi = lane_count - 1 - m;
                if (hi < lo) hi = lo;
                if (out_lo) *out_lo = lo;
                if (out_hi) *out_hi = hi;
                return (lo > 0 || hi < lane_count - 1);
            }
        }
    }
    {
        const uint8_t *lt = g_route_tables[0];
        const uint8_t *rt = g_route_tables[1];
        size_t idx = (size_t)(unsigned)route_span * 3u;
        int lb, rb;
        if (!lt || !rt ||
            idx >= g_route_table_sizes[0] || idx >= g_route_table_sizes[1]) {
            /* [task#20 2026-06-13] BRANCH / no-route-coverage span: the route table
             * only covers the main ring, so branch corridors (and the wide fork
             * spans) had NO band -> traffic wandered into the plaza/gate walls
             * (user: traffic "crash into walls and zig-zag" on branches). Fall back
             * to the CENTRAL HALF of the road, which is the drivable band the TD6
             * surface grid encodes (London row = [7,7,1,1,1,1,7,7] = central 4/8 =
             * road, outer = sidewalk). Keeps branch traffic on the corridor road. */
            int m = lane_count / 4;
            lo = m;
            hi = lane_count - 1 - m;
            if (hi < lo) hi = lo;
            if (out_lo) *out_lo = lo;
            if (out_hi) *out_hi = hi;
            return (lo > 0 || hi < lane_count - 1);
        }
        lb = (int)lt[idx];
        rb = (int)rt[idx];
        if (lb > rb) { int t = lb; lb = rb; rb = t; }
        lo = (lb * lane_count) / 256;
        hi = (rb * lane_count) / 256;
        if (lo < 0) lo = 0;
        if (hi > lane_count - 1) hi = lane_count - 1;
        if (hi < lo) hi = lo;
        /* [#18 2026-06-16] Intersect with the surface road-band so the route band
         * never includes a sidewalk-class edge lane (kerb is full-grip, so the
         * slow-lane filter misses it). Only tightens, never widens. */
        {
            int rl, rh;
            if (td5_track_td6_road_band(route_span, lane_count, &rl, &rh)) {
                if (lo < rl) lo = rl;
                if (hi > rh) hi = rh;
                if (hi < lo) hi = lo;
            }
        }
    }
    if (out_lo) *out_lo = lo;
    if (out_hi) *out_hi = hi;
    return (lo > 0 || hi < lane_count - 1);
}

/* Pick a random clear, non-slow lane on `span`; the drive direction is the
 * LANE's direction (learned from TRAFFIC.BUS / side heuristic), written to
 * *out_polarity. -1 when nothing qualifies.
 *
 * "Non-slow" is a HARD filter only when the span actually has a fast/slow
 * MIX (a shoulder next to asphalt — the user rule targets shoulders). When
 * EVERY lane is slow-flagged the whole road surface is alternate (e.g.
 * Moscow's cobblestone district sets the 0x10 alternate-surface bit on all
 * lanes) and refusing it starves the spawner for the entire stretch — there,
 * any clear lane qualifies.
 *
 * [#16 cross-traffic 2026-06-17] `desired_pol` is a direction HINT: -1 = use
 * each lane's natural direction (the original behaviour); 0/1 = prefer to spawn
 * a car driving in that polarity (1 = oncoming, against the player's forward
 * flow). When a hint is given we try the matching-direction lanes first AND
 * validate clearance in that requested direction (the clearance test is
 * direction-dependent — see traffic_lane_is_clear), then fall back to the
 * natural-direction pass so a hinted spawn is never starved. */
static int trf_dyn_pick_lane_dir(int slot, int span, int lane_count,
                                 int desired_pol, int *out_polarity)
{
    int start = (lane_count > 1) ? (int)(trf_dyn_rand() % (uint32_t)lane_count) : 0;
    int any_fast = 0;
    int band_lo = 0, band_hi = lane_count - 1;
    int pass;
    /* [task#18] TD6: confine spawns to the paved central band (route tables). */
    td5_ai_td6_drivable_band(span, lane_count, &band_lo, &band_hi);
    for (int lane = 0; lane < lane_count; lane++) {
        if (!td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, lane))) {
            any_fast = 1;
            break;
        }
    }
    /* Pass 0 honours `desired_pol` (skipped when no hint); pass 1 is the
     * original natural-direction fallback. */
    for (pass = (desired_pol >= 0) ? 0 : 1; pass <= 1; pass++) {
        for (int k = 0; k < lane_count; k++) {
            int lane = (start + k) % lane_count;
            int nat  = trf_dyn_lane_direction(lane_count, lane);
            int pol  = (pass == 0) ? (desired_pol & 1) : nat;
            if (lane < band_lo || lane > band_hi)
                continue;   /* [task#18] outside the drivable band = sidewalk */
            if (any_fast &&
                td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, lane)))
                continue;   /* user rule: never spawn on a slow (shoulder) lane */
            if (!traffic_lane_is_clear(slot, span, lane, pol))
                continue;
            *out_polarity = pol;
            return lane;
        }
    }
    return -1;
}
/* NOTE: the previous wrapper `trf_dyn_pick_lane(slot,span,lane_count,out)` was a
 * thin alias for `trf_dyn_pick_lane_dir(..., -1, ...)`; it had a single call site
 * which now passes the cross-traffic hint directly, so the alias was removed to
 * avoid an unused-static warning. Pass desired_pol = -1 for the old behaviour. */

/* [task#8] Branch-corridor enumeration helpers (defined in td5_track.c). Declared
 * in-file (not the shared header) per the same convention as the externs above. */

/* [item#9 2026-06-15] Deliberately pick a MAIN-ring span (within [win_lo..win_hi]
 * ahead of the anchor `ps`) that is PARALLELED by an active branch corridor, so a
 * subsequent branch retarget reliably lands a car on a fork. The earlier branch
 * code only retargeted when the randomly-rolled main span happened to sit inside
 * a fork window (statistically rare with only a few short fork windows on the
 * ring), so branch corridors stayed nearly empty (user: "traffic is still not
 * spawning on the right track of branches"). Returning a main span that is KNOWN
 * to have a branch lets the spawner run its normal edge / start-clearance /
 * per-player-proximity checks against that main span (exactly as for any main-road
 * spawn) and then map it to the branch span — every existing safety check still
 * applies, and a fork is hit on demand instead of by luck. Returns a ring-relative
 * main span, or -1 if no corridor overlaps the forward window. */
static int trf_dyn_pick_branch_main_span(int ps, int win_lo, int win_hi)
{
    int ring = td5_track_get_ring_length();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
    int ncorr = td5_track_corridor_count();
    int q_lo[64], q_hi[64];
    int nq = 0;
    int wlo = ps + win_lo;
    int whi = ps + win_hi;
    if (ncorr <= 0) return -1;
    if (ncorr > 64) ncorr = 64;

    for (int i = 0; i < ncorr; i++) {
        int b_lo, b_hi, m_lo, m_hi;
        int lo, hi;
        if (!td5_track_corridor_info(i, &b_lo, &b_hi, &m_lo, &m_hi)) continue;
        if (b_lo < ring) continue;              /* not a displaced corridor */
        lo = m_lo; hi = m_hi;
        if (is_circuit && ring > 0) {
            /* shift the corridor's parallel-main range onto the window's arc so a
             * corridor that straddles the lap seam still qualifies */
            while (hi < wlo) lo += ring, hi += ring;
            while (lo > whi) lo -= ring, hi -= ring;
        }
        if (hi < wlo || lo > whi) continue;     /* no overlap with the window */
        if (lo < wlo) lo = wlo;                  /* clamp to the window */
        if (hi > whi) hi = whi;
        if (hi < lo) continue;
        q_lo[nq] = lo; q_hi[nq] = hi; nq++;
    }
    if (nq <= 0) return -1;
    {
        int pick = (int)(trf_dyn_rand() % (uint32_t)nq);
        int m = q_lo[pick] + (int)(trf_dyn_rand() % (uint32_t)(q_hi[pick] - q_lo[pick] + 1));
        if (is_circuit && ring > 0) {
            m %= ring;
            if (m < 0) m += ring;
        }
        return m;
    }
}

/* Try to place `slot` at a random span [win_lo..win_hi] ahead of `anchor`
 * (-1 = the live RACE LEADER, any racer slot, human or AI — user rule:
 * traffic appears in front of 1st place, and only retires once the TRAILING
 * human has passed it; AI racers never retire traffic). Race-init seeding
 * passes the START-LINE span explicitly because at init time the grid is not
 * placed yet and every actor's span still reads 0 — anchoring on the live
 * "leader" there scattered seeds around the lap line, ignoring the
 * start-clearance zone on circuits (the Scotland race-start bug).
 * Returns 1 on success. */

static int trf_dyn_spawn_in_window(int slot, int anchor, int win_lo, int win_hi)
{
    int ring   = td5_track_get_ring_length();
    int limit  = (ring > 0) ? ring : td5_track_get_span_count();
    int is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);

    if (win_hi < win_lo) win_hi = win_lo;
    if (limit <= 16) return 0;   /* degenerate strip */

    for (int attempt = 0; attempt < 8; attempt++) {
        /* [task#12 2026-06-15] Live-spawn anchor. The legacy anchor is the overall
         * RACE LEADER (any racer, incl. an AI). In a single-player race the AI
         * leader routinely pulls hundreds of spans ahead of the human, so
         * leader-anchored spawns appear off-screen ahead of the AI and the
         * human's surroundings steadily empty out — the user's "traffic gets
         * sparse / spawns less often as the race goes on." With
         * TD5RE_TRAFFIC_DENSITY on, anchor live spawns on the FRONT-MOST HUMAN
         * instead, so traffic always materialises in a human's forward view
         * regardless of where the AI pack is, and the proximity recycle keeps the
         * six slots cycling through that view. (Race-init seeding still passes an
         * explicit start-line anchor.) Knob off → legacy race-leader anchor. */
        int ps;
        if (anchor >= 0)
            ps = anchor;
        else if (trf_dyn_density_enabled()) {
            /* [item#10 2026-06-15] In a multi-human race, anchor on whichever human
             * currently has the LEAST nearby traffic (tie -> trailing player), so
             * the trailing player(s) get refilled instead of every spawn piling up
             * around the leader — each human ends up with a comparable amount of
             * traffic. Single human: this is just that human's span, identical to
             * the front-most-human anchor. The radius matches the far_from_all
             * recycle corridor below (despawn + scaled spawn-max) so "starved" here
             * and "too far to keep" there use the same yardstick. */
            if (g_td5.num_human_players > 1) {
                int rhi = 0;
                trf_dyn_effective_spawn_window(NULL, &rhi);
                ps = trf_dyn_starved_player_span(g_td5.ini.traffic_dyn_despawn + rhi);
            } else {
                ps = ai_player_span_lead();    /* front-most (== only) human */
            }
        } else
            ps = trf_dyn_race_span_lead();     /* legacy: live race leader */
        /* [traffic-density-patch 2026-06-24] On later attempts the nominal
         * [win_lo, win_hi] window is failing to place a car — typically the
         * stretch directly ahead is a junction, a branch / no-road region, or an
         * all-slow surface where every candidate span is rejected, leaving a
         * visible empty PATCH for the whole time the player approaches it (the
         * "patches of track where the frequency of traffic decreased
         * significantly" report). Progressively push the FAR edge of the search
         * band outward so the spawner can reach PAST the bad stretch to placeable
         * road and fill the gap beyond it, instead of hammering the same dead
         * window 8 times and giving up. The NEAR edge stays at win_lo so a car is
         * never popped in close to the player, and the min-player-dist gate below
         * still uses win_lo. The first two attempts use the nominal window so the
         * common (already-placeable) case is byte-unchanged; the span<3 /
         * span>=limit-8 and circuit-wrap clamps below bound the widened far edge.
         * TD5RE_TRAFFIC_SPAWN_DIST / window math are untouched — this only affects
         * the RETRY band when the nominal window has no room. */
        int a_win_hi = win_hi;
        if (attempt >= 2 && win_hi > win_lo)
            a_win_hi = win_hi + (attempt - 1) * (win_hi - win_lo);
        int dist = win_lo + (int)(trf_dyn_rand() % (uint32_t)(a_win_hi - win_lo + 1));
        int span = ps + dist;
        int main_span;
        int lane_count, lane, polarity;
        int want_branch = 0;
        int on_branch = 0;       /* [#16] set when the spawn retargets onto a branch corridor */
        int cross_pol = -1;      /* [#16] desired-direction hint for the lane picker (-1 = none) */

        if (is_circuit && ring > 0) {
            span %= ring;
            if (span < 0) span += ring;
        }

        /* [item#9 2026-06-15] Deliberate branch attempt: roughly every third
         * placement (when branch corridors are drivable + enabled), aim the spawn
         * at a main span that is KNOWN to be paralleled by a fork, so the retarget
         * below reliably populates a branch corridor instead of waiting for a
         * random main span to land in a (rare) fork window. The chosen main span
         * then runs through all the normal edge/clearance/proximity checks. Gated
         * on TD5RE_TRAFFIC_BRANCHES (default on). */
        if (trf_dyn_branch_spawn_ok() && (trf_dyn_rand() % 3u) == 0u) {
            int bm = trf_dyn_pick_branch_main_span(ps, win_lo, win_hi);
            if (bm >= 0) { span = bm; want_branch = 1; }
        }
        /* Keep off the track-edge spans where the faithful route plan brakes
         * (span < 3 || span >= ring-8, cf. Stage 3 @ 0x00435FAA). */
        if (span < 3 || span >= limit - 8) continue;

        /* Start-line clearance ([Traffic] SpawnStartOffset, user rule): no
         * traffic placements within N spans AFTER the start line, so the
         * grid/launch stretch stays clear. Wrap-aware on circuits (the zone
         * repeats each lap). */
        if (g_td5.ini.traffic_dyn_start_offset > 0) {
            int rel = span - g_td5.track_start_span_index;
            if (is_circuit && ring > 0) {
                rel %= ring;
                if (rel < 0) rel += ring;
            }
            if (rel >= 0 && rel < g_td5.ini.traffic_dyn_start_offset)
                continue;
        }

        /* The window must hold against EVERY local player, not just the one
         * we rolled (multiplayer: no spawning on top of another pane). */
        if (trf_dyn_min_player_dist(span) < win_lo) continue;

        /* [proximity gate] Don't clump: skip this span if its stretch of road
         * already holds enough traffic. `span` here is the main-ring candidate
         * (branch retarget below folds back to it for counting), matching how
         * trf_dyn_count_traffic_near normalizes branch cars to their main span. */
        {
            int cmax = trf_dyn_cluster_max();
            if (cmax > 0 &&
                trf_dyn_count_traffic_near(span, trf_dyn_cluster_radius()) >= cmax)
                continue;
        }

        /* [task#8 2026-06-14] Populate ALL branch corridors of a fork, not just
         * the main/right route. The chosen main-ring span has passed every
         * proximity/clearance check; if one or more branch corridors PARALLEL this
         * main span and branches are drivable, roll uniformly over {main road} +
         * {each branch} and retarget the spawn onto whichever was picked, so both
         * sides of a fork get traffic (user: "traffic only populates the RIGHT
         * track of a branch"; with 2 corridors a branch is picked 2/3 of the time).
         * Earlier this called td5_track_main_to_branch_span(), which returns only
         * the FIRST matching corridor -> a single branch was ever populated. The
         * branch span is contiguous, so the walker traverses it and rejoins the
         * main ring at the corridor end like any other car. Gated on
         * TD5RE_TRAFFIC_BRANCHES (default on). */
        main_span = span;
        if (trf_dyn_branch_spawn_ok()) {
            int ncorr = td5_track_count_branch_corridors(main_span);
            if (ncorr > 0) {
                /* When this main span was chosen DELIBERATELY for a branch
                 * (want_branch), always land on a branch corridor — pick one of
                 * the `ncorr` corridors uniformly. Otherwise roll over {main road}
                 * + {each branch} with equal weight so forks fill evenly while the
                 * main road is never starved (the original task#8 behaviour). */
                int corr = want_branch
                    ? (int)(trf_dyn_rand() % (uint32_t)ncorr)
                    : (int)(trf_dyn_rand() % (uint32_t)(ncorr + 1)) - 1;
                if (corr >= 0) {
                    int bspan = td5_track_branch_corridor_span(main_span, corr);
                    /* [#20 2026-06-18] Never place traffic on a blacklisted fork
                     * (user-flagged dead-end sidewalk corridor). If this attempt
                     * was deliberately aiming at a branch, retry a fresh span;
                     * otherwise just stay on the main road. */
                    if (bspan >= 0 && td5_track_branch_blacklisted(bspan)) {
                        if (want_branch) continue;
                        bspan = -1;
                    }
                    if (bspan >= 0) {
                        span = bspan;
                        on_branch = 1;   /* [#16] this car is on a crossing branch corridor */
                    }
                }
            }
        }

        lane_count = td5_track_span_lane_count_at(span);
        if (lane_count <= 0) continue;

        /* [#16 cross-traffic 2026-06-17] If this car landed on a BRANCH corridor
         * (a crossing route taken at a junction) and cross-traffic is enabled,
         * force a fraction of these spawns to ONCOMING (polarity 1) so they drive
         * AGAINST the player's forward flow through the junction. The lane picker
         * still validates clearance in the requested direction and falls back to
         * the lane's natural direction if no oncoming lane is free, so the car is
         * never starved. Netplay-safe: trf_dyn_rand() is the lockstep RNG, so
         * every peer/replay rolls the same cross decision. NOTE/LIMITATION: TD6
         * branches are PARALLEL-alternate corridors, not true perpendicular roads —
         * the data has no crossing geometry — so "cross-traffic" here is realised as
         * OPPOSITE-DIRECTION traffic on the junction's branch route. It reads as
         * cross-traffic where a branch meets the main ring at the junction, which is
         * the best the parallel-branch data supports. */
        cross_pol = -1;
        if (on_branch && trf_cross_traffic_enabled() &&
            (int)(trf_dyn_rand() % 16u) < trf_cross_traffic_x16())
            cross_pol = 1;   /* oncoming = against the player's forward flow */

        /* Direction follows the LANE (authored TRAFFIC.BUS map / side heuristic)
         * unless the cross-traffic hint above asked for oncoming — a car never
         * drives against its lane's direction in normal (non-cross) flow. */
        polarity = 0;
        lane = trf_dyn_pick_lane_dir(slot, span, lane_count, cross_pol, &polarity);
        if (lane < 0) continue;

        trf_dyn_place(slot, span, lane, polarity);
        TD5_LOG_I(LOG_TAG,
                  "traffic_dyn_spawn: slot=%d span=%d (main=%d) lane=%d/%d oncoming=%d "
                  "cross=%d player_dist=%d attempt=%d",
                  slot, span, main_span, lane, lane_count, polarity,
                  (cross_pol == 1), trf_dyn_min_player_dist(span), attempt);
        return 1;
    }
    return 0;
}

/* Race-start seeding: scatter up to `cap` cars around the players (no fade —
 * they were "always there"), park the rest. Replaces the queue fill of
 * InitializeTrafficActorsFromQueue when dynamic mode is on. */
void td5_ai_traffic_dynamic_race_init(void)
{
    int t_base = g_traffic_slot_base;
    int t_end  = g_active_actor_count;
    int cap, placed = 0;

    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS)
        t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;

    memset(s_trf_dyn_state, TRF_DYN_INACTIVE, sizeof(s_trf_dyn_state));
    memset(s_trf_dyn_alpha, 0, sizeof(s_trf_dyn_alpha));
    s_trf_dyn_rng      = 0x54443552u ^ ((uint32_t)g_td5.track_index * 2654435761u);
    s_trf_dyn_cooldown = 0;
    s_trf_dyn_seeded   = 1;
    cop_state_reset();   /* police-chase per-race state (deterministic) */
    trf_per_viewport_setup();   /* [per-viewport traffic] partition + activation */

    /* Learn the per-track lane→direction map from the authored TRAFFIC.BUS
     * records (4-byte {span, flags bit0 = oncoming, lane}, -1 span sentinel
     * [CONFIRMED @ 0x004353CE]). Junction-table entries (lane >= the span's
     * lane-count nibble — the original's REMAP branch @ 0x004359C1) are
     * skipped: their lane index refers to the alternate table. Also derives
     * which HALF of the road carries oncoming traffic, as the fallback for
     * unseen (lane_count, lane) combos and for tracks without a queue
     * (TD6 conversions default to oncoming-on-the-left-half). */
    memset(s_trf_dyn_lane_dir, -1, sizeof(s_trf_dyn_lane_dir));
    s_trf_dyn_oncoming_left = 1;
    s_trf_dyn_oncoming_pct  = 0;
    if (g_traffic_queue_base) {
        static int16_t votes[TRF_DYN_MAX_LANES + 1][TRF_DYN_MAX_LANES][2];
        const uint8_t *qp = g_traffic_queue_base;
        int total = 0, oncoming = 0;
        /* side accumulator: normalized lane position (0..256 across the road)
         * summed separately for forward and oncoming entries. */
        int32_t side_sum[2] = { 0, 0 };
        int32_t side_n[2]   = { 0, 0 };
        memset(votes, 0, sizeof(votes));
        for (int i = 0; i < 4096; i++, qp += 4) {
            int16_t q_span = td5_read_le16s(qp);
            int pol, lane, lc;
            if (q_span == -1) break;
            pol  = (int)(qp[2] & 1u);
            lane = (int)qp[3];
            lc   = td5_track_span_lane_count_at((int)q_span);
            total++;
            if (pol) oncoming++;
            if (lc < 1 || lc > TRF_DYN_MAX_LANES || lane >= lc)
                continue;   /* junction-table entry or unusable span */
            votes[lc][lane][pol]++;
            side_sum[pol] += ((lane * 2 + 1) * 128) / lc;   /* 0..256 centre pos */
            side_n[pol]++;
        }
        for (int lc = 1; lc <= TRF_DYN_MAX_LANES; lc++)
            for (int lane = 0; lane < lc; lane++)
                if (votes[lc][lane][0] | votes[lc][lane][1])
                    s_trf_dyn_lane_dir[lc][lane] =
                        (votes[lc][lane][1] > votes[lc][lane][0]) ? 1 : 0;
        if (side_n[0] > 0 && side_n[1] > 0) {
            s_trf_dyn_oncoming_left =
                (side_sum[1] / side_n[1]) < (side_sum[0] / side_n[0]);
        }
        if (total > 0)
            s_trf_dyn_oncoming_pct = (oncoming * 100) / total;
    }

    cap = trf_dyn_cap();
    if (s_trf_per_vp) {
        /* [PER-VIEWPORT TRAFFIC] Seed each partition identically: same start-line
         * anchor + identical-seed RNG, scoped to its own viewport's player, so every
         * viewport starts with matching grid traffic in matching places. */
        for (int vp = 0; vp < s_trf_vp_count; vp++) {
            int p_lo = t_base + vp * s_trf_vp_k;
            int p_hi = (vp == s_trf_vp_count - 1) ? (t_base + s_trf_vp_count * s_trf_vp_k)
                                                  : (p_lo + s_trf_vp_k);
            int vp_placed = 0, vp_cap = trf_per_viewport_cap();
            if (p_hi > t_end) p_hi = t_end;
            /* Shared start-line anchor + identical-seed RNG => identical grid. */
            s_trf_dyn_rng         = s_trf_dyn_rng_vp[vp];
            s_trf_spawn_partition = vp;   /* density gate counts only this partition */
            for (int slot = p_lo; slot < p_hi; slot++) {
                int seed_lo = g_td5.ini.traffic_dyn_start_offset + 8;
                if (vp_placed < vp_cap &&
                    trf_dyn_spawn_in_window(slot, g_td5.track_start_span_index,
                                            seed_lo, seed_lo + 100)) {
                    s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
                    s_trf_dyn_alpha[slot] = 255;
                    vp_placed++;
                } else {
                    s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
                    s_trf_dyn_alpha[slot] = 0;
                }
            }
            s_trf_spawn_partition = -1;
            s_trf_dyn_rng_vp[vp]  = s_trf_dyn_rng;
            placed += vp_placed;
        }
        s_trf_scope_slot = -1;
    } else
    for (int slot = t_base; slot < t_end; slot++) {
        /* Seed anchored on the START-LINE span (the grid isn't placed yet at
         * init time — live actor spans all read 0 here), scattered across a
         * 100-span stretch just past the start-clearance zone, so the road is
         * already populated where the field will first encounter traffic. */
        int seed_lo = g_td5.ini.traffic_dyn_start_offset + 8;
        if (placed < cap &&
            trf_dyn_spawn_in_window(slot, g_td5.track_start_span_index,
                                    seed_lo, seed_lo + 100)) {
            s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
            s_trf_dyn_alpha[slot] = 255;
            placed++;
        } else {
            /* Parked: every consumer (AI/physics/V2V/render/minimap/audio)
             * skips INACTIVE slots, so the zeroed pose is never observed. */
            s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
            s_trf_dyn_alpha[slot] = 0;
        }
    }
    TD5_LOG_I(LOG_TAG,
              "traffic_dyn_init: seeded %d/%d cars (volume=%d cap=%d oncoming=%d%% "
              "oncoming_left=%d window=[%d..%d] despawn=%d fade=%d period=%d speed=%d%% "
              "start_clear=%d@%d)",
              placed, t_end - t_base, g_td5.traffic_volume, cap,
              s_trf_dyn_oncoming_pct, s_trf_dyn_oncoming_left,
              g_td5.ini.traffic_dyn_spawn_min, g_td5.ini.traffic_dyn_spawn_max,
              g_td5.ini.traffic_dyn_despawn, g_td5.ini.traffic_dyn_fade_ticks,
              g_td5.ini.traffic_dyn_period, g_td5.ini.traffic_dyn_speed_pct,
              g_td5.ini.traffic_dyn_start_offset, g_td5.track_start_span_index);
    {
        /* [task#8/#13] Report the resolved run-time spawn behaviour once per race:
         * the SCALED per-tick spawn window (the close-pop-in fix) and whether
         * branch corridors are populated (the branch-traffic fix). The init seed
         * above intentionally uses the stock close window; these are the values the
         * in-race spawner uses. */
        int eff_lo = 0, eff_hi = 0;
        trf_dyn_effective_spawn_window(&eff_lo, &eff_hi);
        TD5_LOG_I(LOG_TAG,
                  "traffic_dyn_runtime: volume=%d cap=%d period~=%d density=%s "
                  "spawn_window=[%d..%d] (scaled x%d/16) branches=%s td6=%d "
                  "branches_drivable=%d",
                  trf_dyn_volume(), trf_dyn_cap(), g_td5.ini.traffic_dyn_period,
                  trf_dyn_density_enabled() ? "ON" : "OFF",
                  eff_lo, eff_hi, trf_dyn_spawn_dist_x16(),
                  trf_dyn_branches_enabled() ? "ON" : "OFF",
                  g_active_td6_level, td5_track_td6_branches_drivable());
    }
}

/* Per-sim-tick driver — fades, despawn checks, spawn cadence.
 * Called once per tick from td5_ai_pre_tick. */
void td5_ai_traffic_dynamic_tick(void)
{
    int t_base, t_end, fade_step, on_road = 0;

    if (!td5_ai_traffic_dynamic_active()) return;
    if (!g_actor_base || !g_route_state_base) return;
    t_base = g_traffic_slot_base;
    t_end  = g_active_actor_count;
    if (t_end <= t_base) return;
    if (t_end > t_base + TD5_MAX_TRAFFIC_SLOTS) t_end = t_base + TD5_MAX_TRAFFIC_SLOTS;
    if (t_end > TD5_MAX_TOTAL_ACTORS) t_end = TD5_MAX_TOTAL_ACTORS;

    fade_step = 255 / g_td5.ini.traffic_dyn_fade_ticks;
    if (fade_step < 1) fade_step = 1;

    for (int slot = t_base; slot < t_end; slot++) {
        char *a = actor_ptr(slot);
        /* [PER-VIEWPORT TRAFFIC] Despawn uses the SHARED (all-players) corridor so
         * every partition retires the same cars at the same time — keeping the
         * partitions identical-in-place. Independence is collision/render only. */
        switch (s_trf_dyn_state[slot]) {
        case TRF_DYN_FADE_IN:
            s_trf_dyn_alpha[slot] = (int16_t)(s_trf_dyn_alpha[slot] + fade_step);
            if (s_trf_dyn_alpha[slot] >= 255) {
                s_trf_dyn_alpha[slot] = 255;
                s_trf_dyn_state[slot] = TRF_DYN_ACTIVE;
            }
            on_road++;
            break;

        case TRF_DYN_ACTIVE: {
            int sp, behind, ahead, ring, is_circuit;
            /* Never retire the live cop (mirrors recycle's slot-9 guard
             * @ 0x0043545B). */
            if (slot == 9 && g_encounter_tracked_handle != -1) { on_road++; break; }
            /* Live corridor = [trailing HUMAN - despawn .. race LEADER +
             * despawn]. A car only retires once the LAST human has passed it
             * by DespawnDistance (signed — cars still AHEAD of a last-place
             * human stay alive for them to encounter), or if it somehow ends
             * up far past the race leader. AI racers never retire traffic. */
            sp = (int)(int16_t)ACTOR_I16(a, ACTOR_SPAN_NORMALIZED);
            behind = sp - ai_player_span_trailing();
            ahead  = sp - trf_dyn_race_span_lead();
            ring = td5_track_get_ring_length();
            is_circuit = (g_td5.track_type == TD5_TRACK_CIRCUIT);
            if (is_circuit && ring > 0) {
                int half = ring / 2;
                while (behind > half)  behind -= ring;
                while (behind < -half) behind += ring;
                while (ahead > half)   ahead -= ring;
                while (ahead < -half)  ahead += ring;
            }
            /* Ahead-of-leader bound is a generous CLEANUP (despawn+spawn_max,
             * ~115 spans — past the ~88-128 actor render cull): a SpeedScale'd
             * traffic car can genuinely outrun the race leader, and fading it
             * at the plain despawn distance was visible from the leader's
             * seat ("traffic disappears in front of me").
             *
             * [task#13] The cleanup distance uses the EFFECTIVE (scaled) spawn
             * max, not the raw INI value: TD5RE_TRAFFIC_SPAWN_DIST pushes spawns
             * further ahead, so a car freshly placed near the scaled max must NOT
             * be inside the despawn corridor or it would fade out the instant it
             * spawned. Tying the bound to the same scaled max keeps the spawn /
             * cleanup distances consistent.
             *
             * Stuck replacement is DEBOUNCED on s_traffic_stuck_frames (45
             * ticks ≈ 1.5s continuously recovery-frozen, counted by the
             * AntiFreeze layer): the Stage-2 recovery latch also arms
             * TRANSIENTLY for a tick or two on curves (esp. oncoming cars),
             * and replacing on a transient latch vanished healthy cars in
             * plain sight. (With [Traffic] AntiFreeze=0 the counter stays 0 —
             * stuck cars then just brake until the corridor passes them.) */
            {
            int eff_lo, eff_hi;
            int far_from_all;
            int front_keep;
            trf_dyn_effective_spawn_window(&eff_lo, &eff_hi);
            /* [traffic-front-despawn 2026-06-24] Forward-keep distance: cars
             * stay alive until this far ahead of every human, floored at the
             * render-visible distance so none ever fades while on-screen ahead.
             * Only ever WIDENS the keep-zone (never below despawn+eff_hi), so a
             * freshly spawned car at eff_hi ahead is never inside it -> no
             * spawn/despawn thrash. The rear "behind" bound is unchanged. */
            front_keep = g_td5.ini.traffic_dyn_despawn + eff_hi;
            {
                int fk_floor = trf_dyn_front_keep_floor();
                if (front_keep < fk_floor) front_keep = fk_floor;
            }
            /* [task#12 2026-06-15] PROXIMITY RECYCLE — the dwindle fix.
             * The corridor test above is anchored on the TRAILING HUMAN (behind)
             * and the RACE LEADER (ahead, who may be an AI). When the field
             * spreads — a fast AI leader pulls away from a slow human, or two
             * humans drift apart — that corridor grows to hundreds of spans, so
             * a healthy car that nobody is near NEVER satisfies behind<-despawn
             * or ahead>despawn+eff_hi. It stays ACTIVE forever, on_road pins at
             * the cap, the spawner is gated off, and the six slots smear thinly
             * across the whole gap → "traffic gets sparse / spawns less often as
             * the race goes on." Recycling any car that is farther than
             * (despawn + scaled spawn-max) from EVERY human returns its slot to
             * the pool so a fresh car is placed back near the action, holding the
             * density steady for the whole race regardless of field spread.
             * (The fresh car re-enters via FADE_IN ahead of the leader, so this
             * never strands the leader without traffic.) Gated on
             * TD5RE_TRAFFIC_DENSITY. */
            far_from_all = trf_dyn_density_enabled() &&
                           !td5_ai_cop_is_chasing(slot) &&
                           trf_dyn_min_player_dist(sp) > front_keep;
            /* [#20] (Removed) The branch idle-fade + blacklist self-heal that used to
             * sit here coped with traffic stranded on "displaced/off-road" forks.
             * Branches are connected drivable roads, so branch traffic is ordinary
             * traffic handled by the general despawn (behind/ahead/far) below. */
            if (!td5_ai_cop_is_chasing(slot) &&
                (behind < -g_td5.ini.traffic_dyn_despawn ||
                ahead  >  front_keep ||
                far_from_all ||
                (g_traffic_recovery_stage[slot] != 0 &&
                 s_traffic_stuck_frames[slot] >= 45 &&
                 /* [traffic-front-despawn 2026-06-29] far enough that the fade is
                  * unobtrusive — gate on the SAME forward-keep distance as the
                  * ahead/far paths (front_keep, which is >= the ~128-span actor
                  * render cull at ViewDistance=100), NOT the bare 65-span despawn.
                  * A stuck car 66..128 spans away is still ON-SCREEN, so fading it
                  * at the old 65-span gate vanished it in plain sight ("traffic
                  * disappears in front of me"). A nearer stuck car is realigned by
                  * AntiFreeze / just brakes until the player passes it (then the
                  * rear despawn retires it out of view). */
                 trf_dyn_min_player_dist(sp) > front_keep))) {
                s_trf_dyn_state[slot] = TRF_DYN_FADE_OUT;
                TD5_LOG_I(LOG_TAG,
                          "traffic_dyn_despawn: slot=%d behind_trail=%d ahead_lead=%d "
                          "min_player=%d front_keep=%d far_from_all=%d recovery=%d stuck=%d (fading out)",
                          slot, behind, ahead, trf_dyn_min_player_dist(sp),
                          front_keep, far_from_all, g_traffic_recovery_stage[slot],
                          s_traffic_stuck_frames[slot]);
            }
            }   /* eff spawn-window scope */
            on_road++;
            break;
        }

        case TRF_DYN_FADE_OUT:
            s_trf_dyn_alpha[slot] = (int16_t)(s_trf_dyn_alpha[slot] - fade_step);
            if (s_trf_dyn_alpha[slot] <= 0) {
                s_trf_dyn_alpha[slot] = 0;
                s_trf_dyn_state[slot] = TRF_DYN_INACTIVE;
                /* Police: a recycled slot loses its cop identity + any chase
                 * lock, so a future ordinary spawn here isn't born a cop. */
                if (g_cop_is_cop[slot]) cop_release(slot);
                /* Park: freeze all motion so the hidden actor stays put. */
                *(int32_t *)(a + 0x314) = 0;   /* longitudinal speed */
                *(int32_t *)(a + 0x1C0) = 0;
                *(int32_t *)(a + 0x1C4) = 0;
                *(int32_t *)(a + 0x1C8) = 0;
                *(int32_t *)(a + 0x1CC) = 0;   /* lin vel x */
                *(int32_t *)(a + 0x1D0) = 0;
                *(int32_t *)(a + 0x1D4) = 0;   /* lin vel z */
                ACTOR_U8(a, ACTOR_BRAKE_FLAG) = 1;
            } else {
                on_road++;
            }
            break;

        default: /* TRF_DYN_INACTIVE */
            break;
        }
    }

    s_trf_scope_slot = -1;   /* clear per-slot despawn scope before the spawn pass */

    /* [PER-VIEWPORT TRAFFIC] Time-trial split-screen: each partition runs its OWN
     * spawn cadence — scoped to its viewport's player + private identical-seed RNG —
     * so the sets land in matching places yet stay fully independent (one player
     * perturbing a car never shows in another pane). No cop logic (TT has no cops). */
    if (s_trf_per_vp) {
        for (int vp = 0; vp < s_trf_vp_count; vp++) {
            int p_lo = t_base + vp * s_trf_vp_k;
            int p_hi = (vp == s_trf_vp_count - 1) ? (t_base + s_trf_vp_count * s_trf_vp_k)
                                                  : (p_lo + s_trf_vp_k);
            int vp_on_road = 0, pick = -1, slot, spawn_lo, spawn_hi;
            if (p_hi > t_end) p_hi = t_end;
            if (s_trf_dyn_cooldown_vp[vp] > 0) s_trf_dyn_cooldown_vp[vp]--;
            for (slot = p_lo; slot < p_hi; slot++)
                if (s_trf_dyn_state[slot] != TRF_DYN_INACTIVE) vp_on_road++;
            if (vp_on_road >= trf_per_viewport_cap() || s_trf_dyn_cooldown_vp[vp] > 0)
                continue;
            for (slot = p_lo; slot < p_hi; slot++)
                if (s_trf_dyn_state[slot] == TRF_DYN_INACTIVE) { pick = slot; break; }
            if (pick < 0) { s_trf_dyn_cooldown_vp[vp] = 10; continue; }
            /* Shared anchor/proximity (no per-player scope) + this partition's
             * identical-seed RNG => every partition spawns the SAME car at the
             * SAME place; only the target slot differs. */
            s_trf_dyn_rng         = s_trf_dyn_rng_vp[vp];
            s_trf_spawn_partition = vp;   /* density gate counts only this partition */
            trf_dyn_effective_spawn_window(&spawn_lo, &spawn_hi);
            if (trf_dyn_spawn_in_window(pick, -1, spawn_lo, spawn_hi)) {
                s_trf_dyn_state[pick] = TRF_DYN_FADE_IN;
                s_trf_dyn_alpha[pick] = 0;
                s_trf_dyn_cooldown_vp[vp] = trf_dyn_spawn_period();
                g_cop_is_cop[pick] = 0; g_cop_phase[pick] = COP_IDLE; g_cop_target[pick] = -1;
            } else {
                s_trf_dyn_cooldown_vp[vp] = 10;   /* nothing placeable now — retry soon */
            }
            s_trf_spawn_partition = -1;
            s_trf_dyn_rng_vp[vp]  = s_trf_dyn_rng;   /* persist this partition's RNG */
            s_trf_scope_slot      = -1;
        }
        return;
    }

    /* Spawn cadence. Prefer slot 9 (the cop-capable slot) so speeding
     * pursuits can still trigger; round-robin the rest. */
    if (s_trf_dyn_cooldown > 0) s_trf_dyn_cooldown--;
    if (on_road < trf_dyn_cap() && s_trf_dyn_cooldown <= 0) {
        int pick = -1;
        if (9 >= t_base && 9 < t_end && s_trf_dyn_state[9] == TRF_DYN_INACTIVE)
            pick = 9;
        if (pick < 0) {
            for (int slot = t_base; slot < t_end; slot++) {
                if (s_trf_dyn_state[slot] == TRF_DYN_INACTIVE) { pick = slot; break; }
            }
        }
        static int s_trf_dyn_starved = 0;
        /* [task#13] Spawn FURTHER from the player: the per-tick window is scaled by
         * TD5RE_TRAFFIC_SPAWN_DIST (default 2x) so traffic appears in the distance
         * instead of popping in nearby. Race-init seeding (above) keeps the stock
         * close window — those cars were "always there" near the start. */
        int spawn_lo, spawn_hi;
        trf_dyn_effective_spawn_window(&spawn_lo, &spawn_hi);
        if (pick >= 0 &&
            trf_dyn_spawn_in_window(pick, -1, spawn_lo, spawn_hi)) {
            s_trf_dyn_state[pick] = TRF_DYN_FADE_IN;
            s_trf_dyn_alpha[pick] = 0;
            s_trf_dyn_cooldown = trf_dyn_spawn_period();
            s_trf_dyn_starved = 0;
            /* Police 1-in-CopRatio cadence (deterministic: counter advances on
             * each successful spawn, identical on every lockstep peer). A fresh
             * spawn is an ordinary traffic car; after CopRatio regular cars the
             * next spawn is flagged a cop (only when the POLICE option is on).
             * An IDLE cop cruises as normal traffic until a racer passes it. */
            g_cop_is_cop[pick] = 0;
            g_cop_phase[pick]  = COP_IDLE;
            g_cop_target[pick] = -1;
            if (g_encounter_enabled) {
                if (s_cop_spawn_counter >= g_td5.ini.cop_ratio) {
                    g_cop_is_cop[pick]  = 1;
                    s_cop_spawn_counter = 0;
                    TD5_LOG_I(LOG_TAG, "cop_spawn: slot=%d flagged COP (1 per %d cars)",
                              pick, g_td5.ini.cop_ratio);
                } else {
                    s_cop_spawn_counter++;
                }
            }
        } else {
            s_trf_dyn_cooldown = 10;   /* nothing placeable right now — retry soon */
            /* Persistent failure is a data/window problem worth surfacing
             * (e.g. all-slow surface stretch, track-end window, no clear
             * lanes). One line per ~10s of continuous starvation. */
            if ((++s_trf_dyn_starved % 30) == 0) {
                TD5_LOG_W(LOG_TAG,
                          "traffic_dyn_starved: %d consecutive spawn failures "
                          "(lead_span=%d ring=%d span_count=%d on_road=%d cap=%d)",
                          s_trf_dyn_starved, ai_player_span_lead(),
                          td5_track_get_ring_length(), td5_track_get_span_count(),
                          on_road, trf_dyn_cap());
            }
        }
    }
}

void td5_ai_update_traffic_route_plan(int slot) {
    int32_t *rs = route_state(slot);
    char *actor  = actor_ptr(slot);
    int  ref_slot;
    char *ref_actor;
    int32_t heading_shifted, route_byte_val, route_heading_shifted;
    int32_t hdelta, hdelta_neg;
    int polarity, peer;

    /* [FIX 2026-06-02 traffic-recovery-faithful — fix-1780404735]
     * REMOVED a non-original per-tick decrement of g_traffic_recovery_stage
     * that used to sit here. Exhaustive Ghidra search of TD5_d3d.exe confirms
     * the original (gActorTrafficRecoveryStage @ 0x4AFBE8) is NEVER decremented
     * anywhere — it is set 1..7 by Stage 2 (heading-misalignment, unconditional),
     * escalated on heavy collision (0x408215, saturating at 7), and cleared in
     * EXACTLY ONE place: RecycleTrafficActorFromQueue (0x4353B0) when the actor
     * falls >0x28 spans behind the player and is respawned ahead. The prior port
     * author's premise ("the orig has a per-tick countdown somewhere") was wrong
     * — the original relies on queue-recycle, not a countdown, to retire a
     * misaligned/stuck traffic actor. The invented decrement (plus the `== 0`
     * arming guard removed below) produced a non-faithful brake↔recover
     * oscillation at lane-change/junction headings instead of the original's
     * clean brake-until-recycle. Restoring the faithful behavior here. */

    /* --- Stage 1: Recycle --- */
    if (td5_ai_traffic_dynamic_active()) {
        /* [dynamic-traffic] The queue recycle is replaced by the distance
         * spawner (td5_ai_traffic_dynamic_tick, driven from td5_ai_pre_tick).
         * A parked (despawned) car has no route plan at all — hold the brake
         * and bail before any heading/recovery logic can run on its frozen
         * pose. */
        if (td5_ai_traffic_dynamic_parked(slot)) {
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            return;
        }
    } else {
        td5_ai_recycle_traffic_actor();
    }

    /* [S20 AntiFreeze] un-stick a traffic car that the faithful recovery brake
     * has frozen and the player-relative recycle can't reach (parked player). */
    traffic_smart_antifreeze(slot, actor, rs);

    /* [DIAG+FIX 2026-07-07] Stuck detector + universal unstick backstop. Tracks
     * per-slot span progress; under TD5RE_TRAFFIC_DIAG it edge-logs STUCK/UNSTUCK
     * with the freeze cause, and under TD5RE_TRAFFIC_UNSTICK (default ON) it
     * force-resets a car that has made no progress for ~3s so it rejoins the flow
     * — covering the low-speed wedges (recovery_stage==0) that AntiFreeze and
     * collision-escape both miss. An actively-chasing cop is never reset. */
    if ((traffic_diag_enabled() || traffic_unstick_enabled()) &&
        slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS) {
        static int16_t  s_stuck_last_span[TD5_MAX_TOTAL_ACTORS];
        static uint16_t s_stuck_ticks[TD5_MAX_TOTAL_ACTORS];
        static uint8_t  s_stuck_logged[TD5_MAX_TOTAL_ACTORS];
        static uint32_t s_last_reset_frame[TD5_MAX_TOTAL_ACTORS];
        static int      s_sd_init = 0;
        if (!s_sd_init) {
            for (int z = 0; z < TD5_MAX_TOTAL_ACTORS; z++) {
                s_stuck_last_span[z] = ACTOR_I16(actor_ptr(z), ACTOR_SPAN_RAW);
                s_stuck_ticks[z] = 0; s_stuck_logged[z] = 0;
                s_last_reset_frame[z] = 0;
            }
            s_sd_init = 1;
        }
        int16_t cur_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int32_t lspd     = ACTOR_I32(actor, ACTOR_LONGITUDINAL_SPEED);
        if (cur_span == s_stuck_last_span[slot] && lspd < 0x1000 && lspd > -0x1000) {
            s_stuck_ticks[slot]++;
            if (traffic_diag_enabled() && s_stuck_ticks[slot] == 60 &&
                !s_stuck_logged[slot]) {
                s_stuck_logged[slot] = 1;
                TD5_LOG_W(LOG_TAG,
                    "traffic_diag STUCK: slot=%d span=%d ~60t no-progress lspd=%d "
                    "recovery_stage=%d brake=%d cop=%d phase=%d",
                    slot, (int)cur_span, lspd, g_traffic_recovery_stage[slot],
                    (int)ACTOR_U8(actor, ACTOR_BRAKE_FLAG),
                    (int)g_cop_is_cop[slot], (int)g_cop_phase[slot]);
            }
            /* [FIX] force-unstick after ~90 ticks (3s), unless it's an
             * actively-chasing cop (leave the chase alone) or already broken. */
            if (traffic_unstick_enabled() && s_stuck_ticks[slot] >= 90 &&
                !g_actor_broken_down[slot] &&
                !(g_cop_is_cop[slot] && g_cop_phase[slot] != COP_IDLE)) {
                /* If this slot re-wedged shortly after a previous reset, an
                 * in-place reset clearly isn't clearing the obstacle (a hotspot
                 * pinch). Escalate to a full despawn so the distance spawner
                 * relocates the car elsewhere instead of thrashing at one spot. */
                unsigned since = (s_last_reset_frame[slot] != 0)
                    ? (g_ai_frame_counter - s_last_reset_frame[slot]) : 0xFFFFFFFFu;
                s_last_reset_frame[slot] = g_ai_frame_counter ? g_ai_frame_counter : 1u;
                if (since < 150u && td5_ai_traffic_dynamic_active() &&
                    !g_cop_is_cop[slot]) {
                    s_trf_dyn_state[slot] = TRF_DYN_FADE_OUT;   /* relocate */
                    TD5_LOG_I(LOG_TAG,
                        "traffic_unstick: slot=%d RE-WEDGED at span=%d -> despawn/relocate",
                        slot, (int)cur_span);
                } else {
                    traffic_force_unstick(slot, actor, rs);
                    TD5_LOG_I(LOG_TAG,
                        "traffic_unstick: slot=%d FORCE-reset at span=%d after %u stuck ticks",
                        slot, (int)cur_span, (unsigned)s_stuck_ticks[slot]);
                }
                s_stuck_ticks[slot]  = 0;
                s_stuck_logged[slot] = 0;
                cur_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
            }
        } else {
            if (traffic_diag_enabled() && s_stuck_logged[slot])
                TD5_LOG_I(LOG_TAG,
                    "traffic_diag UNSTUCK: slot=%d span=%d after %u ticks",
                    slot, (int)cur_span, (unsigned)s_stuck_ticks[slot]);
            s_stuck_ticks[slot] = 0;
            s_stuck_logged[slot] = 0;
        }
        s_stuck_last_span[slot] = cur_span;
    }

    /* [S20 smart-traffic] periodic diagnostic dump (one slot, every ~300 frames)
     * so the before/after trace can confirm which behaviours actually fired. */
    if (g_td5.ini.traffic_smart && slot == g_traffic_slot_base &&
        (g_ai_frame_counter % 300u) == 0u) {
        TD5_LOG_I(LOG_TAG,
                  "traffic_smart_stat: react=%d ease_difflane=%d ease_lanechg=%d "
                  "single_lane=%d no_clear=%d slow_lane=%d wallnudge=%d",
                  s_smart_stat.react_calls, s_smart_stat.ease_diff_lane,
                  s_smart_stat.ease_lane_change, s_smart_stat.blocked_single_lane,
                  s_smart_stat.no_clear_lane, s_smart_stat.slow_lane_change,
                  s_smart_stat.wall_nudges);
    }

    /* --- Stage 2: Heading misalignment check ---
     * [CONFIRMED @ 0x00435EA6-0x00435FA8]
     * The original reads ref_slot = rs[RS_SLOT_INDEX] (EBX+0xD4), then
     * loads span_normalized + yaw_accum from actor_ptr(ref_slot). For traffic
     * ref_slot == param_1 so behaviour is the same, but the port follows the
     * original dispatch path. */
    ref_slot = rs[RS_SLOT_INDEX];
    if (ref_slot < 0 || ref_slot >= TD5_MAX_TOTAL_ACTORS) ref_slot = slot;
    ref_actor = actor_ptr(ref_slot);

    {
        const uint8_t *rb = (const uint8_t *)(intptr_t)rs[RS_ROUTE_TABLE_PTR];
        int16_t ref_span_norm = ACTOR_I16(ref_actor, ACTOR_SPAN_NORMALIZED);
        if (rb && ref_span_norm >= 0) {
            route_byte_val = (int32_t)rb[(size_t)(unsigned)ref_span_norm * 3u + 1u];
        } else {
            route_byte_val = 0;
        }
    }
    heading_shifted       = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM) >> 8;
    /* Original sequence at 0x435ED8-0x435EF2 computes
     *   tmp = route_byte * 0x102C
     *   tmp = (tmp + (tmp >> 31 & 0xFF)) >> 8
     * which is signed div-by-256 with arithmetic rounding toward zero. */
    {
        int32_t tmp = route_byte_val * 0x102C;
        route_heading_shifted = (tmp + ((tmp >> 31) & 0xFF)) >> 8;
    }
    /* Polarity = gActorRouteDirectionPolarity (dword 0x3F) per
     * UpdateTrafficRoutePlan @ 0x00435ef4. Prior port read dword 0x25 which
     * has no references in the original — the polarity branch was reading
     * a zero-init field that was never written. */
    polarity = rs[RS_ROUTE_DIRECTION_POLARITY];

    /* See 0x435EFD-0x435F18:
     *   ECX = (heading - route_heading) - 0x800           ; (signed)
     *   ECX = ECX & 0xFFF
     *   EAX = (ECX + 0xFFFFF800) & 0xFFF  ; = (ECX - 0x800) & 0xFFF
     *   EAX = (-EAX) & 0xFFF
     * which is the "negated heading delta" used in the polarity==0 test. */
    {
        uint32_t ecx = ((uint32_t)(heading_shifted - route_heading_shifted)) - 0x800u;
        uint32_t eax;
        ecx &= 0xFFFu;
        eax = (ecx - 0x800u) & 0xFFFu;
        eax = ((uint32_t)(-(int32_t)eax)) & 0xFFFu;
        hdelta_neg = (int32_t)eax;
    }

    if (polarity == 0) {
        hdelta = hdelta_neg;
    } else {
        /* 0x435F69: polarity != 0 path subtracts another 0x800 (= flip 180 deg). */
        hdelta = (int32_t)(((uint32_t)hdelta_neg - 0x800u) & 0xFFFu);
    }

    /* Original 0x435F2B JLE 0x400 + 0x435F32 JGE 0xC00 implement strict bounds:
     * body fires for hdelta in (0x400, 0xC00) exclusive. */
    if (hdelta > 0x400 && hdelta < 0xC00) {
        /* [FAITHFUL 0x435F2B] The original arms UNCONDITIONALLY here — no
         * `recovery_stage == 0` guard (the port-only guard was removed with the
         * decrement above). Re-arming each tick to a fresh (world_z & 7)|1 value
         * matches the original; the latch stays non-zero until queue-recycle. */
        /* Original at 0x435F34/0x435F81: reads DAT_004ab30c = actor[0].world_pos_z
         * (g_actor_base + 0x204). Used as a pseudo-random source for the
         * recovery stage. */
        int32_t seed_src = 0;
        if (g_actor_base) {
            seed_src = ACTOR_I32(actor_ptr(0), ACTOR_WORLD_POS_Z);
        }
        g_traffic_recovery_stage[slot] = (int)(seed_src & 7);
        if (g_traffic_recovery_stage[slot] == 0)
            g_traffic_recovery_stage[slot] = 1;
        /* Observability (fix-1780404735): rate-limited. The original re-arms
         * UNCONDITIONALLY every tick a car is in the (0x400,0xC00) heading band,
         * so a stuck (e.g. remap-miss / origin-placed) actor arms every frame —
         * log at most every 30 frames per the bail log to avoid flooding while
         * keeping the arm→brake→recycle cycle visible for diagnosis. */
        if ((g_ai_frame_counter % 30u) == 0u) {
            TD5_LOG_I(LOG_TAG,
                      "traffic_recovery_arm: slot=%d hdelta=0x%X polarity=%d stage=%d "
                      "span_raw=%d (brakes until recycle >0x28 spans behind player)",
                      slot, hdelta, polarity, g_traffic_recovery_stage[slot],
                      (int)ACTOR_I16(actor, ACTOR_SPAN_RAW));
        }
        /* Slot 9 special-encounter audio cleanup. */
        if (slot == 9) {
            if (polarity == 0) {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio @ 0x00440AE0 */
                    td5_sound_stop_tracked_vehicle_audio();
                }
                /* 0x435F5D: only the polarity==0 branch clears the handle. */
                g_encounter_tracked_handle = -1;
            } else {
                if (!g_td5.wanted_mode_enabled) {
                    /* StopTrackedVehicleAudio — handle NOT cleared in this branch. */
                    td5_sound_stop_tracked_vehicle_audio();
                }
            }
        }
    }

    /* --- Stage 3: Edge-of-track / recovery bail-out ---
     * [CONFIRMED @ 0x00435FAA-0x00436019]
     * Original condition (Ghidra):
     *   sVar5 = field_0x80 (ACTOR_SPAN_RAW)   (param_1 actor)
     *   bail if: ((sVar5 < 3 || g_trackTotalSpanCount - 8 <= sVar5) &&
     *             rs[RS_ROUTE_TABLE_SELECTOR] == 0)
     *         || rs[RS_SCRIPT_SPEED_PARAM] != 0   (g_actor_traffic_recovery_stage)
     *         || g_traffic_recovery_stage[slot] != 0   (DAT_004afbe8) */
    {
        int16_t span_raw = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int ring_length = td5_track_get_ring_length();
        /* [dynamic-traffic OnCircuits] the near-edge brake is P2P track-END
         * logic (orig g_trackTotalSpanCount-8 guard); on a CIRCUIT spans
         * [ring-8..ring)+[0..3) are just the lap line and braking there would
         * stall every traffic car once per lap. Inert for faithful mode —
         * the original never has circuit traffic to begin with. */
        int near_edge = (g_td5.track_type != TD5_TRACK_CIRCUIT) &&
                        (span_raw < 3 || (ring_length > 0 && span_raw >= ring_length - 8));
        int on_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);

        if ((near_edge && on_canonical)
            || rs[RS_SCRIPT_SPEED_PARAM] != 0
            || g_traffic_recovery_stage[slot] != 0) {
            /* LAB_004366c7: brake = 1, encounter_steer = 0, return */
            ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
            ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
            /* Observability (fix-1780404735): rate-limited so a latched actor
             * braking-until-recycle is visible without per-tick spam. Only logs
             * the recovery-latch cause (not the faithful near-edge/script bails). */
            if (g_traffic_recovery_stage[slot] != 0 &&
                (g_ai_frame_counter % 30u) == 0u) {
                TD5_LOG_I(LOG_TAG,
                          "traffic_recovery_bail: slot=%d stage=%d span_raw=%d "
                          "(braking, awaiting recycle)",
                          slot, g_traffic_recovery_stage[slot], (int)span_raw);
            }
            return;
        }
    }

    /* --- Stage 4: Normal driving -- cruise throttle (orig constant 0x3C=60) ---
     * S06 2026-06-04: derive the cruise throttle from the traffic vehicle's OWN
     * carparam top speed so different traffic cars move at car-appropriate speeds
     * instead of one difficulty-independent constant. The traffic dynamics are a
     * simplified force balance (no torque curve / +0x74 cap), so we scale the
     * emergent cruise command by the car's top-speed ratio vs a baseline, clamped
     * to a sane band. Gated by [GameOptions] AIAccelFromCar; falls back to the
     * faithful 0x3C when off or the carparam is unavailable. */
    int cruise = 0x3C;
    if (g_td5.ini.ai_accel_from_car) {
        int top = td5_physics_get_carparam_top_speed(slot);
        if (top > 0) {
            const int baseline_top = 950;   /* ~mid of the carparam top-speed range */
            int scaled = (0x3C * top) / baseline_top;
            if (scaled < 0x2A) scaled = 0x2A;   /* ~0.70x floor */
            if (scaled > 0x4E) scaled = 0x4E;   /* ~1.30x ceiling */
            cruise = scaled;
        }
    }
    /* [dynamic-traffic] cruise-speed scale ([Traffic] SpeedScale %, default
     * 150). The traffic dynamics (IntegrateVehicleFrictionForces port) feed
     * throttle = cruise*4 into a linear force balance, so the emergent top
     * speed tracks this scale. Applied BEFORE the SmartAI car-following ease
     * so queueing behind a slower car still slows the follower. */
    if (td5_ai_traffic_dynamic_active()) {
        cruise = (cruise * g_td5.ini.traffic_dyn_speed_pct) / 100;
        if (cruise > 0xB4) cruise = 0xB4;   /* 3x faithful — int16 cmd, sane ceiling */
    }
    /* [SmartAI] car-following: ease the cruise throttle when a car sits close
     * ahead in the same lane so traffic queues instead of nose-to-tail tunnelling.
     * Scope choice = opponents + traffic, so traffic shares the smart brain. */
    if (td5_ai_smart_active()) {
        int cscale = td5_ai_smart_traffic_cruise_scale(slot);
        cruise = (cruise * cscale) >> 8;
        if (cruise < 0) cruise = 0;
    }
    ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)cruise;
    ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;

    /* RAY BRAIN for traffic: slow into bends and brake if aimed at a wall, using
     * the same forward ray sensor as the racers. Floors keep traffic rolling;
     * lane selection still comes from the S20 / smart_traffic_lane chooser. */
    if (td5_ai_smart_active() && g_td5.ini.smart_ai_rays) {
        int tsc = td5_track_get_span_count();
        if (tsc > 0) {
            int   tspan = (int)ACTOR_I16(actor, ACTOR_SPAN_RAW);
            float tsk   = td5_ai_smart_skill(slot);
            SmartCorner tco; smart_corner_eval(slot, tspan, tsc, 0.5, tsk, &tco);
            if (tco.speed_cap < 0.999) {
                int cc = (int)((double)cruise * tco.speed_cap);
                if (cc < 0x14) cc = 0x14;     /* keep traffic moving */
                cruise = cc;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)cruise;
            }
            SmartSense tse; smart_sense(slot, tspan, tsc, tsk, &tse);
            if (tse.wall_imminent) {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG)       = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = 0;
                if ((g_ai_frame_counter % 90u) == 0u)
                    TD5_LOG_I(LOG_TAG, "smart_ray_traffic_wall: slot=%d span=%d front=%.0f",
                              slot, tspan, tse.front_clear);
            }
        }
    }

    /* --- Stage 5: Compute target span and deviation
     * [CONFIRMED @ 0x00435F0A-0x004364CA]
     *
     * Original UpdateTrafficRoutePlan computes LEFT/RIGHT deviation to the
     * next-span waypoint and writes them to route_state +0x58/+0x5c (=
     * RS_LEFT_DEVIATION/RS_RIGHT_DEVIATION). UpdateActorSteeringBias @
     * 0x004340C0 then reads these and writes steering_command (+0x30C).
     *
     * CRITICAL: the original calls InitActorTrackSegmentPlacement @ 0x00445F10
     * for the target point, NOT SampleTrackTargetPoint @ 0x00434800. These are
     * two DIFFERENT geometries:
     *   - InitActorTrackSegmentPlacement: 4-vertex barycenter of the sub_lane
     *     cell at (target_span, sub_lane_index). Uses DAT_00474E40 (spawn
     *     table). Traffic uses this. Port equivalent: td5_track_get_span_lane_world.
     *   - SampleTrackTargetPoint: route_byte interpolation between left rail
     *     and left+lane_count vertex of the whole span. Uses DAT_00473C68 (AI
     *     target table). AI racers use this. Port equivalent: td5_track_sample_target_point.
     *
     * The prior port routed traffic through sample_target_point, so traffic
     * aimed at the road center instead of its assigned sub-lane. On multi-lane
     * Moscow segments this pulled the chasing target into the opposite lane,
     * which drives the bicycle-model steering_command toward the neighbouring
     * rail and clips walls. The original also does NOT apply RS_TRACK_OFFSET_BIAS
     * (peer-avoidance perpendicular offset) to the traffic target — that is a
     * racer-only behaviour inside 0x00434800.
     *
     * Traffic peek-ahead is span+1 forward / span-1 reverse (racer uses span+4).
     *
     * BUG FIXED (2026-04-25): The original uses field_0x82 (ACTOR_SPAN_NORMALIZED)
     * for the jump-table lookup, NOT field_0x80 (ACTOR_SPAN_RAW). On branch roads
     * the raw span holds the branch index (>= ring_length) while the normalized
     * span is the main-road equivalent — using raw caused the remap walker to miss
     * every entry (branch indices are always outside the main-road range_lo values).
     * [CONFIRMED @ 0x00435F6A: Ghidra "iVar14 = field_0x82; iVar8 = iVar14 + 1"]
     *
     * BUG FIXED (2026-04-25): When the remap walker finds a branch match AND the
     * actor's raw span (field_0x80) is still on main road (< ring_length) AND the
     * branch target span is also on main road (<= ring_length), the original adjusts
     * sub_lane by (current_sub_lane - cur_span_lane_count). This accounts for the
     * lane-count difference between the junction span and the branch entry span so
     * that the traffic actor aims at the correct sub_lane cell.
     * [CONFIRMED @ 0x004361C0-0x004361D8: Ghidra "local_4 -= uVar6" path]
     *
     * Sub_lane ±1 adjustments at 0x004361B8/0x0043627C only fire on lane-count
     * mismatches between adjacent strips, which are rare. */
    {
        int16_t span_norm = ACTOR_I16(actor, ACTOR_SPAN_NORMALIZED); /* field_0x82 */
        int16_t span_raw  = ACTOR_I16(actor, ACTOR_SPAN_RAW);        /* field_0x80 */
        int span_count = td5_track_get_span_count();
        int ring_length = td5_track_get_ring_length();
        if (span_count > 0 && span_norm >= 0) {
            int lookup_span;    /* junction lookup span (norm-based, matches orig 0x00435F6A) */
            int fallback_span;  /* no-junction-match target (raw-based, matches orig 0x0043614c /
                                 * 0x004362C6 — `iVar8 = iVar10 + 1` / `iVar8 = iVar7 - 1` where
                                 * iVar10/iVar7 = span_raw). */
            if (polarity == 0) {
                /* [CONFIRMED @ 0x00435F6A] iVar14=field_0x82; iVar8 = iVar14 + 1 */
                lookup_span = ((int)span_norm + 1) % span_count;
                /* [CONFIRMED @ 0x0043614c] iVar10 = field_0x80 (span_raw); iVar8 = iVar10 + 1 */
                fallback_span = ((int)span_raw + 1) % span_count;
                if (fallback_span < 0) fallback_span += span_count;
            } else {
                lookup_span = (int)span_norm - 1;
                if (lookup_span < 0) lookup_span += span_count;
                /* [CONFIRMED @ 0x004362C6] iVar7 = field_0x80; iVar8 = iVar7 - 1 */
                fallback_span = (int)span_raw - 1;
                if (fallback_span < 0) fallback_span += span_count;
                if (fallback_span >= span_count) fallback_span -= span_count;
            }

            int is_canonical = (rs[RS_ROUTE_TABLE_SELECTOR] == 0);
            int remapped = td5_track_apply_target_span_remap(lookup_span,
                                                              is_canonical);
            /* [BUGFIX 2026-05-26 traffic-steer-saturation] When junction remap
             * does NOT fire (remapped == lookup_span), orig uses span_raw±1 as
             * target, NOT span_norm±1. Previously port used norm-based target
             * unconditionally; when actor's span_norm lagged span_raw (e.g. after
             * lap wrap or because normalize_wrap isn't called per-tick for traffic),
             * the target stayed anchored behind the actor's actual position →
             * deviation grew → steering cascade hit ±0x18000 emergency-snap and
             * saturated at -0x18000 within ~13s. Verified via Frida
             * tools/frida_traffic_compare.js: pre-fix slot 6 had 89.5% of ticks
             * at |steer|>50000 vs orig ~131 avg. */
            int remap_fired = (remapped != lookup_span);
            int target_span = remap_fired ? remapped : fallback_span;

            /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part A] A car physically ON a
             * branch road (span_raw >= ring) overshoots the norm-based remap
             * target: span_norm lags span_raw there, so the target stays at the
             * car's own span and flips behind it -> ±0x18000 snap -> wall. Target
             * the ADJACENT branch span by raw so it advances WITH the car. Only
             * branch traffic; main-road cars keep the faithful norm-based remap. */
            int traffic_on_branch =
                branch_traffic_fix_enabled() && slot >= g_traffic_slot_base &&
                ring_length > 0 && (int)span_raw >= ring_length && span_count > 0;
            if (traffic_on_branch) {
                int adj = (polarity == 0) ? ((int)span_raw + 1)
                                          : ((int)span_raw - 1);
                if (adj < 0)           adj += span_count;
                if (adj >= span_count) adj -= span_count;
                target_span = adj;
            }

            /* Target sub_lane: start with current sub_lane (default path). */
            int target_sub_lane = (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX);

            /* [CONFIRMED @ 0x004361B8-0x004361D8]
             * When the remap found a branch target AND the actor is still on the
             * main road (raw < ring_length) AND the target is also within main road
             * bounds (target <= ring_length), adjust sub_lane by subtracting the
             * current span's lane count. This shifts the target cell to match
             * the branch entry sub_lane index.
             * Condition "iVar7 <= g_trackTotalSpanCount" in Ghidra uses <=, meaning
             * the branch target must be <= ring_length (inclusive, since ring_length
             * is the first branch index). */
            if (remap_fired &&                      /* junction remap fired */
                (int)span_raw < ring_length &&       /* actor on main road */
                target_span <= ring_length) {        /* target on/at branch edge */
                int cur_lane_count = td5_track_get_span_lane_count((int)span_norm);
                if (cur_lane_count > 0 && target_sub_lane >= cur_lane_count) {
                    target_sub_lane -= cur_lane_count;
                }
            }

            /* [S20 smart-traffic] AvoidSlowLane + situational lane change:
             * adjust the faithful target sub-lane toward a faster / clearer
             * lane. No-op when disabled / single-lane.
             *
             * [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part C] Skip the chooser on a
             * branch road: it flip-flops the target sub-lane 0<->1 tick to tick,
             * and on a narrow (2-3 lane) branch that lateral jitter jerks the
             * 1-span-lookahead steering and the car fishtails into the rail. Hold
             * the car's current lane there; the wall-nudge below still keeps an
             * edge lane off the rail, and branches are short so we lose nothing. */
            if (!traffic_on_branch) {
                if (td5_ai_smart_active()) {
                    /* [SmartAI] unified lane brain for traffic: score lanes by
                     * surface/occupancy/wall/change-cost (±1 step). Replaces the
                     * S20 react-to-nearest-peer chooser. */
                    target_sub_lane = td5_ai_smart_traffic_lane(
                        slot, target_span,
                        td5_track_get_span_lane_count(target_span),
                        target_sub_lane, polarity);
                } else {
                    target_sub_lane = traffic_smart_choose_lane(
                        slot, target_span,
                        td5_track_get_span_lane_count(target_span), target_sub_lane);
                }
            }

            /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21] The held branch lane must be
             * valid for the raw-based target span (raw±1 may have a different
             * lane count than the car's current span). */
            if (traffic_on_branch) {
                int blc = td5_track_get_span_lane_count(target_span);
                if (blc > 0) {
                    if (target_sub_lane < 0)    target_sub_lane = 0;
                    if (target_sub_lane >= blc) target_sub_lane = blc - 1;
                }
            }

            /* [task#18 2026-06-12] TD6: keep traffic off the sidewalk. The lane
             * choosers above can pick any lane 0..lane_count-1, but on wide TD6
             * city strips only the central route band is paved. Clamp the target
             * lane into the drivable band derived from the route tables (no-op on
             * native TD5 and on branch spans without route coverage). */
            {
                int blo, bhi;
                if (td5_ai_td6_drivable_band(
                        target_span,
                        td5_track_get_span_lane_count(target_span),
                        &blo, &bhi)) {
                    if (target_sub_lane < blo) target_sub_lane = blo;
                    else if (target_sub_lane > bhi) target_sub_lane = bhi;
                }
            }

            int target_x = 0, target_y = 0, target_z = 0;

            if (td5_track_get_span_lane_world(target_span, target_sub_lane,
                                              &target_x, &target_y, &target_z)) {
                /* [S20 smart-traffic] WallAvoid: nudge an edge-lane target
                 * toward the lane interior so the car stops scraping the rail.
                 * Interior lanes are untouched. No-op when disabled. */
                traffic_smart_wall_nudge(
                    target_span, target_sub_lane,
                    td5_track_get_span_lane_count(target_span),
                    &target_x, &target_y, &target_z);
                /* [CONFIRMED @ 0x00436344-0x004363EF] Original re-reads
                 *   EAX = rs[RS_SLOT_INDEX] (= ref_slot)
                 *   ESI = ref_actor.world_pos_x   (offset 0x1FC, DAT_004ab304)
                 *   EAX = ref_actor.world_pos_z   (offset 0x204, DAT_004ab30c)
                 *   EBP = &ref_actor              (DAT_004ab108 + ref_slot*0x388)
                 * before computing target_angle and combined heading.
                 * For traffic ref_slot == slot, but follow the original
                 * dispatch verbatim. */
                int32_t actor_x = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_X);
                int32_t actor_z = ACTOR_I32(ref_actor, ACTOR_WORLD_POS_Z);
                int32_t dx = (target_x - actor_x) >> 8;
                int32_t dz = (target_z - actor_z) >> 8;

                int32_t target_angle = ai_angle_from_vector(dx, dz) & 0xFFF;

                int32_t yaw   = ACTOR_I32(ref_actor, ACTOR_YAW_ACCUM);
                int32_t steer = ACTOR_I32(ref_actor, ACTOR_STEERING_CMD);
                int32_t actor_heading = ((yaw + steer) >> 8) & 0xFFF;

                /* [RIGHT-BRANCH TRAFFIC FIX 2026-06-21 Part B] Reject an
                 * ORPHAN/garbage target span whose lane geometry returns a point
                 * hundreds of thousands of units away (the dead spans between the
                 * main road and the branch dst ranges). A real 1-span lookahead is
                 * < ~6000u; past 20000u is bogus -> hold heading (delta 0 = steer
                 * straight) this tick so the car coasts across the transition
                 * instead of snapping the wheel to ±0x18000 into the wall. */
                if (branch_traffic_fix_enabled()) {
                    long md = (long)(dx < 0 ? -dx : dx) +
                              (long)(dz < 0 ? -dz : dz);
                    if (md > 20000) target_angle = actor_heading;
                }

                int32_t delta = actor_heading - target_angle;
                int32_t abs_delta = delta < 0 ? -delta : delta;
                if (delta >= 0) {
                    rs[RS_LEFT_DEVIATION]  = 0xFFF - abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta;
                } else {
                    rs[RS_LEFT_DEVIATION]  = abs_delta;
                    rs[RS_RIGHT_DEVIATION] = delta + 0xFFF;
                }

                if ((g_ai_frame_counter % 60u) == 0u) {
                    TD5_LOG_I(LOG_TAG,
                              "traffic_dev: slot=%d raw=%d norm=%d tspan=%d sublane=%d "
                              "ta=0x%X hd=0x%X delta=%d L=%d R=%d",
                              slot, (int)span_raw, (int)span_norm, target_span,
                              target_sub_lane, target_angle, actor_heading, delta,
                              rs[RS_LEFT_DEVIATION], rs[RS_RIGHT_DEVIATION]);
                }
            }
        }
    }

    /* --- Stage 6: Steering --- */
    td5_ai_update_steering_bias(rs, td5_ai_td6_steer_weight(0x8000));  /* [task#19] TD6 traffic too */

    /* --- Stage 7: Peer avoidance / yield --- */
    peer = td5_ai_find_nearest_route_peer(rs);

    /* [S20 smart-traffic] default: no lane change this tick (cleared unless a
     * close same-lane peer triggers react_to_peer below). */
    if (slot >= 0 && slot < TD5_MAX_TOTAL_ACTORS)
        s_traffic_lane_bias[slot] = 0;

    if (peer != slot) {
        /* Peer found — faithful TTC formula [CONFIRMED @ 0x4364E0–0x43656E] */
        char *peer_actor = actor_ptr(peer);
        int16_t self_span = ACTOR_I16(actor, ACTOR_SPAN_RAW);
        int16_t peer_span = ACTOR_I16(peer_actor, ACTOR_SPAN_RAW);
        int32_t self_speed, peer_speed, speed_delta;
        int32_t span_diff_dir; /* direction-adjusted span difference (raw spans) */
        int32_t iVar14;        /* combined progress delta: (spans * 0x100) */
        int32_t iVar7, iVar13; /* intermediate TTC and final TTC */
        int32_t speed_shifted; /* self_speed >> 10 with arithmetic rounding */
        int     smart_ease = 0; /* 1 = a clear lane exists; ease instead of hard brake */

        self_speed  = g_actor_forward_track_component[slot];
        peer_speed  = g_actor_forward_track_component[peer];
        speed_delta = self_speed - peer_speed; /* iVar8 in original */

        /* Direction-adjusted span difference */
        if (polarity == 0) {
            span_diff_dir = (int32_t)peer_span - (int32_t)self_span;
        } else {
            span_diff_dir = (int32_t)self_span - (int32_t)peer_span;
        }

        /* [S20 smart-traffic] Lookahead: when the nearest peer is close AHEAD in
         * our lane and a clear adjacent lane exists, set up a lane change (next
         * tick) and ease the hard brake instead of ramming/stopping.
         * span_diff_dir > 0 means the peer is in FRONT of us in our travel
         * direction — we only dodge what we can see. A peer BEHIND us
         * (span_diff_dir <= 0) is left to the faithful path: a driver can't see
         * behind, so traffic must not swerve for a car overtaking from the rear. */
        if (span_diff_dir > 0 && span_diff_dir < 8 && self_speed > 0) {
            smart_ease = traffic_smart_react_to_peer(
                slot, (int)self_span,
                (int)ACTOR_U8(actor, ACTOR_SUB_LANE_INDEX),
                td5_track_get_span_lane_count((int)self_span),
                peer, polarity);
        }

        /* Proximity gate [CONFIRMED @ 0x43646A]: if peer within 4 spans and moving, brake now.
         * [#18 2026-06-16 FIX] The faithful intent is "within 4 spans", but the port
         * checked only the LOWER bound (> -4) — so a peer up to ~30 spans AHEAD in the
         * same lane tripped this emergency brake, and traffic chain-stopped behind any
         * car ahead however far. Restore the near-zone upper bound; farther peers fall
         * through to the TTC formula below, which brakes only when actually closing.
         * Env TD5RE_TRAFFIC_BRAKE_NEAR (default 4) tunes the gate width. */
        if (span_diff_dir > -4 && span_diff_dir < trf_brake_near_spans() && self_speed > 0) {
            /* [traffic pile-up] A peer directly AHEAD within the minimum gap
             * overrides the smart-ease lane change: brake now and keep distance.
             * Easing here (brake off, gentle throttle) would roll us onto the peer
             * this tick because the lane change has a 1-tick latency — that is the
             * "traffic driving onto each other" the user reported. Once there is
             * room (gap > min), the ease/lane-change path resumes. */
            int too_close = (span_diff_dir > 0 && span_diff_dir <= trf_min_gap_spans());
            if (smart_ease && !too_close) {
                /* Clear lane found AND enough room — keep rolling (gentle throttle)
                 * while the lane bias above steers around the obstacle next tick. */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x28; /* eased cruise */
            } else {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
                TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d proximity gate span_diff=%d too_close=%d",
                          slot, span_diff_dir, too_close);
            }
            goto ttc_done;
        }

        /* Combined progress delta (sub_progress approximated as 0) */
        iVar14 = span_diff_dir * 0x100;

        /* TTC formula [CONFIRMED @ 0x4364EE]:
         * iVar7 = ((iVar14 * peer_speed * 0x5DC) / speed_delta + iVar14 * 0x5DC) / self_speed
         * Right-shift by 8 with arithmetic rounding → iVar13 */
        if (speed_delta != 0 && self_speed != 0) {
            int64_t tmp = ((int64_t)iVar14 * peer_speed * 0x5DC) / speed_delta
                        + (int64_t)iVar14 * 0x5DC;
            iVar7  = (int32_t)(tmp / self_speed);
            iVar13 = (iVar7 + ((int32_t)iVar7 >> 31 & 0xFF)) >> 8;
        } else {
            iVar13 = 0x2EE00; /* no closing rate — far away */
        }

        /* self_speed arithmetic right-shift by 10 [CONFIRMED @ 0x436561] */
        speed_shifted = (self_speed + (self_speed >> 31 & 0x3FF)) >> 10;

        /* Brake condition [CONFIRMED @ 0x436561–0x43656E]:
         * ttc - 8 <= speed_shifted  &&  ttc >= 0  &&  self_speed > 0 */
        if (iVar13 - 8 <= speed_shifted && iVar13 > -1 && self_speed > 0) {
            /* [traffic pile-up] Same minimum-gap floor as the proximity gate: a
             * peer within the min gap directly ahead brakes regardless of a
             * found lane, so closing traffic keeps distance instead of overlapping. */
            int too_close = (span_diff_dir > 0 && span_diff_dir <= trf_min_gap_spans());
            if (smart_ease && !too_close) {
                /* [S20 smart-traffic] ease instead of hard brake — a clear lane
                 * exists and there is room, so slow gently and steer around next tick. */
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 0;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0x28;
            } else {
                ACTOR_U8(actor, ACTOR_BRAKE_FLAG) = 1;
                ACTOR_I16(actor, ACTOR_ENCOUNTER_STEER) = (int16_t)0xFF00; /* -256 */
                TD5_LOG_I(LOG_TAG, "ttc_brake: slot=%d ttc=%d spd_shifted=%d", slot, iVar13, speed_shifted);
            }
        }
    }
ttc_done:;
}

