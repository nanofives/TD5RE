/**
 * frida_chassis_probe.js -- Differential chassis-launch probe.
 *
 * Hooks RefreshVehicleWheelContactFrames @ 0x00403720 (TD5_d3d.exe address;
 * port td5re.exe ships its own copy at the SAME logical entry symbol but
 * a different runtime address -- the orchestrator picks the correct address
 * via BINARY_LABEL).
 *
 * Per slot per call we emit ONE CSV row capturing the inputs that decide
 * whether the chassis snaps off the ground:
 *
 *   onEnter:   slot, sub_tick, world_pos (xyz fp8), linear_velocity (xyz fp8)
 *   onLeave:   per-wheel wheel_contact_pos.y (4 wheels), wcb, scf
 *
 * Output CSV columns (single row per call):
 *   binary, slot, sub_tick, world_x, world_y, world_z, vx, vy, vz,
 *   ground_FL, ground_FR, ground_RL, ground_RR, wcb, scf
 *
 * Reference: todo_edinburgh_chassis_launch_proximate_cause.md,
 *            reference_wheel_contact_delta_field.md (wheel offsets per Agent I)
 */

"use strict";

// ===== Orchestrator-substituted vars (rewritten by frida_differential_capture.py) =====
var OUTPUT_PATH  = "C:/Users/maria/Desktop/Proyectos/TD5RE/log/diff_chassis_default.csv";
var BINARY_LABEL = "orig";    // "orig" | "port"

// ===== Constants -- ORIGINAL binary (TD5_d3d.exe, image base 0x00400000) =====
// Port td5re.exe is a separate PE; we resolve the function by symbol export
// when BINARY_LABEL === "port" via Module.findExportByName("td5re.exe",
// "td5_physics_refresh_wheel_contacts"); if export resolution fails we
// log a clear error and bail out (better than capturing garbage).
var ORIG_ADDR_RefreshVehicleWheelContactFrames = ptr(0x00403720);
var ORIG_ADDR_RunRaceFrame                     = ptr(0x0042B580);
var ORIG_ACTOR_BASE   = ptr(0x004AB108);
var ORIG_ACTOR_STRIDE = 0x388;
var ORIG_G_simTick    = ptr(0x004AADA0);
var ORIG_G_gameState  = ptr(0x004C3CE8);

// Actor field offsets (identical across both binaries -- the port keeps the
// 0x388 stride and field offsets verbatim, see CLAUDE.md "actor stride 0x388").
var OFF_WORLD_X     = 0x1FC;
var OFF_WORLD_Y     = 0x200;
var OFF_WORLD_Z     = 0x204;
var OFF_LV_X        = 0x1CC;
var OFF_LV_Y        = 0x1D0;
var OFF_LV_Z        = 0x1D4;
var OFF_WCP_BASE    = 0xF0;     // wheel_contact_pos[i] base (X). +4=Y, +8=Z. 12 bytes each.
                                // Per re/include/td5_actor_struct.h line 294 +
                                // orig piVar5 = &actor->field_0xf4 (= wcp[0].y) which
                                // means wcp[0] base is at +0xF0. Prior probe used 0xF4
                                // which caused the +4 offset for .y to actually land on .z.
var OFF_WCB         = 0x37D;    // wheel_contact_bitmask (snapshot OLD copy)
var OFF_SCF         = 0x370;    // surface_contact_flags
// 2026-05-26 — chassis-launch upstream audit: dump rotation matrix Y-row
// (m[3..5] -- the row that feeds rot_y in the snap formula) + per-wheel
// suspension_pos (the input to body_wy = cwy - sp_div - susp_offset).
// Offsets per Ghidra layout dump of orig + port struct match (CLAUDE.md
// "actor stride 0x388").
var OFF_ROT_M3      = 0x12C;    // rotation_matrix.m[3] (float, Y-row x)
var OFF_ROT_M4      = 0x130;    // rotation_matrix.m[4] (float, Y-row y)
var OFF_ROT_M5      = 0x134;    // rotation_matrix.m[5] (float, Y-row z)
var OFF_SUSP_BASE   = 0x2DC;    // wheel_suspension_pos[i] (int32, stride 4)
// 2026-05-26 — per-wheel walker audit: dump per-wheel probe span/sub_lane
// to identify which wheel walks to wrong span at descent-to-climb transitions.
var OFF_WHEEL_PROBE_BASE = 0x40;    // wheel_probes[i] (TD5_TrackProbeState, stride 0x10)
//   +0x00 = span_index (int16)
//   +0x0C = sub_lane_index (int8)

// ===== Runtime ==============================================================

var addrRefresh = null;
var addrRunRaceFrame = null;
var actorBase = null;
var actorStride = 0x388;
var gSimTick = null;
var gGameState = null;

function readS32(p) { try { return p.readS32(); } catch (e) { return 0; } }
function readU8 (p) { try { return p.readU8 (); } catch (e) { return 0; } }

function actorSlot(actorPtr) {
    if (actorBase === null) return -1;
    var d = actorPtr.sub(actorBase).toInt32();
    if (d < 0) return -1;
    var slot = Math.floor(d / actorStride);
    if (slot * actorStride !== d) return -1;
    if (slot < 0 || slot > 11) return -1;
    return slot;
}

// Monotonic per-call sub-tick counter (Frida side). The original increments
// g_simulationTickCounter only AFTER countdown ends; per-call serial number
// gives us a tick-equivalent index that the merger can align across binaries.
var subTickCounter = 0;
var raceConfirmed  = false;
var fridaFrameCounter = 0;

var fp = null;

function openOutput() {
    try {
        fp = new File(OUTPUT_PATH, "w");
        fp.write("binary,slot,sub_tick,world_x,world_y,world_z,vx,vy,vz," +
                 "ground_FL,ground_FR,ground_RL,ground_RR,wcb,scf," +
                 "m3,m4,m5,susp_FL,susp_FR,susp_RL,susp_RR," +
                 "wcpx_FL,wcpz_FL,wcpx_FR,wcpz_FR,wcpx_RL,wcpz_RL,wcpx_RR,wcpz_RR," +
                 "wpsp_FL,wpsl_FL,wpsp_FR,wpsl_FR,wpsp_RL,wpsl_RL,wpsp_RR,wpsl_RR\n");
        fp.flush();
        console.log("[chassis_probe] (" + BINARY_LABEL + ") -> " + OUTPUT_PATH);
    } catch (e) {
        console.log("[chassis_probe] ERROR opening output: " + e);
        fp = null;
    }
}

function resolveAddresses() {
    if (BINARY_LABEL === "orig") {
        addrRefresh = ORIG_ADDR_RefreshVehicleWheelContactFrames;
        addrRunRaceFrame = ORIG_ADDR_RunRaceFrame;
        actorBase = ORIG_ACTOR_BASE;
        gSimTick = ORIG_G_simTick;
        gGameState = ORIG_G_gameState;
        return true;
    }
    // Port: resolve by exported symbol.
    // Frida 17+ removed Module.findExportByName as a top-level fn; use the
    // module-instance API instead.
    var portMod = Process.findModuleByName("td5re.exe");
    if (portMod === null) {
        console.log("[chassis_probe] ERROR: cannot find td5re.exe module");
        return false;
    }
    function modFindExport(name) {
        try { return portMod.findExportByName(name); }
        catch (e) { return null; }
    }
    var exp = modFindExport("td5_physics_refresh_wheel_contacts");
    if (exp === null) {
        // Fallback: try by symbol of td5re.exe with leading underscore (MinGW C linkage).
        exp = modFindExport("_td5_physics_refresh_wheel_contacts");
    }
    if (exp === null) {
        // PE export table fallback: td5re.exe only exports the 4 DDraw stubs.
        // Walk the module symbol table (static symbols from MinGW link) to find
        // the C-linkage name (with leading underscore).
        try {
            var syms = portMod.enumerateSymbols();
            for (var i = 0; i < syms.length; i++) {
                var s = syms[i];
                if (s.name === "_td5_physics_refresh_wheel_contacts" ||
                    s.name === "td5_physics_refresh_wheel_contacts") {
                    exp = s.address;
                    console.log("[chassis_probe] resolved via enumerateSymbols: " + s.name + " @ " + exp);
                    break;
                }
            }
        } catch (e) {
            console.log("[chassis_probe] enumerateSymbols failed: " + e.message);
        }
    }
    if (exp === null) {
        // Last-resort: hardcoded RVA from nm (0x40be90 - 0x400000 = 0xbe90).
        // NOTE: this RVA shifts every rebuild — re-fetch with
        // `nm td5re.exe | grep _td5_physics_refresh_wheel_contacts` and update.
        // ASLR may shift module.base; compute as base + RVA.
        try {
            exp = portMod.base.add(0xbe90);
            console.log("[chassis_probe] resolved via hardcoded RVA: 0xbe90 -> " + exp);
        } catch (e) {
            console.log("[chassis_probe] hardcoded RVA fallback failed: " + e.message);
        }
    }
    if (exp === null) {
        console.log("[chassis_probe] ERROR: cannot resolve td5_physics_refresh_wheel_contacts in td5re.exe");
        console.log("[chassis_probe] HINT: build with `-Wl,--export-all-symbols` or add a .def export");
        return false;
    }
    addrRefresh = exp;
    // Port actor base / simTick are read from the same exported globals if available.
    // td5re.exe does not export these via the PE table; resolve from the
    // statically-known RVAs from nm.exe (s_actor_memory.24 @ 0x4f2800).
    try {
        actorBase = portMod.base.add(0xf3800);
        console.log("[chassis_probe] actor_base via hardcoded RVA: 0xf3800 -> " + actorBase);
    } catch (e) {
        console.log("[chassis_probe] actor_base RVA fallback failed: " + e.message);
    }
    if (actorBase === null) {
        console.log("[chassis_probe] WARN: g_actors not exported; slot resolution disabled (using arg0 as slot index instead)");
    }
    return true;
}

// ===== Hook installation ====================================================

if (!resolveAddresses()) {
    console.log("[chassis_probe] address resolution failed; aborting install");
} else {
    openOutput();

    // RunRaceFrame is original-only; we detect race state via game state poll
    // on the port side (same end-result: skip countdown menus & post-race UI).
    if (addrRunRaceFrame !== null) {
        Interceptor.attach(addrRunRaceFrame, {
            onEnter: function () {
                fridaFrameCounter++;
                if (!raceConfirmed && gGameState !== null) {
                    if (readS32(gGameState) === 2) {
                        raceConfirmed = true;
                        console.log("[chassis_probe] race confirmed at frame=" + fridaFrameCounter);
                    }
                }
            }
        });
    } else {
        // Port path: race-confirm on first hook entry (the hook only fires
        // during sim).
        raceConfirmed = true;
    }

    Interceptor.attach(addrRefresh, {
        onEnter: function (args) {
            this.actor = args[0];
            // Resolve slot. If actorBase is known we compute from stride;
            // otherwise treat arg0 as a small integer (rare port-only path).
            var slot = -1;
            if (actorBase !== null) {
                slot = actorSlot(this.actor);
            }
            if (slot < 0) {
                // Fallback heuristic: lower 4 bits of arg0 as candidate slot.
                var v = args[0].toInt32 ? args[0].toInt32() : 0;
                if (v >= 0 && v < 12) slot = v;
            }
            this.slot = slot;

            // Capture inputs on entry (world_pos, linear_velocity).
            if (slot >= 0 && slot < 6) {
                var a = this.actor;
                this.wx = readS32(a.add(OFF_WORLD_X));
                this.wy = readS32(a.add(OFF_WORLD_Y));
                this.wz = readS32(a.add(OFF_WORLD_Z));
                this.vx = readS32(a.add(OFF_LV_X));
                this.vy = readS32(a.add(OFF_LV_Y));
                this.vz = readS32(a.add(OFF_LV_Z));
            } else {
                this.wx = 0; this.wy = 0; this.wz = 0;
                this.vx = 0; this.vy = 0; this.vz = 0;
            }
        },
        onLeave: function () {
            if (!raceConfirmed) return;
            if (fp === null) return;
            if (this.slot < 0 || this.slot >= 6) return;
            var a = this.actor;
            var fl = readS32(a.add(OFF_WCP_BASE + 0  + 4));   // wheel 0 y
            var fr = readS32(a.add(OFF_WCP_BASE + 12 + 4));   // wheel 1 y
            var rl = readS32(a.add(OFF_WCP_BASE + 24 + 4));   // wheel 2 y
            var rr = readS32(a.add(OFF_WCP_BASE + 36 + 4));   // wheel 3 y
            var wcb = readU8(a.add(OFF_WCB));
            var scf = readU8(a.add(OFF_SCF));
            // Upstream audit fields (2026-05-26): rotation Y-row + susp pos
            var m3 = 0, m4 = 0, m5 = 0;
            try {
                m3 = a.add(OFF_ROT_M3).readFloat();
                m4 = a.add(OFF_ROT_M4).readFloat();
                m5 = a.add(OFF_ROT_M5).readFloat();
            } catch (e) {}
            var sFL = readS32(a.add(OFF_SUSP_BASE +  0));
            var sFR = readS32(a.add(OFF_SUSP_BASE +  4));
            var sRL = readS32(a.add(OFF_SUSP_BASE +  8));
            var sRR = readS32(a.add(OFF_SUSP_BASE + 12));
            // wcp.x and wcp.z per wheel (12 byte stride, x at +0, z at +8)
            function rs32(off) { return readS32(a.add(off)); }
            function rs16(off) { try { return a.add(off).readS16(); } catch(e){return 0;} }
            function ru8 (off) { try { return a.add(off).readU8(); } catch(e){return 0;} }
            var wcpx = [];
            var wcpz = [];
            for (var wi = 0; wi < 4; wi++) {
                wcpx.push(rs32(OFF_WCP_BASE + wi*12 + 0));
                wcpz.push(rs32(OFF_WCP_BASE + wi*12 + 8));
            }
            // per-wheel probe span_index (int16) + sub_lane_index (int8)
            var wpsp = []; var wpsl = [];
            for (var wj = 0; wj < 4; wj++) {
                wpsp.push(rs16(OFF_WHEEL_PROBE_BASE + wj*0x10 + 0));
                wpsl.push(ru8 (OFF_WHEEL_PROBE_BASE + wj*0x10 + 0x0C));
            }
            var subT = subTickCounter++;
            var row = [
                BINARY_LABEL, this.slot, subT,
                this.wx, this.wy, this.wz,
                this.vx, this.vy, this.vz,
                fl, fr, rl, rr,
                wcb, scf,
                m3.toFixed(6), m4.toFixed(6), m5.toFixed(6),
                sFL, sFR, sRL, sRR,
                wcpx[0], wcpz[0], wcpx[1], wcpz[1], wcpx[2], wcpz[2], wcpx[3], wcpz[3],
                wpsp[0], wpsl[0], wpsp[1], wpsl[1], wpsp[2], wpsl[2], wpsp[3], wpsl[3]
            ].join(",");
            fp.write(row + "\n");
            if ((subT & 0x3F) === 0) {
                try { fp.flush(); } catch (e) {}
            }
        }
    });
    console.log("[chassis_probe] hook installed at " + addrRefresh + " (binary=" + BINARY_LABEL + ")");
}

// Periodic flush (250ms) so partial captures are still useful if the user
// kills the process.
var flushTimer = setInterval(function () {
    if (fp) {
        try { fp.flush(); } catch (e) {}
    }
}, 250);

Script.bindWeak(globalThis, function () {
    if (fp) {
        try { fp.flush(); fp.close(); } catch (e) {}
        fp = null;
    }
    clearInterval(flushTimer);
});
