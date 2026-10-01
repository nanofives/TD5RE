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
 *
 * [2026-10-01] Three changes from the local-6-player feedback round:
 *   - the backdrop is OPAQUE (was 50% black, unreadable over bright tracks);
 *   - every callout carries a procedurally drawn ACTION ICON next to its label
 *     (see "SECTION: action icons" for the approach and why no raster atlas);
 *   - the bottom row NAMES the players who still have to confirm instead of
 *     counting them, and names the button they must press.
 * Dev knob (dev builds only): TD5RE_TUTORIAL_TEST_HUMANS / _TEST_NAMES force a
 * multi-player overlay on a single-pad machine — see s_test_humans below.
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
#include "td5_hud.h"        /* td5_hud_get_player_identity_name -- per-slot player names */
#include "td5_config.h"     /* td5_env_int -- dev-only multi-player test knob */
#include "td5_sound.h"      /* td5_sound_set_paused -- mute race audio while overlay is up */
#include "td5_tutorial_pad_art.h"
#include "td5_i18n.h"       /* [I18N] TR() runtime string translation */

#define LOG_TAG "hud"
#define PAD_TEX_PAGE 990

/* ------------------------------------------------------------------ state -- */
static int      s_active     = 0;
static int      s_force_mode = 0;
static int      s_humans     = 1;
static const char *s_mode_hint = NULL;   /* short "what's different" line, or NULL */
static uint32_t s_ready_mask = 0;
static uint32_t s_prev_in[TD5_MAX_HUMAN_PLAYERS];
static unsigned s_anim       = 0;
static int      s_pad_ready  = 0;
static int      s_pad_warned = 0;
#ifndef TD5RE_RELEASE
/* [DEV HARNESS 2026-10-01] TD5RE_TUTORIAL_TEST_HUMANS=N arms the overlay for N
 * players without N pads on the couch, so the "waiting for" row can be captured
 * / eyeballed at 2, 4 or 6 players. Names come from
 * TD5RE_TUTORIAL_TEST_NAMES="ONE,TWO,..." and are pushed through the SAME
 * td5_hud_set_player_identity() the MP setup flow uses, so the row renders off
 * the real identity path and not a special case. While forced, each press on
 * player 1's device readies the LOWEST still-waiting slot, so one keyboard can
 * walk the list down. Compiled out of RELEASE. */
static int      s_test_humans = 0;
#endif

/* Uniform scale + screen origin (set each draw). */
static float U = 1.0f, OX = 0.0f, OY = 0.0f;
static float ART_X, ART_Y, ART_W, ART_H;
/* Panel edges in VIRTUAL units (the panel is sized in screen px, so on
 * widescreen it reaches well outside the 640x480 box) — icons clamp to these. */
static float PANEL_L, PANEL_R;

/* [OPAQUE PANEL 2026-10-01] The backdrop was 0x80000000 (50% black), which left
 * the diagram fighting whatever was behind it — unreadable on bright tracks
 * (snow, sand, pale walls) and on any brightly lit grid. Solid now, with a thin
 * rim so it reads as a deliberate panel instead of a video glitch. */
#define PANEL_BG   0xFF0B0F16u
#define PANEL_RIM  0xFF3B4450u
#define PANEL_RIM_PX 2.0f

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
/* Centred text that shrinks to fit max_w (virtual units) — used for the
 * mode-hint line since its length varies per game mode and the slot between
 * the title and the top callouts is narrow. */
static void text_center_fit(float cx, float y, const char *s, uint32_t c, float max_sc, float max_w)
{
    float sc = max_sc;
    float w = td5_vui_text_width(s, U*sc);
    if (w > max_w * U) sc *= (max_w * U) / w;
    text_center(cx, y, s, c, sc);
}

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

/* ====================================================================== */
/* SECTION: action icons                                                  */
/* ====================================================================== */
/* [ACTION ICONS 2026-10-01] Every callout now carries a pictogram of WHAT the
 * control does (pedals, steering wheel, handbrake lever, horn, shift knob,
 * camera, mirror, pause bars, car) next to its text label, so a player sitting
 * across the room reads the diagram without reading the button name.
 *
 * APPROACH: drawn PROCEDURALLY from the primitives the overlay already uses --
 * axis-aligned quads, the roundrect SDF (discs, rings, pills) and the triangle
 * SDF (cones, chevrons) -- in the same 640x480 virtual space as the rest of the
 * diagram. That makes them resolution-INDEPENDENT: the same shape at 720p,
 * 1080p and 4K, and no texel sampling when the overlay is drawn at a reduced
 * render scale or into a small pane. The alternative (bake an icon atlas with a
 * script under re/tools/ and embed it like the pad art) was rejected: a raster
 * atlas is only crisp at the size it was baked at, and the pad-art blob already
 * costs 2.8 MB inflated at runtime.
 *
 * Shapes are authored in a normalised (u,v) box, u/v in [-1,+1], +v = DOWN,
 * mapped through IX/IY/IS onto the icon's centre + half-size. The three
 * speed-critical controls are COLOUR-CODED (green throttle, red brake, amber
 * handbrake); the rest are white like their labels.
 *
 * BRAKE is a DISC + CALIPER, not a second pedal: two pedals differing only in
 * tilt read alike at couch distance, and the disc shares no silhouette with the
 * steering wheel once you add the caliper block and the red tint. */
enum {
    IC_NONE = 0, IC_STEER, IC_THROTTLE, IC_BRAKE, IC_HANDBRAKE, IC_HORN,
    IC_GEAR_UP, IC_GEAR_DOWN, IC_VIEW, IC_REARVIEW, IC_PAUSE, IC_RESET
};
/* 24 is the ceiling: the tightest callout rows (GEAR UP at y=212 and CHANGE
 * VIEW at y=238) are 26 virtual units apart, so a taller box would collide. */
#define ICON_S    24.0f       /* icon box edge, virtual units */
#define ICON_GAP   7.0f       /* gap between label text and icon */
#define IC_WHITE  0xFFFFFFFFu
#define IC_GREEN  0xFF6FD36Fu
#define IC_RED    0xFFE2564Eu
#define IC_AMBER  0xFFFFD24Au

/* Shared circle / ring / pill helpers (fall back to plain quads when the
 * roundrect shader is unavailable, same as rrect() above). */
static void disc(float cx, float cy, float r, uint32_t c)
{
    if (td5_vui_shapes_available())
        td5_vui_roundrect(PXc(cx - r), PYc(cy - r), 2*r*U, 2*r*U, r*U, r*U,
                          0, 0, 0, 0, 0, c, 1.0f);
    else q(cx - r, cy - r, 2*r, 2*r, c);
}
static void ring(float cx, float cy, float r, float th, uint32_t c)
{
    if (td5_vui_shapes_available())
        td5_vui_roundrect(PXc(cx - r), PYc(cy - r), 2*r*U, 2*r*U, r*U, r*U,
                          th*U, th*U, c, c, c, 0, 0.0f);
    else disc(cx, cy, r, c);
}
static void pill(float x, float y, float w, float h, float r, uint32_t c)
{
    if (td5_vui_shapes_available())
        td5_vui_roundrect(PXc(x), PYc(y), w*U, h*U, r*U, r*U, 0, 0, 0, 0, 0, c, 1.0f);
    else q(x, y, w, h, c);
}
/* Slanted bar: the vui primitives are axis-aligned, so a diagonal is a dense
 * staircase of small squares (steps overlap heavily, so it reads solid). */
static void bar_diag(float x0, float y0, float x1, float y1, float th, uint32_t c)
{
    const int N = 28;
    for (int i = 0; i <= N; i++) {
        float t = (float)i / (float)N;
        q(x0 + (x1 - x0)*t - th*0.5f, y0 + (y1 - y0)*t - th*0.5f, th, th, c);
    }
}
/* Solid triangle pointing down (tri_up's mirror; the arrow SDF only does L/R). */
static void tri_down(float lx, float baseY, float len, float hw, uint32_t c)
{
    const int N = 14; float s = len / N;
    for (int i = 0; i < N; i++) {
        float wv = 2.0f*hw*(1.0f - (i + 0.5f)*s/len);
        q(lx - wv*0.5f, baseY + i*s, wv, s + 0.5f, c);
    }
}

/* Current icon frame (centre + half-size), set by draw_action_icon. */
static float IC_X, IC_Y, IC_H;
static float IX(float u) { return IC_X + u * IC_H; }
static float IY(float v) { return IC_Y + v * IC_H; }
static float IS(float n) { return n * IC_H; }
static void ibox(float u0, float v0, float u1, float v1, uint32_t c)
{ q(IX(u0), IY(v0), IS(u1 - u0), IS(v1 - v0), c); }
static void ipill(float u0, float v0, float u1, float v1, float r, uint32_t c)
{ pill(IX(u0), IY(v0), IS(u1 - u0), IS(v1 - v0), IS(r), c); }
static void idisc(float u, float v, float r, uint32_t c) { disc(IX(u), IY(v), IS(r), c); }
static void iring(float u, float v, float r, float th, uint32_t c)
{ ring(IX(u), IY(v), IS(r), IS(th), c); }
static void ibar(float u0, float v0, float u1, float v1, float th, uint32_t c)
{ bar_diag(IX(u0), IY(v0), IX(u1), IY(v1), IS(th), c); }
/* Solid chevron/triangle filling the (u0,v0)-(u1,v1) box, pointing dir:
 * DIR_UP / DIR_L / DIR_R (the arrow SDF for L/R, stacked quads for up/down). */
static void itri(float u0, float v0, float u1, float v1, int dir, uint32_t c)
{
    if (dir == DIR_UP) {
        tri_up(IX((u0 + u1)*0.5f), IY(v1), IS(v1 - v0), IS((u1 - u0)*0.5f), c);
    } else {
        td5_vui_arrow(PXc(IX(u0)), PYc(IY(v0)), IS(u1 - u0)*U, IS(v1 - v0)*U,
                      dir == DIR_R, c);
    }
}
static void itri_down(float u0, float v0, float u1, float v1, uint32_t c)
{ tri_down(IX((u0 + u1)*0.5f), IY(v0), IS(v1 - v0), IS((u1 - u0)*0.5f), c); }

/* Throttle pedal: floor rail + a slanted arm carrying a PILL-shaped foot pad.
 * A first pass drew the whole pedal as one thick diagonal bar (bar_diag) -- at
 * 24 virtual units that reads as an abstract wedge, not a pedal, so the pad is
 * now its own axis-aligned rounded rect and the arm is thin. */
static void pedal(uint32_t c)
{
    ibox(-1.00f, 0.80f, 0.55f, 1.00f, c);              /* floor rail */
    ibar(-0.45f, 0.86f, 0.20f, -0.16f, 0.20f, c);      /* arm        */
    ipill(-0.02f, -0.74f, 0.86f, -0.10f, 0.16f, c);    /* foot pad   */
}

static void draw_action_icon(int icon, uint32_t col, float cx, float cy, float s)
{
    if (icon == IC_NONE) return;
    IC_X = cx; IC_Y = cy; IC_H = s * 0.5f;
    switch (icon) {
    case IC_STEER:                                   /* steering wheel */
        iring(0.0f, 0.0f, 0.92f, 0.24f, col);
        idisc(0.0f, 0.0f, 0.24f, col);
        ibox(-0.74f, -0.10f, -0.20f, 0.10f, col);
        ibox( 0.20f, -0.10f,  0.74f, 0.10f, col);
        ibox(-0.10f,  0.20f,  0.10f, 0.78f, col);
        break;
    case IC_THROTTLE:                                /* accelerator pedal */
        pedal(col);
        break;
    case IC_BRAKE:                                   /* brake disc + caliper */
        iring(-0.14f, 0.00f, 0.80f, 0.24f, col);
        idisc(-0.14f, 0.00f, 0.28f, col);
        ipill(0.42f, -0.46f, 0.96f, 0.46f, 0.16f, col);
        break;
    case IC_HANDBRAKE:                               /* ratchet lever */
        ibox(-0.96f, 0.62f, 0.34f, 0.98f, col);
        ibar(-0.32f, 0.64f, 0.46f, -0.62f, 0.30f, col);
        idisc(0.52f, -0.72f, 0.28f, col);
        break;
    case IC_HORN:                                    /* horn bell + sound ticks */
        ibox(-0.98f, -0.30f, -0.52f, 0.30f, col);
        itri(-0.56f, -0.76f, 0.26f, 0.76f, DIR_L, col);
        ibox(0.40f, -0.34f, 0.54f, 0.34f, col);
        ibox(0.64f, -0.58f, 0.78f, 0.58f, col);
        ibox(0.88f, -0.84f, 1.00f, 0.84f, col);
        break;
    case IC_GEAR_UP:                                 /* shift knob + up chevron */
    case IC_GEAR_DOWN:
        ibox(-0.96f, 0.80f, 0.14f, 1.00f, col);        /* gate plate */
        ibox(-0.58f, -0.26f, -0.26f, 0.84f, col);      /* lever rod  */
        idisc(-0.42f, -0.50f, 0.32f, col);             /* knob       */
        if (icon == IC_GEAR_UP) itri(0.30f, -0.84f, 1.00f, 0.20f, DIR_UP, col);
        else                    itri_down(0.30f, -0.20f, 1.00f, 0.84f, col);
        break;
    case IC_VIEW:                                    /* camera body + lens */
        ibox(-0.60f, -0.66f, -0.04f, -0.34f, col);   /* viewfinder hump */
        ipill(-0.96f, -0.38f, 0.84f, 0.72f, 0.18f, col);
        ring(IX(-0.06f), IY(0.17f), IS(0.40f), IS(0.13f), PANEL_BG);
        idisc(-0.06f, 0.17f, 0.17f, PANEL_BG);
        ibox(0.52f, -0.24f, 0.74f, -0.06f, PANEL_BG);/* flash window */
        break;
    case IC_REARVIEW:                                /* mirror + look-back arrow */
        ipill(-0.96f, -0.90f, 0.96f, -0.18f, 0.18f, col);  /* mirror glass */
        ibox(-0.13f, -0.18f, 0.13f, 0.16f, col);           /* stalk        */
        itri(-1.00f, 0.28f, -0.26f, 1.00f, DIR_L, col);    /* back arrow   */
        ibox(-0.32f, 0.50f, 0.94f, 0.78f, col);
        break;
    case IC_PAUSE:                                   /* two bars */
        ipill(-0.58f, -0.82f, -0.14f, 0.82f, 0.12f, col);
        ipill( 0.14f, -0.82f,  0.58f, 0.82f, 0.12f, col);
        break;
    case IC_RESET:                                   /* car + back arrow */
        itri(-0.98f, -0.98f, -0.40f, -0.44f, DIR_L, col);
        ibox(-0.46f, -0.80f, 0.66f, -0.62f, col);
        ipill(-0.52f, -0.20f, 0.40f, 0.14f, 0.14f, col);   /* roof   */
        ipill(-0.92f, 0.10f, 0.92f, 0.58f, 0.16f, col);    /* body   */
        idisc(-0.50f, 0.68f, 0.24f, col);
        idisc( 0.50f, 0.68f, 0.24f, col);
        break;
    default: break;
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
typedef struct { int action; int elem; const char *label; int region; float p0, p1, p2, p3;
                 int icon; uint32_t icol; } CalloutDef;

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

/* Place an action icon on the OUTER side of a label (the side away from the
 * pad), on the label's own text row. align: 0 = text left-justified at
 * anchor_x, 1 = right-justified at anchor_x, 2 = centred on anchor_x. The icon
 * tracks the MEASURED text width, so it stays clear of the label in every
 * language and at every font. */
static void place_action_icon(int icon, uint32_t col, const char *label,
                              float anchor_x, int align, float text_y, int outer_right)
{
    if (icon == IC_NONE) return;
    float tw = td5_vui_text_width(td5_tr(label), U * LBL_SCALE) / U;
    float l = (align == 0) ? anchor_x : (align == 1) ? (anchor_x - tw) : (anchor_x - tw*0.5f);
    float r = l + tw;
    float h = ICON_S * 0.5f;
    float cx = outer_right ? (r + ICON_GAP + h) : (l - ICON_GAP - h);
    float lo = PANEL_L + h + 4.0f, hi = PANEL_R - h - 4.0f;
    if (cx < lo) cx = lo;
    if (cx > hi) cx = hi;
    draw_action_icon(icon, col, cx, text_y + LBL_VC, ICON_S);
}

static const CalloutDef k_callouts[] = {
    { 3,  E_NONE, "BRAKE",        REG_T, 0,   0,   0, 0,  IC_BRAKE,     IC_RED   },
    { 2,  E_NONE, "ACCELERATE",   REG_T, 0,   0,   0, 0,  IC_THROTTLE,  IC_GREEN },
    { 7,  E_NONE, "GEAR DOWN",    REG_L, 176, 212, 0, 0,  IC_GEAR_DOWN, IC_WHITE },  /* straight (at bumper) */
    { 0,  E_NONE, "STEER",        REG_L, 176, 258, 0, 0,  IC_STEER,     IC_WHITE },  /* straight (at stick)  */
    { 5,  E_NONE, "HORN/SIREN",   REG_L, 176, 300, 0, 42, IC_HORN,      IC_WHITE }, /* down then out (L3)   */
    { 8,  E_NONE, "CHANGE VIEW",  REG_R, 448, 238, 0, 0,  IC_VIEW,      IC_WHITE },  /* straight (at Y)      */
    { 6,  E_NONE, "GEAR UP",      REG_R, 448, 212, 0, 0,  IC_GEAR_UP,   IC_WHITE },  /* straight (at bumper) */
    { 4,  E_NONE, "HANDBRAKE",    REG_R, 448, 272, 0, 20, IC_HANDBRAKE, IC_AMBER }, /* down then out (no up) */
    /* [2026-07-04] RESET CAR/PAUSE/REAR VIEW anchor to BACK/START/Y, which sit
     * near the pad's vertical CENTER (~y=238-252) while these labels sit below
     * the pad — the old lanes (372/394) made these by far the longest leaders
     * in the diagram (~150-200 virtual units of combined line vs. ~60-90 for
     * the L/R callouts). Pulled the lanes up closer to the pad's bottom edge
     * (~327) and RESET CAR/REAR VIEW's label x closer to their buttons to cut
     * roughly a quarter to a third off the total leader length. */
    { -1, E_BACK, "RESET CAR",    REG_B, 225, 338, 0, 0,  IC_RESET,     IC_WHITE },  /* Select/Back, not rebindable */
    { 10, E_NONE, "PAUSE",        REG_B, 316, 352, 0, 0,  IC_PAUSE,     IC_WHITE },
    { 9,  E_NONE, "REAR VIEW",    REG_B, 405, 338, 0, 0,  IC_REARVIEW,  IC_WHITE },
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
        text_center(b.x, LBL_TOP_Y, td5_tr(c->label), LEAD_WHITE, LBL_SCALE);
        place_action_icon(c->icon, c->icol, c->label, b.x, 2, LBL_TOP_Y, b.x >= 320.0f);
    } else if (c->region == REG_B) {
        float lx = c->p0, lane = c->p1;
        int right = (lx >= b.x);
        float basex = right ? (lx - AL - 4.0f) : (lx + AL + 4.0f);
        pts[0]=b; pts[1]=(Vec2){b.x,lane}; pts[2]=(Vec2){basex,lane};
        leader(pts, 3);
        arrow_h(basex, lane, right ? DIR_R : DIR_L);
        if (right) text_left (lx, lane - LBL_VC, td5_tr(c->label), LEAD_WHITE, LBL_SCALE);
        else       text_right(lx, lane - LBL_VC, td5_tr(c->label), LEAD_WHITE, LBL_SCALE);
        place_action_icon(c->icon, c->icol, c->label, lx, right ? 0 : 1, lane - LBL_VC, right);
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
        if (left) text_right(lx, ly - LBL_VC, td5_tr(c->label), LEAD_WHITE, LBL_SCALE);
        else      text_left (lx, ly - LBL_VC, td5_tr(c->label), LEAD_WHITE, LBL_SCALE);
        place_action_icon(c->icon, c->icol, c->label, lx, left ? 1 : 0, ly - LBL_VC, !left);
    }
    start_dot(b);
}

static void draw_callouts(void)
{
    const uint32_t *bind = td5_plat_input_player_action_bindings(0);
    for (size_t i = 0; i < sizeof k_callouts / sizeof k_callouts[0]; i++)
        draw_callout(&k_callouts[i], bind);
}

/* -------------------------------------------------------------- mode hint --- */
/* [MODE HINT 2026-07-04] One short line spelling out this race's special rule
 * when it plays differently enough from a normal race that the control
 * diagram alone doesn't explain the goal (cop chase incl. INFECT, drag race,
 * traffic battle, cup). Checked against the SAME flags td5_game already uses
 * to configure the race, so it matches what's actually running whether SP or
 * MP, split-screen or (non-network) local — NULL for a normal race/time-trial,
 * drawing nothing. */
static const char *mode_hint_text(void)
{
    if (td5_game_cop_chase_infect_enabled())
        return "INFECT: suspects arrested by a cop become cops themselves.";
    if (td5_game_is_wanted_mode())
        return "COP CHASE: cops must catch every suspect before time runs out.";
    if (g_td5.drag_race_enabled)
        return "DRAG RACE: launch on green, stay in your lane to the finish.";
    if (td5_game_battle_mode_active())
        return "TRAFFIC BATTLE: wreck the most oncoming traffic to win.";
    if (td5_game_mp_cup_active())
        return "CUP: points are awarded by finishing position each race.";
    return NULL;
}

/* ----------------------------------------------------------- dismiss row ---- */
/* [WAITING-FOR 2026-10-01] The row used to read "ALL PLAYERS PRESS A BUTTON TO
 * START (n/m)" over a P1/P2/P3 chip strip: on a 6-way split-screen couch
 * nobody could tell whose pad was holding up the grid. It now NAMES the players
 * who have not confirmed yet, and the list shrinks as each one presses.
 *
 * Names come from the same per-racer-slot identity the in-race name plates use
 * (td5_hud_get_player_identity_name) -- human player i drives racer slot i
 * (identity map, td5_input.c:730) -- with "PLAYER N" as the fallback when no
 * profile name was picked (single player, or a race that skipped the local-MP
 * setup flow). */
static const char *waiting_player_name(int slot, char *buf, size_t cap)
{
    const char *nm = td5_hud_get_player_identity_name(slot);
    if (nm && nm[0]) return nm;
    snprintf(buf, cap, TR("PLAYER %d"), slot + 1);
    return buf;
}

/* The overlay accepts ANY button (player_dismiss_input), but the prompt has to
 * name a real one. The diagram is an Xbox-layout pad, so a pad player reads "A";
 * a PlayStation-family pad reports a device name we can recognise and its
 * button 0 is CROSS; a keyboard player gets SPACE, one of the keys
 * player_dismiss_input() accepts. */
static const char *confirm_label(int slot)
{
    int src = td5_input_get_input_source(slot);
    const char *nm;
    char low[96];
    size_t i;
    if (src <= 0) return "SPACE";
    nm = td5_plat_input_device_name(src);
    if (!nm) return "A";
    for (i = 0; i + 1 < sizeof low && nm[i]; i++) {
        char c = nm[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[i] = '\0';
    if (strstr(low, "playstation") || strstr(low, "dualshock") ||
        strstr(low, "dualsense")   || strstr(low, "sony")      ||
        strstr(low, "wireless controller"))
        return "CROSS";
    return "A";
}

static void draw_dismiss_row(void)
{
    unsigned t = s_anim % 64u;
    unsigned tw = (t < 32u) ? t : (64u - t);
    uint32_t a = 0xB0u + (uint32_t)(tw * 48u / 32u);
    uint32_t amber = (a << 24) | 0x00FFD24Au;
    char prompt[96];

    if (s_humans <= 1) {
        snprintf(prompt, sizeof prompt, TR("PRESS %s TO START THE RACE"), confirm_label(0));
        text_center_fit(320, 432, prompt, amber, 0.85f, 560.0f);
        return;
    }

    /* Collect the players still to press, plus the DISTINCT confirm-button
     * labels among them (a keyboard player and a pad player waiting together
     * get "PRESS SPACE / A TO START"). */
    char names[256];
    char keys[3][12];
    size_t used = 0;
    int nkeys = 0, waiting = 0;
    names[0] = '\0';
    for (int i = 0; i < s_humans; i++) {
        char nb[TD5_PLAYER_NAME_BUF];
        const char *nm, *cl;
        int k, seen = 0;
        if (s_ready_mask & (1u << i)) continue;
        nm = waiting_player_name(i, nb, sizeof nb);
        if (used + 2 < sizeof names) {
            int n = snprintf(names + used, sizeof names - used, "%s%s",
                             waiting ? ", " : "", nm);
            used = (n > 0 && (size_t)n < sizeof names - used) ? used + (size_t)n
                                                              : sizeof names - 1;
        }
        waiting++;
        cl = confirm_label(i);
        for (k = 0; k < nkeys; k++) if (strcmp(keys[k], cl) == 0) { seen = 1; break; }
        if (!seen && nkeys < (int)(sizeof keys / sizeof keys[0]))
            snprintf(keys[nkeys++], sizeof keys[0], "%s", cl);
    }
    if (!waiting) return;                 /* all ready -- update() closes us */

    char line[320];
    snprintf(line, sizeof line, "%s %s", TR("WAITING FOR:"), names);
    text_center_fit(320, 414, line, 0xFFFFFFFFu, 0.80f, 580.0f);

    char keylist[40];
    keylist[0] = '\0';
    for (int k = 0, p = 0; k < nkeys; k++)
        p += snprintf(keylist + p, sizeof keylist - (size_t)p, "%s%s",
                      k ? " / " : "", keys[k]);
    snprintf(prompt, sizeof prompt, TR("PRESS %s TO START"), keylist);
    text_center_fit(320, 442, prompt, amber, 0.80f, 560.0f);
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

    /* OPAQUE sharp rectangle covering most of the screen, with a thin rim.
     * (Was 50% black -- see the PANEL_BG note at the top of the file.) */
    {
        float px = rw * 0.035f, py = rh * 0.03f, pw = rw * 0.93f, ph = rh * 0.94f;
        float b = PANEL_RIM_PX * U;
        PANEL_L = 320.0f + (px        - OX) / U;
        PANEL_R = 320.0f + (px + pw   - OX) / U;
        td5_vui_quad(px - b, py - b, pw + 2*b, ph + 2*b, PANEL_RIM, -1, 0, 0, 0, 0);
        td5_vui_quad(px,     py,     pw,       ph,       PANEL_BG,  -1, 0, 0, 0, 0);
    }
    text_center(320, 48, TR("Controls"), 0xFFFFFFFFu, 1.7f);
    if (s_mode_hint)
        text_center_fit(320, 82, s_mode_hint, 0xFFFFD24Au, 0.62f, 560.0f);

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
#ifndef TD5RE_RELEASE
    if (s_test_humans > 1) {
        /* Dev harness: one device stands in for all the forced pads — each
         * press readies the lowest still-waiting slot. */
        uint32_t now = player_dismiss_input(0);
        uint32_t rising = now & ~s_prev_in[0];
        s_prev_in[0] = now;
        if (rising) {
            for (int i = 0; i < s_humans; i++)
                if (!(s_ready_mask & (1u << i))) {
                    s_ready_mask |= (1u << i);
                    TD5_LOG_I(LOG_TAG, "Tutorial DEV: player %d ready (%u/%d)",
                              i, (unsigned)__builtin_popcount(s_ready_mask), s_humans);
                    break;
                }
        }
    } else
#endif
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
    s_active = 0; s_force_mode = 0; s_anim = 0; s_ready_mask = 0; s_mode_hint = NULL;
    int mode = g_td5.ini.tutorial_overlay;
    if (mode <= 0) return;
    if (g_td5.network_active) return;
    if (td5_game_is_cinematic_race()) return;
    if (g_td5.ini.player_is_ai) return;
    if (g_td5.num_human_players < 1) return;
    if (g_td5.ini.race_trace_enabled || g_td5.ini.auto_throttle) return;

    s_mode_hint = mode_hint_text();
    if (s_mode_hint)
        TD5_LOG_I(LOG_TAG, "Tutorial mode hint: \"%s\"", s_mode_hint);

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

#ifndef TD5RE_RELEASE
    /* Dev multi-player harness (see s_test_humans at the top of the file). */
    s_test_humans = td5_env_int("TD5RE_TUTORIAL_TEST_HUMANS", 0, 0, TD5_MAX_HUMAN_PLAYERS);
    if (s_test_humans > 1) {
        const char *names = getenv("TD5RE_TUTORIAL_TEST_NAMES");
        s_humans = s_test_humans;
        if (names && names[0]) {
            char buf[256];
            int i = 0, a = 0, b;
            snprintf(buf, sizeof buf, "%s", names);
            for (b = 0; i < s_humans; b++) {           /* split on ',' in place */
                if (buf[b] != ',' && buf[b] != '\0') continue;
                int end = buf[b] == '\0';
                buf[b] = '\0';
                if (buf[a]) td5_hud_set_player_identity(i++, buf + a, 0xFFFFFFu);
                if (end) break;
                a = b + 1;
            }
        }
        TD5_LOG_W(LOG_TAG, "Tutorial DEV harness: forcing %d players "
                           "(TD5RE_TUTORIAL_TEST_HUMANS) -- player 1's device "
                           "readies one waiting slot per press", s_humans);
    } else {
        s_test_humans = 0;
    }
#endif

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
