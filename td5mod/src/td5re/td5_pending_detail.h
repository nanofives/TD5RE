/**
 * td5_pending_detail.h -- Per-item test-focus detail text for the PENDING TO
 * TEST screen's ENTER-key modal (td5_pending.c / td5_fe_devscreens.c).
 *
 * Each entry pairs the EXACT k_seed[] (td5_pending.c) summary string with a
 * longer, testing-focused note: what to actually do, what correct behavior
 * looks like, and any gotcha worth knowing before testing. Matched by exact
 * string compare against the summary (td5_pending_detail_text), so order and
 * position here don't matter and coverage can be partial -- any item without
 * an entry falls back to a generic note.
 *
 * [2026-07-04] Introduced alongside the removal of the checklist's
 * crossing-out ("mark tested") feature: ENTER now opens a details modal
 * instead of toggling a checkbox. Investigated against the project's
 * session-memory notes so each detail reflects how the feature was actually
 * built and what its known gotchas are, not just a restatement of the
 * one-line summary.
 *
 * Included by exactly ONE translation unit (td5_pending.c); the array is
 * file-static there.
 */
#ifndef TD5_PENDING_DETAIL_H
#define TD5_PENDING_DETAIL_H

typedef struct { const char *text; const char *detail; } TD5_PendingDetail;

static const TD5_PendingDetail k_pending_detail[] = {
    { "PENDING TO TEST: ENTER opens a details modal (no more mark-tested)",
      "Highlight any row on the PENDING TO TEST list and press ENTER (pad A / mouse click) -- "
      "a centred modal should appear showing the full item text plus a longer test-focus note. "
      "Press ENTER, B, or ESC again to close it and return to the list at the same page/scroll "
      "position; paging (PREV/NEXT) and SUPR/DELETE should be inert while the modal is open." },
    { "PENDING TO TEST: crossing-out is gone; SUPR/DELETE still clears an item",
      "Rows no longer show a [ ]/[x] checkbox and ENTER no longer strikes a row through -- it only "
      "opens the details modal now. SUPR/DELETE on the highlighted row should still work exactly as "
      "before: it removes the row immediately and it stays gone (tombstoned) on the next launch." },
    { "Cup CHOOSE YOUR TEAM: roster box (top right) lists members by name",
      "On the CHOOSE YOUR TEAM cup screen, look for a box in the top-right listing each team and "
      "its member profile names; it should update live as you and the host assign players/AI to "
      "teams, and the AI-skill slider was compacted to make room for it." },
    { "MP mode select: buttons no longer overlap HOST banner text",
      "On the multiplayer SELECT GAME MODE screen, confirm the mode-select buttons sit clear of "
      "the HOST banner and mode-vote ring text at the top -- check as the actual host and also "
      "mid-vote, when border rings are drawn around a mode." },
    { "MP mode select: TIME TRIAL entry is gone, other modes unaffected",
      "On the multiplayer SELECT GAME MODE list, TIME TRIAL should no longer appear at all (removed "
      "because ghost pass-through and checkpoint start/finish don't work correctly in MP); confirm "
      "the remaining modes still line up with no gap where it used to sit." },
    { "MP profile screen: NAME + COLOUR button labels now centred",
      "On the MP split-screen profile-select screen, check the NAME and COLOUR button labels sit "
      "centred on their buttons rather than left-biased. Dev harness: TD5RE_MP_SIMUL_PREVIEW_PHASE=0 "
      "+ --StartScreen=20 boots straight to this screen without a full MP lobby." },
    { "Street lamps (EXPERIMENTAL, StreetLights=1 to enable): pools under posts",
      "Off by default -- set [Lighting] StreetLights=1 and drive past lamp posts at night/dusk; each "
      "nearby lamp should cast a warm light pool underneath it. Marked EXPERIMENTAL pending a "
      "look-dev pass, so rough pool shape/falloff is expected feedback, not necessarily a bug." },
    { "Shadows: no duplicated/offset echo copies (fence area, Moscow)",
      "Drive the fence area on Moscow in daylight with shadows on and look for the reported bug: "
      "shadows appearing as several offset duplicate copies (a ray-march dithering artifact) rather "
      "than one clean shadow. Compare with SunShadows=0 to confirm they're really gone." },
    { "Reflections P3: wet road mirrors cars/buildings on rainy Moscow",
      "Race Moscow in rain with [Lighting] Reflections=1; the road should turn wet-looking and "
      "reflect nearby cars/buildings, ray-traced against the scene rather than a flat wet tint." },
    { "Reflections P3: car paint/glass sheen at grazing angles",
      "With reflections on, watch your car's paint and windows while turning -- at grazing/shallow "
      "viewing angles they should show a mirror-like environment sheen, not a flat unlit surface." },
    { "Reflections P3: no smearing/ghosting artifacts while driving",
      "Drive at speed with reflections enabled and watch reflective surfaces (wet road, paint/glass) "
      "for smearing or ghosting trails -- the reflection should track cleanly frame to frame." },
    { "Reflections P3: Reflections=0 removes all of it (A/B)",
      "A/B test: set [Lighting] Reflections=0 and confirm every reflection effect above disappears "
      "completely, leaving flat surfaces -- the master kill-switch for the whole feature." },
    { "Shadows P2: cars/walls cast shadows on the road (drive daylight)",
      "Drive any track in daylight and confirm your car and roadside walls/buildings cast a visible "
      "shadow onto the road as you pass -- the base shadow-casting pass." },
    { "Shadows P2: no shadow acne/flicker on the road while driving",
      "While driving in daylight with shadows on, watch the road surface for shadow acne (speckled "
      "noise) or flicker frame-to-frame -- edges should stay clean and stable, not shimmer or crawl." },
    { "Shadows P2: tunnels/ambient zones cast NO sun shadow",
      "Drive through a tunnel or a track's ambient/shaded zone and confirm NO directional sun shadow "
      "is drawn inside it -- those zones use ambient lighting only." },
    { "Shadows P2: headlight beam blocked by the car in front",
      "At night with headlights on, tail another car closely and check that its body blocks your "
      "headlight beam -- you should see a shadow cast by the car ahead onto the road/car behind it." },
    { "Shadows P2: SunShadows=0 removes all of it (A/B)",
      "A/B test: set [Lighting] SunShadows=0 and confirm every sun-shadow effect above disappears -- "
      "the master kill-switch for the shadow-casting pass." },
    { "Lighting v2: authored zone colours show (dusk/tunnel tint)",
      "Drive through a track's authored lighting zones (dusk section, tunnel) and confirm the "
      "intended colour tint actually shows, rather than looking uniformly lit everywhere." },
    { "Lighting v2: headlights don't shine through walls (N.L)",
      "At night with headlights on, put a wall/building between the headlight and a surface behind "
      "it -- the beam should NOT shine through; lighting should respect the surface normal (N.L) and "
      "be occluded by geometry." },
    { "Lighting v2: Mode=0 looks identical to the old build (A/B)",
      "A/B test: set [Lighting] Mode=0 and confirm the scene looks the same as before the lighting "
      "v2 rework -- this mode is a faithfulness fallback, so any visible difference here is a "
      "regression." },
    { "PLAYER NAME option: results row + high-score prefill",
      "Set your player name in Game Options, finish a race, and confirm your typed name (not "
      "'PLAYER 1') appears in the results row; also check the high-score name-entry screen prefills "
      "with the same name instead of starting blank." },
    { "Damage bar: top-centre + pause-style blue-red fill",
      "In a race with car damage on, confirm the health bar sits top-centre and fills/drains using "
      "the same blue-to-red gradient style as the pause menu's bars, not a plain solid colour." },
    { "Damage bar on: checkpoint timer sits below it",
      "With the damage bar visible and checkpoint timers enabled, confirm the checkpoint timer is "
      "positioned below the damage bar rather than overlapping or sitting above it." },
    { "Police pullover: brakes hold all the way through (no coast gap)",
      "Get pulled over by a cop and watch the deceleration -- braking should be continuous all the "
      "way to a stop, with no gap where the car coasts freely partway through the sequence." },
    { "Pause menu: END RACE NOW only in local split-screen MP (not SP/net)",
      "Open the pause menu in single-player and in a networked race and confirm END RACE NOW is "
      "absent in both; open it in local split-screen MP and confirm it IS present there." },
    { "Pause menu: RESTART/QUIT/EXIT/LOBBY ask YES/NO confirm in split-screen MP",
      "In local split-screen MP, open the pause menu and pick RESTART, QUIT, EXIT, or LOBBY -- each "
      "should raise a YES/NO prompt before acting, not execute immediately." },
    { "Pause menu (single-player): RESTART/QUIT/EXIT act immediately, no confirm",
      "In single-player, open the pause menu and pick RESTART, QUIT, or EXIT -- these should act "
      "immediately with no YES/NO prompt, unlike split-screen MP." },
    { "END RACE NOW: next race camera starts normal (was stuck shifted)",
      "Use END RACE NOW to force-finish a race, then start the next race and check the chase camera "
      "begins in its normal default position, not stuck shifted from the forced finish." },
    { "Track walker: no span warp on degenerate quads (race normally)",
      "Race normally on any track and watch for a sudden position 'warp'/teleport as the car crosses "
      "a span boundary -- degenerate (malformed) track quads used to snap the walker incorrectly." },
    { "Headlights: per-pixel beams flood the road (deferred light pass)",
      "At night with headlights on, confirm the beam is a proper per-pixel lit cone flooding the "
      "road (via the deferred lighting pass), correctly lighting bumps/curves, not a flat decal." },
    { "Headlights auto-on in rain/overcast/dusk, off in bright daylight",
      "Drive across different weather/time conditions without touching the headlight control -- "
      "they should switch on automatically in rain/overcast/dusk and stay off in bright daylight." },
    { "Headlights: verify on a sunny track they stay OFF (Maui/Sydney)",
      "Race Maui or Sydney (bright, sunny reference tracks) and confirm headlights stay OFF the "
      "whole race -- the negative-case check for the auto-headlight logic above." },
    { "Split cup: later-race player can always accelerate (was camera-only)",
      "In a split-screen cup with multiple races, check a non-lead player's car in race 2+ can "
      "accelerate normally -- a bug had only the camera advancing correctly while the car itself "
      "stayed throttle-limited." },
    { "Pause menu: END RACE NOW force-finishes (YES/NO confirm; any player)",
      "Open the pause menu as ANY player (not just host) in split-screen and pick END RACE NOW; "
      "confirm it raises a YES/NO prompt and, on YES, force-finishes for everyone." },
    { "END RACE NOW: places ranked by current track progress, no DNFs",
      "After using END RACE NOW, check results rank every player by how far along the track they'd "
      "actually gotten -- nobody should show as DNF; everyone gets a placed result." },
    { "END RACE NOW: net race ends + shows results on all machines together",
      "In a networked race, have one player trigger END RACE NOW and confirm the race ends and the "
      "results screen appears at roughly the same time on every connected machine." },
    { "Alt+Enter fullscreen uses the display's native refresh, not 60Hz",
      "Press Alt+Enter to toggle fullscreen during gameplay and check the observed refresh rate "
      "matches your monitor's native refresh (e.g. 120/144Hz), not capped at 60Hz." },
    { "Paint: SELECT CAR PAINT panel - secondary colour + pattern + 32 presets",
      "Open SELECT CAR PAINT and confirm it offers a secondary colour picker, a pattern selector, "
      "and roughly 32 preset combinations, not just the single primary-colour picker from before." },
    { "Paint: Two-Tone/Stripes/Split show in BOTH menu preview and in-race body",
      "Pick a Two-Tone, Stripes, or Split pattern in the paint panel and confirm it renders on both "
      "the car-select preview AND the actual car body once in a race -- one-place-only is a bug." },
    { "Lane Assist at a fork: picks one branch a few spans early, not the divider",
      "Turn Lane Assist on and approach a fork -- it should commit to one branch a few spans BEFORE "
      "the fork rather than aiming at the physical divider/wall between the branches." },
    { "Split: one pad seen as 2 gamepads no longer joins as 2 players (cam bleed)",
      "If you have a controller Windows enumerates as two devices, confirm the split-screen lobby "
      "only lets it join as ONE player -- it could previously join as two, causing camera/input "
      "bleed between panes." },
    { "Reset car (split-screen): resets in place, no teleport-to-start/wall",
      "In split-screen, press reset-car on a stuck/wedged car and confirm it resets IN PLACE (same "
      "spot, corrected orientation), not teleporting to the track start or into a wall." },
    { "Reset car: fully repairs (health+dents); un-sticks a knocked-out car",
      "With damage on, wreck/knock out a car, then reset-car and confirm it comes back with full "
      "health AND cleared dents -- specifically test un-sticking a fully knocked-out car, not just a "
      "lightly damaged one." },
    { "Tutorial overlay now shows at the START of every race (was once-ever)",
      "Start several races in a row (gamepad connected) and confirm the controller tutorial overlay "
      "appears at the start of EACH one, not just the very first race ever played." },
    { "Game Options: new TUTORIAL on/off row turns the overlay off",
      "Open Game Options, find the TUTORIAL row, toggle it OFF, then start a race and confirm the "
      "tutorial overlay no longer appears (toggle back ON to confirm it returns)." },
    { "Game Options paginated (2 pages, < PREV / NEXT >); all rows fit, values/arrows OK",
      "Open Game Options and confirm it's split across 2 pages with < PREV / NEXT >, every row is "
      "fully visible with nothing clipped, and each row's value + selector arrows still work after "
      "paging." },
    { "Car defaults to your PROFILE colour (TD6 exact / TD5 nearest); PAINT overrides",
      "Set an accent colour on your profile, pick a car without touching PAINT -- it should default "
      "to your profile colour (exact match for TD6, nearest available for TD5). Then set PAINT and "
      "confirm it overrides the profile default." },
    { "MP profile: typed-but-unsaved NEW name auto-saves when the race starts",
      "On the MP profile screen, type a new profile name but do NOT explicitly save, then start the "
      "race -- the typed name should auto-save as a real profile rather than being discarded." },
    { "Celebrity leaderboard: unraced tracks show celeb names (not Frank/Ben)",
      "Look at the leaderboard for a track you have never raced -- default names should be "
      "celebrity-style (e.g. Michael, Lewis, Ayrton) rather than the old generic Frank/Ben. "
      "Detection is exact-match against the original default names, so a track with a real saved "
      "score should be unaffected." },
    { "Celebrity leaderboard: CelebrityNamesAPI=1 → fetches from randomuser.me",
      "Set CelebrityNamesAPI=1 (needs internet) and confirm celebrity names are fetched live from "
      "randomuser.me instead of the built-in static table; check the log for a successful fetch and "
      "that names apply and flush to the save immediately after load." },
    { "Pause menu RADIO slider (row 2) sets radio volume live",
      "Open the pause menu during a race with radio playing, find the RADIO slider on row 2, and "
      "move it -- volume should change live, not just on menu close." },
    { "Radio: quiet by default + mutes when minimized/paused/in menus",
      "Confirm the radio starts quiet by default and mutes automatically when the window is "
      "minimized, the game is paused, or you're back in frontend menus." },
    { "Radio: live station plays during a race (needs internet)",
      "With internet available and radio enabled, start a race and confirm a live internet radio "
      "station actually plays audio during the race -- needs a real network connection to verify." },
    { "Music seam wired: no audio regression (jukebox/race/pause)",
      "Move through jukebox/menu music, into a race, and into the pause menu, listening for any "
      "regression -- no missing tracks, no jarring cutoffs, no failure to resume across states." },
    { "Traffic: visible ~60% further on open tracks (no short-range pop-out)",
      "On an open (non-TD6) track, watch traffic ahead as you approach at speed -- it should become "
      "visible noticeably further away (~60% further) rather than popping in at short range. Knob: "
      "TD5RE_TRAFFIC_VIEW_DIST (default ~1.6x); TD6 tracks are perf-capped separately, so test on a "
      "faithful TD5 track like Moscow." },
    { "Damage Bar VISIBLE for every split-screen player (was invisible before)",
      "In split-screen with car damage on, confirm EVERY player's pane shows their own damage bar -- "
      "some panes could previously have an invisible/missing bar." },
    { "Damage Bar OFF (Game Options): no top bar, no wreck-out; dents stay",
      "In Game Options turn the damage bar OFF, then race and take damage -- the top bar should be "
      "hidden and the car should never wreck/knock out from zero health, but dents should still "
      "appear normally." },
    { "Damage Bar toggle is global: every split-screen player gets it or none",
      "In split-screen, toggle the damage bar and confirm it applies to ALL players' panes at once -- "
      "it's a single global setting, not per-player." },
    { "Cops: accel rubber-band out-drags a faster car on straights (catch up)",
      "Get chased by a cop while driving a notably faster car and watch a long straight -- the cop "
      "should accelerate hard enough to close the gap despite lower top speed (an acceleration "
      "rubber-band, not a top-speed cheat). Knob: TD5RE_COP_CHASE_ACCEL (default 280). Headless "
      "verification needs TD5RE_COP_CHASE_AI=1 + PlayerIsAI=1 + AutoThrottle=0 since cops normally "
      "only chase HUMAN slots." },
    { "Cops: tougher (300%); chasing cop shrugs off wall scrapes (no chase-end)",
      "Deliberately clip a chasing cop into a wall -- with durability boosted 300%, it should shrug "
      "the scrape off and keep chasing rather than the chase ending from a minor bump." },
    { "Cops: ease off for corners instead of flooring into the outside wall",
      "Watch a chasing cop through a sharp corner -- it should visibly ease off the throttle for the "
      "corner rather than flooring it into the outside wall. Knob: TD5RE_COP_CORNER_EASE." },
    { "Cops: pass a cluster and ALL of them chase you (no 1-cop limit)",
      "Drive past a cluster of multiple cop cars and confirm ALL of them start chasing, not just one "
      "-- the original one-chaser-per-target cap was removed." },
    { "Cops: longer chase leash (110 spans) + 200% catch-up; harder to shake",
      "Get chased, then try to lose the cop by pulling ahead -- it should take noticeably "
      "longer/further to shake it: the chase persists up to a 110-span gap and catch-up is boosted "
      "200%, so the cop keeps closing rather than giving up quickly." },
    { "Traffic: stuck car no longer fades in front of you (gates on render dist)",
      "Watch traffic ahead for a car that gets stuck -- it should NOT despawn/fade while still within "
      "your render distance in front of you; the despawn gate now respects the render-cull distance "
      "(front_keep) instead of a fixed shorter range." },
    { "Drag MP mode: SELECT GAME MODE -> DRAG RACE (no track pick, no AI)",
      "In multiplayer, open SELECT GAME MODE and pick DRAG RACE -- confirm it skips the normal "
      "track-selection screen entirely and adds no AI opponents; drag race MP is humans-only." },
    { "Drag MP options: TRAFFIC on/off, DISTANCE, EXTRA LANES",
      "On the drag MP options screen, toggle TRAFFIC, pick a DISTANCE preset, and set EXTRA LANES, "
      "confirming each choice actually changes the race you get." },
    { "Drag LONG/EPIC: strip+stadium truly extend, finish moves down it",
      "Pick LONG or EPIC and confirm the strip and stadium geometry are PHYSICALLY extended (not "
      "just a further finish marker on the original strip) -- you drive further over newly-inserted "
      "road/stadium tiles before the finish." },
    { "Drag race: strip widens to one lane per car (field-size lanes)",
      "Start drag races with different player counts and confirm the strip width scales to one lane "
      "per car -- a 2-car race should be narrower than a 6-car race." },
    { "Drag race: tap left/right to CHANGE LANES (NFS-Underground feel)",
      "During a drag race, tap left/right and confirm your car snaps into the adjacent lane with a "
      "quick, arcade lane-change feel, not a slow drift or no response." },
    { "Drag race: longer race via TD5RE_DRAG_LENGTH (existing road reused)",
      "Set TD5RE_DRAG_LENGTH higher and confirm the drag race is proportionally longer, reusing "
      "existing road geometry rather than needing new assets." },
    { "Lane Assist: road-centre aid, firmer further off; avoids grass",
      "Turn Lane Assist on, drift toward the road edge, and feel the correction -- gentle near "
      "centre, firmer further off-centre, always aiming for the drivable road (never the grass/slow "
      "lane) even at a fork. Baked constants: STRENGTH=900, MAX_YAW=400; feel previously "
      "user-confirmed 'perfect'." },
    { "Lane Assist toggle: SP Game Options / MP Profile screen / L key",
      "Confirm Lane Assist can be turned on/off from THREE places -- single-player Game Options, the "
      "MP Profile screen (next to AUTO/MANUAL), and the L key live in a race -- and all three "
      "reflect the same setting." },
    { "Traffic Battle: MP split-screen mode (below CUP) + solo (TD5RE_BATTLE=1)",
      "In multiplayer split-screen, find TRAFFIC BATTLE below CUP on mode-select; separately set "
      "TD5RE_BATTLE=1 and confirm you can also play it solo." },
    { "Traffic Battle: WIN option MOST WRECKS or CHECKPOINTS (catch-up deadline)",
      "In Traffic Battle options, choose the win condition -- MOST WRECKS or CHECKPOINTS (a catch-up "
      "deadline) -- and test both, confirming the results/winner logic matches the chosen mode." },
    { "Traffic Battle: oncoming traffic, no rivals/cops, spawn mid-track",
      "Start a Traffic Battle race and confirm traffic spawns oncoming (toward you), there are no AI "
      "rivals or cops, and players spawn mid-track rather than at a normal start line." },
    { "Traffic Battle: crashes wreck at ANY angle/speed; airborne cars wreck too",
      "Hit a traffic car at various angles and speeds (including glancing hits) and confirm each "
      "registers as a wreck; also confirm a car airborne on impact still wrecks correctly." },
    { "Traffic Battle: wrecked cars go translucent + pass-through (drive thru)",
      "After wrecking a traffic car, confirm its wreck goes translucent and you can drive straight "
      "through it, rather than it staying solid and blocking the lane." },
    { "Traffic Battle: live WRECKS HUD; results sort/label by WRECKS",
      "During Traffic Battle confirm a live WRECKS counter is on the HUD; at the end confirm results "
      "sort/label by WRECKS rather than the normal race-position columns." },
    { "Arcade: new power-ups SHIELD/EMP-FREEZE/MAGNET/ROCKET/REPAIR (HAZARD off)",
      "Play an Arcade race and grab boxes until you've seen SHIELD, EMP-FREEZE, MAGNET, ROCKET, and "
      "REPAIR; confirm each does something distinct and the old HAZARD (oil) no longer appears." },
    { "Game Options: CAR TOUGHNESS + DEFORMATION levels (Low/Norm/High)",
      "Open Game Options, set CAR TOUGHNESS and DEFORMATION to different Low/Normal/High "
      "combinations, and confirm crashes feel/look different accordingly." },
    { "Car damage ON all races: dents+scuff, smoke, wreck, finish orbit cam",
      "Race with damage on and confirm the full chain: dents/scuff accumulate, smoke appears on a "
      "badly damaged car, the car can wreck/knock out, and finishing triggers the orbit finish cam." },
    { "Chase cam: no vertical jitter at race start on high-refresh (120/144+)",
      "On a high-refresh display, watch the chase camera closely in the first second or two of a "
      "race start -- it should be smooth with no vertical jitter/bounce. Fallback if it persists on "
      "a given rig is a terrain-tilt IIR filter, not fully confirmed on every setup." },
    { "MP car select: host X/TAB menu sets everyone's car (same/slow/avg/fast)",
      "As host on MP car-select, open the X/TAB quick menu and pick 'same car', 'slowest', "
      "'average', or 'fastest' -- confirm it reassigns EVERY player's/AI's car, not just your own." },
    { "MP cup: player 1 keeps chosen car on race 2+ (no pre-cup car revert)",
      "Start an MP cup, have player 1 pick a specific car, complete race 1, and check race 2 onward "
      "-- player 1 should keep the chosen car rather than reverting to a pre-cup car." },
    { "ARCADE: power-up timer bar shows the full duration (not last secs)",
      "Grab a timed power-up in Arcade and watch its HUD timer from the moment you grab it -- it "
      "should show the FULL duration counting down, not only appear in the last few seconds." },
    { "ARCADE: item boxes spawn one lane closer to track centre",
      "In Arcade, compare item box placement to the road edges -- boxes should sit one lane closer "
      "to centre than the very outer edge, making them easier to reach without hugging the wall." },
    { "ARCADE: power-up effects last 20% longer (NITRO/GHOST/WRECK)",
      "Grab NITRO, GHOST, and WRECK in Arcade and time each effect -- each should run about 20% "
      "longer than the previous baseline duration." },
    { "Power-ups now ARCADE-only: SIMULATION shows no item boxes",
      "Start a race with Dynamics set to SIMULATION and confirm NO item boxes spawn anywhere -- "
      "power-ups are strictly Arcade-only. Watch for a dynamics-commit timing edge case where a late "
      "mode switch could still let boxes through." },
    { "MP splitscreen: gold HOST tag on profile/screen/mode/car selectors",
      "As session host in split-screen MP, check the profile, position, mode-vote, and car-select "
      "screens -- your selector should show a gold HOST pill badge on all four, not just some." },
    { "Split car-select 5+ players: big car + buttons|stats two columns; 7-9p car stays big",
      "Join a split-screen car-select with 5+ players -- each narrow pane should show a big car "
      "preview on top with buttons/stats in two columns below, and the preview should stay "
      "reasonably large even at 7-9 players rather than shrinking to a thumbnail." },
    { "Tutorial overlay: Xbox pad diagram (gamepad only); ALL players press to start; every race",
      "Start a race with a gamepad connected -- the overlay should show a real Xbox pad diagram with "
      "arrows from each control to its action; in split-screen, the race shouldn't start until ALL "
      "players press a button, and this should happen every race, not just the first." },
    { "Tutorial overlay: TutorialOverlay=2 forces (bypass gamepad gate); =0 off; never in MP/replay/trace",
      "Set TutorialOverlay=2 and confirm the overlay shows even on keyboard-only (bypassing the "
      "gamepad-only gate); set =0 and confirm it never shows. Confirm it also never appears during "
      "networked MP, replay playback, or a trace/benchmark run regardless of the setting." },
    { "MP cop chase: suspects can now pick TD6 cars, not just TD5",
      "In an MP cop chase lobby, as a suspect, open car select and confirm TD6 cars are selectable "
      "alongside TD5 -- suspects were previously restricted to TD5-only." },
    { "Reverse direction now works on the 7 lap circuits (Newcastle etc)",
      "Pick a 7-lap circuit (e.g. Newcastle) with Reverse enabled and confirm the race runs "
      "correctly for the full lap count -- checkpoints, finish detection, and AI routing all working, "
      "not just the first lap or two." },
    { "Controller: Y changes camera; horn moved to L3 (stick click)",
      "On a gamepad, press Y and confirm it cycles the camera view; click the left stick (L3) and "
      "confirm it sounds the horn -- horn was moved off its old binding onto L3." },
    { "Time trial on city tracks (HK/London): mid-track start no insta-end",
      "Run a Time Trial on a TD6 city track (Hong Kong or London) that starts mid-track via a P2P "
      "checkpoint layout, and confirm the run does NOT instantly end right after starting." },
    { "Replay no longer counts as a race (MP cup keeps real results)",
      "Play back a replay and confirm it isn't counted as an actual race; specifically, watching a "
      "replay between MP cup races should NOT affect or overwrite the real cup standings." },
    { "MP cop chase: bust arrow shows only when you crash a suspect",
      "As a cop, ram a suspect and confirm a bust arrow/indicator appears tied to that crash event -- "
      "it should not show from mere proximity without a crash." },
    { "MP cop chase: other players' name labels hidden",
      "In MP cop chase, look at other players' cars and confirm their floating name labels are "
      "hidden -- this mode intentionally suppresses labels, unlike normal MP racing." },
    { "MP cop chase: no 1st/2nd; top shows per-cop ARRESTS scoreboard",
      "Check the top HUD during MP cop chase -- it should show a per-cop ARRESTS scoreboard, not a "
      "normal 1st/2nd/3rd race-position indicator." },
    { "MP cop chase: arrest needs SIREN ON; else 'turn on siren' msg",
      "As a cop, try to arrest a suspect with siren OFF -- it should NOT count, with a 'turn on "
      "siren' message; turn the siren on and confirm the same crash now registers as a valid arrest." },
    { "MP cop chase: HORN toggles each cop's own siren",
      "As a cop, press HORN and confirm it toggles YOUR OWN siren (light + sound) independently of "
      "any other cop's siren state in the same race." },
    { "MP mode vote: A=vote -> profile-colour border ring on mode",
      "During an MP mode vote, press A on a mode option and confirm a border ring appears around it "
      "in YOUR profile's accent colour, marking your vote." },
    { "MP mode vote: more voters = more nested rings; host decides",
      "Have multiple players vote for the same mode and confirm each voter adds another nested ring "
      "(their own colour) rather than just one ring total; confirm the HOST ultimately picks the "
      "winning mode." },
    { "MP mode vote: arrow disappears once a player casts their vote",
      "Before voting a player's pane shows a selection arrow; once they vote (press A), confirm that "
      "arrow disappears for them (their vote now shows via the coloured ring instead)." },
    { "Time Trial after a cup: no instant end / right car on Race Again / no rivals on your lane",
      "Finish an MP cup, then start a Time Trial (or Race Again) -- confirm the run doesn't instantly "
      "end, you're driving the expected car (not a leftover cup car), and no AI rivals share your "
      "lane." },
    { "Snow tracks (Bern) easier: ice surface grips more, less slide",
      "Race Bern and compare handling on the icy surface -- grip should be noticeably higher and the "
      "car should slide less than the original unmodified ice physics." },
    { "Snow grip boost OFF on dry tracks (tarmac feel unchanged)",
      "Race a normal dry/tarmac track and confirm handling feels exactly as before the snow-grip "
      "change -- the ice-grip boost should apply ONLY to snow/ice surfaces." },
    { "MP CUP podium: humans show profile colour, CPU shown in grey",
      "Finish an MP cup and check the podium -- human players show their own profile accent colour, "
      "CPU/AI opponents show grey, so humans are easy to tell from bots at a glance." },
    { "Cops re-chase after you crash (was: 1 chase then never, esp. MP)",
      "Get chased, deliberately crash, recover, and keep driving -- a cop should be willing to chase "
      "you again; previously a cop would chase once and never re-engage for the rest of the race, "
      "especially in MP." },
    { "Split MP: reset car resets YOUR car, not the other player",
      "In split-screen, have player 2 press reset-car and confirm ONLY player 2's car resets -- "
      "player 1 is completely unaffected." },
    { "Split MP: change-camera affects YOUR pane, not the other",
      "In split-screen, have one player change camera view and confirm only THEIR pane's camera "
      "changes -- other panes keep their own independent camera." },
    { "Split MP: rear-view affects YOUR pane (positioned panes)",
      "In split-screen (including non-default 'positioned' pane layouts), hold rear-view as one "
      "player and confirm only their pane shows it, regardless of pane arrangement." },
    { "MP profile DELETE asks 'DELETE <name>? A=YES B=NO' first",
      "On the MP profile screen, select DELETE on a profile and confirm a prompt names that exact "
      "profile ('DELETE <name>?') with A=YES / B=NO, rather than deleting on the first press." },
    { "Profile delete removes ONLY the named/selected profile",
      "Create a few profiles, delete one via the confirm prompt, and confirm ONLY that profile is "
      "removed -- all others (and anyone holding one) are unaffected." },
    { "MP setup: ARCADE/SIM selector only on track select (not mode opts)",
      "Go through MP setup and confirm the ARCADE/SIMULATION selector appears on track-select and "
      "does NOT also duplicate on the mode-options screen." },
    { "ARCADE: ~2x as many item boxes spawn along the track",
      "Compare item box density along the track in Arcade mode -- roughly twice as many boxes "
      "should spawn along the route, so you should rarely go long without seeing one." },
    { "ARCADE: power-up effects last 2x longer (GHOST/WRECK/oil)",
      "Grab GHOST, WRECK, or the oil HAZARD in Arcade and time the duration -- each should last "
      "roughly double the original baseline (an earlier, larger duration pass than the later "
      "20%-longer tweak)." },
    { "ARCADE NITRO: sustained 2.5x acceleration boost (~5s), not instant",
      "Grab NITRO and watch acceleration -- it should ramp into a sustained ~2.5x boost lasting "
      "about 5 seconds, not an instant one-shot speed jump that fades immediately." },
    { "Collisions hug car silhouette (hull) - tight contact, no gap",
      "Clip the side of another car or wall at a shallow angle -- collision should trigger right at "
      "your car's visual silhouette/convex hull, no visible-but-no-collision gap." },
    { "AI un-wedges without driving off-track (steers to interior)",
      "Watch an AI car that gets wedged against a wall or another car -- its recovery steering "
      "should aim toward the track interior, never driving off-track while un-wedging." },
    { "AI steers away when wedged vs car/player (no infinite grind)",
      "Deliberately wedge an AI car against your own car and watch how long it stays stuck -- it "
      "should actively steer away to break the wedge, not grind indefinitely with no progress." },
    { "ARCADE airborne launch dialled way down (was too high/unplayable)",
      "Hit a jump/ramp in Arcade and compare the airborne launch height/distance -- should be "
      "noticeably tamer than an earlier version that launched cars unplayably high." },
    { "ARCADE power-ups now floating boxes you drive through",
      "Confirm item boxes in Arcade are floating cubes you simply drive INTO to collect, not a "
      "target you shoot or press a button on." },
    { "Item box: glowing spinning pulsing cube w/ kind icon",
      "Look closely at an item box before grabbing it -- it should be a glowing cube that's both "
      "spinning and pulsing, with a small icon indicating which power-up kind it holds." },
    { "Grabbed item box vanishes, respawns after 5 seconds",
      "Grab an item box, note its position -- it should vanish immediately then reappear in "
      "roughly the same spot about 5 seconds later, not stay gone or respawn instantly." },
    { "One power-up at a time; can't grab another until it ends",
      "While holding a power-up, drive over another item box and confirm you can NOT pick up a "
      "second one until the current effect ends." },
    { "Box frequency scales with human player count (100-300)",
      "Compare item box spawn frequency between a 1-human race and one with several humans -- more "
      "humans should mean noticeably more boxes (roughly a 100-300% scale), not a fixed rate." },
    { "5+ players: chance of paired boxes (left + right shoulder)",
      "With 5+ players, watch for item boxes spawning in PAIRS -- one on each shoulder of the road "
      "at roughly the same point -- as an extra spawn variant that starts at this player count." },
    { "HAZARD oil (3 lanes): hit it = drift uncontrollably ~2.5s",
      "Hit a HAZARD oil slick (should span roughly 3 lanes) and confirm it causes an uncontrollable "
      "drift for about 2.5 seconds before you regain full control." },
    { "Game Options: POWER-UPS on/off toggle (persists)",
      "In Game Options toggle POWER-UPS off, race and confirm no boxes spawn; restart the game and "
      "confirm the OFF state persisted rather than resetting to default." },
    { "Item boxes start after span 100, sit low near the road",
      "Note where the first item box appears -- should not spawn before roughly span 100, and boxes "
      "generally sit low, near road height, not floating high above it." },
    { "Item boxes sit on road edges; steer over to grab one",
      "Confirm item boxes sit along the road EDGES (not the centre), so you must steer toward one "
      "side to collect them rather than driving straight through the middle." },
    { "Car SILHOUETTE glows the effect colour (not a blob)",
      "Grab a power-up with a visual effect (e.g. GHOST) and look at your car -- the glow should "
      "trace your car's actual silhouette/shape, not appear as a generic blob detached from it." },
    { "GHOST: car turns translucent, passes through traffic",
      "Grab GHOST and drive into a traffic car or wall -- your car should turn visibly translucent "
      "and pass through it without colliding, for the effect's duration." },
    { "Effect name shows centred below the checkpoint timer",
      "Grab any power-up and check the HUD -- the active effect's name should display centred, "
      "positioned below the checkpoint timer, not overlapping it or off to a side." },
    { "Quick Race PHYSICS row toggles ARCADE/SIMULATION + fits OK/Back",
      "Open Quick Race and find the PHYSICS row -- toggling ARCADE/SIMULATION should actually change "
      "the race's dynamics, and the row should fit on screen alongside OK/BACK with no overlap." },
    { "Minimap: on a branch both fork tracks render (not just dots)",
      "Drive up to a fork with the minimap visible -- both branches should render as actual road "
      "geometry, not just a couple of disconnected dots marking the branch point." },
    { "MORE STATS: POWER captioned (engine output); WEIGHT under GRIP",
      "Open MORE STATS for a car and confirm POWER has a caption clarifying it's engine output, and "
      "WEIGHT sits under GRIP in the layout, not its old position." },
    { "MORE STATS BALANCE shows a visual front/rear weight split bar",
      "In MORE STATS, confirm BALANCE is a visual bar showing the front/rear weight split (a marker "
      "on the bar), not just a plain number." },
    { "Quick stat bars (under PAINT) match the MORE STATS scale",
      "Compare the quick stat bars under PAINT against the same car's bars in MORE STATS -- both "
      "should use the same scale so a car doesn't look inconsistently different between screens." },
    { "MORE STATS: Left/Right switches car in place (1P stats screen)",
      "On the single-player MORE STATS screen, press Left/Right and confirm it switches which car "
      "you're viewing IN PLACE, rather than returning you to car-select." },
    { "MORE STATS: captions under less-obvious bars; text clears RANDOM",
      "Confirm any stat bar whose meaning isn't obvious has a caption underneath; switch cars via "
      "RANDOM and confirm any leftover caption from the previous car is cleared, not overlapping." },
    { "Menu nav: UP/DOWN/LEFT/RIGHT follow visual order, no skips",
      "Navigate any frontend menu with UP/DOWN/LEFT/RIGHT and confirm the highlight moves in the "
      "order things are actually laid out on screen, no skipped or visually-unrelated jumps." },
    { "Menu nav: LEFT/RIGHT steps OK<->BACK on a GAMEPAD (no skip)",
      "On a gamepad, use LEFT/RIGHT between OK and BACK at the bottom of a menu -- it should step "
      "cleanly between just those two, without skipping BACK or landing elsewhere." },
    { "Menu nav: selectors still cycle their value on LEFT/RIGHT",
      "On a value-selector row (e.g. difficulty/laps), confirm LEFT/RIGHT still cycles its VALUE, "
      "not repurposed to only move focus between rows." },
    { "Track Select: DYNAMICS has arrows, sits above OK/BACK",
      "On Track Select, find the DYNAMICS (ARCADE/SIM) row and confirm it has visible left/right "
      "arrows and sits above OK/BACK without overlapping or crowding them." },
    { "Track Select: option rows fit without overlap (cup too)",
      "On both Track Select and its Cup version, confirm every option row (traffic, cops, laps, "
      "dynamics, etc.) fits without any row overlapping another." },
    { "Cop Chase RANDOM COP: cop drawn at race start, pick car normally",
      "Start a Cop Chase with RANDOM COP and confirm which player becomes the cop is decided at race "
      "start (not pre-assigned in the lobby), and the cop then picks a car through the normal flow." },
    { "MORE STATS shows real carparam physics (1P + split)",
      "Check MORE STATS numbers against a car's actual physics parameters (not placeholder values) "
      "in both single-player and split-screen." },
    { "Heavy cars hit harder/shove; light cars get flung",
      "Collide a heavy car into a light car and vice versa -- the heavy car should shove the light "
      "one aside with more force, while the light car gets flung further in the reverse case." },
    { "Heavy cars climb hills slower; light cars climb easy",
      "Take a heavy and a light car up the same steep hill -- the heavy car should climb noticeably "
      "slower/more strained, the light car comparatively easily." },
    { "Power-to-weight: lighter cars out-accelerate heavy",
      "Compare acceleration between a lighter and a heavier car with similar power -- the lighter "
      "car should out-accelerate, reflecting real power-to-weight rather than weight-independent "
      "acceleration." },
    { "Slipstream boost when tucked behind another car",
      "Tuck in closely behind another car on a straight and watch your speed -- a noticeable "
      "slipstream/draft boost should apply while tucked in, fading if you pull away or overtake." },
    { "Downforce: grippy cars stay planted in fast corners",
      "Take a high-grip car through a fast corner at speed and confirm it stays planted noticeably "
      "better than a low-grip car would in the same corner at the same speed." },
    { "ARCADE mode: collisions punchier than SIM but controlled",
      "Compare the same collision in ARCADE vs SIMULATION dynamics -- Arcade should feel punchier "
      "but still controlled, not so exaggerated the car becomes uncontrollable or glitches." },
    { "ARCADE pads grant NITRO/GHOST/WRECK/HAZARD power-ups",
      "In Arcade, grab item boxes repeatedly and confirm you see all four original power-up kinds "
      "over time: NITRO, GHOST, WRECK, and HAZARD." },
    { "Power-up chip + timer shows in each player's pane",
      "In split-screen Arcade with multiple humans holding power-ups at once, confirm EACH player's "
      "own pane shows their own power-up chip/timer, not player 1's or a shared/incorrect one." },
    { "GHOST cars pass through others (white shimmer aura)",
      "Grab GHOST and check your car's visual effect -- a white shimmering aura, and you should pass "
      "through other cars/traffic without colliding while it's active." },
    { "HAZARD drop spins out the next car that touches it",
      "Drop a HAZARD (oil) power-up behind you and have another car drive over it -- that car should "
      "visibly spin out from touching it." },
    { "SIMULATION = arcade grip/power + realistic gravity",
      "Compare SIMULATION against ARCADE -- SIMULATION should keep the same grip/power feel but use "
      "more realistic gravity (jumps/airborne moments should feel heavier than Arcade's floatier "
      "feel)." },
    { "ARCADE/SIM picked on track + multiplayer screens",
      "Confirm the ARCADE/SIMULATION choice is available and settable from both the single-player "
      "track-select screen and the multiplayer setup screens." },
    { "Demo fires after 1 min idle on main menu",
      "Sit on the main menu without pressing anything for about 1 minute and confirm the "
      "attract-mode demo automatically kicks in." },
    { "Demo: random track/car/opponents/traffic, no cops",
      "Watch an attract-mode demo and confirm it picks a random track, car, and opponents, includes "
      "normal traffic, but never spawns cops." },
    { "Demo returns to main menu on finish (no results)",
      "Let a demo race run to completion and confirm it returns straight to the main menu, without "
      "ever showing a race-results screen." },
    { "Cop Chase INFECT: arrested car becomes a random-car cop",
      "In INFECT mode, get a suspect arrested and confirm they immediately become a cop themselves, "
      "driving a randomly-chosen car, not necessarily the arresting cop's or their old suspect car." },
    { "INFECT: human keeps driving as cop; AI cop gives chase",
      "After a human suspect is infected in INFECT mode, confirm they keep driving under their own "
      "control as the new cop, and AI-controlled cop(s) actually give chase, not idle." },
    { "INFECT: round ends once every suspect is infected",
      "Play an INFECT round through and confirm it ends exactly when every original suspect has been "
      "turned into a cop, not on a fixed timer or lap count." },
    { "Collisions fire on real model contact (mesh hitbox)",
      "Clip another car at an angle where the visual models actually touch but a simple bounding box "
      "might not (or vice versa) -- collisions should fire on the real model/mesh hitbox." },
    { "Cop Chase results: COPS (arrests) + SUSPECTS (bust time)",
      "Finish a Cop Chase race and check results -- cops should be scored by arrest count and "
      "suspects by bust-survival time, as two distinct categories, not one shared column." },
    { "Cop Chase ends: all suspects busted or all finish",
      "Play Cop Chase to completion and confirm it only ends when EITHER every suspect is busted OR "
      "every suspect finishes, not some other condition like a time limit." },
    { "Italy (Courmayeur) starts a race without crashing",
      "Start any race type on Courmayeur (Italy) and confirm it loads and races normally without "
      "crashing -- previously an out-of-bounds route-table crash specific to this track's launch." },
    { "Cop roles: OK rejected if all-cop or all-suspect",
      "On the Cop Chase role-assignment screen, try to confirm a setup where every player is a cop, "
      "and separately all-suspect -- both should be REJECTED since a valid chase needs at least one "
      "of each role." },
    { "Arrested car stops dead, can't be driven/pushed",
      "Get a suspect arrested and try to drive or push that car -- it should be completely immobile: "
      "no throttle response and not nudgeable by other cars." },
    { "ARRESTED splash centred; its floating bar hidden",
      "When a suspect is arrested, confirm the ARRESTED splash appears centred, and any floating "
      "status bar that car normally has is hidden rather than still showing." },
    { "Arrested cars drop off the split-screen overview map",
      "In split-screen Cop Chase, arrest a suspect and check the overview/minimap -- their icon "
      "should disappear rather than continuing to be tracked." },
    { "Strong short rumble for BOTH cop+suspect on arrest",
      "With both a cop's and a suspect's controllers connected during an arrest, confirm BOTH get a "
      "strong but short rumble pulse at the moment of arrest, not just one side." },
    { "All suspects show a bust bar; shrinks w/ distance, 2x range",
      "As a cop, look at multiple suspects -- each should show its own bust-progress bar, shrinking "
      "as the suspect gets further away, with roughly double the effective range of an earlier "
      "version of this bar." },
    { "CHOOSE YOUR TEAM / COP ROLES show profile names",
      "On both the CHOOSE YOUR TEAM (cup) and COP ROLES (cop chase) screens, confirm every player "
      "shows their actual profile name, not a generic PLAYER N placeholder." },
    { "Cup team mode: host assigns AI opponents' teams",
      "As host, set up a team-mode cup and confirm you can assign which team each AI opponent "
      "belongs to, not just the human players." },
    { "Cup team mode: per-opponent skill slider drives AI",
      "In cup team-mode setup, adjust an individual AI opponent's skill slider and race them -- "
      "their driving pace should visibly reflect that setting." },
    { "Dev: host assigns teams to ADD AI PLAYER bots",
      "Using the dev ADD AI PLAYER tool, confirm the host can also assign added bots to specific "
      "teams in a team-mode cup, the same as real AI opponents." },
    { "Cup final standings show profile names (not PLAYER N)",
      "Finish a full cup and check the final standings -- every entrant should be listed by their "
      "actual profile name, not a generic PLAYER N label." },
    { "WHAT NEXT? CAR SELECTION -> car grid (not profile select)",
      "After a cup race, choose CAR SELECTION from WHAT NEXT? and confirm it goes straight to the "
      "car-grid picker, not a profile-select screen first." },
    { "WHAT NEXT? buttons aligned under the title (both layouts)",
      "Open WHAT NEXT? in both single-player and split-screen/MP layouts and confirm the buttons "
      "align consistently under the title in both, not shifted in one." },
    { "Crisp on 4K/high-DPI scaled displays (no blur)",
      "Run the game on a 4K or high-DPI/scaled display and check menu text/UI for blur -- everything "
      "should render crisp at that scaling, not soft or upscaled." },
    { "Menu UP/LEFT work with 5+ pads (no stuck-stick block)",
      "Connect 5+ gamepads and try UP/LEFT from any one of them -- navigation should work normally; "
      "previously that many pads could leave UP/LEFT effectively stuck/blocked." },
    { "Rumble survives many races (no FF death after first race)",
      "Play several races in a row on the same session and confirm force-feedback keeps working on "
      "every one -- previously it could die after the first race and never return." },
    { "Pad that sleeps/reconnects mid-session re-rumbles next race",
      "Let a wireless pad sleep (or disconnect/reconnect) mid-session, then start the next race -- "
      "rumble should resume, not stay permanently dead." },
    { "MP results table: name aligned, columns, divider",
      "Finish an MP race with several players and check results -- names aligned, values in clean "
      "columns, and a visible divider separating header from rows." },
    { "Cup results screen + POINTS column (race + total)",
      "Finish a cup race and check the results screen for a POINTS column showing both this race's "
      "points and the running cup total." },
    { "7-player empty cells: one MAP + one STANDINGS (no dup)",
      "Set up a 7-player split-screen session and confirm the resulting empty grid cells show "
      "exactly one MAP filler and one STANDINGS filler, not two of the same kind." },
    { "Empty-cell map zoom/rotation is smooth, no flicker",
      "Watch an empty-cell filler MAP over time -- its zoom/rotation animation should be smooth, "
      "with no flickering or popping as it adjusts to the live race." },
    { "Split-screen overview map: no diagonal on P2P tracks",
      "On a point-to-point track in split-screen, check the overview/filler map -- it should not "
      "show a stray diagonal artifact that circuit-track map logic could otherwise produce." },
    { "CUP - WHAT NEXT? title at top, buttons realigned/narrower",
      "After a cup race, open WHAT NEXT? and confirm the CUP title sits at the top with the option "
      "buttons realigned and narrower than the older wider layout." },
    { "7th+ split-screen car sits flat (no roll/wobble)",
      "In a session with 7+ players, watch the 7th-and-later cars specifically -- they should sit "
      "flat without the roll/wobble artifact that only affected cars beyond the 6th slot." },
    { "Cup picker: per-race track/traffic/police, one at a time",
      "In the cup track picker, confirm you can set track, traffic, and police individually FOR EACH "
      "race in the cup, one race at a time, not one shared setting for the whole cup." },
    { "Cup options: AI OPPONENTS count (they score points)",
      "Set the AI OPPONENTS count in cup setup and run it -- those opponents should actually score "
      "cup points across races like a real competitor, not just be decorative." },
    { "Cup race ends the moment all humans finish",
      "Run a cup race with AI still on track after the humans finish -- the race should end as soon "
      "as ALL human players finish, without waiting for every AI car." },
    { "Dev: ADD AI PLAYER bots in the MP lobby (A key)",
      "In an MP lobby, press A on the dev ADD AI PLAYER tool and confirm it adds a bot that behaves "
      "like a normal joined player for setup purposes." },
    { "CHANGELOG screen: bullets, spacing, top button",
      "Open CHANGELOG from the main menu and confirm entries are clean bulleted lines with sensible "
      "spacing between days/sections, and the button to reach it sits at the top of the main menu." },
    { "MP split-screen post-race menu",
      "Finish a split-screen MP race and confirm a proper post-race menu appears (results + "
      "next-step options), not a drop to the frontend or a blank/broken screen." },
    { "Police chase: durable cop, no airborne, dodges traffic",
      "Get chased by a cop and watch its behaviour -- durable (not easily wrecked), avoids launching "
      "airborne over bumps/jumps, and actively dodges traffic rather than plowing into it." },
    { "Car-select HANDLING bar (Aston Vantages not blank)",
      "On car-select, cycle through cars including the Aston Vantage variants and confirm the "
      "HANDLING bar shows an actual value for each -- specific cars could previously show blank." },
    { "WGI rumble backend for a 5th+ pad",
      "Connect a 5th (or later) gamepad and confirm it gets working rumble via the "
      "Windows.Gaming.Input fallback path, since earlier slots use a different backend." },
    { "Split-screen back-confirm on every setup screen",
      "Walk through every split-screen setup screen (profile, mode, car-select, track, etc.) and "
      "press BACK/ESC on each -- a GO BACK? confirmation should appear consistently on all of them." },
    { "Regular-car horn + TD6 character horns",
      "Press horn in a regular TD5 car and confirm a normal horn plays; drive a TD6 character car "
      "and press horn -- confirm it plays that character's distinct horn instead." },
    { "Short friendly controller names in the lobby",
      "Look at controller names in the MP lobby -- they should show short, human-friendly device "
      "names, not long raw Windows device-enumeration strings." },
    { "MP catch-up paces off the next opponent ahead",
      "Fall behind in an MP race and watch the rubber-band catch-up -- it should pace off the next "
      "opponent directly ahead of you, not some fixed global target." },
    { "Traffic: lighter, no despawn-in-front, fills gaps",
      "Drive through dense traffic and confirm: density feels lighter overall, no car despawns while "
      "visibly in front of you, and gaps in the flow get filled rather than staying empty." },
    { "Controller slots stable across a pad disconnect",
      "With multiple pads connected, disconnect one mid-session and confirm the remaining pads' "
      "player-slot assignments stay stable, not shuffled/reassigned by the disconnect." },
    { "MP opponent labels below the wheels, 50% opacity",
      "In MP, look at another player's car -- their name label should float near wheel height (not "
      "above the roof) at roughly 50% opacity, not fully solid." },
    { "Reset-car on SELECT; CHANGE VIEW on Y; horn on L3",
      "On a gamepad, confirm SELECT/Back resets your car, Y changes camera view, and L3 sounds the "
      "horn -- all three bindings should be in effect together." },
    { "Split-screen force feedback on the correct pad",
      "In split-screen with multiple pads, cause a force-feedback event (e.g. a collision) for one "
      "specific player and confirm the rumble fires on THAT player's pad, not a different one." },
    { "Manual-only stuck recovery + 5s cooldown",
      "Get your car stuck and confirm recovery only happens when you manually trigger it (no "
      "automatic teleport-free after a few seconds), with roughly a 5-second cooldown before you can "
      "use it again." },
    { "MP cop-chase multi-cop + cop-only car select",
      "Set up an MP cop chase with more than one cop and confirm multiple cops can be active "
      "simultaneously (not capped at one), and cop-role players go through car-select correctly." },
    { "Split-screen empty-cell map zoom-to-field",
      "In split-screen with empty grid cells showing a filler map, confirm the map's zoom "
      "automatically frames the live field of racers rather than a fixed zoom that could cut cars "
      "off or show mostly empty space." },
    { "Per-viewport engine audio pan for 3+ players",
      "In split-screen with 3+ players, listen to engine audio panning per viewport -- each player's "
      "pane should have engine sound appropriately panned for that pane, not all panes sounding "
      "centred/identical." },
    { "CHANGELOG + version screen (this build)",
      "Confirm CHANGELOG is reachable from the main menu and displays a version/build identifier for "
      "the current build, so testers can confirm which build they're looking at." },
    { "PENDING TO TEST menu + overlay (this build)",
      "Confirm the PENDING TO TEST screen itself is reachable from CHANGELOG, lists the current "
      "backlog, and that its in-game right-edge overlay (F11) can be toggled on while actually "
      "racing." },
    { "PENDING TO TEST: SUPR/DELETE removes the highlighted item",
      "On the PENDING TO TEST screen, highlight any item and press SUPR/DELETE -- it should be "
      "removed immediately and stay gone (retired) even after restarting the game." },
};
#define K_PENDING_DETAIL_COUNT ((int)(sizeof(k_pending_detail) / sizeof(k_pending_detail[0])))

#endif /* TD5_PENDING_DETAIL_H */
