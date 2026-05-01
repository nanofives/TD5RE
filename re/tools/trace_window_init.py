#!/usr/bin/env python3
"""Spawn TD5_d3d.exe under Frida and trace every call that affects window
size/style/visibility and every DirectDraw display-mode transition.

Goal: identify where the original game switches from a 640x480 window to
covering the whole screen, despite M2DX + EXE patches already bypassing
the documented fullscreen paths.

Logs land at log/trace_window_init.log.
"""
from __future__ import annotations
import os
import sys
import time
from pathlib import Path

import frida

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
EXE = REPO / "original" / "TD5_d3d.exe"
LOG = REPO / "log" / "trace_window_init.log"

HOOK = r"""
const log = (msg) => send({type: 'log', msg: msg});

function hexp(p) { return p === null || p.isNull() ? '0' : p.toString(); }

// ---- user32 -------------------------------------------------------------
const user32 = Process.getModuleByName('user32.dll');

function hookCreateWindowEx(name) {
    const fn = user32.findExportByName(name);
    if (!fn) { log('MISS ' + name); return; }
    Interceptor.attach(fn, {
        onEnter(args) {
            // lpClassName, lpWindowName, dwStyle, X, Y, nWidth, nHeight
            const cls  = args[1].isNull() ? '(null)' : (name.endsWith('W') ? args[1].readUtf16String() : args[1].readAnsiString());
            const ttl  = args[2].isNull() ? '(null)' : (name.endsWith('W') ? args[2].readUtf16String() : args[2].readAnsiString());
            const sty  = args[3].toUInt32().toString(16);
            const exs  = args[0].toUInt32().toString(16);
            const x    = args[4].toInt32(), y = args[5].toInt32();
            const w    = args[6].toInt32(), h = args[7].toInt32();
            log(`${name}: cls="${cls}" title="${ttl}" style=0x${sty} exstyle=0x${exs} pos=(${x},${y}) size=${w}x${h}`);
        },
        onLeave(retval) { log(`  -> hWnd=${hexp(retval)}`); }
    });
}
hookCreateWindowEx('CreateWindowExA');
hookCreateWindowEx('CreateWindowExW');

const hookSimple = (mod, name, fmt) => {
    const fn = mod.findExportByName(name);
    if (!fn) { log('MISS ' + name); return; }
    Interceptor.attach(fn, {
        onEnter(args) { log(name + ': ' + fmt(args)); }
    });
};
hookSimple(user32, 'ShowWindow',         a => `hWnd=${hexp(a[0])} nCmdShow=${a[1].toInt32()}`);

// SetWindowPos with backtrace when it's a real resize (not SWP_NOSIZE)
const swp = user32.findExportByName('SetWindowPos');
if (swp) {
    Interceptor.attach(swp, {
        onEnter(args) {
            const flags = args[6].toUInt32();
            const w = args[4].toInt32(), h = args[5].toInt32();
            const info = `SetWindowPos: hWnd=${hexp(args[0])} hInsertAfter=${hexp(args[1])} pos=(${args[2].toInt32()},${args[3].toInt32()}) size=${w}x${h} flags=0x${flags.toString(16)}`;
            // Backtrace only for real size changes (no SWP_NOSIZE=0x1) where size is suspicious
            if ((flags & 0x1) === 0 && (w > 800 || h > 600)) {
                const bt = Thread.backtrace(this.context, Backtracer.ACCURATE)
                    .map(DebugSymbol.fromAddress)
                    .slice(0, 10)
                    .map(s => `    ${s.address} ${s.moduleName || '?'}!${s.name || '?'}`)
                    .join('\n');
                log(info + '\n  BACKTRACE (large resize):\n' + bt);
            } else {
                log(info);
            }
        }
    });
}
hookSimple(user32, 'MoveWindow',         a => `hWnd=${hexp(a[0])} pos=(${a[1].toInt32()},${a[2].toInt32()}) size=${a[3].toInt32()}x${a[4].toInt32()} repaint=${a[5].toInt32()}`);
hookSimple(user32, 'SetWindowLongA',     a => `hWnd=${hexp(a[0])} nIndex=${a[1].toInt32()} newVal=0x${a[2].toUInt32().toString(16)}`);
hookSimple(user32, 'SetWindowLongW',     a => `hWnd=${hexp(a[0])} nIndex=${a[1].toInt32()} newVal=0x${a[2].toUInt32().toString(16)}`);
hookSimple(user32, 'AdjustWindowRect',   a => `rect=${hexp(a[0])} style=0x${a[1].toUInt32().toString(16)} menu=${a[2].toInt32()}`);
hookSimple(user32, 'AdjustWindowRectEx', a => `rect=${hexp(a[0])} style=0x${a[1].toUInt32().toString(16)} menu=${a[2].toInt32()} exstyle=0x${a[3].toUInt32().toString(16)}`);

// ChangeDisplaySettings — dumps the DEVMODE width/height if the lpDevMode
// pointer is non-null.
const cds = user32.findExportByName('ChangeDisplaySettingsA');
if (cds) {
    Interceptor.attach(cds, {
        onEnter(args) {
            let info = `ChangeDisplaySettingsA: flags=0x${args[1].toUInt32().toString(16)}`;
            if (!args[0].isNull()) {
                // DEVMODE: dmPelsWidth @ +0xb0 (DEVMODEA) for NT, +0xa8 for 95/98... use 0xb0
                try {
                    const w = args[0].add(0xb0).readU32();
                    const h = args[0].add(0xb4).readU32();
                    const bpp = args[0].add(0xa8).readU32();
                    info += ` dmPelsWidth=${w} dmPelsHeight=${h} dmBitsPerPel=${bpp}`;
                } catch (e) { info += ' (devmode read failed: ' + e.message + ')'; }
            } else {
                info += ' lpDevMode=NULL (restore default)';
            }
            log(info);
        }
    });
}

const cdsEx = user32.findExportByName('ChangeDisplaySettingsExA');
if (cdsEx) {
    Interceptor.attach(cdsEx, {
        onEnter(args) {
            const dev = args[0].isNull() ? '(default)' : args[0].readAnsiString();
            let info = `ChangeDisplaySettingsExA: device="${dev}" flags=0x${args[2].toUInt32().toString(16)}`;
            if (!args[1].isNull()) {
                try {
                    const w = args[1].add(0xb0).readU32();
                    const h = args[1].add(0xb4).readU32();
                    info += ` dmPelsWidth=${w} dmPelsHeight=${h}`;
                } catch (e) {}
            } else {
                info += ' lpDevMode=NULL';
            }
            log(info);
        }
    });
}

// ---- M2DX.dll — key internal functions documented in Ghidra -------------
function hookM2DX() {
    const m = Process.findModuleByName('M2DX.dll');
    if (!m) { log('M2DX.dll not loaded yet'); return false; }
    log(`M2DX.dll base=${m.base}`);
    const base = m.base;

    const hookM = (rva, name, dumper) => {
        Interceptor.attach(base.add(rva), {
            onEnter(args) {
                this.args = args;
                log(`M2DX!${name}+0x0: ENTER${dumper ? ' ' + dumper(args) : ''}`);
            },
            onLeave(retval) {
                log(`M2DX!${name}: RET eax=0x${retval.toUInt32().toString(16)}`);
            }
        });
    };

    // From memory + Ghidra:
    // 0x6600 = DXDraw::Create (windowed check at +0x6637)
    // 0x78C0 = ConfigureDirectDrawCooperativeLevel (flag at +0x78CA)
    // 0x8F60 = ApplyWindowedRenderSize
    // 0x12A10 = DXWin::Initialize (CreateWindowExA at +0x12ADB)
    // 0x6600 area is __thiscall, args come via ECX
    hookM(0x12A10, 'DXWin::Initialize');
    hookM(0x78C0, 'ConfigureDirectDrawCooperativeLevel');
    hookM(0x8F60, 'ApplyWindowedRenderSize');

    // DAT_10061c1c at M2DX+0x61c1c is the "isFullscreenCooperative" flag.
    // Read it periodically to see when it flips.
    const flagPtr = base.add(0x61c1c);
    log(`DAT_10061c1c init value = 0x${flagPtr.readU32().toString(16)}`);
    return true;
}

// ---- ddraw.dll — hook SetCooperativeLevel + SetDisplayMode via vtable ---
// DirectDrawCreate returns IDirectDraw*. Vtable method 20 = SetCooperativeLevel,
// method 21 = SetDisplayMode. Hook them the first time we see the vtable.
const ddraw = Process.findModuleByName('ddraw.dll');
if (ddraw) {
    const hookedVTs = new Set();
    const hookDDVtable = (pDD) => {
        if (pDD.isNull()) return;
        const vt = pDD.readPointer();
        const vtKey = vt.toString();
        if (hookedVTs.has(vtKey)) return;
        hookedVTs.add(vtKey);
        log(`  hooking new vtable=${vt}`);
        // SetCooperativeLevel = slot 20, SetDisplayMode = slot 21 for IDirectDraw/2
        // For IDirectDraw4/7 these are slot 20, 21 too (same inheritance order).
        try {
            const setCoopLevel = vt.add(20 * 4).readPointer();
            Interceptor.attach(setCoopLevel, {
                onEnter(a) {
                    const flags = a[2].toUInt32();
                    const names = [];
                    if (flags & 0x001) names.push('FULLSCREEN');
                    if (flags & 0x004) names.push('NOWINDOWCHANGES');
                    if (flags & 0x008) names.push('NORMAL');
                    if (flags & 0x010) names.push('EXCLUSIVE');
                    if (flags & 0x020) names.push('ALLOWMODEX');
                    if (flags & 0x040) names.push('SETFOCUSWINDOW');
                    if (flags & 0x080) names.push('SETDEVICEWINDOW');
                    if (flags & 0x100) names.push('CREATEDEVICEWINDOW');
                    if (flags & 0x200) names.push('MULTITHREADED');
                    const bt = Thread.backtrace(this.context, Backtracer.ACCURATE)
                        .map(DebugSymbol.fromAddress)
                        .slice(0, 6)
                        .map(s => `    ${s.address} ${s.moduleName || '?'}!${s.name || '?'}`)
                        .join('\n');
                    log(`IDD::SetCooperativeLevel: hWnd=${hexp(a[1])} flags=0x${flags.toString(16)} [${names.join('|')}]\n${bt}`);
                }
            });
            const setDispMode = vt.add(21 * 4).readPointer();
            Interceptor.attach(setDispMode, {
                onEnter(a) { log(`IDD::SetDisplayMode: ${a[1].toInt32()}x${a[2].toInt32()} bpp=${a[3].toInt32()}`); }
            });
        } catch (e) { log('vtable hook err: ' + e.message); }
    };
    const fn = ddraw.findExportByName('DirectDrawCreate');
    if (fn) {
        Interceptor.attach(fn, {
            onEnter(args) { this.ppDD = args[1]; },
            onLeave(retval) {
                if (retval.toInt32() === 0 && !this.ppDD.isNull()) hookDDVtable(this.ppDD.readPointer());
            }
        });
    }
    // Also hook QueryInterface results — every IDirectDraw2/4/7 goes through it.
    // We hook by patching the QI slot (0) of every vtable we learn about and
    // re-hooking the returned object's vtable.
    // Simpler: attach to the very first hooked object's QueryInterface only;
    // that's enough for the game's own upgrade path.
}

// Hook M2DX once loaded (it's not in the import table, loaded dynamically).
const loadLibraryA = Module.findExportByName('kernel32.dll', 'LoadLibraryA');
Interceptor.attach(loadLibraryA, {
    onEnter(args) { this.name = args[0].isNull() ? '' : args[0].readAnsiString(); },
    onLeave(retval) {
        if (this.name && this.name.toLowerCase().includes('m2dx')) {
            log(`LoadLibraryA("${this.name}") -> ${retval}`);
            setImmediate(() => { hookM2DX(); });
        }
    }
});

// Also try at startup in case M2DX is statically linked.
if (hookM2DX()) {
    log('M2DX hooks installed at startup');
}

log('=== hooks installed, waiting for calls ===');
"""

def main() -> int:
    if not EXE.exists():
        print(f"ERROR: {EXE} not found", file=sys.stderr)
        return 1
    LOG.parent.mkdir(parents=True, exist_ok=True)
    with LOG.open('w', encoding='utf-8', buffering=1) as out:
        print(f"Logging to {LOG}")
        def on_message(msg, data):
            if msg['type'] == 'send':
                payload = msg['payload']
                if payload.get('type') == 'log':
                    line = payload['msg']
                    out.write(line + '\n')
                    print(line)
            elif msg['type'] == 'error':
                err = f"[error] {msg.get('description', msg)}"
                out.write(err + '\n')
                print(err)

        pid = frida.spawn(str(EXE), cwd=str(EXE.parent))
        session = frida.attach(pid)
        script = session.create_script(HOOK)
        script.on('message', on_message)
        script.load()
        frida.resume(pid)
        print(f"Spawned TD5_d3d.exe pid={pid}, tracing... Ctrl+C to stop.")
        try:
            for _ in range(30):
                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            try:
                frida.kill(pid)
            except Exception:
                pass
    return 0

if __name__ == '__main__':
    sys.exit(main())
