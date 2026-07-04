/**
 * td5_pending.c -- Dev/QA "pending to test" checklist + in-game overlay.
 *
 * See td5_pending.h. Backing file: td5re_pending.txt next to the exe (untracked,
 * gitignored). Line format:
 *     # comment
 *     item still to test
 *     #- retired item        <- deleted (SUPR); never shown again and never
 *                                re-seeded from k_seed[]
 *
 * [2026-07-04] Items no longer track a "tested"/"done" state -- there is no
 * more crossing-out. ENTER on a row instead opens a details modal
 * (td5_pending_detail_text, backed by td5_pending_detail.h's k_pending_detail[]
 * table) with a longer, testing-focused note. SUPR/DELETE is the only way to
 * clear an item, once it's actually been verified. Old files from before this
 * change may still have "[ ] "/"[x] " prefixes; pending_load_file strips
 * either and treats the item as a normal (untracked-state) row.
 *
 * On launch td5_pending_init() MERGES any k_seed[] entry not already present
 * (and not retired) into the loaded file, newest-first — so a freshly-shipped
 * feature surfaces in the checklist even on a machine whose td5re_pending.txt
 * already exists. Before this the file was frozen after its first run, so every
 * item shipped later stayed invisible in the in-game list. Deleted items are
 * tombstoned (#- ) so the merge doesn't bring them back.
 */
#include "td5_pending.h"
#include "td5_platform.h"
#include "td5re.h"

#include <string.h>
#include <stdio.h>

#define LOG_TAG "pending"

#define PENDING_MAX       256   /* > K_SEED_COUNT so no shipped item is dropped */
#define PENDING_TEXT_MAX  120

typedef struct { char text[PENDING_TEXT_MAX]; } PendingItem;

static PendingItem s_items[PENDING_MAX];
static int s_count      = 0;
static int s_overlay_on = 0;

/* Tombstones: items the user tested-and-cleared (/end retires [x] rows) or
 * deleted (SUPR). Persisted as "#- text" lines so the launch-time seed merge
 * never resurrects them. */
static char s_retired[PENDING_MAX][PENDING_TEXT_MAX];
static int  s_retired_count = 0;

static int retired_contains(const char *text) {
    int i;
    if (!text) return 0;
    for (i = 0; i < s_retired_count; i++)
        if (strcmp(s_retired[i], text) == 0) return 1;
    return 0;
}

static void retired_add(const char *text) {
    if (s_retired_count >= PENDING_MAX || !text) return;
    while (*text == ' ' || *text == '\t') text++;
    if (!*text || retired_contains(text)) return;
    strncpy(s_retired[s_retired_count], text, PENDING_TEXT_MAX - 1);
    s_retired[s_retired_count][PENDING_TEXT_MAX - 1] = '\0';
    s_retired_count++;
}

/* Compiled-in seed list — used ONLY when no file exists yet (first run on a
 * machine). Keep each concise: these double as the right-justified overlay
 * lines. New work shipped later should be added here (and folded into the
 * changelog) so a fresh checkout seeds the current testing backlog. */
static const char *const k_seed[] = {
    "Tutorial overlay: adds a mode-specific hint line",
    "Pause confirm popup (END RACE NOW etc.) scales with resolution/DPI",
    "Race-end black transition: HUD text no longer bleeds through (split-screen)",
    "Cop Chase RANDOM COP: the drawn cop now gets a police car, not their pick",
    "Joystick 1's CHANGE VIEW button now matches other joysticks' default",
    "Tutorial overlay: BRAKE/ACCEL/RESET CAR/PAUSE/REAR VIEW arrows shortened",
    "Tutorial overlay: race audio (SFX+music) mutes while it's on screen",
    "Net play: host picks GAME MODE in the network lobby (Race/Drag/Battle/Cop)",
    "Net play: CUP over the network (series advance, points, standings, auto-teams)",
    "Net play: Cop Chase INFECT online + COPS/SUSPECTS results table over net",
    "Net play: Drag Race over the network (lanes=players, no AI, deterministic)",
    "Net play: Traffic Battle over the network (power-up boxes in sync on all PCs)",
    "Net play: Cop Chase over the network (random human cop; bust/arrest in sync)",
    "Net play: ARCADE dynamics online (power-up boxes + 3x collisions in sync)",
    "Net play: remote players show their names in results/standings (not P2/P3)",
    "Auto headlights: ON in Bern tunnels, OFF in Bern sunlit stretches",
    "Auto headlights: ON across dark-sky tracks (Moscow), OFF on bright tracks",
    "Car bodies stay neutral (no red/colour tint from track lighting, e.g. Bern)",
    "Track select: RACE OPTIONS screen gathers all race options in one place",
    "RACE OPTIONS screen: adds checkpoint timers/power-ups/toughness/deform",
    "Track select: LAPS shows on TD5 circuit tracks (Newcastle etc.), not P2P",
    "2D trees/foliage: smooth anti-aliased edges (Switzerland, Blue Ridge, etc.)",
    "Banners: overhead track signs (Tokyo STAGE, etc.) not garbled/mirrored",
    "High Scores: COLLISIONS + AIR TIME columns match race results",
    "High Scores: celebrity names show in full (Michael Schumacher)",
    "High Scores: default best times all read 5-10 min (unraced tracks)",
    "High Scores: single-player CUPS no longer in the browse list",
    "High Scores: split-screen MP records every player by profile name",
    "Game Options: PLAYER NAME edits INLINE in place (no pop-up), full-size font",
    "Game Options: DAMAGE toggle ON=car damage+bar, OFF=both off (label 'DAMAGE')",
    "Game Options: PREV/OK/NEXT aligned under the option-row column (120/220/320)",
    "Tutorial: TUTORIAL=ON now shows the overlay on keyboard too (was pad-only)",
    "PENDING TO TEST: ENTER opens a details modal (no more mark-tested)",
    "PENDING TO TEST: crossing-out is gone; SUPR/DELETE still clears an item",
    "Arcade NITRO: glowing speed trail + 1.5x top speed while active",
    "Arcade GHOST: blocks new + ends an active cop chase",
    "Arcade INDESTRUCTIBLE: no self-damage/speed loss from traffic or racers (walls unaffected)",
    "Arcade SHIELD and ROCKET power-ups removed",
    "Arcade FREEZE: ram slows victim to 1/3, glows, recovers over 3s",
    "Arcade MAGNET: only spawns in Traffic Battle mode",
    "Arcade REPAIR: actually repairs the car; damage-gated",
    "Arcade POWER-UPS option: OFF / CASUAL / CHAOS (one per lane)",
    "Cup CHOOSE YOUR TEAM: roster box (top right) lists members by name",
    "MP mode select: buttons no longer overlap HOST banner text",
    "MP mode select: TIME TRIAL entry is gone, other modes unaffected",
    "MP profile screen: NAME + COLOUR button labels now centred",
    "Street lamps (EXPERIMENTAL, StreetLights=1 to enable): pools under posts",
    "Shadows: no duplicated/offset echo copies (fence area, Moscow)",
    "Reflections P3: wet road mirrors cars/buildings on rainy Moscow",
    "Reflections P3: car paint/glass sheen at grazing angles",
    "Reflections P3: no smearing/ghosting artifacts while driving",
    "Reflections P3: Reflections=0 removes all of it (A/B)",
    "Shadows P2: cars/walls cast shadows on the road (drive daylight)",
    "Shadows P2: no shadow acne/flicker on the road while driving",
    "Shadows P2: tunnels/ambient zones cast NO sun shadow",
    "Shadows P2: headlight beam blocked by the car in front",
    "Shadows P2: SunShadows=0 removes all of it (A/B)",
    "Lighting v2: authored zone colours show (dusk/tunnel tint)",
    "Lighting v2: headlights don't shine through walls (N.L)",
    "Lighting v2: Mode=0 looks identical to the old build (A/B)",
    "PLAYER NAME option: results row + high-score prefill",
    "Damage bar: top-centre + pause-style blue-red fill",
    "Damage bar on: checkpoint timer sits below it",
    "Police pullover: brakes hold all the way through (no coast gap)",
    "Pause menu: END RACE NOW only in local split-screen MP (not SP/net)",
    "Pause menu: RESTART/QUIT/EXIT/LOBBY ask YES/NO confirm in split-screen MP",
    "Pause menu (single-player): RESTART/QUIT/EXIT act immediately, no confirm",
    "END RACE NOW: next race camera starts normal (was stuck shifted)",
    "Track walker: no span warp on degenerate quads (race normally)",
    "Headlights: per-pixel beams flood the road (deferred light pass)",
    "Headlights auto-on in rain/overcast/dusk, off in bright daylight",
    "Headlights: verify on a sunny track they stay OFF (Maui/Sydney)",
    "Split cup: later-race player can always accelerate (was camera-only)",
    "Pause menu: END RACE NOW force-finishes (YES/NO confirm; any player)",
    "END RACE NOW: places ranked by current track progress, no DNFs",
    "END RACE NOW: net race ends + shows results on all machines together",
    "Alt+Enter fullscreen uses the display's native refresh, not 60Hz",
    "Paint: SELECT CAR PAINT panel - secondary colour + pattern + 32 presets",
    "Paint: Two-Tone/Stripes/Split show in BOTH menu preview and in-race body",
    "Lane Assist at a fork: picks one branch a few spans early, not the divider",
    "Split: one pad seen as 2 gamepads no longer joins as 2 players (cam bleed)",
    "Reset car (split-screen): resets in place, no teleport-to-start/wall",
    "Reset car: fully repairs (health+dents); un-sticks a knocked-out car",
    "Tutorial overlay now shows at the START of every race (was once-ever)",
    "Game Options: new TUTORIAL on/off row turns the overlay off",
    "Game Options paginated (2 pages, < PREV / NEXT >); all rows fit, values/arrows OK",
    "Car defaults to your PROFILE colour (TD6 exact / TD5 nearest); PAINT overrides",
    "MP profile: typed-but-unsaved NEW name auto-saves when the race starts",
    "Celebrity leaderboard: unraced tracks show celeb names (not Frank/Ben)",
    "Celebrity leaderboard: CelebrityNamesAPI=1 → fetches from randomuser.me",
    "Pause menu RADIO slider (row 2) sets radio volume live",
    "Radio: quiet by default + mutes when minimized/paused/in menus",
    "Radio: live station plays during a race (needs internet)",
    "Music seam wired: no audio regression (jukebox/race/pause)",
    "Traffic: visible ~60% further on open tracks (no short-range pop-out)",
    "Damage Bar VISIBLE for every split-screen player (was invisible before)",
    "Damage Bar OFF (Game Options): no top bar, no wreck-out; dents stay",
    "Damage Bar toggle is global: every split-screen player gets it or none",
    "Cops: accel rubber-band out-drags a faster car on straights (catch up)",
    "Cops: tougher (300%); chasing cop shrugs off wall scrapes (no chase-end)",
    "Cops: ease off for corners instead of flooring into the outside wall",
    "Cops: pass a cluster and ALL of them chase you (no 1-cop limit)",
    "Cops: longer chase leash (110 spans) + 200% catch-up; harder to shake",
    "Traffic: stuck car no longer fades in front of you (gates on render dist)",
    "Drag MP mode: SELECT GAME MODE -> DRAG RACE (no track pick, no AI)",
    "Drag MP options: TRAFFIC on/off, DISTANCE, EXTRA LANES",
    "Drag LONG/EPIC: strip+stadium truly extend, finish moves down it",
    "Drag race: strip widens to one lane per car (field-size lanes)",
    "Drag race: tap left/right to CHANGE LANES (NFS-Underground feel)",
    "Drag race: longer race via TD5RE_DRAG_LENGTH (existing road reused)",
    "Lane Assist: road-centre aid, firmer further off; avoids grass",
    "Lane Assist toggle: SP Game Options / MP Profile screen / L key",
    "Traffic Battle: MP split-screen mode (below CUP) + solo (TD5RE_BATTLE=1)",
    "Traffic Battle: WIN option MOST WRECKS or CHECKPOINTS (catch-up deadline)",
    "Traffic Battle: oncoming traffic, no rivals/cops, spawn mid-track",
    "Traffic Battle: crashes wreck at ANY angle/speed; airborne cars wreck too",
    "Traffic Battle: wrecked cars go translucent + pass-through (drive thru)",
    "Traffic Battle: live WRECKS HUD; results sort/label by WRECKS",
    "Arcade: new power-ups SHIELD/EMP-FREEZE/MAGNET/ROCKET/REPAIR (HAZARD off)",
    "Game Options: CAR TOUGHNESS + DEFORMATION levels (Low/Norm/High)",
    "Car damage ON all races: dents+scuff, smoke, wreck, finish orbit cam",
    "Chase cam: no vertical jitter at race start on high-refresh (120/144+)",
    "MP car select: host X/TAB menu sets everyone's car (same/slow/avg/fast)",
    "MP cup: player 1 keeps chosen car on race 2+ (no pre-cup car revert)",
    "ARCADE: power-up timer bar shows the full duration (not last secs)",
    "ARCADE: item boxes spawn one lane closer to track centre",
    "ARCADE: power-up effects last 20% longer (NITRO/GHOST/WRECK)",
    "Power-ups now ARCADE-only: SIMULATION shows no item boxes",
    "MP splitscreen: gold HOST tag on profile/screen/mode/car selectors",
    "Split car-select 5+ players: big car + buttons|stats two columns; 7-9p car stays big",
    "Tutorial overlay: Xbox pad diagram (gamepad only); ALL players press to start; every race",
    "Tutorial overlay: TutorialOverlay=2 forces (bypass gamepad gate); =0 off; never in MP/replay/trace",
    "MP cop chase: suspects can now pick TD6 cars, not just TD5",
    "Reverse direction now works on the 7 lap circuits (Newcastle etc)",
    "Controller: Y changes camera; horn moved to L3 (stick click)",
    "Time trial on city tracks (HK/London): mid-track start no insta-end",
    "Replay no longer counts as a race (MP cup keeps real results)",
    "MP cop chase: bust arrow shows only when you crash a suspect",
    "MP cop chase: other players' name labels hidden",
    "MP cop chase: no 1st/2nd; top shows per-cop ARRESTS scoreboard",
    "MP cop chase: arrest needs SIREN ON; else 'turn on siren' msg",
    "MP cop chase: HORN toggles each cop's own siren",
    "MP mode vote: A=vote -> profile-colour border ring on mode",
    "MP mode vote: more voters = more nested rings; host decides",
    "MP mode vote: arrow disappears once a player casts their vote",
    "Time Trial after a cup: no instant end / right car on Race Again / no rivals on your lane",
    "Snow tracks (Bern) easier: ice surface grips more, less slide",
    "Snow grip boost OFF on dry tracks (tarmac feel unchanged)",
    "MP CUP podium: humans show profile colour, CPU shown in grey",
    "Cops re-chase after you crash (was: 1 chase then never, esp. MP)",
    "Split MP: reset car resets YOUR car, not the other player",
    "Split MP: change-camera affects YOUR pane, not the other",
    "Split MP: rear-view affects YOUR pane (positioned panes)",
    "MP profile DELETE asks 'DELETE <name>? A=YES B=NO' first",
    "Profile delete removes ONLY the named/selected profile",
    "MP setup: ARCADE/SIM selector only on track select (not mode opts)",
    "ARCADE: ~2x as many item boxes spawn along the track",
    "ARCADE: power-up effects last 2x longer (GHOST/WRECK/oil)",
    "ARCADE NITRO: sustained 2.5x acceleration boost (~5s), not instant",
    "Collisions hug car silhouette (hull) - tight contact, no gap",
    "AI un-wedges without driving off-track (steers to interior)",
    "AI steers away when wedged vs car/player (no infinite grind)",
    "ARCADE airborne launch dialled way down (was too high/unplayable)",
    "ARCADE power-ups now floating boxes you drive through",
    "Item box: glowing spinning pulsing cube w/ kind icon",
    "Grabbed item box vanishes, respawns after 5 seconds",
    "One power-up at a time; can't grab another until it ends",
    "Box frequency scales with human player count (100-300)",
    "5+ players: chance of paired boxes (left + right shoulder)",
    "HAZARD oil (3 lanes): hit it = drift uncontrollably ~2.5s",
    "Game Options: POWER-UPS on/off toggle (persists)",
    "Item boxes start after span 100, sit low near the road",
    "Item boxes sit on road edges; steer over to grab one",
    "Car SILHOUETTE glows the effect colour (not a blob)",
    "GHOST: car turns translucent, passes through traffic",
    "Effect name shows centred below the checkpoint timer",
    "Quick Race PHYSICS row toggles ARCADE/SIMULATION + fits OK/Back",
    "Minimap: on a branch both fork tracks render (not just dots)",
    "MORE STATS: POWER captioned (engine output); WEIGHT under GRIP",
    "MORE STATS BALANCE shows a visual front/rear weight split bar",
    "Quick stat bars (under PAINT) match the MORE STATS scale",
    "MORE STATS: Left/Right switches car in place (1P stats screen)",
    "MORE STATS: captions under less-obvious bars; text clears RANDOM",
    "Menu nav: UP/DOWN/LEFT/RIGHT follow visual order, no skips",
    "Menu nav: LEFT/RIGHT steps OK<->BACK on a GAMEPAD (no skip)",
    "Menu nav: selectors still cycle their value on LEFT/RIGHT",
    "Track Select: DYNAMICS has arrows, sits above OK/BACK",
    "Track Select: option rows fit without overlap (cup too)",
    "Cop Chase RANDOM COP: cop drawn at race start, pick car normally",
    "MORE STATS shows real carparam physics (1P + split)",
    "Heavy cars hit harder/shove; light cars get flung",
    "Heavy cars climb hills slower; light cars climb easy",
    "Power-to-weight: lighter cars out-accelerate heavy",
    "Slipstream boost when tucked behind another car",
    "Downforce: grippy cars stay planted in fast corners",
    "ARCADE mode: collisions punchier than SIM but controlled",
    "ARCADE pads grant NITRO/GHOST/WRECK/HAZARD power-ups",
    "Power-up chip + timer shows in each player's pane",
    "GHOST cars pass through others (white shimmer aura)",
    "HAZARD drop spins out the next car that touches it",
    "SIMULATION = arcade grip/power + realistic gravity",
    "ARCADE/SIM picked on track + multiplayer screens",
    "Demo fires after 1 min idle on main menu",
    "Demo: random track/car/opponents/traffic, no cops",
    "Demo returns to main menu on finish (no results)",
    "Cop Chase INFECT: arrested car becomes a random-car cop",
    "INFECT: human keeps driving as cop; AI cop gives chase",
    "INFECT: round ends once every suspect is infected",
    "Collisions fire on real model contact (mesh hitbox)",
    "Cop Chase results: COPS (arrests) + SUSPECTS (bust time)",
    "Cop Chase ends: all suspects busted or all finish",
    "Italy (Courmayeur) starts a race without crashing",
    "Cop roles: OK rejected if all-cop or all-suspect",
    "Arrested car stops dead, can't be driven/pushed",
    "ARRESTED splash centred; its floating bar hidden",
    "Arrested cars drop off the split-screen overview map",
    "Strong short rumble for BOTH cop+suspect on arrest",
    "All suspects show a bust bar; shrinks w/ distance, 2x range",
    "CHOOSE YOUR TEAM / COP ROLES show profile names",
    "Cup team mode: host assigns AI opponents' teams",
    "Cup team mode: per-opponent skill slider drives AI",
    "Dev: host assigns teams to ADD AI PLAYER bots",
    "Cup final standings show profile names (not PLAYER N)",
    "WHAT NEXT? CAR SELECTION -> car grid (not profile select)",
    "WHAT NEXT? buttons aligned under the title (both layouts)",
    "Crisp on 4K/high-DPI scaled displays (no blur)",
    "Menu UP/LEFT work with 5+ pads (no stuck-stick block)",
    "Rumble survives many races (no FF death after first race)",
    "Pad that sleeps/reconnects mid-session re-rumbles next race",
    "MP results table: name aligned, columns, divider",
    "Cup results screen + POINTS column (race + total)",
    "7-player empty cells: one MAP + one STANDINGS (no dup)",
    "Empty-cell map zoom/rotation is smooth, no flicker",
    "Split-screen overview map: no diagonal on P2P tracks",
    "CUP - WHAT NEXT? title at top, buttons realigned/narrower",
    "7th+ split-screen car sits flat (no roll/wobble)",
    "Cup picker: per-race track/traffic/police, one at a time",
    "Cup options: AI OPPONENTS count (they score points)",
    "Cup race ends the moment all humans finish",
    "Dev: ADD AI PLAYER bots in the MP lobby (A key)",
    "CHANGELOG screen: bullets, spacing, top button",
    "MP split-screen post-race menu",
    "Police chase: durable cop, no airborne, dodges traffic",
    "Car-select HANDLING bar (Aston Vantages not blank)",
    "WGI rumble backend for a 5th+ pad",
    "Split-screen back-confirm on every setup screen",
    "Regular-car horn + TD6 character horns",
    "Short friendly controller names in the lobby",
    "MP catch-up paces off the next opponent ahead",
    "Traffic: lighter, no despawn-in-front, fills gaps",
    "Controller slots stable across a pad disconnect",
    "MP opponent labels below the wheels, 50% opacity",
    "Reset-car on SELECT; CHANGE VIEW on Y; horn on L3",
    "Split-screen force feedback on the correct pad",
    "Manual-only stuck recovery + 5s cooldown",
    "MP cop-chase multi-cop + cop-only car select",
    "Split-screen empty-cell map zoom-to-field",
    "Per-viewport engine audio pan for 3+ players",
    "CHANGELOG + version screen (this build)",
    "PENDING TO TEST menu + overlay (this build)",
    "PENDING TO TEST: SUPR/DELETE removes the highlighted item",
};
#define K_SEED_COUNT ((int)(sizeof(k_seed) / sizeof(k_seed[0])))

#include "td5_pending_detail.h"   /* k_pending_detail[]: per-item ENTER-modal test notes */

static const char *pending_path(void) {
    static char p[600];
    static int  done = 0;
    if (!done) { td5_plat_ini_resolve_path("td5re_pending.txt", p, sizeof p); done = 1; }
    return p;
}

static void pending_add(const char *text) {
    int i;
    if (s_count >= PENDING_MAX || !text) return;
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return;
    for (i = 0; i < s_count; i++)               /* de-dup by text */
        if (strcmp(s_items[i].text, text) == 0) return;
    strncpy(s_items[s_count].text, text, PENDING_TEXT_MAX - 1);
    s_items[s_count].text[PENDING_TEXT_MAX - 1] = '\0';
    s_count++;
}

static void pending_load_file(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "rb");
    static char buf[65536];   /* must match td5_pending_save's buffer */
    size_t      n;
    char       *line;
    if (!f) return;
    n = td5_plat_file_read(f, buf, sizeof buf - 1);
    td5_plat_file_close(f);
    if (n == 0) return;
    buf[n] = '\0';
    line = buf;
    while (line && *line) {
        char *nl = strpbrk(line, "\r\n");
        char *s;
        if (nl) *nl = '\0';
        s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#') {
            if (s[1] == '-') {                      /* "#- text" = retired tombstone */
                const char *t = s + 2;
                while (*t == ' ' || *t == '\t') t++;
                retired_add(t);
            }
            /* any other "# ..." line is an ordinary comment — ignore */
        } else if (*s) {
            const char *txt = s;
            if (s[0] == '[' && s[2] == ']')     /* legacy "[ ] text" / "[x] text" prefix from
                                                  * before crossing-out was removed -- strip it,
                                                  * the tested/untested state is no longer tracked */
                txt = s + 3;
            pending_add(txt);
        }
        if (!nl) break;
        line = nl + 1;
        while (*line == '\r' || *line == '\n') line++;
    }
}

void td5_pending_save(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "wb");
    static char buf[65536];
    int         len  = 0, i;
    if (!f) { TD5_LOG_W(LOG_TAG, "save: cannot open %s", path); return; }
    len += snprintf(buf + len, sizeof buf - (size_t)len,
        "# TD5RE pending-test checklist.  one item per line,  #- = retired (deleted).\r\n"
        "# The game merges newly-shipped items on launch; edit freely.  SUPR in-game deletes.\r\n");
    for (i = 0; i < s_count && len < (int)sizeof buf - (PENDING_TEXT_MAX + 16); i++)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "%s\r\n", s_items[i].text);
    for (i = 0; i < s_retired_count && len < (int)sizeof buf - (PENDING_TEXT_MAX + 16); i++)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "#- %s\r\n", s_retired[i]);
    if (len < 0) len = 0;
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    td5_plat_file_write(f, buf, (size_t)len);
    td5_plat_file_close(f);
}

int td5_pending_init(void) {
    static PendingItem prior[PENDING_MAX];   /* snapshot of the file's live items */
    int prior_count, i, j;

    s_count         = 0;
    s_retired_count = 0;
    pending_load_file();                 /* live items + retired (#- ) tombstones */

    /* Rebuild the list so freshly-shipped k_seed[] entries surface. New work is
     * added at the TOP of k_seed[], so iterating it in order lands newest-first.
     * THIS is what makes new work show up in the in-game checklist on a machine
     * that already has a td5re_pending.txt — the file used to be frozen after
     * the first run, so nothing shipped later ever appeared. */
    prior_count = s_count;
    for (i = 0; i < prior_count; i++) prior[i] = s_items[i];
    s_count = 0;

    for (i = 0; i < K_SEED_COUNT; i++) {           /* 1) seeds, newest-first */
        const char *t = k_seed[i];
        while (*t == ' ' || *t == '\t') t++;
        if (!*t || retired_contains(t)) continue;
        pending_add(t);
    }
    for (j = 0; j < prior_count; j++)              /* 2) keep hand-added, non-seed */
        if (!retired_contains(prior[j].text))
            pending_add(prior[j].text);            /* de-dups against seeds */

    td5_pending_save();                            /* persist merged list + tombstones */
    TD5_LOG_I(LOG_TAG, "pending-test: %d item(s), %d retired (%s)",
              s_count, s_retired_count, pending_path());
    return 1;
}

void td5_pending_shutdown(void) { /* writes happen on delete; nothing to flush */ }

int td5_pending_count(void)        { return s_count; }
const char *td5_pending_text(int i){ return (i >= 0 && i < s_count) ? s_items[i].text : ""; }

/* Longer test-focus note for the ENTER-key modal. Matched by exact text
 * against k_pending_detail[] (td5_pending_detail.h) rather than by index, so
 * coverage can be partial/out-of-order and survives list reshuffling. */
const char *td5_pending_detail_text(int i) {
    const char *text = td5_pending_text(i);
    int j;
    for (j = 0; j < K_PENDING_DETAIL_COUNT; j++)
        if (strcmp(k_pending_detail[j].text, text) == 0) return k_pending_detail[j].detail;
    return "No additional test notes recorded for this item yet -- use the summary above as the test focus.";
}

/* Remove item i from the list (shift the tail down), tombstone it, and persist.
 * Used by the SUPR/Delete key on the PENDING TO TEST menu to drop a row outright
 * once it's been verified. The tombstone (#- ) is what keeps the launch-time
 * seed merge from re-adding the deleted item. */
void td5_pending_delete(int i) {
    int k;
    if (i < 0 || i >= s_count) return;
    retired_add(s_items[i].text);        /* tombstone so the seed merge won't re-add it */
    for (k = i; k < s_count - 1; k++) s_items[k] = s_items[k + 1];
    s_count--;
    s_items[s_count].text[0] = '\0';
    td5_pending_save();
}

int  td5_pending_overlay_on(void)      { return s_overlay_on; }
void td5_pending_set_overlay(int on)   { s_overlay_on = on ? 1 : 0; }
void td5_pending_toggle_overlay(void)  { s_overlay_on = !s_overlay_on; }

/* The in-race overlay draw lives in td5_hud.c (td5_hud_draw_pending_overlay) so it
 * uses the same HUD font/size as the debug-stats overlay. This module only owns
 * the on/off state above plus the list. */
