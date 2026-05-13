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

    // CRT srand — Ghidra mis-labels as __set_new_handler at 0x0044814a; verified
    // via decompile (Issue 9 /fix agent 2026-04-24). Seeding _holdrand here before
    // InitializeRaceSeriesSchedule aligns the Frida harness's CRT RNG state with
    // the port's srand(0x1A2B3C4D) seeding, enabling byte-exact AI-car selection
    // parity between the port and the Frida-captured original trace.
    _srand:                                0x0004814a,   // void __cdecl _srand(unsigned int)

    // player_is_ai: slot[0].state stays 1 (human) for race-end detection,
    // HUD, camera, and every other consumer. It is flipped to 0 ONLY for
    // the duration of UpdateRaceActors so the per-tick actor dispatch
    // takes the AI branch for slot 0, then restored to 1 on return.
    UpdateRaceActors:                      0x00036a70,
    slot0_state_byte:                      0x000AADF4,  // gRaceSlotStateTable.slot[0].state

    // Time-Trial trap: in InitializeRaceSession (0x0042aa10) the block at
    //   if (TVar16 != 0) { g_trackTextureIndex = 0x1e; g_trackPoolIndex = 0x1e; }
    // overrides the loaded track to drag-strip pool 30 whenever
    // g_selectedGameType != 0. The frontend normally sidesteps this by setting
    // g_selectedScheduleIndex < 0 for non-single-race modes; the Frida hook
    // bypasses the frontend and feeds raw values, so for cfg.game_type==7
    // (Time Trials) we flip the guard JZ to JMP so the trap is always skipped
    // and the requested track loads.
    InitializeRaceSession:                 0x0002aa10,
    slot1_state_byte:                      0x000AADF8,  // gRaceSlotStateTable.slot[1].state

    // The "TT trap" flag in InitializeRaceSession (decompiled as TVar16)
    // is *not* g_selectedGameType — Ghidra mislabeled it. The real source
    // is loaded into EBP at 0x0042abbd:  MOV EBP, [0x004aaf6c].
    // ConfigureGameTypeFlags writes it:
    //   - early gate (gt != 7):   [0x004aaf6c] = 0
    //   - case 7  (TT):           [0x004aaf6c] = 1
    // No other writers in the binary. So this flag is *uniquely* the TT bit,
    // which is why every other game_type loads Moscow correctly while gt=7
    // forces drag-pool 30. Clearing it in our hook *after* ConfigureGameTypeFlags
    // (so the TT side-effects are kept) but *before* InitializeRaceSession
    // runs lets TT load the requested track.
    g_timeTrialFlag:                       0x000AAF6C,
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
    /* player_is_ai: windows slot[0].state=0 to just the body of
     * UpdateRaceActors so the AI dispatch fires for slot 0, then
     * restores state=1 on return. Race-end detection / HUD / camera /
     * humanPlayerCount all continue to see slot 0 as the normal human,
     * so they do not mistake an AI-driven slot 0 for "all humans
     * finished" and auto-complete the race. Mirrors td5re.ini
     * [GameOptions] PlayerIsAI=1. */
    player_is_ai: false,
    /* seed_crt: if true, call _srand(crt_seed) before
     * InitializeRaceSeriesSchedule so the CRT rand() sequence used by the
     * AI-car selection loop matches the port's when race_trace_enabled=1.
     * Required for byte-exact diff of AI slot cars (Issue 9 follow-up).
     * Python launcher auto-enables this when --trace is passed. */
    seed_crt: false,
    crt_seed: 0x1A2B3C4D,
    /* frontend_screen: when >= 0, jump to this screen index instead of
     * launching a race. Race globals are NOT written. -1 = normal quickrace. */
    frontend_screen: -1,
    /* frontend_only: when true, bypass intro/legal/language screens but do NOT
     * replace ScreenMainMenuAnd1PRaceFlow — let the full menu run normally.
     * Useful for frontend render capture. Overrides frontend_screen. */
    frontend_only: false,
};

var launched = false;
var __quickrace_replacement = null;     // keep-alive for NativeCallback
var __quickrace_legalsNoop = null;      // keep-alive for ShowLegalScreens no-op
var __quickrace_introNoop = null;       // keep-alive for PlayIntroMovie no-op
var __quickrace_setScreenHook = null;   // keep-alive for Interceptor.attach
var __quickrace_spawnHook = null;       // keep-alive for InitializeActorTrackPose
var __quickrace_loadTrackHook = null;   // keep-alive for LoadTrackRuntimeData
var __quickrace_updActorsHook = null;      // keep-alive: UpdateRaceActors slot[0] window
var __quickrace_initSessionHook = null;    // keep-alive: InitializeRaceSession TT slot[1] suppress
var __quickrace_ttSpawnHook = null;        // keep-alive: TT slot 0 spawn redirect off span 0x73

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
    var SetFrontendScreen = new NativeFunction(
        addrOf.SetFrontendScreen, 'void', ['int'], 'stdcall');

    // CRT srand — seeded before InitializeRaceSeriesSchedule so the AI-car
    // selection loop pulls from the same sequence the port does under the
    // matching deterministic-seed configuration.
    var _srand = new NativeFunction(addrOf._srand, 'void', ['uint32'], 'mscdecl');

    function wi(name, val) {
        addrOf[name].writeInt(val);
        vlog('wrote ' + name + '@' + addrOf[name] + ' = ' + val);
    }

    // Save a callable pointer to the REAL ScreenMainMenuAnd1PRaceFlow before
    // we replace it, so frontend_only mode can call through to it.
    var realScreenMainMenu = new NativeFunction(
        addrOf.ScreenMainMenuAnd1PRaceFlow, 'void', [], 'stdcall');

    // Replace ScreenMainMenuAnd1PRaceFlow with a one-shot race launcher.
    // This function is the main menu screen fn pointer — it starts being
    // ticked right after ScreenLocalizationInit sets up car ZIPs / config.td5.
    // On the first tick we stamp the race globals and call the same three
    // functions the attract-mode demo path uses to transition into a race.
    var replacement = new NativeCallback(function () {
        if (launched) return;
        launched = true;

        // frontend_only: revert to the real menu function and call through.
        // cfg is correct here (recv('cfg') arrives well before the first tick).
        if (cfg.frontend_only) {
            log('quickrace: frontend_only — reverting to real ScreenMainMenuAnd1PRaceFlow');
            Interceptor.revert(addrOf.ScreenMainMenuAnd1PRaceFlow);
            realScreenMainMenu();
            return;
        }

        // Frontend debug mode: jump to a specific screen instead of a race.
        if (cfg.frontend_screen >= 0 && cfg.frontend_screen < 30) {
            log('quickrace: frontend_screen=' + cfg.frontend_screen +
                ', jumping to screen instead of launching race');
            SetFrontendScreen(cfg.frontend_screen);
            return;
        }

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

        // "Time Trials" (cfg.game_type === 7) — the engine's real TT path
        // can't be safely reached through the Frida hook (it routes through
        // the drag-pool trap + a hardcoded slot-0 spawn at span 0x73 + a
        // mode-switch latch path that initialises render globals). Instead
        // we synthesize the *behavior* of TT (player-solo, no AI rear-ends)
        // by:
        //   a) Letting the setup pair (ConfigureGameTypeFlags +
        //      InitializeRaceSeriesSchedule) see game_type=7 so the TT
        //      flags (no traffic, tier 2, preset 3) and the 2-slot
        //      schedule mask are committed.
        //   b) Flipping g_selectedGameType back to 0 *before*
        //      InitializeRaceSession runs so it takes the proven
        //      single-race spawn path — proper grid, all 6 actors
        //      InitializeActorTrackPose'd, geometry renders.
        //   c) Forcing slots 1..5 to state=3 (INACTIVE) on the way out
        //      so no AI ticks for them and slot 0 has nobody to ram into.

        vlog('calling ConfigureGameTypeFlags()');
        ConfigureGameTypeFlags();

        // Seed the CRT _holdrand with the same fixed value the port uses when
        // race_trace_enabled=1. Guarded by cfg.seed_crt so non-trace quickrace
        // runs keep their normal timeGetTime()-seeded behavior.
        if (cfg.seed_crt) {
            vlog('calling _srand(0x' + cfg.crt_seed.toString(16) + ')');
            _srand(cfg.crt_seed >>> 0);
        }

        vlog('calling InitializeRaceSeriesSchedule()');
        InitializeRaceSeriesSchedule();

        vlog('calling InitializeFrontendDisplayModeState()');
        InitializeFrontendDisplayModeState();

        // Time Trials post-setup. ConfigureGameTypeFlags has set the TT
        // flags (preset=3, tier=2, no traffic, [g_timeTrialFlag]=1) and
        // InitializeRaceSeriesSchedule has built the 2-slot table.
        // Now we:
        //   - Clear g_timeTrialFlag so InitializeRaceSession's drag-pool
        //     trap (gated on EBP=[g_timeTrialFlag]) doesn't fire — the
        //     requested track loads instead of pool 30.
        //   - Flip g_selectedGameType to 0 so the spawn block takes the
        //     proven single-race grid path, avoiding the broken hardcoded
        //     span 0x73 / lane 1 TT spawn that's only valid on drag strip.
        //   - Hook InitializeRaceSession.onLeave to force slots 1..5 to
        //     state=3 (INACTIVE) so the player runs solo (no AI rear-ends).
        if (cfg.game_type === 7) {
            wi('g_timeTrialFlag', 0);
            wi('g_selectedGameType', 0);
            __quickrace_initSessionHook = Interceptor.attach(
                addrOf.InitializeRaceSession, {
                    onLeave: function () {
                        for (var i = 0; i < 5; i++) {
                            addrOf.slot1_state_byte.add(i * 4).writeU8(3);
                        }
                        vlog('TT-synth: slots 1..5 state forced to 3 (inactive)');
                    }
                });
            log('quickrace: TT post-setup — cleared trap flag, gt->0, slots 1..5 will be INACTIVE');
        }
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
    // Always installed; cfg.start_span_offset / cfg.trace_track_load checked at
    // call time so the hook is in place before resume regardless of when cfg
    // is delivered. Fires only 6 times (one per actor spawn) so overhead is nil.
    __quickrace_spawnHook = Interceptor.attach(addrOf.InitializeActorTrackPose, {
        onEnter: function (args) {
            if (cfg.start_span_offset === 0 && !cfg.trace_track_load) return;
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
    log('quickrace: spawn hook installed (start_span_offset + trace_track_load cfg-gated at runtime)');

    // LoadTrackRuntimeData(levelZipNumber) — always installed; trace_track_load
    // checked at call time.
    __quickrace_loadTrackHook = Interceptor.attach(addrOf.LoadTrackRuntimeData, {
        onEnter: function (args) {
            if (!cfg.trace_track_load) return;
            var n = this.context.esp.add(4).readU32();
            var pad = ('000' + n).slice(-3);
            log('LoadTrackRuntimeData(' + n + ') -> level' + pad + '.zip');
        }
    });
    log('quickrace: track-load hook installed (trace_track_load cfg-gated at runtime)');

    // player_is_ai: always installed; cfg.player_is_ai checked at call time.
    // Scopes slot[0].state=0 to just the body of UpdateRaceActors so AI
    // dispatch fires for slot 0, restores state=1 on return so race-end
    // detection / HUD / camera / humanPlayerCount keep seeing slot 0 as human.
    __quickrace_updActorsHook = Interceptor.attach(addrOf.UpdateRaceActors, {
        onEnter: function () {
            if (!cfg.player_is_ai) return;
            addrOf.slot0_state_byte.writeU8(0);
        },
        onLeave: function () {
            if (!cfg.player_is_ai) return;
            addrOf.slot0_state_byte.writeU8(1);
        }
    });
    log('quickrace: UpdateRaceActors player_is_ai hook installed (cfg-gated at runtime)');
}

// =========================================================================
// Optional harness ergonomics — only installed when cfg.harness_quiet is true
// (set via diff_race --harness-quiet or the JSON cfg post). Each hook is
// failure-isolated: a throw here MUST NOT prevent the race-entry hooks below
// from being installed. Without this isolation, a transient resolve failure
// would silently leave the original sitting in the frontend forever.
// =========================================================================
function installHarnessMute() {
    try {
        function killDsCreate(modName, expName) {
            var p = Module.findExportByName(modName, expName);
            if (!p) return false;
            Interceptor.replace(p, new NativeCallback(function () {
                return 0x88780014; // DSERR_NODRIVER
            }, 'int32', ['pointer', 'pointer', 'pointer']));
            return true;
        }
        var any = false;
        if (killDsCreate("dsound.dll", "DirectSoundCreate8")) any = true;
        if (killDsCreate("dsound.dll", "DirectSoundCreate"))  any = true;
        log('quickrace: harness mute ' + (any ? 'installed' : 'SKIPPED (dsound not loaded)'));
    } catch (e) {
        log('quickrace: harness mute threw: ' + e.message + ' — continuing');
    }
}

function installHarnessWindow() {
    try {
        var createW = Module.findExportByName("user32.dll", "CreateWindowExA");
        var setPos  = Module.findExportByName("user32.dll", "SetWindowPos");
        if (!createW || !setPos) {
            log('quickrace: harness window resize SKIPPED (user32 exports missing)');
            return;
        }
        var setWindowPos = new NativeFunction(setPos, 'int32',
            ['pointer', 'pointer', 'int32', 'int32', 'int32', 'int32', 'uint32']);
        Interceptor.attach(createW, {
            onLeave: function (retval) {
                try {
                    if (retval.isNull()) return;
                    setWindowPos(retval, NULL, 50, 50, 640, 480, 0x34);
                } catch (_) {}
            }
        });
        log('quickrace: harness window resize installed (640x480 at +50,+50)');
    } catch (e) {
        log('quickrace: harness window resize threw: ' + e.message + ' — continuing');
    }
}

// Install hooks synchronously at script load time (before frida.resume) so all
// Interceptor entries are in place before the first instruction runs.
// cfg is populated with defaults above; recv('cfg') below updates it in-place
// before ScreenMainMenuAnd1PRaceFlow fires (game init takes ~1-2 s), so the
// replacement function always reads the correct scenario values.
log('quickrace: script loaded, installing hooks with default cfg');
// Harness ergonomics first — failure-isolated so they cannot prevent install()
// from setting up the race-entry hooks. Mute is safe: stubs DirectSoundCreate
// to fail at audio init. Window-resize hook was REMOVED 2026-05-12: it
// resized the OS window but not the render resolution, leaving the original
// rendering at native res cropped to 640x480 (upper-left zoom). Properly
// forcing render res needs DirectDraw COM vtable hooks (TODO if needed).
installHarnessMute();
install();

recv('cfg', function (msg) {
    // Update the shared cfg object; all hooks read it at call time, so no
    // re-installation is needed. This arrives well before the first frontend
    // tick because process startup (WinMain + DX init) takes ~1-2 seconds.
    Object.keys(msg.cfg).forEach(function (k) { cfg[k] = msg.cfg[k]; });
    log('quickrace: cfg applied ' + JSON.stringify(cfg));
});
