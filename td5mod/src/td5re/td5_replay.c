/* td5_replay.c -- full ghost-state "View Replay" recorder (PORT-ONLY).
 *
 * Records the actual per-tick transform of every actor (player, AI opponents,
 * traffic) during the real race and poses them straight back on "View Replay",
 * with the sim (AI/physics/traffic-spawn) fully disabled so the replay cannot
 * deviate. See td5_replay.h for the full rationale and lifecycle.
 */
#include "td5_types.h"          /* g_traffic_slot_base, TD5_MAX_* */
#include "td5_platform.h"       /* TD5_LOG_* */
#include "td5_race_state.h"     /* td5_game_get_actor, td5_game_get_total_actor_count */
#include "td5_ai.h"             /* td5_ai_traffic_get_draw_alpha, td5_ai_traffic_replay_force */
#include "td5_config.h"         /* td5_env_flag_on */
#include "td5_replay.h"
#include "../../../re/include/td5_actor_struct.h"  /* TD5_Actor field layout */

#include <stdlib.h>
#include <string.h>

#define LOG_TAG "replay"

/* Safety cap on recorded live sub-ticks (~20 min at the 30 Hz fixed tick).
 * A full snapshot is ~130 bytes/actor; 12 actors * 36000 ticks ~= 56 MB worst
 * case. Real races are a fraction of this; recording freezes (stops growing)
 * once the cap is hit rather than allocating without bound. */
#define TD5_REPLAY_MAX_TICKS  36000

/* Per-actor per-tick snapshot. Plain struct (internal buffer -- no on-disk
 * layout, so natural alignment is fine). Carries the pose plus the cosmetic
 * fields the renderer derives per frame (wheels, taillights, tire spin) so a
 * posed car looks alive, not frozen. */
typedef struct {
    TD5_Vec3_Fixed    world_pos;            /* +0x1FC */
    TD5_Vec3_Float    render_pos;           /* +0x144 */
    TD5_Mat3x3        rotation_matrix;      /* +0x120 */
    TD5_EulerAccum    euler_accum;          /* +0x1F0 */
    TD5_DisplayAngles display_angles;       /* +0x208 */
    int32_t           longitudinal_speed;   /* +0x314 (engine audio / HUD speed) */
    int16_t           slip_x;               /* +0x340 accumulated_tire_slip_x */
    int16_t           slip_z;               /* +0x342 accumulated_tire_slip_z */
    int16_t           slip_metric;          /* +0x33C current_slip_metric */
    int16_t           traffic_alpha;        /* recorded draw-alpha; -1 = racer/not traffic */
    /* Track-position block (+0x080..0x08C). Normally produced each tick by the
     * physics/track walker -- which ghost replay SKIPS -- so it must be recorded
     * and restored or it freezes on the countdown-end value. The span-keyed
     * cinematic trackside replay camera (SelectTracksideCameraProfile reads
     * actor+0x80) then never cuts between shots, and race-order / lap / minimap
     * read stale span data. */
    int16_t           track_span_raw;         /* +0x080 span the replay camera keys on */
    int16_t           track_span_normalized;  /* +0x082 */
    int16_t           track_span_accumulated; /* +0x084 */
    int16_t           track_span_high_water;  /* +0x086 race-order sort key */
    int16_t           track_contact_vertex_A; /* +0x088 */
    int16_t           track_contact_vertex_B; /* +0x08A */
    uint8_t           track_sub_lane_index;   /* +0x08C */
    uint8_t           wheel_display_angles_pad;/* keep the 16-short array 2-aligned */
    int16_t           wheel_display_angles[16]; /* +0x210 (4 wheels x 4 shorts) */
    uint8_t           current_gear;         /* +0x36B */
    uint8_t           brake_flag;           /* +0x36D */
    uint8_t           handbrake_flag;       /* +0x36E */
    uint8_t           _pad;
} TD5_ReplaySnap;

/* ---- Recorder / player state (process-lifetime; buffer survives the gap
 * between the recorded race ending and the View-Replay race starting) ------- */
static int             s_ghost_init;        /* env flag resolved yet? */
static int             s_ghost_enabled;     /* cached TD5RE_GHOST_REPLAY */
static TD5_ReplaySnap *s_buf;               /* flat [frame*actor_count + slot] */
static int             s_actor_count;       /* actors captured per frame */
static int             s_frame_count;       /* frames captured */
static int             s_frame_capacity;    /* frames allocated */
static int             s_recording;         /* armed for record this race */
static int             s_capped_logged;     /* one-shot cap warning */

int td5_replay_ghost_enabled(void)
{
    if (!s_ghost_init) {
        s_ghost_enabled = td5_env_flag_on("TD5RE_GHOST_REPLAY");  /* default ON */
        s_ghost_init = 1;
        TD5_LOG_I(LOG_TAG, "ghost-state replay %s (TD5RE_GHOST_REPLAY)",
                  s_ghost_enabled ? "enabled" : "disabled (legacy input re-sim)");
    }
    return s_ghost_enabled;
}

int td5_replay_is_recording(void) { return s_recording; }
int td5_replay_frame_count(void)  { return s_frame_count; }

int td5_replay_playback_at_end(uint32_t sim_tick)
{
    return (s_frame_count > 0) && ((int)sim_tick >= s_frame_count);
}

/* Grow the frame buffer to hold at least `frames` frames (doubling). Returns 1
 * on success, 0 if allocation failed (recording then freezes). */
static int replay_reserve(int frames)
{
    int new_cap;
    TD5_ReplaySnap *nb;
    if (frames <= s_frame_capacity) return 1;
    new_cap = s_frame_capacity ? s_frame_capacity : 512;
    while (new_cap < frames) new_cap *= 2;
    nb = (TD5_ReplaySnap *)realloc(s_buf,
             (size_t)new_cap * (size_t)s_actor_count * sizeof(TD5_ReplaySnap));
    if (!nb) {
        TD5_LOG_E(LOG_TAG, "replay buffer realloc failed at %d frames -- recording stops", frames);
        return 0;
    }
    s_buf = nb;
    s_frame_capacity = new_cap;
    return 1;
}

void td5_replay_begin_record(void)
{
    if (!td5_replay_ghost_enabled()) return;
    s_actor_count = td5_game_get_total_actor_count();
    if (s_actor_count <= 0 || s_actor_count > TD5_MAX_TOTAL_ACTORS)
        s_actor_count = TD5_MAX_TOTAL_ACTORS;
    /* Reset the buffer for a fresh recording. Keep any prior allocation only if
     * the actor count matches; otherwise the per-frame stride changed, so free
     * and start over. */
    free(s_buf);
    s_buf = NULL;
    s_frame_capacity = 0;
    s_frame_count = 0;
    s_capped_logged = 0;
    s_recording = 1;
    TD5_LOG_I(LOG_TAG, "recording begin: actors/frame=%d", s_actor_count);
}

void td5_replay_begin_playback(void)
{
    if (!td5_replay_ghost_enabled()) return;
    s_recording = 0;
    if (s_frame_count <= 0 || !s_buf) {
        TD5_LOG_W(LOG_TAG, "playback begin: no recorded frames -- replay will pose nothing");
        return;
    }
    TD5_LOG_I(LOG_TAG, "playback begin: frames=%d actors/frame=%d", s_frame_count, s_actor_count);
}

/* Snapshot one actor into `dst`. */
static void replay_capture_actor(TD5_ReplaySnap *dst, const TD5_Actor *a, int slot)
{
    dst->world_pos          = a->world_pos;
    dst->render_pos         = a->render_pos;
    dst->rotation_matrix    = a->rotation_matrix;
    dst->euler_accum        = a->euler_accum;
    dst->display_angles     = a->display_angles;
    dst->longitudinal_speed = a->longitudinal_speed;
    dst->slip_x             = a->accumulated_tire_slip_x;
    dst->slip_z             = a->accumulated_tire_slip_z;
    dst->slip_metric        = a->current_slip_metric;
    dst->current_gear       = a->current_gear;
    dst->brake_flag         = a->brake_flag;
    dst->handbrake_flag     = a->handbrake_flag;
    dst->_pad               = 0;
    dst->track_span_raw         = a->track_span_raw;
    dst->track_span_normalized  = a->track_span_normalized;
    dst->track_span_accumulated = a->track_span_accumulated;
    dst->track_span_high_water  = a->track_span_high_water;
    dst->track_contact_vertex_A = a->track_contact_vertex_A;
    dst->track_contact_vertex_B = a->track_contact_vertex_B;
    dst->track_sub_lane_index   = a->track_sub_lane_index;
    dst->wheel_display_angles_pad = 0;
    memcpy(dst->wheel_display_angles, a->wheel_display_angles,
           sizeof(dst->wheel_display_angles));
    /* Traffic slots record their exact per-tick draw alpha so fade-in/out and
     * despawn (alpha 0 = invisible) reproduce without running the spawner. */
    if (slot >= g_traffic_slot_base)
        dst->traffic_alpha = (int16_t)td5_ai_traffic_get_draw_alpha(slot);
    else
        dst->traffic_alpha = -1;
}

void td5_replay_record_tick(uint32_t sim_tick)
{
    int slot;
    TD5_ReplaySnap *frame;
    if (!s_recording) return;
    if ((int)sim_tick >= TD5_REPLAY_MAX_TICKS) {
        if (!s_capped_logged) {
            TD5_LOG_W(LOG_TAG, "recording hit cap %d ticks -- freezing buffer", TD5_REPLAY_MAX_TICKS);
            s_capped_logged = 1;
        }
        return;
    }
    if (!replay_reserve((int)sim_tick + 1)) { s_recording = 0; return; }

    frame = s_buf + (size_t)sim_tick * (size_t)s_actor_count;
    for (slot = 0; slot < s_actor_count; slot++) {
        TD5_Actor *a = td5_game_get_actor(slot);
        if (!a) { memset(&frame[slot], 0, sizeof(frame[slot])); frame[slot].traffic_alpha = -1; continue; }
        replay_capture_actor(&frame[slot], a, slot);
    }
    if ((int)sim_tick + 1 > s_frame_count)
        s_frame_count = (int)sim_tick + 1;
}

/* Pose one actor from a snapshot: write the transform, zero the velocities (so
 * the renderer's sub-tick extrapolation from linear_velocity adds nothing), and
 * restore the cosmetic derived fields. */
static void replay_pose_actor(TD5_Actor *a, const TD5_ReplaySnap *src, int slot)
{
    a->world_pos          = src->world_pos;
    a->render_pos         = src->render_pos;
    a->rotation_matrix    = src->rotation_matrix;
    a->euler_accum        = src->euler_accum;
    a->display_angles     = src->display_angles;
    a->longitudinal_speed = src->longitudinal_speed;
    a->accumulated_tire_slip_x = src->slip_x;
    a->accumulated_tire_slip_z = src->slip_z;
    a->current_slip_metric     = src->slip_metric;
    a->current_gear       = src->current_gear;
    a->brake_flag         = src->brake_flag;
    a->handbrake_flag     = src->handbrake_flag;
    /* Restore the track-position block so the span-keyed trackside replay camera
     * cuts between shots as the recorded car advances, and race-order / lap /
     * minimap track the played-back run instead of freezing at the grid span. */
    a->track_span_raw         = src->track_span_raw;
    a->track_span_normalized  = src->track_span_normalized;
    a->track_span_accumulated = src->track_span_accumulated;
    a->track_span_high_water  = src->track_span_high_water;
    a->track_contact_vertex_A = src->track_contact_vertex_A;
    a->track_contact_vertex_B = src->track_contact_vertex_B;
    a->track_sub_lane_index   = src->track_sub_lane_index;
    memcpy(a->wheel_display_angles, src->wheel_display_angles,
           sizeof(a->wheel_display_angles));
    /* Zero all velocities so nothing integrates or extrapolates off the pose. */
    a->angular_velocity_roll  = 0;
    a->angular_velocity_yaw   = 0;
    a->angular_velocity_pitch = 0;
    a->linear_velocity_x = 0;
    a->linear_velocity_y = 0;
    a->linear_velocity_z = 0;
    /* Manually drive traffic visibility from the recorded alpha instead of the
     * (now-disabled) spawn/fade state machine. */
    if (slot >= g_traffic_slot_base && src->traffic_alpha >= 0)
        td5_ai_traffic_replay_force(slot, src->traffic_alpha);
}

void td5_replay_pose_tick(uint32_t sim_tick)
{
    int slot, count;
    const TD5_ReplaySnap *frame;
    int idx;
    if (s_frame_count <= 0 || !s_buf) return;

    /* Clamp: if the replay runs longer than the recording, freeze on the final
     * recorded pose. */
    idx = (int)sim_tick;
    if (idx >= s_frame_count) idx = s_frame_count - 1;
    frame = s_buf + (size_t)idx * (size_t)s_actor_count;

    count = td5_game_get_total_actor_count();
    if (count > s_actor_count) count = s_actor_count;
    for (slot = 0; slot < count; slot++) {
        TD5_Actor *a = td5_game_get_actor(slot);
        if (!a) continue;
        replay_pose_actor(a, &frame[slot], slot);
    }
}
