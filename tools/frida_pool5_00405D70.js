/**
 * frida_pool5_00405D70.js — Per-call input/output capture for
 * ResetVehicleActorState @ 0x00405D70.
 *
 * Pilot for the precise-port workflow (re/analysis/precise_port_workflow.md).
 * 1 row per call, keyed by (sim_tick, slot, caller_ra). Only slot 0 emitted.
 *
 * Pool tag: pool5
 * Output:   C:\Users\maria\Desktop\Proyectos\TD5RE\log\orig\pool5_00405D70.csv
 *
 * Function signature:
 *   void __cdecl ResetVehicleActorState(RuntimeSlotActor *actor)
 *
 * Listing facts (Ghidra 2026-05-14):
 *   - 54 instructions / 25 unique stores + 1 CALL to 0x00405E80.
 *   - Writes ONLY:
 *     +0x376 (surface_contact_flags), +0x379 (vehicle_mode),
 *     +0x1C0..1D4 (ang_vel + lin_vel, 6 dwords; re-zeroed selectively after CALL),
 *     +0x338 (frame_counter, int16),
 *     +0x37C (wheel_contact_bitmask, byte; NOT damage_lockout at +0x37D),
 *     +0x200 = 0xC0000000 (world_pos.y sentinel),
 *     +0x36B = 0x02 (current_gear),
 *     +0x310 = 0x190 (engine_speed_accum),
 *     +0x2DC..2E8 (wheel_suspension_pos[4]),
 *     +0x2EC..2F8 (wheel_spring_dv[4]),
 *     +0x208/+0x20A/+0x20C (display_angles roll/yaw/pitch — yaw = int16(euler_accum.yaw>>8)),
 *     +0x1F0/+0x1F8 (euler_accum roll, pitch — NOT yaw),
 *     +0x144/+0x148/+0x14C (render_pos.x/y/z via FILD+FMUL[1/256]+FSTP),
 *     CALL IntegrateVehiclePoseAndContacts (settles world_pos.y from sentinel),
 *     +0x2FC..308 (wheel_load_accum[4]).
 *
 * Callers (6, recorded via caller_ra):
 *   0x00409520 CheckAndUpdateActorCollisionAlignment
 *   0x00409D20 IntegrateScriptedVehicleMotion
 *   0x00434350 InitializeActorTrackPose          ← primary spawn caller
 *   0x00434DA0 UpdateSpecialTrafficEncounter
 *   0x004353B0 RecycleTrafficActorFromQueue
 *   0x00435940 InitializeTrafficActorsFromQueue
 */
"use strict";

var OUT = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\orig\\pool5_00405D70.csv";
var ACTOR_BASE   = ptr(0x004AB108);
var ACTOR_STRIDE = 0x388;
var TARGET_SLOT  = 0;

var G_simTick = ptr(0x004AADA0);
var G_paused  = ptr(0x004AAD60);

/* Actor field offsets touched by ResetVehicleActorState. */
var O_RENDER_POS    = 0x144;     /* float[3] */
var O_ANG_VEL_ROLL  = 0x1C0;
var O_ANG_VEL_YAW   = 0x1C4;
var O_ANG_VEL_PITCH = 0x1C8;
var O_LIN_VEL_X     = 0x1CC;
var O_LIN_VEL_Y     = 0x1D0;
var O_LIN_VEL_Z     = 0x1D4;
var O_EULER_ROLL    = 0x1F0;
var O_EULER_YAW     = 0x1F4;
var O_EULER_PITCH   = 0x1F8;
var O_WORLD_POS_X   = 0x1FC;
var O_WORLD_POS_Y   = 0x200;
var O_WORLD_POS_Z   = 0x204;
var O_DISP_ROLL     = 0x208;
var O_DISP_YAW      = 0x20A;
var O_DISP_PITCH    = 0x20C;
var O_SUSP_POS      = 0x2DC;     /* int32 stride 4, 4 entries */
var O_SPRING_DV     = 0x2EC;
var O_LOAD_ACCUM    = 0x2FC;
var O_ENGINE        = 0x310;
var O_FRAME_COUNTER = 0x338;
var O_GEAR          = 0x36B;
var O_SURF_FLAGS    = 0x376;
var O_VEH_MODE      = 0x379;
var O_WCB           = 0x37C;
var O_DAM_LOCKOUT   = 0x37D;

var fp = new File(OUT, "w");
fp.write([
    "sim_tick","paused","slot","caller_ra","actor_addr",
    /* PRE-call inputs that survive into post-state */
    "world_pos_x_pre","world_pos_y_pre","world_pos_z_pre",
    "euler_yaw_pre","dam_lockout_pre",
    /* POST-call outputs (function's final state) */
    "surface_contact_flags","vehicle_mode",
    "ang_vel_roll","ang_vel_yaw","ang_vel_pitch",
    "lin_vel_x","lin_vel_y","lin_vel_z",
    "frame_counter","wheel_contact_bitmask","damage_lockout",
    "world_pos_y_post","current_gear","engine_speed",
    "susp_pos_0","susp_pos_1","susp_pos_2","susp_pos_3",
    "spring_dv_0","spring_dv_1","spring_dv_2","spring_dv_3",
    "load_accum_0","load_accum_1","load_accum_2","load_accum_3",
    "render_pos_x","render_pos_y","render_pos_z",
    "euler_roll_post","euler_yaw_post","euler_pitch_post",
    "disp_roll","disp_yaw","disp_pitch"
].join(",") + "\n");
fp.flush();

Interceptor.attach(ptr(0x00405D70), {
    onEnter: function(args) {
        /* cdecl: [ESP+0]=ret, [ESP+4]=actor.
         * Interceptor fires BEFORE prologue, so ESP still points at return-addr. */
        var actor = this.context.esp.add(4).readPointer();

        var rel = actor.sub(ACTOR_BASE).toInt32();
        var slot = rel / ACTOR_STRIDE;
        if (slot < 0 || slot >= 6 || (rel % ACTOR_STRIDE) !== 0) {
            this.skip = true; return;
        }
        if (slot !== TARGET_SLOT) { this.skip = true; return; }

        this.skip = false;
        this.actor = actor;
        this.slot  = slot;
        this.tick  = G_simTick.readS32();
        this.paused = G_paused.readS32();
        this.caller_ra = this.context.esp.readPointer().toString();

        /* PRE-state — captures what reset SEES on entry */
        this.world_pos_x_pre = actor.add(O_WORLD_POS_X).readS32();
        this.world_pos_y_pre = actor.add(O_WORLD_POS_Y).readS32();
        this.world_pos_z_pre = actor.add(O_WORLD_POS_Z).readS32();
        this.euler_yaw_pre   = actor.add(O_EULER_YAW).readS32();
        this.dam_lockout_pre = actor.add(O_DAM_LOCKOUT).readU8();
    },
    onLeave: function() {
        if (this.skip) return;
        var actor = this.actor;

        var susp = [0,1,2,3].map(function(w){ return actor.add(O_SUSP_POS  + w*4).readS32(); });
        var sdv  = [0,1,2,3].map(function(w){ return actor.add(O_SPRING_DV + w*4).readS32(); });
        var lacc = [0,1,2,3].map(function(w){ return actor.add(O_LOAD_ACCUM+ w*4).readS32(); });

        var row = [
            this.tick, this.paused, this.slot, this.caller_ra, this.actor.toString(),
            this.world_pos_x_pre, this.world_pos_y_pre, this.world_pos_z_pre,
            this.euler_yaw_pre, this.dam_lockout_pre,
            actor.add(O_SURF_FLAGS).readU8(),
            actor.add(O_VEH_MODE).readU8(),
            actor.add(O_ANG_VEL_ROLL).readS32(),
            actor.add(O_ANG_VEL_YAW).readS32(),
            actor.add(O_ANG_VEL_PITCH).readS32(),
            actor.add(O_LIN_VEL_X).readS32(),
            actor.add(O_LIN_VEL_Y).readS32(),
            actor.add(O_LIN_VEL_Z).readS32(),
            actor.add(O_FRAME_COUNTER).readS16(),
            actor.add(O_WCB).readU8(),
            actor.add(O_DAM_LOCKOUT).readU8(),
            actor.add(O_WORLD_POS_Y).readS32(),
            actor.add(O_GEAR).readU8(),
            actor.add(O_ENGINE).readS32(),
            susp[0], susp[1], susp[2], susp[3],
            sdv[0],  sdv[1],  sdv[2],  sdv[3],
            lacc[0], lacc[1], lacc[2], lacc[3],
            actor.add(O_RENDER_POS + 0).readFloat(),
            actor.add(O_RENDER_POS + 4).readFloat(),
            actor.add(O_RENDER_POS + 8).readFloat(),
            actor.add(O_EULER_ROLL).readS32(),
            actor.add(O_EULER_YAW).readS32(),
            actor.add(O_EULER_PITCH).readS32(),
            actor.add(O_DISP_ROLL).readS16(),
            actor.add(O_DISP_YAW).readS16(),
            actor.add(O_DISP_PITCH).readS16()
        ].join(",");
        fp.write(row + "\n");
        fp.flush();
    }
});

send("[pool5_00405D70 probe attached @ slot " + TARGET_SLOT + "]");
