/**
 * td5_sound.c -- Sound playback, vehicle audio, ambient, CD
 *
 * Translates the original TD5_d3d.exe two-tier sound architecture into
 * clean C11 using td5_platform.h audio abstractions. The game manages
 * 44 base + 44 duplicate buffer slots (88 total) through the platform
 * layer. Per-vehicle engine sound banks use 3 channels each (Drive,
 * Rev/Reverb, Horn). A 16-entry logarithmic volume attenuation table
 * is handled internally by the platform audio backend.
 *
 * Key original functions reimplemented:
 *   0x440A30  UpdateVehicleLoopingAudioState
 *   0x440AB0  StartTrackedVehicleAudio
 *   0x440AE0  StopTrackedVehicleAudio
 *   0x440B00  UpdateVehicleAudioMix
 *   0x441A80  LoadVehicleSoundBank
 *   0x441C60  LoadRaceAmbientSoundBuffers
 *   0x441D50  ReleaseRaceSoundChannels
 *   0x441D90  PlayVehicleSoundAtPosition
 *   0x414640  LoadFrontendSoundEffects
 */

#include "td5_sound.h"
#include "td5_platform.h"
#include "td5_asset.h"
#include "td5re.h"
#include "td5_vfx.h"

/* Full actor struct needed for field-level access (engine speed, slip, position) */
#include "../../../re/include/td5_actor_struct.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Logging tag
 * ======================================================================== */

#define LOG_TAG "sound"

static uint32_t s_audio_mix_log_counter = 0;

/* ========================================================================
 * Internal constants
 * ======================================================================== */

/** Engine state machine values (per-vehicle, per-viewport). */
enum {
    ENGINE_STATE_STOPPED = 99,
    ENGINE_STATE_DRIVE   = 1,
    ENGINE_STATE_REV     = 2
};

/** Horn volume lookup by gear state. Original switch at 0x440F7C. */
static const int s_horn_vol_by_gear[6] = {
    10,   /* gear state 0 (default) */
    90,   /* gear state 1 */
    80,   /* gear state 2 */
    60,   /* gear state 3 */
    40,   /* gear state 4 */
    20    /* gear state 5 */
};

/** Ambient sound WAV filenames (19 entries, matches pointer table at 0x474A00). */
static const char *s_ambient_wav_names[TD5_SOUND_AMBIENT_COUNT] = {
    "Rain.wav",     /* slot 18 - loop */
    "SkidBit.wav",  /* slot 19 - loop */
    "Siren3.wav",   /* slot 20 - loop */
    "Siren5.wav",   /* slot 21 - loop */
    "ScrapeX.wav",  /* slot 22 */
    "Bottom3.wav",  /* slot 23 */
    "Bottom1.wav",  /* slot 24 */
    "Bottom4.wav",  /* slot 25 */
    "Bottom2.wav",  /* slot 26 */
    "HHit1.wav",    /* slot 27 */
    "HHit2.wav",    /* slot 28 */
    "HHit3.wav",    /* slot 29 */
    "HHit4.wav",    /* slot 30 */
    "LHit1.wav",    /* slot 31 */
    "LHit2.wav",    /* slot 32 */
    "LHit3.wav",    /* slot 33 */
    "LHit4.wav",    /* slot 34 */
    "LHit5.wav",    /* slot 35 */
    "Gear1.wav"     /* slot 36 */
};

/** Traffic engine variant WAV filenames (matches table at 0x474A4C). */
static const char *s_traffic_engine_wavs[3] = {
    "Engine0.wav",  /* variant 0: generic */
    "car.wav",      /* variant 1: car */
    "diesel.wav"    /* variant 2: diesel/truck */
};

/** Frontend SFX WAV paths (10 entries, slots 1-10). Original at 0x414640. */
static const char *s_frontend_sfx_paths[TD5_SOUND_FRONTEND_SFX_COUNT] = {
    "Front End\\Sounds\\ping3.wav",
    "Front End\\Sounds\\ping2.wav",
    "Front End\\Sounds\\Ping1.wav",
    "Front End\\Sounds\\Crash1.wav",
    "Front End\\Sounds\\Whoosh.wav",
    "Front End\\Sounds\\Whoosh.wav",
    "Front End\\Sounds\\Crash1.wav",
    "Front End\\Sounds\\Whoosh.wav",
    "Front End\\Sounds\\Crash1.wav",
    "Front End\\Sounds\\Uh-Oh.wav"
};

static const int s_frontend_sfx_slots[TD5_SOUND_FRONTEND_SFX_COUNT] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10
};
static int s_frontend_sfx_buffer_ids[TD5_SOUND_FRONTEND_SFX_COUNT] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* Slot-to-buffer and slot-to-channel mapping arrays.
 * The platform audio layer assigns sequential buffer IDs on load and returns
 * channel handles from play(). Game logic uses logical slot numbers. */
static int s_slot_to_buffer[TD5_SOUND_TOTAL_SLOTS];
static int s_slot_to_channel[TD5_SOUND_TOTAL_SLOTS];

static int slot_buf(int slot) {
    if (slot < 0 || slot >= TD5_SOUND_TOTAL_SLOTS) return -1;
    return s_slot_to_buffer[slot];
}
static void slot_play(int slot, int loop, int volume, int pan, int frequency) {
    int buf_id = slot_buf(slot);
    if (buf_id < 0) return;
    int ch = td5_plat_audio_play(buf_id, loop, volume, pan, frequency);
    if (slot >= 0 && slot < TD5_SOUND_TOTAL_SLOTS)
        s_slot_to_channel[slot] = ch;
}
static void slot_stop(int slot) {
    if (slot < 0 || slot >= TD5_SOUND_TOTAL_SLOTS) return;
    int ch = s_slot_to_channel[slot];
    if (ch >= 0) { td5_plat_audio_stop(ch); s_slot_to_channel[slot] = -1; }
}
static void slot_modify(int slot, int volume, int pan, int frequency) {
    if (slot < 0 || slot >= TD5_SOUND_TOTAL_SLOTS) return;
    int ch = s_slot_to_channel[slot];
    if (ch >= 0) td5_plat_audio_modify(ch, volume, pan, frequency);
}
static int slot_is_playing(int slot) {
    if (slot < 0 || slot >= TD5_SOUND_TOTAL_SLOTS) return 0;
    int ch = s_slot_to_channel[slot];
    if (ch < 0) return 0;
    return td5_plat_audio_is_playing(ch);
}

/* ========================================================================
 * Sound state -- mirrors original globals
 * ======================================================================== */

/**
 * Per-vehicle engine state for each viewport pass.
 * Index: [viewport * 6 + vehicle]. Values: ENGINE_STATE_STOPPED/DRIVE/REV.
 * Original: DAT_004c3770 (P1), DAT_004c3774 (P2 interleaved).
 */
static int s_engine_state[12];

/**
 * Per-traffic-vehicle engine state for each viewport pass.
 * Original: DAT_004c37a0.
 */
static int s_traffic_engine_state[12];

/**
 * Per-vehicle horn/siren tracked audio state.
 * Original: DAT_004c37dc.
 */
static int s_tracked_audio_state[12];

/**
 * Per-vehicle horn playing state.
 * 0=idle, 1=start pending, 2=playing.
 * Original: DAT_004c3848.
 */
static int s_horn_state[12];

/**
 * Per-vehicle reverb flag (non-zero = use Reverb.wav instead of Rev.wav).
 * Original: DAT_004c382c.
 */
static int s_reverb_flag[6];

/** Index of actor using Reverb.wav. Original: DAT_004c3878. */
static int s_reverb_actor_index;

/** Per-viewport listener world position (2 viewports x 3 components).
 *  Original: DAT_004c3810 (P1), DAT_004c381c (P2). */
static int32_t s_listener_pos[2][3];

/** Previous frame listener position for velocity delta.
 *  Original: DAT_004c38c0. */
static int32_t s_listener_prev_pos[2][3];

/** Listener velocity delta. Original: DAT_004c3888, DAT_004c388c, DAT_004c3890. */
static int32_t s_listener_vel[2][3];

/** Active listener position pointer (for current viewport pass). */
static int32_t *s_active_listener_pos;

/** Active listener velocity pointer. Original: DAT_004c387c. */
static int32_t *s_active_listener_vel;

/** Per-vehicle gear state (for horn volume table). Original: DAT_004c38a0. */
static int s_gear_state[6];

/** Tracked vehicle (siren) state. */
static int s_tracked_veh_active;       /* Original: DAT_004c37d0 (viewport 0) */
static int s_tracked_veh_active_p2;    /* Original: _DAT_004c37d4 */
static int s_tracked_veh_fade_target;  /* Original: DAT_004c37d8 */
static int s_tracked_veh_fade_level;   /* Original: DAT_004c38b8 */
static int s_tracked_veh_actor;        /* Original: DAT_004c380c */
static int s_siren_active_flag;        /* Original: DAT_004c3880 */
static int s_siren_refreshed;          /* Original: DAT_004c3844 */

/** Per-viewport skid playing state. Original: DAT_004c3768. */
static int s_skid_playing[2];

/** Per-viewport rain sound playing state.
 *  [CONFIRMED @ 0x00440B00]: DAT_004c3768 tracks whether rain has been started
 *  per viewport ("if DAT_004c3768 != 0 → already playing"). */
static int s_rain_playing[2];

/** Per-viewport skid intensity. Original: DAT_004c3de0. */
static int s_skid_intensity[2];

/** Viewport audio state change tracker. Original: DAT_004c38d8. */
static int s_viewport_audio_state;

/** Race end flag (suppresses new sounds during fade-out). Original: DAT_004c3de8. */
static int s_race_end_flag;

/* ========================================================================
 * External state accessed from other modules (via g_td5)
 * ======================================================================== */

/** Get actor pointer by slot index. Declared extern -- provided by td5_game. */
extern TD5_Actor *td5_game_get_actor(int slot);

/** Get the player slot index for a given viewport. */
extern int td5_game_get_player_slot(int viewport);

/** Check if replay mode is active. */
extern int td5_game_is_replay_active(void);

/** Get total actor count (racers + traffic). */
extern int td5_game_get_total_actor_count(void);

/** Get the traffic vehicle engine variant type (0/1/2). */
extern int td5_game_get_traffic_variant(int traffic_index);

/** Get the wanted/cop actor index (-1 if none). */
extern int td5_game_get_cop_actor_index(void);

/** Check if wanted mode is enabled. */
extern int td5_game_is_wanted_mode(void);

/** Advance sky rotation visual effect for wanted mode. */
extern void td5_game_advance_sky_rotation(void);

/* ========================================================================
 * Forward declarations (internal helpers)
 * ======================================================================== */

static float sound_compute_distance(const int32_t *listener_pos,
                                    const int32_t *vehicle_pos);

static float sound_compute_doppler_ratio(const int32_t *listener_vel,
                                         const int32_t *vehicle_vel,
                                         float dx, float dz, float dist);

static int sound_attenuate_volume(int raw_vol, float distance);

static int sound_apply_doppler_pitch(int base_pitch, float doppler_ratio);

static int sound_load_wav_from_zip(const char *wav_name, const char *zip_path,
                                   int slot, int loop, int duplicates);

/* ========================================================================
 * Init / Shutdown / Tick
 * ======================================================================== */

int td5_sound_init(void)
{
    memset(s_frontend_sfx_buffer_ids, 0xFF, sizeof(s_frontend_sfx_buffer_ids));
    memset(s_slot_to_buffer, 0xFF, sizeof(s_slot_to_buffer));
    memset(s_slot_to_channel, 0xFF, sizeof(s_slot_to_channel));
    if (!td5_plat_audio_init()) {
        TD5_LOG_W(LOG_TAG, "audio init failed -- running silent");
        return 1; /* graceful degradation, same as original */
    }
    TD5_LOG_I(LOG_TAG, "audio subsystem initialized");
    return 1;
}

void td5_sound_shutdown(void)
{
    td5_plat_audio_shutdown();
}

void td5_sound_tick(void)
{
    td5_plat_audio_stream_refresh();

    /* Per-frame audio update is driven by td5_sound_update_audio_mix(),
     * called explicitly by the race frame loop after rendering. */
}

/* ========================================================================
 * Race Sound Lifecycle
 * ======================================================================== */

/**
 * td5_sound_init_race_resources -- prepare for race audio.
 * Called at the start of InitializeRaceSession.
 */
int td5_sound_init_race_resources(void)
{
    memset(s_engine_state, 0, sizeof(s_engine_state));
    memset(s_traffic_engine_state, 0, sizeof(s_traffic_engine_state));
    memset(s_tracked_audio_state, 0, sizeof(s_tracked_audio_state));
    memset(s_horn_state, 0, sizeof(s_horn_state));
    memset(s_reverb_flag, 0, sizeof(s_reverb_flag));
    memset(s_listener_pos, 0, sizeof(s_listener_pos));
    memset(s_listener_prev_pos, 0, sizeof(s_listener_prev_pos));
    memset(s_listener_vel, 0, sizeof(s_listener_vel));
    memset(s_gear_state, 0, sizeof(s_gear_state));

    s_reverb_actor_index    = 0;
    s_tracked_veh_active    = 0;
    s_tracked_veh_active_p2 = 0;
    s_tracked_veh_fade_target = 0;
    s_tracked_veh_fade_level  = 0;
    s_tracked_veh_actor     = 0;
    s_siren_active_flag     = 0;
    s_siren_refreshed       = 0;
    s_skid_playing[0]       = 0;
    s_skid_playing[1]       = 0;
    s_rain_playing[0]       = 0;
    s_rain_playing[1]       = 0;
    s_skid_intensity[0]     = 0;
    s_skid_intensity[1]     = 0;
    s_viewport_audio_state  = 0;
    s_race_end_flag         = 0;
    s_active_listener_pos   = NULL;
    s_active_listener_vel   = NULL;

    memset(s_slot_to_buffer, 0xFF, sizeof(s_slot_to_buffer));
    memset(s_slot_to_channel, 0xFF, sizeof(s_slot_to_channel));

    TD5_LOG_I(LOG_TAG, "Race audio resources reset");

    return 1;
}

/**
 * ReleaseRaceSoundChannels (0x441D50).
 *
 * Race shutdown: stop all duplicate-range channels (44-87),
 * then remove all base-range buffers (0-43).
 */
void td5_sound_release_race_channels(void)
{
    /* Stop duplicate slots 44-87 */
    for (int i = TD5_SOUND_DUP_OFFSET; i < TD5_SOUND_TOTAL_SLOTS; i++) {
        slot_stop(i);
    }
    /* Stop and free base slots 0-43 */
    for (int i = 0; i < TD5_SOUND_MAX_BASE_SLOTS; i++) {
        slot_stop(i);
        int buf_id = s_slot_to_buffer[i];
        if (buf_id >= 0) {
            td5_plat_audio_free(buf_id);
            s_slot_to_buffer[i] = -1;
            if (i + TD5_SOUND_DUP_OFFSET < TD5_SOUND_TOTAL_SLOTS)
                s_slot_to_buffer[i + TD5_SOUND_DUP_OFFSET] = -1;
        }
    }
}

/* ========================================================================
 * Vehicle Sound Bank Loading
 * ======================================================================== */

/**
 * LoadVehicleSoundBank (0x441A80).
 *
 * Loads the three per-vehicle WAVs (Drive.wav, Rev.wav/Reverb.wav, Horn.wav)
 * from the car's sound ZIP into consecutive slots [vehicle*3 .. vehicle*3+2].
 *
 * When vehicle_index == 0, all audio state arrays are zeroed (first vehicle
 * initialization clears the slate).
 *
 * @param car_dir       Path to the car's ZIP archive containing WAVs.
 * @param vehicle_index Vehicle slot 0-5.
 * @param is_reverb_vehicle Non-zero to load Reverb.wav instead of Rev.wav
 *                          (set for the local player's own vehicle).
 * @return 1 on success.
 */
int td5_sound_load_vehicle_bank(const char *car_dir, int vehicle_index,
                                int is_reverb_vehicle)
{
    if (vehicle_index < 0 || vehicle_index >= TD5_SOUND_MAX_RACE_VEHICLES) {
        TD5_LOG_E(LOG_TAG, "vehicle_index %d out of range", vehicle_index);
        return 0;
    }

    /* On first vehicle load, clear all audio state arrays */
    if (vehicle_index == 0) {
        memset(s_engine_state, 0, sizeof(s_engine_state));
        memset(s_horn_state, 0, sizeof(s_horn_state));
        memset(s_traffic_engine_state, 0, sizeof(s_traffic_engine_state));
        memset(s_reverb_flag, 0, sizeof(s_reverb_flag));

        s_tracked_veh_fade_target = 0;
        s_tracked_veh_fade_level  = 0;
        s_tracked_veh_active_p2   = 0;
        s_tracked_veh_active      = 0;
    }

    int base_slot = vehicle_index * TD5_SOUND_CHANNELS_PER_VEHICLE;

    /* Initialize per-vehicle state to "stopped" */
    s_engine_state[vehicle_index * 2]     = ENGINE_STATE_STOPPED; /* P1 */
    s_engine_state[vehicle_index * 2 + 1] = ENGINE_STATE_STOPPED; /* P2 */
    s_tracked_audio_state[vehicle_index * 2] = ENGINE_STATE_STOPPED;

    /* Set reverb flag for this vehicle */
    s_reverb_flag[vehicle_index] = is_reverb_vehicle;
    if (is_reverb_vehicle) {
        s_reverb_actor_index = vehicle_index;
    }

    /* Load Drive.wav into slot base+0 (looping, 2 duplicates) */
    sound_load_wav_from_zip("Drive.wav", car_dir, base_slot, 1, 2);

    /* Load Rev.wav or Reverb.wav into slot base+1 (looping, 2 duplicates) */
    const char *rev_name = is_reverb_vehicle ? "Reverb.wav" : "Rev.wav";
    sound_load_wav_from_zip(rev_name, car_dir, base_slot + 1, 1, 2);

    /* Load Horn.wav into slot base+2 (one-shot, 2 duplicates) */
    sound_load_wav_from_zip("Horn.wav", car_dir, base_slot + 2, 0, 2);

    /* Reset skid, rain, and siren flags */
    s_skid_playing[0]   = 0;
    s_skid_playing[1]   = 0;
    s_rain_playing[0]   = 0;
    s_rain_playing[1]   = 0;
    s_siren_refreshed   = 0;
    s_siren_active_flag = 0;

    return 1;
}

/* ========================================================================
 * Ambient Sound Loading
 * ======================================================================== */

/**
 * LoadRaceAmbientSoundBuffers (0x441C60).
 *
 * Loads 19 ambient WAVs from SOUND\SOUND.ZIP into slots 18-36.
 * First 4 are looping (Rain, SkidBit, Siren3, Siren5); rest are one-shot.
 * Also loads traffic engine loops for actors beyond the 6 race slots.
 */
int td5_sound_load_ambient(void)
{
    const char *zip_path = "SOUND\\SOUND.ZIP";

    /* Load 19 ambient WAVs into slots 18-36 */
    for (int i = 0; i < TD5_SOUND_AMBIENT_COUNT; i++) {
        int slot = TD5_SOUND_AMBIENT_SLOT_BASE + i;
        int loop = (i < 4) ? 1 : 0; /* first 4 entries are looping */
        TD5_LOG_I(LOG_TAG, "Ambient WAV load begin: name=%s slot=%d loop=%d dup=%d",
                  s_ambient_wav_names[i], slot, loop, 2);
        sound_load_wav_from_zip(s_ambient_wav_names[i], zip_path, slot, loop, 2);
        TD5_LOG_I(LOG_TAG, "Ambient WAV load end: name=%s slot=%d",
                  s_ambient_wav_names[i], slot);
    }

    /* Load traffic engine loops for actors 6+ */
    int total_actors = td5_game_get_total_actor_count();
    if (total_actors > 6) {
        for (int i = 0; i < total_actors - 6; i++) {
            int variant = td5_game_get_traffic_variant(i);
            if (variant < 0 || variant > 2) variant = 0;
            int slot = TD5_SOUND_TRAFFIC_SLOT_BASE + i;
            TD5_LOG_I(LOG_TAG,
                      "Traffic WAV load begin: name=%s traffic_index=%d slot=%d loop=%d dup=%d",
                      s_traffic_engine_wavs[variant], i, slot, 1, 2);
            sound_load_wav_from_zip(s_traffic_engine_wavs[variant], zip_path,
                                    slot, 1, 2);
            TD5_LOG_I(LOG_TAG, "Traffic WAV load end: name=%s traffic_index=%d slot=%d",
                      s_traffic_engine_wavs[variant], i, slot);
        }
    }

    return 1;
}

void td5_sound_update_ambient(void)
{
    /* ----------------------------------------------------------------
     * UpdateVehicleAudioMix ambient weather section [CONFIRMED @ 0x00440B00]
     *
     * Per-viewport rain sound management.  In the original this runs
     * inside UpdateVehicleAudioMix for the "viewer vehicle" iteration.
     * The port calls it as a separate step from RunRaceFrame so it
     * executes once per frame after the main audio mix.
     *
     * Sound slots (from LoadRaceAmbientSoundBuffers [CONFIRMED @ 0x00441C60]):
     *   slot 18 (0x12): Rain.wav  — looped rain loop start channel
     *   slot 19 (0x13): second ambient layer — rain volume channel
     *
     * [CONFIRMED @ 0x00440B00]:
     *   g_weatherActiveCountView0/1 → td5_vfx_get_weather_active_count(vp)
     *   g_weatherType               → td5_vfx_get_weather_type()
     *   DAT_004c3768 (per-viewport) → s_rain_playing[vp]
     * ---------------------------------------------------------------- */

    int num_passes  = (g_td5.split_screen_mode != 0) ? 2 : 1;
    int slot_offset = 0;

    for (int vp = 0; vp < num_passes; vp++) {
        int pan = (g_td5.split_screen_mode == 0) ? 0 : (vp == 0 ? -10000 : 10000);
        int weather_count = td5_vfx_get_weather_active_count(vp);
        int weather_type  = td5_vfx_get_weather_type();

        /* [CONFIRMED @ 0x00440B00]: Start rain if count>=1 AND not yet
         * playing AND weather==RAIN (0).  Play at volume=0 (silent start). */
        if (weather_count >= 1 && !s_rain_playing[vp] && weather_type == 0) {
            slot_play(slot_offset + 0x12, 1, 0, pan, TD5_SOUND_FREQ_22050);
            s_rain_playing[vp] = 1;
            TD5_LOG_I(LOG_TAG, "ambient: rain start vp=%d count=%d",
                      vp, weather_count);
        } else if (weather_count == 0 && s_rain_playing[vp]) {
            /* [CONFIRMED @ 0x00440B00]: Stop rain when count drops to zero. */
            slot_stop(slot_offset + 0x12);
            s_rain_playing[vp] = 0;
            TD5_LOG_I(LOG_TAG, "ambient: rain stop vp=%d", vp);
        }

        /* [CONFIRMED @ 0x00440B00]: Modulate rain volume by particle count.
         * Volume = clamp(weather_count, 0, 0x7f).
         * Slot 0x12 (Rain.wav) — NOT 0x13 (SkidBit.wav). Using 0x13 was the
         * conflict: rain modify on 0x13 fought skid modify on 0x13 every frame. */
        if (weather_count > 0 && weather_type == 0) {
            int rain_vol = weather_count;
            if (rain_vol > 0x7F) rain_vol = 0x7F;
            slot_modify(slot_offset + 0x12, rain_vol, pan, TD5_SOUND_FREQ_22050);
            if ((s_audio_mix_log_counter % 60u) == 0u) {
                TD5_LOG_D(LOG_TAG, "ambient: rain vp=%d vol=%d count=%d",
                          vp, rain_vol, weather_count);
            }
        }

        slot_offset = TD5_SOUND_DUP_OFFSET;
    }
}

/* ========================================================================
 * Tracked Vehicle (Siren) Audio
 * ======================================================================== */

/**
 * UpdateVehicleLoopingAudioState (0x440A30).
 *
 * Called per-actor when the horn button is pressed. If wanted mode is active
 * and this actor is the cop, activates the siren audio layer. Otherwise,
 * triggers the horn sound for this vehicle.
 */
void td5_sound_update_vehicle_looping_state(int actor_index)
{
    int base_slot = actor_index * 3;

    /* Check if wanted mode is active and this is the cop */
    if (td5_game_is_wanted_mode() && actor_index == td5_game_get_cop_actor_index()) {
        if (s_siren_active_flag == 0) {
            /* Activate tracked vehicle audio (siren) */
            s_tracked_veh_active_p2 = 1;
            s_tracked_veh_active    = 1;
            s_tracked_veh_actor     = actor_index;
            s_tracked_veh_fade_target = TD5_SOUND_SIREN_FADE_FULL;
        }
        td5_game_advance_sky_rotation();
        s_siren_refreshed   = 1;
        s_siren_active_flag = 1;
        return;
    }

    /* Keep the looping engine state latched to the existing channel rather
     * than requesting a fresh play every frame. */
    if (!slot_is_playing(base_slot) &&
        !slot_is_playing(base_slot + 1)) {
        s_engine_state[actor_index * 2] = ENGINE_STATE_STOPPED;
        s_engine_state[actor_index * 2 + 1] = ENGINE_STATE_STOPPED;
    }
}

/**
 * StartTrackedVehicleAudio (0x440AB0).
 *
 * Explicitly start the siren fade-in for a given actor.
 */
void td5_sound_start_tracked_vehicle_audio(int actor_index)
{
    s_tracked_veh_active_p2   = 1;
    s_tracked_veh_active      = 1;
    s_tracked_veh_actor       = actor_index;
    s_tracked_veh_fade_target = TD5_SOUND_SIREN_FADE_FULL;
}

/**
 * StopTrackedVehicleAudio (0x440AE0).
 *
 * Begin siren fade-out. The mixer will stop playback on the next frame
 * when the fade target is zero and the active flag is still set.
 */
void td5_sound_stop_tracked_vehicle_audio(void)
{
    if (s_tracked_veh_active != 0) {
        s_tracked_veh_fade_target = 0;
    }
}

/* ========================================================================
 * Master Audio Mixer -- UpdateVehicleAudioMix (0x440B00)
 *
 * This is the core per-frame sound function (~800 lines in the original).
 * Called once per frame after rendering. Processes:
 *   A. Listener velocity computation
 *   B. Split-screen viewport transitions
 *   C. Tracked vehicle audio (siren/pursuit)
 *   D. Per-vehicle engine sound update (main loop)
 *   E. Skid/tire screech
 *   F. Horn playback
 *   G. Traffic engine loops
 * ======================================================================== */

void td5_sound_update_audio_mix(void)
{
    /* ----------------------------------------------------------------
     * A. Compute listener velocity deltas (for Doppler)
     * ---------------------------------------------------------------- */
    for (int vp = 0; vp < 2; vp++) {
        for (int c = 0; c < 3; c++) {
            s_listener_vel[vp][c] = s_listener_pos[vp][c] - s_listener_prev_pos[vp][c];
            s_listener_prev_pos[vp][c] = s_listener_pos[vp][c];
        }
    }

    s_audio_mix_log_counter++;
    if ((s_audio_mix_log_counter % 60u) == 0u) {
        int active_engine_channels = 0;
        for (int i = 0; i < 12; i++) {
            if (s_engine_state[i] != 0 && s_engine_state[i] != ENGINE_STATE_STOPPED) {
                active_engine_channels++;
            }
        }
        TD5_LOG_D(LOG_TAG,
                  "Audio mix: engine_channels=%d listener0=(%d,%d,%d)",
                  active_engine_channels,
                  (int)s_listener_pos[0][0],
                  (int)s_listener_pos[0][1],
                  (int)s_listener_pos[0][2]);
    }

    /* ----------------------------------------------------------------
     * B. Split-screen viewport transition detection
     *
     * When switching between 1P and 2P modes, reset engine states for
     * all vehicles to force re-initialization of audio playback.
     * ---------------------------------------------------------------- */
    int desired_viewport_state = (g_td5.split_screen_mode != 0) ? 1 : 0;
    if (s_viewport_audio_state != desired_viewport_state) {
        if (s_viewport_audio_state == 0) {
            /* Entering split-screen: mark all engines as needing restart */
            for (int v = 0; v < 6; v++) {
                if (s_engine_state[v * 2] != 0) {
                    s_engine_state[v * 2]     = ENGINE_STATE_STOPPED;
                    s_engine_state[v * 2 + 1] = ENGINE_STATE_STOPPED;
                }
            }
            s_viewport_audio_state = 1;
        } else {
            /* Leaving split-screen: stop all duplicate-range slots */
            for (int i = TD5_SOUND_DUP_OFFSET; i < TD5_SOUND_TOTAL_SLOTS; i++) {
                slot_stop(i);
            }
            s_skid_playing[1] = 0;
            s_viewport_audio_state = 0;
        }
    }

    /* ----------------------------------------------------------------
     * C. Tracked vehicle (siren) fade processing
     * ---------------------------------------------------------------- */
    if (s_tracked_veh_active == 0 || s_tracked_veh_fade_level != 0 ||
        s_tracked_veh_fade_target != 0) {
        /* Snap fade level to target (instant binary fade) */
        if (s_tracked_veh_fade_level < s_tracked_veh_fade_target) {
            s_tracked_veh_fade_level = TD5_SOUND_SIREN_FADE_FULL;
        } else if (s_tracked_veh_fade_target < s_tracked_veh_fade_level) {
            s_tracked_veh_fade_level = 0;
        }
    } else {
        /* Active but both fade level and target are zero: stop siren */
        slot_stop(0x15);
        slot_stop(0x16);
        slot_stop(0x41);
        slot_stop(0x42);
        s_tracked_veh_active_p2 = 0;
        s_tracked_veh_active    = 0;
    }

    /* ----------------------------------------------------------------
     * D. Determine viewport pass count and initial state
     * ---------------------------------------------------------------- */
    int num_passes;
    int viewer_vehicle;  /* which vehicle index is the "viewer" for this pass */
    int pan;

    if (g_td5.split_screen_mode == 0) {
        num_passes     = 1;
        viewer_vehicle = s_reverb_actor_index;
        pan            = 0;
    } else {
        num_passes     = 2;
        viewer_vehicle = 0; /* first pass: vehicle 0, will increment */
        pan            = -10000;
    }

    /* ----------------------------------------------------------------
     * Main viewport loop
     * ---------------------------------------------------------------- */
    int slot_offset = 0; /* 0 for P1 base slots, 44 for P2 duplicate slots */

    for (int pass = 0; pass < num_passes; pass++) {
        /* Set active listener pointers for this viewport */
        if (td5_game_is_replay_active()) {
            s_active_listener_vel = s_listener_vel[pass];
            s_active_listener_pos = s_listener_pos[pass];
        } else {
            /* During normal gameplay, use actor velocity for viewer */
            s_active_listener_vel = s_listener_vel[pass];
            s_active_listener_pos = s_listener_pos[pass];
        }

        /* ============================================================
         * Per-vehicle engine sound loop (6 race vehicles)
         * ============================================================ */
        for (int veh = 0; veh < TD5_SOUND_MAX_RACE_VEHICLES; veh++) {
            TD5_Actor *actor = td5_game_get_actor(veh);
            if (!actor) continue;

            int state_idx = pass + veh * 2;

            /* ----------------------------------------------------------
             * Viewer vehicle horn/siren tracked audio (per-vehicle)
             * ---------------------------------------------------------- */
            if ((int)veh == td5_game_get_player_slot(pass)) {
                /* Check if this is a "dead" vehicle (all -1 bytes at identity check) */
                /* Simplified: check if vehicle is active */

                /* Get lateral slip for skid/horn volume */
                int slip_front = actor->front_axle_slip_excess;
                int slip_rear  = actor->rear_axle_slip_excess;
                int slip_max   = (slip_front > slip_rear) ? slip_front : slip_rear;

                /* Check stunned state */
                if (actor->damage_lockout == 0x0F) {
                    slip_max = 0;
                }

                int horn_vol_raw = slip_max / 3;

                /* Clamp tracked audio volume to [0, 0x1000] */
                int tracked_vol;
                if (horn_vol_raw < 0)      tracked_vol = 0;
                else if (horn_vol_raw > 0x1000) tracked_vol = 0x1000;
                else                       tracked_vol = horn_vol_raw;

                /* Clamp pitch base for tracked audio */
                int tracked_pitch;
                if (horn_vol_raw < 0x800)      tracked_pitch = 0x800;
                else if (horn_vol_raw > 0x1000) tracked_pitch = 0x1000;
                else                           tracked_pitch = horn_vol_raw;

                /* Spatial audio for tracked sound (non-viewer only in original,
                 * but for the viewer vehicle this path computes direct) */
                float dx = (float)s_active_listener_pos[0] - (float)actor->world_pos.x;
                float dz = (float)s_active_listener_pos[2] - (float)actor->world_pos.z;
                dx *= TD5_SOUND_DISTANCE_SCALE;
                dz *= TD5_SOUND_DISTANCE_SCALE;
                float dist = sqrtf(dx * dx + dz * dz);

                int vol_scaled;
                if (tracked_vol < 0) {
                    vol_scaled = 0;
                } else {
                    int v = tracked_vol;
                    if (v > 0xFFF) v = 0xFFF;
                    vol_scaled = v >> 5;
                }

                int vol_atten = (vol_scaled * (0x7F - ((int)roundf(dist) >> 7))) / 0x7F;

                if (vol_atten < 0)       vol_atten = 0;
                else if (vol_atten >= 0x80) vol_atten = 0x7F;

                /* Doppler pitch shift */
                int final_pitch = tracked_pitch;
                if (vol_atten > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        s_active_listener_vel,
                        &actor->linear_velocity_x,
                        dx, dz, dist);
                    final_pitch = sound_apply_doppler_pitch(tracked_pitch, doppler);
                }

                /* Play or modify tracked audio */
                if (s_tracked_audio_state[state_idx] == ENGINE_STATE_STOPPED) {
                    slot_play(slot_offset + 0x13, 1, vol_atten, pan, final_pitch);
                    s_tracked_audio_state[state_idx] = 1;
                } else {
                    slot_modify(slot_offset + 0x14, vol_atten, pan, final_pitch);
                }
            }

            /* ----------------------------------------------------------
             * Engine sound computation
             * ---------------------------------------------------------- */
            int raw_speed = actor->engine_speed_accum;
            int engine_target_state;
            int engine_vol;
            int engine_pitch;

            if (raw_speed < 1000 && s_reverb_flag[veh]) {
                /* Diesel/reverb mode: fixed low-frequency idle */
                engine_target_state = ENGINE_STATE_REV;
                engine_vol   = 0x50; /* ~63% of max */
                engine_pitch = TD5_SOUND_FREQ_22050;
            } else {
                engine_target_state = ENGINE_STATE_DRIVE;

                /* Speed scaling: raw/4 + base offset */
                int speed_scaled = (raw_speed + (raw_speed >> 31 & 3)) / 4;
                speed_scaled += ((veh == (int)viewer_vehicle) + 2) * 0x400;

                /* Engaged gear adds +0x400 base for non-reverb vehicles */
                if ((int)veh != s_reverb_actor_index &&
                    s_gear_state[veh] > 0) {
                    speed_scaled += 0x400;
                }

                /* Clamp to [0, 0xFFF] then scale to [0, 127] */
                if (speed_scaled < 0)        engine_vol = 0;
                else {
                    if (speed_scaled > 0xFFF) speed_scaled = 0xFFF;
                    engine_vol = speed_scaled >> 5;
                }

                /* Pitch formula: ((rand%100 + raw_speed) * 103) / 35 + 10000 */
                engine_pitch = ((rand() % 100 + raw_speed) * 103) / 35 + 10000;
            }

            /* ----------------------------------------------------------
             * Engine state machine: start/switch/modify
             * ---------------------------------------------------------- */
            int cur_state = s_engine_state[state_idx];
            if (cur_state != engine_target_state) {
                int next_slot = engine_target_state + veh * 3 - 1 + slot_offset;

                /* State change: stop old, start new */
                if (cur_state == ENGINE_STATE_STOPPED) {
                    /* First time: if this is the viewer, also start the idle loop */
                    if ((int)veh == (int)viewer_vehicle &&
                        !td5_game_is_replay_active() &&
                        !slot_is_playing(veh * 3 + 1 + slot_offset)) {
                        slot_play(veh * 3 + 1 + slot_offset, 1, 0, 0, 1000);
                    }
                } else {
                    /* Stop the previously active engine channel */
                    slot_stop(cur_state + veh * 3 + slot_offset);
                }
                /* Start the new engine channel */
                if (!slot_is_playing(next_slot)) {
                    slot_play(next_slot, 1, 0, 0, 1000);
                }
                s_engine_state[state_idx] = engine_target_state;
            }

            /* ----------------------------------------------------------
             * Viewer vehicle: direct volume/pitch (no distance atten)
             * ---------------------------------------------------------- */
            if ((int)veh == (int)viewer_vehicle && !td5_game_is_replay_active()) {
                int steer_pan = pan;
                /* Steering-based pan for single-player (subtle L/R shift) */
                if (raw_speed > 999 && g_td5.split_screen_mode == 0) {
                    /* pan = steering_command * -0x51EB851F >> 38 */
                    int64_t pan_calc = (int64_t)actor->steering_command * (int64_t)(-0x51EB851F);
                    steer_pan = (int)(pan_calc >> 38);
                }

                int modify_slot = slot_offset + veh * 3;
                slot_modify(modify_slot + 1, engine_vol, steer_pan, engine_pitch);

                /* Horn volume from gear state switch table */
                int gear_idx = s_gear_state[veh];
                if (gear_idx < 0 || gear_idx > 5) gear_idx = 0;
                engine_vol = s_horn_vol_by_gear[gear_idx];
                modify_slot = modify_slot + 2;

                /* Modify the horn/main engine channel */
                slot_modify(modify_slot, engine_vol, steer_pan, engine_pitch);

                /* ---- Skid sound management ---- */
                int skid_val = s_skid_intensity[pass];
                if (skid_val > 0 && s_skid_playing[pass] == 0 && s_race_end_flag == 0) {
                    /* Original @ 0x440B00: Play(0x12, vol=0) on skid start — silences
                     * Rain.wav at the moment screech begins. Skid screech itself is
                     * SkidBit.wav at slot 0x13 (Modify below). */
                    slot_play(slot_offset + 0x12, 1, 0, pan, TD5_SOUND_FREQ_22050);
                    /* Start SkidBit.wav loop — slot 0x13 [CONFIRMED @ 0x4413D1] */
                    if (!slot_is_playing(slot_offset + 0x13)) {
                        slot_play(slot_offset + 0x13, 1, 0, pan, TD5_SOUND_FREQ_22050);
                        TD5_LOG_I(LOG_TAG, "Skid start: pass=%d slot=%d skid_val=%d",
                                  pass, slot_offset + 0x13, skid_val);
                    }
                    s_skid_playing[pass] = 1;
                }
                if (skid_val != 0 && s_race_end_flag == 0) {
                    int skid_vol = skid_val;
                    if (skid_vol > 0x7F) skid_vol = 0x7F;
                    slot_modify(slot_offset + 0x13, skid_vol, pan,
                                          TD5_SOUND_FREQ_22050);
                }
                if (skid_val == 0 && s_skid_playing[pass] != 0) {
                    slot_stop(slot_offset + 0x13);
                    s_skid_playing[pass] = 0;
                }
            } else {
                /* ----------------------------------------------------------
                 * Non-viewer vehicle: spatial audio with distance + Doppler
                 * ---------------------------------------------------------- */
                int spatial_pan = pan;
                if (g_td5.split_screen_mode == 0) {
                    spatial_pan = 0;
                }

                float dx = ((float)s_active_listener_pos[0] - (float)actor->world_pos.x)
                           * TD5_SOUND_DISTANCE_SCALE;
                float dz = ((float)s_active_listener_pos[2] - (float)actor->world_pos.z)
                           * TD5_SOUND_DISTANCE_SCALE;
                float dist_sq = dx * dx + dz * dz;
                float dist;
                if (td5_game_is_replay_active()) {
                    dist = sqrtf(dist_sq) * TD5_SOUND_REPLAY_DIST_SCALE;
                } else {
                    dist = sqrtf(dist_sq);
                }

                int vol_atten = sound_attenuate_volume(engine_vol, dist);

                int final_pitch = engine_pitch;
                if (vol_atten > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        s_active_listener_vel,
                        &actor->linear_velocity_x,
                        dx, dz, dist);
                    final_pitch = sound_apply_doppler_pitch(engine_pitch, doppler);
                }

                int modify_slot = engine_target_state + veh * 3 + slot_offset;
                slot_modify(modify_slot, vol_atten, spatial_pan, final_pitch);
            }

            /* ----------------------------------------------------------
             * Horn playback for this vehicle
             * ---------------------------------------------------------- */
            if (s_horn_state[state_idx] != 0) {
                int horn_slot = veh * 3 + slot_offset + 2;
                int horn_playing = slot_is_playing(horn_slot + 1);

                if (horn_playing == 0 && s_horn_state[state_idx] != 1) {
                    /* Horn finished playing, reset state */
                    s_horn_state[state_idx] = 0;
                } else {
                    /* Compute spatial horn volume */
                    float dx = ((float)s_active_listener_pos[0] - (float)actor->world_pos.x)
                               * TD5_SOUND_DISTANCE_SCALE;
                    float dz = ((float)s_active_listener_pos[2] - (float)actor->world_pos.z)
                               * TD5_SOUND_DISTANCE_SCALE;
                    float dist = sqrtf(dx * dx + dz * dz);
                    int horn_vol = ((0x7F - ((int)roundf(dist) >> 7)) * 0x1000) / 0x7F;
                    if (horn_vol < 0) horn_vol = 0;
                    if (horn_vol >= 0x80) horn_vol = 0x7F;

                if (s_horn_state[state_idx] == 1) {
                        /* Start horn */
                        if (!slot_is_playing(veh * 3 + 2 + slot_offset)) {
                            slot_play(veh * 3 + 2 + slot_offset, 0,
                                                horn_vol, pan, TD5_SOUND_FREQ_22050);
                        }
                    } else {
                        slot_modify(horn_slot + 1, horn_vol,
                                              pan, TD5_SOUND_FREQ_22050);
                    }
                    s_horn_state[state_idx] = 2;
                }
            }

        } /* end per-vehicle loop */

        /* ============================================================
         * Siren (tracked vehicle) audio for this viewport
         * ============================================================ */
        if (s_tracked_veh_active != 0 || (pass == 1 && s_tracked_veh_active_p2 != 0)) {
            TD5_Actor *cop = td5_game_get_actor(s_tracked_veh_actor);
            if (cop) {
                float dx = ((float)s_active_listener_pos[0] - (float)cop->world_pos.x)
                           * TD5_SOUND_DISTANCE_SCALE * TD5_SOUND_REPLAY_DIST_SCALE;
                float dz = ((float)s_active_listener_pos[2] - (float)cop->world_pos.z)
                           * TD5_SOUND_DISTANCE_SCALE * TD5_SOUND_REPLAY_DIST_SCALE;
                float dist = sqrtf(dx * dx + dz * dz);

                int siren_vol = sound_attenuate_volume(s_tracked_veh_fade_level, dist);
                int siren_pitch = TD5_SOUND_SIREN_FREQ;

                if (siren_vol > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        s_active_listener_vel,
                        &cop->linear_velocity_x,
                        dx, dz, dist);
                    siren_pitch = sound_apply_doppler_pitch(TD5_SOUND_SIREN_FREQ, doppler);
                }

                int siren_active_for_pass = (pass == 0) ? s_tracked_veh_active
                                                        : s_tracked_veh_active_p2;
                if (siren_active_for_pass == 1) {
                    /* First frame: start both siren channels */
                    if (!slot_is_playing(slot_offset + 0x14)) {
                        slot_play(slot_offset + 0x14, 1, siren_vol, pan, siren_pitch);
                    }
                    if (!slot_is_playing(slot_offset + 0x15)) {
                        slot_play(slot_offset + 0x15, 1, siren_vol, pan, siren_pitch);
                    }
                } else {
                    slot_modify(slot_offset + 0x15, siren_vol, pan, siren_pitch);
                    slot_modify(slot_offset + 0x16, siren_vol, pan, siren_pitch);
                }

                if (pass == 0) s_tracked_veh_active = 2;
                else           s_tracked_veh_active_p2 = 2;
            }
        }

        /* ============================================================
         * Traffic vehicle engine loops (actors 6+)
         * ============================================================ */
        int total_actors = td5_game_get_total_actor_count();
        if (total_actors > 6) {
            int traffic_slot = slot_offset + 0x26; /* DXSound slot for first traffic */
            for (int t = 6; t < total_actors; t++) {
                TD5_Actor *traffic = td5_game_get_actor(t);
                if (!traffic) { traffic_slot++; continue; }

                int t_state_idx = pass + (t - 6) * 2;
                int t_speed = traffic->engine_speed_accum;

                /* Traffic speed scaling: raw/4 + 0x800 base */
                int t_scaled = (t_speed + (t_speed >> 31 & 3)) / 4 + 0x800;
                int t_vol;
                if (t_scaled < 0)       t_vol = 0;
                else {
                    if (t_scaled > 0xFFF) t_scaled = 0xFFF;
                    t_vol = t_scaled >> 5;
                }

                /* Same pitch formula as race vehicles */
                int t_pitch = ((rand() % 100 + t_speed) * 103) / 35 + 10000;

                /* Start the engine loop if not yet started */
                if (s_traffic_engine_state[t_state_idx] == 0) {
                    slot_play(t - 6 + 0x1F + slot_offset, 1, 0, 0, 1000);
                    s_traffic_engine_state[t_state_idx] = 1;
                }

                /* Spatial audio: distance attenuation + Doppler */
                float dx = ((float)s_active_listener_pos[0] - (float)traffic->world_pos.x)
                           * TD5_SOUND_DISTANCE_SCALE;
                float dz = ((float)s_active_listener_pos[2] - (float)traffic->world_pos.z)
                           * TD5_SOUND_DISTANCE_SCALE;
                float dist = sqrtf(dx * dx + dz * dz);

                int t_vol_atten = sound_attenuate_volume(t_vol, dist);
                int t_final_pitch = t_pitch;
                if (t_vol_atten > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        s_active_listener_vel,
                        &traffic->linear_velocity_x,
                        dx, dz, dist);
                    t_final_pitch = sound_apply_doppler_pitch(t_pitch, doppler);
                }

                slot_modify(traffic_slot, t_vol_atten, pan, t_final_pitch);
                traffic_slot++;
            }
        }

        /* Advance to next viewport */
        viewer_vehicle++;
        pan        = 10000;
        slot_offset = TD5_SOUND_DUP_OFFSET;
    } /* end viewport loop */

    /* ----------------------------------------------------------------
     * Post-loop: siren timeout check
     *
     * If siren_active_flag is set but was NOT refreshed this frame,
     * begin siren fade-out and clear the flag.
     * ---------------------------------------------------------------- */
    if (s_siren_active_flag != 0) {
        if (s_siren_refreshed == 0) {
            if (s_tracked_veh_active != 0) {
                s_tracked_veh_fade_target = 0;
            }
            s_siren_active_flag = 0;
        }
        s_siren_refreshed = 0;
    }
}

/* ========================================================================
 * Positional One-Shot Sound -- PlayVehicleSoundAtPosition (0x441D90)
 *
 * Used for collision and scrape sounds. Plays a spatial one-shot with
 * optional random variant selection. In split-screen, plays twice
 * (once per viewport, using the duplicate slot range for P2).
 * ======================================================================== */

void td5_sound_play_at_position(int base_slot, int volume, int pitch,
                                const int32_t *pos, int num_variants)
{
    int logged_sound_id = base_slot;
    float logged_distance = 0.0f;

    /* Random variant selection */
    if (num_variants > 1) {
        base_slot += rand() % num_variants;
    }
    logged_sound_id = base_slot;

    /* Clamp volume */
    if (volume >= 0x1000) volume = 0xFFF;
    else if (volume < 0)  volume = 0;

    /* Determine number of viewport passes */
    int num_passes;
    int start_pan;
    if (g_td5.split_screen_mode == 0) {
        num_passes = 1;
        start_pan  = 0;
    } else {
        num_passes = 2;
        start_pan  = -10000;
    }

    int current_pan    = start_pan;
    int current_offset = 0;

    for (int pass = 0; pass < num_passes; pass++) {
        /* Set listener for this viewport */
        s_active_listener_vel = s_listener_vel[pass];
        s_active_listener_pos = s_listener_pos[pass];

        /* Compute distance from listener to sound source */
        float dx = ((float)s_active_listener_pos[0] - (float)pos[0]) * TD5_SOUND_DISTANCE_SCALE;
        float dz = ((float)s_active_listener_pos[2] - (float)pos[2]) * TD5_SOUND_DISTANCE_SCALE;
        float dist_sq = dx * dx + dz * dz;
        float dist;
        if (td5_game_is_replay_active()) {
            dist = sqrtf(dist_sq) * TD5_SOUND_REPLAY_DIST_SCALE;
        } else {
            dist = sqrtf(dist_sq);
        }
        if (pass == 0) {
            logged_distance = dist;
        }

        /* Volume attenuation */
        int vol_atten = ((0x7F - ((int)roundf(dist) >> 7)) * (volume >> 5)) / 0x7F;
        if (vol_atten < 0)       vol_atten = 0;
        else if (vol_atten >= 0x80) vol_atten = 0x7F;

        /* Doppler pitch shift */
        int final_pitch = pitch;
        if (vol_atten > 0) {
            /* For one-shot positional sounds, use listener velocity only
             * (no vehicle velocity term -- the source position is static). */
            float doppler_raw = ((float)(s_active_listener_vel[0] >> 8) * dx +
                                 (float)(s_active_listener_vel[2] >> 8) * dz) /
                                (dist + TD5_SOUND_DOPPLER_EPSILON);
            float listener_term = doppler_raw * TD5_SOUND_DOPPLER_VEL_SCALE
                                + TD5_SOUND_DOPPLER_VEL_OFFSET;

            float ratio;
            if (listener_term == TD5_SOUND_DOPPLER_ZERO) {
                ratio = TD5_SOUND_DOPPLER_VEL_OFFSET;
            } else {
                ratio = TD5_SOUND_DOPPLER_VEL_OFFSET / listener_term;
            }

            if (ratio < TD5_SOUND_DOPPLER_ZERO) ratio = TD5_SOUND_DOPPLER_ZERO;
            if (ratio > TD5_SOUND_DOPPLER_MAX)   ratio = TD5_SOUND_DOPPLER_MAX;

            final_pitch = (int)roundf(
                ((ratio - 1.0f) * TD5_SOUND_DOPPLER_PITCH_SCALE + 1.0f) * (float)pitch);
        }

        slot_play(current_offset + base_slot, 0, vol_atten,
                            current_pan, final_pitch);

        /* Second pass uses duplicate slot offset */
        current_pan    = 10000;
        current_offset = TD5_SOUND_DUP_OFFSET;
    }

    TD5_LOG_I(LOG_TAG, "Positional sound: id=%d pos=(%d,%d,%d) distance=%.2f",
              logged_sound_id,
              pos ? (int)pos[0] : 0,
              pos ? (int)pos[1] : 0,
              pos ? (int)pos[2] : 0,
              logged_distance);
}

/* ========================================================================
 * Frontend SFX
 * ======================================================================== */

/**
 * LoadFrontendSoundEffects (0x414640).
 *
 * Loads 10 frontend WAVs from "Front End\Sounds\Sounds.zip" into
 * DXSound slots 1-10. All are one-shot (no looping).
 */
int td5_sound_load_frontend_sfx(void)
{
    const char *zip_path = "Front End\\Sounds\\Sounds.zip";

    for (int i = 0; i < TD5_SOUND_FRONTEND_SFX_COUNT; i++) {
        int slot = s_frontend_sfx_slots[i];
        const char *path = s_frontend_sfx_paths[i];
        const char *entry_name = path;

        if (!path) {
            TD5_LOG_E(LOG_TAG, "Frontend WAV load rejected: null path at index=%d slot=%d",
                      i, slot);
            continue;
        }

        if (s_frontend_sfx_buffer_ids[i] >= 0) {
            continue;
        }

        {
            const char *slash = strrchr(path, '\\');
            if (!slash) slash = strrchr(path, '/');
            if (slash && slash[1] != '\0') entry_name = slash + 1;
        }

        TD5_LOG_I(LOG_TAG, "Frontend WAV load begin: name=%s slot=%d", path, slot);

        /* td5_asset_open_and_read tries loose-file fallback (re/assets/sounds/<name>)
         * before falling back to the ZIP. The OO archive API would silently fail
         * here because shipped builds extract Sounds.zip into re/assets/sounds/. */
        int read_size = 0;
        void *buf = td5_asset_open_and_read(entry_name, zip_path, &read_size);
        if (!buf || read_size <= 0) {
            TD5_LOG_W(LOG_TAG, "Frontend WAV load failed: name=%s slot=%d (file not found)",
                      entry_name, slot);
            if (buf) free(buf);
            continue;
        }

        /* Load into platform audio buffer (one-shot, no duplicates). */
        int buffer_id = td5_plat_audio_load_wav(buf, (size_t)read_size);
        if (buffer_id < 0) {
            TD5_LOG_E(LOG_TAG, "Frontend WAV load failed: name=%s slot=%d bytes=%d",
                      entry_name, slot, read_size);
        } else {
            s_frontend_sfx_buffer_ids[i] = buffer_id;
            TD5_LOG_I(LOG_TAG, "Frontend WAV load complete: name=%s buffer=%d slot=%d bytes=%d",
                      entry_name, buffer_id, slot, read_size);
        }

        free(buf);
    }

    return 1;
}

/**
 * Play a frontend sound effect by SFX ID (0-9).
 * Maps to DXSound::Play(slot) for the corresponding slot.
 */
void td5_sound_play_frontend_sfx(int sfx_id)
{
    int index = sfx_id;

    /* Original frontend code uses 1-based sound IDs; accept 0-based too. */
    if (index > 0 && index <= TD5_SOUND_FRONTEND_SFX_COUNT) {
        index -= 1;
    }

    if (index < 0 || index >= TD5_SOUND_FRONTEND_SFX_COUNT) return;
    if (s_frontend_sfx_buffer_ids[index] < 0) return;
    td5_plat_audio_play(s_frontend_sfx_buffer_ids[index], 0, 0x7F, 0, TD5_SOUND_FREQ_22050);
}

/* ========================================================================
 * CD Audio Passthrough
 * ======================================================================== */

void td5_sound_cd_play(int track) {
    TD5_LOG_I(LOG_TAG, "CD play track=%d", track);
    td5_plat_cd_play(track);
}

void td5_sound_cd_stop(void) {
    TD5_LOG_I(LOG_TAG, "CD stop");
    td5_plat_cd_stop();
}
void td5_sound_cd_set_volume(int v) { td5_plat_cd_set_volume(v); }

/* ========================================================================
 * Master Volume
 * ======================================================================== */

void td5_sound_set_sfx_volume(int v) { td5_plat_audio_set_master_volume(v); }
void td5_sound_set_music_volume(int v) { td5_plat_cd_set_volume(v); }

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/**
 * Compute 2D (XZ plane) distance from listener to a vehicle position,
 * scaled by the audio distance factor.
 */
static float sound_compute_distance(const int32_t *listener_pos,
                                    const int32_t *vehicle_pos)
{
    float dx = ((float)listener_pos[0] - (float)vehicle_pos[0]) * TD5_SOUND_DISTANCE_SCALE;
    float dz = ((float)listener_pos[2] - (float)vehicle_pos[2]) * TD5_SOUND_DISTANCE_SCALE;
    return sqrtf(dx * dx + dz * dz);
}

/**
 * Compute Doppler ratio from listener and vehicle velocities projected
 * onto the line of sight.
 *
 * Original formula (from 0x440E71 and 0x441296):
 *   v_vehicle = dot(vehicle_vel >> 8, normalize(dx,dz))
 *   v_listener = dot(listener_vel >> 8, normalize(dx,dz))
 *   listener_term = v_listener * 4.0 + 4096.0
 *   vehicle_term  = v_vehicle  * 4.0 + 4096.0
 *   if listener_term == 0: ratio = vehicle_term
 *   else:                  ratio = vehicle_term / listener_term
 *   clamp ratio to [0.0, 2.0]
 */
static float sound_compute_doppler_ratio(const int32_t *listener_vel,
                                         const int32_t *vehicle_vel,
                                         float dx, float dz, float dist)
{
    float safe_dist = dist + TD5_SOUND_DOPPLER_EPSILON;

    /* Project vehicle velocity onto line-of-sight */
    float v_veh = ((float)(vehicle_vel[0] >> 8) * dx +
                   (float)(vehicle_vel[2] >> 8) * dz) / safe_dist;

    /* Project listener velocity onto line-of-sight */
    float v_lis = ((float)(listener_vel[0] >> 8) * dx +
                   (float)(listener_vel[2] >> 8) * dz) / safe_dist;

    float vehicle_term  = v_veh * TD5_SOUND_DOPPLER_VEL_SCALE + TD5_SOUND_DOPPLER_VEL_OFFSET;
    float listener_term = v_lis * TD5_SOUND_DOPPLER_VEL_SCALE + TD5_SOUND_DOPPLER_VEL_OFFSET;

    float ratio;
    if (listener_term == 0.0f) {
        ratio = vehicle_term;
    } else {
        ratio = vehicle_term / listener_term;
    }

    /* Clamp to [0.0, 2.0] */
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > TD5_SOUND_DOPPLER_MAX) ratio = TD5_SOUND_DOPPLER_MAX;

    return ratio;
}

/**
 * Attenuate volume by distance.
 *
 * Formula: vol = ((0x7F - (round(distance) >> 7)) * raw_vol) / 0x7F
 * Clamped to [0, 0x7F].
 */
static int sound_attenuate_volume(int raw_vol, float distance)
{
    int dist_int = (int)roundf(distance);
    int atten = (0x7F - (dist_int >> 7));
    if (atten < 0) atten = 0;
    int vol = (atten * raw_vol) / 0x7F;
    if (vol < 0)       return 0;
    if (vol >= 0x80)   return 0x7F;
    return vol;
}

/**
 * Apply Doppler ratio to a base pitch value.
 *
 * Formula: pitch = round(((ratio - 1.0) * 0.074074 + 1.0) * base_pitch)
 */
static int sound_apply_doppler_pitch(int base_pitch, float doppler_ratio)
{
    float adjusted = ((doppler_ratio - 1.0f) * TD5_SOUND_DOPPLER_PITCH_SCALE + 1.0f)
                   * (float)base_pitch;
    /* The original uses TD5_SOUND_DOPPLER_PITCH_BASE (1.0) as multiplier
     * for the base pitch in some paths; this is equivalent. */
    return (int)roundf(adjusted);
}

/**
 * Load a WAV file from a ZIP archive into a platform audio buffer slot.
 *
 * This mirrors the original pattern: find entry size in ZIP, allocate a
 * staging buffer, read the entry, call DXSound::LoadBuffer, free the buffer.
 */
static int sound_load_wav_from_zip(const char *wav_name, const char *zip_path,
                                   int slot, int loop, int duplicates)
{
    (void)loop;       /* Loop flag is passed through to platform layer */
    (void)duplicates; /* Duplicate count handled by platform layer */

    if (!wav_name || !zip_path) {
        TD5_LOG_E(LOG_TAG, "WAV load rejected: wav_name=%p zip_path=%p slot=%d",
                  (const void *)wav_name, (const void *)zip_path, slot);
        return -1;
    }

    if (slot < 0 || slot >= TD5_SOUND_MAX_BASE_SLOTS) {
        TD5_LOG_E(LOG_TAG, "WAV load rejected: name=%s invalid slot=%d max=%d",
                  wav_name, slot, TD5_SOUND_MAX_BASE_SLOTS);
        return -1;
    }

    TD5_LOG_I(LOG_TAG, "WAV load attempt begin: name=%s zip=%s slot=%d loop=%d dup=%d",
              wav_name, zip_path, slot, loop, duplicates);

    /* td5_asset_open_and_read handles loose-file fallback (re/assets/<subfolder>/<name>)
     * before reaching for the ZIP. Builds ship extracted assets, so the ZIP open
     * always fails and the OO archive API would never get past it. */
    int read_size = 0;
    void *buf = td5_asset_open_and_read(wav_name, zip_path, &read_size);
    if (!buf || read_size <= 0) {
        TD5_LOG_W(LOG_TAG, "WAV not found: name=%s zip=%s slot=%d", wav_name, zip_path, slot);
        if (buf) free(buf);
        return -1;
    }

    int buffer_id = td5_plat_audio_load_wav(buf, (size_t)read_size);
    if (buffer_id < 0) {
        TD5_LOG_E(LOG_TAG,
                  "WAV load attempt failed: name=%s slot=%d bytes=%d loop=%d dup=%d",
                  wav_name, slot, read_size, loop, duplicates);
    } else {
        s_slot_to_buffer[slot] = buffer_id;
        if (duplicates > 0 && slot + TD5_SOUND_DUP_OFFSET < TD5_SOUND_TOTAL_SLOTS)
            s_slot_to_buffer[slot + TD5_SOUND_DUP_OFFSET] = buffer_id;
        TD5_LOG_I(LOG_TAG,
                  "WAV load attempt complete: name=%s buffer=%d slot=%d bytes=%d loop=%d dup=%d",
                  wav_name, buffer_id, slot, read_size, loop, duplicates);
    }

    free(buf);
    return buffer_id;
}

/* ========================================================================
 * State Accessors (for other modules to feed data into the sound system)
 * ======================================================================== */

void td5_sound_set_listener_pos(int viewport, int32_t x, int32_t y, int32_t z)
{
    if (viewport < 0 || viewport > 1) return;
    s_listener_pos[viewport][0] = x;
    s_listener_pos[viewport][1] = y;
    s_listener_pos[viewport][2] = z;
}

void td5_sound_set_skid_intensity(int viewport, int intensity)
{
    if (viewport < 0 || viewport > 1) return;
    s_skid_intensity[viewport] = intensity;
}

void td5_sound_set_gear_state(int vehicle, int gear)
{
    if (vehicle < 0 || vehicle >= 6) return;
    s_gear_state[vehicle] = gear;
}

void td5_sound_set_race_end(int ended)
{
    s_race_end_flag = ended;
}
