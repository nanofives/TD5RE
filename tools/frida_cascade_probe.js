/**
 * frida_cascade_probe.js -- Differential cascade probe (find_offset_peer).
 *
 * Hooks FindActorTrackOffsetPeer @ 0x004337E0 in TD5_d3d.exe (and the same
 * exported function in td5re.exe -- resolved by symbol). Per call we capture
 * the full RS state that decides whether the function finds a valid peer
 * slot or returns "no peer".
 *
 * onEnter:
 *   self_slot, sub_tick,
 *   route_table_ptr     (RS +0x00),
 *   bias                (RS +0x24),
 *   span_norm           (actor +0x82),
 *   FWD_TRACK_COMP      (RS +0x18),
 *   ACTIVE_LOWER        (RS +0x16),
 *   ACTIVE_UPPER        (RS +0x17),
 *   RIGHT_BOUNDARY_A    (RS +0x15),
 *   RIGHT_BOUNDARY_B    (RS +0x14),
 *   g_lateral_avoidance_direction (global)
 *
 * onLeave:
 *   peer_returned       (return value; -1 = no-peer sentinel in port,
 *                        self_slot = no-peer sentinel in orig --
 *                        see reference_arch_find_offset_peer_return_minus_one.md)
 *
 * Output CSV columns (single row per call):
 *   binary, self_slot, sub_tick, route_ptr, bias, span_norm, fwd_track,
 *   active_lo, active_hi, right_a, right_b, lat_dir, peer_returned
 *
 * Reference: reference_steering_cascade_root_cause_find_offset_peer.md
 */

"use strict";

// ===== Orchestrator-substituted vars =====
var OUTPUT_PATH  = "C:/Users/maria/Desktop/Proyectos/TD5RE/log/diff_cascade_default.csv";
var BINARY_LABEL = "orig";    // "orig" | "port"

// ===== Constants -- ORIGINAL binary =====
var ORIG_ADDR_FindActorTrackOffsetPeer = ptr(0x004337E0);
var ORIG_ADDR_RunRaceFrame             = ptr(0x0042B580);
var ORIG_ACTOR_BASE         = ptr(0x004AB108);
var ORIG_ACTOR_STRIDE       = 0x388;
var ORIG_ROUTE_STATE_BASE   = ptr(0x004AFBB8);
var ORIG_ROUTE_STATE_STRIDE = 0x11C;
var ORIG_G_simTick          = ptr(0x004AADA0);
var ORIG_G_gameState        = ptr(0x004C3CE8);
var ORIG_G_lateralAvoidDir  = ptr(0x004B08B0);   // global mentioned in brief

// RS field offsets (per reference_steering_cascade_root_cause_find_offset_peer.md):
var OFF_RS_ROUTE_PTR     = 0x00;   // route_table_ptr (s32)
var OFF_RS_RIGHT_B       = 0x14;   // RIGHT_BOUNDARY_B (u8)
var OFF_RS_RIGHT_A       = 0x15;   // RIGHT_BOUNDARY_A (u8)
var OFF_RS_ACTIVE_LO     = 0x16;   // ACTIVE_LOWER_BOUND (u8)
var OFF_RS_ACTIVE_HI     = 0x17;   // ACTIVE_UPPER_BOUND (u8)
var OFF_RS_FWD_TRACK     = 0x18;   // FWD_TRACK_COMP (s32)
var OFF_RS_BIAS          = 0x24;   // RS_TRACK_OFFSET_BIAS (s32)

// Actor field offsets:
var OFF_ACTOR_SPAN_NORM  = 0x82;   // track_span_normalized (s16)

// ===== Runtime ==============================================================

var addrFindPeer = null;
var addrRunRaceFrame = null;
var actorBase = null;
var actorStride = 0x388;
var rsBase = null;
var rsStride = 0x11C;
var gSimTick = null;
var gGameState = null;
var gLateralAvoid = null;

function readS32(p) { try { return p.readS32(); } catch (e) { return 0; } }
function readS16(p) { try { return p.readS16(); } catch (e) { return 0; } }
function readU8 (p) { try { return p.readU8 (); } catch (e) { return 0; } }

function rsSlot(rsPtr) {
    if (rsBase === null) return -1;
    var d = rsPtr.sub(rsBase).toInt32();
    if (d < 0) return -1;
    var slot = Math.floor(d / rsStride);
    if (slot * rsStride !== d) return -1;
    if (slot < 0 || slot > 11) return -1;
    return slot;
}

function actorPtr(slot) {
    if (actorBase === null) return null;
    return actorBase.add(slot * actorStride);
}

// Monotonic per-call sub-tick counter (Frida side).
var subTickCounter = 0;
var raceConfirmed = false;
var fridaFrameCounter = 0;

var fp = null;

function openOutput() {
    try {
        fp = new File(OUTPUT_PATH, "w");
        fp.write("binary,self_slot,sub_tick,route_ptr,bias,span_norm,fwd_track," +
                 "active_lo,active_hi,right_a,right_b,lat_dir,peer_returned\n");
        fp.flush();
        console.log("[cascade_probe] (" + BINARY_LABEL + ") -> " + OUTPUT_PATH);
    } catch (e) {
        console.log("[cascade_probe] ERROR opening output: " + e);
        fp = null;
    }
}

function resolveAddresses() {
    if (BINARY_LABEL === "orig") {
        addrFindPeer = ORIG_ADDR_FindActorTrackOffsetPeer;
        addrRunRaceFrame = ORIG_ADDR_RunRaceFrame;
        actorBase = ORIG_ACTOR_BASE;
        rsBase = ORIG_ROUTE_STATE_BASE;
        gSimTick = ORIG_G_simTick;
        gGameState = ORIG_G_gameState;
        gLateralAvoid = ORIG_G_lateralAvoidDir;
        return true;
    }
    // Port path -- resolve by exported symbol.
    var exp = Module.findExportByName("td5re.exe", "td5_ai_find_offset_peer");
    if (exp === null) {
        exp = Module.findExportByName("td5re.exe", "_td5_ai_find_offset_peer");
    }
    if (exp === null) {
        console.log("[cascade_probe] ERROR: cannot resolve td5_ai_find_offset_peer in td5re.exe");
        return false;
    }
    addrFindPeer = exp;
    // Optional helpers -- resolve by export when present.
    var gRS = Module.findExportByName("td5re.exe", "g_route_state_table");
    if (gRS !== null) rsBase = gRS;
    var gActors = Module.findExportByName("td5re.exe", "g_actors");
    if (gActors !== null) actorBase = gActors;
    var gST = Module.findExportByName("td5re.exe", "g_simulation_tick_counter");
    if (gST !== null) gSimTick = gST;
    var gGS = Module.findExportByName("td5re.exe", "g_game_state");
    if (gGS !== null) gGameState = gGS;
    var gLA = Module.findExportByName("td5re.exe", "g_lateral_avoidance_direction");
    if (gLA !== null) gLateralAvoid = gLA;
    if (rsBase === null) {
        console.log("[cascade_probe] WARN: g_route_state_table not exported; slot resolution disabled");
    }
    return true;
}

// ===== Hook installation ====================================================

if (!resolveAddresses()) {
    console.log("[cascade_probe] address resolution failed; aborting install");
} else {
    openOutput();

    if (addrRunRaceFrame !== null) {
        Interceptor.attach(addrRunRaceFrame, {
            onEnter: function () {
                fridaFrameCounter++;
                if (!raceConfirmed && gGameState !== null) {
                    if (readS32(gGameState) === 2) {
                        raceConfirmed = true;
                        console.log("[cascade_probe] race confirmed at frame=" + fridaFrameCounter);
                    }
                }
            }
        });
    } else {
        raceConfirmed = true;
    }

    Interceptor.attach(addrFindPeer, {
        onEnter: function (args) {
            // FindActorTrackOffsetPeer signature: takes (RS*); slot derived
            // from RS pointer. Port may take an int -- detect via heuristic.
            this.rs = args[0];
            var slot = -1;
            if (rsBase !== null) {
                slot = rsSlot(this.rs);
            }
            if (slot < 0) {
                // Fallback: treat as small integer slot index.
                var v = args[0].toInt32 ? args[0].toInt32() : -1;
                if (v >= 0 && v < 12) {
                    slot = v;
                    if (rsBase !== null) this.rs = rsBase.add(v * rsStride);
                }
            }
            this.slot = slot;

            // Snapshot inputs (read in onEnter so we get the pre-call state).
            if (slot >= 0 && slot < 6 && rsBase !== null) {
                this.route_ptr = readS32(this.rs.add(OFF_RS_ROUTE_PTR));
                this.bias      = readS32(this.rs.add(OFF_RS_BIAS));
                this.fwd_track = readS32(this.rs.add(OFF_RS_FWD_TRACK));
                this.active_lo = readU8 (this.rs.add(OFF_RS_ACTIVE_LO));
                this.active_hi = readU8 (this.rs.add(OFF_RS_ACTIVE_HI));
                this.right_a   = readU8 (this.rs.add(OFF_RS_RIGHT_A));
                this.right_b   = readU8 (this.rs.add(OFF_RS_RIGHT_B));
                // Span_norm pulled from actor (parallel array).
                if (actorBase !== null) {
                    this.span_norm = readS16(actorPtr(slot).add(OFF_ACTOR_SPAN_NORM));
                } else {
                    this.span_norm = 0;
                }
                this.lat_dir = gLateralAvoid !== null ? readS32(gLateralAvoid) : 0;
            } else {
                this.route_ptr = 0; this.bias = 0; this.span_norm = 0;
                this.fwd_track = 0; this.active_lo = 0; this.active_hi = 0;
                this.right_a = 0; this.right_b = 0; this.lat_dir = 0;
            }
        },
        onLeave: function (retval) {
            if (!raceConfirmed) return;
            if (fp === null) return;
            if (this.slot < 0 || this.slot >= 6) return;
            var peer = retval.toInt32();
            var subT = subTickCounter++;
            var row = [
                BINARY_LABEL, this.slot, subT,
                this.route_ptr, this.bias, this.span_norm, this.fwd_track,
                this.active_lo, this.active_hi, this.right_a, this.right_b,
                this.lat_dir, peer
            ].join(",");
            fp.write(row + "\n");
            if ((subT & 0x3F) === 0) {
                try { fp.flush(); } catch (e) {}
            }
        }
    });
    console.log("[cascade_probe] hook installed at " + addrFindPeer + " (binary=" + BINARY_LABEL + ")");
}

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
