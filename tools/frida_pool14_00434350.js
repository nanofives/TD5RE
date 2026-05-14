/**
 * frida_pool14_00434350.js — Input/output capture for
 * InitializeActorTrackPose @ 0x00434350.
 *
 * Pilot for the precise-port workflow (re/analysis/precise_port_workflow.md).
 * One row per call. Captures ALL 6 racer slots because this function fires
 * once per slot during InitializeRaceSession (sim_tick = 0 / pre-tick).
 *
 * Pool tag: pool14
 * Output:   C:\Users\maria\Desktop\Proyectos\TD5RE\log\orig\pool14_00434350.csv
 *
 * Function signature (from Ghidra):
 *   void __cdecl InitializeActorTrackPose(
 *       uint  param_1,  // slot id
 *       short param_2,  // span_raw
 *       char  param_3,  // sub_lane (signed)
 *       int   param_4)  // flip flag (always 0 in observed binary)
 *
 * Captures inputs (params + actor state at entry) and outputs (actor+0x1F4
 * yaw_accum + actor+0x8c sub_lane post-clamp + per-slot
 * gActorTrackSpanProgress[slot*0x47] and DAT_004afb84[slot*0x47]).
 *
 * Globals:
 *   g_actorRuntimeState.slot @ 0x004AB108  stride 0x388
 *   g_trackStripRecords      @ 0x004C3D9C  (record array; entry stride 0x18)
 *   g_trackVertexPool        @ 0x004C3D98  (int16[3] per vertex, stride 6)
 *   gActorTrackSpanProgress  @ 0x004AFBC4  stride 0x11C (RS dword 0x19 offset)
 *   gActorRouteStateTable    @ 0x004AFB60  stride 0x11C (RS dword 0x00)
 *   DAT_004AFB84             @ 0x004AFB84  stride 0x11C (RS dword 0x09)
 */
"use strict";

var OUT = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\orig\\pool14_00434350.csv";

var ACTOR_BASE   = ptr(0x004AB108);
var ACTOR_STRIDE = 0x388;
var RS_BASE      = ptr(0x004AFB60);
var RS_STRIDE    = 0x11C;
var RS_PROGRESS_OFF = 0x64;  /* RS dword 0x19 * 4 = 0x64 → gActorTrackSpanProgress */
var RS_BIAS_OFF     = 0x24;  /* RS dword 0x09 * 4 = 0x24 → DAT_004AFB84 */

/* g_trackStripRecords / g_trackVertexPool are POINTERS in .data — read indirect. */
var P_stripRecords = ptr(0x004C3D9C);
var P_vertexPool   = ptr(0x004C3D98);

/* Actor field offsets */
var O_SPAN_RAW        = 0x080;
var O_SPAN_NORMALIZED = 0x082;
var O_SPAN_ACCUM      = 0x084;
var O_SPAN_HIGH       = 0x086;
var O_SUB_LANE        = 0x08C;
var O_YAW_ACCUM       = 0x1F4;
var O_WORLD_POS_X     = 0x1FC;
var O_WORLD_POS_Y     = 0x200;
var O_WORLD_POS_Z     = 0x204;

function actorOfSlot(s) { return ACTOR_BASE.add(s * ACTOR_STRIDE); }
function rsOfSlot(s)    { return RS_BASE.add(s * RS_STRIDE); }

/* Strip record layout (24 bytes / 0x18):
 *   byte 0 : span_type
 *   byte 3 : lane_count nibble (low 4 bits)
 *   u16 +4 : left_vertex_index
 *   u16 +6 : right_vertex_index
 */
function readSpanRecord(span_idx) {
    var base = P_stripRecords.readPointer().add(span_idx * 0x18);
    return {
        span_type:          base.readU8(),
        lane_count_nibble:  base.add(3).readU8() & 0x0F,
        left_vertex_index:  base.add(4).readU16(),
        right_vertex_index: base.add(6).readU16(),
    };
}

function readVertexXZ(vidx) {
    /* Vertex pool stride = 6 (int16 x, y, z). */
    var base = P_vertexPool.readPointer().add(vidx * 6);
    return {
        x: base.add(0).readS16(),
        z: base.add(4).readS16(),
    };
}

/* Write header. */
var fp = new File(OUT, "w");
fp.write([
    /* keys */
    "call_idx","slot",
    /* params */
    "param_span","param_sub_lane","param_flip",
    /* span record */
    "span_type","lane_count_nibble","left_vi","right_vi",
    /* relevant vertices */
    "vl0_x","vl0_z","vl1_x","vl1_z","vl2_x","vl2_z",
    "vr0_x","vr0_z","vr1_x","vr1_z","vr2_x","vr2_z",
    /* outputs at exit */
    "actor_span_raw","actor_span_norm","actor_span_accum","actor_span_high",
    "actor_sub_lane_post",
    "actor_yaw_accum",
    "actor_world_x","actor_world_y","actor_world_z",
    "rs_track_progress","rs_track_offset_bias"
].join(",") + "\n");
fp.flush();

var call_idx = 0;

Interceptor.attach(ptr(0x00434350), {
    onEnter: function(args) {
        /* cdecl: [ESP+0]=ret, [ESP+4..0x10] params. */
        var esp = this.context.esp;
        var slot = esp.add(4).readU32();
        var span = esp.add(8).readS16();          /* word at +0x8 */
        var sub_lane = esp.add(0xC).readS8();     /* byte at +0xC */
        var flip = esp.add(0x10).readS32();

        this.slot = slot;
        this.param_span = span;
        this.param_sub_lane = sub_lane;
        this.param_flip = flip;
        this.actor = actorOfSlot(slot);
        this.rs = rsOfSlot(slot);
        this.call_idx = call_idx++;

        /* Capture span-record + vertex inputs BEFORE call. */
        try {
            this.rec = readSpanRecord(span);
        } catch (e) {
            this.rec = {span_type: -1, lane_count_nibble: -1, left_vertex_index: -1, right_vertex_index: -1};
        }
        var lvi = this.rec.left_vertex_index, rvi = this.rec.right_vertex_index;
        function safeVtx(idx) {
            try { return readVertexXZ(idx); } catch (e) { return {x: 0, z: 0}; }
        }
        this.vl0 = safeVtx(lvi);
        this.vl1 = safeVtx(lvi + 1);
        this.vl2 = safeVtx(lvi + 2);
        this.vr0 = safeVtx(rvi);
        this.vr1 = safeVtx(rvi + 1);
        this.vr2 = safeVtx(rvi + 2);
    },
    onLeave: function() {
        var actor = this.actor;
        var rs = this.rs;
        var row = [
            this.call_idx, this.slot,
            this.param_span, this.param_sub_lane, this.param_flip,
            this.rec.span_type, this.rec.lane_count_nibble,
            this.rec.left_vertex_index, this.rec.right_vertex_index,
            this.vl0.x, this.vl0.z, this.vl1.x, this.vl1.z, this.vl2.x, this.vl2.z,
            this.vr0.x, this.vr0.z, this.vr1.x, this.vr1.z, this.vr2.x, this.vr2.z,
            actor.add(O_SPAN_RAW).readS16(),
            actor.add(O_SPAN_NORMALIZED).readS16(),
            actor.add(O_SPAN_ACCUM).readS16(),
            actor.add(O_SPAN_HIGH).readS16(),
            actor.add(O_SUB_LANE).readU8(),
            actor.add(O_YAW_ACCUM).readS32(),
            actor.add(O_WORLD_POS_X).readS32(),
            actor.add(O_WORLD_POS_Y).readS32(),
            actor.add(O_WORLD_POS_Z).readS32(),
            rs.add(RS_PROGRESS_OFF).readS32(),
            rs.add(RS_BIAS_OFF).readS32()
        ].join(",");
        fp.write(row + "\n");
        fp.flush();
    }
});

send("[pool14_00434350 probe attached]");
