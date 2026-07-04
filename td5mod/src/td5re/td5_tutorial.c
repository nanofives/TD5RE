/**
 * td5_tutorial.c -- First-race controller-tutorial overlay (PORT ENHANCEMENT).
 * See td5_tutorial.h for the behavioural contract.
 *
 * The controller is the real Xbox pad (Wikipedia Xbox_Controller.svg), baked to
 * a raw-DEFLATE BGRA blob in td5_tutorial_pad_art.h, inflated once and blitted
 * as a single textured quad. Everything is laid out in a 640x480 virtual space
 * drawn with a UNIFORM scale, centred on screen, so the pad keeps its real
 * aspect on widescreen (the HUD's non-uniform 640x480 mapping would stretch it).
 * Labels sit around the pad with short white leader lines (subtle black halo)
 * routed in nested channels. Text is TTF-first (NOT gated on
 * td5_vui_text_available()).
 */
#include "td5_tutorial.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "td5re.h"
#include "td5_platform.h"
#include "td5_vectorui.h"
#include "td5_save.h"
#include "td5_game.h"
#include "td5_input.h"     /* td5_input_get_input_source (joystick vs keyboard) */
#include "td5_inflate.h"
#include "td5_sound.h"      /* td5_sound_set_paused -- mute race audio while overlay is up */
#include "td5_tutorial_pad_art.h"

#define LOG_TAG "hud"
#define PAD_TEX_PAGE 990

/* ------------------------------------------------------------------ state -- */
static int      s_active     = 0;
static int      s_force_mode = 0;
static int      s_humans     = 1;
static uint32_t s_ready_mask = 0;
static uint32_t s_prev_in[TD5_MAX_HUMAN_PLAYERS];
static unsigned s_anim       = 0;
static int      s_pad_ready  = 0;
static int      s_pad_warned = 0;

/* Uniform scale + screen origin (set each draw). */
static float U = 1.0f, OX = 0.0f, OY = 0.0f;
static float ART_X, ART_Y, ART_W, ART_H;

static float PXc(float vx) { return OX + (vx - 320.0f) * U; }
static float PYc(float vy) { return OY + (vy - 240.0f) * U; }

/* ----------------------------------------------------- controller geometry -- */
enum {
    E_A = 0, E_B, E_X, E_Y, E_LB, E_RB, E_BACK, E_START,
    E_LSTICK, E_RSTICK, E_RT, E_LT, E_DPAD, E_L3, E_R3, E_NONE
};
typedef struct { float x, y; } Vec2;
static const Vec2 k_anchor[E_NONE] = {
    [E_A]={PAD_AX_A,PAD_AY_A}, [E_B]={PAD_AX_B,PAD_AY_B}, [E_X]={PAD_AX_X,PAD_AY_X},
    [E_Y]={PAD_AX_Y,PAD_AY_Y}, [E_LB]={PAD_AX_LB,PAD_AY_LB}, [E_RB]={PAD_AX_RB,PAD_AY_RB},
    [E_BACK]={PAD_AX_BACK,PAD_AY_BACK}, [E_START]={PAD_AX_START,PAD_AY_START},
    [E_LSTICK]={PAD_AX_LSTICK,PAD_AY_LSTICK}, [E_RSTICK]={PAD_AX_RSTICK,PAD_AY_RSTICK},
    [E_RT]={PAD_AX_RT,PAD_AY_RT}, [E_LT]={PAD_AX_LT,PAD_AY_LT}, [E_DPAD]={PAD_AX_DPAD,PAD_AY_DPAD},
    [E_L3]={PAD_AX_L3,PAD_AY_L3}, [E_R3]={PAD_AX_R3,PAD_AY_R3},
};
static Vec2 elem_pos(int e)
{ Vec2 v = { ART_X + k_anchor[e].x * ART_W, ART_Y + k_anchor[e].y * ART_H }; return v; }
static int elem_is_left(int e) { return k_anchor[e].x < 0.5f; }

static int button_to_elem(int btn)
{
    switch (btn) {
        case 0: return E_A;    case 1: return E_B;   case 2: return E_X;   case 3: return E_Y;
        case 4: return E_LB;   case 5: return E_RB;  case 6: return E_BACK; case 7: return E_START;
        case 8: return E_L3;   case 9: return E_R3;  default: return E_NONE;
    }
}
static int code_to_elem(uint32_t code)
{
    if (code & TD5_JSBIND_AXIS) {
        unsigned v = code & 0xFFu, axis = v >> 1, dir = v & 1u;
        if (axis == 0) return E_LSTICK;
        if (axis == 2) return dir ? E_RT : E_LT;
        return E_NONE;
    }
    if (code & TD5_JSBIND_BUTTON) return button_to_elem((int)(code & 0xFFu));
    return E_NONE;
}

/* ------------------------------------------------------------ draw helpers -- */
static void q(float x, float y, float w, float h, uint32_t c)
{ td5_vui_quad(PXc(x), PYc(y), w * U, h * U, c, -1, 0, 0, 0, 0); }
static void line_h(float x0, float x1, float y, float th, uint32_t c)
{ float a = x0<x1?x0:x1, b = x0<x1?x1:x0; q(a, y - th*0.5f, b - a, th, c); }
static void line_v(float x, float y0, float y1, float th, uint32_t c)
{ float a = y0<y1?y0:y1, b = y0<y1?y1:y0; q(x - th*0.5f, a, th, b - a, c); }
static void rrect(float x, float y, float w, float h, float r, uint32_t fill, uint32_t rim)
{
    if (td5_vui_shapes_available())
        td5_vui_roundrect(PXc(x), PYc(y), w*U, h*U, r*U, r*U, 1.3f*U, 1.3f*U, rim, rim, rim, fill, 1.0f);
    else q(x, y, w, h, fill);
}
static void text_center(float cx, float y, const char *s, uint32_t c, float sc)
{ td5_vui_text_centered(PXc(cx), PYc(y), s, c, U*sc, U*sc); }
static void text_left(float x, float y, const char *s, uint32_t c, float sc)
{ td5_vui_text(PXc(x), PYc(y), s, c, U*sc, U*sc); }
static void text_right(float xr, float y, const char *s, uint32_t c, float sc)
{ float w = td5_vui_text_width(s, U*sc); td5_vui_text(PXc(xr) - w, PYc(y), s, c, U*sc, U*sc); }

/* --- Leader lines: thin white core + subtle black halo, continuous joins. --- */
#define LEAD_CORE  1.4f
#define LEAD_HALO  2.6f
#define LEAD_WHITE 0xFFFFFFFFu
#define LEAD_BLACK 0x80000000u

static void seg(Vec2 a, Vec2 b, float th, uint32_t c)
{ if (a.y == b.y) line_h(a.x, b.x, a.y, th, c); else line_v(a.x, a.y, b.y, th, c); }
static void cap(Vec2 p, float th, uint32_t c) { q(p.x - th*0.5f, p.y - th*0.5f, th, th, c); }
static void leader(const Vec2 *pts, int n)
{
    for (int i=0;i<n-1;i++) seg(pts[i],pts[i+1],LEAD_HALO,LEAD_BLACK);
    for (int i=1;i<n-1;i++) cap(pts[i],LEAD_HALO,LEAD_BLACK);
    for (int i=0;i<n-1;i++) seg(pts[i],pts[i+1],LEAD_CORE,LEAD_WHITE);
    for (int i=1;i<n-1;i++) cap(pts[i],LEAD_CORE,LEAD_WHITE);
}

#define DIR_R 1   /* td5_vui_arrow dir_right=1 -> points RIGHT */
#define DIR_L 0   /* dir_right=0 -> points LEFT */
#define DIR_UP 2
/* Horizontal arrowhead via the smooth SDF arrow primitive. (lx,ly) is the line
 * end; the arrow base overlaps it slightly so they read as one shape. */
static void arrow_h(float lx, float ly, int dir_right)
{
    const float w = 9.0f, h = 10.0f;
    float ex = PXc(lx), ey = PYc(ly);
    float bx = dir_right ? (ex - 1.5f*U) : (ex + 1.5f*U - w*U);
    td5_vui_arrow(bx - 1.0f*U, ey - (h*0.5f + 1.0f)*U, (w+2.0f)*U, (h+2.0f)*U, dir_right, LEAD_BLACK);
    td5_vui_arrow(bx,          ey -  h*0.5f*U,          w*U,        h*U,        dir_right, LEAD_WHITE);
}
/* Up arrowhead (only the two top labels need this) — stacked quads, white core
 * + thin black rim. (lx,ly) = line end (= base); tip points up just below the
 * label. */
static void tri_up(float lx, float baseY, float len, float hw, uint32_t c)
{
    const int N = 14; float s = len / N;
    for (int i=0;i<N;i++) {                       /* wide at base (bottom) -> tip at top */
        float wv = 2.0f*hw*(1.0f - (i+0.5f)*s/len);
        q(lx - wv*0.5f, baseY - (i+1)*s, wv, s + 0.5f, c);
    }
}
static void arrow_up(float lx, float ly)
{ tri_up(lx, ly, 8.8f, 4.6f, LEAD_BLACK); tri_up(lx, ly, 7.6f, 3.8f, LEAD_WHITE); }

/* Small white start-circle (subtle dark rim) marking a leader's origin on its
 * button. */
static void start_dot(Vec2 p)
{
    const float r = 3.4f;
    if (td5_vui_shapes_available()) {
        td5_vui_roundrect(PXc(p.x - r - 0.7f), PYc(p.y - r - 0.7f),
                          (2*r + 1.4f)*U, (2*r + 1.4f)*U, (r + 0.7f)*U, (r + 0.7f)*U,
                          0, 0, 0, 0, 0, LEAD_BLACK, 1.0f);
        td5_vui_roundrect(PXc(p.x - r), PYc(p.y - r), 2*r*U, 2*r*U, r*U, r*U,
                          0, 0, 0, 0, 0, LEAD_WHITE, 1.0f);
    } else {
        q(p.x - r, p.y - r, 2*r, 2*r, LEAD_WHITE);
    }
}

static void ensure_pad_texture(void)
{
    if (s_pad_ready) return;
    unsigned char *bgra = (unsigned char *)malloc(PAD_ART_RAW_LEN);
    if (!bgra) return;
    size_t n = td5_inflate_mem_to_mem(bgra, PAD_ART_RAW_LEN, k_pad_art_deflate, k_pad_art_deflate_len);
    if (n == (size_t)PAD_ART_RAW_LEN &&
        td5_plat_render_upload_texture(PAD_TEX_PAGE, bgra, PAD_ART_W, PAD_ART_H, 2)) {
        s_pad_ready = 1;
        TD5_LOG_I(LOG_TAG, "Tutorial pad art uploaded: page=%d %dx%d", PAD_TEX_PAGE, PAD_ART_W, PAD_ART_H);
    } else if (!s_pad_warned) {
        s_pad_warned = 1;
        TD5_LOG_W(LOG_TAG, "Tutorial pad art not ready (inflate=%u/%d)", (unsigned)n, PAD_ART_RAW_LEN);
    }
    free(bgra);
}

/* ------------------------------------------------------------- callouts ----- */
enum { REG_T, REG_B, REG_L, REG_R };
/* action: driving-action index, OR -1 for a fixed (non-rebindable) control whose
 * element is given by `elem` (e.g. RESET CAR is hardwired to Select/Back). */
typedef struct { int action; int elem; const char *label; int region; float p0, p1, p2, p3; } CalloutDef;

#define LBL_VC 6.0f         /* nudge side/bottom labels up to centre on the row */
/* [2026-07-04] LBL_TOP_Y was 100 — with the LT/RT trigger anchors sitting at
 * ~y=197 on the pad art, that put ~68 virtual units of bare leader line
 * between the button and the label (vs. ~60-90 units total for the L/R side
 * callouts' short, mostly-horizontal leaders). Moved down to 132 (button is
 * now only ~36 units above the arrow base) so BRAKE/ACCELERATE read like the
 * rest of the diagram instead of the longest lines on screen. */
#define LBL_TOP_Y 132.0f    /* top-label text Y */
#define LBL_SCALE 0.66f
#define TXT_H 18.0f         /* approx label height (virtual) for top-arrow spacing */

static const CalloutDef k_callouts[] = {
    { 3,  E_NONE, "BRAKE",        REG_T, 0,   0,   0, 0  },
    { 2,  E_NONE, "ACCELERATE",   REG_T, 0,   0,   0, 0  },
    { 7,  E_NONE, "GEAR DOWN",    REG_L, 176, 212, 0, 0  },  /* straight (at bumper) */
    { 0,  E_NONE, "STEER",        REG_L, 176, 258, 0, 0  },  /* straight (at stick)  */
    { 5,  E_NONE, "HORN/SIREN",   REG_L, 176, 300, 0, 42 }, /* down then out (L3)   */
    { 8,  E_NONE, "CHANGE VIEW",  REG_R, 448, 238, 0, 0  },  /* straight (at Y)      */
    { 6,  E_NONE, "GEAR UP",      REG_R, 448, 212, 0, 0  },  /* straight (at bumper) */
    { 4,  E_NONE, "HANDBRAKE",    REG_R, 448, 272, 0, 20 }, /* down then out (no up) */
    /* [2026-07-04] RESET CAR/PAUSE/REAR VIEW anchor to BACK/START/Y, which sit
     * near the pad's vertical CENTER (~y=238-252) while these labels sit below
     * the pad — the old lanes (372/394) made these by far the longest leaders
     * in the diagram (~150-200 virtual units of combined line vs. ~60-90 for
     * the L/R callouts). Pulled the lanes up closer to the pad's bottom edge
     * (~327) and RESET CAR/REAR VIEW's label x closer to their buttons to cut
     * roughly a quarter to a third off the total leader length. */
    { -1, E_BACK, "RESET CAR",    REG_B, 225, 338, 0, 0  },  /* Select/Back, not rebindable */
    { 10, E_NONE, "PAUSE",        REG_B, 316, 352, 0, 0  },
    { 9,  E_NONE, "REAR VIEW",    REG_B, 405, 338, 0, 0  },
};

static void draw_callout(const CalloutDef *c, const uint32_t *bind)
{
    int e;
    if (c->action < 0) {
        e = c->elem;                              /* fixed control (RESET CAR) */
    } else {
        e = code_to_elem(bind[c->action]);
        if (c->action == 0 && e >= E_NONE) e = code_to_elem(bind[1]);
    }
    if (e >= E_NONE) return;
    Vec2 b = elem_pos(e);
    Vec2 pts[6];
    const float AL = 8.0f;

    if (c->region == REG_T) {
        float tip = LBL_TOP_Y + TXT_H + 3.0f;     /* arrow tip just below text */
        float base_y = tip + AL;                   /* line end / arrow base     */
        pts[0] = b; pts[1] = (Vec2){ b.x, base_y };
        leader(pts, 2);
        arrow_up(b.x, base_y);
        text_center(b.x, LBL_TOP_Y, c->label, LEAD_WHITE, LBL_SCALE);
    } else if (c->region == REG_B) {
        float lx = c->p0, lane = c->p1;
        int right = (lx >= b.x);
        float basex = right ? (lx - AL - 4.0f) : (lx + AL + 4.0f);
        pts[0]=b; pts[1]=(Vec2){b.x,lane}; pts[2]=(Vec2){basex,lane};
        leader(pts, 3);
        arrow_h(basex, lane, right ? DIR_R : DIR_L);
        if (right) text_left (lx, lane - LBL_VC, c->label, LEAD_WHITE, LBL_SCALE);
        else       text_right(lx, lane - LBL_VC, c->label, LEAD_WHITE, LBL_SCALE);
    } else {
        /* L/R: at most one bend. Optional vertical 'dip' off the button first
         * (used to drop below the face cluster / split steer & horn), then a
         * horizontal out to the label edge; the trailing vertical collapses when
         * the label sits at the button's row, so most leaders are dead straight. */
        int left = (c->region == REG_L);
        float lx = c->p0, ly = c->p1, dip = c->p3;
        float basex = left ? (lx + AL + 4.0f) : (lx - AL - 4.0f);
        float y0 = b.y + dip;
        int k = 0; pts[k++] = b;
        if (dip > 0.0f) pts[k++] = (Vec2){ b.x, y0 };
        pts[k++] = (Vec2){ basex, y0 };
        if (ly != y0) pts[k++] = (Vec2){ basex, ly };
        leader(pts, k);
        arrow_h(basex, ly, left ? DIR_L : DIR_R);
        if (left) text_right(lx, ly - LBL_VC, c->label, LEAD_WHITE, LBL_SCALE);
        else      text_left (lx, ly - LBL_VC, c->label, LEAD_WHITE, LBL_SCALE);
    }
    start_dot(b);
}

static void draw_callouts(void)
{
    const uint32_t *bind = td5_plat_input_player_action_bindings(0);
    for (size_t i = 0; i < sizeof k_callouts / sizeof k_callouts[0]; i++)
        draw_callout(&k_callouts[i], bind);
}

/* ----------------------------------------------------------- dismiss row ---- */
static void draw_dismiss_row(void)
{
    unsigned t = s_anim % 64u;
    unsigned tw = (t < 32u) ? t : (64u - t);
    uint32_t a = 0xB0u + (uint32_t)(tw * 48u / 32u);
    uint32_t amber = (a << 24) | 0x00FFD24Au;

    if (s_humans <= 1) {
        text_center(320, 432, "PRESS ANY BUTTON TO START THE RACE", amber, 0.85f);
        return;
    }
    int ready = 0;
    for (int i=0;i<s_humans;i++) if (s_ready_mask & (1u<<i)) ready++;
    char line[64];
    snprintf(line, sizeof line, "ALL PLAYERS PRESS A BUTTON TO START  (%d/%d)", ready, s_humans);
    text_center(320, 420, line, amber, 0.78f);
    const float chip_w = 56.0f;
    float x0 = 320.0f - s_humans * chip_w * 0.5f;
    for (int i=0;i<s_humans;i++) {
        int rdy = (s_ready_mask & (1u<<i)) != 0;
        char p[16]; snprintf(p, sizeof p, "P%d", i + 1);
        text_center(x0 + i*chip_w + chip_w*0.5f, 444, p, rdy ? 0xFF59C24Au : 0xFF8C94A0u, 0.62f);
    }
}

/* ---------------------------------------------------------------- public ---- */
void td5_tutorial_draw(void)
{
    if (!s_active) return;
    int rw = g_td5.render_width, rh = g_td5.render_height;
    if (rw <= 0 || rh <= 0) return;
    float ux = (float)rw / 640.0f, uy = (float)rh / 480.0f;
    U = ux < uy ? ux : uy;                 /* uniform: no stretch */
    OX = rw * 0.5f; OY = rh * 0.5f;

    ART_W = 224.0f;
    ART_H = ART_W * (float)PAD_ART_H / (float)PAD_ART_W;
    ART_X = 320.0f - ART_W * 0.5f;
    ART_Y = 252.0f - ART_H * 0.5f;

    /* 50%-opacity sharp rectangle covering most of the screen (no border). */
    td5_vui_quad(rw * 0.035f, rh * 0.03f, rw * 0.93f, rh * 0.94f, 0x80000000u, -1, 0, 0, 0, 0);
    text_center(320, 48, "Controls", 0xFFFFFFFFu, 1.7f);

    ensure_pad_texture();
    if (s_pad_ready)
        td5_vui_quad(PXc(ART_X), PYc(ART_Y), ART_W * U, ART_H * U,
                     0xFFFFFFFFu, PAD_TEX_PAGE, 0.0f, 0.0f, 1.0f, 1.0f);
    else
        rrect(ART_X, ART_Y, ART_W, ART_H, 24, 0xFF2C3038u, 0xFF7A8290u);

    draw_callouts();
    draw_dismiss_row();
}

static uint32_t player_dismiss_input(int slot)
{
    uint32_t m = td5_plat_input_joystick_buttons(slot) & 0x3FFFFFFFu;
    if (slot == 0 &&
        (td5_plat_input_key_pressed(0x39) || td5_plat_input_key_pressed(0x1C) ||
         td5_plat_input_key_pressed(0xC8) || td5_plat_input_key_pressed(0x11)))
        m |= 0x80000000u;
    return m;
}

void td5_tutorial_update(void)
{
    if (!s_active) return;
    s_anim++;
    for (int i=0;i<s_humans;i++) {
        uint32_t now = player_dismiss_input(i);
        uint32_t rising = now & ~s_prev_in[i];
        s_prev_in[i] = now;
        if (rising && !(s_ready_mask & (1u<<i))) {
            s_ready_mask |= (1u<<i);
            TD5_LOG_I(LOG_TAG, "Tutorial: player %d ready (%u/%d)",
                      i, (unsigned)__builtin_popcount(s_ready_mask), s_humans);
        }
    }
    uint32_t all = (s_humans >= 32) ? 0xFFFFFFFFu : ((1u << s_humans) - 1u);
    if ((s_ready_mask & all) == all) {
        s_active = 0;
        /* [2026-06-29] No persistent "seen" flag any more — the overlay is
         * armed afresh at the start of every race (gated only by the Game
         * Options TUTORIAL on/off below), so dismissing it just releases THIS
         * race's countdown. */
        /* [AUDIO 2026-07-04] Release the mute td5_tutorial_begin_race applied.
         * Shared with the pause menu's own suspend/resume (td5_sound.c) — safe
         * because td5_sound_init_race_resources() unconditionally resets the
         * mute at the start of every race before this overlay can re-arm. */
        td5_sound_set_paused(0);
        TD5_LOG_I(LOG_TAG, "Tutorial overlay dismissed by all %d player(s) — countdown released, audio unmuted", s_humans);
    }
}

int td5_tutorial_is_active(void) { return s_active; }

void td5_tutorial_begin_race(void)
{
    s_active = 0; s_force_mode = 0; s_anim = 0; s_ready_mask = 0;
    int mode = g_td5.ini.tutorial_overlay;
    if (mode <= 0) return;
    if (g_td5.network_active) return;
    if (td5_game_is_cinematic_race()) return;
    if (g_td5.ini.player_is_ai) return;
    if (g_td5.num_human_players < 1) return;
    if (g_td5.ini.race_trace_enabled || g_td5.ini.auto_throttle) return;

    /* [2026-06-29] Show at the start of EVERY race (the first thing you see on
     * each race), not just once-ever. The old td5re_progress.ini [Tutorial] Seen
     * gate is gone: the player turns the overlay off via the Game Options
     * TUTORIAL row (mode 0). mode 2 ("force") is retained only as a dev marker
     * (logged below); it no longer changes gating.
     * [TUTORIAL 2026-07-04] The overlay used to be suppressed on keyboard-only
     * play (shown only when a player was on a joystick), so turning TUTORIAL = ON
     * appeared to "do nothing" for keyboard players. The Game Options TUTORIAL row
     * is now the SINGLE authority: ON always arms the overlay at race start
     * regardless of input device — keyboard players dismiss it with the same
     * "press any button" keys handled in player_dismiss_input(). The old
     * gamepad-only gate is gone. */
    s_force_mode = (mode >= 2);

    s_humans = g_td5.num_human_players;
    if (s_humans < 1) s_humans = 1;
    if (s_humans > TD5_MAX_HUMAN_PLAYERS) s_humans = TD5_MAX_HUMAN_PLAYERS;

    s_active = 1;
    for (int i = 0; i < TD5_MAX_HUMAN_PLAYERS; i++)
        s_prev_in[i] = (i < s_humans) ? player_dismiss_input(i) : 0;

    /* [AUDIO 2026-07-04] Mute race SFX + duck music while the overlay holds the
     * grid, same lever the in-race pause menu uses (td5_sound_set_paused --
     * td5_sound.c). Runs AFTER td5_sound_init_race_resources() in this same
     * init function, so it isn't clobbered by that call's per-race unmute. */
    td5_sound_set_paused(1);

    TD5_LOG_I(LOG_TAG, "Tutorial overlay armed (mode=%d force=%d humans=%d), audio muted", mode, s_force_mode, s_humans);
    {
        const uint32_t *b = td5_plat_input_player_action_bindings(0);
        for (int a = 0; a < TD5_JSBIND_ACTIONS; a++) {
            int e = code_to_elem(b[a]);
            const char *side = (e >= E_NONE) ? "-" : (elem_is_left(e) ? "L" : "R");
            TD5_LOG_I(LOG_TAG, "  tut map: action=%d code=0x%03X elem=%d side=%s",
                      a, (unsigned)b[a], e, side);
        }
    }
}
