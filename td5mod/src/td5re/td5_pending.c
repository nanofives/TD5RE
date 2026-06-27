/**
 * td5_pending.c -- Dev/QA "pending to test" checklist + in-game overlay.
 *
 * See td5_pending.h. Backing file: td5re_pending.txt next to the exe (untracked,
 * gitignored). Line format:
 *     # comment
 *     [ ] item still to test
 *     [x] item already tested   <- pruned by the /end routine
 */
#include "td5_pending.h"
#include "td5_platform.h"
#include "td5re.h"

#include <string.h>
#include <stdio.h>

#define LOG_TAG "pending"

#define PENDING_MAX       128
#define PENDING_TEXT_MAX  120

typedef struct { char text[PENDING_TEXT_MAX]; int done; } PendingItem;

static PendingItem s_items[PENDING_MAX];
static int s_count      = 0;
static int s_overlay_on = 0;

/* Compiled-in seed list — used ONLY when no file exists yet (first run on a
 * machine). Keep each concise: these double as the right-justified overlay
 * lines. New work shipped later should be added here (and folded into the
 * changelog) so a fresh checkout seeds the current testing backlog. */
static const char *const k_seed[] = {
    "MP mode vote: A=vote -> profile-colour border ring on mode",
    "MP mode vote: more voters = more nested rings; host decides",
    "MP mode vote: arrow disappears once a player casts their vote",
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
    "Reset-car on SELECT, CHANGE VIEW on L3",
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

static const char *pending_path(void) {
    static char p[600];
    static int  done = 0;
    if (!done) { td5_plat_ini_resolve_path("td5re_pending.txt", p, sizeof p); done = 1; }
    return p;
}

static void pending_add(const char *text, int done) {
    int i;
    if (s_count >= PENDING_MAX || !text) return;
    while (*text == ' ' || *text == '\t') text++;
    if (!*text) return;
    for (i = 0; i < s_count; i++)               /* de-dup by text */
        if (strcmp(s_items[i].text, text) == 0) return;
    strncpy(s_items[s_count].text, text, PENDING_TEXT_MAX - 1);
    s_items[s_count].text[PENDING_TEXT_MAX - 1] = '\0';
    s_items[s_count].done = done ? 1 : 0;
    s_count++;
}

static void pending_load_file(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "rb");
    static char buf[16384];
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
        if (*s && *s != '#') {
            int         done = 0;
            const char *txt  = s;
            if (s[0] == '[' && s[2] == ']') {       /* "[ ] text" / "[x] text" */
                done = (s[1] == 'x' || s[1] == 'X');
                txt  = s + 3;
            }
            pending_add(txt, done);
        }
        if (!nl) break;
        line = nl + 1;
        while (*line == '\r' || *line == '\n') line++;
    }
}

void td5_pending_save(void) {
    const char *path = pending_path();
    TD5_File   *f    = td5_plat_file_open(path, "wb");
    static char buf[16384];
    int         len  = 0, i;
    if (!f) { TD5_LOG_W(LOG_TAG, "save: cannot open %s", path); return; }
    len += snprintf(buf + len, sizeof buf - (size_t)len,
        "# TD5RE pending-test checklist.  [x] = tested/done (pruned by /end),  [ ] = still to test.\r\n"
        "# The game rewrites this when you toggle items on the PENDING TO TEST menu; edit freely too.\r\n");
    for (i = 0; i < s_count && len < (int)sizeof buf - (PENDING_TEXT_MAX + 16); i++)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "[%c] %s\r\n",
                        s_items[i].done ? 'x' : ' ', s_items[i].text);
    if (len < 0) len = 0;
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    td5_plat_file_write(f, buf, (size_t)len);
    td5_plat_file_close(f);
}

int td5_pending_init(void) {
    s_count = 0;
    pending_load_file();                 /* an existing file is authoritative */
    if (s_count == 0) {                  /* first run / empty → seed + persist */
        int i;
        for (i = 0; i < K_SEED_COUNT; i++) pending_add(k_seed[i], 0);
        td5_pending_save();
        TD5_LOG_I(LOG_TAG, "seeded %d pending-test items at %s", s_count, pending_path());
    } else {
        TD5_LOG_I(LOG_TAG, "loaded %d pending-test items (%d remaining)",
                  s_count, td5_pending_remaining());
    }
    return 1;
}

void td5_pending_shutdown(void) { /* writes happen on toggle; nothing to flush */ }

int td5_pending_count(void)        { return s_count; }
const char *td5_pending_text(int i){ return (i >= 0 && i < s_count) ? s_items[i].text : ""; }
int td5_pending_is_done(int i)     { return (i >= 0 && i < s_count) ? s_items[i].done : 0; }

int td5_pending_remaining(void) {
    int i, r = 0;
    for (i = 0; i < s_count; i++) if (!s_items[i].done) r++;
    return r;
}

void td5_pending_toggle(int i) {
    if (i < 0 || i >= s_count) return;
    s_items[i].done = !s_items[i].done;
    td5_pending_save();
}

/* Remove item i from the list (shift the tail down) and persist. Used by the
 * SUPR/Delete key on the PENDING TO TEST menu to drop a row outright instead of
 * just marking it tested. The seed list is only consulted on a first run, so a
 * deleted item stays gone in the backing file. */
void td5_pending_delete(int i) {
    int k;
    if (i < 0 || i >= s_count) return;
    for (k = i; k < s_count - 1; k++) s_items[k] = s_items[k + 1];
    s_count--;
    s_items[s_count].text[0] = '\0';
    s_items[s_count].done    = 0;
    td5_pending_save();
}

int  td5_pending_overlay_on(void)      { return s_overlay_on; }
void td5_pending_set_overlay(int on)   { s_overlay_on = on ? 1 : 0; }
void td5_pending_toggle_overlay(void)  { s_overlay_on = !s_overlay_on; }

/* The in-race overlay draw lives in td5_hud.c (td5_hud_draw_pending_overlay) so it
 * uses the same HUD font/size as the debug-stats overlay. This module only owns
 * the on/off state above plus the list. */
