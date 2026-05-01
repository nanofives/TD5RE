"""
frida_trace_fullscreen.py
=========================
Spawn-and-trace helper to find what's flipping the (windowed-patched)
original TD5_d3d.exe into fullscreen on frontend boot.

Hooks:
  - M2DX!DXD3D::FullScreen                  (10002170)
  - M2DX!ConfigureDirectDrawCooperativeLevel(100078a0)  -- param_2=1 = exclusive
  - M2DX!ApplyDirectDrawDisplayMode         (10008ed0)  -- SetDisplayMode
  - writes to DAT_10061c1c                  (10061c1c)  -- fullscreen flag
Captures Thread.backtrace(ACCURATE) on entry so we can see who is calling.

Also prints all module load events so we know when M2DX becomes hookable.
"""

import os, sys, time, frida

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(HERE, "frida_trace_fullscreen.log")
GAME = os.path.abspath(os.path.join(HERE, "..", "..", "original", "TD5_d3d.exe"))
GAME_DIR = os.path.dirname(GAME)

SCRIPT = r"""
'use strict';

function hookAt(mod, rva, label, capture_bt) {
    try {
        var p = mod.base.add(rva);
        Interceptor.attach(p, {
            onEnter: function (args) {
                var info = { kind: 'call', fn: label };
                try {
                    info.arg0 = args[0].toString();
                    info.arg1 = args[1].toString();
                } catch (e) {}
                if (capture_bt) {
                    try {
                        info.bt = Thread.backtrace(this.context, Backtracer.ACCURATE)
                            .slice(0, 10)
                            .map(DebugSymbol.fromAddress)
                            .map(function (s) { return s.toString(); });
                    } catch (e) { info.bt_err = e.toString(); }
                }
                send(info);
            }
        });
        send({ kind: 'hook_ok', label: label, addr: p.toString() });
    } catch (e) {
        send({ kind: 'hook_fail', label: label, err: e.toString() });
    }
}

function watchWrite32(mod, rva, label) {
    try {
        var p = mod.base.add(rva);
        MemoryAccessMonitor.enable([{ base: p, size: 4 }], {
            onAccess: function (details) {
                if (details.operation !== 'write') return;
                var info = {
                    kind: 'write',
                    label: label,
                    from: details.from.toString(),
                    addr: details.address.toString(),
                    op: details.operation
                };
                try {
                    info.bt = Thread.backtrace(this.context, Backtracer.ACCURATE)
                        .slice(0, 10)
                        .map(DebugSymbol.fromAddress)
                        .map(function (s) { return s.toString(); });
                } catch (e) {}
                send(info);
            }
        });
        send({ kind: 'watch_ok', label: label, addr: p.toString() });
    } catch (e) {
        send({ kind: 'watch_fail', label: label, err: e.toString() });
    }
}

function installM2dxHooks() {
    var m = Process.findModuleByName('M2DX.dll');
    if (!m) return false;
    send({ kind: 'module_loaded', name: 'M2DX.dll', base: m.base.toString() });
    hookAt(m, 0x2170, 'DXD3D::FullScreen', true);
    hookAt(m, 0x78a0, 'ConfigureCoopLevel', true);
    hookAt(m, 0x8ed0, 'ApplyDirectDrawDisplayMode', true);
    hookAt(m, 0x12750, 'ApplyStartupOptionToken', false);
    return true;
}

// Peek the current value of DAT_10061c1c every time a suspect function runs.
function readFlag() {
    var m = Process.findModuleByName('M2DX.dll');
    if (!m) return null;
    try { return Memory.readU32(m.base.add(0x61c1c)); }
    catch (e) { return null; }
}

var td5 = Process.findModuleByName('TD5_d3d.exe');
if (td5) {
    send({ kind: 'module_loaded', name: 'TD5_d3d.exe', base: td5.base.toString() });
}

if (!installM2dxHooks()) {
    var pLL = Module.getExportByName('kernel32.dll', 'LoadLibraryA');
    var pLLW = Module.getExportByName('kernel32.dll', 'LoadLibraryW');
    Interceptor.attach(pLL, {
        onLeave: function (retval) {
            if (!retval.isNull() && !Process.findModuleByName('M2DX.dll')) return;
            installM2dxHooks();
        }
    });
    Interceptor.attach(pLLW, {
        onLeave: function (retval) {
            if (!retval.isNull() && !Process.findModuleByName('M2DX.dll')) return;
            installM2dxHooks();
        }
    });
}

send({ kind: 'ready' });
"""

def main():
    if not os.path.exists(GAME):
        print(f"ERROR: {GAME} not found"); return 1
    log = open(LOG, "w", encoding="utf-8")
    def w(s):
        print(s); log.write(s + "\n"); log.flush()

    w(f"[+] Spawning {GAME}")
    pid = frida.spawn(GAME, cwd=GAME_DIR)
    session = frida.attach(pid)
    script = session.create_script(SCRIPT)

    def on_message(msg, data):
        if msg["type"] == "send":
            w(f"[trace] {msg['payload']}")
        elif msg["type"] == "error":
            w(f"[error] {msg.get('description','')}")

    script.on("message", on_message)
    script.load()
    frida.resume(pid)
    w(f"[+] pid={pid} resumed")

    deadline = time.time() + 15
    while time.time() < deadline:
        try:
            os.kill(pid, 0)
        except OSError:
            w("[+] process exited"); break
        time.sleep(0.25)
    w("[+] 15s trace window elapsed, detaching")
    try: session.detach()
    except Exception: pass
    try:
        import subprocess
        subprocess.run(["taskkill", "/F", "/PID", str(pid)],
                       capture_output=True, text=True)
    except Exception: pass
    log.close()
    return 0

if __name__ == "__main__":
    sys.exit(main())
