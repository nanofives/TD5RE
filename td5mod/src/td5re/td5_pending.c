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
    "CUP - WHAT NEXT? title at top, buttons realigned/narrower",
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

int  td5_pending_overlay_on(void)      { return s_overlay_on; }
void td5_pending_set_overlay(int on)   { s_overlay_on = on ? 1 : 0; }
void td5_pending_toggle_overlay(void)  { s_overlay_on = !s_overlay_on; }

/* The in-race overlay draw lives in td5_hud.c (td5_hud_draw_pending_overlay) so it
 * uses the same HUD font/size as the debug-stats overlay. This module only owns
 * the on/off state above plus the list. */
