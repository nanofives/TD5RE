/* frida_main_menu_hook.js
 * Captures every frontend rendering primitive that fires during Screen_MainMenu.
 * Frame boundary = onLeave of FlushFrontendSpriteBlits (0x425540).
 *
 * Defensive: every onEnter wraps args in try/catch; reads stay numeric.
 * String contents are read at IDLE (after first sample) on best-effort basis.
 */
'use strict';

var BASE = Process.findModuleByName('TD5_d3d.exe').base;
function va(v) { return BASE.add(v - 0x400000); }

var FRAME = 0;
var SCREEN_NAME = '?';
var SCREEN_IDX  = -1;

/* per-frame counters */
var COUNTS = {};
function bump(n) { COUNTS[n] = (COUNTS[n] || 0) + 1; }

/* per-call sample cap — keep first N invocations of each primitive per frame */
var SAMPLE_CAP = 8;
var SAMPLES_THIS_FRAME = {};

function sample(name, payload) {
    bump(name);
    var n = (SAMPLES_THIS_FRAME[name] || 0) + 1;
    SAMPLES_THIS_FRAME[name] = n;
    if (n > SAMPLE_CAP) return;
    payload.kind = 'call';
    payload.name = name;
    payload.frame = FRAME;
    payload.screen = SCREEN_NAME;
    send(payload);
}

function safeI32(p) { try { return p.toInt32(); } catch(e) { return 0; } }
function safeStr(p)  {
    try {
        var v = p.toUInt32();
        if (v === 0 || v < 0x10000 || v > 0x7fffffff) return null;
        return p.readCString(80);
    } catch(e) { return null; }
}

/* ---- Screen tracking ---- */
function attachScreen(idx, addr, name) {
    try {
        Interceptor.attach(va(addr), {
            onEnter: function() {
                SCREEN_NAME = name;
                SCREEN_IDX  = idx;
            }
        });
    } catch(e) { send({kind:'err', where:'screen ' + name, e:''+e}); }
}
attachScreen(28, 0x415370, 'StartupInit');
attachScreen( 0, 0x4269D0, 'LocalizationInit');
attachScreen( 4, 0x4274A0, 'LegalCopyright');
attachScreen( 3, 0x427290, 'LanguageSelect');
attachScreen( 5, 0x415490, 'MainMenu');
attachScreen( 2, 0x4275A0, 'AttractDemo');

/* ---- Frame boundary: FlushFrontendSpriteBlits ---- */
try {
    Interceptor.attach(va(0x425540), {
        onEnter: function()  { bump('FlushFrontendSpriteBlits'); },
        onLeave: function() {
            send({
                kind: 'frame',
                frame: FRAME,
                screen: SCREEN_NAME,
                screenIdx: SCREEN_IDX,
                counts: COUNTS,
            });
            COUNTS = {};
            SAMPLES_THIS_FRAME = {};
            FRAME++;
        }
    });
} catch(e) { send({kind:'err', where:'flush', e:''+e}); }

/* ---- Generic safe wrapper for primitives ---- */
function attachCounter(addr, name) {
    try {
        Interceptor.attach(va(addr), {
            onEnter: function() { sample(name, {}); }
        });
    } catch(e) { send({kind:'err', where:name, e:''+e}); }
}

function attachWithArgs(addr, name, captureFn) {
    try {
        Interceptor.attach(va(addr), {
            onEnter: function(args) {
                var payload = {};
                try { captureFn(args, payload); }
                catch(e) { payload._argerr = '' + e; }
                sample(name, payload);
            }
        });
    } catch(e) { send({kind:'err', where:name, e:''+e}); }
}

/* Surface fills — __cdecl: (color, x, y, w, h[, surf]) */
attachWithArgs(0x423DB0, 'ClearBackbufferWithColor', function(a, p) {
    p.color = safeI32(a[0]);
});
attachWithArgs(0x423ED0, 'FillPrimaryFrontendRect', function(a, p) {
    p.color = safeI32(a[0]); p.x=safeI32(a[1]); p.y=safeI32(a[2]);
    p.w=safeI32(a[3]); p.h=safeI32(a[4]);
});
attachWithArgs(0x423F90, 'FillSurfaceRectWithColor', function(a, p) {
    p.color = safeI32(a[0]); p.x=safeI32(a[1]); p.y=safeI32(a[2]);
    p.w=safeI32(a[3]); p.h=safeI32(a[4]);
});

/* String draws — __cdecl: (x, y, str[, surf, color]) */
attachWithArgs(0x424110, 'DrawFrontendFontStringPrimary', function(a, p) {
    p.x = safeI32(a[0]); p.y = safeI32(a[1]); p.str = safeStr(a[2]);
});
attachWithArgs(0x4242B0, 'DrawFrontendLocalizedStringPrimary', function(a, p) {
    p.x = safeI32(a[0]); p.y = safeI32(a[1]); p.str = safeStr(a[2]);
});
attachWithArgs(0x424470, 'DrawFrontendFontStringToSurface', function(a, p) {
    p.x = safeI32(a[0]); p.y = safeI32(a[1]); p.surf=a[2].toString(); p.str=safeStr(a[3]);
});
attachWithArgs(0x424560, 'DrawFrontendLocalizedStringToSurface', function(a, p) {
    p.x = safeI32(a[0]); p.y = safeI32(a[1]); p.surf=a[2].toString(); p.str=safeStr(a[3]);
});
attachWithArgs(0x424660, 'DrawFrontendSmallFontStringToSurface', function(a, p) {
    p.x = safeI32(a[0]); p.y = safeI32(a[1]); p.surf=a[2].toString(); p.str=safeStr(a[3]);
});
attachWithArgs(0x412D50, 'MeasureOrDrawFrontendFontString', function(a, p) {
    p.surf=a[0].toString(); p.x=safeI32(a[1]); p.y=safeI32(a[2]); p.str=safeStr(a[3]);
});

/* Presentation */
attachCounter(0x424AF0, 'PresentPrimaryFrontendBufferViaCopy');
attachCounter(0x424CA0, 'PresentPrimaryFrontendBuffer');
attachWithArgs(0x424D40, 'PresentPrimaryFrontendRect', function(a, p) {
    p.x=safeI32(a[0]); p.y=safeI32(a[1]); p.w=safeI32(a[2]); p.h=safeI32(a[3]);
});
attachCounter(0x424B30, 'CopyPrimaryFrontendBufferToSecondary');
attachCounter(0x4254D0, 'ResetFrontendOverlayState');

/* Sprite/overlay queue — __cdecl: (dx, dy, dx2, dy2, sx, sy, sx2, sy2, color, surf) */
attachWithArgs(0x425730, 'QueueFrontendSpriteBlit', function(a, p) {
    p.dx=safeI32(a[0]); p.dy=safeI32(a[1]); p.dx2=safeI32(a[2]); p.dy2=safeI32(a[3]);
    p.sx=safeI32(a[4]); p.sy=safeI32(a[5]); p.sx2=safeI32(a[6]); p.sy2=safeI32(a[7]);
    p.color=safeI32(a[8]); p.surf=a[9].toString();
});
attachWithArgs(0x425660, 'QueueFrontendOverlayRect', function(a, p) {
    p.dx=safeI32(a[0]); p.dy=safeI32(a[1]); p.dx2=safeI32(a[2]); p.dy2=safeI32(a[3]);
    p.sx=safeI32(a[4]); p.sy=safeI32(a[5]); p.sx2=safeI32(a[6]); p.sy2=safeI32(a[7]);
    p.color=safeI32(a[8]); p.surf=a[9].toString();
});

/* Buttons */
attachWithArgs(0x4258F0, 'CreateFrontendMenuRectEntry', function(a, p) {
    p.dx=safeI32(a[0]); p.dy=safeI32(a[1]); p.dx2=safeI32(a[2]); p.dy2=safeI32(a[3]);
});
attachWithArgs(0x425DE0, 'CreateFrontendDisplayModeButton', function(a, p) {
    p.w=safeI32(a[0]); p.h=safeI32(a[1]); p.str=safeStr(a[2]);
});
attachCounter(0x4260E0, 'CreateFrontendDisplayModePreviewButton');
attachCounter(0x426260, 'InitializeFrontendDisplayModeArrows');
attachCounter(0x426390, 'ReleaseFrontendDisplayModeButtons');
attachCounter(0x425B60, 'DrawFrontendButtonBackground');
attachCounter(0x426580, 'UpdateFrontendDisplayModeSelection');
attachWithArgs(0x412E30, 'CreateMenuStringLabelSurface', function(a, p) {
    p.str=safeStr(a[0]); p.color=safeI32(a[1]);
});

send({kind: 'ready', base: BASE.toString()});
