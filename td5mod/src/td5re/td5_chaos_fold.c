/* ========================================================================
 * td5_chaos_fold.c — CHAOS CO-OP: N-seat -> 2-car input fold (PORT-ONLY)
 *
 * Pure function: folds the per-seat polled words into the two team-car
 * words under the current role map. Called from td5_input.c between the raw
 * poll pass and the per-slot post-processing pass. Design:
 * docs/plans/CHAOS_COOP_MODE_PLAN.md section 4.
 *
 * [CHAOS CO-OP 2026-09-29] Why this cannot be a bitwise OR of the seat
 * words: when a pad is bound the platform REPLACES the word
 * (td5_platform_win32.c:972) after collapsing both steering directions into
 * ONE packed analog axis (:926-935), so a seat that owns only LEFT still
 * emits a full signed X axis and carries no STEER_LEFT bit at all. The fold
 * therefore works in the AXIS domain: extract a signed contribution per
 * seat, apply the seat's role, sum per team, re-encode per axis.
 *
 * No globals other than the td5_chaos_* role queries (contract, td5_chaos.h).
 * ======================================================================== */
#include "td5_chaos.h"
#include "td5_input.h"   /* TD5_INPUT_JS_AXIS_CENTER — the packed-axis centre */

/* Normalised per-axis contribution domain: -256 .. +256, positive = LEFT on
 * the steering axis and THROTTLE on the pedal axis. This is exactly the
 * platform's pre-pack `steer` / `thr` domain (td5_platform_win32.c:926-927),
 * so the re-encode below is the platform's own pack step run backwards. */
#define CHAOS_AXIS_FULL 256

/* Packed analog axis field width (bits 0-8 for X, bits 9-17 for Y). */
#define CHAOS_AXIS_MASK 0x1FF

static int chaos_clamp_axis(int v)
{
    if (v >  CHAOS_AXIS_FULL) return  CHAOS_AXIS_FULL;
    if (v < -CHAOS_AXIS_FULL) return -CHAOS_AXIS_FULL;
    return v;
}

/* Signed steering contribution of one seat: +256 = full left, -256 = full
 * right. Analog whenever the seat's word carries the packed X axis (every pad
 * seat does), digital otherwise (keyboard seats, and the inputscript /
 * control-socket overlays, which only ever OR digital bits in).
 *
 * The digital bits are also honoured when the analog axis reads exactly
 * centre: a pad seat contributes nothing at that point, so letting the
 * overlay bits through costs no fidelity and keeps the control-socket
 * `hold_action left|right` harness working on pad-bound seats too. */
static int chaos_seat_steer(uint32_t w, int ax)
{
    int v = 0;
    if (w & (uint32_t)TD5_INPUT_ANALOG_X_FLAG)
        v = (ax * CHAOS_AXIS_FULL) / TD5_INPUT_JS_AXIS_CENTER;
    if (v == 0) {
        if (w & (uint32_t)TD5_INPUT_STEER_LEFT)  v += CHAOS_AXIS_FULL;
        if (w & (uint32_t)TD5_INPUT_STEER_RIGHT) v -= CHAOS_AXIS_FULL;
    }
    return chaos_clamp_axis(v);
}

/* Signed pedal contribution of one seat: +256 = full throttle, -256 = full
 * brake. `ay` is the re-centred packed Y axis, which the platform builds as
 * `centre - thr` — so ay is POSITIVE when braking and the sign flips here. */
static int chaos_seat_pedal(uint32_t w, int ay)
{
    int v = 0;
    if (w & (uint32_t)TD5_INPUT_ANALOG_Y_FLAG)
        v = (-ay * CHAOS_AXIS_FULL) / TD5_INPUT_JS_AXIS_CENTER;
    if (v == 0) {
        if (w & (uint32_t)TD5_INPUT_THROTTLE) v += CHAOS_AXIS_FULL;
        if (w & (uint32_t)TD5_INPUT_BRAKE)    v -= CHAOS_AXIS_FULL;
    }
    return chaos_clamp_axis(v);
}

int td5_chaos_fold_inputs(const uint32_t *seat_bits, const int16_t *seat_ax,
                          const int16_t *seat_ay, int seat_count,
                          uint32_t *out_bits, int16_t *out_ax, int16_t *out_ay) {
    int n, t, s;
    uint32_t shared_all;

    if (!td5_chaos_active()) return 0;
    if (!seat_bits || !seat_ax || !seat_ay || !out_bits || !out_ax || !out_ay)
        return 0;

    n = seat_count;
    if (n > td5_chaos_seat_count()) n = td5_chaos_seat_count();
    if (n > TD5_CHAOS_MAX_SEATS)    n = TD5_CHAOS_MAX_SEATS;
    if (n <= 0) return 0;

    /* Pause / Escape is unowned across BOTH teams (section 2.3): any of the
     * 4-8 seats can open the pause menu or leave the race, and the bit is
     * written into BOTH car words so the two-pane race pauses as one. */
    shared_all = 0;
    for (s = 0; s < n; s++)
        shared_all |= seat_bits[s] &
                      (uint32_t)(TD5_INPUT_PAUSE | TD5_INPUT_ESCAPE);

    for (t = 0; t < TD5_CHAOS_TEAMS; t++) {
        uint32_t bits = shared_all;
        int steer = 0, pedal = 0;
        int steer_analog = 0, pedal_analog = 0;
        int steer_owned = 0, pedal_owned = 0;
        int hb_owner;

        for (s = 0; s < n; s++) {
            uint32_t w;
            int v;

            if (td5_chaos_team_of_seat(s) != t) continue;
            w = seat_bits[s];

            /* Team-shared actions (section 2.3): horn, camera change and rear
             * view fire from ANY seat on the team. RECOVER rides along as a
             * team action for the same reason — a chaos car with no reachable
             * recovery would stay wedged (it is not in the 2.3 table; the
             * post-processing pass in td5_input.c ORs its own device on top
             * rather than clearing this). */
            bits |= w & (uint32_t)(TD5_INPUT_HORN | TD5_INPUT_CAMERA_CHANGE |
                                   TD5_INPUT_REAR_VIEW | TD5_INPUT_RECOVER);

            switch (td5_chaos_role_of_seat(s)) {
            case TD5_CHAOS_ROLE_STEER:      /* team of 2: both directions */
                steer += chaos_seat_steer(w, seat_ax[s]);
                steer_owned = 1;
                steer_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_X_FLAG) != 0;
                break;
            case TD5_CHAOS_ROLE_LEFT:       /* pushing right does nothing */
                v = chaos_seat_steer(w, seat_ax[s]);
                if (v > 0) steer += v;
                steer_owned = 1;
                steer_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_X_FLAG) != 0;
                break;
            case TD5_CHAOS_ROLE_RIGHT:      /* pushing left does nothing */
                v = chaos_seat_steer(w, seat_ax[s]);
                if (v < 0) steer += v;
                steer_owned = 1;
                steer_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_X_FLAG) != 0;
                break;
            case TD5_CHAOS_ROLE_PEDALS:     /* teams of 2-3: both pedals */
                pedal += chaos_seat_pedal(w, seat_ay[s]);
                pedal_owned = 1;
                pedal_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_Y_FLAG) != 0;
                break;
            case TD5_CHAOS_ROLE_THROTTLE:
                v = chaos_seat_pedal(w, seat_ay[s]);
                if (v > 0) pedal += v;
                pedal_owned = 1;
                pedal_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_Y_FLAG) != 0;
                break;
            case TD5_CHAOS_ROLE_BRAKE:
                v = chaos_seat_pedal(w, seat_ay[s]);
                if (v < 0) pedal += v;
                pedal_owned = 1;
                pedal_analog |= (w & (uint32_t)TD5_INPUT_ANALOG_Y_FLAG) != 0;
                break;
            default:
                break;                      /* ROLE_NONE: seat contributes no axis */
            }
        }

        /* Two people fighting over the same axis cancel out. That is the mode
         * working as designed (EXPECTED_BEHAVIOR.md), not a bug. */
        steer = chaos_clamp_axis(steer);
        pedal = chaos_clamp_axis(pedal);

        /* Decision D1 (section 4.4): each axis re-encodes DIGITAL when every
         * contributor to it was digital, so an all-keyboard team still gets
         * the digital steering ramp (Path A) instead of instant full lock;
         * ANALOG as soon as one contributor is a pad, with any digital
         * contributor counting as a full 256. The X and Y flags are
         * independent, so the two axes decide independently. */
        if (steer_owned && steer_analog) {
            int axv = TD5_INPUT_JS_AXIS_CENTER +
                      (steer * TD5_INPUT_JS_AXIS_CENTER) / CHAOS_AXIS_FULL;
            if (axv < 0) axv = 0;
            if (axv > CHAOS_AXIS_MASK) axv = CHAOS_AXIS_MASK;
            bits |= ((uint32_t)axv & CHAOS_AXIS_MASK) |
                    (uint32_t)TD5_INPUT_ANALOG_X_FLAG;
            out_ax[t] = (int16_t)(axv - TD5_INPUT_JS_AXIS_CENTER);
        } else {
            if (steer > 0)      bits |= (uint32_t)TD5_INPUT_STEER_LEFT;
            else if (steer < 0) bits |= (uint32_t)TD5_INPUT_STEER_RIGHT;
            out_ax[t] = 0;
        }

        if (pedal_owned && pedal_analog) {
            int ayv = TD5_INPUT_JS_AXIS_CENTER -
                      (pedal * TD5_INPUT_JS_AXIS_CENTER) / CHAOS_AXIS_FULL;
            if (ayv < 0) ayv = 0;
            if (ayv > CHAOS_AXIS_MASK) ayv = CHAOS_AXIS_MASK;
            bits |= (((uint32_t)ayv & CHAOS_AXIS_MASK) << 9) |
                    (uint32_t)TD5_INPUT_ANALOG_Y_FLAG;
            out_ay[t] = (int16_t)(ayv - TD5_INPUT_JS_AXIS_CENTER);
        } else {
            if (pedal > 0)      bits |= (uint32_t)TD5_INPUT_THROTTLE;
            else if (pedal < 0) bits |= (uint32_t)TD5_INPUT_BRAKE;
            out_ay[t] = 0;
        }

        /* Handbrake belongs to the BRAKE owner in a team of 4 and to the
         * PEDALS owner in a team of 2-3 (section 2.3) — exactly "whoever owns
         * the brake half of the pedal axis". */
        hb_owner = td5_chaos_owner_of_role(t, TD5_CHAOS_ROLE_BRAKE);
        if (hb_owner < 0)
            hb_owner = td5_chaos_owner_of_role(t, TD5_CHAOS_ROLE_PEDALS);
        if (hb_owner >= 0 && hb_owner < n)
            bits |= seat_bits[hb_owner] & (uint32_t)TD5_INPUT_HANDBRAKE;

        /* GEAR_UP / GEAR_DOWN are deliberately never set: no seat owns the
         * gearbox (section 2.3). Bit 28 (the auto/manual select read by
         * td5_input_update_player_control) is likewise left CLEAR = AUTO, and
         * the post-processing pass in td5_input.c forces want_manual = 0 in
         * this mode so it cannot set the bit back afterwards. */

        out_bits[t] = bits;
    }

    return 1;
}
