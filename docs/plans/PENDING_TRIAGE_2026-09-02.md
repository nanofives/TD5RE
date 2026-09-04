# PENDING TO TEST — triage, list refactor and test rounds (2026-09-02)

Source: root `pending_to_test.csv` (447 rows, 224 pending, 223 tested) plus 3 rows only present in the
tracked seed `td5mod/src/td5re/pending_to_test.csv`. Row numbers below are 0-based indexes into the root
CSV data rows (header excluded).

## 1. State of play (verified, not assumed)

| Fact | Value |
|------|-------|
| Main tree HEAD | `3e16219d` detached, 216 commits BEHIND master |
| master | `cf722053` (lives in worktree `infra-toolchain-ini`), has `td5_trackgen.c` (21181 lines) |
| r14-integration | 63 commits ahead of master (R14 autotrack, not merged) |
| Root `td5re.exe` / `td5re_release.exe` | built 2026-08-22, stale for every item dated after that |
| Root CSV vs tracked CSV | 40 rows only in root (all pending: R8-R10 autotrack + Aug 19-20 fixes), 3 rows only in tracked (R5 bridge), 0 status disagreements on shared rows |
| Loader precedence (`td5_pending.c` pending_path) | the TRACKED copy wins when present under the exe dir, so a dev run reads/writes the tracked file, not root |

Consequences:
- No test round is valid on the current root exe. A fresh build from `r14-integration` (superset of master) is the precondition for every category, autotrack included.
- The in-game list cannot show the 40 root-only rows because the game loads the tracked copy. The memory note claiming the root file is live is contradicted by the loader code and needs correcting.

## 2. List refactor — EXECUTED 2026-09-02 (both CSVs now identical, 450 rows, 212 pending)

Applied as STATUS FLIPS, not deletions, so the row numbers in this document stay valid: 19=duplicate; 4/57/446=info; 445/3=open; 271/269 merged into 302; 270/317 superseded by 418; 309/310/311/316 merged into 308; 294 merged into 120. Tracked seed NOT yet committed (main tree is detached HEAD; commit from the master worktree).

1. **Unify the two CSVs.** Union of root + tracked = 450 rows. Write the union to the tracked seed (commit it) and copy it to root so both are identical. Going forward, one rule: write the tracked file, mirror to root.
2. **Drop the exact duplicate.** Rows 17 and 19 (net cop chase field) are byte-identical. Keep 17.
3. **Retire non-drive-testable rows** with status `info` (hidden from the list, kept for history):
   - 4 (R9 CITY instrument: a log report, not a visible behaviour)
   - 57 (selftest rework: verified by `scripts/selftest.ps1`, not by driving)
   - 446 (span 66 facade overlap: explicitly "no separate fix", re-measure only if items 1/2 still show)
4. **Mark known-open rows honestly** with status `open` (hidden from the test list, tracked as bugs): 445 (side-street outer sidewalk, NOT FIXED), 3 (span 66 massing, NOT REPRODUCED). Testing them as pending fixes produces false failures.
5. **Merge superseded arcade rows** (three duration passes and four box-placement passes are pending at once):
   - 302 (2x) + 271 (+20%) + 269 (timer bar full duration) -> one row "power-up durations and timer bar"
   - 270 (one lane closer) is contradicted by 317 (road edges) and 418 (every lane); keep 418 + 417, retire 270 and 317 as superseded
   - 308 + 309 + 310 + 311 + 316 -> one row "item box look, respawn, one-at-a-time, start span"
6. **Merge cop re-engage rows**: 120 (crashed cop recovers) + 294 (re-chase after crash) test the same behaviour; one row.
7. **Fix the loader/documentation mismatch**: either (a) keep code, update the memory + `/fix` skill text to "tracked seed is live, mirror to root", or (b) change `pending_path` to prefer root. Recommendation: (a), the code comment gives a deliberate reason (a root copy must not hijack dev writes).

Net effect: 224 pending -> about 205 testable rows in 13 categories.

## 3. Code refactor candidates ("needs optimization"), gated on test outcome

Retire a knob only after the round confirms the behaviour it guards. Measured facts:

| Module | Signal | Refactor |
|--------|--------|----------|
| `td5_trackgen.c` (master) | 21181 lines in ONE file; 207 `TD5RE_*` env knobs, 95 of them round-scoped `TD5RE_R<n>_*` A/B switches for shipped fixes | (1) after round A: delete every `TD5RE_R<n>_*` knob whose fix is confirmed (each is a dead branch plus a getenv per span-side). (2) Split along the SECTION banners already present: topo authority, city/cross, bridge/water, tunnel, flora/props, guard/instruments. (3) Rows 1, 432, 439 all describe "two emitters disagreed, replaced by one shared authority". Generalise: every emitter queries the same corridor/carriageway/topo predicates (`tg_side_corridor_here`, `tg_carriageway_reach` including side streets, `tg_topo_*`, `tg_bridge_column_lateral`). Audit remaining direct users of `tg_side_blocked` and `k_biomes[].ground_page` (row 437 found two more). |
| `td5_arcade.c` | 31 `TD5RE_ARCADE_*` knobs; three successive duration passes still pending | One per-power-up table `{frames, accel_pct, alpha, lanes}` replacing scattered knobs; keep one `TD5RE_ARCADE_DURATION_PCT` scale. |
| Cop chase (`td5_game.c`, `td5_ai.c`, `td5_hud.c`) | 20 `TD5RE_COP_*` knobs; row 26 found racer-slot bounds using `TD5_MAX_RACER_SLOTS` (16) where the runtime boundary is `g_traffic_slot_base` (6) | Sweep every `< TD5_MAX_RACER_SLOTS` bound that classifies an actor as racer/cop/suspect; route through one `td5_game_slot_is_racer()` helper. Row 25 made `td5_game_cop_chase_field()` the single source for the field; finish the job for is_cop/is_suspect. |
| `td5_ai_driver.c` | 30 `TD5RE_AI_DRIVER_*` knobs across P0-P6 + C2/C3 + followup-a | After round C: freeze confirmed defaults into constants, keep only LINE (opt-in feature) and LEASH (balance). |
| Pending workflow | two CSVs, loader precedence undocumented in the skill | Section 2 item 7. |

## 4. Test rounds — one category per session

Precondition for all rounds: build `r14-integration` via `build_all.bat` (absolute path), confirm the root exe timestamp is fresh, `[Logging] Enabled=1`. Each round: I set up the launch config without stealing focus, you drive, you mark rows tested in-game (DELETE) or give me the row numbers.

| Round | Setup | What you look for |
|-------|-------|-------------------|
| A Autotrack (43) | `--DefaultTrack=60`, seeds 99991 and 777, `TD5RE_AUTOTRACK_BRIDGES=1`, SCENERY=1; spans named per row (66, 143-150, 358-361, 549, 617, 1002, 1039, 1050) | Per sub-round A1..A6; `--StartSpanOffset` to jump to the named span |
| B Cars/gearbox (10) | Quick race, MANUAL then AUTO; cars: Charger, Cerbera, Audi TT, Cobra, XJR-15, Pitbull; Bern for snow | Top speed reachable, per-gear ceilings, shift bonus, tier labels in the MP deal, ice grip |
| C AI driver (12) | RACE OPTIONS AI MODEL row; Moscow, Newcastle, Blue Ridge, Tokyo; `TD5RE_AI_DRIVER_LINE=1` for row 34 | No wedged cars, clean overtakes, pace vs CLASSIC on twisty tracks |
| D Cop chase (23) | D1 SP police chase with OPPONENTS 1..5; D2 2-pad split cop chase; D3 needs the second machine | Siren after countdown, no traffic suspects, no COP<n> strip, cops re-engage, siren-gated arrests |
| E Drag (10) | SP Drag Race (one car pick), OPPONENTS 1..7, TRAFFIC on; then MP drag LONG/EPIC | Lanes == opponents+1, traffic keeps coming past 60 s, banners clean, lane-change taps |
| F Arcade (24) | Quick Race PHYSICS=ARCADE, POWER-UPS on, Newcastle + Pelton | Boxes on the road in every lane, each of the 10 power-up kinds, durations |
| G Traffic/damage/battle (16) | Damage on, TRAFFIC BATTLE 2-pad, Moscow for traffic view distance | Damage bar in every pane, wrecks translucent, 60 s finish timer |
| H Split-screen MP (33) | 2 pads, then 5+ pads/keyboard for the car-select layout; cup of 2 races | Pause confirms, END RACE NOW rules, per-pane camera/reset/rear-view, HOST tags, vote rings |
| I Netplay (2) | Two machines on LAN, one deliberately older build for the version NAK | DIFFERENT GAME VERSION message, END RACE NOW syncs results |
| J Graphics (22) | LOW then HIGH (RT); Keswick night/rain, Blue Ridge, SF, Australia sunny | Banners, fences at distance, tree occlusion, selective wet reflection, no first-frame freeze, headlights off in sun |
| K Frontend (17) | Menus only, mouse + pad | Double-click, CONTROL OPTIONS spacing, paint panel, MORE STATS, minimap on Newcastle |
| L Audio/input (9) | Horn near traffic, radio with internet, 5 pads for WGI rumble | No engine detune, radio mutes on pause, rumble persists across races |
| M Misc (6) | View Replay at 144 Hz, Lane Assist at a fork, `scripts/selftest.ps1 -Suite full` | Smooth replay, branch commit before the fork, suite exit 0 |

Suggested order: A (largest, newest, most likely to regress) -> J -> F -> D -> H -> B -> C -> E -> G -> K -> L -> M -> I (needs the second machine).

## 5. Category listing (row index and summary)

## A. AUTOTRACK (DefaultTrack 60, seeds 99991 + 777)


### A1 City / crossings / pavements (12)

- [0] Auto-track item 2 (span 150): double sidewalk on a fork branch -- R8 REGRESSION, fixed
- [1] Auto-track item 4 (span 361): no sidewalk where a branch merges back -- fixed
- [2] Auto-track item 3 (span 358): the thing right of 358 is a PLAZA, not a park; its ends are now built
- [3] Auto-track item 1 (span 66): crossing massing over the sidewalk -- NOT REPRODUCED, do not assume fixed
- [10] Auto-track city (R8 item 2): pavement TAPERS into a fork instead of stopping dead on one span
- [11] Auto-track city (R8 item 12): no city building block standing in the middle of a roadside park
- [431] R8 item 1: perpendicular streets are much longer and the massing no longer crowds their mouth
- [432] R8 carried-in: side-street mouths inside a fork now get their flanking pavement
- [433] R8 item 9: a sharp turn now reads as turning off an existing street (continuation)
- [444] Auto-track item 1 (span 66): a phone box and spectators stood IN THE ROAD at a side-street crossing -- fixed
- [445] Auto-track item 2 (span 66): buildings on the OUTER EDGE of the side street leave the crossing with no sidewalk -- NOT FIXED (measured, still present)
- [446] Auto-track item 3 (span 66): overlapping facade geometry -- re-measured post-merge, no separate fix

### A2 Topography / terrain / flora (9)

- [5] Auto-track topographic continuity (R9 items 6/7/11): one shared rule every ground emitter consults
- [6] Auto-track item 6 (span 549): a falling verge RUNS OUT instead of ending in mid-air
- [7] Auto-track item 6 (trees): flora follows a slope down instead of standing on its lip
- [8] Auto-track item 7 (span 617 U-turn): a verge stops at the other carriageway instead of crossing it
- [9] Auto-track item 11 (span 1050): no more grass slabs scattered between tile slabs at a biome change
- [434] Auto-track terrain (R8 items 5/15): the field beside the road reaches the horizon again
- [435] Auto-track terrain (R8 item 15b): the seaward horizon ends in a coastline, not in nothing
- [436] Auto-track terrain (R8 item 14): the distant tree line is no longer stretched or one repeated page
- [437] Auto-track terrain (R8 item 16): snow gets more than one ground texture, and a snowy median distinct from it

### A3 Bridges / water / coast (7)

- [439] Auto-track item 9 (span 1002): over-bridge structure now lands on the piers -- fixed
- [440] Auto-track item 9 (textures): bridge pillars and horizontal beam have their own materials -- fixed
- [441] Auto-track item 9 (coastline): shore where the river meets the bank -- fixed
- [442] Auto-track item 10 (span 1039): background massing over water -- fixed
- [443] Auto-track: silent generation death on seed 777 -- root-caused (heap corruption from a compaction cursor), over-water guard restored ON
- [425] Auto-track water: sea plane alongside COAST biome (Phase 3)
- [426] Auto-track bridges rebuilt as decks over a river (Phase 4)

### A4 Tunnels / underpasses (2)

- [429] Auto-track tunnels: enclosed dim interior + lit portal mouths (Phase 7)
- [438] Auto-track tunnels (R9 items 5/13): city tunnels rebuilt as highway underpasses; the tunnel mouth's surround was the HILLSIDE page all along

### A5 Biomes / surfaces / branches (4)

- [430] Auto-track snow-coherent biome seeds + ALPTOWN + one-side sea groundwork
- [427] Auto-track themed road surfaces: ice/cobble/dirt/gravel per biome (Phase 5)
- [428] Auto-track multiple branches: N forks with varied topologies (Phase 6)
- [423] Auto-track thematic trees + COAST/ALPINE/ORIENTAL biomes (Phase 1 of all-elements port)

### A6 Facades / props (Phase 1-2 port) (5)

- [419] Auto-track buildings are flat facade street-walls (no mid-window cut)
- [420] Auto-track facade + tree sizes copied from measured shipped-track geometry
- [421] Auto-track streets mix facade textures per block + grey road/ground
- [422] Auto-track buildings have ground-floor storefronts (shops at street level)
- [424] Auto-track roadside prop layer: people/statues/animals/streetlamps (Phase 2)

### A7 Instruments (log-only, not drive-testable) (1)

- [4] Auto-track R9 CITY instrument: per-span-side pavement UNIQUENESS + fork-mouth sweep

## B. CARS / PHYSICS / GEARBOX


### B (10)

- [12] Top-gear fix: 8 cars can finally reach their own top speed (cops excluded)
- [13] Gearbox rework + earned manual bonus up to +50%
- [14] AUDI TT / MUSTANG COBRA / XJR-15 got real physics (were CERBERA clones)
- [15] PITBULL mass fixed (had the traffic-vehicle value); tracked override layer
- [16] Every car gets a SLOW/NORMAL/FAST class (was 56% unclassified)
- [45] Transmission is car-select-only (no INI AutoGearbox); defaults to Automatic
- [291] Snow tracks (Bern) easier: ice surface grips more, less slide
- [292] Snow grip boost OFF on dry tracks (tarmac feel unchanged)
- [304] Collisions hug car silhouette (hull) - tight contact, no gap
- [353] Collisions fire on real model contact (mesh hitbox)

## C. AI DRIVER MODEL


### C (12)

- [44] RACE OPTIONS AI MODEL row cycles CLASSIC/SMART/DRIVER and switches opponent AI
- [43] DRIVER AI (default) drives cleanly + faster than CLASSIC on Moscow & Newcastle
- [42] DRIVER AI traction cap + spin/stuck recovery -- no opponents wedged for a whole race
- [41] DRIVER AI racecraft: follow without rear-ending, overtake slower cars, spread the field
- [40] DRIVER AI personalities: per-driver pace/aggression/line/consistency + hidden catch-up
- [39] DRIVER AI cross-track sweep: two real bugs fixed (pace bootstrap + corner line-clip)
- [38] DRIVER AI multi-track robustness rework: authored corner speeds + traffic + wall escape
- [36] DRIVER AI twisty-track pace: less conservative on Blue Ridge/Tokyo/Kyoto
- [34] DRIVER AI experimental racing line (opt-in, TD5RE_AI_DRIVER_LINE=1): +27-29%% twisty pace
- [305] AI un-wedges without driving off-track (steers to interior)
- [306] AI steers away when wedged vs car/player (no infinite grind)
- [393] MP catch-up paces off the next opponent ahead

## D. POLICE / COP CHASE


### D1 Single-player cop chase (10)

- [25] SP cop chase suspect count follows OPPONENTS (1-5), was hardcoded 1
- [26] Police chase: traffic is no longer treated as a suspect (chase arrow/points)
- [27] Police chase: siren starts AFTER the countdown (with the lights)
- [28] Police chase: COP<n> arrest strip removed from the top of the pane
- [120] Crashed cops recover and re-engage instead of staying wrecked
- [208] Police pullover: brakes hold all the way through (no coast gap)
- [244] Cops: tougher (300%); chasing cop shrugs off wall scrapes (no chase-end)
- [245] Cops: ease off for corners instead of flooring into the outside wall
- [246] Cops: pass a cluster and ALL of them chase you (no 1-cop limit)
- [294] Cops re-chase after you crash (was: 1 chase then never, esp. MP)

### D2 Split-screen MP cop chase (11)

- [277] MP cop chase: suspects can now pick TD6 cars, not just TD5
- [282] MP cop chase: bust arrow shows only when you crash a suspect
- [283] MP cop chase: other players' name labels hidden
- [284] MP cop chase: no 1st/2nd; top shows per-cop ARRESTS scoreboard
- [285] MP cop chase: arrest needs SIREN ON; else 'turn on siren' msg
- [286] MP cop chase: HORN toggles each cop's own siren
- [359] ARRESTED splash centred; its floating bar hidden
- [360] Arrested cars drop off the split-screen overview map
- [361] Strong short rumble for BOTH cop+suspect on arrest
- [362] All suspects show a bust bar; shrinks w/ distance, 2x range
- [400] MP cop-chase multi-cop + cop-only car select

### D3 Net cop chase (2 machines) (2)

- [17] Net cop chase activates all N players (was capped at 2)
- [19] Net cop chase activates all N players (was capped at 2)

## E. DRAG RACE


### E (10)

- [31] SP Drag Race picks ONE car (second car pick removed)
- [32] SP Drag Race options: OPPONENTS (sets lane count) + TRAFFIC
- [29] Drag traffic dry-up ROOT CAUSE fixed: wrecks now retired (SP + MP)
- [30] Drag traffic no longer starves after the first wave (SP + MP)
- [415] Drag START + FINISH banners read cleanly (were garbled/mirrored/doubled)
- [249] Drag MP mode: SELECT GAME MODE -> DRAG RACE (no track pick, no AI)
- [250] Drag MP options: TRAFFIC on/off, DISTANCE, EXTRA LANES
- [251] Drag LONG/EPIC: strip+stadium truly extend, finish moves down it
- [252] Drag race: strip widens to one lane per car (field-size lanes)
- [253] Drag race: tap left/right to CHANGE LANES (NFS-Underground feel)

## F. ARCADE POWER-UPS


### F1 Item boxes (placement, look, respawn) (12)

- [417] Arcade power-up boxes sit on the road (were spawning in the air on some tracks)
- [418] Arcade power-ups spawn in every drivable lane; you collect the one in your lane
- [270] ARCADE: item boxes spawn one lane closer to track centre
- [301] ARCADE: ~2x as many item boxes spawn along the track
- [308] ARCADE power-ups now floating boxes you drive through
- [309] Item box: glowing spinning pulsing cube w/ kind icon
- [310] Grabbed item box vanishes, respawns after 5 seconds
- [311] One power-up at a time; can't grab another until it ends
- [312] Box frequency scales with human player count (100-300)
- [313] 5+ players: chance of paired boxes (left + right shoulder)
- [316] Item boxes start after span 100, sit low near the road
- [317] Item boxes sit on road edges; steer over to grab one

### F2 Effects (durations, kinds, visuals) (10)

- [263] Arcade: new power-ups SHIELD/EMP-FREEZE/MAGNET/ROCKET/REPAIR (HAZARD off)
- [269] ARCADE: power-up timer bar shows the full duration (not last secs)
- [271] ARCADE: power-up effects last 20% longer (NITRO/GHOST/WRECK)
- [302] ARCADE: power-up effects last 2x longer (GHOST/WRECK/oil)
- [303] ARCADE NITRO: sustained 2.5x acceleration boost (~5s), not instant
- [307] ARCADE airborne launch dialled way down (was too high/unplayable)
- [314] HAZARD oil (3 lanes): hit it = drift uncontrollably ~2.5s
- [318] Car SILHOUETTE glows the effect colour (not a blob)
- [319] GHOST: car turns translucent, passes through traffic
- [320] Effect name shows centred below the checkpoint timer

### F3 Options / rows (2)

- [315] Game Options: POWER-UPS on/off toggle (persists)
- [321] Quick Race PHYSICS row toggles ARCADE/SIMULATION + fits OK/Back

## G. TRAFFIC, DAMAGE, TRAFFIC BATTLE


### G1 Traffic behaviour (3)

- [122] Wedged/stuck traffic frees itself instead of blocking the road
- [239] Traffic: visible ~60% further on open tracks (no short-range pop-out)
- [248] Traffic: stuck car no longer fades in front of you (gates on render dist)

### G2 Car damage system (7)

- [206] Damage bar: top-centre + pause-style blue-red fill
- [207] Damage bar on: checkpoint timer sits below it
- [227] Reset car: fully repairs (health+dents); un-sticks a knocked-out car
- [240] Damage Bar VISIBLE for every split-screen player (was invisible before)
- [242] Damage Bar toggle is global: every split-screen player gets it or none
- [264] Game Options: CAR TOUGHNESS + DEFORMATION levels (Low/Norm/High)
- [265] Car damage ON all races: dents+scuff, smoke, wreck, finish orbit cam

### G3 Traffic Battle (6)

- [68] TRAFFIC BATTLE: first-finisher 60s countdown ends the race
- [257] Traffic Battle: MP split-screen mode (below CUP) + solo (TD5RE_BATTLE=1)
- [258] Traffic Battle: WIN option MOST WRECKS or CHECKPOINTS (catch-up deadline)
- [260] Traffic Battle: crashes wreck at ANY angle/speed; airborne cars wreck too
- [261] Traffic Battle: wrecked cars go translucent + pass-through (drive thru)
- [262] Traffic Battle: live WRECKS HUD; results sort/label by WRECKS

## H. LOCAL SPLIT-SCREEN MP


### H1 Pause menu / END RACE NOW (6)

- [209] Pause menu: END RACE NOW only in local split-screen MP (not SP/net)
- [210] Pause menu: RESTART/QUIT/EXIT/LOBBY ask YES/NO confirm in split-screen MP
- [211] Pause menu (single-player): RESTART/QUIT/EXIT act immediately, no confirm
- [212] END RACE NOW: next race camera starts normal (was stuck shifted)
- [218] Pause menu: END RACE NOW force-finishes (YES/NO confirm; any player)
- [219] END RACE NOW: places ranked by current track progress, no DNFs

### H2 Lobby / profiles / car select / cup (16)

- [225] Split: one pad seen as 2 gamepads no longer joins as 2 players (cam bleed)
- [232] MP profile: typed-but-unsaved NEW name auto-saves when the race starts
- [267] MP car select: host X/TAB menu sets everyone's car (same/slow/avg/fast)
- [268] MP cup: player 1 keeps chosen car on race 2+ (no pre-cup car revert)
- [273] MP splitscreen: gold HOST tag on profile/screen/mode/car selectors
- [274] Split car-select 5+ players: big car + buttons|stats two columns; 7-9p car stays big
- [287] MP mode vote: A=vote -> profile-colour border ring on mode
- [288] MP mode vote: more voters = more nested rings; host decides
- [289] MP mode vote: arrow disappears once a player casts their vote
- [293] MP CUP podium: humans show profile colour, CPU shown in grey
- [298] MP profile DELETE asks 'DELETE <name>? A=YES B=NO' first
- [299] Profile delete removes ONLY the named/selected profile
- [300] MP setup: ARCADE/SIM selector only on track select (not mode opts)
- [363] CHOOSE YOUR TEAM / COP ROLES show profile names
- [364] Cup team mode: host assigns AI opponents' teams
- [290] Time Trial after a cup: no instant end / right car on Race Again / no rivals on your lane

### H3 In-race per-pane (11)

- [217] Split cup: later-race player can always accelerate (was camera-only)
- [226] Reset car (split-screen): resets in place, no teleport-to-start/wall
- [295] Split MP: reset car resets YOUR car, not the other player
- [296] Split MP: change-camera affects YOUR pane, not the other
- [297] Split MP: rear-view affects YOUR pane (positioned panes)
- [416] Split-screen top/bottom panes no longer zoomed in too far
- [398] Split-screen force feedback on the correct pad
- [402] Per-viewport engine audio pan for 3+ players
- [275] Tutorial overlay: Xbox pad diagram (gamepad only); ALL players press to start; every race
- [276] Tutorial overlay: TutorialOverlay=2 forces (bypass gamepad gate); =0 off; never in MP/replay/trace
- [229] Game Options: new TUTORIAL on/off row turns the overlay off

## I. NETPLAY (2 machines)


### I (2)

- [18] Net play protocol version gate + custom-track identity check
- [220] END RACE NOW: net race ends + shows results on all machines together

## J. GRAPHICS / RT / LIGHTING


### J1 Banners, fences, foliage (6)

- [46] Blue Ridge START banner reads clean (no garbled/mirrored text)
- [47] Keswick START banner reads clean (no garbled/mirrored text)
- [48] Thin alpha-tested fences (Keswick) draw at full distance again - no fade-out / near pop-in
- [49] Track chain-link fences / alpha-cutout signs no longer checkerboard at distance (SF fence)
- [50] Trees: no black borders around foliage sprite edges (Keswick)
- [37] Billboard trees render SOLID over houses behind them + no black top-edge line

### J2 RT / HIGH lighting (6)

- [51] RT wet-road reflection now SELECTIVE (whole ground no longer mirrors)
- [52] RT reflections ON by default (car chassis/glass reflect at night)
- [53] RT night speckle on headlight road + car smoothed (denoise 2->3)
- [54] Keswick tunnel: painted wall lamp patches now cast real cool-white light (RT)
- [55] RT: no first-frame freeze on slower GPUs (warms up on loading screen)
- [72] Sun-aligned car lighting + subtle sun glint (HIGH)

### J3 Lighting v2 + headlights (8)

- [202] Lighting v2: authored zone colours show (dusk/tunnel tint)
- [203] Lighting v2: headlights don't shine through walls (N.L)
- [204] Lighting v2: Mode=0 looks identical to the old build (A/B)
- [214] Headlights: per-pixel beams flood the road (deferred light pass)
- [215] Headlights auto-on in rain/overcast/dusk, off in bright daylight
- [216] Headlights: verify on a sunny track they stay OFF (Maui/Sydney)
- [23] LIGHTING OPTIONS: CAR LIGHTS row turns off car-emitted light (LOW + HIGH)
- [24] LIGHTING OPTIONS: LEGACY SHADOWS row swaps the soft car shadow for the flat quad

### J4 Camera / display (2)

- [266] Chase cam: no vertical jitter at race start on high-refresh (120/144+)
- [221] Alt+Enter fullscreen uses the display's native refresh, not 60Hz

## K. FRONTEND / OPTIONS / HUD


### K1 Menus and options screens (8)

- [21] Menu mouse double-click: no timeout + whole-button hit area
- [22] CONTROL OPTIONS: standard row spacing + PLAYER limited to connected controllers
- [33] RACE OPTIONS + NET CREATE screens now animate on entry
- [230] Game Options paginated (2 pages, < PREV / NEXT >); all rows fit, values/arrows OK
- [205] PLAYER NAME option: results row + high-score prefill
- [231] Car defaults to your PROFILE colour (TD6 exact / TD5 nearest); PAINT overrides
- [233] Celebrity leaderboard: unraced tracks show celeb names (not Frank/Ben)
- [234] Celebrity leaderboard: CelebrityNamesAPI=1 → fetches from randomuser.me

### K2 Car select / paint / stats (7)

- [222] Paint: SELECT CAR PAINT panel - secondary colour + pattern + 32 presets
- [223] Paint: Two-Tone/Stripes/Split show in BOTH menu preview and in-race body
- [323] MORE STATS: POWER captioned (engine output); WEIGHT under GRIP
- [324] MORE STATS BALANCE shows a visual front/rear weight split bar
- [325] Quick stat bars (under PAINT) match the MORE STATS scale
- [326] MORE STATS: Left/Right switches car in place (1P stats screen)
- [327] MORE STATS: captions under less-obvious bars; text clears RANDOM

### K3 HUD / minimap (2)

- [56] Minimap draws the whole lap on big circuits (no abrupt road cut-off)
- [322] Minimap: on a branch both fork tracks render (not just dots)

## L. AUDIO / INPUT


### L (9)

- [20] Car horn: smooth distance falloff + no longer detunes a nearby car's engine
- [391] Regular-car horn + TD6 character horns
- [235] Pause menu RADIO slider (row 2) sets radio volume live
- [236] Radio: quiet by default + mutes when minimized/paused/in menus
- [237] Radio: live station plays during a race (needs internet)
- [238] Music seam wired: no audio regression (jukebox/race/pause)
- [279] Controller: Y changes camera; horn moved to L3 (stick click)
- [372] Rumble survives many races (no FF death after first race)
- [389] WGI rumble backend for a 5th+ pad

## M. MISC (replay, lane assist, selftest)


### M (6)

- [79] View Replay: smooth playback at high FPS (was stuttering at 30 Hz)
- [281] Replay no longer counts as a race (MP cup keeps real results)
- [224] Lane Assist at a fork: picks one branch a few spans early, not the divider
- [255] Lane Assist: road-centre aid, firmer further off; avoids grass
- [256] Lane Assist toggle: SP Game Options / MP Profile screen / L key
- [57] Dev self-test suite reworked: deterministic damage detector

### Rows only in the tracked seed (add to round A3)
- [seed] Auto-track: only water under bridges (item 14)
- [seed] Auto-track: bridge guardrail gap + armco texture (item 17)
- [seed] Auto-track: flat overhead bridge geometry now solid (item 13)
