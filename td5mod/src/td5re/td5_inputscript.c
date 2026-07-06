/* ========================================================================
 * td5_inputscript.c — scripted input harness (see td5_inputscript.h)
 *
 * TD5RE-only dev/test affordance; no original counterpart. The engine keeps
 * one frame-based clock and two output channels:
 *   - race action bits per slot, OR'd into s_control_bits by
 *     td5_input_poll_race_session (td5_input.c) after the hardware poll;
 *   - raw DIK key states, merged into the platform keyboard snapshot via
 *     td5_plat_input_inject_key (td5_platform_win32.c), so every reader
 *     (frontend nav, pause menu, dev keys, rebindable race map) sees them
 *     as physical presses — including when the window is unfocused.
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "td5re.h"
#include "td5_types.h"
#include "td5_platform.h"
#include "td5_frontend.h"    /* td5_frontend_harness_force_input */
#include "td5_inputscript.h"

#define LOG_TAG "input"

#define ISC_MAX_CMDS      512
#define ISC_MAX_RELEASES  32
#define ISC_PRESS_TICKS   4     /* hold duration of a 'press' before auto-release */

typedef enum {
    ISC_NONE = 0,
    ISC_ACTION,     /* race control bit set/clear            */
    ISC_KEY,        /* raw DIK key down/up                   */
    ISC_SYNC,       /* wait for context, re-base clock       */
    ISC_SLOT,       /* change default race slot              */
    ISC_LOG,        /* marker line                           */
    ISC_QUIT        /* request clean shutdown                */
} ISC_Type;

typedef struct {
    int      when;      /* ticks after the sync base */
    ISC_Type type;
    uint32_t bit;       /* ISC_ACTION */
    int      key;       /* ISC_KEY: DIK scancode */
    int      value;     /* 0 = release, 1 = hold, 2 = press (auto-release) */
    int      slot;      /* ISC_ACTION target slot / ISC_SLOT new default */
    int      sync_race; /* ISC_SYNC: 1 = race, 0 = menu */
    char     text[96];  /* ISC_LOG */
} ISC_Cmd;

typedef struct {
    int      at_tick;   /* absolute engine tick to release at */
    int      is_key;    /* 1 = key, 0 = action bit */
    uint32_t bit;
    int      key;
    int      slot;
} ISC_Release;

static ISC_Cmd     s_cmds[ISC_MAX_CMDS];
static int         s_cmd_count   = 0;
static int         s_next_cmd    = 0;
static int         s_clock       = 0;   /* absolute engine ticks since boot */
static int         s_base        = 0;   /* clock value at the last sync     */
static int         s_sync_wait   = -1;  /* -1 none, else 1=race 0=menu      */
static int         s_finished    = 1;   /* 1 = inactive/done                */
static uint32_t    s_held_bits[TD5_MAX_RACER_SLOTS];
static ISC_Release s_rel[ISC_MAX_RELEASES];
static int         s_rel_count   = 0;

/* ---- verb tables ---------------------------------------------------- */

static const struct { const char *name; uint32_t bit; } k_actions[] = {
    { "throttle",  TD5_INPUT_THROTTLE      },
    { "brake",     TD5_INPUT_BRAKE         },
    { "handbrake", TD5_INPUT_HANDBRAKE     },
    { "horn",      TD5_INPUT_HORN          },
    { "gearup",    TD5_INPUT_GEAR_UP       },
    { "geardown",  TD5_INPUT_GEAR_DOWN     },
    { "camera",    TD5_INPUT_CAMERA_CHANGE },
    { "rearview",  TD5_INPUT_REAR_VIEW     },
    { "left",      TD5_INPUT_STEER_LEFT    },
    { "right",     TD5_INPUT_STEER_RIGHT   },
    { "pause",     TD5_INPUT_PAUSE         },
    { "escape",    TD5_INPUT_ESCAPE        },
};

static const struct { const char *name; int dik; } k_keys[] = {
    { "up", 0xC8 }, { "down", 0xD0 }, { "left", 0xCB }, { "right", 0xCD },
    { "enter", 0x1C }, { "esc", 0x01 }, { "space", 0x39 },
    { "backspace", 0x0E }, { "tab", 0x0F },
    { "p", 0x19 }, { "q", 0x10 }, { "a", 0x1E }, { "z", 0x2C },
    { "t", 0x14 }, { "x", 0x2D }, { "r", 0x13 },
    { "f1", 0x3B }, { "f2", 0x3C }, { "f3", 0x3D }, { "f4", 0x3E },
    { "f5", 0x3F }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 },
    { "f9", 0x43 }, { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 },
};

static int isc_lookup_key(const char *tok)
{
    size_t i;
    if (tok[0] == '0' && (tok[1] == 'x' || tok[1] == 'X')) {
        int v = (int)strtol(tok, NULL, 16);
        return (v > 0 && v < 256) ? v : -1;
    }
    for (i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++)
        if (_stricmp(tok, k_keys[i].name) == 0) return k_keys[i].dik;
    return -1;
}

static uint32_t isc_lookup_action(const char *tok)
{
    size_t i;
    for (i = 0; i < sizeof(k_actions) / sizeof(k_actions[0]); i++)
        if (_stricmp(tok, k_actions[i].name) == 0) return k_actions[i].bit;
    return 0;
}

/* ---- parsing --------------------------------------------------------- */

static int isc_parse_value(const char *tok)
{
    if (_stricmp(tok, "press") == 0) return 2;
    if (strcmp(tok, "1") == 0) return 1;
    if (strcmp(tok, "0") == 0) return 0;
    return -1;
}

static int isc_parse_file(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    int lineno = 0, prev_when = 0, cur_slot = 0;

    if (!f) {
        TD5_LOG_E(LOG_TAG, "InputScript: cannot open '%s'", path);
        return 0;
    }

    while (fgets(line, sizeof(line), f) && s_cmd_count < ISC_MAX_CMDS) {
        char *p = line, *tok;
        ISC_Cmd c;
        lineno++;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
        { char *nl = strpbrk(p, "\r\n"); if (nl) *nl = '\0'; }

        memset(&c, 0, sizeof(c));
        c.slot = cur_slot;

        tok = strtok(p, " \t");
        if (!tok) continue;
        if (tok[0] == '+') {
            c.when = prev_when + atoi(tok + 1);
            tok = strtok(NULL, " \t");
        } else if (isdigit((unsigned char)tok[0])) {
            c.when = atoi(tok);
            tok = strtok(NULL, " \t");
        } else {
            /* Bare verb (no <when>) — fire at the previous command's time.
             * Lets structural lines read naturally: `sync race`. */
            c.when = prev_when;
        }
        prev_when = c.when;

        if (!tok) { TD5_LOG_W(LOG_TAG, "InputScript:%d: missing verb", lineno); continue; }

        if (_stricmp(tok, "key") == 0) {
            char *ktok = strtok(NULL, " \t");
            char *vtok = strtok(NULL, " \t");
            int v = vtok ? isc_parse_value(vtok) : -1;
            c.type = ISC_KEY;
            c.key  = ktok ? isc_lookup_key(ktok) : -1;
            c.value = v;
            if (c.key < 0 || v < 0) {
                TD5_LOG_W(LOG_TAG, "InputScript:%d: bad key command (skipped)", lineno);
                continue;
            }
        } else if (_stricmp(tok, "sync") == 0) {
            char *stok = strtok(NULL, " \t");
            c.type = ISC_SYNC;
            if (stok && _stricmp(stok, "race") == 0)      c.sync_race = 1;
            else if (stok && _stricmp(stok, "menu") == 0) c.sync_race = 0;
            else {
                TD5_LOG_W(LOG_TAG, "InputScript:%d: sync needs race|menu (skipped)", lineno);
                continue;
            }
        } else if (_stricmp(tok, "slot") == 0) {
            char *stok = strtok(NULL, " \t");
            int sv = stok ? atoi(stok) : -1;
            if (sv < 0 || sv >= TD5_MAX_RACER_SLOTS) {
                TD5_LOG_W(LOG_TAG, "InputScript:%d: bad slot (skipped)", lineno);
                continue;
            }
            c.type = ISC_SLOT;
            c.slot = sv;
            cur_slot = sv;
        } else if (_stricmp(tok, "log") == 0) {
            char *rest = strtok(NULL, "");   /* remainder of the line */
            c.type = ISC_LOG;
            strncpy(c.text, rest ? rest : "", sizeof(c.text) - 1);
        } else if (_stricmp(tok, "quit") == 0) {
            c.type = ISC_QUIT;
        } else {
            char *vtok = strtok(NULL, " \t");
            int v = vtok ? isc_parse_value(vtok) : -1;
            c.type = ISC_ACTION;
            c.bit  = isc_lookup_action(tok);
            c.value = v;
            if (c.bit == 0 || v < 0) {
                TD5_LOG_W(LOG_TAG, "InputScript:%d: unknown verb '%s' (skipped)", lineno, tok);
                continue;
            }
        }
        s_cmds[s_cmd_count++] = c;
    }
    fclose(f);
    return s_cmd_count;
}

/* ---- runtime --------------------------------------------------------- */

static void isc_schedule_release(int is_key, uint32_t bit, int key, int slot)
{
    if (s_rel_count >= ISC_MAX_RELEASES) {
        TD5_LOG_W(LOG_TAG, "InputScript: auto-release queue full, releasing now");
        if (is_key) td5_plat_input_inject_key(key, 0);
        else if (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) s_held_bits[slot] &= ~bit;
        return;
    }
    s_rel[s_rel_count].at_tick = s_clock + ISC_PRESS_TICKS;
    s_rel[s_rel_count].is_key  = is_key;
    s_rel[s_rel_count].bit     = bit;
    s_rel[s_rel_count].key     = key;
    s_rel[s_rel_count].slot    = slot;
    s_rel_count++;
}

static void isc_run_releases(void)
{
    int i = 0;
    while (i < s_rel_count) {
        if (s_clock >= s_rel[i].at_tick) {
            if (s_rel[i].is_key) {
                td5_plat_input_inject_key(s_rel[i].key, 0);
            } else if (s_rel[i].slot >= 0 && s_rel[i].slot < TD5_MAX_RACER_SLOTS) {
                s_held_bits[s_rel[i].slot] &= ~s_rel[i].bit;
            }
            s_rel[i] = s_rel[--s_rel_count];
        } else {
            i++;
        }
    }
}

static int isc_context_reached(int want_race)
{
    if (want_race)
        return g_td5.game_state == TD5_GAMESTATE_RACE &&
               g_td5.simulation_tick_counter >= 1;
    return g_td5.game_state == TD5_GAMESTATE_MENU;
}

static void isc_exec(const ISC_Cmd *c)
{
    switch (c->type) {
    case ISC_ACTION:
        if (c->value == 0) s_held_bits[c->slot] &= ~c->bit;
        else               s_held_bits[c->slot] |=  c->bit;
        if (c->value == 2) isc_schedule_release(0, c->bit, 0, c->slot);
        TD5_LOG_I(LOG_TAG, "InputScript t=%d: action bit=0x%08X slot=%d val=%d",
                  s_clock - s_base, c->bit, c->slot, c->value);
        break;
    case ISC_KEY:
        td5_plat_input_inject_key(c->key, c->value != 0);
        if (c->value == 2) isc_schedule_release(1, 0, c->key, 0);
        TD5_LOG_I(LOG_TAG, "InputScript t=%d: key 0x%02X val=%d",
                  s_clock - s_base, c->key, c->value);
        break;
    case ISC_SLOT:
        TD5_LOG_I(LOG_TAG, "InputScript t=%d: slot -> %d", s_clock - s_base, c->slot);
        break;
    case ISC_LOG:
        TD5_LOG_I(LOG_TAG, "InputScript MARKER: %s", c->text);
        break;
    case ISC_QUIT:
        TD5_LOG_I(LOG_TAG, "InputScript t=%d: quit requested", s_clock - s_base);
        g_td5.quit_requested = 1;
        break;
    default:
        break;
    }
}

/* ---- public API ------------------------------------------------------ */

int td5_inputscript_init(void)
{
    memset(s_held_bits, 0, sizeof(s_held_bits));
    s_cmd_count = s_next_cmd = 0;
    s_clock = s_base = 0;
    s_sync_wait = -1;
    s_rel_count = 0;
    s_finished  = 1;

    if (!g_td5.ini.input_script[0]) return 0;

    if (isc_parse_file(g_td5.ini.input_script) <= 0) {
        TD5_LOG_E(LOG_TAG, "InputScript: no commands loaded from '%s' — harness OFF",
                  g_td5.ini.input_script);
        return 0;
    }
    s_finished = 0;
    /* Keep injected input alive in background windows for the whole script
     * lifetime; released when the script completes or on shutdown. */
    td5_frontend_harness_force_input(1);
    TD5_LOG_I(LOG_TAG, "InputScript: loaded %d command(s) from '%s'",
              s_cmd_count, g_td5.ini.input_script);
    return 1;
}

void td5_inputscript_shutdown(void)
{
    if (!s_finished) {
        TD5_LOG_W(LOG_TAG, "InputScript: shutdown with %d command(s) unfired",
                  s_cmd_count - s_next_cmd);
        td5_frontend_harness_force_input(0);
    }
    td5_plat_input_inject_clear();
    memset(s_held_bits, 0, sizeof(s_held_bits));
    s_finished = 1;
}

int td5_inputscript_active(void)
{
    return !s_finished;
}

uint32_t td5_inputscript_race_bits(int slot)
{
    if (s_finished || slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return s_held_bits[slot];
}

/* One engine step ≈ 1/60 s (wall-clocked below). */
static void isc_step(void)
{
    s_clock++;
    isc_run_releases();

    /* Blocked on a sync? Resume (and re-base) once the context is reached. */
    if (s_sync_wait >= 0) {
        if (!isc_context_reached(s_sync_wait)) return;
        s_base = s_clock;
        TD5_LOG_I(LOG_TAG, "InputScript: sync %s reached, clock re-based",
                  s_sync_wait ? "race" : "menu");
        s_sync_wait = -1;
    }

    while (s_next_cmd < s_cmd_count) {
        const ISC_Cmd *c = &s_cmds[s_next_cmd];
        if (s_clock - s_base < c->when) break;
        if (c->type == ISC_SYNC) {
            s_next_cmd++;
            if (isc_context_reached(c->sync_race)) {
                s_base = s_clock;   /* already there — re-base immediately */
            } else {
                s_sync_wait = c->sync_race;
                return;
            }
            continue;
        }
        isc_exec(c);
        s_next_cmd++;
    }

    if (s_next_cmd >= s_cmd_count && s_rel_count == 0) {
        TD5_LOG_I(LOG_TAG, "InputScript: script complete (%d commands, %d ticks)",
                  s_cmd_count, s_clock);
        td5_plat_input_inject_clear();
        memset(s_held_bits, 0, sizeof(s_held_bits));
        s_finished = 1;
        td5_frontend_harness_force_input(0);
    }
}

void td5_inputscript_frame_tick(void)
{
    static uint32_t s_last_ms = 0;

    if (s_finished) return;

    /* Wall-clock the engine at 60 ticks/s instead of one tick per rendered
     * frame: at uncapped fps a frame-based clock made a 4-tick `press`
     * shorter than one 30 Hz sim poll, so poll_race_session could sample
     * right past it (observed: camera press never firing while the longer
     * holds worked). 60 Hz wall ticks make script times fps-independent and
     * guarantee a press spans >= 2 sim polls. */
    {
        uint32_t now = td5_plat_time_ms();
        int steps;
        if (s_last_ms == 0) s_last_ms = now;
        steps = (int)((now - s_last_ms) / 16u);
        if (steps <= 0) return;
        if (steps > 30) {           /* hitch/debugger clamp — don't burst-fire */
            steps = 30;
            s_last_ms = now;
        } else {
            s_last_ms += (uint32_t)steps * 16u;
        }
        while (steps-- > 0 && !s_finished)
            isc_step();
    }
}
