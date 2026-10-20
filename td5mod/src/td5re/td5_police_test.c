/**
 * td5_police_test.c -- the `police` self-test drive + verdict (PORT-ONLY, DEV-ONLY, round H HH).
 * Contract: td5_police.h (DEV section). A stock track, POLICE on, dynamic traffic on, slot 0 a HUMAN: lane assist
 * steers, this module's THROTTLE / BRAKE bits drive (ORed in by td5_input.c like the other dev drives), through the
 * real input path, the stock police scheduler, the pursuit layer, physics and contacts.
 *
 *   A. PURSUIT. Full throttle; at 100 km/h an idle cop is planted 45 spans ahead in a down-track lane beside the car
 *      (td5_ai_traffic_police_dev_plant_cop). Passing it at speed is the stock trigger: the pursuit must start.
 *   B. HEAT + ROADBLOCK. The heat is raised to 3 (DEV) and the drive goes on at full throttle: a roadblock must be set
 *      up 200..400 m ahead; the car drives at it (lane assist keeps its lane, so it crosses through the gap or
 *      touching the cars), the block must be removed once passed.
 *   C. ESCAPE. The pursuers are held back (DEV: they brake), the car keeps going: the escape rule must fire (lost
 *      sight for 6 + 2 x heat s, or 320 m).
 *   D. BUST. After the grace a second cop is planted ahead and passed; once it chases, the car brakes to a stop and
 *      waits. The cop comes up (overtake -> pull-over, or the PULL OVER meter): BUSTED. During the 3 s hold the car
 *      presses THROTTLE and must not move; after the release it must drive off again.
 *
 * Shots (log dir; TD5RE_D3D12_CAPTURE=1 arms the capture): police_pursuit_{a,b}.png (rear view, a pursuer 8..60 m
 * behind), police_roadblock.png (the block 50..110 m ahead), police_escape.png (the ESCAPED toast),
 * police_busted.png (the BUSTED banner).
 *
 * ONE file-scope struct. Compiled out of RELEASE.
 */
#ifndef TD5RE_RELEASE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_config.h"
#include "td5re.h"
#include "td5_types.h"
#include "td5_race_state.h"
#include "td5_input.h"
#include "td5_ai.h"
#include "td5_police.h"
#include "../../../re/include/td5_actor_struct.h"

#define LOG_TAG "ai"
#define PT_HZ 30.0

enum { PT_IDLE = 0, PT_WARM, PT_WAIT_PURSUIT, PT_BLOCK, PT_ESCAPE, PT_GRACE, PT_WARM2, PT_WAIT_PURSUIT2, PT_STOP, PT_HELD, PT_GO, PT_DONE };
static const char *const k_phase[] = { "idle", "warm", "wait pursuit", "roadblock", "escape", "grace", "warm 2", "wait pursuit 2",
                                       "stop", "held", "go", "done" };

static struct {
    int    mode, phase, done, failed;
    char   why[96];
    long   t0, phase_t0, t_pursuit, t_block, t_block_gone, t_escape, t_pursuit2, t_bust, t_release, t_go;
    int    cop_a, cop_b, plants;
    int    block_seen, block_span, block_ncar; double block_gap;
    double kmh_max, kmh_go, held_kmh_max;
    int    rear, shot_p, shot_blk, shot_esc, shot_bust;
    uint32_t bits;
    TD5_PoliceStats st0;
} T;

static long pt_tick(void) { return (long)g_td5.simulation_tick_counter; }

static double pt_kmh(void)
{
    const TD5_Actor *a = td5_game_get_actor(0);
    if (!a) return 0.0;
    return fabs((double)a->longitudinal_speed) / 778.0;   /* the speedometer's KPH formula (long_speed * 256 / 778 >> 8) */
}

static void pt_shot(const char *name)
{
    char path[512];
    const char *dir = td5_plat_log_dir();
    snprintf(path, sizeof path, "%spolice_%s.png", dir ? dir : "", name);
    td5_plat_request_frame_dump(path);
    TD5_LOG_I(LOG_TAG, "[POLICETEST] shot %s -> %s", name, path);
}

static void pt_phase(int p)
{
    TD5_LOG_I(LOG_TAG, "[POLICETEST] %s -> %s at %.1f s, %.0f km/h", k_phase[T.phase], k_phase[p], (pt_tick() - T.t0) / PT_HZ, pt_kmh());
    T.phase = p;
    T.phase_t0 = pt_tick();
}

static void pt_fail(const char *why)
{
    if (T.failed) return;
    T.failed = 1;
    snprintf(T.why, sizeof T.why, "%s (%s)", why, k_phase[T.phase]);
    TD5_LOG_W(LOG_TAG, "[POLICETEST] FAIL: %s", T.why);
    pt_phase(PT_DONE);
    T.done = 1;
}

void td5_police_test_arm(int mode)
{
    memset(&T, 0, sizeof T);
    T.mode = mode;
    T.cop_a = T.cop_b = -1;
    td5_police_dev_slow_pursuers(0);
}

void td5_police_test_disarm(void)
{
    T.mode = 0;
    T.bits = 0;
    td5_police_dev_slow_pursuers(0);
}

int td5_police_test_mode(void) { return T.mode; }
int td5_police_test_done(void) { return T.mode && T.done; }
uint32_t td5_police_test_bits(void) { return T.mode ? T.bits : 0; }

/* rear view while a pursuer sits 8..60 m behind: 2 frames 4 ticks apart (the strobe flashes) */
static uint32_t pt_rear_shots(void)
{
    if (T.shot_p) return 0;
    if (T.rear > 0) {
        ++T.rear;
        if (T.rear == 25) pt_shot("pursuit_a");
        if (T.rear == 29) pt_shot("pursuit_b");
        if (T.rear >= 30) { T.rear = 0; T.shot_p = 1; }
        return (uint32_t)TD5_INPUT_REAR_VIEW;
    }
    {
        const TD5_Actor *pl = td5_game_get_actor(0);
        int k;
        if (!pl) return 0;
        for (k = g_traffic_slot_base; k < td5_game_get_total_actor_count() && k < TD5_MAX_TOTAL_ACTORS; k++) {
            const TD5_Actor *c = td5_game_get_actor(k);
            double dx, dz, d, f;
            if (!c || td5_ai_cop_target(k) != 0) continue;
            dx = (double)(c->world_pos.x - pl->world_pos.x) / 256.0;
            dz = (double)(c->world_pos.z - pl->world_pos.z) / 256.0;
            d = sqrt(dx * dx + dz * dz) / 330.0;
            {
                const double hy = (double)((((int)pl->euler_accum.yaw) >> 8) & 0xFFF) * (2.0 * 3.14159265358979323846 / 4096.0);
                f = (dx * sin(hy) + dz * cos(hy)) / 330.0;
            }
            if (d > 8.0 && d < 60.0 && f < -4.0) { T.rear = 1; break; }
        }
    }
    return 0;
}

void td5_police_test_tick(void)
{
    const long tick = pt_tick();
    const double kmh = pt_kmh();
    TD5_PoliceStats st;
    uint32_t bits = (uint32_t)TD5_INPUT_THROTTLE;
    const long cap = (long)((T.mode == TD5_POLICE_TEST_FULL ? 360.0 : 300.0) * PT_HZ);
    if (!T.mode || T.done) { T.bits = 0; return; }
    if (g_td5.game_state != TD5_GAMESTATE_RACE) return;
    td5_police_stats(&st);
    if (T.phase == PT_IDLE) {
        if (!td5_police_active()) { pt_fail("the pursuit layer is off this race"); return; }
        T.t0 = tick; T.st0 = st;
        pt_phase(PT_WARM);
    }
    if (kmh > T.kmh_max) T.kmh_max = kmh;
    if (tick - T.t0 > cap) { pt_fail("timeout"); return; }
    switch (T.phase) {
    case PT_WARM:
    case PT_WARM2:
        if (tick - T.phase_t0 > 40 * 30) { pt_fail("never reached 100 km/h"); return; }
        if (kmh >= 100.0 && tick - T.phase_t0 > 60) {
            const int slot = td5_ai_traffic_police_dev_plant_cop(45);
            if (slot < 0) { if (tick - T.phase_t0 > 30 * 30) pt_fail("no free traffic slot for the cop"); break; }
            T.plants++;
            if (T.phase == PT_WARM) { T.cop_a = slot; pt_phase(PT_WAIT_PURSUIT); }
            else { T.cop_b = slot; pt_phase(PT_WAIT_PURSUIT2); }
        }
        break;
    case PT_WAIT_PURSUIT:
        if (td5_police_dev_pursuit(0)) {
            T.t_pursuit = tick;
            td5_police_dev_set_heat(0, 3);
            pt_phase(PT_BLOCK);
        } else if (tick - T.phase_t0 > 25 * 30) {
            /* the planted cop may have been passed too slowly or out of sight: plant another, at most 3 */
            if (T.plants >= 3) { pt_fail("no pursuit after 3 cops"); return; }
            pt_phase(PT_WARM);
        }
        break;
    case PT_BLOCK:
        bits |= pt_rear_shots();
        if (!td5_police_dev_pursuit(0)) {
            /* the pursuit ended on its own before the block was seen through (an escape at full speed): fine, go on */
            if (!T.block_seen) TD5_LOG_W(LOG_TAG, "[POLICETEST] the pursuit ended before a roadblock was placed");
            T.t_escape = tick;
            pt_phase(PT_GRACE);
            break;
        }
        {
            int span, ncar;
            double gap;
            if (td5_police_dev_block(0, &span, &ncar, &gap) || td5_police_dev_block(1, &span, &ncar, &gap)) {
                if (!T.block_seen) {
                    T.block_seen = 1; T.block_span = span; T.block_ncar = ncar; T.block_gap = gap; T.t_block = tick;
                    TD5_LOG_I(LOG_TAG, "[POLICETEST] roadblock on span %d, %d cars, gap %.1f m", span, ncar, gap);
                }
                if (!T.shot_blk) {
                    const TD5_Actor *pl = td5_game_get_actor(0);
                    const int d = pl ? span - (int)(int16_t)pl->track_span_normalized : 0;
                    if (d > 12 && d < 30) { pt_shot("roadblock"); T.shot_blk = 1; }
                }
            } else if (T.block_seen && !T.t_block_gone) {
                T.t_block_gone = tick;
                TD5_LOG_I(LOG_TAG, "[POLICETEST] roadblock gone after %.1f s", (tick - T.t_block) / PT_HZ);
            }
        }
        if ((T.t_block_gone && tick - T.t_block_gone > 60) || tick - T.phase_t0 > 60 * 30) {
            if (!T.block_seen) TD5_LOG_W(LOG_TAG, "[POLICETEST] no roadblock in 60 s of heat 3");
            td5_police_dev_slow_pursuers(1);
            pt_phase(PT_ESCAPE);
        }
        break;
    case PT_ESCAPE:
        if (!td5_police_dev_pursuit(0)) {
            T.t_escape = tick;
            td5_police_dev_slow_pursuers(0);
            pt_phase(PT_GRACE);
        } else if (tick - T.phase_t0 > 45 * 30) { pt_fail("no escape in 45 s with the pursuers held back"); return; }
        break;
    case PT_GRACE:
        if (!T.shot_esc && tick - T.phase_t0 == 20) { pt_shot("escape"); T.shot_esc = 1; }
        if (tick - T.phase_t0 > 7 * 30) pt_phase(PT_WARM2);
        break;
    case PT_WAIT_PURSUIT2:
        if (td5_police_dev_pursuit(0)) {
            T.t_pursuit2 = tick;
            pt_phase(PT_STOP);
        } else if (tick - T.phase_t0 > 25 * 30) {
            if (T.plants >= 5) { pt_fail("no second pursuit"); return; }
            pt_phase(PT_WARM2);
        }
        break;
    case PT_STOP:
        /* brake to rest, then wait for the cop */
        bits = kmh > 3.0 ? (uint32_t)TD5_INPUT_BRAKE : 0u;
        if (td5_police_dev_busted(0)) {
            T.t_bust = tick;
            pt_phase(PT_HELD);
        } else if (!td5_police_dev_pursuit(0) && tick - T.phase_t0 > 30) {
            /* the pursuit ended without a bust (escaped while standing?): plant again */
            if (T.plants >= 5) { pt_fail("the second pursuit ended without a bust"); return; }
            pt_phase(PT_WARM2);
        } else if (tick - T.phase_t0 > 40 * 30) { pt_fail("no bust in 40 s standing still"); return; }
        break;
    case PT_HELD:
        if (!T.shot_bust && tick - T.phase_t0 == 20) { pt_shot("busted"); T.shot_bust = 1; }
        /* THROTTLE pressed through the hold: the car must stay put */
        if (tick - T.phase_t0 > 30 && kmh > T.held_kmh_max) T.held_kmh_max = kmh;
        if (!td5_police_dev_busted(0)) {
            T.t_release = tick;
            pt_phase(PT_GO);
        }
        break;
    case PT_GO:
        if (kmh > T.kmh_go) T.kmh_go = kmh;
        if (tick - T.phase_t0 > 6 * 30) {
            T.t_go = tick;
            pt_phase(PT_DONE);
            T.done = 1;
        }
        break;
    default:
        break;
    }
    T.bits = bits;
}

int td5_police_test_verdict(char *note, size_t n)
{
    TD5_PoliceStats s;
    char bad[160] = "";
    size_t bl = 0;
    int status = 0;
    long pur, blk, esc, bus, rel;
    td5_police_stats(&s);
    pur = s.pursuits - T.st0.pursuits;
    blk = s.blocks_placed - T.st0.blocks_placed;
    esc = s.escapes - T.st0.escapes;
    bus = s.busts - T.st0.busts;
    rel = s.releases - T.st0.releases;
#define PT_BAD(...) do { if (bl < sizeof bad - 1) bl += (size_t)snprintf(bad + bl, sizeof bad - bl, __VA_ARGS__); } while (0)
    if (!T.done) PT_BAD("not finished (%s); ", k_phase[T.phase]);
    else if (T.failed) PT_BAD("%s; ", T.why);
    if (pur < 2) PT_BAD("pursuits %ld < 2; ", pur);
    if (s.max_heat < 3) PT_BAD("max heat %d < 3; ", s.max_heat);
    if (blk < 1) PT_BAD("no roadblock; ");
    if (esc < 1) PT_BAD("no escape; ");
    if (bus < 1) PT_BAD("no bust; ");
    if (bus >= 1 && rel < 1) PT_BAD("never released; ");
    if (T.held_kmh_max > 3.0) PT_BAD("moved %.0f km/h in the hold; ", T.held_kmh_max);
    if (T.t_release && T.kmh_go < 30.0) PT_BAD("drove off at %.0f km/h only; ", T.kmh_go);
#undef PT_BAD
    if (bad[0]) status = 2;
    else if (blk >= 1 && s.blocks_passed < 1) status = 1;   /* placed but the drive never crossed it */
    snprintf(note, n, "%spursuit %ld heat %d block %ld (pass %ld gap %ld touch %ld) esc %ld bust %ld (po %ld m %ld)%s%.60s",
             status == 2 ? "FAIL " : "", pur, s.max_heat, blk, s.blocks_passed, s.blocks_gap, s.blocks_rammed, esc, bus,
             s.busts_pullover, s.busts_meter, bad[0] ? ": " : "", bad);
    TD5_LOG_I(LOG_TAG, "[POLICETEST] VERDICT %s: %s | notices speed %ld ram %ld joins %ld (behind %ld) offences %ld reinforcements %ld | "
              "escapes los %ld dist %ld | blocks %ld cars %ld gap %ld touched %ld passed %ld removed %ld fails %ld hits %ld ai hits %ld | "
              "hold %ld ticks (%ld moving) held max %.1f km/h, drive-off %.0f km/h | max pursuers %d | player vmax %.0f km/h | "
              "t: pursuit %.1f block %.1f gone %.1f escape %.1f pursuit2 %.1f bust %.1f release %.1f",
              status == 2 ? "FAIL" : (status ? "WARN" : "PASS"), note, s.notice_speed, s.notice_ram, s.joins, s.joins_behind,
              s.offences, s.reinforcements, s.escapes_los, s.escapes_dist, s.blocks_placed, s.block_cars, s.blocks_gap,
              s.blocks_rammed, s.blocks_passed, s.blocks_removed, s.block_fails, s.block_hits, s.block_ai_hits, s.hold_ticks,
              s.hold_moving_ticks, T.held_kmh_max, T.kmh_go, s.max_pursuers, T.kmh_max,
              T.t_pursuit ? (T.t_pursuit - T.t0) / PT_HZ : -1.0, T.t_block ? (T.t_block - T.t0) / PT_HZ : -1.0,
              T.t_block_gone ? (T.t_block_gone - T.t0) / PT_HZ : -1.0, T.t_escape ? (T.t_escape - T.t0) / PT_HZ : -1.0,
              T.t_pursuit2 ? (T.t_pursuit2 - T.t0) / PT_HZ : -1.0, T.t_bust ? (T.t_bust - T.t0) / PT_HZ : -1.0,
              T.t_release ? (T.t_release - T.t0) / PT_HZ : -1.0);
    return status;
}

#endif /* TD5RE_RELEASE */
