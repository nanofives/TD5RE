/**
 * td5_police.c -- POLICE PURSUIT for the normal races (PORT-ONLY, round H HH). Contract and rules: td5_police.h.
 *
 * Sections: shared HUD drawing / knobs + helpers / span geometry / pursuit lifecycle / hooks from the stock police /
 * roadblocks / tick / race HUD / stats + DEV.
 *
 * UNITS. Distances in metres from the world (vertex) units through TD5RE_POLICE_UPM (world units per metre on the
 * stock tracks, default 330, see POL_UPM_DEFAULT); speeds from the actor's linear velocity (24.8 world units per
 * 30 Hz sim tick). Spans only decide AHEAD / BEHIND and where a roadblock goes.
 *
 * STATE. One file-scope struct, sim thread only (the HUD reads it in the frame pass), reset at every race init.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_ai.h"
#include "td5_track.h"
#include "td5_render.h"         /* g_render_width_f */
#include "td5_hud.h"            /* td5_hud_prim_tris, td5_hud_pane_rect */
#include "td5_vectorui.h"
#include "td5_roam_mode.h"
#include "td5_police.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "ai"

#define POL_HZ          30.0
#define POL_PI          3.14159265358979323846
#define POL_UPM_DEFAULT 330      /* world units per metre on the stock tracks: see pol_upm() */
#define POL_BLK_MAX     2
#define POL_BLK_CARS    3
#define POL_BLK_CAR_W   4.4      /* lateral cover of one car parked across the road, metres (free roam's) */

/* ============================================================ shared HUD drawing ============ */

#define QB_QUADS 32
static TD5_D3DVertex qb_v[QB_QUADS * 4];
static uint16_t      qb_i[QB_QUADS * 6];
static int           qb_n;

static void qb_flush(void)
{
    if (qb_n > 0) { td5_hud_prim_tris(qb_v, qb_n * 4, qb_i, qb_n * 6); qb_n = 0; }
}

static void qb_rect(float x, float y, float w, float h, uint32_t col)
{
    TD5_D3DVertex *v;
    uint16_t *ix;
    uint16_t b;
    int k;
    if (qb_n >= QB_QUADS) qb_flush();
    b = (uint16_t)(qb_n * 4);
    v = &qb_v[qb_n * 4];
    ix = &qb_i[qb_n * 6];
    v[0].screen_x = x;     v[0].screen_y = y;
    v[1].screen_x = x + w; v[1].screen_y = y;
    v[2].screen_x = x + w; v[2].screen_y = y + h;
    v[3].screen_x = x;     v[3].screen_y = y + h;
    for (k = 0; k < 4; k++) {
        v[k].depth_z = 0.0f; v[k].rhw = 1.0f; v[k].diffuse = col; v[k].specular = 0;
        v[k].tex_u = 0.5f; v[k].tex_v = 0.5f;
    }
    ix[0] = b; ix[1] = (uint16_t)(b + 1); ix[2] = (uint16_t)(b + 2);
    ix[3] = b; ix[4] = (uint16_t)(b + 2); ix[5] = (uint16_t)(b + 3);
    qb_n++;
}

static void tx(float x, float y, const char *s, uint32_t col, float px)
{
    qb_flush();
    td5_vui_text(x, y, s, col, px / 26.0f, px / 26.0f);
}
static void tx_c(float cx, float y, const char *s, uint32_t col, float px)
{
    qb_flush();
    td5_vui_text_centered(cx, y, s, col, px / 26.0f, px / 26.0f);
}
static float tx_w(const char *s, float px) { return td5_vui_text_width(s, px / 26.0f); }

static float pol_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void td5_police_hud_draw(const TD5_PoliceHud *h, float x0, float y0, float W, float H)
{
    const float S = H / 720.0f < 0.6f ? 0.6f : H / 720.0f;
    const long tick = h ? h->tick : 0;
    char line[96];
    int i;
    if (!h || W < 2.0f || H < 2.0f) return;
    if (!h->pursuit && !h->busted && !(h->toast && h->toast[0])) return;
    td5_plat_render_set_preset(TD5_PRESET_TRANSLUCENT_LINEAR_HUD);
    if (h->pursuit && !h->busted) {
        /* top right: PURSUIT, heat boxes, the cops on it, the evade bar */
        const float bw = 250.0f * S, bx = x0 + W - bw - 12.0f * S, by = y0 + 80.0f * S;
        const int flash = ((tick / 8) & 1) != 0;
        qb_rect(bx, by, bw, 92.0f * S, 0xA0000000);
        tx(bx + 12.0f * S, by + 6.0f * S, "PURSUIT", flash ? 0xFFFF3B3B : 0xFF3B7BFF, 24.0f * S);
        for (i = 0; i < 5; i++)
            qb_rect(bx + 140.0f * S + (float)i * 20.0f * S, by + 12.0f * S, 15.0f * S, 15.0f * S, i < h->heat ? 0xFFFF3B3B : 0xFF3A3F48);
        snprintf(line, sizeof line, "HEAT %d   COPS %d", h->heat, h->cops);
        tx(bx + 12.0f * S, by + 38.0f * S, line, 0xFFFFFFFF, 15.0f * S);
        tx(bx + 12.0f * S, by + 62.0f * S, "EVADE", 0xFFC8D0DA, 13.0f * S);
        qb_rect(bx + 70.0f * S, by + 66.0f * S, bw - 84.0f * S, 9.0f * S, 0xFF2A323D);
        qb_rect(bx + 70.0f * S, by + 66.0f * S, (bw - 84.0f * S) * pol_clampf(h->evade, 0.0f, 1.0f), 9.0f * S, 0xFF50E070);
        if (h->bust > 0.0f) {
            const float mw = 360.0f * S, mx = x0 + (W - mw) * 0.5f, my = y0 + H * 0.72f;
            qb_rect(mx - 6.0f * S, my - 30.0f * S, mw + 12.0f * S, 52.0f * S, 0xA0000000);
            tx_c(x0 + W * 0.5f, my - 26.0f * S, "PULL OVER", 0xFFFF3B3B, 18.0f * S);
            qb_rect(mx, my, mw, 12.0f * S, 0xFF2A323D);
            qb_rect(mx, my, mw * pol_clampf(h->bust, 0.0f, 1.0f), 12.0f * S, 0xFFFF3B3B);
        }
    }
    if (h->busted) {
        const float bw = 520.0f * S, bx = x0 + (W - bw) * 0.5f, by = y0 + H * 0.30f;
        qb_rect(bx, by, bw, 110.0f * S, 0xC0000000);
        qb_rect(bx, by, bw, 4.0f * S, ((tick / 6) & 1) ? 0xFFFF3B3B : 0xFF3B7BFF);
        tx_c(x0 + W * 0.5f, by + 12.0f * S, "BUSTED", 0xFFFF3B3B, 52.0f * S);
        if (h->busted_line && h->busted_line[0]) tx_c(x0 + W * 0.5f, by + 74.0f * S, h->busted_line, 0xFFFFFFFF, 18.0f * S);
    } else if (h->toast && h->toast[0]) {
        const float w = tx_w(h->toast, 20.0f * S);
        qb_rect(x0 + (W - w) * 0.5f - 14.0f * S, y0 + H * 0.22f, w + 28.0f * S, 34.0f * S, 0xB0000000);
        tx(x0 + (W - w) * 0.5f, y0 + H * 0.22f + 6.0f * S, h->toast, h->toast_col, 20.0f * S);
    }
    qb_flush();
    td5_plat_render_set_preset(TD5_PRESET_OPAQUE_LINEAR);
}

/* ============================================================ state + knobs ================= */

typedef struct {
    int    on;                          /* a pursuit runs                                    */
    int    heat, offences, cause;
    long   t0, lost, last_offence, grace_until, reinf_cd, block_cd;
    double bust;                        /* the PULL OVER meter, 0..1                         */
    int    busted_t;                    /* ticks left of the BUSTED hold                     */
    int    npur;                        /* pursuers this tick                                */
    double dmin, bdmin;                 /* nearest pursuer / block car, metres               */
    int    ram_cop; long ram_tick;      /* a ram recorded by the physics hook                */
    char   toast[96]; uint32_t toast_col; long toast_until;
} PolTarget;

typedef struct {
    int    on, target, span, ncar, heat;
    int    slot[POL_BLK_CARS];
    int32_t px[POL_BLK_CARS], pz[POL_BLK_CARS];   /* the cars, world (vertex) units                */
    int     yaw[POL_BLK_CARS];                    /* the cars' yaw, 12-bit                          */
    double cx, cz, tx, tz;              /* centre + forward tangent, world units             */
    double gap_w;                       /* metres, 0 = closed                                */
    double gx, gz;                      /* the gap's centre, world units                     */
    long   t0;
    double min_d, prev_f;
    int    have_prev, crossed, contact, releasing;
} PolBlock;

static struct {
    int      on;                        /* the layer runs this race                          */
    int      knobs;
    int      knob_on, block_heat, block_max, upm, queue_spans;
    double   see_m, lose_s, escape_m, bust_s, join_m, block_min_m, block_max_m, ram_impact;
    uint32_t rng;
    PolTarget t[TD5_MAX_RACER_SLOTS];
    PolBlock  blk[POL_BLK_MAX];
    uint8_t   blk_of[TD5_MAX_TOTAL_ACTORS];   /* block index + 1 per actor slot, 0 = not a block car */
    long      tick_last;
    int       dev_slow;
    TD5_PoliceStats st;
} P;

static void pol_read_knobs(void)
{
    if (P.knobs) return;
    P.knobs = 1;
    P.knob_on     = td5_env_int("TD5RE_POLICE_PURSUIT", 1, 0, 1);
    P.upm         = td5_env_int("TD5RE_POLICE_UPM", POL_UPM_DEFAULT, 100, 2000);
    P.see_m       = td5_env_int("TD5RE_POLICE_SEE_M", 150, 30, 600);
    P.lose_s      = td5_env_int("TD5RE_POLICE_LOSE_S", 6, 1, 120);
    P.escape_m    = td5_env_int("TD5RE_POLICE_ESCAPE_M", 320, 80, 2000);
    P.bust_s      = td5_env_int("TD5RE_POLICE_BUST_DS", 20, 3, 200) / 10.0;
    P.join_m      = td5_env_int("TD5RE_POLICE_JOIN_M", 200, 0, 1000);
    P.block_heat  = td5_env_int("TD5RE_POLICE_BLOCK_HEAT", 3, 0, 5);
    P.block_max   = td5_env_int("TD5RE_POLICE_BLOCKS", 2, 1, POL_BLK_MAX);
    P.block_min_m = td5_env_int("TD5RE_POLICE_BLOCK_MIN_M", 200, 60, 600);
    P.block_max_m = td5_env_int("TD5RE_POLICE_BLOCK_MAX_M", 400, 100, 900);
    if (P.block_max_m < P.block_min_m + 50.0) P.block_max_m = P.block_min_m + 50.0;
    P.ram_impact  = td5_env_int("TD5RE_POLICE_RAM_IMPACT", 12000, 1000, 400000);
    P.queue_spans = td5_env_int("TD5RE_POLICE_QUEUE_SPANS", 18, 0, 200);
    TD5_LOG_I(LOG_TAG, "police pursuit: %s (TD5RE_POLICE_PURSUIT), %d units/m, sight %.0f m, lose %.0f+2xheat s, escape %.0f m, "
              "bust %.1f s, join %.0f m, roadblocks from heat %d (max %d, %.0f..%.0f m ahead), ram impact %.0f",
              P.knob_on ? "ON" : "off", P.upm, P.see_m, P.lose_s, P.escape_m, P.bust_s, P.join_m, P.block_heat, P.block_max,
              P.block_min_m, P.block_max_m, P.ram_impact);
}

static long pol_tick(void) { return (long)g_td5.simulation_tick_counter; }
static uint32_t pol_lcg(void) { P.rng = P.rng * 1664525u + 1013904223u; return P.rng >> 8; }
static double pol_unit(void) { return (double)(pol_lcg() & 0xFFFFu) / 65536.0; }

static double pol_m(double world_units) { return world_units / (double)P.upm; }

/* world (vertex) units of an actor, from its 24.8 position */
static void pol_xz(const TD5_Actor *a, double *x, double *z)
{
    *x = (double)a->world_pos.x / 256.0;
    *z = (double)a->world_pos.z / 256.0;
}

static double pol_dist_m(const TD5_Actor *a, const TD5_Actor *b)
{
    double ax, az, bx, bz;
    pol_xz(a, &ax, &az);
    pol_xz(b, &bx, &bz);
    return pol_m(hypot(ax - bx, az - bz));
}

static double pol_speed_ms(const TD5_Actor *a)
{
    return pol_m(sqrt((double)a->linear_velocity_x * a->linear_velocity_x + (double)a->linear_velocity_z * a->linear_velocity_z) / 256.0) * POL_HZ;
}

static int pol_racer_n(void)
{
    int n = g_traffic_slot_base;
    if (n > TD5_MAX_RACER_SLOTS) n = TD5_MAX_RACER_SLOTS;
    return n;
}

static int pol_on_branch(const TD5_Actor *a)
{
    const int ring = td5_track_get_ring_length();
    return ring > 0 && (int)(int16_t)a->track_span_raw >= ring;
}

/* a HUMAN racer still racing (the stock police chase humans only) */
static int pol_human_racing(int slot)
{
    if (slot < 0 || slot >= pol_racer_n()) return 0;
    if (td5_game_get_slot_state(slot) != 1) return 0;
    if (td5_game_slot_is_finished(slot)) return 0;
    return td5_game_get_actor(slot) != NULL;
}

static void pol_toast(PolTarget *T, const char *msg, uint32_t col, double secs)
{
    snprintf(T->toast, sizeof T->toast, "%s", msg);
    T->toast_col = col;
    T->toast_until = pol_tick() + (long)(secs * POL_HZ);
}

int td5_police_active(void) { return P.on; }

void td5_police_race_init(void)
{
    pol_read_knobs();
    {
        const int knobs = P.knobs, knob_on = P.knob_on, bh = P.block_heat, bm = P.block_max, upm = P.upm, qs = P.queue_spans;
        const double see = P.see_m, lose = P.lose_s, esc = P.escape_m, bs = P.bust_s, jm = P.join_m, bmin = P.block_min_m,
                     bmax = P.block_max_m, ri = P.ram_impact;
        const int slow = P.dev_slow;
        memset(&P, 0, sizeof P);
        P.knobs = knobs; P.knob_on = knob_on; P.block_heat = bh; P.block_max = bm; P.upm = upm; P.queue_spans = qs;
        P.see_m = see; P.lose_s = lose; P.escape_m = esc; P.bust_s = bs; P.join_m = jm; P.block_min_m = bmin;
        P.block_max_m = bmax; P.ram_impact = ri;
        P.dev_slow = slow;
    }
    P.rng = 0x5EED0C05u;
    P.tick_last = -1;
    P.on = P.knob_on && g_td5.special_encounter_enabled && g_td5.traffic_enabled && !g_td5.wanted_mode_enabled &&
           !g_td5.drag_race_enabled && !td5_roam_active();
    TD5_LOG_I(LOG_TAG, "police pursuit: race init: %s (POLICE %d, traffic %d, wanted %d, drag %d, roam %d)",
              P.on ? "ON" : "off", g_td5.special_encounter_enabled, g_td5.traffic_enabled, g_td5.wanted_mode_enabled,
              g_td5.drag_race_enabled, td5_roam_active());
}

/* ============================================================ span geometry ================= */

/* The road band of `span`, from its lane quads (td5_track_get_lane_quad_world corners: [0] near-left, [1] near-right,
 * [2] far-right, [3] far-left of the lane, the lanes ordered across the road): the centre of the span (midway between
 * its near and far edges, across the full lane set), the unit lateral vector (lane 0 -> last lane), the unit forward
 * vector (near edge -> far edge) and the band's lateral extent [lmin, lmax] in METRES from that centre, slow verge
 * lanes at both edges trimmed. World (vertex) units for the points. 0 when the span has no usable lanes. */
static int pol_span_band(int span, double *cx, double *cz, double *rx, double *rz, double *fx, double *fz,
                         double *lmin, double *lmax)
{
    float q0[4][3], q1[4][3];
    const int n = td5_track_get_span_lane_count(span);
    double lx, lz, Rx, Rz, w, nx, nz, farx, farz, l;
    int lo = 0, hi;
    if (n <= 0 || !td5_track_get_lane_quad_world(span, 0, q0) || !td5_track_get_lane_quad_world(span, n - 1, q1)) return 0;
    /* the left and right road edges, each midway between the near and far edge of the span */
    lx = (q0[0][0] + q0[3][0]) * 0.5; lz = (q0[0][2] + q0[3][2]) * 0.5;
    Rx = (q1[1][0] + q1[2][0]) * 0.5; Rz = (q1[1][2] + q1[2][2]) * 0.5;
    w = hypot(Rx - lx, Rz - lz);
    if (w < 1.0) return 0;
    *rx = (Rx - lx) / w; *rz = (Rz - lz) / w;
    *cx = (lx + Rx) * 0.5; *cz = (lz + Rz) * 0.5;
    nx = (q0[0][0] + q1[1][0]) * 0.5; nz = (q0[0][2] + q1[1][2]) * 0.5;          /* near edge centre */
    farx = (q0[3][0] + q1[2][0]) * 0.5; farz = (q0[3][2] + q1[2][2]) * 0.5;      /* far edge centre  */
    l = hypot(farx - nx, farz - nz);
    if (l < 1.0) return 0;
    *fx = (farx - nx) / l; *fz = (farz - nz) / l;
    hi = n - 1;
    while (lo < hi && td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, lo))) lo++;
    while (hi > lo && td5_track_surface_is_slow(td5_track_get_span_lane_surface(span, hi))) hi--;
    {
        float a[4][3], c[4][3];
        double ax, az, bx, bz;
        if (!td5_track_get_lane_quad_world(span, lo, a) || !td5_track_get_lane_quad_world(span, hi, c)) return 0;
        ax = (a[0][0] + a[3][0]) * 0.5; az = (a[0][2] + a[3][2]) * 0.5;
        bx = (c[1][0] + c[2][0]) * 0.5; bz = (c[1][2] + c[2][2]) * 0.5;
        *lmin = pol_m((ax - *cx) * *rx + (az - *cz) * *rz);
        *lmax = pol_m((bx - *cx) * *rx + (bz - *cz) * *rz);
    }
    return *lmax > *lmin;
}

/* the sub-lane of `span` holding lateral `lat_m` (metres from the band centre), from each lane's mid-span edges */
static int pol_lane_at(int span, double cx, double cz, double rx, double rz, double lat_m)
{
    const int n = td5_track_get_span_lane_count(span);
    int i;
    for (i = 0; i < n; i++) {
        float q[4][3];
        double a, b;
        if (!td5_track_get_lane_quad_world(span, i, q)) continue;
        a = pol_m(((q[0][0] + q[3][0]) * 0.5 - cx) * rx + ((q[0][2] + q[3][2]) * 0.5 - cz) * rz);
        b = pol_m(((q[1][0] + q[2][0]) * 0.5 - cx) * rx + ((q[1][2] + q[2][2]) * 0.5 - cz) * rz);
        if ((lat_m >= a && lat_m <= b) || (lat_m >= b && lat_m <= a)) return i;
    }
    return n > 0 ? (lat_m < 0.0 ? 0 : n - 1) : 0;
}

static int pol_angle_of(double dx, double dz)
{
    if (dx == 0.0 && dz == 0.0) return 0;
    return (int)floor(atan2(dx, dz) * (4096.0 / (2.0 * POL_PI)) + 0.5) & 0xFFF;
}

/* ============================================================ pursuit lifecycle ============= */

static void pol_blocks_end_for(int target, const char *why);

static const char *const k_cause[3] = { "", "SPEEDING", "RAMMED A POLICE CAR" };

static void pol_start(int target, int cop, int cause)
{
    PolTarget *T = &P.t[target];
    const TD5_Actor *pl = td5_game_get_actor(target), *c = td5_game_get_actor(cop);
    char msg[96];
    T->on = 1;
    T->heat = 1;
    T->offences = 1;
    T->cause = cause;
    T->t0 = pol_tick();
    T->lost = 0;
    T->bust = 0.0;
    T->reinf_cd = pol_tick() + 60;
    T->block_cd = pol_tick();
    P.st.pursuits++;
    if (cause == 1) P.st.notice_speed++; else P.st.notice_ram++;
    if (P.st.max_heat < 1) P.st.max_heat = 1;
    if (cause == 1 && pl) snprintf(msg, sizeof msg, "POLICE PURSUIT: SPEEDING %.0f KM/H", pol_speed_ms(pl) * 3.6);
    else snprintf(msg, sizeof msg, "POLICE PURSUIT: %s", k_cause[cause]);
    pol_toast(T, msg, 0xFFFF4040, 3.0);
    TD5_LOG_I(LOG_TAG, "POLICE: PURSUIT %ld starts on slot %d: cop %d, %s (%.0f km/h), %.0f m away", P.st.pursuits, target, cop,
              k_cause[cause], pl ? pol_speed_ms(pl) * 3.6 : 0.0, (pl && c) ? pol_dist_m(pl, c) : -1.0);
}

static void pol_offence(int target, const char *what)
{
    PolTarget *T = &P.t[target];
    T->offences++;
    T->last_offence = pol_tick();
    P.st.offences++;
    TD5_LOG_I(LOG_TAG, "POLICE: offence in the pursuit on slot %d: %s (offences %d)", target, what, T->offences);
}

static void pol_end(int target, int escaped, int dist)
{
    PolTarget *T = &P.t[target];
    const long dur = pol_tick() - T->t0;
    T->on = 0;
    T->heat = 0;
    T->bust = 0.0;
    T->lost = 0;
    td5_ai_police_end_chases(target);
    pol_blocks_end_for(target, escaped ? "escaped" : "pursuit over");
    if (escaped) {
        P.st.escapes++;
        if (dist) P.st.escapes_dist++; else P.st.escapes_los++;
        T->grace_until = pol_tick() + 5 * 30;
        pol_toast(T, dist ? "ESCAPED: THE COPS ARE FAR BEHIND" : "ESCAPED: THE COPS LOST SIGHT OF YOU", 0xFF50E070, 3.0);
        TD5_LOG_I(LOG_TAG, "POLICE: ESCAPE %ld on slot %d (%s) after %.1f s", P.st.escapes, target, dist ? "distance" : "line of sight",
                  dur / POL_HZ);
    } else {
        TD5_LOG_I(LOG_TAG, "POLICE: pursuit on slot %d over after %.1f s (no escape counted)", target, dur / POL_HZ);
    }
}

static void pol_bust(int target, const char *how, int by_pullover)
{
    PolTarget *T = &P.t[target];
    const long dur = pol_tick() - T->t0;
    P.st.busts++;
    if (by_pullover) P.st.busts_pullover++; else P.st.busts_meter++;
    TD5_LOG_I(LOG_TAG, "POLICE: BUST %ld on slot %d (%s) after %.1f s of pursuit, heat %d, %d pursuer(s), nearest %.1f m, block car %.1f m",
              P.st.busts, target, how, dur / POL_HZ, T->heat, T->npur, T->dmin < 1e8 ? T->dmin : -1.0, T->bdmin < 1e8 ? T->bdmin : -1.0);
    td5_ai_police_end_chases(target);
    pol_blocks_end_for(target, "busted");
    T->on = 0;
    T->bust = 1.0;
    T->busted_t = 90;
    pol_toast(T, "BUSTED", 0xFFFF3B3B, 3.0);
}

static void pol_release(int target)
{
    PolTarget *T = &P.t[target];
    T->heat = 0;
    T->offences = 0;
    T->bust = 0.0;
    T->grace_until = pol_tick() + 15 * 30;
    P.st.releases++;
    pol_toast(T, "RELEASED: BACK IN THE RACE", 0xFFFFE070, 2.5);
    TD5_LOG_I(LOG_TAG, "POLICE: slot %d released after the BUSTED hold (15 s of grace)", target);
}

/* ============================================================ hooks from the stock police === */

int td5_police_acquire_gate(int cop, int target, int gap)
{
    PolTarget *T;
    if (!P.on) return TD5_POLICE_GATE_STOCK;
    if (cop >= 0 && cop < TD5_MAX_TOTAL_ACTORS && P.blk_of[cop]) return TD5_POLICE_GATE_SKIP;
    if (!pol_human_racing(target)) return TD5_POLICE_GATE_STOCK;
    T = &P.t[target];
    if (T->busted_t > 0 || pol_tick() < T->grace_until) return TD5_POLICE_GATE_SKIP;
    if (!T->on) return TD5_POLICE_GATE_STOCK;
    /* a pursuit runs: a cop the car passes joins at any speed (the stock window: just passed, 0..15 spans) */
    if (gap >= 0 && gap <= 15) return TD5_POLICE_GATE_JOIN;
    /* from heat 2 an idle cop up to join_m BEHIND the car joins while the pursuers are short */
    if (T->heat >= 2 && gap > 0 && T->npur < 1 + T->heat) {
        const TD5_Actor *a = td5_game_get_actor(cop), *b = td5_game_get_actor(target);
        if (a && b && pol_dist_m(a, b) <= P.join_m) { P.st.joins_behind++; return TD5_POLICE_GATE_JOIN; }
    }
    return TD5_POLICE_GATE_SKIP;
}

void td5_police_on_acquire(int cop, int target, int joined)
{
    PolTarget *T;
    if (!P.on || !pol_human_racing(target)) return;
    T = &P.t[target];
    if (!T->on) { pol_start(target, cop, 1); return; }
    P.st.joins++;
    TD5_LOG_I(LOG_TAG, "POLICE: cop %d joins the pursuit on slot %d (%s)", cop, target, joined ? "pursuit rule" : "stock trigger");
    /* passing another cop at speed is a further offence (once per 2 s) */
    if (!joined && pol_tick() - T->last_offence > 60) pol_offence(target, "passed another cop speeding");
}

void td5_police_on_pullover_stopped(int cop, int target)
{
    PolTarget *T;
    if (!P.on || !pol_human_racing(target)) return;
    T = &P.t[target];
    if (T->busted_t > 0) return;
    if (!T->on) pol_start(target, cop, 1);
    pol_bust(target, "pulled over by the cop ahead", 1);
}

int td5_police_block_slot(int slot)
{
    if (!P.on || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) return 0;
    return P.blk_of[slot] != 0;
}

int td5_police_block_lit(int slot) { return td5_police_block_slot(slot); }

void td5_police_block_hold(int slot)
{
    TD5_Actor *a = td5_game_get_actor(slot);
    const PolBlock *b;
    int j;
    long age;
    if (!a || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || !P.blk_of[slot]) return;
    b = &P.blk[P.blk_of[slot] - 1];
    for (j = 0; j < b->ncar && b->slot[j] != slot; j++) {}
    if (j >= b->ncar) return;
    /* parked: back on its spot, square across the road, no motion at all (a ram moves the RACER, not the block). The
     * height, roll and pitch come from the road each tick (integrate_traffic_pose). Hit hard it never scatters
     * (td5_physics_collision.c: a block car is always a tough cop). */
    a->world_pos.x = b->px[j] * 256;
    a->world_pos.z = b->pz[j] * 256;
    a->euler_accum.yaw = (b->yaw[j] & 0xFFF) << 8;
    a->euler_accum.roll = 0;
    a->euler_accum.pitch = 0;
    a->linear_velocity_x = a->linear_velocity_y = a->linear_velocity_z = 0;
    a->angular_velocity_roll = a->angular_velocity_yaw = a->angular_velocity_pitch = 0;
    a->longitudinal_speed = 0;
    a->vehicle_mode = 0;
    a->steering_command = 0;
    a->encounter_steering_cmd = 0;
    a->brake_flag = 1;
    a->handbrake_flag = 0;
    a->throttle_state = 1;
    /* pose log: the first 60 ticks after placement, then any tilt off the road (once a second) */
    age = pol_tick() - b->t0;
    if (age == 1 || age == 2 || age == 3 || age == 5 || age == 10 || age == 20 || age == 30 || age == 45 || age == 60 ||
        ((((a->display_angles.roll & 0xFFF) > 0x100 && (a->display_angles.roll & 0xFFF) < 0xF00) ||
          ((a->display_angles.pitch & 0xFFF) > 0x100 && (a->display_angles.pitch & 0xFFF) < 0xF00)) && (age % 30) == 0))
        TD5_LOG_I(LOG_TAG, "POLICE: block car %d t+%ld: world (%d,%d,%d) display roll %d pitch %d yaw %d (placed yaw %d) mode %d span %d lane %d",
                  slot, age, (int)(a->world_pos.x / 256), (int)(a->world_pos.y / 256), (int)(a->world_pos.z / 256),
                  a->display_angles.roll & 0xFFF, a->display_angles.pitch & 0xFFF, a->display_angles.yaw & 0xFFF, b->yaw[j] & 0xFFF,
                  a->vehicle_mode, (int)(int16_t)a->track_span_raw, (int)a->track_sub_lane_index);
}

int td5_police_traffic_blocked(int slot, int span, int oncoming)
{
    const int ring = td5_track_get_ring_length();
    int i;
    if (!P.on || P.queue_spans <= 0 || slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS || P.blk_of[slot]) return 0;
    for (i = 0; i < POL_BLK_MAX; i++) {
        const PolBlock *b = &P.blk[i];
        int d;
        if (!b->on || b->releasing) continue;
        d = oncoming ? span - b->span : b->span - span;
        if (g_track_is_circuit && ring > 0) { while (d > ring / 2) d -= ring; while (d < -ring / 2) d += ring; }
        if (d > 0 && d <= P.queue_spans) return 1;
    }
    return 0;
}

int td5_police_want_cop_spawn(void)
{
    int t, n = pol_racer_n(), live = -1;
    if (!P.on) return 0;
    for (t = 0; t < n; t++) {
        PolTarget *T = &P.t[t];
        if (!T->on || pol_tick() < T->reinf_cd) continue;
        if (live < 0) {
            int k;
            live = 0;
            for (k = g_traffic_slot_base; k < TD5_MAX_TOTAL_ACTORS && k < td5_game_get_total_actor_count(); k++)
                if (td5_ai_actor_is_cop(k) && !td5_ai_traffic_dynamic_parked(k) && !P.blk_of[k]) live++;
        }
        if (live >= 1 + T->heat) continue;
        T->reinf_cd = pol_tick() + 90;
        P.st.reinforcements++;
        TD5_LOG_I(LOG_TAG, "POLICE: reinforcement %ld for slot %d (heat %d, %d cop(s) on the road)", P.st.reinforcements, t, T->heat, live);
        return 1;
    }
    return 0;
}

void td5_police_on_contact(int sa, int sb, int32_t impact_mag)
{
    int h = -1, c = -1;
    if (!P.on) return;
    if (sa >= 0 && sa < TD5_MAX_TOTAL_ACTORS && P.blk_of[sa] && sb >= 0 && sb < pol_racer_n()) { h = sb; c = sa; }
    else if (sb >= 0 && sb < TD5_MAX_TOTAL_ACTORS && P.blk_of[sb] && sa >= 0 && sa < pol_racer_n()) { h = sa; c = sb; }
    if (c >= 0) {   /* a racer against a roadblock car */
        const PolBlock *b = &P.blk[P.blk_of[c] - 1];
        if (pol_human_racing(h)) P.st.block_hits++; else P.st.block_ai_hits++;
        if (b->target == h) P.blk[P.blk_of[c] - 1].contact = 1;
        return;
    }
    if (impact_mag < (int32_t)P.ram_impact) return;
    if (pol_human_racing(sa) && sb >= g_traffic_slot_base && td5_ai_actor_is_cop(sb)) { h = sa; c = sb; }
    else if (pol_human_racing(sb) && sa >= g_traffic_slot_base && td5_ai_actor_is_cop(sa)) { h = sb; c = sa; }
    if (h < 0) return;
    {   /* the RACER drove into the cop: it was the faster of the two (a cop boxing the car in is not a ram by it) */
        const TD5_Actor *A = td5_game_get_actor(h), *C = td5_game_get_actor(c);
        if (!A || !C) return;
        if (fabs((double)A->longitudinal_speed) <= fabs((double)C->longitudinal_speed)) return;
    }
    P.t[h].ram_cop = c;
    P.t[h].ram_tick = pol_tick() + 1;   /* pending: the next police tick takes it (physics runs after the AI step) */
}

int td5_police_held(int slot)
{
    if (!P.on || slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return P.t[slot].busted_t > 0;
}

int td5_police_heat(int slot)
{
    if (!P.on || slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return 0;
    return P.t[slot].on ? P.t[slot].heat : 0;
}

/* ============================================================ roadblocks ==================== */

/* free roam's block (td5_roam_cops.c rc_block_layout) with its RACE rule (always a gap: 4.2 m at heat 3, 3.6 m above,
 * a strip race always has a field to let through), but EVERY car lies wholly on the road band: a car's 4.4 m length
 * across the road stays inside [lmin + 0.3, lmax - 0.3], packed to the edges. A side too short for a car gets none
 * (the gap grows); with only one car abreast the second parks 5.2 m behind it. Returns the car count (0 = no block). */
static int pol_block_layout(int heat, double lmin, double lmax, int ncap, double *lat, double *aoff, double *gap_l, double *gap_w)
{
    const double lo = lmin + 0.3, hi = lmax - 0.3, W = hi - lo, half = POL_BLK_CAR_W * 0.5;
    const double gw = heat >= 4 ? 3.6 : 4.2;
    double gl;
    int n = 0, ka, kb, j;
    if (W < POL_BLK_CAR_W + gw) return 0;                /* not even one car beside the gap */
    if (W >= 2.0 * POL_BLK_CAR_W + gw) gl = lo + POL_BLK_CAR_W + pol_unit() * (W - 2.0 * POL_BLK_CAR_W - gw);   /* both sides */
    else gl = (pol_lcg() & 1) ? lo + POL_BLK_CAR_W : hi - POL_BLK_CAR_W - gw;                                /* one side  */
    ka = (int)floor((gl - lo) / POL_BLK_CAR_W + 1e-9);
    kb = (int)floor((hi - gl - gw) / POL_BLK_CAR_W + 1e-9);
    if (ka + kb > ncap) {
        if (ka > ncap) ka = ncap;
        if (kb > ncap - ka) kb = ncap - ka;
    }
    for (j = 0; j < ka && n < POL_BLK_CARS; j++) { lat[n] = lo + half + POL_BLK_CAR_W * j; aoff[n] = 0.0; n++; }
    for (j = 0; j < kb && n < POL_BLK_CARS; j++) { lat[n] = hi - half - POL_BLK_CAR_W * j; aoff[n] = 0.0; n++; }
    if (n == 1 && ncap >= 2) { lat[1] = lat[0]; aoff[1] = -5.2; n = 2; }
    *gap_l = gl; *gap_w = gw;
    return n;
}

static int pol_clear_of_actors(double x, double z, double r_m)
{
    const int n = td5_game_get_total_actor_count();
    int k;
    for (k = 0; k < n && k < TD5_MAX_TOTAL_ACTORS; k++) {
        const TD5_Actor *a = td5_game_get_actor(k);
        double ax, az;
        if (!a) continue;
        if (k < g_traffic_slot_base) { if (td5_game_slot_is_empty_racer(k)) continue; }
        else if (td5_ai_traffic_dynamic_parked(k)) continue;
        pol_xz(a, &ax, &az);
        if (pol_m(hypot(ax - x, az - z)) < r_m) return 0;
    }
    return 1;
}

/* 1 when no live traffic car sits within +-pad spans of span s: a car that close at speed could not stop in the
 * queue before the block (measured: 87 km/h, 5-11 spans short, drove into a block car and was launched) */
static int pol_span_traffic_clear(int s, int pad)
{
    const int ring = td5_track_get_ring_length(), n = td5_game_get_total_actor_count();
    int k;
    for (k = g_traffic_slot_base; k < n && k < TD5_MAX_TOTAL_ACTORS; k++) {
        const TD5_Actor *a;
        int d;
        if (td5_ai_traffic_dynamic_parked(k) || P.blk_of[k]) continue;
        a = td5_game_get_actor(k);
        if (!a) continue;
        d = (int)(int16_t)a->track_span_normalized - s;
        if (g_track_is_circuit && ring > 0) { while (d > ring / 2) d -= ring; while (d < -ring / 2) d += ring; }
        if (d >= -pad && d <= pad) return 0;
    }
    return 1;
}

/* 1 when span s (main ring) is clear of fork / loop mouths over +-pad spans */
static int pol_span_plain(int s, int ring, int pad)
{
    int d;
    if (td5_track_loop_near_mouth(s, pad)) return 0;
    for (d = -pad; d <= pad; d++) {
        int q = s + d;
        if (ring > 0) { q %= ring; if (q < 0) q += ring; }
        if (q < 0 || q >= td5_track_get_span_count()) return 0;
        if (td5_track_count_branch_corridors(q) > 0) return 0;
        if (td5_track_loop_departure(q, NULL) >= 0) return 0;
    }
    return 1;
}

static void pol_block_release_cars(PolBlock *b)
{
    int j;
    for (j = 0; j < b->ncar; j++) td5_ai_traffic_police_block_release(b->slot[j]);
    b->releasing = 1;
}

static void pol_block_end(int i, const char *why)
{
    PolBlock *b = &P.blk[i];
    if (!b->on || b->releasing) return;
    pol_block_release_cars(b);
    P.st.blocks_removed++;
    if (b->crossed) P.st.blocks_passed++;
    TD5_LOG_I(LOG_TAG, "POLICE: roadblock %d at span %d removed (%s) after %.1f s, closest %.0f m, %s", i, b->span, why,
              (pol_tick() - b->t0) / POL_HZ, b->min_d,
              b->crossed ? (b->contact ? "crossed touching the cars" : "crossed through the gap") : "not crossed");
}

static void pol_blocks_end_for(int target, const char *why)
{
    int i;
    for (i = 0; i < POL_BLK_MAX; i++) if (P.blk[i].on && P.blk[i].target == target) pol_block_end(i, why);
}

/* try to set a roadblock on a main-ring span block_min_m..block_max_m ahead of `target`; 1 = placed */
static int pol_block_place(int target)
{
    PolTarget *T = &P.t[target];
    const TD5_Actor *pl = td5_game_get_actor(target);
    const int ring = td5_track_get_ring_length(), nspan = td5_track_get_span_count();
    double cand_d[160];
    int    cand_s[160];
    int    nc = 0, bi, sp, k, steps;
    double acc = 0.0, px, pz, prevx, prevz;
    if (!pl || pol_on_branch(pl) || nspan <= 0) return 0;
    for (bi = 0; bi < POL_BLK_MAX && P.blk[bi].on; bi++) {}
    if (bi >= POL_BLK_MAX) return 0;
    sp = (int)(int16_t)pl->track_span_raw;
    if (sp < 0 || sp >= nspan) return 0;
    pol_xz(pl, &px, &pz);
    prevx = px; prevz = pz;
    /* walk the spans ahead: along-road distance between successive span centres */
    for (steps = 1; steps < 1200 && nc < 160; steps++) {
        double cx, cz, rx, rz, fx, fz, lmin, lmax;
        int s = sp + steps;
        if (ring > 0 && g_track_is_circuit) s %= ring;
        else if (s >= (ring > 0 ? ring : nspan) - 2) break;
        if (!pol_span_band(s, &cx, &cz, &rx, &rz, &fx, &fz, &lmin, &lmax)) continue;
        acc += pol_m(hypot(cx - prevx, cz - prevz));
        prevx = cx; prevz = cz;
        if (acc > P.block_max_m) break;
        if (acc >= P.block_min_m && (nc == 0 || acc - cand_d[nc - 1] >= 15.0)) { cand_s[nc] = s; cand_d[nc] = acc; nc++; }
    }
    if (nc == 0) return 0;
    {
        const int start = (int)(pol_unit() * nc);
        for (k = 0; k < nc; k++) {
            const int s = cand_s[(start + k) % nc];
            double cx, cz, rx, rz, fx, fz, lmin, lmax, lat[POL_BLK_CARS], aoff[POL_BLK_CARS], gl, gw;
            int32_t wx[POL_BLK_CARS], wz[POL_BLK_CARS];
            int yaw[POL_BLK_CARS], lane[POL_BLK_CARS];
            int n, j, ok = 1, nfree;
            if (!pol_span_plain(s, ring, 6)) continue;
            if (!pol_span_traffic_clear(s, 25)) continue;
            if (!pol_span_band(s, &cx, &cz, &rx, &rz, &fx, &fz, &lmin, &lmax)) continue;
            for (j = 0; j < POL_BLK_MAX; j++)
                if (P.blk[j].on && pol_m(hypot(cx - P.blk[j].cx, cz - P.blk[j].cz)) < 120.0) ok = 0;
            if (!ok) continue;
            nfree = td5_ai_traffic_police_free_slots();
            n = pol_block_layout(T->heat, lmin, lmax, T->heat >= 4 ? 3 : 2, lat, aoff, &gl, &gw);
            if (n < 2) continue;
            if (n > nfree) {
                /* the pool is out: the farthest traffic behind every human gives up its car (fades out, a later tick retries) */
                td5_ai_traffic_police_make_room(n - nfree);
                return -1;
            }
            for (j = 0; j < n && ok; j++) {
                const double jit = (j & 1 ? 1.0 : -1.0) * (0.12 + 0.1 * pol_unit());
                const double sgn = (j & 1) ? 1.0 : -1.0;
                const double ux = rx * sgn * cos(jit) + fx * sin(jit), uz = rz * sgn * cos(jit) + fz * sin(jit);
                const double x = cx + rx * lat[j] * P.upm + fx * aoff[j] * P.upm;
                const double z = cz + rz * lat[j] * P.upm + fz * aoff[j] * P.upm;
                wx[j] = (int32_t)floor(x + 0.5); wz[j] = (int32_t)floor(z + 0.5);
                yaw[j] = pol_angle_of(ux, uz);
                lane[j] = pol_lane_at(s, cx, cz, rx, rz, lat[j]);
                if (!pol_clear_of_actors(x, z, 12.0)) ok = 0;
            }
            if (!ok) continue;
            {
                PolBlock *b = &P.blk[bi];
                int placed = 0;
                memset(b, 0, sizeof *b);
                for (j = 0; j < n; j++) {
                    const int slot = td5_ai_traffic_police_block_place(s, lane[j], wx[j], wz[j], yaw[j]);
                    if (slot < 0) continue;
                    b->slot[placed] = slot; b->px[placed] = wx[j]; b->pz[placed] = wz[j]; b->yaw[placed] = yaw[j];
                    P.blk_of[slot] = (uint8_t)(bi + 1);
                    placed++;
                }
                if (placed < 2) {
                    for (j = 0; j < placed; j++) td5_ai_traffic_police_block_release(b->slot[j]);
                    b->ncar = placed;
                    b->on = 1; b->releasing = 1;      /* the marks go when the cars are parked again */
                    continue;
                }
                b->on = 1; b->target = target; b->span = s; b->ncar = placed; b->heat = T->heat;
                b->cx = cx; b->cz = cz; b->tx = fx; b->tz = fz; b->gap_w = gw;
                b->gx = cx + rx * (gl + gw * 0.5) * P.upm; b->gz = cz + rz * (gl + gw * 0.5) * P.upm;
                b->t0 = pol_tick();
                b->min_d = pol_m(hypot(cx - px, cz - pz));
                P.st.blocks_placed++; P.st.block_cars += placed;
                TD5_LOG_I(LOG_TAG, "POLICE: ROADBLOCK %ld placed for slot %d (heat %d): %d cars across %.1f m of road (%.1f..%.1f) on span %d, "
                          "%.0f m ahead along the road (%.0f m straight), gap %.1f m at lateral %.1f",
                          P.st.blocks_placed, target, T->heat, placed, lmax - lmin, lmin, lmax, s, cand_d[(start + k) % nc], b->min_d, gw,
                          gw > 0.0 ? gl + gw * 0.5 : 0.0);
                pol_toast(T, "ROADBLOCK AHEAD", 0xFFFFA040, 2.5);
                return 1;
            }
        }
    }
    return 0;
}

static void pol_blocks_tick(void)
{
    int i, active = 0;
    for (i = 0; i < POL_BLK_MAX; i++) {
        PolBlock *b = &P.blk[i];
        const TD5_Actor *pl;
        double plx, plz, d, f;
        int j, alive = 0;
        if (!b->on) continue;
        if (b->releasing) {   /* the marks stay until every car is parked again (it is held still while it fades) */
            for (j = 0; j < b->ncar; j++) {
                if (!td5_ai_traffic_dynamic_parked(b->slot[j])) alive = 1;
                else P.blk_of[b->slot[j]] = 0;
            }
            if (!alive) { for (j = 0; j < b->ncar; j++) P.blk_of[b->slot[j]] = 0; memset(b, 0, sizeof *b); }
            continue;
        }
        for (j = 0; j < b->ncar; j++) if (!td5_ai_traffic_dynamic_parked(b->slot[j])) alive = 1;
        if (!alive) { for (j = 0; j < b->ncar; j++) P.blk_of[b->slot[j]] = 0; memset(b, 0, sizeof *b); P.st.blocks_removed++; continue; }
        pl = td5_game_get_actor(b->target);
        if (!pl || !P.t[b->target].on) { pol_block_end(i, "pursuit over"); continue; }
        pol_xz(pl, &plx, &plz);
        d = pol_m(hypot(plx - b->cx, plz - b->cz));
        if (d < b->min_d) b->min_d = d;
        f = pol_m((plx - b->cx) * b->tx + (plz - b->cz) * b->tz);
        if (b->have_prev && b->prev_f < 0.0 && f >= 0.0 && d < 25.0 && !b->crossed) {
            b->crossed = 1;
            if (b->contact) P.st.blocks_rammed++; else P.st.blocks_gap++;
            TD5_LOG_I(LOG_TAG, "POLICE: slot %d crossed roadblock %d at %.0f km/h: %s", b->target, i, pol_speed_ms(pl) * 3.6,
                      b->contact ? "touching the cars" : "through the gap");
        }
        b->prev_f = f; b->have_prev = 1;
        if (b->crossed && f > 35.0) { pol_block_end(i, "passed"); continue; }
        if (d > 560.0) { pol_block_end(i, "out of range"); continue; }
        if (pol_tick() - b->t0 > 90 * 30) { pol_block_end(i, "timed out"); continue; }
        active++;
    }
    P.st.blocks_active = active;
}

/* ============================================================ tick ========================== */

static void pol_target_tick(int t)
{
    PolTarget *T = &P.t[t];
    const long tick = pol_tick();
    const TD5_Actor *pl = td5_game_get_actor(t);
    const int nact = td5_game_get_total_actor_count();
    double vpl;
    int k, seen = 0, npur = 0;
    if (!pl) return;
    vpl = pol_speed_ms(pl);
    T->dmin = 1e9; T->bdmin = 1e9;
    for (k = g_traffic_slot_base; k < nact && k < TD5_MAX_TOTAL_ACTORS; k++) {
        const TD5_Actor *c;
        double d;
        if (td5_ai_traffic_dynamic_parked(k)) continue;
        if (P.blk_of[k]) {
            const PolBlock *b = &P.blk[P.blk_of[k] - 1];
            if (b->target == t && !b->releasing && (c = td5_game_get_actor(k)) != NULL) {
                d = pol_dist_m(pl, c);
                if (d < T->bdmin) T->bdmin = d;
            }
            continue;
        }
        if (td5_ai_cop_target(k) != t) continue;
        c = td5_game_get_actor(k);
        if (!c) continue;
        npur++;
        d = pol_dist_m(pl, c);
        if (d < T->dmin) T->dmin = d;
        if (d < P.see_m && pol_on_branch(c) == pol_on_branch(pl)) seen = 1;
    }
    T->npur = npur;
    if (npur > P.st.max_pursuers) P.st.max_pursuers = npur;
    /* heat: +1 per further offence, +1 per 25 s */
    {
        int h = T->offences + (int)((tick - T->t0) / (25 * 30));
        if (h > 5) h = 5;
        if (h > T->heat) {
            T->heat = h;
            TD5_LOG_I(LOG_TAG, "POLICE: slot %d heat %d", t, T->heat);
        }
        if (T->heat > P.st.max_heat) P.st.max_heat = T->heat;
    }
    /* reinforcements: a spawn carries one when the pool has room (td5_police_want_cop_spawn); otherwise, or first,
     * a far car ahead becomes a cop. One per 3 s either way (shared cooldown). */
    if (tick >= T->reinf_cd) {
        int live = 0;
        for (k = g_traffic_slot_base; k < nact && k < TD5_MAX_TOTAL_ACTORS; k++)
            if (td5_ai_actor_is_cop(k) && !td5_ai_traffic_dynamic_parked(k) && !P.blk_of[k]) live++;
        if (live < 1 + T->heat) {
            const int r = td5_ai_traffic_police_recruit(t, 90);
            T->reinf_cd = tick + 90;
            if (r >= 0) {
                P.st.reinforcements++;
                TD5_LOG_I(LOG_TAG, "POLICE: reinforcement %ld for slot %d (heat %d, %d cop(s) on the road): traffic slot %d recruited",
                          P.st.reinforcements, t, T->heat, live, r);
            }
        }
    }
    /* bust: (nearly) stopped with a pursuer or a block car within 10 m; the meter drains at the rate it fills */
    if (vpl < 10.0 / 3.6 && (T->dmin < 10.0 || T->bdmin < 10.0)) T->bust += 1.0 / (P.bust_s * POL_HZ);
    else T->bust -= 1.0 / (P.bust_s * POL_HZ);
    if (T->bust < 0.0) T->bust = 0.0;
    if (T->bust >= 1.0) { pol_bust(t, T->bdmin < 10.0 && T->bdmin <= T->dmin ? "stopped at a roadblock" : "stopped beside a pursuer", 0); return; }
    /* escape */
    if (seen) T->lost = 0; else T->lost++;
#ifndef TD5RE_RELEASE
    if (P.dev_slow == 1 && npur > 0) T->lost = 0;   /* DEV (`police` test, roadblock phase): the pursuers held back, no escape yet */
    else
#endif
    {
        if (npur == 0 || T->dmin > P.escape_m) { pol_end(t, 1, 1); return; }
        if (T->lost > (long)((P.lose_s + 2.0 * T->heat) * POL_HZ)) { pol_end(t, 1, 0); return; }
    }
    /* roadblocks */
    if (P.block_heat > 0 && T->heat >= P.block_heat && tick >= T->block_cd) {
        int i, active = 0;
        for (i = 0; i < POL_BLK_MAX; i++) if (P.blk[i].on && !P.blk[i].releasing) active++;
        if (active < P.block_max) {
            const int r = pol_block_place(t);
            if (r > 0) T->block_cd = tick + 20 * 30;
            else if (r < 0) T->block_cd = tick + 15;
            else { P.st.block_fails++; T->block_cd = tick + 45; }
        }
    }
}

void td5_police_tick(void)
{
    const long tick = pol_tick();
    int t, n;
    if (!P.on) return;
    if (tick == P.tick_last) return;
    P.tick_last = tick;
    if (td5_game_is_replay_active()) return;
    n = pol_racer_n();
    for (t = 0; t < n; t++) {
        PolTarget *T = &P.t[t];
        if (!pol_human_racing(t)) {
            if (T->on) pol_end(t, 0, 0);
            if (T->busted_t > 0) { T->busted_t = 0; pol_release(t); }
            continue;
        }
        if (T->busted_t > 0) {
            const TD5_Actor *pl = td5_game_get_actor(t);
            P.st.hold_ticks++;
            if (pl && pol_speed_ms(pl) > 3.0 / 3.6 && T->busted_t < 60) P.st.hold_moving_ticks++;
            if (--T->busted_t == 0) pol_release(t);
            continue;
        }
        /* a ram recorded by the physics hook this tick */
        if (T->ram_tick > 0) {
            const int c = T->ram_cop;
            T->ram_tick = 0;
            if (tick >= T->grace_until) {
                if (!T->on) {
                    if (td5_ai_police_force_chase(c, t)) pol_start(t, c, 2);
                } else if (td5_ai_cop_target(c) == t && tick - T->last_offence > 60) {
                    pol_offence(t, "rammed a police car");
                }
            }
        }
        if (T->on) pol_target_tick(t);
    }
    pol_blocks_tick();
#ifndef TD5RE_RELEASE
    td5_police_test_tick();
#endif
}

/* ============================================================ race HUD ====================== */

void td5_police_draw_hud(void)
{
    const long tick = pol_tick();
    const int panes = td5_hud_pane_count();
    int v;
    if (!P.on || td5_game_is_pause_menu_active() || td5_game_is_replay_active()) return;
    for (v = 0; v < panes; v++) {
        const int slot = td5_game_get_player_slot(v);
        const PolTarget *T;
        TD5_PoliceHud h;
        char line[64];
        float x0, y0, w, hh;
        if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) continue;
        T = &P.t[slot];
        if (!T->on && T->busted_t <= 0 && tick >= T->toast_until) continue;
        if (!td5_hud_pane_rect(v, &x0, &y0, &w, &hh)) continue;
        memset(&h, 0, sizeof h);
        h.tick = tick;
        h.pursuit = T->on;
        h.heat = T->heat;
        h.cops = T->npur;
        h.evade = (float)((double)T->lost / ((P.lose_s + 2.0 * T->heat) * POL_HZ));
        h.bust = (float)T->bust;
        h.busted = T->busted_t > 0;
        if (h.busted) {
            snprintf(line, sizeof line, "HELD BY THE POLICE   %d", T->busted_t / 30 + 1);
            h.busted_line = line;
        }
        if (tick < T->toast_until) { h.toast = T->toast; h.toast_col = T->toast_col; }
        td5_police_hud_draw(&h, x0, y0, w, hh);
    }
}

/* ============================================================ stats + DEV =================== */

void td5_police_stats(TD5_PoliceStats *out)
{
    if (out) *out = P.st;
}

#ifndef TD5RE_RELEASE
void td5_police_dev_set_heat(int slot, int h)
{
    PolTarget *T;
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    T = &P.t[slot];
    if (!T->on || h < 1) return;
    if (h > 5) h = 5;
    if (T->offences < h) T->offences = h;
    if (T->heat < h) { T->heat = h; if (T->heat > P.st.max_heat) P.st.max_heat = T->heat; }
    TD5_LOG_I(LOG_TAG, "POLICE: DEV heat %d on slot %d", T->heat, slot);
}
int td5_police_dev_pursuers(int slot) { return (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) ? P.t[slot].npur : 0; }
int td5_police_dev_busted(int slot) { return td5_police_held(slot); }
int td5_police_dev_pursuit(int slot) { return (slot >= 0 && slot < TD5_MAX_RACER_SLOTS) ? P.t[slot].on : 0; }
int td5_police_dev_block(int i, int *span, int *ncar, double *gap_m)
{
    if (i < 0 || i >= POL_BLK_MAX || !P.blk[i].on || P.blk[i].releasing) return 0;
    *span = P.blk[i].span; *ncar = P.blk[i].ncar; *gap_m = P.blk[i].gap_w;
    return 1;
}
int td5_police_dev_block_gap(int i, double *gx, double *gz, double *fx, double *fz)
{
    if (i < 0 || i >= POL_BLK_MAX || !P.blk[i].on || P.blk[i].releasing) return 0;
    *gx = P.blk[i].gx; *gz = P.blk[i].gz; *fx = P.blk[i].tx; *fz = P.blk[i].tz;
    return 1;
}
void td5_police_dev_slow_pursuers(int level) { P.dev_slow = level < 0 ? 0 : (level > 2 ? 2 : level); }
int  td5_police_dev_slow(void) { return P.on ? P.dev_slow : 0; }
#endif
