"""
frida_windowed.py
=================
Runtime windowed-mode enforcer for the original Test Drive 5 (TD5_d3d.exe).

Spawns the game under Frida and replaces M2DX's DXD3D::FullScreen with a
no-op stub that returns 1 (success). That bypasses the exclusive-mode
cooperative-level flip and the SetDisplayMode call, so the game stays in
DDSCL_NORMAL at whatever desktop resolution the user is running.

Keeps the binaries pristine — no static patches needed.
"""

import os
import sys
import subprocess
import time
import frida

HERE = os.path.dirname(os.path.abspath(__file__))
GAME = os.path.abspath(os.path.join(HERE, "..", "..", "original", "TD5_d3d.exe"))
GAME_DIR = os.path.dirname(GAME)

# RVAs inside M2DX.dll (image base 0x10000000)
RVA_FULLSCREEN = 0x2170

SCRIPT = r"""
'use strict';

var RVA_FULLSCREEN   = %d;
var RVA_COOPLEVEL    = 0x78a0;
var RVA_DISPLAYMODE  = 0x8ed0;

function bt(ctx) {
    try {
        return Thread.backtrace(ctx, Backtracer.ACCURATE)
            .slice(0, 10)
            .map(DebugSymbol.fromAddress)
            .map(function (s) { return s.toString(); });
    } catch (e) { return ['bt_err: ' + e]; }
}

function installHooks() {
    var m = Process.findModuleByName('M2DX.dll');
    if (!m) return false;

    // 1. Byte-patch DXD3D::FullScreen entry to "mov eax, 1 ; ret"
    //    B8 01 00 00 00   MOV EAX, 1
    //    C3               RET
    var pFull = m.base.add(RVA_FULLSCREEN);
    Memory.protect(pFull, 8, 'rwx');
    pFull.writeByteArray([0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3]);
    send({ kind: 'fullscreen_stub_installed', addr: pFull.toString(),
           bytes: pFull.readByteArray(6) });

    // 2. Trace ConfigureDirectDrawCooperativeLevel(hWnd, mode)
    Interceptor.attach(m.base.add(RVA_COOPLEVEL), {
        onEnter: function (args) {
            send({
                kind: 'call_cooplevel',
                hWnd: args[0].toString(),
                mode: args[1].toInt32(),
                bt: bt(this.context)
            });
        }
    });

    // 3. Trace ApplyDirectDrawDisplayMode(w, h, bpp) -- this is SetDisplayMode
    Interceptor.attach(m.base.add(RVA_DISPLAYMODE), {
        onEnter: function (args) {
            send({
                kind: 'call_setdisplaymode',
                w: args[0].toInt32(),
                h: args[1].toInt32(),
                bpp: args[2].toInt32(),
                bt: bt(this.context)
            });
        }
    });

    return true;
}

if (!installHooks()) {
    var pLLA = Module.getExportByName('kernel32.dll', 'LoadLibraryA');
    var pLLW = Module.getExportByName('kernel32.dll', 'LoadLibraryW');
    var installed = false;
    function tryInstall() { if (!installed && installHooks()) installed = true; }
    Interceptor.attach(pLLA, { onLeave: tryInstall });
    Interceptor.attach(pLLW, { onLeave: tryInstall });
}

send({ kind: 'ready' });
""" % RVA_FULLSCREEN


LOG_PATH = os.path.join(HERE, "frida_windowed.log")

def main():
    if not os.path.exists(GAME):
        print(f"ERROR: {GAME} not found"); return 1

    log = open(LOG_PATH, "w", encoding="utf-8", buffering=1)
    def w(s):
        print(s, flush=True)
        log.write(s + "\n")
        log.flush()

    w(f"[+] Spawning {GAME}")
    try:
        pid = frida.spawn(GAME, cwd=GAME_DIR)
    except frida.NotSupportedError as e:
        print(f"[!] Frida spawn failed: {e}")
        print("[!] This usually means an app-compat shim is set on TD5_d3d.exe")
        print("[!] and requires elevation. Remove the shim temporarily, or run")
        print("[!] this script elevated.")
        return 2

    session = frida.attach(pid)
    script = session.create_script(SCRIPT)

    def on_message(msg, _data):
        if msg["type"] == "send":
            w(f"[frida] {msg['payload']}")
        elif msg["type"] == "error":
            w(f"[error] {msg.get('description','')}")

    script.on("message", on_message)
    script.load()
    frida.resume(pid)
    w(f"[+] pid={pid} resumed - FullScreen stub installed")

    try:
        while True:
            try:
                os.kill(pid, 0)
            except OSError:
                w("[+] game process exited"); break
            time.sleep(0.5)
    except KeyboardInterrupt:
        w("[+] detaching")
    finally:
        try: session.detach()
        except Exception: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
