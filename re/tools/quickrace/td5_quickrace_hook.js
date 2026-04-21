// td5_quickrace_hook.js
// Frida hook that turns the original TD5_d3d.exe into a quick-race launcher.
// Replaces ScreenMainMenuAnd1PRaceFlow with a one-shot that writes race params
// and calls the same functions the attract-mode demo uses to enter a race:
//   ConfigureGameTypeFlags(); InitializeRaceSeriesSchedule(); InitializeFrontendDisplayModeState();
//
// Config is injected by the Python launcher via script.post({cfg: ...}).

'use strict';

// Addresses are image-base-relative (TD5_d3d.exe imagebase 0x00400000).
var RVA = {
    ScreenMainMenuAnd1PRaceFlow:           0x00015490,
    SetFrontendScreen:                     0x00014610,
    ConfigureGameTypeFlags:                0x00010ca0,
    InitializeRaceSeriesSchedule:          0x0000dac0,
    InitializeFrontendDisplayModeState:    0x00014a90,
    ShowLegalScreens:                      0x0002c8e0,
    PlayIntroMovie:                        0x0003c440,
    g_currentScreenFnPtr:                  0x00095238,  // live frontend screen fn ptr

    g_selectedGameType:                    0x0009635c,
    g_selectedScheduleIndex:               0x000a2c90,
    g_selectedTrackDirection:              0x000a2c98,
    g_selectedCarIndex:                    0x0008f364,
    g_p1PaintIndex:                        0x0008f368,   // DAT_0048f368
    g_p1SlotPosition:                      0x0008f370,   // DAT_0048f370
    g_p1Transmission:                      0x0008f378,   // DAT_0048f378
    g_transmissionShadow:                  0x0008f338,   // DAT_0048f338
    g_browseCarIndex:                      0x0008f31c,   // DAT_0048f31c
    gCircuitLapCount:                      0x00066e8c,
    g_twoPlayerModeEnabled:                0x000962a0,
    g_networkSessionActive:                0x00097324,
    g_returnToScreenIndex:                 0x00096350,
    g_dragOpponentCarIndex:                0x00063e08,   // DAT_00463e08 — drag-race opponent

    // Live-hook sites (used when start_span_offset != 0 or trace_track_load)
    InitializeActorTrackPose:              0x00034350,   // (slot, span, lane, flag) __cdecl
    LoadTrackRuntimeData:                  0x0002fb90,   // (levelZipNumber)          __cdecl
};

var cfg = {
    game_type: 0,
    track: 0,
    direction: 0,
    laps: 3,
    car: 0,
    paint: 0,
    transmission: 0,
    start_span_offset: 0,
    opponent_car: 0,
    verbose: false,
    trace_track_load: false,
};

var launched = false;
var __quickrace_replacement = null;     // keep-alive for NativeCallback
var __quickrace_legalsNoop = null;      // keep-alive for ShowLegalScreens no-op
var __quickrace_introNoop = null;       // keep-alive for PlayIntroMovie no-op
var __quickrace_setScreenHook = null;   // keep-alive for Interceptor.attach
var __quickrace_spawnHook = null;       // keep-alive for InitializeActorTrackPose
var __quickrace_loadTrackHook = null;   // keep-alive for LoadTrackRuntimeData

function log(msg) {
    send({kind: 'log', msg: msg});
}

function vlog(msg) {
    if (cfg.verbose) send({kind: 'log', msg: '[v] ' + msg});
}

function install() {
    var mod = Process.findModuleByName('TD5_d3d.exe');
    if (!mod) {
        log('TD5_d3d.exe module not yet loaded, retrying...');
        setTimeout(install, 50);
        return;
    }

    var base = mod.base;
    log('TD5_d3d.exe base = ' + base);

    var addrOf = {};
    for (var k in RVA) addrOf[k] = base.add(RVA[k]);

    // NativeFunctions for the three game calls.
    var ConfigureGameTypeFlags = new NativeFunction(
        addrOf.ConfigureGameTypeFlags, 'int', [], 'stdcall');
    var InitializeRaceSeriesSchedule = new NativeFunction(
        addrOf.InitializeRaceSeriesSchedule, 'void', [], 'stdcall');
    var InitializeFrontendDisplayModeState = new NativeFunction(
        addrOf.InitializeFrontendDisplayModeState, 'void', [], 'stdcall');

    function wi(name, val) {
        addrOf[name].writeInt(val);
        vlog('wrote ' + name + '@' + addrOf[name] + ' = ' + val);
    }

    // Replace ScreenMainMenuAnd1PRaceFlow with a one-shot race launcher.
    // This function is the main menu screen fn pointer — it starts being
    // ticked right after ScreenLocalizationInit sets up car ZIPs / config.td5.
    // On the first tick we stamp the race globals and call the same three
    // functions the attract-mode demo path uses to transition into a race.
    var replacement = new NativeCallback(function () {
        if (launched) return;
        launched = true;
        log('quickrace: launching race ' +
            'type=' + cfg.game_type +
            ' track=' + cfg.track +
            ' car=' + cfg.car +
            ' laps=' + cfg.laps);

        wi('g_networkSessionActive', 0);
        wi('g_twoPlayerModeEnabled', 0);

        wi('g_selectedGameType', cfg.game_type);
        wi('g_selectedScheduleIndex', cfg.track);
        wi('g_selectedTrackDirection', cfg.direction);

        wi('g_selectedCarIndex', cfg.car);
        wi('g_browseCarIndex', cfg.car);
        wi('g_p1PaintIndex', cfg.paint);
        wi('g_p1SlotPosition', 0);
        wi('g_p1Transmission', cfg.transmission);
        wi('g_transmissionShadow', cfg.transmission);

        wi('gCircuitLapCount', cfg.laps);

        wi('g_returnToScreenIndex', -1);

        // Drag-race opponent — only meaningful when the game type picks up
        // the drag-race init path. Written unconditionally because the field
        // is also the 2P P2-car slot and zeroing/refreshing it on race entry
        // matches the vanilla frontend snapshot behavior.
        if (cfg.game_type === 9) {
            wi('g_dragOpponentCarIndex', cfg.opponent_car);
        }

        vlog('calling ConfigureGameTypeFlags()');
        ConfigureGameTypeFlags();
        vlog('calling InitializeRaceSeriesSchedule()');
        InitializeRaceSeriesSchedule();
        vlog('calling InitializeFrontendDisplayModeState()');
        InitializeFrontendDisplayModeState();
        log('quickrace: race launch sequence complete');
    }, 'void', [], 'stdcall');

    // Keep a strong reference to the callback so Frida doesn't GC it.
    __quickrace_replacement = replacement;
    Interceptor.replace(addrOf.ScreenMainMenuAnd1PRaceFlow, replacement);
    log('quickrace: hook installed on ScreenMainMenuAnd1PRaceFlow @ ' +
        addrOf.ScreenMainMenuAnd1PRaceFlow);

    // RunMainGameLoop's GAMESTATE_INTRO branch unconditionally calls
    // ShowLegalScreens() before transitioning to GAMESTATE_MENU, and plays
    // the intro FMV when g_introMoviePendingFlag is set. Replace both with
    // no-ops so the game flips straight to the menu (and therefore our
    // main-menu replacement) on the first intro tick.
    __quickrace_legalsNoop = new NativeCallback(function () {
        vlog('ShowLegalScreens() no-op');
    }, 'void', [], 'stdcall');
    Interceptor.replace(addrOf.ShowLegalScreens, __quickrace_legalsNoop);

    __quickrace_introNoop = new NativeCallback(function () {
        vlog('PlayIntroMovie() no-op');
        return 0;
    }, 'int', [], 'stdcall');
    Interceptor.replace(addrOf.PlayIntroMovie, __quickrace_introNoop);
    log('quickrace: ShowLegalScreens + PlayIntroMovie replaced with no-ops');

    // Skip the first-run language select (3) and legal splash (4) by
    // rewriting any SetFrontendScreen(3|4) call into SetFrontendScreen(5).
    // Screen 5 is ScreenMainMenuAnd1PRaceFlow which we've replaced above,
    // so the race launch fires on its very first tick.
    __quickrace_setScreenHook = Interceptor.attach(addrOf.SetFrontendScreen, {
        onEnter: function (args) {
            var idx = args[0].toInt32();
            if (idx === 3 || idx === 4) {
                vlog('SetFrontendScreen(' + idx + ') -> rewriting to 5');
                args[0] = ptr(5);
            }
        }
    });
    log('quickrace: SetFrontendScreen filter installed');

    // SetFrontendScreen() copies g_frontendScreenFnTable[idx] into the live
    // pointer g_currentScreenFnPtr at call time, so rewriting the table does
    // nothing once the dispatcher has already latched on to a screen. Patch
    // the live pointer directly so the next frame runs our race launcher no
    // matter which screen the game is currently sitting on (language, legal,
    // main-menu attract-mode timer, etc.).
    addrOf.g_currentScreenFnPtr.writePointer(addrOf.ScreenMainMenuAnd1PRaceFlow);
    log('quickrace: g_currentScreenFnPtr forced to main-menu replacement');

    // InitializeActorTrackPose(slot, span, lane, flag) — __cdecl, args on stack.
    // When start_span_offset != 0, rewrite the span arg so every actor spawns
    // further down the span ring (offsets are additive; the -3/-6/-9/-12/-15
    // grid spacing is preserved). The function rebuilds heading, route
    // progress, lateral offset, and physics state entirely from the new span,
    // so no further state has to be patched.
    // Also used as the hook site for trace_track_load's per-spawn logging.
    if (cfg.start_span_offset !== 0 || cfg.trace_track_load) {
        __quickrace_spawnHook = Interceptor.attach(addrOf.InitializeActorTrackPose, {
            onEnter: function (args) {
                var esp = this.context.esp;
                var slot = esp.add(4).readU32();
                var span = esp.add(8).readS16();
                if (cfg.start_span_offset !== 0) {
                    var newSpan = (span + cfg.start_span_offset) & 0xffff;
                    esp.add(8).writeS16(newSpan);
                    if (cfg.trace_track_load) {
                        log('pose slot=' + slot + ': span ' + span + ' -> ' + newSpan);
                    }
                } else if (cfg.trace_track_load) {
                    log('pose slot=' + slot + ': span=' + span);
                }
            }
        });
        log('quickrace: spawn hook installed (start_span_offset=' +
            cfg.start_span_offset + ')');
    }

    // LoadTrackRuntimeData(levelZipNumber) — logs which level%03d.zip the
    // race actually resolved to. Key signal for confirming track=-1 goes to
    // drag strip (level030) vs any surprises from the schedule table.
    if (cfg.trace_track_load) {
        __quickrace_loadTrackHook = Interceptor.attach(addrOf.LoadTrackRuntimeData, {
            onEnter: function (args) {
                var n = this.context.esp.add(4).readU32();
                var pad = ('000' + n).slice(-3);
                log('LoadTrackRuntimeData(' + n + ') -> level' + pad + '.zip');
            }
        });
        log('quickrace: track-load trace hook installed');
    }
}

recv('cfg', function (msg) {
    Object.keys(msg.cfg).forEach(function (k) { cfg[k] = msg.cfg[k]; });
    log('quickrace: cfg received ' + JSON.stringify(cfg));
    install();
});

log('quickrace: script loaded, waiting for cfg');
