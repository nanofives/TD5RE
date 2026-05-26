/**
 * frida_traffic_compare.js — Paired Frida probe for traffic spawn + per-tick
 * state across orig (TD5_d3d.exe) and port (td5re.exe). Used to compare
 * traffic behavior (spawn positions, direction, lane, speed) head-to-head.
 *
 * Mode auto-detected by which module is present.
 *
 * Captures three event types in a single CSV:
 *   - event="init_leave"   : after InitializeTrafficActorsFromQueue — snapshot
 *                            all traffic slots (6..11)
 *   - event="recycle_leave": after RecycleTrafficActorFromQueue — snapshot all
 *                            traffic slots (only one slot changed but we dump
 *                            all so the diff sees full state)
 *   - event="tick"         : on entry to IntegrateVehicleFrictionForces /
 *                            td5_physics_update_traffic — one row per call
 *                            per traffic actor per tick
 *
 * Field offsets (actor base, 0x388 stride):
 *   +0x080 track_span_raw            int16
 *   +0x082 track_span_normalized     int16
 *   +0x084 track_span_accumulated    int16
 *   +0x08C track_sub_lane_index      uint8
 *   +0x1C4 angular_velocity_yaw      int32
 *   +0x1CC linear_velocity_x         int32
 *   +0x1D0 linear_velocity_y         int32
 *   +0x1D4 linear_velocity_z         int32
 *   +0x1F4 euler_accum.yaw           int32   (direction baked in: +0x80000)
 *   +0x1FC world_pos.x               int32   (24.8)
 *   +0x200 world_pos.y               int32
 *   +0x204 world_pos.z               int32
 *   +0x30C steering_command          int32
 *   +0x314 longitudinal_speed        int32   (signed 24.8 body-frame)
 *   +0x33E encounter_steering_cmd    int16
 */
"use strict";

/* ------------------------------------------------------------------------
 * Per-run overrides (frida_differential_capture.py rewrites these in place)
 * ------------------------------------------------------------------------ */

var OUTPUT_PATH  = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\traffic_compare.csv";
var BINARY_LABEL = "auto";   /* "orig" | "port" | "auto" — auto detects */

/* Port RVA overrides (frida_differential_capture.py resolves these via nm.exe
 * before each port capture and rewrites them in place). td5re.exe function
 * addresses shift every rebuild, so hardcoding goes stale. -1 = "use the
 * built-in fallback below" (only correct for the build these were captured on). */
var PORT_RVA_INIT_TRAFFIC = -1;  /* _td5_ai_init_traffic_actors    */
var PORT_RVA_RECYCLE      = -1;  /* _td5_ai_recycle_traffic_actor  */
var PORT_RVA_FRICTION     = -1;  /* _td5_physics_update_traffic    */
var PORT_RVA_ACTOR_BASE   = -1;  /* _s_actor_memory                */
var PORT_RVA_SIM_TICK     = -1;  /* _g_tick_counter                */
var PORT_RVA_PAUSED       = -1;  /* _g_game_paused                 */

/* ------------------------------------------------------------------------
 * Mode + address table
 * ------------------------------------------------------------------------ */

var origMod = Process.findModuleByName("TD5_d3d.exe");
var portMod = Process.findModuleByName("td5re.exe");

var MODE, MOD, ADDR;

if (origMod) {
    MODE = "orig";
    MOD  = origMod;
    ADDR = {
        actor_base       : ptr(0x004AB108),
        sim_tick         : ptr(0x004AADA0),
        paused           : ptr(0x004AAD60),
        fn_init_traffic  : ptr(0x00435940),
        fn_recycle       : ptr(0x004353B0),
        fn_friction      : ptr(0x004438F0),
    };
} else if (portMod) {
    MODE = "port";
    MOD  = portMod;
    /* Port addresses (PE absolute @ ImageBase 0x00400000). td5re.exe is
     * marked DYNAMIC_BASE so resolve via module.base + RVA. RVAs come from
     * the override block above (nm-resolved by the capture script); the
     * fallbacks are only valid for the build that hardcoded them. */
    var base = portMod.base;
    var rva = function(override, fallback) {
        return base.add(override >= 0 ? override : fallback);
    };
    ADDR = {
        actor_base       : rva(PORT_RVA_ACTOR_BASE,   0xF2800),
        sim_tick         : rva(PORT_RVA_SIM_TICK,     0xF2778),
        paused           : rva(PORT_RVA_PAUSED,       0xF75D0),
        fn_init_traffic  : rva(PORT_RVA_INIT_TRAFFIC, 0x223D0),
        fn_recycle       : rva(PORT_RVA_RECYCLE,      0x21FA0),
        fn_friction      : rva(PORT_RVA_FRICTION,     0xB280),
    };
} else {
    throw new Error("No TD5 binary found (TD5_d3d.exe or td5re.exe)");
}

if (BINARY_LABEL === "auto") BINARY_LABEL = MODE;

/* ------------------------------------------------------------------------
 * Constants + helpers
 * ------------------------------------------------------------------------ */

var ACTOR_STRIDE = 0x388;
var TRAFFIC_SLOT_MIN = 6;
var TRAFFIC_SLOT_MAX = 11;

var O_SPAN_RAW   = 0x080;
var O_SPAN_NORM  = 0x082;
var O_SPAN_ACC   = 0x084;
var O_SUB_LANE   = 0x08C;
var O_ANG_YAW    = 0x1C4;
var O_LIN_X      = 0x1CC;
var O_LIN_Y      = 0x1D0;
var O_LIN_Z      = 0x1D4;
var O_EULER_YAW  = 0x1F4;
var O_POS_X      = 0x1FC;
var O_POS_Y      = 0x200;
var O_POS_Z      = 0x204;
var O_STEER      = 0x30C;
var O_LONG_SPD   = 0x314;
var O_ENC_STEER  = 0x33E;

function actorOf(slot) { return ADDR.actor_base.add(slot * ACTOR_STRIDE); }

function slotOfActor(actor) {
    var off = actor.sub(ADDR.actor_base).toInt32();
    if (off < 0 || (off % ACTOR_STRIDE) !== 0) return -1;
    var slot = off / ACTOR_STRIDE;
    if (slot < 0 || slot > 11) return -1;
    return slot;
}

function snap(actor) {
    return {
        span_raw  : actor.add(O_SPAN_RAW).readS16(),
        span_norm : actor.add(O_SPAN_NORM).readS16(),
        span_acc  : actor.add(O_SPAN_ACC).readS16(),
        sub_lane  : actor.add(O_SUB_LANE).readU8(),
        ang_yaw   : actor.add(O_ANG_YAW).readS32(),
        lin_x     : actor.add(O_LIN_X).readS32(),
        lin_y     : actor.add(O_LIN_Y).readS32(),
        lin_z     : actor.add(O_LIN_Z).readS32(),
        euler_yaw : actor.add(O_EULER_YAW).readS32(),
        pos_x     : actor.add(O_POS_X).readS32(),
        pos_y     : actor.add(O_POS_Y).readS32(),
        pos_z     : actor.add(O_POS_Z).readS32(),
        steer     : actor.add(O_STEER).readS32(),
        long_spd  : actor.add(O_LONG_SPD).readS32(),
        enc_steer : actor.add(O_ENC_STEER).readS16(),
    };
}

var COLS = [
    "binary", "event", "sim_tick", "paused", "slot",
    "span_raw", "span_norm", "span_acc", "sub_lane",
    "pos_x", "pos_y", "pos_z",
    "lin_x", "lin_y", "lin_z",
    "euler_yaw", "ang_yaw",
    "long_spd", "steer", "enc_steer"
];

var fp = new File(OUTPUT_PATH, "w");
fp.write(COLS.join(",") + "\n");
fp.flush();

function emit(event, slot, s) {
    var tick   = ADDR.sim_tick.readS32();
    var paused = ADDR.paused.readS32();
    fp.write([
        BINARY_LABEL, event, tick, paused, slot,
        s.span_raw, s.span_norm, s.span_acc, s.sub_lane,
        s.pos_x, s.pos_y, s.pos_z,
        s.lin_x, s.lin_y, s.lin_z,
        s.euler_yaw, s.ang_yaw,
        s.long_spd, s.steer, s.enc_steer
    ].join(",") + "\n");
}

function emitAllTraffic(event) {
    for (var slot = TRAFFIC_SLOT_MIN; slot <= TRAFFIC_SLOT_MAX; slot++) {
        emit(event, slot, snap(actorOf(slot)));
    }
    fp.flush();
}

/* ------------------------------------------------------------------------
 * Hooks
 * ------------------------------------------------------------------------ */

/* Bulk spawn — fires once per race start; dump all traffic slots after. */
Interceptor.attach(ADDR.fn_init_traffic, {
    onLeave: function() { emitAllTraffic("init_leave"); }
});

/* Recycle — fires whenever a traffic actor drops behind player + new queue
 * entry consumed. We dump all 6 traffic slots so the diff can see which
 * slot moved (and the rest stay consistent). */
Interceptor.attach(ADDR.fn_recycle, {
    onLeave: function() { emitAllTraffic("recycle_leave"); }
});

/* Per-tick traffic friction call. Once per traffic actor per tick.
 * Orig: __stdcall, arg0 at [ESP+4] (interceptor before prologue).
 * Port: __cdecl, arg0 at [ESP+4]. Same in both cases. */
Interceptor.attach(ADDR.fn_friction, {
    onEnter: function(args) {
        var actor = this.context.esp.add(4).readPointer();
        var slot = slotOfActor(actor);
        if (slot < TRAFFIC_SLOT_MIN || slot > TRAFFIC_SLOT_MAX) { this.skip = true; return; }
        this.skip = false;
        emit("tick", slot, snap(actor));
    },
    onLeave: function() {
        if (!this.skip) fp.flush();
    }
});

send("[traffic_compare mode=" + MODE + " label=" + BINARY_LABEL + " out=" + OUTPUT_PATH + "]");
