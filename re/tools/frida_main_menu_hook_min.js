'use strict';
var BASE = Process.findModuleByName('TD5_d3d.exe').base;
function va(v) { return BASE.add(v - 0x400000); }

var FRAME = 0;

try {
    Interceptor.attach(va(0x425540), {  /* FlushFrontendSpriteBlits */
        onLeave: function() {
            FRAME++;
            if (FRAME % 30 === 0) {
                send({kind:'frame', n: FRAME});
            }
        }
    });
} catch(e) { send({kind:'err', where:'flush', e: '' + e}); }

var SCREENS = [
    [0, 0x4269D0, 'LocalizationInit'],
    [4, 0x4274A0, 'LegalCopyright'],
    [5, 0x415490, 'MainMenu'],
    [28, 0x415370, 'StartupInit'],
    [2, 0x4275A0, 'AttractDemo'],
];
SCREENS.forEach(function(s) {
    try {
        Interceptor.attach(va(s[1]), {
            onEnter: function() {
                if (!this._fired) {
                    send({kind:'screen_first_call', idx:s[0], name:s[2], frame:FRAME});
                    this._fired = true;
                }
            }
        });
    } catch(e) { send({kind:'err', where:'screen ' + s[2], e:''+e}); }
});

/* Also hook a primitive that should fire every frame */
try {
    Interceptor.attach(va(0x424CA0), { /* PresentPrimaryFrontendBuffer */
        onEnter: function() {
            if (!this._counted) this._counted = true;
            if (FRAME % 30 === 0) send({kind:'present', frame:FRAME});
        }
    });
} catch(e) { send({kind:'err', where:'present', e:''+e}); }

send({kind:'ready'});
