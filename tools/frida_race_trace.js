/**
 * frida_race_trace.js -- Capture a race trace CSV from the original TD5_d3d.exe
 *
 * Hooks RunRaceFrame (0x42B580) and its key callees at the same stage boundaries
 * as the source port's td5_trace module, then writes a matching CSV to
 * log/race_trace_original.csv for differential comparison.
 *
 * Usage:
 *   frida -p <pid> -l tools/frida_race_trace.js
 *   frida -f TD5_d3d.exe -l tools/frida_race_trace.js
 *
 * Configuration (edit below):
 *   TRACE_SLOT      -1 = all slots, 0 = player only, N = specific slot
 *   TRACE_MAX_FRAMES  0 = unlimited, N = stop after N frames
 *   OUTPUT_PATH       where to write the CSV
 */

"use strict";

// ============================================================================
// Configuration
// ============================================================================

var TRACE_SLOT         = -1;     // -1 = all, 0 = player only, 1-5 = specific AI
var TRACE_MAX_FRAMES   = 3000;   // 0 = unlimited render frames (safety ceiling)
var TRACE_MAX_SIM_TICK = 0;      // 0 = unlimited; N = stop when g_simulationTickCounter >= N
                                 // (engine-clock cap; 450 = 15 s post-countdown at 30 Hz)
// Inner-tick hooks were historically disabled because early revisions crashed.
// Re-enabled 2026-04-13 after Ghidra confirmed all 3 target bodies are large
// enough for Frida's 5-byte trampoline (UpdateRaceActors=1569B, ResolveVehicleContacts=381B).
// NormalizeWrapState (57B) remains unhooked — the per-site comment flagging it
// as "called 6× per actor OUTSIDE the tick loop" is the real blocker, not the size.
// Safety guards (raceConfirmed + INNER_TICK_MAX_PER_FRAME) still in effect.
var ENABLE_INNER_TICK_HOOKS = true;

// Deterministic RNG seeding — the binary does NOT call srand from its own code;
// it maintains its own race-RNG scaffolding at three globals (confirmed via
// Ghidra symbol audit 2026-04-13). We overwrite those when raceConfirmed first
// flips, so slot-1+ spawn state becomes run-to-run identical.
// CRT _holdrand IS seeded — but by the quickrace hook (td5_quickrace_hook.js),
// which calls _srand(0x1A2B3C4D) @ RVA 0x4814a just before
// InitializeRaceSeriesSchedule. That timing is load-bearing: AI-car selection
// inside the schedule init uses rand(), and seeding at race-frame-begin (here)
// would be too late. Launcher auto-sets cfg.seed_crt when --trace is passed.
var DETERMINISTIC_SEED  = true;
var FIXED_SEED_VALUE    = 0x1A2B3C4D;   // arbitrary fixed 32-bit constant

// Brake/RPM per-frame capture (Item 3 from /fix Cluster 4). Writes a SECOND
// CSV with a narrow schema tuned for diffing brake-to-0 runs. Kept separate
// from race_trace_*.csv so its schema can evolve without breaking the main
// comparator's header-equality check.
var CAPTURE_BRAKE_TRACE = true;
var BRAKE_OUTPUT_PATH   = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\race_trace_brake_original.csv";

// V2V contact capture (Item 4). Event-driven: one row per
// CollectVehicleCollisionContacts invocation. Dumps the full 8-quad shorts
// array so contactData[2,3] "own_x/own_z" semantics can be resolved.
var CAPTURE_CONTACTS    = true;
var CONTACTS_OUTPUT_PATH = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\race_trace_contacts_original.csv";

// -- Call-trace hook specs (filled by the launcher via patch_trace_script) --
// Each entry: {name: str, original_rva: hex int, args: int (count, 0..8),
//              capture_return: bool}. `args` is the number of int32 stack
// args to capture (__stdcall/__cdecl assumed; on x86 args[0]..args[n-1] are
// at [esp+4] onward at Interceptor.attach onEnter).
// Emits rows to CALLS_OUTPUT_PATH keyed (sim_tick, fn_name, call_idx).
var HOOK_SPECS = [];
var CALLS_OUTPUT_PATH = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\calls_trace_original.csv";
// AUTO_THROTTLE re-added 2026-04-10 with the CORRECT bit mask.
// Per Ghidra decomp of UpdatePlayerVehicleControlState at 0x402E60 [CONFIRMED]:
//   0x01 = RIGHT
//   0x02 = LEFT
//   0x200 = HANDBRAKE(!)
//   0x400 = BRAKE
//   0x100000 = ACCELERATE (the real throttle)
//   0x8000000 = REVERSE
//   0x40000000 = ESCAPE
// Player 1 control bits are at g_playerControlBits = 0x482FFC.
// Without this, the original sits at idle during the capture window while
// the port (with [Trace] AutoThrottle=1) accelerates forward, producing a
// false world_x divergence.
var AUTO_THROTTLE = true;
// Absolute path so it works regardless of process CWD
var OUTPUT_PATH      = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\race_trace_original.csv";

// ============================================================================
// Original binary addresses (TD5_d3d.exe, image base 0x00400000)
// ============================================================================

var BASE = ptr(0x00400000);

// --- Functions (CALL targets from race-frame-hook-points.md) ---
var ADDR_RunRaceFrame           = ptr(0x42B580);
var ADDR_PollRaceSessionInput   = ptr(0x42C470);
var ADDR_UpdateRaceActors       = ptr(0x436A70);  // AI + track dispatcher
var ADDR_ResolveVehicleContacts = ptr(0x409150);  // collision
var ADDR_UpdateTireTrackPool    = ptr(0x43EB50);  // cosmetic
var ADDR_UpdateRaceOrder        = ptr(0x42F5B0);  // position sort
var ADDR_UpdateChaseCamera      = ptr(0x401590);  // camera
var ADDR_UpdateRaceParticles    = ptr(0x429790);  // VFX
var ADDR_NormalizeWrapState     = ptr(0x443FB0);  // wrap norm (last sim op)
var ADDR_EndRaceScene           = ptr(0x40AE00);  // end scene
var ADDR_UpdateVehicleAudioMix  = ptr(0x440B00);  // audio mix (near end of frame)
var ADDR_UpdateRaceCameraTransitionTimer = ptr(0x40A490);  // last per-tick callee before counter inc; safe 5-byte prologue
// Added 2026-04-13 for brake/RPM/contact capture (Cluster 4 harness upgrade).
var ADDR_UpdatePlayerVehicleDynamics     = ptr(0x404030);  // per-actor physics tick
var ADDR_UpdateEngineSpeedAccumulator    = ptr(0x42EDF0);  // RPM accumulator (callee of above)
var ADDR_CollectVehicleCollisionContacts = ptr(0x408570);  // per V2V pair contact solver

// --- Globals (from frame-timing-system.md + global-variable-catalog.md) ---
var G_gameState            = ptr(0x4C3CE8);   // dword
var G_gamePaused           = ptr(0x4AAD60);   // dword
var G_raceEndFadeState     = ptr(0x4C3D80);   // dword
var G_simTimeAccumulator   = ptr(0x4AAED0);   // uint32
var G_simTickBudget        = ptr(0x466E88);    // float
var G_simulationTickCounter= ptr(0x4AADA0);   // int
var G_normalizedFrameDt    = ptr(0x4AAD70);    // float
var G_instantFPS           = ptr(0x466E90);    // float
// 0x49522C is the frontend frame counter -- NOT incremented during race.
// Use a Frida-side counter instead, incremented each RunRaceFrame entry.
var fridaFrameCounter      = 0;
// Inner-tick hook safety: only fire after the first successful frame_begin in
// game_state==2. The sim-time accumulator can hold many ticks of residual on
// the first transition frame, which would flood Frida's I/O before actors are
// initialized. raceConfirmed flips to true on the first frame_begin, after which
// inner hooks are allowed. ticksThisFrameLimit caps per-frame I/O in case the
// accumulator burst is very large on later frames too.
var raceConfirmed          = false;
var raceFrameCount         = 0;     // frames since raceConfirmed (for auto-close)
var INNER_TICK_MAX_PER_FRAME = 8;   // ignore inner-hook calls beyond this count per frame
var G_viewCount            = ptr(0x4B1134);    // dword
var G_splitscreenCount     = ptr(0x4C3D44);    // dword
var G_cameraWorldPosFloat  = ptr(0x4AAFC4);    // float[3] (shared camera pos)
var G_camWorldPosFixed     = ptr(0x482F30);    // int[2][3] (per-viewport, 24.8 fixed)

// Game-level RNG seed globals (Ghidra-confirmed 2026-04-13; binary has NO
// `srand` symbol — these three addresses ARE the race-determinism scaffolding).
var G_randomSeedForRace        = ptr(0x4969D4);
var G_raceSessionRandomSeed    = ptr(0x4AAD64);
var G_raceRandomSeedTable      = ptr(0x4AADBC);

// Player 1 control-bits address, used by auto-throttle AND read by
// UpdatePlayerVehicleDynamics callers. We log the brake+throttle bits from
// here each time 0x404030 fires, to cross-reference with in-actor brake_flag.
var G_player1ControlBits       = ptr(0x482FFC);

// Race slot state table: 6 entries x 4 bytes (state, comp1, comp2, reserved)
var G_raceSlotStateTable   = ptr(0x4AADF4);

// Actor table
var ACTOR_BASE             = ptr(0x4AB108);
var ACTOR_STRIDE           = 0x388;

// Race order array (6 bytes)
var G_raceOrderArray       = ptr(0x4AE278);

// Countdown timer (camera transition active)
// This is stored in the camera state block; use g_gamePaused as proxy

// ============================================================================
// Helpers
// ============================================================================

var fileHandle = null;
var brakeFileHandle = null;
var contactsFileHandle = null;
var callsFileHandle = null;

// Per-sim-tick call-index table, reset when sim_tick changes. Mirrors the
// port's td5_trace.c s_call_idx_table logic so both sides assign the same
// call_idx for the Nth call to fn_name in sim_tick T.
var callIdxTick = -1;
var callIdxCounts = {};  // fn_name -> next index
function nextCallIdx(fnName, simTick) {
    if (simTick !== callIdxTick) {
        callIdxTick = simTick;
        callIdxCounts = {};
    }
    var c = callIdxCounts[fnName] || 0;
    callIdxCounts[fnName] = c + 1;
    return c;
}
var framesStarted = 0;
var lastFrameIndex = -1;
var enabled = false;
var simTicksThisFrame = 0;
// Flip once so RNG seeding only happens on the first race frame, not each one.
var rngSeededThisRace = false;

function actorSlotFromPtr(p) {
    // Return slot index 0..5 if p is in the actor table; -1 otherwise.
    try {
        var base = ACTOR_BASE;
        var delta = p.sub(base).toInt32();
        if (delta < 0) return -1;
        var slot = Math.floor(delta / ACTOR_STRIDE);
        if (slot < 0 || slot >= 6) return -1;
        if ((slot * ACTOR_STRIDE) !== delta) return -1; // misaligned → not a slot
        return slot;
    } catch (e) { return -1; }
}

function readU32(addr) { return addr.readU32(); }
function readS32(addr) { return addr.readS32(); }
function readS16(addr) { return addr.readS16(); }
function readU16(addr) { return addr.readU16(); }
function readU8(addr)  { return addr.readU8(); }
function readFloat(addr) { return addr.readFloat(); }

function actorPtr(slot) {
    return ACTOR_BASE.add(slot * ACTOR_STRIDE);
}

function slotStatePtr(slot) {
    return G_raceSlotStateTable.add(slot * 4);
}

function selectedSlot(slot) {
    if (TRACE_SLOT < 0) return true;
    return slot === TRACE_SLOT;
}

// ============================================================================
// CSV output
// ============================================================================

var CSV_HEADER =
    "frame,sim_tick,stage,kind,id," +
    "game_state,paused,pause_menu,fade_state,countdown_timer,sim_accum,sim_budget,frame_dt,instant_fps,viewport_count,split_mode,ticks_this_frame," +
    "slot_state,slot_comp1,slot_comp2,view_target," +
    "world_x,world_y,world_z,vel_x,vel_y,vel_z,ang_roll,ang_yaw,ang_pitch,disp_roll,disp_yaw,disp_pitch," +
    "span_raw,span_norm,span_accum,span_high,steer,engine,long_speed,lat_speed,front_slip,rear_slip,finish_time,accum_distance,pending_finish,gear,vehicle_mode,track_contact,wheel_mask,race_pos," +
    "metric_checkpoint,metric_mask,metric_norm_span,metric_timer,metric_display_pos,metric_speed_bonus,metric_top_speed," +
    "cam_world_x,cam_world_y,cam_world_z,cam_x,cam_y,cam_z\n";

function writeRow(line) {
    if (fileHandle !== null) {
        fileHandle.write(line);
    }
}

function beginFrame(frameIndex) {
    if (!enabled) return false;

    if (frameIndex !== lastFrameIndex) {
        lastFrameIndex = frameIndex;
        framesStarted++;
        if (TRACE_MAX_FRAMES > 0 && framesStarted > TRACE_MAX_FRAMES) {
            console.log("[trace] Reached frame limit (" + TRACE_MAX_FRAMES + "), stopping");
            shutdown();
            return false;
        }
    }
    return true;
}

function writePrefix(frameIndex, simTick, stage, kind, id) {
    return frameIndex + "," + simTick + "," + stage + "," + kind + "," + id + ",";
}

function writeFrameRow(frameIndex, simTick, stage) {
    var gs       = readS32(G_gameState);
    var paused   = readS32(G_gamePaused);
    var fadeState = 0; // G_raceEndFadeState address uncertain, zero for now
    var simAccum = readU32(G_simTimeAccumulator);
    var simBudget= readFloat(G_simTickBudget);
    var frameDt  = readFloat(G_normalizedFrameDt);
    var fps      = readFloat(G_instantFPS);
    var vpCount  = readS32(G_viewCount);
    var splitMode= readS32(G_splitscreenCount);

    var prefix = writePrefix(frameIndex, simTick, stage, "frame", 0);
    var line = prefix +
        gs + "," + paused + ",0," + fadeState + ",0," +
        simAccum + "," + simBudget.toFixed(6) + "," + frameDt.toFixed(6) + "," + fps.toFixed(6) + "," +
        vpCount + "," + splitMode + "," + simTicksThisFrame + "," +
        // actor fields (zeros for frame row)
        "0,0,0,0," +
        "0,0,0,0,0,0,0,0,0,0,0,0," +
        "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0," +
        "0,0,0,0,0,0,0," +
        // view fields (zeros)
        "0,0,0,0.000000,0.000000,0.000000\n";
    writeRow(line);
}

function writeActorRow(frameIndex, simTick, stage, slot) {
    var a = actorPtr(slot);
    var ss = slotStatePtr(slot);

    var slotState = readU8(ss);
    var comp1     = readU8(ss.add(1));
    var comp2     = readU8(ss.add(2));

    // view target: -1 if not viewed
    var viewTarget = -1;
    // We don't have the actorSlotForView address confirmed, so skip exact matching

    var worldX  = readS32(a.add(0x1FC));
    var worldY  = readS32(a.add(0x200));
    var worldZ  = readS32(a.add(0x204));
    var velX    = readS32(a.add(0x1CC));
    var velY    = readS32(a.add(0x1D0));
    var velZ    = readS32(a.add(0x1D4));
    var angRoll = readS32(a.add(0x1C0));
    var angYaw  = readS32(a.add(0x1C4));
    var angPitch= readS32(a.add(0x1C8));
    var dispRoll = readS16(a.add(0x208));
    var dispYaw  = readS16(a.add(0x20A));
    var dispPitch= readS16(a.add(0x20C));
    var spanRaw  = readS16(a.add(0x080));
    var spanNorm = readS16(a.add(0x082));
    var spanAccum= readS16(a.add(0x084));
    var spanHigh = readS16(a.add(0x086));
    var steer    = readS32(a.add(0x30C));
    var engine   = readS32(a.add(0x310));
    var longSpd  = readS32(a.add(0x314));
    var latSpd   = readS32(a.add(0x318));
    var frontSlip= readS32(a.add(0x31C));
    var rearSlip = readS32(a.add(0x320));
    var finishT  = readS32(a.add(0x328));
    var accumDist= readS32(a.add(0x32C));
    var pendFin  = readU16(a.add(0x344));
    var gear     = readU8(a.add(0x36B));
    var vehMode  = readU8(a.add(0x379));
    var trkContact= readU8(a.add(0x37B));
    var wheelMask= readU8(a.add(0x37D));
    var racePos  = readU8(a.add(0x383));

    // Metrics: in the original binary these are in a separate table,
    // not embedded in the actor. We emit zeros for now -- the key
    // physics/position fields are the important comparison targets.
    var prefix = writePrefix(frameIndex, simTick, stage, "actor", slot);
    var line = prefix +
        // frame fields (zeros for actor row)
        "0,0,0,0,0,0,0.000000,0.000000,0.000000,0,0,0," +
        // actor fields
        slotState + "," + comp1 + "," + comp2 + "," + viewTarget + "," +
        worldX + "," + worldY + "," + worldZ + "," +
        velX + "," + velY + "," + velZ + "," +
        angRoll + "," + angYaw + "," + angPitch + "," +
        dispRoll + "," + dispYaw + "," + dispPitch + "," +
        spanRaw + "," + spanNorm + "," + spanAccum + "," + spanHigh + "," +
        steer + "," + engine + "," + longSpd + "," + latSpd + "," +
        frontSlip + "," + rearSlip + "," + finishT + "," + accumDist + "," +
        pendFin + "," + gear + "," + vehMode + "," + trkContact + "," + wheelMask + "," + racePos + "," +
        // metric fields (zeros -- original metrics table address TBD)
        "0,0,0,0,0,0,0," +
        // view fields (zeros)
        "0,0,0,0.000000,0.000000,0.000000\n";
    writeRow(line);
}

function writeViewRow(frameIndex, simTick, stage, vpIndex) {
    // Per-viewport camera world position (24.8 fixed-point int[3])
    var camBase = G_camWorldPosFixed.add(vpIndex * 12);
    var cwx = readS32(camBase);
    var cwy = readS32(camBase.add(4));
    var cwz = readS32(camBase.add(8));
    var cx = cwx / 256.0;
    var cy = cwy / 256.0;
    var cz = cwz / 256.0;

    var actorSlot = 0; // default to player for view 0

    var prefix = writePrefix(frameIndex, simTick, stage, "view", vpIndex);
    var line = prefix +
        // frame fields (zeros)
        "0,0,0,0,0,0,0.000000,0.000000,0.000000,0,0,0," +
        // actor fields (zeros except view_target)
        "0,0,0," + actorSlot + "," +
        "0,0,0,0,0,0,0,0,0,0,0,0," +
        "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0," +
        "0,0,0,0,0,0,0," +
        // view fields
        cwx + "," + cwy + "," + cwz + "," +
        cx.toFixed(6) + "," + cy.toFixed(6) + "," + cz.toFixed(6) + "\n";
    writeRow(line);
}

// ============================================================================
// Stage snapshot -- mirrors td5_game_trace_stage() in the source port
// ============================================================================

function traceStage(stage) {
    if (!enabled) return;

    var frameIndex = fridaFrameCounter;
    var simTick    = readS32(G_simulationTickCounter);

    if (!beginFrame(frameIndex)) return;

    // Frame row
    writeFrameRow(frameIndex, simTick, stage);

    // Actor rows — all 6 racer slots, no state filter
    for (var i = 0; i < 6; i++) {
        if (!selectedSlot(i)) continue;
        writeActorRow(frameIndex, simTick, stage, i);
    }

    // View rows
    var vpCount = readS32(G_viewCount);
    for (var vp = 0; vp < vpCount && vp < 2; vp++) {
        writeViewRow(frameIndex, simTick, stage, vp);
    }
}

// ============================================================================
// Lifecycle
// ============================================================================

function init() {
    console.log("[trace] Initializing race trace hooks on TD5_d3d.exe");
    console.log("[trace] Output: " + OUTPUT_PATH);
    console.log("[trace] Slot filter: " + (TRACE_SLOT < 0 ? "all" : TRACE_SLOT));
    console.log("[trace] Max frames: " + (TRACE_MAX_FRAMES > 0 ? TRACE_MAX_FRAMES : "unlimited"));

    fileHandle = new File(OUTPUT_PATH, "w");
    if (fileHandle === null) {
        console.log("[trace] ERROR: Could not open " + OUTPUT_PATH);
        return;
    }
    writeRow(CSV_HEADER);
    fileHandle.flush();

    if (CAPTURE_BRAKE_TRACE) {
        brakeFileHandle = new File(BRAKE_OUTPUT_PATH, "w");
        if (brakeFileHandle !== null) {
            // Columns chosen to cover Item 3 requirements:
            //   long_speed, engine_accum, gear, throttle, brake_flag, handbrake_flag.
            // Added front_slip/rear_slip as proxies for per-axle wheel forces
            // since per-wheel force writes live in callees (0x00403720 et al.)
            // and can be captured in a follow-up pass.
            brakeFileHandle.write(
                "sim_tick,slot,long_speed,engine_accum,gear,brake_flag,handbrake_flag," +
                "throttle_bit,brake_bit,analog_y_flag,front_slip,rear_slip," +
                "steering_cmd,surface_flags\n"
            );
            brakeFileHandle.flush();
            console.log("[trace] Brake trace: " + BRAKE_OUTPUT_PATH);
        } else {
            console.log("[trace] WARN: Could not open " + BRAKE_OUTPUT_PATH);
        }
    }

    if (CAPTURE_CONTACTS) {
        contactsFileHandle = new File(CONTACTS_OUTPUT_PATH, "w");
        if (contactsFileHandle !== null) {
            // Header: 32 short values (8 quads of 4) plus actor slots & return mask.
            // q<k>_pt_x = contact point X in opponent's frame
            // q<k>_pt_z = contact point Z in opponent's frame
            // q<k>_arm_x = contact - origin (moment-arm X). Confirmed NOT own_x.
            // q<k>_arm_z = contact - origin (moment-arm Z). Confirmed NOT own_z.
            var hdr = "call_seq,sim_tick,a_slot,b_slot,ret_mask";
            for (var k = 0; k < 8; k++) {
                hdr += ",q" + k + "_pt_x,q" + k + "_pt_z,q" + k + "_arm_x,q" + k + "_arm_z";
            }
            hdr += "\n";
            contactsFileHandle.write(hdr);
            contactsFileHandle.flush();
            console.log("[trace] Contacts trace: " + CONTACTS_OUTPUT_PATH);
        } else {
            console.log("[trace] WARN: Could not open " + CONTACTS_OUTPUT_PATH);
        }
    }

    // Open the calls trace CSV (always opened; unused if HOOK_SPECS empty).
    callsFileHandle = new File(CALLS_OUTPUT_PATH, "w");
    if (callsFileHandle !== null) {
        callsFileHandle.write(
            "sim_tick,fn_name,call_idx,n_args,arg_0,arg_1,arg_2,arg_3," +
            "arg_4,arg_5,arg_6,arg_7,has_ret,ret\n"
        );
        callsFileHandle.flush();
        console.log("[trace] Calls trace: " + CALLS_OUTPUT_PATH);
    } else {
        console.log("[trace] WARN: Could not open " + CALLS_OUTPUT_PATH);
    }

    enabled = true;
    framesStarted = 0;
    lastFrameIndex = -1;
    simTicksThisFrame = 0;
    fridaFrameCounter = 0;
    raceConfirmed = false;
    rngSeededThisRace = false;
    callIdxTick = -1;
    callIdxCounts = {};

    installHooks();
    installHookSpecs();

    // Auto-throttle: clamp player 1 control bits on PollRaceSessionInput onLeave
    // so the original binary's car accelerates like td5re (with its [Trace]
    // AutoThrottle=1 ini flag).
    //
    // Bit layout confirmed against td5mod/src/td5re/td5_types.h (TD5_InputBits):
    //   0x00000200 = THROTTLE  (accelerate)
    //   0x00000400 = BRAKE
    //   0x00100000 = GEAR_UP
    //   0x08000000 = ANALOG_Y_FLAG (reverse implied via negative Y axis)
    //
    // Previous version used 0x100000 as ACCELERATE (actually GEAR_UP) and
    // masked off 0x200 thinking it was HANDBRAKE (actually THROTTLE), which
    // made the original: OR in gear-up every frame, then clear throttle
    // every frame — so the car shifted up repeatedly then coasted to a
    // halt. Observed 2026-04-11 in /diff-race: original stopped by sim_tick
    // 240 while port kept accelerating.
    if (AUTO_THROTTLE) {
        var PLAYER1_CONTROL = ptr(0x482FFC);
        var THROTTLE_BIT    = 0x00000200;
        var BRAKE_BIT       = 0x00000400;
        var GEAR_UP_BIT     = 0x00100000;
        var GEAR_DOWN_BIT   = 0x00080000;
        var ANALOG_Y_FLAG   = 0x08000000;
        safeAttach("PollRaceSessionInput_AutoThrottle", ADDR_PollRaceSessionInput, {
            onLeave: function (retval) {
                if (!raceConfirmed) return;
                try {
                    var cbits = PLAYER1_CONTROL.readU32();
                    // Force throttle on, clear brake/reverse/gear changes.
                    cbits |= THROTTLE_BIT;
                    cbits &= ~(BRAKE_BIT | GEAR_UP_BIT | GEAR_DOWN_BIT | ANALOG_Y_FLAG);
                    PLAYER1_CONTROL.writeU32(cbits);
                } catch (e) { /* skip */ }
            }
        });
        console.log("[trace] Auto-throttle: forcing THROTTLE_BIT (0x200) on PollRaceSessionInput");
    }

    // Windowed mode: not implemented — DDraw exclusive mode can't be reliably
    // overridden via Frida without crashing. Accept fullscreen for trace capture.
    console.log("[trace] Hooks installed, waiting for race...");
}

/*  Windowed mode removed — DDraw COM vtable hooks crash the game.
function installWindowedMode() {
    // 1. Hook IDirectDraw4::SetCooperativeLevel via ddraw.dll export trampoline.
    //    SetCooperativeLevel is vtable index 20 on IDirectDraw4.
    //    We intercept DirectDrawCreateEx to grab the DDraw interface pointer,
    //    then hook its vtable.
    var ddrawMod = Module.findBaseAddress("ddraw.dll");
    if (!ddrawMod) {
        // ddraw.dll not loaded yet — hook LoadLibrary to catch it
        var pLoadLibA = Module.findExportByName("kernel32.dll", "LoadLibraryA");
        Interceptor.attach(pLoadLibA, {
            onLeave: function (retval) {
                if (retval.isNull()) return;
                var mod = new NativePointer(retval);
                // Check if this is ddraw.dll
                try {
                    var name = Module.findModuleByAddress(mod).name.toLowerCase();
                    if (name === "ddraw.dll") {
                        hookDDrawExports(mod);
                    }
                } catch (e) {}
            }
        });
        console.log("[windowed] ddraw.dll not yet loaded, hooking LoadLibraryA");
    } else {
        hookDDrawExports(ddrawMod);
    }

    // 2. Prevent display mode changes
    var pChangeDisplayA = Module.findExportByName("user32.dll", "ChangeDisplaySettingsA");
    if (pChangeDisplayA) {
        Interceptor.attach(pChangeDisplayA, {
            onEnter: function (args) {
                // Force no-op: set lpDevMode to NULL = reset to registry default
                args[0] = ptr(0);
                args[1] = ptr(0);
            }
        });
        console.log("[windowed] Hooked ChangeDisplaySettingsA");
    }
    var pChangeDisplayExA = Module.findExportByName("user32.dll", "ChangeDisplaySettingsExA");
    if (pChangeDisplayExA) {
        Interceptor.attach(pChangeDisplayExA, {
            onEnter: function (args) {
                args[1] = ptr(0);
                args[2] = ptr(0);
            }
        });
        console.log("[windowed] Hooked ChangeDisplaySettingsExA");
    }

    // 3. Fix window style after creation
    var pCreateWindowExA = Module.findExportByName("user32.dll", "CreateWindowExA");
    if (pCreateWindowExA) {
        Interceptor.attach(pCreateWindowExA, {
            onLeave: function (retval) {
                if (retval.isNull()) return;
                var hwnd = retval;
                var WS_OVERLAPPEDWINDOW = 0x00CF0000;
                var WS_VISIBLE = 0x10000000;
                var GWL_STYLE = -16;
                var SetWindowLongA = new NativeFunction(
                    Module.findExportByName("user32.dll", "SetWindowLongA"),
                    "long", ["pointer", "int", "long"], "stdcall");
                var SetWindowPos = new NativeFunction(
                    Module.findExportByName("user32.dll", "SetWindowPos"),
                    "int", ["pointer", "pointer", "int", "int", "int", "int", "uint"], "stdcall");
                var SWP_FRAMECHANGED = 0x0020;
                var SWP_NOZORDER = 0x0004;
                SetWindowLongA(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
                SetWindowPos(hwnd, ptr(0), 50, 50, 656, 519, SWP_FRAMECHANGED | SWP_NOZORDER);
                console.log("[windowed] Window style set to overlapped 640x480 at (50,50)");
            }
        });
        console.log("[windowed] Hooked CreateWindowExA");
    }
}

var hookedVtables = {};  // prevent double-hooking same vtable

function hookDDrawVtable(pDD, label) {
    var vtable = pDD.readPointer();
    var vtKey = vtable.toString();
    if (hookedVtables[vtKey]) return;
    hookedVtables[vtKey] = true;

    // SetCooperativeLevel = vtable index 20
    var pSetCoopLevel = vtable.add(20 * 4).readPointer();
    Interceptor.attach(pSetCoopLevel, {
        onEnter: function (args) {
            var DDSCL_NORMAL = 0x8;
            console.log("[windowed] " + label + "::SetCooperativeLevel 0x" +
                        args[2].toInt32().toString(16) + " -> DDSCL_NORMAL");
            args[2] = ptr(DDSCL_NORMAL);
        }
    });

    // SetDisplayMode not hooked — Interceptor.replace crashed before.
    // With DDSCL_NORMAL, SetDisplayMode should fail gracefully.

    console.log("[windowed] Hooked " + label + " vtable at " + vtable);
}

function hookDDrawExports(ddrawBase) {
    // Hook DirectDrawCreateEx (IDirectDraw4/7)
    var pDDCreateEx = Module.findExportByName("ddraw.dll", "DirectDrawCreateEx");
    if (pDDCreateEx) {
        Interceptor.attach(pDDCreateEx, {
            onEnter: function (args) { this.ppDD = args[1]; },
            onLeave: function (retval) {
                if (retval.toInt32() !== 0) return;
                hookDDrawVtable(this.ppDD.readPointer(), "DDCreateEx");
            }
        });
        console.log("[windowed] Hooked DirectDrawCreateEx");
    }

    // Hook DirectDrawCreate (IDirectDraw)
    var pDDCreate = Module.findExportByName("ddraw.dll", "DirectDrawCreate");
    if (pDDCreate) {
        Interceptor.attach(pDDCreate, {
            onEnter: function (args) { this.ppDD = args[1]; },
            onLeave: function (retval) {
                if (retval.toInt32() !== 0) return;
                hookDDrawVtable(this.ppDD.readPointer(), "DDCreate");
            }
        });
        console.log("[windowed] Hooked DirectDrawCreate");
    }

    // Hook QueryInterface to catch IDirectDraw -> IDirectDraw4 upgrades
    // (game may create IDirectDraw then QI to IDirectDraw4)
}  End of windowed mode block */

function shutdown() {
    enabled = false;
    if (fileHandle !== null) {
        fileHandle.flush();
        fileHandle.close();
        fileHandle = null;
        console.log("[trace] Trace file closed (" + framesStarted + " frames captured)");
    }
    if (brakeFileHandle !== null) {
        brakeFileHandle.flush();
        brakeFileHandle.close();
        brakeFileHandle = null;
        console.log("[trace] Brake trace closed");
    }
    if (contactsFileHandle !== null) {
        contactsFileHandle.flush();
        contactsFileHandle.close();
        contactsFileHandle = null;
        console.log("[trace] Contacts trace closed");
    }
    if (callsFileHandle !== null) {
        callsFileHandle.flush();
        callsFileHandle.close();
        callsFileHandle = null;
        console.log("[trace] Calls trace closed");
    }
}

function seedGameRng() {
    // Race-RNG seeding. Called exactly once per race (gated by rngSeededThisRace).
    // The 3 globals were identified via Ghidra symbol audit on 2026-04-13 — they
    // are the only named "random seed" storage in TD5_d3d.exe.
    try {
        G_randomSeedForRace.writeU32(FIXED_SEED_VALUE);
        G_raceSessionRandomSeed.writeU32(FIXED_SEED_VALUE);
        // g_raceRandomSeedTable is a multi-entry table; blast with a
        // deterministic ramp so every AI slot gets a known starting seed.
        // 8 dwords covers 6 race slots + 2 spare.
        for (var i = 0; i < 8; i++) {
            G_raceRandomSeedTable.add(i * 4).writeU32(FIXED_SEED_VALUE + i * 0x1111);
        }
        console.log("[trace] RNG seeded: race=" + FIXED_SEED_VALUE.toString(16) +
                    " session=" + FIXED_SEED_VALUE.toString(16) +
                    " table[0..7]");
    } catch (e) {
        console.log("[trace] WARN: seedGameRng failed: " + e.message);
    }
}

// ============================================================================
// Hook installation
//
// Strategy: We hook at the callee level and reconstruct the source-port stage
// boundaries as closely as possible.
//
// Source port stages vs original binary hooks:
//   frame_begin   -> RunRaceFrame entry
//   pre_physics   -> UpdateRaceActors entry (AI+physics dispatcher)
//   post_physics  -> ResolveVehicleContacts entry (after UpdateRaceActors returned)
//   post_ai       -> ResolveVehicleContacts onLeave (after collision)
//   post_track    -> UpdateRaceOrder entry (after tire tracks)
//   post_progress -> NormalizeWrapState onLeave (last sim tick operation)
//   post_progress -> NormalizeWrapState onLeave (preferred)
//   frame_end     -> EndRaceScene onLeave
//
// Note: The original binary's pipeline order is slightly different from the
// source port's decomposed calls. UpdateRaceActors encompasses physics+AI+track
// in one dispatcher. We capture before/after the major stages.
//
// The fallback "minimal" hook set (ENABLE_INNER_TICK_HOOKS=false) captures only
// frame_begin and frame_end. It is sufficient for position/velocity differential
// comparison because both the source port and original sample state at the same
// sim_tick counter value. Enable inner hooks only when you need per-tick
// stage-boundary data (e.g., isolating which stage introduces a divergence).
//
// Inner-hook safety: the sim time accumulator (0x4AAED0) can hold a large
// residual on the first post-transition frame, causing 10-30+ inner loop
// iterations before Frida's file I/O can keep up. Two guards prevent the crash:
//   • raceConfirmed: set on first frame_begin — ensures actor memory is valid
//   • INNER_TICK_MAX_PER_FRAME: caps CSV rows even if accumulator bursts
// ============================================================================

function safeAttach(name, addr, callbacks) {
    try {
        Interceptor.attach(addr, callbacks);
        console.log("[trace] Hooked " + name + " at " + addr);
    } catch (e) {
        console.log("[trace] SKIP " + name + " at " + addr + ": " + e.message);
    }
}

// ============================================================================
// Generic hook-spec installer (diff-race custom hooks)
// ============================================================================

function writeCallRow(simTick, fnName, callIdx, args, hasRet, retVal) {
    if (callsFileHandle === null) return;
    var padded = [];
    for (var i = 0; i < 8; ++i) {
        padded.push(i < args.length ? (args[i] | 0) : 0);
    }
    var line = simTick + "," + fnName + "," + callIdx + "," + args.length;
    for (var j = 0; j < 8; ++j) line += "," + padded[j];
    line += "," + (hasRet ? 1 : 0) + "," + (hasRet ? (retVal | 0) : 0) + "\n";
    callsFileHandle.write(line);
}

function installHookSpecs() {
    if (!HOOK_SPECS || HOOK_SPECS.length === 0) return;
    console.log("[trace] Installing " + HOOK_SPECS.length + " custom hook spec(s)");
    for (var i = 0; i < HOOK_SPECS.length; ++i) {
        var spec = HOOK_SPECS[i];
        var name = spec.name;
        var rva  = spec.original_rva;
        var nArgs = spec.args | 0;
        var cap  = !!spec.capture_return;
        // YAML's `original_rva` is the absolute VA as Ghidra displays it
        // (e.g. 0x00445A70). Convert to an offset from module base before
        // adding, since BASE.add(0x00445A70) would overflow past the
        // module and produce access violations.
        var baseInt = BASE.toUInt32();
        var offset  = rva >= baseInt ? (rva - baseInt) : rva;
        var addr = BASE.add(offset);
        (function (name, nArgs, cap, addr) {
            try {
                Interceptor.attach(addr, {
                    onEnter: function (args) {
                        // Only emit during live race (avoids menu noise).
                        try { if (readS32(G_gameState) !== 2) return; } catch (e) { return; }
                        if (!raceConfirmed) return;
                        var simTick = readS32(G_simulationTickCounter);
                        var captured = [];
                        for (var k = 0; k < nArgs && k < 8; ++k) {
                            try { captured.push(args[k].toInt32()); }
                            catch (e) { captured.push(0); }
                        }
                        var idx = nextCallIdx(name, simTick);
                        this._td5_call_simTick = simTick;
                        this._td5_call_idx = idx;
                        this._td5_call_args = captured;
                        if (!cap) {
                            writeCallRow(simTick, name, idx, captured, false, 0);
                        }
                    },
                    onLeave: function (retval) {
                        if (!cap) return;
                        if (typeof this._td5_call_simTick === 'undefined') return;
                        var retInt = 0;
                        try { retInt = retval.toInt32(); } catch (e) { retInt = 0; }
                        writeCallRow(
                            this._td5_call_simTick, name,
                            this._td5_call_idx, this._td5_call_args,
                            true, retInt
                        );
                    }
                });
                console.log("[trace] Hook spec: " + name + " @ rva=" + rva +
                            " args=" + nArgs + " ret=" + (cap ? "yes" : "no"));
            } catch (e) {
                console.log("[trace] Hook spec SKIP " + name + " @ " + addr +
                            ": " + e.message);
            }
        })(name, nArgs, cap, addr);
    }
}

function installHooks() {
    safeAttach("RunRaceFrame", ADDR_RunRaceFrame, {
        onEnter: function (args) {
            try {
                var gs = readS32(G_gameState);
                if (gs !== 2) {
                    // If we leave race state, reset the confirmed flag so the next
                    // race entry re-arms the guard correctly.
                    raceConfirmed = false;
                    raceFrameCount = 0;
                    rngSeededThisRace = false;  // re-seed on next race entry
                    return;
                }
                // Clamp g_simTickBudget to >= 1.0 each frame.
                //
                // The original's inner sim-tick loop is gated by
                //   g_simTimeAccumulator >= 0x10000  (set fresh per frame via
                //   accumulator = budget * 65536)
                // and UpdateRaceCameraTransitionTimer (0x40A490) — which drains
                // the race countdown — only runs inside that loop. Frida hook
                // overhead slows the game enough that the game's own adaptive
                // budget drifts below 1.0, which permanently prevents the
                // sim-tick loop from running and leaves the race stuck at
                // paused=1 forever. Unlike the source port, the original's
                // accumulator does not carry over across frames, so a single
                // starved frame is enough to stall the race indefinitely.
                // Clamping budget to 1.0 guarantees >= 1 sim tick per frame.
                if (G_simTickBudget.readFloat() < 1.0) {
                    G_simTickBudget.writeFloat(1.0);
                }
                simTicksThisFrame = 0;
                fridaFrameCounter++;
                // Auto-close: stop when either the engine-clock cap (sim_tick)
                // or the render-frame ceiling is reached.
                // sim_tick is the authoritative "game engine time" — ticks at
                // 30 Hz once the countdown ends, so TRACE_MAX_SIM_TICK=450
                // maps to exactly 15 seconds of post-countdown race on both
                // binaries and lets the diff comparator align rows on a
                // common timeline.
                var curSimTick = readS32(G_simulationTickCounter);
                if ((TRACE_MAX_SIM_TICK > 0 && curSimTick >= TRACE_MAX_SIM_TICK) ||
                    (TRACE_MAX_FRAMES > 0 && raceFrameCount > TRACE_MAX_FRAMES)) {
                    console.log("[trace] Auto-close: sim_tick=" + curSimTick +
                                " raceFrames=" + raceFrameCount +
                                " (caps: sim_tick=" + TRACE_MAX_SIM_TICK +
                                " frames=" + TRACE_MAX_FRAMES + ")");
                    shutdown();
                    Thread.sleep(0.1);
                    send({type: "auto-close"});
                }
                traceStage("frame_begin");
                // Mark that at least one frame_begin has fired in race state.
                // Inner tick hooks are now safe to read actor memory.
                if (!raceConfirmed) {
                    // Deterministic seeding: this is the earliest point where the
                    // race actor table is initialized but the first physics tick
                    // has not yet run. Seeding now fixes AI spawn jitter.
                    if (DETERMINISTIC_SEED && !rngSeededThisRace) {
                        seedGameRng();
                        rngSeededThisRace = true;
                    }
                    // Re-apply windowed style on first race frame (game may
                    // have switched to fullscreen during the transition).
                    try {
                        var FindWindowA = new NativeFunction(
                            Module.findExportByName("user32.dll", "FindWindowA"),
                            "pointer", ["pointer", "pointer"], "stdcall");
                        var SetWindowLongA = new NativeFunction(
                            Module.findExportByName("user32.dll", "SetWindowLongA"),
                            "long", ["pointer", "int", "long"], "stdcall");
                        var SetWindowPos = new NativeFunction(
                            Module.findExportByName("user32.dll", "SetWindowPos"),
                            "int", ["pointer", "pointer", "int", "int", "int", "int", "uint"], "stdcall");
                        var hwnd = FindWindowA(ptr(0), Memory.allocUtf8String("Test Drive 5"));
                        if (!hwnd.isNull()) {
                            SetWindowLongA(hwnd, -16, 0x10CF0000);  // WS_OVERLAPPEDWINDOW|WS_VISIBLE
                            SetWindowPos(hwnd, ptr(0), 50, 50, 656, 519, 0x0024);  // SWP_FRAMECHANGED|SWP_NOZORDER
                            console.log("[windowed] Re-applied window style on race start");
                        }
                    } catch(e) {}
                }
                raceConfirmed = true;
                raceFrameCount++;
            } catch (e) { /* skip */ }
        }
    });

    safeAttach("EndRaceScene", ADDR_EndRaceScene, {
        onEnter: function (args) {
            try {
                var gs = readS32(G_gameState);
                if (gs !== 2) return;
                if (!ENABLE_INNER_TICK_HOOKS) {
                    // Skip post_progress during countdown — the port only
                    // emits this stage inside the post-countdown sim-tick
                    // loop (after simulation_tick_counter++), so matching
                    // its semantics keeps the comparator's (sim_tick, stage,
                    // kind, id) keys aligned on both sides.
                    if (readS32(G_simulationTickCounter) > 0) {
                        traceStage("post_progress");
                    }
                }
            } catch (e) { /* skip */ }
        },
        onLeave: function (retval) {
            try {
                var gs = readS32(G_gameState);
                if (gs !== 2) return;
                traceStage("frame_end");
            } catch (e) { /* skip */ }
        }
    });

    if (CAPTURE_BRAKE_TRACE) {
        hookBrakeCapture();
    }
    if (CAPTURE_CONTACTS) {
        hookContactCapture();
    }

    if (!ENABLE_INNER_TICK_HOOKS) {
        return;
    }

    // Inner tick hooks (re-enabled 2026-04-13 after Ghidra size audit).
    //
    // Safety: three guards before any memory read or CSV write:
    //   1. raceConfirmed — true only after first frame_begin in game_state==2.
    //      Prevents reads during the actor-init burst on the very first sim tick.
    //   2. game_state == 2 — race state still active.
    //   3. simTicksThisFrame < INNER_TICK_MAX_PER_FRAME — caps I/O when the
    //      time accumulator carries a large residual (e.g., after a stall or the
    //      first post-transition frame). The sim runs at 60 Hz fixed-step so
    //      normal frames have 1-2 ticks; 8 is a safe ceiling.
    //
    // NormalizeWrapState (0x443FB0) is intentionally NOT hooked: per-site
    // comment says it is called 6× per actor OUTSIDE the tick loop, which
    // pollutes per-tick stage accounting. Its 57-byte body is large enough
    // for Frida's trampoline (size is not the blocker).

    safeAttach("UpdateRaceActors", ADDR_UpdateRaceActors, {
        onEnter: function (args) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (simTicksThisFrame >= INNER_TICK_MAX_PER_FRAME) return;
                traceStage("pre_physics");
            } catch (e) {}
        }
    });

    safeAttach("ResolveVehicleContacts", ADDR_ResolveVehicleContacts, {
        onEnter: function (args) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (simTicksThisFrame >= INNER_TICK_MAX_PER_FRAME) return;
                traceStage("post_physics");
            } catch (e) {}
        },
        onLeave: function (retval) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (simTicksThisFrame >= INNER_TICK_MAX_PER_FRAME) return;
                traceStage("post_ai");
            } catch (e) {}
        }
    });

    // UpdateRaceOrder @ 0x42F5B0 is UNSAFE for Frida's Interceptor: its 5-byte
    // prologue (PUSH ECX/EBX/ESI/EDI + XOR ESI,ESI) has an internal CONDITIONAL_JUMP
    // from 0x42F62B targeting 0x42F5B4 — the loop-top label sits inside the region
    // Frida overwrites with its JMP trampoline. Installing the hook hangs the
    // game at the first sim-tick. [verified via Ghidra 2026-04-20]
    //
    // Workaround: emit post_track on UpdateTireTrackPool onLeave (fires once per
    // tick, just before UpdateRaceOrder) and post_progress on
    // UpdateRaceCameraTransitionTimer onEnter (fires once per tick, after
    // UpdateRaceOrder + cameras + particles — the last callee before
    // g_simulationTickCounter increments).
    safeAttach("UpdateTireTrackPool", ADDR_UpdateTireTrackPool, {
        onLeave: function (retval) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (simTicksThisFrame >= INNER_TICK_MAX_PER_FRAME) return;
                traceStage("post_track");
            } catch (e) {}
        }
    });

    safeAttach("UpdateRaceCameraTransitionTimer", ADDR_UpdateRaceCameraTransitionTimer, {
        onEnter: function (args) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (simTicksThisFrame >= INNER_TICK_MAX_PER_FRAME) return;
                simTicksThisFrame++;
                traceStage("post_progress");
            } catch (e) {}
        }
    });
}

// -----------------------------------------------------------------------------
// Brake/RPM capture (Item 3). Hooks UpdatePlayerVehicleDynamics @ 0x404030
// and reads the actor struct directly. Offsets are CONFIRMED from the
// 2026-04-13 Ghidra audit:
//   +0x310 engine_speed_accum   (int)
//   +0x314 longitudinal_speed   (int)
//   +0x30C steering_command     (int)
//   +0x31C front_axle_slip_excess (int)  — proxy for front wheel force
//   +0x320 rear_axle_slip_excess  (int)  — proxy for rear wheel force
//   +0x36B current_gear         (u8)
//   +0x36D brake_flag           (u8)
//   +0x36E handbrake_flag       (u8)
//   +0x376 surface_contact_flags(u8)
// -----------------------------------------------------------------------------
function hookBrakeCapture() {
    safeAttach("UpdatePlayerVehicleDynamics_BrakeCap", ADDR_UpdatePlayerVehicleDynamics, {
        onEnter: function (args) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (brakeFileHandle === null) return;
                var actor = args[0];
                var slot  = actorSlotFromPtr(actor);
                if (slot < 0) return; // called with non-actor arg — skip
                var simTick = readS32(G_simulationTickCounter);

                var longSpd = readS32(actor.add(0x314));
                var engAcc  = readS32(actor.add(0x310));
                var steer   = readS32(actor.add(0x30C));
                var frontSl = readS32(actor.add(0x31C));
                var rearSl  = readS32(actor.add(0x320));
                var gear    = readU8 (actor.add(0x36B));
                var brakeF  = readU8 (actor.add(0x36D));
                var handbF  = readU8 (actor.add(0x36E));
                var surface = readU8 (actor.add(0x376));

                // Player 1 control-bits decomposition (player slot only).
                // Bit layout per td5_types.h TD5_InputBits:
                //   0x00000200 = THROTTLE
                //   0x00000400 = BRAKE
                //   0x08000000 = ANALOG_Y_FLAG (negative analog Y ⇒ reverse)
                // Previous revisions used 0x100000 (GEAR_UP) for throttle —
                // wrong, already corrected elsewhere in this file.
                var thrBit = 0, brkBit = 0, yFlag = 0;
                if (slot === 0) {
                    var cbits = G_player1ControlBits.readU32();
                    thrBit = (cbits & 0x00000200) ? 1 : 0;
                    brkBit = (cbits & 0x00000400) ? 1 : 0;
                    yFlag  = (cbits & 0x08000000) ? 1 : 0;
                }

                brakeFileHandle.write(
                    simTick + "," + slot + "," +
                    longSpd + "," + engAcc + "," + gear + "," +
                    brakeF + "," + handbF + "," +
                    thrBit + "," + brkBit + "," + yFlag + "," +
                    frontSl + "," + rearSl + "," +
                    steer + "," + surface + "\n"
                );
            } catch (e) { /* skip */ }
        }
    });

    // UpdateEngineSpeedAccumulator @ 0x42EDF0 is a CALLEE of 0x404030 and
    // writes the same engine_speed_accum field. Hooking it is redundant for
    // per-frame logging (we already read +0x310 after 0x404030 returns), but
    // wire it up as a dormant no-op so the address is registered for
    // future drill-down (e.g., logging target accumulator BEFORE damping).
    safeAttach("UpdateEngineSpeedAccumulator_Probe", ADDR_UpdateEngineSpeedAccumulator, {
        onEnter: function (args) { /* no-op; reserved for finer-grained capture */ }
    });
}

// -----------------------------------------------------------------------------
// V2V contact capture (Item 4). Hooks CollectVehicleCollisionContacts @ 0x408570
// and dumps param_5[0..31] on every invocation. Signature per Ghidra audit:
//   uint __cdecl CollectVehicleCollisionContacts(
//       int actor_a, int actor_b, int *state_a, int *state_b, short *contactData);
// contactData is a short[32] — 8 quads of {pt_x, pt_z, arm_x, arm_z}.
// Return value is a bitmask of populated quads (0x01..0xFF).
// -----------------------------------------------------------------------------
var contactsCallSeq = 0;
function hookContactCapture() {
    safeAttach("CollectVehicleCollisionContacts", ADDR_CollectVehicleCollisionContacts, {
        onEnter: function (args) {
            try {
                if (!raceConfirmed) return;
                if (readS32(G_gameState) !== 2) return;
                if (contactsFileHandle === null) return;
                this.actor_a = args[0];
                this.actor_b = args[1];
                this.contact_data = args[4]; // short *
            } catch (e) { this.contact_data = null; }
        },
        onLeave: function (retval) {
            try {
                if (this.contact_data === null) return;
                if (contactsFileHandle === null) return;
                var slotA = actorSlotFromPtr(this.actor_a);
                var slotB = actorSlotFromPtr(this.actor_b);
                var mask  = retval.toInt32() & 0xFF;
                var simTick = readS32(G_simulationTickCounter);
                var line = (contactsCallSeq++) + "," + simTick + "," +
                           slotA + "," + slotB + "," + mask;
                // 8 quads × 4 shorts = 32 signed 16-bit reads.
                for (var k = 0; k < 8; k++) {
                    var base = this.contact_data.add(k * 8); // 4 shorts = 8 bytes
                    var pt_x  = base.readS16();
                    var pt_z  = base.add(2).readS16();
                    var arm_x = base.add(4).readS16();
                    var arm_z = base.add(6).readS16();
                    line += "," + pt_x + "," + pt_z + "," + arm_x + "," + arm_z;
                }
                line += "\n";
                contactsFileHandle.write(line);
            } catch (e) { /* skip */ }
        }
    });
}

// ============================================================================
// Cleanup on script unload
// ============================================================================

// Clean up when the Frida script is unloaded (detach / ctrl-C)
Script.setGlobalAccessHandler({
    enumerate: function () { return []; },
    get: function (property) { return undefined; }
});
// Frida calls rpc.exports.dispose on unload if defined
rpc.exports = {
    dispose: function () {
        shutdown();
    }
};

// ============================================================================
// Go
// ============================================================================

init();
