// frida_target_probe.js
// Captures the dx / dz inputs to AngleFromVector12 inside UpdateActorTrackBehavior
// for slot 0 on the original TD5_d3d.exe. Pairs with the port's `target_probe`
// log line (td5_ai.c:1908) so /diff-race-style debugging can confirm whether
// the port's 10× steering bias is driven by divergent dx formation.
//
// Usage: pass via td5_quickrace.py --extra-script
//
// RVA references (image base 0x00400000):
//   UpdateActorTrackBehavior  0x00034FE0  -> 0x00434FE0 in memory
//   AngleFromVector12         0x0000A720  -> 0x0040A720
//   gActorTrackReferenceSlot  0x000B1B58  -> 0x004B1B58 (per RS dword 0x37 stride 0x11c)

'use strict';

(function () {
    var mod = Process.findModuleByName('TD5_d3d.exe');
    if (!mod) { send({kind: 'log', msg: 'target_probe: module not loaded yet'}); return; }
    var base = mod.base;

    var UATB        = base.add(0x00034FE0);
    var ANGLE_FN    = base.add(0x0000A720);

    var inUATB     = false;
    var currentSlot = -1;
    var sampleCount = 0;
    var maxSamples  = 60;   // first 60 hits = ~2 seconds @ 30 Hz

    Interceptor.attach(UATB, {
        onEnter: function (args) {
            inUATB = true;
            currentSlot = args[0].toInt32();
        },
        onLeave: function () {
            inUATB = false;
            currentSlot = -1;
        }
    });

    Interceptor.attach(ANGLE_FN, {
        onEnter: function (args) {
            if (!inUATB) return;
            if (currentSlot !== 0) return;
            if (sampleCount >= maxSamples) return;
            // AngleFromVector12 is __cdecl(dx, dz) per port memory note
            // reference_anglefromvector12_convention.md (the original takes
            // (dz, dx); the port's atan2(dx, dz) flipped semantics). For raw
            // capture we just log both args verbatim and the user can map.
            var a0 = args[0].toInt32();
            var a1 = args[1].toInt32();
            send({
                kind: 'log',
                msg: 'orig_target_probe: slot=0 hit=' + sampleCount +
                     ' arg0=' + a0 + ' arg1=' + a1
            });
            sampleCount++;
        }
    });

    send({kind: 'log', msg: 'target_probe: hooks installed @ UATB=' + UATB +
                            ' AngleFn=' + ANGLE_FN});
})();
