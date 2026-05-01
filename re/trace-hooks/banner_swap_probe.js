// banner_swap_probe.js
// Reverse-direction "FINISH" banner research probe.
//
// Hooks:
//   0x0042FD70 RemapCheckpointOrderForTrackDirection
//   0x0040B530 SwapIndexedRuntimeEntries
//
// Goal: identify what arrays SwapIndexedRuntimeEntries actually mutates when
// called from RemapCheckpointOrderForTrackDirection during a reverse-direction
// race load. The memory file claims it operates on strip-runtime descriptor
// fields at *(int*)(0x48dc40+4) and *(int*)(0x48dc40+0xc), but the global
// catalog has 0x48dc40 = g_texCacheDescArray. Frida resolves the contradiction.
//
// Run alongside the quickrace launcher with reverse direction enabled:
//   cd C:/Users/maria/Desktop/Proyectos/TD5RE
//   # 1. Manual launch (M2DX-crash workaround):
//   cd original && cmd /c start "" "TD5_d3d.exe" && cd ..
//   # 2. Attach Frida:
//   python re/tools/quickrace/td5_quickrace.py ^
//     --no-ini --attach-name TD5_d3d.exe ^
//     --track 0 --car 0 --direction 1 --frontend-only false ^
//     --extra-script re/trace-hooks/banner_swap_probe.js
//
// Output: log/banner_swap_probe.log (text, single-shot at race load).

"use strict";

var OUT_PATH = "C:\\Users\\maria\\Desktop\\Proyectos\\TD5RE\\log\\banner_swap_probe.log";

var BASE = ptr(0x00400000);

// Functions
var ADDR_REMAP = BASE.add(0x0002FD70);  // RemapCheckpointOrderForTrackDirection
var ADDR_SWAP  = BASE.add(0x0000B530);  // SwapIndexedRuntimeEntries

// CHECKPT.NUM static buffer (96 bytes / 24 int32 / 4 rows x 6 cols)
var DAT_LUT       = BASE.add(0x000AEDB0);
var DAT_LUT_SIZE  = 0x60;

// Mode-selector sentinel (Col5/Row0 of LUT, but read via a separate symbol)
var DAT_SENTINEL  = BASE.add(0x000AEE00);

// Strip/texture runtime descriptor (still ambiguous — Frida will resolve).
// Memory file says +4 is 4-byte stride array, +0xc is 8-byte stride array.
// Catalog says this is g_texCacheDescArray. We dump both interpretations.
var DAT_RUNTIME   = BASE.add(0x0008DC40);

// gReverseTrackDirection (per analysis: 0x4AAF54)
var DAT_REVERSE   = BASE.add(0x000AAF54);

var fh = null;
var inRemap = false;
var swapCallCounter = 0;

function openLog() {
    try {
        fh = new File(OUT_PATH, "w");
        logLine("=== banner_swap_probe ===");
        logLine("hooks:");
        logLine("  0x" + ADDR_REMAP.toString(16) + " RemapCheckpointOrderForTrackDirection");
        logLine("  0x" + ADDR_SWAP.toString(16)  + " SwapIndexedRuntimeEntries");
        logLine("symbols:");
        logLine("  0x" + DAT_LUT.toString(16)      + " DAT_004aedb0 LUT (CHECKPT.NUM, 96 bytes)");
        logLine("  0x" + DAT_SENTINEL.toString(16) + " DAT_004aee00 sentinel");
        logLine("  0x" + DAT_RUNTIME.toString(16)  + " DAT_0048dc40 (g_texCacheDescArray ?)");
        logLine("  0x" + DAT_REVERSE.toString(16)  + " gReverseTrackDirection");
        send({kind:"log", msg:"[banner_swap] log opened: " + OUT_PATH});
    } catch (e) {
        send({kind:"log", msg:"[banner_swap] ERROR opening log: " + e});
    }
}

function logLine(s) {
    if (!fh) return;
    try { fh.write(s + "\n"); fh.flush(); } catch (e) {}
}

function hex8(n)  { return ("0000000" + (n >>> 0).toString(16)).slice(-8); }
function hex2(n)  { return ("0" + (n & 0xff).toString(16)).slice(-2); }

function hexDumpInt32s(addr, count) {
    var lines = [];
    for (var row = 0; row < count; row += 4) {
        var parts = [];
        for (var c = 0; c < 4 && row + c < count; c++) {
            try {
                var v = Memory.readU32(addr.add((row + c) * 4));
                parts.push(hex8(v));
            } catch (e) { parts.push("--------"); }
        }
        lines.push("    [+0x" + ("00" + (row*4).toString(16)).slice(-2) + "] " + parts.join(" "));
    }
    return lines.join("\n");
}

function hexDumpBytes(addr, len) {
    var bytes;
    try { bytes = Memory.readByteArray(addr, len); } catch (e) { return "    <unreadable>"; }
    var view = new Uint8Array(bytes);
    var lines = [];
    for (var i = 0; i < view.length; i += 16) {
        var parts = [];
        for (var j = 0; j < 16 && i + j < view.length; j++) {
            parts.push(hex2(view[i + j]));
        }
        lines.push("    [+0x" + hex2(i) + "] " + parts.join(" "));
    }
    return lines.join("\n");
}

function safeReadPtr(p) {
    try { return Memory.readPointer(p); } catch (e) { return null; }
}

function dumpRuntimeStruct(label) {
    logLine("  " + label + " runtime struct (DAT_0048dc40):");
    var basePtr = safeReadPtr(DAT_RUNTIME);
    logLine("    *(0x48dc40)        = " + (basePtr ? basePtr : "<unreadable>"));
    if (basePtr === null || basePtr.isNull()) return;

    // Dump first 0x20 bytes of the struct as int32 to see all field pointers.
    logLine("    struct contents (first 32 bytes as int32):");
    logLine(hexDumpInt32s(basePtr, 8));

    // Pull and label specific fields.
    var p_plus4  = safeReadPtr(basePtr.add(0x04));
    var p_plus8  = safeReadPtr(basePtr.add(0x08));
    var p_plusc  = safeReadPtr(basePtr.add(0x0c));
    logLine("    struct+0x04 ->     " + (p_plus4 ? p_plus4 : "<null>"));
    logLine("    struct+0x08 ->     " + (p_plus8 ? p_plus8 : "<null>"));
    logLine("    struct+0x0c ->     " + (p_plusc ? p_plusc : "<null>"));
}

function dumpSwapTargets(label, p1, p2) {
    var basePtr = safeReadPtr(DAT_RUNTIME);
    if (basePtr === null || basePtr.isNull()) return;
    var arr4_ptr = safeReadPtr(basePtr.add(0x04));
    var arr8_ptr = safeReadPtr(basePtr.add(0x0c));
    logLine("    " + label + " entries at p1=" + p1 + " p2=" + p2 + ":");

    // 4-byte stride at +0x04
    if (arr4_ptr && !arr4_ptr.isNull()) {
        try {
            var v1 = Memory.readU32(arr4_ptr.add(p1 * 4));
            var v2 = Memory.readU32(arr4_ptr.add(p2 * 4));
            logLine("      arr@+0x04[" + p1 + "] (4B) = 0x" + hex8(v1));
            logLine("      arr@+0x04[" + p2 + "] (4B) = 0x" + hex8(v2));
        } catch (e) { logLine("      arr@+0x04: <read error>"); }
    }
    // 8-byte stride at +0x0c
    if (arr8_ptr && !arr8_ptr.isNull()) {
        try {
            var v1a = Memory.readU32(arr8_ptr.add(p1 * 8));
            var v1b = Memory.readU32(arr8_ptr.add(p1 * 8 + 4));
            var v2a = Memory.readU32(arr8_ptr.add(p2 * 8));
            var v2b = Memory.readU32(arr8_ptr.add(p2 * 8 + 4));
            logLine("      arr@+0x0c[" + p1 + "] (8B) = 0x" + hex8(v1a) + " 0x" + hex8(v1b));
            logLine("      arr@+0x0c[" + p2 + "] (8B) = 0x" + hex8(v2a) + " 0x" + hex8(v2b));
        } catch (e) { logLine("      arr@+0x0c: <read error>"); }
    }
}

function dumpLUT(label) {
    logLine("  " + label + " LUT (DAT_004aedb0, 96 bytes / 24 int32 / 4 rows x 6 cols):");
    logLine(hexDumpInt32s(DAT_LUT, 24));
    try {
        var sentinel = Memory.readS32(DAT_SENTINEL);
        logLine("    sentinel(0x4AEE00) = " + sentinel + " (0x" + hex8(sentinel) + ")");
    } catch (e) {}
    try {
        var rev = Memory.readS32(DAT_REVERSE);
        logLine("    gReverseTrackDirection(0x4AAF54) = " + rev);
    } catch (e) {}
}

openLog();

Interceptor.attach(ADDR_REMAP, {
    onEnter: function (args) {
        inRemap = true;
        swapCallCounter = 0;
        logLine("\n========================================");
        logLine("[REMAP entry @ 0x42FD70] return_addr=" + this.returnAddress);
        dumpLUT("BEFORE");
        dumpRuntimeStruct("BEFORE");
    },
    onLeave: function (rv) {
        logLine("[REMAP leave] swaps_seen=" + swapCallCounter);
        dumpLUT("AFTER");
        dumpRuntimeStruct("AFTER");
        logLine("========================================");
        inRemap = false;
    }
});

Interceptor.attach(ADDR_SWAP, {
    onEnter: function (args) {
        // __cdecl: param_1 at [esp+4], param_2 at [esp+8]
        var sp = this.context.esp;
        try {
            this._p1 = Memory.readS32(sp.add(4));
            this._p2 = Memory.readS32(sp.add(8));
        } catch (e) { this._p1 = -1; this._p2 = -1; }
        this._inRemap = inRemap;
        if (inRemap) swapCallCounter++;
        logLine("\n[SWAP @ 0x40B530 #" + swapCallCounter + "] in_remap=" + this._inRemap +
                "  p1=" + this._p1 + " p2=" + this._p2 +
                "  caller=" + this.returnAddress);
        // Always dump even outside remap so we know if this fn is called elsewhere
        if (this._p1 >= 0 && this._p2 >= 0 && this._p1 < 4096 && this._p2 < 4096) {
            dumpSwapTargets("BEFORE", this._p1, this._p2);
        } else {
            logLine("    (p1/p2 out of sane range, skipping target dump)");
        }
    },
    onLeave: function (rv) {
        if (this._p1 >= 0 && this._p2 >= 0 && this._p1 < 4096 && this._p2 < 4096) {
            dumpSwapTargets("AFTER ", this._p1, this._p2);
        }
    }
});

send({kind:"log", msg:"[banner_swap] hooks installed at 0x42FD70 + 0x40B530"});
