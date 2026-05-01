// susp_probe_original.js
// Per-tick suspension state probe for original TD5_d3d.exe.
//
// Hooks IntegrateWheelSuspensionTravel @ 0x00403A20.
// Function signature (Ghidra-confirmed 2026-04-26): __cdecl (actor, tuning_ptr, vel_x, vel_z)
//   [esp+4]  = actor_ptr  (in actor table 0x4AB108..0x4AC650)
//   [esp+8]  = tuning_ptr (game data pointer — skip)
//   [esp+12] = vel_x      (iVar11 from caller — int32, world-space X velocity delta)
//   [esp+16] = vel_z      (iVar36 from caller — int32, world-space Z velocity delta)
// Calling convention auto-detected: checks ECX first (thiscall), falls back to [esp+4] (cdecl).
//
// Writes POST-integration wheel state to log/susp_probe_original.csv.
//
// Run alongside the quickrace --trace flag (provides AUTO_THROTTLE):
//   cd C:/Users/maria/Desktop/Proyectos/TD5RE
//   python re/tools/quickrace/td5_quickrace.py ^
//     --trace --trace-max-frames 0 --trace-max-sim-tick 4000 --trace-auto-exit ^
//     --set race.track=15 --set race.car=0 --set race.start_span_offset=0 ^
//     --extra-script re/trace-hooks/susp_probe_original.js

"use strict";

var OUT_PATH   = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\susp_probe_original.csv";
var SLOT_FILTER    = 0;     // only log slot 0 (player actor)
var MAX_SIM_TICK   = 5000;  // flush+stop beyond this (launcher kills at 4000)

var BASE = ptr(0x00400000);

var ADDR_INTEGRATE_SUSP = BASE.add(0x00003A20);  // 0x00403A20

// Global state addresses (confirmed from frida_race_trace.js)
var G_simTick    = BASE.add(0x0AADA0);  // 0x4AADA0  int32 simulationTickCounter
var G_gameState  = BASE.add(0x0C3CE8);  // 0x4C3CE8  int32 (2 = RACE)
var G_gamePaused = BASE.add(0x0AAD60);  // 0x4AAD60  int32

// Actor table
var ACTOR_BASE   = BASE.add(0x0AB108);  // 0x4AB108
var ACTOR_STRIDE = 0x388;
var ACTOR_TABLE_END = ACTOR_BASE.add(6 * ACTOR_STRIDE);  // exclusive

// Actor struct offsets (from td5_actor_struct.h, all [CONFIRMED])
var OFF_SLOT      = 0x375;  // uint8   slot_index
var OFF_SUSP_POS  = 0x2DC;  // int32[4] wheel_suspension_pos   FL=0 FR=1 RL=2 RR=3
var OFF_SPRING_DV = 0x2EC;  // int32[4] wheel_spring_dv
var OFF_LOAD_ACC  = 0x2FC;  // int32[4] wheel_load_accum
var OFF_AV_ROLL   = 0x1C0;  // int32   angular_velocity_roll
var OFF_AV_PITCH  = 0x1C8;  // int32   angular_velocity_pitch
var OFF_DISP_ROLL = 0x208;  // int16   display_angles.roll
var OFF_DISP_PITCH= 0x20C;  // int16   display_angles.pitch
var OFF_CONTACT   = 0x37D;  // uint8   wheel_contact_bitmask
var OFF_WORLD_Y   = 0x200;  // int32   world_pos.y (24.8 FP)

var fh       = null;
var stopped  = false;
var flushCtr = 0;
var convDetected = null;   // 'thiscall' | 'cdecl' — set on first valid call

function inActorTable(p) {
    var lo = ACTOR_BASE.compare(p);
    var hi = ACTOR_TABLE_END.compare(p);
    return (lo <= 0 && hi > 0);
}

function openCSV() {
    try {
        fh = new File(OUT_PATH, "w");
        fh.write(
            "sim_tick,conv,vel_x,vel_z," +
            "susp0,susp1,susp2,susp3," +
            "dv0,dv1,dv2,dv3," +
            "load0,load1,load2,load3," +
            "contact_mask,av_roll,av_pitch,disp_roll,disp_pitch,world_y\n"
        );
        fh.flush();
        send({kind: 'log', msg: '[susp_probe] CSV opened: ' + OUT_PATH});
    } catch (e) {
        send({kind: 'log', msg: '[susp_probe] ERROR opening CSV: ' + e});
    }
}

Interceptor.attach(ADDR_INTEGRATE_SUSP, {
    onEnter: function(args) {
        this.active = false;
        if (stopped || !fh) return;
        if (G_gameState.readS32() !== 2) return;
        if (G_gamePaused.readS32() !== 0) return;

        // Detect calling convention on first call:
        // Priority 1: ECX in actor table → thiscall
        // Priority 2: [esp+4] in actor table → cdecl
        var actorPtr = null;
        var conv = convDetected;

        if (conv === null) {
            // Auto-detect
            var ecxPtr = this.context.ecx;
            try {
                if (inActorTable(ecxPtr)) {
                    conv = 'thiscall';
                }
            } catch(e) {}
            if (conv === null) {
                try {
                    var cdeclPtr = ptr(this.context.esp.add(4).readU32());
                    if (inActorTable(cdeclPtr)) {
                        conv = 'cdecl';
                    }
                } catch(e) {}
            }
            if (conv !== null) {
                convDetected = conv;
                send({kind: 'log', msg: '[susp_probe] detected calling convention: ' + conv});
            }
        }

        if (conv === 'thiscall') {
            actorPtr = this.context.ecx;
            try {
                var slotByte = actorPtr.add(OFF_SLOT).readU8();
                if (slotByte !== SLOT_FILTER) return;
            } catch(e) { return; }
            // thiscall: (vel_x, vel_z) at [esp+4],[esp+8]  (tuning_ptr follows ECX separately)
            this.velX = this.context.esp.add(4).readS32();
            this.velZ = this.context.esp.add(8).readS32();
        } else if (conv === 'cdecl') {
            try {
                actorPtr = ptr(this.context.esp.add(4).readU32());
                var slotByte = actorPtr.add(OFF_SLOT).readU8();
                if (slotByte !== SLOT_FILTER) return;
            } catch(e) { return; }
            // cdecl 4-arg: [esp+4]=actor, [esp+8]=tuning_ptr(skip), [esp+12]=vel_x, [esp+16]=vel_z
            this.velX = this.context.esp.add(12).readS32();
            this.velZ = this.context.esp.add(16).readS32();
        } else {
            return;  // convention not yet detected
        }

        var tick = G_simTick.readS32();
        if (MAX_SIM_TICK > 0 && tick > MAX_SIM_TICK) {
            if (!stopped) {
                stopped = true;
                try { fh.flush(); fh.close(); } catch(e2) {}
                fh = null;
                send({kind: 'log', msg: '[susp_probe] MAX_SIM_TICK reached, closed.'});
            }
            return;
        }

        this.active  = true;
        this.actor   = actorPtr;
        this.simTick = tick;
        this.conv    = conv;
    },

    onLeave: function(retval) {
        if (!this.active || stopped || !fh) return;

        var a = this.actor;
        try {
            var row =
                this.simTick + "," + this.conv + "," +
                this.velX + "," + this.velZ + "," +
                a.add(OFF_SUSP_POS +  0).readS32() + "," +
                a.add(OFF_SUSP_POS +  4).readS32() + "," +
                a.add(OFF_SUSP_POS +  8).readS32() + "," +
                a.add(OFF_SUSP_POS + 12).readS32() + "," +
                a.add(OFF_SPRING_DV +  0).readS32() + "," +
                a.add(OFF_SPRING_DV +  4).readS32() + "," +
                a.add(OFF_SPRING_DV +  8).readS32() + "," +
                a.add(OFF_SPRING_DV + 12).readS32() + "," +
                a.add(OFF_LOAD_ACC  +  0).readS32() + "," +
                a.add(OFF_LOAD_ACC  +  4).readS32() + "," +
                a.add(OFF_LOAD_ACC  +  8).readS32() + "," +
                a.add(OFF_LOAD_ACC  + 12).readS32() + "," +
                a.add(OFF_CONTACT).readU8()     + "," +
                a.add(OFF_AV_ROLL).readS32()    + "," +
                a.add(OFF_AV_PITCH).readS32()   + "," +
                a.add(OFF_DISP_ROLL).readS16()  + "," +
                a.add(OFF_DISP_PITCH).readS16() + "," +
                a.add(OFF_WORLD_Y).readS32()    + "\n";

            fh.write(row);
            flushCtr++;
            if (flushCtr >= 50) {
                fh.flush();
                flushCtr = 0;
            }
        } catch(e) {
            send({kind: 'log', msg: '[susp_probe] read error @ tick ' + this.simTick + ': ' + e});
        }
    }
});

openCSV();
send({kind: 'log', msg: '[susp_probe] hook installed @ ' + ADDR_INTEGRATE_SUSP});
