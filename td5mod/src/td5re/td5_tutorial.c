/**
 * td5_tutorial.c -- First-race controller-tutorial overlay (PORT ENHANCEMENT).
 * See td5_tutorial.h for the behavioural contract.
 *
 * Everything draws through the resolution-independent VectorUI primitives
 * (td5_vectorui.h), in the HUD's 640x480 virtual layout scaled to the render
 * target. Colours are 0xAARRGGBB. No external asset: the controller is drawn
 * procedurally from rounded-rects, discs and thin quads.
 */
#include "td5_tutorial.h"

#include <stdint.h>
#include <string.h>

#include "td5re.h"          /* g_td5 (render size, ini, race flags) */
#include "td5_platform.h"   /* input polling, JSBIND codes, TD5_LOG_* */
#include "td5_vectorui.h"   /* td5_vui_* drawing primitives */
#include "td5_save.h"       /* tutorial-seen persistence */
#include "td5_game.h"       /* td5_game_is_cinematic_race */

#define LOG_TAG "hud"

/* ------------------------------------------------------------------ state -- */

static int      s_active      = 0;   /* overlay up + holding the countdown */
static int      s_force_mode  = 0;   /* TutorialOverlay==2: never set seen */
static uint32_t s_prev_input  = 0;   /* latched dismiss-input mask (rising edge) */
static unsigned s_anim        = 0;   /* per-frame counter for the pulse */

/* Canvas scale for the current draw call (render_px / virtual). */
static float SX = 1.0f, SY = 1.0f;

/* ----------------------------------------------------- controller geometry --
 * Virtual (640x480) coordinates of each physical pad element. The face-button
 * cluster uses real Xbox colours so the diagram is instantly recognisable. */
enum {
    E_A = 0, E_B, E_X, E_Y, E_LB, E_RB, E_BACK, E_START,
    E_LSTICK, E_RSTICK, E_RT, E_LT, E_DPAD, E_NONE
};

typedef struct { float x, y; } Vec2;
static const Vec2 k_elem_pos[E_NONE] = {
    [E_A]      = { 396, 264 },
    [E_B]      = { 416, 242 },
    [E_X]      = { 376, 242 },
    [E_Y]      = { 396, 220 },
    [E_LB]     = { 252, 190 },
    [E_RB]     = { 392, 190 },
    [E_BACK]   = { 304, 246 },
    [E_START]  = { 342, 246 },
    [E_LSTICK] = { 262, 238 },
    [E_RSTICK] = { 352, 288 },
    [E_RT]     = { 392, 168 },
    [E_LT]     = { 252, 168 },
    [E_DPAD]   = { 290, 288 },
};

/* Map a button index (XInput-via-DInput order) to a pad element. L3/R3 (the
 * stick-click buttons 8/9) resolve to the stick they live on. */
static int button_to_elem(int btn)
{
    switch (btn) {
        case 0: return E_A;     case 1: return E_B;
        case 2: return E_X;     case 3: return E_Y;
        case 4: return E_LB;    case 5: return E_RB;
        case 6: return E_BACK;  case 7: return E_START;
        case 8: return E_LSTICK; /* L3 = left-stick click */
        case 9: return E_RSTICK; /* R3 = right-stick click */
        default: return E_NONE;
    }
}

/* Decode a per-action binding code (td5_platform.h TD5_JSBIND_*) into the pad
 * element it points at. Axis 0 = left stick (steering); axis 2 = the shared
 * trigger axis (dir 1 = RT below centre = accelerate, dir 0 = LT = brake). */
static int code_to_elem(uint32_t code)
{
    if (code & TD5_JSBIND_AXIS) {
        unsigned v = code & 0xFFu;
        unsigned axis = v >> 1, dir = v & 1u;
        if (axis == 0) return E_LSTICK;
        if (axis == 2) return dir ? E_RT : E_LT;
        return E_NONE;
    }
    if (code & TD5_JSBIND_BUTTON)
        return button_to_elem((int)(code & 0xFFu));
    return E_NONE;
}

/* ------------------------------------------------------------ draw helpers -- */

static void q(float x, float y, float w, float h, uint32_t c)
{
    td5_vui_quad(x * SX, y * SY, w * SX, h * SY, c, -1, 0, 0, 0, 0);
}

static void line_h(float x0, float x1, float y, float th, uint32_t c)
{
    float a = x0 < x1 ? x0 : x1, b = x0 < x1 ? x1 : x0;
    q(a, y - th * 0.5f, b - a, th, c);
}
static void line_v(float x, float y0, float y1, float th, uint32_t c)
{
    float a = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    q(x - th * 0.5f, a, th, b - a, c);
}

/* Filled disc (button / stick) with a rim, via the rounded-rect SDF (radius =
 * half the box). Falls back to a square quad when shapes are unavailable. */
static void disc(float cx, float cy, float r, uint32_t fill, uint32_t rim)
{
    if (td5_vui_shapes_available()) {
        td5_vui_roundrect((cx - r) * SX, (cy - r) * SY, 2 * r * SX, 2 * r * SY,
                          r * SX, r * SX, 1.4f * SX, 1.4f * SY,
                          rim, rim, rim, fill, 1.0f);
    } else {
        q(cx - r, cy - r, 2 * r, 2 * r, fill);
    }
}

static void panel(float x, float y, float w, float h, float r,
                  uint32_t fill, float fa, uint32_t rim)
{
    if (td5_vui_shapes_available()) {
        td5_vui_roundrect(x * SX, y * SY, w * SX, h * SY, r * SX, r * SX,
                          2.0f * SX, 2.0f * SY, rim, rim, rim, fill, fa);
    } else {
        q(x, y, w, h, fill);
    }
}

static int text_ok(void) { return td5_vui_text_available(); }

static void text_center(float cx, float y, const char *s, uint32_t c, float sc)
{
    if (text_ok()) td5_vui_text_centered(cx * SX, y * SY, s, c, SX * sc, SY * sc);
}
static void text_left(float x, float y, const char *s, uint32_t c, float sc)
{
    if (text_ok()) td5_vui_text(x * SX, y * SY, s, c, SX * sc, SY * sc);
}
static void text_right(float xr, float y, const char *s, uint32_t c, float sc)
{
    if (!text_ok()) return;
    float w = td5_vui_text_width(s, SX * sc);
    td5_vui_text(xr * SX - w, y * SY, s, c, SX * sc, SY * sc);
}

/* Small left/right arrowhead at the label end of a leader line. */
static void arrowhead(float x, float y, int dir_right, uint32_t c)
{
    if (td5_vui_shapes_available())
        td5_vui_arrow(x * SX, (y - 4.0f) * SY, 7.0f * SX, 8.0f * SY, dir_right, c);
    else
        q(x, y - 3.0f, 6.0f, 6.0f, c);   /* crude fallback */
}

/* ------------------------------------------------------------- pad drawing -- */

static void draw_pad(void)
{
    const uint32_t body_fill = 0xFF23272Eu;
    const uint32_t body_rim  = 0xFF566070u;
    const uint32_t stick_fill = 0xFF14171Cu;
    const uint32_t stick_rim  = 0xFF707A88u;
    const uint32_t shoulder   = 0xFF2C313Au;
    const uint32_t glyph_lt   = 0xFFC8D0DAu;

    /* Grips + body. */
    panel(228, 286, 46, 78, 18, 0xFF1C2026u, 1.0f, body_rim);
    panel(396, 286, 46, 78, 18, 0xFF1C2026u, 1.0f, body_rim);
    panel(236, 204, 198, 96, 26, body_fill, 1.0f, body_rim);

    /* Triggers + bumpers. */
    panel(238, 158, 30, 16, 6, shoulder, 1.0f, body_rim);   /* LT */
    panel(376, 158, 30, 16, 6, shoulder, 1.0f, body_rim);   /* RT */
    panel(236, 182, 34, 14, 7, shoulder, 1.0f, body_rim);   /* LB */
    panel(374, 182, 34, 14, 7, shoulder, 1.0f, body_rim);   /* RB */
    text_center(253, 163, "LT", glyph_lt, 0.42f);
    text_center(391, 163, "RT", glyph_lt, 0.42f);
    text_center(253, 185, "LB", glyph_lt, 0.42f);
    text_center(391, 185, "RB", glyph_lt, 0.42f);

    /* Sticks. */
    disc(k_elem_pos[E_LSTICK].x, k_elem_pos[E_LSTICK].y, 15, stick_fill, stick_rim);
    disc(k_elem_pos[E_LSTICK].x, k_elem_pos[E_LSTICK].y, 6,  0xFF2A2F37u, stick_rim);
    disc(k_elem_pos[E_RSTICK].x, k_elem_pos[E_RSTICK].y, 15, stick_fill, stick_rim);
    disc(k_elem_pos[E_RSTICK].x, k_elem_pos[E_RSTICK].y, 6,  0xFF2A2F37u, stick_rim);

    /* D-pad (plus of two quads). */
    q(k_elem_pos[E_DPAD].x - 6,  k_elem_pos[E_DPAD].y - 18, 12, 36, 0xFF12151Au);
    q(k_elem_pos[E_DPAD].x - 18, k_elem_pos[E_DPAD].y - 6,  36, 12, 0xFF12151Au);

    /* Back / Start pills. */
    disc(k_elem_pos[E_BACK].x,  k_elem_pos[E_BACK].y,  5, 0xFF3A4049u, body_rim);
    disc(k_elem_pos[E_START].x, k_elem_pos[E_START].y, 5, 0xFF3A4049u, body_rim);

    /* Face buttons in Xbox colours, with their letters. */
    disc(k_elem_pos[E_A].x, k_elem_pos[E_A].y, 9, 0xFF5FAE48u, 0xFF8FE07Au); /* A green */
    disc(k_elem_pos[E_B].x, k_elem_pos[E_B].y, 9, 0xFFC2362Fu, 0xFFE86A60u); /* B red   */
    disc(k_elem_pos[E_X].x, k_elem_pos[E_X].y, 9, 0xFF2F66B8u, 0xFF6A98DEu); /* X blue  */
    disc(k_elem_pos[E_Y].x, k_elem_pos[E_Y].y, 9, 0xFFD0A828u, 0xFFF0D060u); /* Y yellow*/
    text_center(k_elem_pos[E_A].x, k_elem_pos[E_A].y - 6, "A", 0xFF0E1A0Cu, 0.5f);
    text_center(k_elem_pos[E_B].x, k_elem_pos[E_B].y - 6, "B", 0xFF1E0606u, 0.5f);
    text_center(k_elem_pos[E_X].x, k_elem_pos[E_X].y - 6, "X", 0xFF081222u, 0.5f);
    text_center(k_elem_pos[E_Y].x, k_elem_pos[E_Y].y - 6, "Y", 0xFF201A06u, 0.5f);
}

/* ------------------------------------------------------------- callouts ----- */

typedef struct { const char *label; int elem; } Callout;

/* Action -> display name (index-aligned to the 11 driving actions). LEFT/RIGHT
 * are merged into one "STEER" callout below. */
static const char *k_act_name[TD5_JSBIND_ACTIONS] = {
    "STEER", "STEER", "ACCELERATE", "BRAKE", "HANDBRAKE", "HORN / SIREN",
    "GEAR UP", "GEAR DOWN", "CHANGE VIEW", "REAR VIEW", "PAUSE"
};

/* Draw one column of callouts (count items) tied to their pad elements with an
 * L-rail leader: element -> side rail -> label, arrowhead at the label. */
static void draw_callout_column(const Callout *items, int count, int left_side)
{
    if (count <= 0) return;
    const float ytop = 138.0f, ybot = 372.0f;
    const float rail   = left_side ? 214.0f : 456.0f;
    const float lbl_x  = left_side ? 150.0f : 492.0f;   /* anchored text edge */
    const float head_x = left_side ? 156.0f : 484.0f;   /* arrowhead x */
    const uint32_t line_c = 0xFF7F8A99u;
    const uint32_t lbl_c  = 0xFFEDEDEDu;

    for (int i = 0; i < count; i++) {
        if (items[i].elem >= E_NONE) continue;
        float ex = k_elem_pos[items[i].elem].x;
        float ey = k_elem_pos[items[i].elem].y;
        float ly = ytop + (i + 0.5f) * (ybot - ytop) / (float)count;

        line_h(ex, rail, ey, 1.6f, line_c);   /* element -> rail */
        line_v(rail, ey, ly, 1.6f, line_c);    /* rail vertical   */
        line_h(rail, head_x, ly, 1.6f, line_c);/* rail -> label   */
        arrowhead(head_x, ly, left_side ? 0 : 1, line_c);

        if (left_side) text_right(lbl_x, ly - 5.0f, items[i].label, lbl_c, 0.5f);
        else           text_left (lbl_x, ly - 5.0f, items[i].label, lbl_c, 0.5f);
    }
}

static void draw_callouts(void)
{
    const uint32_t *bind = td5_plat_input_player_action_bindings(0);

    Callout left[TD5_JSBIND_ACTIONS], right[TD5_JSBIND_ACTIONS];
    int nl = 0, nr = 0;

    for (int a = 0; a < TD5_JSBIND_ACTIONS; a++) {
        if (a == 1) continue;                 /* RIGHT merged into STEER (a==0) */

        int elem = code_to_elem(bind[a]);
        if (a == 0 && elem >= E_NONE)         /* LEFT unbound: try RIGHT */
            elem = code_to_elem(bind[1]);
        if (elem >= E_NONE) continue;         /* nothing to point at */

        Callout c = { k_act_name[a], elem };
        if (k_elem_pos[elem].x < 320.0f) { if (nl < TD5_JSBIND_ACTIONS) left[nl++]  = c; }
        else                             { if (nr < TD5_JSBIND_ACTIONS) right[nr++] = c; }
    }

    draw_callout_column(left,  nl, 1);
    draw_callout_column(right, nr, 0);
}

/* ---------------------------------------------------------------- public ---- */

void td5_tutorial_draw(void)
{
    if (!s_active) return;

    int rw = g_td5.render_width, rh = g_td5.render_height;
    if (rw <= 0 || rh <= 0) return;
    SX = (float)rw / 640.0f;
    SY = (float)rh / 480.0f;

    /* 30%-opacity dark background over the whole frame. */
    q(0, 0, 640, 480, 0x4C000000u);

    /* The overlay rectangle (subtle frame so it reads as a panel). */
    panel(40, 44, 560, 392, 14, 0x66101418u, 1.0f, 0xC0A8B4C4u);

    text_center(320, 70, "CONTROLS", 0xFFFFFFFFu, 0.95f);
    line_h(120, 520, 96, 1.5f, 0x40FFFFFFu);

    draw_pad();
    draw_callouts();

    /* Pulsing dismiss hint (triangle-wave brightness, no libm). */
    {
        unsigned t = s_anim % 64u;
        unsigned tri = (t < 32u) ? t : (64u - t);     /* 0..32 */
        uint32_t a = 0xB0u + (uint32_t)(tri * 48u / 32u); /* 0xB0..0xE0 */
        uint32_t hint = (a << 24) | 0x00FFD24Au;       /* amber */
        text_center(320, 408, "PRESS ANY BUTTON TO START THE RACE", hint, 0.55f);
    }
}

/* Combined dismiss-input mask: any gamepad button on any human slot (low bits)
 * plus a synthetic high bit for the keyboard "go" keys (NOT Esc — that is the
 * pause menu). */
static uint32_t dismiss_input_mask(void)
{
    uint32_t m = 0;
    int hp = g_td5.num_human_players;
    if (hp < 1) hp = 1;
    if (hp > TD5_MAX_HUMAN_PLAYERS) hp = TD5_MAX_HUMAN_PLAYERS;
    for (int i = 0; i < hp; i++)
        m |= td5_plat_input_joystick_buttons(i) & 0x3FFFFFFFu; /* mask off our bit */

    if (td5_plat_input_key_pressed(0x39) ||  /* Space */
        td5_plat_input_key_pressed(0x1C) ||  /* Enter */
        td5_plat_input_key_pressed(0xC8) ||  /* Up arrow (accelerate) */
        td5_plat_input_key_pressed(0x11))    /* W */
        m |= 0x80000000u;
    return m;
}

void td5_tutorial_update(void)
{
    if (!s_active) return;
    s_anim++;

    uint32_t now = dismiss_input_mask();
    uint32_t rising = now & ~s_prev_input;
    s_prev_input = now;

    if (rising) {
        s_active = 0;
        if (!s_force_mode) td5_save_set_tutorial_seen(1);
        TD5_LOG_I(LOG_TAG, "Tutorial overlay dismissed (rising=0x%08X) — countdown released", rising);
    }
}

int td5_tutorial_is_active(void) { return s_active; }

void td5_tutorial_begin_race(void)
{
    s_active = 0;
    s_force_mode = 0;
    s_anim = 0;

    int mode = g_td5.ini.tutorial_overlay;
    if (mode <= 0) return;                       /* disabled */
    if (g_td5.network_active) return;            /* never pause a lockstep race */
    if (td5_game_is_cinematic_race()) return;    /* replay / attract demo */
    if (g_td5.ini.player_is_ai) return;          /* slot 0 is AI — no human to teach */
    if (g_td5.num_human_players < 1) return;
    /* Non-interactive harness (CSV trace / auto-throttle sweep / diff-race) has
     * no human to press a button — arming here would hold the countdown forever
     * and freeze the trace at tick 0. Stay inert so race captures are unaffected. */
    if (g_td5.ini.race_trace_enabled || g_td5.ini.auto_throttle) return;

    s_force_mode = (mode >= 2);
    if (!s_force_mode && td5_save_get_tutorial_seen()) return; /* already shown once */

    s_active = 1;
    /* Latch whatever is held right now (e.g. a button still down from the menu
     * confirm) so the overlay waits for a FRESH press before dismissing. */
    s_prev_input = dismiss_input_mask();
    TD5_LOG_I(LOG_TAG, "Tutorial overlay armed (mode=%d force=%d humans=%d)",
              mode, s_force_mode, g_td5.num_human_players);

    /* Numeric proof of the "read from config" mapping (screenshots are black in
     * the headless test env, so the per-key resolution is verified via the log).
     * Each driving action -> its live-bound code -> the pad element it points at
     * -> which label column it lands in. */
    {
        const uint32_t *b = td5_plat_input_player_action_bindings(0);
        for (int a = 0; a < TD5_JSBIND_ACTIONS; a++) {
            int e = code_to_elem(b[a]);
            const char *side = (e >= E_NONE) ? "-" :
                               (k_elem_pos[e].x < 320.0f ? "L" : "R");
            TD5_LOG_I(LOG_TAG, "  tut map: action=%d %-12s code=0x%03X elem=%d side=%s",
                      a, k_act_name[a], (unsigned)b[a], e, side);
        }
    }
}
