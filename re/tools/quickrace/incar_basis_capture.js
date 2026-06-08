// Capture the ORIGINAL in-car/bumper camera basis matrix.
// Frida 17 API: NativePointer methods (ptr.readS32()/writeInt()/readFloat()),
// NOT the removed Memory.readX()/Memory.writeX() module functions.
//
// RunRaceFrame view-0 camera dispatch (disasm 0x42bce0..0x42bd20):
//   0x42bce0 MOV EAX,[0x466e9c]   ; g_inputPlaybackActive
//   0x42bce8 CMP EAX,EBP          ; EBP==0
//   0x42bcea JNZ 0x42bd56         ; inPlay!=0 -> trackside
//   0x42bcec CMP [0x4aaf64],EBP   ; viewZ gate
//   0x42bcf2 JNZ 0x42bd56         ; viewZ!=0 -> trackside
//   0x42bcf4 CMP [0x482fd8],EBP   ; gRaceCameraPresetMode
//   0x42bcfa JZ  0x42bd2a         ; mode==0 -> orbit/chase
//   0x42bcfc CMP [0x4aaef0],EBP   ; camera transition timer
//   0x42bd02 JG  0x42bd2a         ; timer>0 -> orbit/chase
//   0x42bd20 CALL 0x401c20        ; else -> UpdateVehicleRelativeCamera (IN-CAR)
// To force in-car: inPlay==0, viewZ==0, mode!=0, timer<=0.
(function () {
  function L(m) { send({ kind: "log", msg: m }); }

  var mod = Process.findModuleByName("TD5_d3d.exe");
  var base = mod ? mod.base : null;
  if (base === null) { L("BASIS-ERR: TD5_d3d.exe base not found"); return; }
  L("BASIS: base=" + base);

  var pInPlay = base.add(0x66e9c); // g_inputPlaybackActive
  var pViewZ  = base.add(0xaaf64); // view_z gate
  var pMode0  = base.add(0x82fd8); // gRaceCameraPresetMode view0
  var pMode1  = base.add(0x82fdc); // view1
  var pTimer0 = base.add(0xaaef0); // camera transition timer (view0 gate)
  var pBasis  = base.add(0xaafa0); // g_cameraBasis (9 floats row-major)

  var RVA_RUNRACEFRAME = 0x2b580;
  var RVA_UPDATECAM    = 0x1c20;
  var RVA_BUILDBASIS   = 0x2d0b0;
  var RVA_SEQ_START    = 0x2bce0; // MOV EAX,[0x466e9c] (dispatch sequence start)

  // Belt: force gates at RunRaceFrame onEnter.
  Interceptor.attach(base.add(RVA_RUNRACEFRAME), {
    onEnter: function () {
      pMode0.writeInt(1); pMode1.writeInt(1);
      pTimer0.writeInt(0);
    }
  });

  // Suspenders: force gates right at the start of the view-0 dispatch sequence,
  // so the two JNZ-to-trackside and JZ/JG-to-orbit all fall through to the
  // in-car CALL. This runs AFTER the inner sim loop that re-increments timer.
  var seqSeen = 0;
  Interceptor.attach(base.add(RVA_SEQ_START), {
    onEnter: function () {
      if (seqSeen < 4) {
        L("SEQ " + seqSeen +
          " inPlay=" + pInPlay.readS32() +
          " viewZ=0x" + pViewZ.readU32().toString(16) +
          " mode=" + pMode0.readS32() +
          " timer=" + pTimer0.readS32());
        seqSeen++;
      }
      pInPlay.writeInt(0);
      pViewZ.writeInt(0);
      pMode0.writeInt(1);
      pTimer0.writeInt(0);
    }
  });

  // Capture the short[3] angle array passed to BuildCameraBasisFromAngles.
  var lastBuildAngles = null;
  Interceptor.attach(base.add(RVA_BUILDBASIS), {
    onEnter: function (args) {
      var cands = [];
      try { cands.push(args[0]); } catch (e) {}
      try { cands.push(ptr(this.context.ecx)); } catch (e) {}
      try { cands.push(ptr(this.context.eax)); } catch (e) {}
      for (var c = 0; c < cands.length; c++) {
        var p = cands[c];
        if (!p || p.isNull()) continue;
        try {
          var s0 = p.readS16();
          var s1 = p.add(2).readS16();
          var s2 = p.add(4).readS16();
          lastBuildAngles = [s0, s1, s2, "argidx=" + c];
          break;
        } catch (e) {}
      }
    }
  });

  var samples = 0;
  var MAX = 12;
  var camEntries = 0;

  Interceptor.attach(base.add(RVA_UPDATECAM), {
    onEnter: function (args) {
      camEntries++;
      if (camEntries === 1) L("BASIS: UpdateVehicleRelativeCamera FIRED (in-car active)");
      // __cdecl: param_1 (actor ptr) is first stack arg. Caller pushes it.
      this.a0 = null; this.a1 = null; this.a2 = null;
      var tries = [];
      try { tries.push(args[0]); } catch (e) {}
      try { tries.push(ptr(this.context.esi)); } catch (e) {}
      for (var t = 0; t < tries.length; t++) {
        var actor = tries[t];
        if (!actor || actor.isNull()) continue;
        try {
          this.a0 = actor.add(0x208).readS16(); // pitch
          this.a1 = actor.add(0x20A).readS16(); // yaw
          this.a2 = actor.add(0x20C).readS16(); // roll
          break;
        } catch (e) { this.a0 = null; this.a1 = null; this.a2 = null; }
      }
    },
    onLeave: function () {
      if (samples >= MAX) return;
      try {
        var f = [];
        for (var i = 0; i < 9; i++) f.push(pBasis.add(i * 4).readFloat());
        function fmt(x) { return x.toFixed(4); }
        var ba = lastBuildAngles
          ? ("[" + lastBuildAngles[0] + "," + lastBuildAngles[1] + "," + lastBuildAngles[2] + " " + lastBuildAngles[3] + "]")
          : "[none]";
        L("SAMPLE " + samples +
          " | actorAngles pitch=" + this.a0 + " yaw=" + this.a1 + " roll=" + this.a2 +
          " | buildAngles=" + ba +
          " | right=(" + fmt(f[0]) + "," + fmt(f[1]) + "," + fmt(f[2]) + ")" +
          " up=(" + fmt(f[3]) + "," + fmt(f[4]) + "," + fmt(f[5]) + ")" +
          " fwd=(" + fmt(f[6]) + "," + fmt(f[7]) + "," + fmt(f[8]) + ")");
        samples++;
        if (samples === MAX) L("BASIS: captured " + MAX + " samples (done)");
      } catch (e) {
        L("BASIS-ERR onLeave: " + e);
      }
    }
  });

  L("BASIS: hooks installed (RunRaceFrame=" + base.add(RVA_RUNRACEFRAME) +
    " UpdateCam=" + base.add(RVA_UPDATECAM) + " basis=" + pBasis + ")");
})();
