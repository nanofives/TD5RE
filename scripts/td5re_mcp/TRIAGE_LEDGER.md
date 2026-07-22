# pending_to_test.csv — burn-down triage ledger

Worker-account triage of every OPEN row (status `pending`/blank) in
`td5mod/src/td5re/pending_to_test.csv`, classified for the two-account
testing loop (see [TESTING_WORKFLOW.md](TESTING_WORKFLOW.md)). Regenerate
when the CSV grows substantially; otherwise update the affected rows in place.

**Triaged 2026-07-22 — 303 open rows.**

| Class | Count | Meaning |
|---|---|---|
| `auto` | 28 | assertable today through the control surface |
| `auto-blocked:<x>` | 106 | needs a missing verb / state field first |
| `manual` | 165 | hardware / audio / visual look-dev / 2-machine netplay |
| `covered-by-selftest` | 4 | already exercised — status flip only |

Manual dominates: the backlog is mostly render look-dev, audio, gamepad/FFB
and 2-machine netplay — none observable through the state/log surface.

## Grounding facts used

- `set_param` whitelist = 19 int knobs + `trace_fast_forward` (NO
  powerups/tutorial/toughness/deform/language knob).
- Race actions = `throttle brake handbrake horn gearup geardown camera
  rearview left right pause escape` (no reset / gear-position).
- `get_state` per-racer = position, lap, speed, damage_health+accum, is_cop,
  is_suspect, pursued, finished, finish_position, arcade_effect+frames.
  Race-level = num_racers, countdown, tutorial, wanted_mode, cop_actor,
  victory_position, sim_tick.
- `TD5RE_*` env knobs ARE usable (set via `os.environ` before launch).
- Track menu indices: Moscow 0, Newcastle 5, Courmayeur 7, Tokyo 9,
  Montego 17, TD6 base ≈26.

## Batch order & progress

(Line numbers below are the ORIGINAL triage-time numbers; the CSV has since
grown, so match rows by summary text, not line number.)

- **Cycle 1 (done, @131f9a86):** rows 267 (courmayeur_loads), 20+14
  (gpu_device_lost_recovery), 183 (arcade_only_sim), 191 (time_trial_midtrack)
  → tested. covered-by-selftest flipped: 320 (suite). Left pending on
  inspection (not actually covered): 28 (render goldens capture mid-race
  ticks, not the end wipe), 85/140 (partial / drifted cross-ref).
- **Cycle 2 (done, @aece09be):** 154 (cop_rubberband), 258+259+260
  (attract_demo), 170 (traffic_battle_solo) → tested. Left pending: 168/169
  (battle MP split-screen), 169-win (battle WIN conditions) — solo scenario
  only.
- **Cycle 3 (done, @<this commit>):** 5 (covered by gpu_device_lost_recovery),
  54 (split_drag), 81 (damage_toggle), 83 (tutorial_keyboard), 110
  (retired_screens), 111 (inputscript_drive) → tested.
- **Cycle 4 (done, @<this commit>):** 140 (tutorial_every_race — race.tutorial
  true at the start of 3 consecutive races), 153 (damage_no_wreckout —
  car_damage=0, sustained battering, health never <=0 / no knockout), 166
  (drag_length — new `race.drag_repeats` get_state field: two launches at
  TD5RE_DRAG_LENGTH_LEVEL=0 vs 3, SHORT repeats=0 < EPIC repeats=816, no
  driving needed) → tested. Remaining `auto`: 10 (per-player traffic cap —
  race.log has an `on_road` counter but no `eff_cap`/`clusters` token, so the
  cluster claim isn't cleanly log-observable — DEFERRED), 86 (arcade NITRO
  1.5x — stochastic pickup + multiplier), 158 (cop leash), 160 (drag MP,
  needs MP setup), 165 (drag longer — same as 166, now covered by drag_repeats).
  **Reclassified `auto` → `auto-blocked` (the `auto` call was optimistic — the
  claim isn't observable through the current control surface):**
  - **19** frame cap → needs a present-rate / present_count readout in
    get_state; race.log `fps` is a SIM-timing metric (norm_dt), not the render
    rate the cap paces.
  - **124** track-walker span warp → needs a per-slot span/progress-continuity
    readout (race_position is a rank, not a span) + the specific
    degenerate-quad track.
  - **247** heavy cars climb slower / **248** power-to-weight accel → need a
    car mass / accel(power-to-weight) field in get_state to pick "heavy" vs
    "light" cars; only the internal carstats screen has it today.

## get_state extensions (@<cycle-3 ext commit>) — added + outcomes

Added `present_count` (top level) and per-racer `span`, `heaviness` (Q8), and
`accel` (power-to-weight). Golden-safe (read-only accessors, never called in a
golden race; full selftest 51/51). Outcomes:

- **`present_count` → row 19 (frame cap) CLOSED.** `frame_cap.py` samples it
  over a wall interval; measured exactly 40/s at cap 40 (VSync off). This is
  the only client-observable render rate — race.log `fps` is sim-timing.
- **`accel` → row 248 (power-to-weight) CLOSED.** `car_accel.py`: two solo
  drag runs, higher-accel car reaches the target speed in fewer ticks
  (car0 accel1920=15t vs car8 accel1428=31t). Straight-line drag isolates
  acceleration (a circuit confounds it with corners; a multi-car drag field
  confounds it with inactive lanes).
- **`span` alone was NOT sufficient for row 124 (walker warp)** — folded span
  conflates the legit lap-wrap (resets at S/F line, lap counter lags a beat)
  and branch-corridor folding with a real warp. Closed via the wrap-counting
  approach below.

## get_state extensions (progress + climb) — added + outcomes

Added per-racer `progress` (monotonic `lap*ring + folded span`; folded span on
P2P/no-track) and `climb` (signed per-tick vertical rate, `>0` = uphill).
Golden-safe (read-only accessors, never called in a golden race; full selftest
51/51). Outcomes:

- **row 124 (walker warp) CLOSED** via `walker_no_warp.py`. The warp test is
  built on the folded `span` + wrap counting (lag-immune): span must climb
  smoothly and wrap to ~0 exactly once per lap; a real warp = an extra wrap or
  a mid-range backward jump. Newcastle, AI, 2 laps, 8x FF: ring~627, 2 wraps
  for 2 laps, 0 mid-range warps. NOTE: `progress` (lap*ring+span) momentarily
  dips ~one ring at each lap line because the game lap counter increments a
  tick or two AFTER the span folds — so `progress` is an end-of-race sanity
  value, not the warp signal.
- **row 247 (heavy climbs slower) CLOSED** via `heavy_climb.py`. `climb` marks
  uphill; two solo AI runs on Courmayeur (track 7, alpine) split `speed` by
  uphill/flat and compare the uphill/flat ratio. Heavier car retains less speed
  uphill (heaviness 341 → ratio 0.869 vs heaviness 256 → 0.944; stable across
  runs). The ratio normalises out raw top-speed differences.

Remaining get_state candidates that would unblock more rows:
- cop `speed`/`gap` (traffic slot) → the exact cop catch-up (154 detail).

## Scenario robustness

`_lib.Scenario.recover_if_broken(st, slot=0)` — long unattended AI runs tap R
(DIK_R 0x13, the recover key) when a tracked car's `damage_health` hits 0, so a
wreck never stalls sampling. Wired into `walker_no_warp` / `heavy_climb`; a
guarded no-op when damage isn't initialised. Reusable in any sampling loop.

## Full classified ledger

Reason shorthand: `vis`=visual look-dev, `aud`=audio, `hw`=pad/wheel/FFB/
display, `net`=2-machine netplay, `MP-vis`=split/MP layout.

| Line | Summary (~60c) | Class | Reason |
|---|---|---|---|
|2|Spanish es-AR localization + LANGUAGE screen|manual|vis translated menus; no language state|
|3|RACE OPTIONS single screen per mode|auto-blocked|menu-row/value introspection; + net|
|4|crash.log GPU draw-ring forensics|auto-blocked|reliable unrecovered-SEH injection|
|5|In-race HUD survives GPU reset no crash|auto|FORCE_DEVICE_LOST + survival + no crash.log|
|6|Split FFB follows own car after swap|manual|hw FFB output|
|7|RACE OPTIONS power-ups OFF/CASUAL/CHAOS|auto-blocked|powerups tier not a param/env|
|8|Shift up/down arrows on rev dial|auto-blocked|no gear/rpm state; + vis|
|9|Analog steering centre deadzone|manual|hw analog stick|
|10|Dynamic per-player traffic cap|auto|env + race.log clusters/eff_cap/on_road|
|11|Traffic Battle wrecks clear after 10s|auto-blocked|needs headless wreck generation|
|12|Crash screen-shake gate (UNVALIDATED)|manual|vis + real crash-heavy 5P|
|13|Chase cam wobble start of later races|manual|vis camera across races|
|14|Device-lost recovery no first-frame crash|auto|FORCE_DEVICE_LOST + RECOVERED log|
|15|View Replay cameras capped view dist|auto-blocked|no View-Replay verb; + vis|
|16|View Replay ends at end of recording|auto-blocked|View-Replay + END-RACE-NOW verbs|
|17|Dev pause menu END RACE NOW (SP)|auto-blocked|pause-menu action/introspection|
|18|View Replay trackside cams cut|auto-blocked|View-Replay verb; + vis|
|19|Frame-rate cap when VSync off|auto|TD5RE_FRAME_CAP + fps in log|
|20|GPU device-lost recovery no black screen|auto|FORCE_DEVICE_LOST + RECOVERED log|
|21|View Replay reproduces driven race|auto-blocked|View-Replay verb + pose compare|
|27|Montego countdown camera fly-in|manual|vis camera|
|28|Race-end wipe solid black|covered-selftest|render-golden row 24 catches wipe|
|29|Fences/foliage render both sides|manual|vis culling/texture|
|31|Crashed cops recover and re-engage|auto-blocked|needs cop crash generation|
|32|Traffic changes lanes smoothly|auto-blocked|no per-car lane field|
|33|Wedged traffic frees itself|auto-blocked|no stuck-traffic metric|
|34|End-of-race transition opaque|manual|vis at results screen|
|40|Street lamp glow heads render|manual|vis|
|41|Tutorial overlay mode-specific hint|manual|vis text|
|42|Pause confirm scales with DPI|manual|vis|
|43|Race-end black transition no bleed|manual|vis MP-vis|
|44|Cop Chase RANDOM COP gets police car|auto-blocked|cop-car-assignment field; MP|
|45|Joystick 1 CHANGE VIEW default|manual|hw|
|46|Tutorial overlay arrows shortened|manual|vis|
|47|Tutorial overlay mutes race audio|manual|aud|
|48|DEFORMATION Low/Normal/High/Off|auto-blocked|deform not a param; + vis|
|49|CAR TOUGHNESS Low/Med/High/Off|auto-blocked|toughness not settable|
|50|Reset-car recovery repairs 20%|auto-blocked|no reset-car action bit|
|51|2D trees/foliage no sky-blue tint|manual|vis|
|52|Split headlights follow own zone|manual|vis MP-vis|
|53|Bern START banner reads correctly|manual|vis|
|54|Split Drag launches drag strip|auto|start drag split + assert track/mode|
|55|Cop Chase MP sirens default ON|manual|aud/vis MP|
|56|Net RESTART re-rolls cop|manual|net|
|57|Net host picks GAME MODE|manual|net|
|58|Net CUP over network|manual|net|
|59|Net Cop Chase INFECT online|manual|net|
|60|Net Drag over network|manual|net|
|61|Net Traffic Battle over network|manual|net|
|62|Net Cop Chase over network|manual|net|
|63|Net ARCADE dynamics online|manual|net|
|64|Net remote player names in results|manual|net|
|65|Lane Assist no sidewalk on TD6|auto-blocked|no lane-target field; + vis|
|66|Auto headlights Bern tunnels|manual|vis; no headlight state|
|67|Auto headlights dark vs bright|manual|vis (mechanism tested row 22)|
|68|Car bodies stay neutral no tint|manual|vis|
|69|Track select RACE OPTIONS gathers|auto-blocked|menu introspection|
|70|RACE OPTIONS adds checkpoint/deform rows|auto-blocked|menu introspection|
|71|Track select LAPS circuit vs P2P|auto-blocked|menu-row introspection|
|72|2D trees/foliage smooth AA edges|manual|vis|
|73|Overhead signs not garbled/mirrored|manual|vis|
|74|High Scores COLLISIONS+AIR TIME cols|auto-blocked|high-score screen data field|
|75|High Scores celebrity names full|manual|vis text|
|76|High Scores default times 5-10min|manual|vis; HS data|
|77|High Scores SP cups not in browse|auto-blocked|HS list introspection|
|78|High Scores split MP by profile|manual|vis MP|
|79|Race-start cam no water swing Montego|manual|vis camera|
|80|Game Options PLAYER NAME inline edit|manual|vis; text-entry|
|81|Game Options DAMAGE toggle ON/OFF|auto|car_damage param; health drops iff on|
|82|Game Options PREV/OK/NEXT aligned|manual|vis layout|
|83|TUTORIAL=ON shows overlay on keyboard|auto|race.tutorial true on keyboard launch|
|84|PENDING ENTER opens details modal|auto-blocked|no modal/menu-selection state|
|85|PENDING crossing-out gone; SUPR clears|covered-selftest|tested row 316|
|86|Arcade NITRO trail + 1.5x top speed|auto|arcade_effect kind + speed while active|
|87|Arcade GHOST blocks + ends cop chase|auto-blocked|effect-id map + pickup timing|
|88|Arcade INDESTRUCTIBLE no self-damage|auto-blocked|pickup+collision timing|
|89|Arcade SHIELD/ROCKET removed|auto-blocked|prove-a-negative pool introspection|
|90|Arcade FREEZE ram slows victim 1/3|auto-blocked|effect + victim-speed; pickup|
|91|Arcade MAGNET only Traffic Battle|auto-blocked|per-mode effect-pool introspection|
|92|Arcade REPAIR repairs car (gated)|auto-blocked|effect + pickup timing|
|93|Arcade POWER-UPS OFF/CASUAL/CHAOS|auto-blocked|powerups tier not settable|
|94|Cup CHOOSE YOUR TEAM roster box|manual|vis MP|
|95|MP mode select vs HOST banner|manual|vis MP|
|96|MP mode select TIME TRIAL gone|auto-blocked|menu introspection; MP|
|97|MP profile NAME+COLOUR centred|manual|vis MP|
|98|Street lamps experimental pools|manual|vis look-dev|
|99|Shadows no duplicated echo|manual|vis|
|100|Reflections wet road mirrors|manual|vis|
|101|Reflections car paint sheen|manual|vis|
|102|Reflections no smearing|manual|vis|
|103|Reflections=0 removes all|manual|vis|
|104|Shadows cars/walls on road|manual|vis|
|105|Shadows no acne/flicker|manual|vis|
|106|Shadows tunnels no sun shadow|manual|vis|
|107|Shadows headlight blocked by car|manual|vis|
|108|SunShadows=0 removes all|manual|vis|
|109|UI GUIDE changelog widgets + modal|manual|vis|
|110|Retired StartScreen=3/4 to main menu|auto|set_screen + screen-index assert|
|111|InputScript scripted drive works|auto|hold_action drive → speed>0|
|112|StartScreen jumped == navigated|manual|vis|
|113|Lighting v2 authored zone colours|manual|vis|
|114|Lighting v2 headlights not thru walls|manual|vis|
|115|Lighting v2 Mode=0 identical A/B|manual|vis|
|116|PLAYER NAME results row + HS prefill|auto-blocked|name-in-results field; + vis|
|117|Damage bar top-centre blue-red|manual|vis|
|118|Damage bar checkpoint timer below|manual|vis layout|
|119|Police pullover brakes hold|auto-blocked|pullover trigger/state|
|120|Pause END RACE NOW local split MP|auto-blocked|pause-menu introspection|
|121|Pause RESTART/QUIT confirm split|auto-blocked|pause-menu introspection|
|122|Pause SP immediate no confirm|auto-blocked|pause-menu introspection|
|123|END RACE NOW next race cam normal|manual|vis; + verb|
|124|Track walker no span warp|auto|position-continuity over an AI lap|
|125|Headlights per-pixel beams|manual|vis|
|126|Headlights auto-on rain/dusk|manual|vis|
|127|Headlights sunny stay OFF|manual|vis|
|128|Split cup later-race can accelerate|auto-blocked|cup flow + split|
|129|Pause END RACE NOW any player|auto-blocked|pause-menu verb|
|130|END RACE NOW ranks by progress|auto-blocked|end-race-now verb + results|
|131|END RACE NOW net ends all machines|manual|net|
|132|Alt+Enter fullscreen refresh|manual|hw/display|
|133|Paint SELECT CAR PAINT panel|manual|vis|
|134|Paint patterns preview + in-race|manual|vis|
|135|Lane Assist fork picks branch|auto-blocked|lane-target field; + vis|
|136|Split one pad seen as 2 gamepads|manual|hw|
|137|Reset car split resets in place|auto-blocked|reset-car action bit|
|138|Reset car fully repairs+unsticks|auto-blocked|reset-car action bit|
|139|Tutorial overlay START of every race|auto|race.tutorial true across N races|
|140|Game Options TUTORIAL off|covered-selftest|tested row 25|
|141|Game Options paginated 2 pages|manual|vis; menu introspection|
|142|Car defaults to profile colour|manual|vis|
|143|MP typed-unsaved name auto-saves|auto-blocked|profile-save state; MP|
|144|Celebrity leaderboard unraced names|manual|vis text|
|145|Celebrity leaderboard API fetch|manual|external network|
|146|Pause RADIO slider live volume|manual|aud; + pause-menu|
|147|Radio quiet default + mutes|manual|aud|
|148|Radio live station plays|manual|aud + network|
|149|Music seam no audio regression|manual|aud|
|150|Traffic visible ~60% further|manual|vis draw-distance|
|151|Damage Bar every split player|manual|vis MP-vis|
|152|Damage Bar OFF no wreck-out|auto|car_damage=0 → no knockout|
|153|Damage Bar toggle global|manual|vis MP-vis|
|154|Cops accel rubber-band out-drags|auto|headless recipe; position gap closes|
|155|Cops tougher 300% shrug scrapes|auto-blocked|needs cop crash generation|
|156|Cops ease off for corners|auto-blocked|cop-throttle telemetry|
|157|Cops cluster ALL chase|auto-blocked|multi-cop pursuit field|
|158|Cops longer leash 110 spans|auto|pursued persists across gap|
|159|Traffic stuck car no fade in front|auto-blocked|traffic fade/render-dist field|
|160|Drag MP no track pick no AI|auto|start drag MP; num_racers==players|
|161|Drag MP options TRAFFIC/DISTANCE|auto-blocked|menu introspection|
|162|Drag LONG/EPIC strip extends|auto-blocked|strip-length metric|
|163|Drag strip widens per car|auto-blocked|lane-count field; vis|
|164|Drag tap left/right lanes|auto-blocked|no lateral-position field|
|165|Drag longer via TD5RE_DRAG_LENGTH|auto|env + finish span/tick longer|
|166|Lane Assist road-centre aid|manual|feel/vis|
|167|Lane Assist toggle 3 places|auto-blocked|source introspection; + hw|
|168|Traffic Battle MP + solo env|auto|env + assert battle composition|
|169|Traffic Battle WIN MOST WRECKS|auto-blocked|win-condition + wreck-count field|
|170|Traffic Battle oncoming no rivals|auto|env + num_racers/no cops + start pos|
|171|Traffic Battle crashes wreck|auto-blocked|wreck-count field; needs collisions|
|172|Traffic Battle wrecked translucent|manual|vis|
|173|Traffic Battle live WRECKS HUD|manual|vis; wreck-score field|
|174|Arcade new power-ups SHIELD/EMP|auto-blocked|effect-kind enumeration|
|175|Game Options TOUGHNESS+DEFORM|auto-blocked|not settable; + vis|
|176|Car damage ON full chain|manual|vis smoke/dents/orbit|
|177|Chase cam no jitter high-refresh|manual|vis + display|
|178|MP car select host sets everyone|auto-blocked|car-assignment field; MP|
|179|MP cup player1 keeps car race2+|auto-blocked|cup flow + car field|
|180|Arcade power-up timer bar|manual|vis (frames is data, bar is vis)|
|181|Arcade boxes one lane centre|manual|vis|
|182|Arcade effects 20% longer|auto-blocked|duration baseline comparison|
|183|Power-ups ARCADE-only SIM none|auto|dynamics=1 → arcade_effect stays 0|
|184|MP splitscreen gold HOST tag|manual|vis MP|
|185|Split car-select 5+ players|manual|vis MP|
|186|Tutorial overlay Xbox pad diagram|manual|vis + hw|
|187|TutorialOverlay=2 forces / =0 off|auto-blocked|setting not in whitelist|
|188|MP cop chase suspects TD6 cars|auto-blocked|car-select introspection; MP|
|190|Controller Y camera; horn L3|manual|hw|
|191|Time trial city mid-track no insta-end|auto|TT + game_state stays RACE|
|192|Replay not counted as race|auto-blocked|replay flow + cup standings|
|193|MP cop chase bust arrow|manual|vis MP|
|194|MP cop chase names hidden|manual|vis MP|
|195|MP cop chase ARRESTS scoreboard|manual|vis MP|
|196|MP cop chase arrest needs siren|manual|vis/aud MP|
|197|MP cop chase HORN toggles siren|manual|aud MP|
|198|MP mode vote colour ring|manual|vis MP|
|199|MP mode vote nested rings|manual|vis MP|
|200|MP mode vote arrow disappears|manual|vis MP|
|201|Time Trial after cup right car|auto-blocked|cup flow; MP|
|202|Snow Bern easier grip|manual|feel/vis|
|203|Snow grip boost OFF dry|manual|feel (sim → golden covers)|
|204|MP CUP podium human vs CPU grey|manual|vis MP|
|205|Cops re-chase after crash|auto-blocked|cop re-chase + needs crash|
|206|Split MP reset car|auto-blocked|reset-car action; split|
|207|Split MP change-camera pane|manual|vis split|
|208|Split MP rear-view pane|manual|vis split|
|209|MP profile DELETE confirm|manual|vis MP|
|210|Profile delete only named|auto-blocked|profile-list introspection; MP|
|211|MP setup ARCADE/SIM track select|auto-blocked|menu introspection; MP|
|212|Arcade ~2x item boxes|manual|vis; box-count metric|
|213|Arcade effects 2x longer|auto-blocked|duration baseline|
|214|Arcade NITRO 2.5x accel ~5s|auto-blocked|effect + accel slope; pickup|
|215|Collisions hug silhouette hull|manual|vis/physics|
|216|AI un-wedges without off-track|auto-blocked|wedge state; + vis|
|217|AI steers away when wedged|auto-blocked|wedge state; + vis|
|218|Arcade airborne launch dialled down|manual|vis|
|219|Arcade boxes floating drive-through|manual|vis|
|220|Item box glowing spinning cube|manual|vis|
|221|Grabbed box vanishes respawns 5s|auto-blocked|no item-box state field|
|222|One power-up at a time|auto-blocked|pickup timing|
|223|Box frequency scales w/ humans|auto-blocked|box-spawn metric|
|224|5+ players paired boxes|manual|vis MP|
|225|HAZARD oil 3 lanes drift|auto-blocked|effect; + vis|
|226|Game Options POWER-UPS persists|auto-blocked|powerups param + persistence|
|227|Item boxes start after span 100|auto-blocked|box-position field|
|228|Item boxes on road edges|manual|vis|
|229|Car silhouette glows effect colour|manual|vis|
|230|GHOST translucent thru traffic|auto-blocked|effect + pickup; + vis|
|231|Effect name centred below timer|manual|vis|
|232|Quick Race PHYSICS row ARCADE/SIM|auto-blocked|menu introspection|
|233|Minimap branch both forks render|manual|vis|
|234|MORE STATS POWER caption|manual|vis|
|235|MORE STATS BALANCE bar|manual|vis|
|236|Quick stat bars match MORE STATS|manual|vis|
|237|MORE STATS L/R switches car|auto-blocked|stats-screen state; + vis|
|238|MORE STATS captions on RANDOM|manual|vis|
|239|Menu nav UP/DOWN visual order|auto-blocked|no menu-selection-index field|
|240|Menu nav L/R OK<->BACK gamepad|manual|hw; + selection field|
|241|Menu nav selectors cycle value|auto-blocked|menu-value field|
|242|Track Select DYNAMICS arrows|manual|vis|
|243|Track Select rows fit no overlap|manual|vis|
|244|Cop Chase RANDOM COP at start|auto-blocked|cop-assignment; MP|
|245|MORE STATS real carparam physics|manual|vis; stat field|
|246|Heavy cars hit harder/shove light|auto-blocked|collision-outcome setup|
|247|Heavy cars climb hills slower|auto|two cars, speed on same climb|
|248|Power-to-weight lighter out-accel|auto|two cars, accel slope compare|
|249|Slipstream boost behind car|auto-blocked|slipstream state; positioning|
|250|Downforce grippy cars planted|manual|feel; + setup|
|251|ARCADE collisions punchier|manual|feel/vis|
|252|ARCADE pads grant NITRO/GHOST|auto-blocked|effect-kind enumeration|
|253|Power-up chip+timer each pane|manual|vis split|
|254|GHOST cars pass through|auto-blocked|effect + pickup; + vis|
|255|HAZARD spins out next car|auto-blocked|effect + victim; + vis|
|256|SIMULATION realistic gravity|manual|feel/vis|
|257|ARCADE/SIM on track+MP screens|auto-blocked|menu introspection|
|258|Demo fires after 1min idle|auto|idle main menu → attract RACE|
|259|Demo random track no cops|auto|demo → no cop_actor + traffic|
|260|Demo returns to menu no results|auto|demo → MENU, never results|
|261|INFECT arrested becomes cop|auto-blocked|infect role-flip; MP|
|262|INFECT human keeps driving as cop|auto-blocked|MP|
|263|INFECT round ends all infected|auto-blocked|MP|
|264|Collisions fire on mesh contact|manual|physics/vis|
|265|Cop Chase results COPS/SUSPECTS|manual|vis MP|
|266|Cop Chase ends all busted/finish|auto-blocked|end-condition; MP|
|267|Italy (Courmayeur) starts no crash|auto|track 7 → RACE, ticks, survives|
|268|Cop roles OK rejected all-cop|auto-blocked|role-assign screen; MP|
|269|Arrested car stops dead|auto-blocked|arrest state; MP|
|270|ARRESTED splash centred|manual|vis MP|
|271|Arrested cars drop off map|manual|vis MP|
|272|Strong rumble on arrest|manual|hw|
|273|All suspects show bust bar|manual|vis MP|
|274|TEAM/COP ROLES profile names|manual|vis MP|
|275|Cup team host assigns AI|auto-blocked|cup-team setup; MP|
|276|Cup per-opponent skill slider|auto-blocked|MP; AI-pace inference|
|277|Dev host assigns teams to bots|auto-blocked|MP|
|278|Cup final standings names|manual|vis MP|
|279|WHAT NEXT? CAR SELECTION grid|auto-blocked|menu-flow introspection|
|280|WHAT NEXT? buttons aligned|manual|vis|
|281|Crisp on 4K/high-DPI|manual|vis + display|
|282|Menu UP/LEFT with 5+ pads|manual|hw|
|283|Rumble survives many races|manual|hw|
|284|Pad sleep/reconnect re-rumbles|manual|hw|
|285|MP results table alignment|manual|vis MP|
|286|Cup results POINTS column|manual|vis; standings field|
|287|7-player empty cells MAP|manual|vis MP|
|288|Empty-cell map zoom smooth|manual|vis MP|
|289|Split overview map no diagonal|manual|vis MP|
|290|CUP WHAT NEXT? title/buttons|manual|vis MP|
|291|7th+ split car sits flat|manual|vis MP|
|292|Cup picker per-race track/police|auto-blocked|cup-picker introspection; MP|
|293|Cup AI OPPONENTS score points|auto-blocked|cup standings; MP|
|294|Cup ends when all humans finish|auto-blocked|cup flow (finished per slot)|
|295|Dev ADD AI PLAYER bots lobby|auto-blocked|MP lobby verb|
|296|CHANGELOG bullets/spacing|manual|vis (tested 314)|
|297|MP split post-race menu|manual|vis MP|
|298|Police chase durable/dodges|manual|vis; + AB|
|299|Car-select HANDLING bar|manual|vis; stat field|
|300|WGI rumble 5th+ pad|manual|hw|
|301|Split back-confirm every screen|manual|vis MP|
|302|Regular horn + TD6 horns|manual|aud|
|303|Short controller names lobby|manual|vis/hw MP|
|304|MP catch-up paces off opponent|auto-blocked|catch-up telemetry; MP|
|305|Traffic lighter/fills gaps|auto-blocked|traffic on_road field; + vis|
|306|Controller slots stable|manual|hw|
|307|MP opponent labels below wheels|manual|vis MP|
|308|Reset SELECT / VIEW Y / horn L3|manual|hw|
|309|Split FFB correct pad|manual|hw split|
|310|Manual-only stuck recovery cooldown|auto-blocked|reset-car action + cooldown|
|311|MP cop-chase multi-cop select|auto-blocked|MP|
|312|Split empty-cell map zoom|manual|vis MP|
|313|Per-viewport engine audio pan|manual|aud split|
|320|Live-control v2 verbs + suite|covered-selftest|run_all.py suite exercises it|
|321|Golden harness pin GameOptions|auto-blocked|dev-infra /fix item, not a behaviour test|
|322|rgold-pelton t130 CSV sensitivity|auto-blocked|dev-infra investigation, not a behaviour test|
