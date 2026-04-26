/**
 * frida_ai_probe.js — Capture per-slot per-tick AI internals from TD5_d3d.exe
 *
 * Writes log/ai_probe_original.csv with one row per call to
 * UpdateActorTrackBehavior (0x00434FE0) and one row per call to
 * UpdateActorRouteThresholdState (0x00434AA0).
 *
 * Fields per row:
 *   fn, sim_tick, slot, actor, yaw, steer, world_x, world_z, long_speed,
 *   left_dev, right_dev, retval
 *
 * Purpose: diff against port's internal AI values to find the first tick
 * where cascade inputs diverge.
 */
"use strict";

var OUTPUT_PATH = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\ai_probe_original.csv";

var BASE = ptr(0x00400000);
var ADDR_UpdateActorTrackBehavior    = ptr(0x00434FE0);
var ADDR_UpdateActorRouteThresholdState = ptr(0x00434AA0);
var ADDR_UpdateActorSteeringBias     = ptr(0x004340C0);
var ADDR_AngleFromVector12           = ptr(0x0040A720);
var UAB_RA_LO = 0x00434FE0;
var UAB_RA_HI = 0x00435500;   // upper bound of UpdateActorTrackBehavior body

var G_simulationTickCounter = ptr(0x004AADA0);
var ACTOR_BASE   = ptr(0x004AB108);
var ACTOR_STRIDE = 0x388;
var ROUTE_STATE_BASE   = ptr(0x004AFBB8);
var ROUTE_STATE_STRIDE = 0x11C;

var OFF_WORLD_X    = 0x1FC;
var OFF_WORLD_Z    = 0x204;
var OFF_YAW_ACCUM  = 0x1F4;
var OFF_STEERING   = 0x30C;
var OFF_LONG_SPEED = 0x314;

function readS32(p) { return p.readS32(); }

function actorSlotFromPtr(actorPtr) {
    var delta = actorPtr.sub(ACTOR_BASE).toInt32();
    if (delta < 0) return -1;
    var slot = Math.floor(delta / ACTOR_STRIDE);
    if ((slot * ACTOR_STRIDE) !== delta) return -1;
    if (slot < 0 || slot > 11) return -1;
    return slot;
}

function routeStateSlotFromPtr(rsPtr) {
    var delta = rsPtr.sub(ROUTE_STATE_BASE).toInt32();
    if (delta < 0) return -1;
    var slot = Math.floor(delta / ROUTE_STATE_STRIDE);
    if ((slot * ROUTE_STATE_STRIDE) !== delta) return -1;
    if (slot < 0 || slot > 11) return -1;
    return slot;
}

function actorPtr(slot) { return ACTOR_BASE.add(slot * ACTOR_STRIDE); }
function routeStatePtr(slot) { return ROUTE_STATE_BASE.add(slot * ROUTE_STATE_STRIDE); }

// ============================================================================
// CSV setup
// ============================================================================

var fp = null;
try {
    fp = new File(OUTPUT_PATH, "w");
    fp.write("fn,sim_tick,slot,yaw,steer,world_x,world_z,long_speed,left_dev,right_dev,retval\n");
    fp.flush();
    console.log("[ai_probe] Writing " + OUTPUT_PATH);
} catch (e) {
    console.log("[ai_probe] FAILED to open " + OUTPUT_PATH + ": " + e);
}

function emitRow(fn, slot, retval) {
    if (!fp) return;
    var simTick = readS32(G_simulationTickCounter);
    var a = actorPtr(slot);
    var rs = routeStatePtr(slot);
    var yaw = readS32(a.add(OFF_YAW_ACCUM));
    var steer = readS32(a.add(OFF_STEERING));
    var wx = readS32(a.add(OFF_WORLD_X));
    var wz = readS32(a.add(OFF_WORLD_Z));
    var ls = readS32(a.add(OFF_LONG_SPEED));
    var ld = readS32(rs.add(0));
    var rd = readS32(rs.add(4));
    fp.write(fn + "," + simTick + "," + slot + "," + yaw + "," + steer + "," +
             wx + "," + wz + "," + ls + "," + ld + "," + rd + "," + retval + "\n");
}

// ============================================================================
// Hooks
// ============================================================================

// UpdateActorTrackBehavior — the port's equivalent takes (int slot). The
// original's calling convention: unclear — look at both ECX (fastcall) and
// [ESP+4] (stdcall/cdecl). Ghidra decomp shows the function uses
// `DAT_004afc48 + iVar10` indexing, where iVar10 = param_1 * 0x11C; that
// strongly suggests it takes a slot-index-derived offset, not a direct
// actor pointer. We capture raw arg on entry and resolve slot on leave.
Interceptor.attach(ADDR_UpdateActorTrackBehavior, {
    onEnter: function (args) {
        // __stdcall: param_1 at [esp+4]. `args[0]` already resolves that.
        this.arg0 = args[0];
        // Best-effort slot resolution: if arg0 looks like an actor ptr, use it;
        // if it's an int in [0,11], treat as slot; else brute-force via iVar10
        // by matching against known actor bases.
        var arg0int = this.arg0.toInt32 ? this.arg0.toInt32() : 0;
        var slot = -1;
        if (arg0int >= 0 && arg0int < 12) {
            slot = arg0int;
        } else {
            slot = actorSlotFromPtr(this.arg0);
            if (slot < 0) slot = routeStateSlotFromPtr(this.arg0);
        }
        this.slot = slot;
        g_currentSlot = slot;
    },
    onLeave: function () {
        if (this.slot >= 0 && this.slot < 6) {
            emitRow("track_behavior", this.slot, 0);
        }
        g_currentSlot = -1;
    }
});

Interceptor.attach(ADDR_UpdateActorRouteThresholdState, {
    onEnter: function (args) {
        this.arg0 = args[0];
        var arg0int = this.arg0.toInt32 ? this.arg0.toInt32() : 0;
        var slot = -1;
        if (arg0int >= 0 && arg0int < 12) slot = arg0int;
        else {
            slot = actorSlotFromPtr(this.arg0);
            if (slot < 0) slot = routeStateSlotFromPtr(this.arg0);
        }
        this.slot = slot;
    },
    onLeave: function (retval) {
        if (this.slot >= 0 && this.slot < 6) {
            emitRow("threshold", this.slot, retval.toInt32());
        }
    }
});

// UpdateActorSteeringBias — cascade entry. Capture left_dev/right_dev as
// CASCADE sees them, plus steering_weight (param_2). Tries stack args first,
// then falls back to a persistent "current slot" set by UpdateActorTrackBehavior
// since nested calls may use fastcall (ECX) instead of stack.
var g_currentSlot = -1;
Interceptor.attach(ADDR_UpdateActorSteeringBias, {
    onEnter: function (args) {
        // Try stack-arg first.
        this.rs = args[0];
        this.weight = args[1].toInt32 ? args[1].toInt32() : 0;
        var slotFromArg = routeStateSlotFromPtr(this.rs);
        if (slotFromArg < 0) {
            // Fastcall: ECX holds rs, EDX holds weight.
            var ecxRs = this.context.ecx;
            slotFromArg = routeStateSlotFromPtr(ecxRs);
            if (slotFromArg >= 0) {
                this.rs = ecxRs;
                this.weight = this.context.edx.toInt32();
            }
        }
        if (slotFromArg < 0) slotFromArg = g_currentSlot;
        this.slot = slotFromArg;
    },
    onLeave: function () {
        if (this.slot >= 0 && this.slot < 6 && fp) {
            var simTick = readS32(G_simulationTickCounter);
            var a = actorPtr(this.slot);
            var rs = routeStatePtr(this.slot);
            var yaw = readS32(a.add(OFF_YAW_ACCUM));
            var steer = readS32(a.add(OFF_STEERING));
            var wx = readS32(a.add(OFF_WORLD_X));
            var wz = readS32(a.add(OFF_WORLD_Z));
            var ls = readS32(a.add(OFF_LONG_SPEED));
            var ld = readS32(rs.add(0));
            var rd = readS32(rs.add(4));
            fp.write("cascade," + simTick + "," + this.slot + "," + yaw + "," +
                     steer + "," + wx + "," + wz + "," + ls + "," + ld + "," +
                     rd + "," + this.weight + "\n");
        }
    }
});

// Hook AngleFromVector12 with return-address filter → emits one row per call
// originating from UpdateActorTrackBehavior (the AI target-angle compute).
// Captures (arg0=dx, arg1=dz, retval=angle) so we can diff against port's
// target_probe log.
var ADDR_AngleFromVector12_str = ADDR_AngleFromVector12.toString();
var afvFp = null;
try {
    afvFp = new File("C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\ai_angle_probe_original.csv", "w");
    afvFp.write("sim_tick,caller_ra,dx,dz,angle\n");
    afvFp.flush();
} catch (e) {
    console.log("[ai_probe] angle probe open failed: " + e);
}

Interceptor.attach(ADDR_AngleFromVector12, {
    onEnter: function (args) {
        // Return address is at [esp] on entry (cdecl).
        var ra = this.context.esp.readPointer().toInt32();
        // Only record calls from inside UpdateActorTrackBehavior body.
        if (ra < UAB_RA_LO || ra > UAB_RA_HI) {
            this.record = false;
            return;
        }
        this.record = true;
        this.ra = ra;
        this.dx = args[0].toInt32 ? args[0].toInt32() : 0;
        this.dz = args[1].toInt32 ? args[1].toInt32() : 0;
    },
    onLeave: function (retval) {
        if (!this.record || !afvFp) return;
        var simTick = readS32(G_simulationTickCounter);
        afvFp.write(simTick + ",0x" + this.ra.toString(16) + "," +
                    this.dx + "," + this.dz + "," + retval.toInt32() + "\n");
    }
});

console.log("[ai_probe] Hooks installed at track_behavior=0x434FE0 threshold=0x434AA0 cascade=0x4340C0 afv=0x40A720");

// Periodic flush — otherwise rows buffer until process exit.
var flushCount = 0;
var flushTimer = setInterval(function () {
    if (fp) {
        try { fp.flush(); } catch (e) {}
    }
    flushCount++;
}, 250);

// Graceful close on unload. Frida doesn't guarantee this fires on process kill
// but it helps when the launcher detaches cleanly.
Script.bindWeak(globalThis, function () {
    if (fp) {
        try { fp.flush(); fp.close(); } catch (e) {}
        fp = null;
    }
    clearInterval(flushTimer);
});
