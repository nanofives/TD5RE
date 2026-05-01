"""
trace_windowed_crash.py
=======================
Launch TD5_d3d.exe under Frida and capture the unhandled exception address
+ a lightweight call trace of M2DX windowed-mode critical functions so we
can figure out why the original binary crashes on race entry after the
windowed patches are applied.

Usage:
  python re/tools/trace_windowed_crash.py

Stops automatically when the target exits or when the user presses Ctrl+C.
The raw trace is written to re/tools/trace_windowed_crash.log.
"""

import os
import sys
import time
import frida

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(HERE, "trace_windowed_crash.log")
GAME_DIR = os.path.abspath(os.path.join(HERE, "..", "..", "original"))
GAME_EXE = os.path.join(GAME_DIR, "TD5_d3d.exe")

SCRIPT = r"""
'use strict';

function addr(mod, rva) {
    return mod.base.add(rva);
}

function readCString(p) {
    if (p.isNull()) return '(null)';
    try { return Memory.readCString(p); }
    catch (e) { return '<unreadable>'; }
}

function hookFn(mod, rva, name, onEnter, onLeave) {
    var p = addr(mod, rva);
    try {
        Interceptor.attach(p, { onEnter: onEnter, onLeave: onLeave });
        send({kind: 'hook', name: name, addr: p.toString()});
    } catch (e) {
        send({kind: 'hook_fail', name: name, err: e.toString()});
    }
}

var m2dx = Process.findModuleByName('M2DX.dll');
var td5 = Process.findModuleByName('TD5_d3d.exe');

function installHooks() {
    m2dx = Process.findModuleByName('M2DX.dll');
    if (!m2dx) return false;
    send({kind: 'module', name: 'M2DX.dll', base: m2dx.base.toString()});

    // Msg() - error dialogs
    hookFn(m2dx, 0x11120, 'Msg',
        function (args) { send({kind: 'call', fn: 'Msg', msg: readCString(args[0])}); });

    // ApplyDirectDrawDisplayMode(w, h, bpp)
    hookFn(m2dx, 0x8ED0, 'ApplyDirectDrawDisplayMode',
        function (args) { send({kind: 'call', fn: 'ApplyDirectDrawDisplayMode',
            w: args[0].toInt32(), h: args[1].toInt32(), bpp: args[2].toInt32()}); });

    // ConfigureDirectDrawCooperativeLevel(hWnd, mode)
    hookFn(m2dx, 0x78a0, 'ConfigureDirectDrawCooperativeLevel',
        function (args) { send({kind: 'call', fn: 'ConfigureCoopLevel',
            hWnd: args[0].toString(), mode: args[1].toInt32()}); });

    // InitializeD3DDriverAndMode(createCb, releaseCb)
    hookFn(m2dx, 0x34d0, 'InitializeD3DDriverAndMode',
        function (args) { send({kind: 'call', fn: 'InitializeD3DDriverAndMode'}); },
        function (retval) { send({kind: 'ret', fn: 'InitializeD3DDriverAndMode', val: retval.toInt32()}); });

    // CreateDDSurface(desc, outSurface, outer)
    hookFn(m2dx, 0x8710, 'CreateDDSurface',
        function (args) {
            var desc = args[0];
            var info = {kind: 'call', fn: 'CreateDDSurface'};
            try {
                info.dwSize   = Memory.readU32(desc.add(0));
                info.dwFlags  = Memory.readU32(desc.add(4));
                info.dwHeight = Memory.readU32(desc.add(8));
                info.dwWidth  = Memory.readU32(desc.add(12));
                info.ddsCaps1 = Memory.readU32(desc.add(0x68));
                info.pfFlags  = Memory.readU32(desc.add(0x50));
                info.pfBpp    = Memory.readU32(desc.add(0x58));
            } catch (e) { info.err = e.toString(); }
            send(info);
        });

    return true;
}

function installExceptionHandler() {
    Process.setExceptionHandler(function (details) {
        var info = {
            kind: 'exception',
            type: details.type,
            address: details.address.toString(),
            context: {
                eip: details.context.eip ? details.context.eip.toString() : null,
                esp: details.context.esp ? details.context.esp.toString() : null,
            }
        };
        if (details.memory) {
            info.memory = {
                op: details.memory.operation,
                addr: details.memory.address.toString(),
            };
        }
        var mod = Process.findModuleByAddress(details.address);
        if (mod) {
            info.module = mod.name;
            info.rva = details.address.sub(mod.base).toString();
        }
        // Log nearby return addresses from the stack for a crude backtrace
        try {
            var bt = Thread.backtrace(details.context, Backtracer.ACCURATE)
                     .map(DebugSymbol.fromAddress).map(function (s) { return s.toString(); });
            info.backtrace = bt;
        } catch (e) { info.bt_err = e.toString(); }
        send(info);
        return false;  // let the process crash normally
    });
}

installExceptionHandler();

// M2DX.dll is already loaded by the time Frida resumes, but in case of
// early-attach we install hooks on LoadLibraryA too.
if (!installHooks()) {
    var kernel = Process.findModuleByName('kernel32.dll');
    if (kernel) {
        var pLoadLibraryA = Module.findExportByName('kernel32.dll', 'LoadLibraryA');
        Interceptor.attach(pLoadLibraryA, {
            onLeave: function (retval) {
                if (!m2dx) { installHooks(); }
            }
        });
    }
}

send({kind: 'ready'});
"""


def main():
    if not os.path.exists(GAME_EXE):
        print(f"ERROR: {GAME_EXE} not found")
        return 1

    log = open(LOG_PATH, "w", encoding="utf-8")
    def w(s):
        print(s)
        log.write(s + "\n")
        log.flush()

    import subprocess, re as _re
    def find_pid():
        try:
            out = subprocess.check_output(
                ["tasklist", "/FI", "IMAGENAME eq TD5_d3d.exe", "/FO", "CSV", "/NH"],
                text=True, errors="ignore"
            )
            m = _re.search(r'"TD5_d3d\.exe","(\d+)"', out)
            return int(m.group(1)) if m else None
        except Exception:
            return None

    w("[+] Waiting for TD5_d3d.exe (launch it manually)...")
    pid = None
    deadline = time.time() + 120
    while time.time() < deadline:
        pid = find_pid()
        if pid:
            break
        time.sleep(0.5)
    if not pid:
        w("[-] Timed out waiting for TD5_d3d.exe")
        return 1
    w(f"[+] Found TD5_d3d.exe pid={pid}, attaching")
    session = frida.attach(pid)
    script = session.create_script(SCRIPT)

    def on_message(msg, data):
        if msg["type"] == "send":
            p = msg["payload"]
            w(f"[trace] {p}")
        elif msg["type"] == "error":
            w(f"[error] {msg.get('description','')}")
            w(f"[stack] {msg.get('stack','')}")

    script.on("message", on_message)
    script.load()
    w(f"[+] Attached to pid={pid}. Navigate to race and reproduce crash. Ctrl+C to stop.")

    try:
        while True:
            try:
                # Detect when process exits
                frida.get_device_manager().enumerate_devices()
                time.sleep(1)
                try:
                    os.kill(pid, 0)
                except (OSError, ProcessLookupError):
                    w("[+] Target process has exited.")
                    break
            except KeyboardInterrupt:
                w("[+] Interrupted by user.")
                break
    finally:
        try: session.detach()
        except Exception: pass
        log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
