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
#include "td5_music.h"   /* pluggable music-backend seam (replaces direct td5_plat_cd_*) */
#include "td5_radio.h"   /* internet-radio music backend */
#include "td5_platform.h"
#include "td5_asset.h"
#include "td5_game.h"  /* td5_game_get_player_slot, is_replay_active, etc. */
#include "td5re.h"
#include "td5_config.h" /* shared TD5RE_* env-knob helpers */
#include "td5_vfx.h"
#include "td5_ai.h"    /* td5_ai_traffic_get_draw_alpha (dynamic-traffic fade) */
#include "td5_physics.h" /* td5_physics_get_crash_fx — crash-SFX trigger (Item #12) */

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

/** Traffic engine variant WAV filenames.
 *
 * The original table @0x474A4C maps variant 0/1/2 -> {Engine0.wav, car.wav,
 * diesel.wav}, indexed by GetTrafficVehicleVariantType @0x443240. BUT car.wav
 * and diesel.wav exist in NO original archive -- SOUND.ZIP ships only
 * engine0.wav..engine5.wav (and only engine0 is referenced by the table). So
 * the ORIGINAL game itself loads no engine sound for traffic variants 1/2
 * (which covers most traffic), leaving them silent -- evidently a dev filename
 * bug (6 engine variants shipped, 5 unused).
 *
 * DEVIATION (user-approved 2026-05-31): point the two missing names at the
 * shipped engine1.wav / engine2.wav so traffic cars are actually audible,
 * restoring the apparent dev intent. To restore byte-faithful (silent)
 * behavior, change these back to "car.wav" / "diesel.wav". */
static const char *s_traffic_engine_wavs[3] = {
    "Engine0.wav",  /* variant 0: generic (faithful; exists) */
    "engine1.wav",  /* variant 1: was "car.wav"    (missing in all archives) */
    "engine2.wav"   /* variant 2: was "diesel.wav" (missing in all archives) */
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
    /* [S11] Looping voices are deduped per source buffer inside
     * td5_plat_audio_play (one voice per loop buffer, reused on re-issue), which
     * is what prevents the per-frame loop re-trigger from stranding voices and
     * pegging the pool. So slot_play just records the (possibly reused) channel. */
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
/* Whether this slot still owns a live voice (a parked/idle loop counts), as
 * opposed to slot_is_playing() which reflects the backend's transient PLAYING
 * bit. Used by the engine looping-state update so a momentarily-parked engine
 * loop is not treated as gone and re-triggered every frame. */
static int slot_has_voice(int slot) {
    if (slot < 0 || slot >= TD5_SOUND_TOTAL_SLOTS) return 0;
    int ch = s_slot_to_channel[slot];
    if (ch < 0) return 0;
    return td5_plat_audio_channel_valid(ch);
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
 * Maximum number of traffic vehicles that get a dedicated engine-loop voice.
 *
 * The original sound bank reserves exactly 7 traffic engine slots (37..43, see
 * re/analysis/wave4_deep_audits/da_m3_dxsound_polyphony.md §A.2). The mixer
 * plays slot 37+(t-6) and modifies slot 38+(t-6), so keeping BOTH within the
 * 37..43 dedicated range — and out of the duplicate-buffer range that starts at
 * slot 44 — bounds the count at 6. This also bounds the per-pass state index
 * (pass + (t-6)*2, max 11) inside s_traffic_engine_state[].
 *
 * [S11 fix] The N-way build allows up to TD5_MAX_TOTAL_ACTORS (22) actors, and
 * the mixer treats every actor >= 6 as "traffic". The old uncapped loop wrote
 * s_traffic_engine_state[12] at indices up to 31 (a 20-int out-of-bounds write
 * that corrupted the adjacent horn/tracked/reverb state arrays) and played
 * traffic engines into the duplicate-buffer slots (44+), orphaning real voices.
 * Both effects scaled with player count — the dominant ">6 players" trigger for
 * the stuck-sound overload. The mixer loop is now capped at this many traffic
 * voices; extra actors simply get no dedicated engine loop (logged, not silent).
 */
#define TD5_SOUND_TRAFFIC_AUDIO_MAX 6

/**
 * Per-traffic-vehicle engine state for each viewport pass.
 * Index: pass + (t-6)*2, bounded by TD5_SOUND_TRAFFIC_AUDIO_MAX * 2.
 * Original: DAT_004c37a0.
 */
static int s_traffic_engine_state[TD5_SOUND_TRAFFIC_AUDIO_MAX * 2];

/* One-shot guard so the "traffic audio capped" notice logs once per race, not
 * every frame. Reset in td5_sound_init_race_resources. */
static int s_traffic_audio_cap_logged;

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
static int32_t s_listener_pos[TD5_MAX_VIEWPORTS][3];

/** Previous frame listener position for velocity delta.
 *  Original: DAT_004c38c0. */
static int32_t s_listener_prev_pos[TD5_MAX_VIEWPORTS][3];

/** Listener velocity delta. Original: DAT_004c3888, DAT_004c388c, DAT_004c3890. */
static int32_t s_listener_vel[TD5_MAX_VIEWPORTS][3];

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

/* Police/siren positional-audio fix (#15, PORT ENHANCEMENT 2026-06-15).
 *
 * Two bugs in the tracked-vehicle (cop siren) audio path:
 *   1. DISTANCE: the siren fade level lives in the 0..0x1000 domain, but every
 *      other positional loop (engine line ~1248, traffic line ~1505) pre-scales
 *      its level by >> 5 into 0..0x7F before sound_attenuate_volume(). The siren
 *      path passed the raw 0x1000 level straight in; since the attenuator
 *      computes (atten * raw_vol) / 0x7F and clamps to 0x7F, a 0x1000 input
 *      stays >= 0x7F until the cop is extremely far -> siren pinned at full
 *      volume regardless of distance.
 *   2. HARD CUT: the fade processing snapped the level to FULL or 0 in a single
 *      tick, so the siren popped on at chase start and cut off abruptly at
 *      chase end instead of fading.
 *
 * Both fixes are gated on TD5RE_POLICE_AUDIO_FIX (default ON; "0" reverts to the
 * old raw-level + binary-snap behavior). */
#define TD5_SOUND_SIREN_FADE_RAMP_STEP 0x100  /* per-tick ramp -> ~0.5s @ 30Hz */
static int sound_police_audio_fix_enabled(void)
{
    static int s_enabled = -1;   /* -1 = env not yet read */
    if (s_enabled < 0) {
        const char *env = getenv("TD5RE_POLICE_AUDIO_FIX");
        s_enabled = (env && env[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "police-audio fix %s (TD5RE_POLICE_AUDIO_FIX=%s)",
                  s_enabled ? "ENABLED" : "disabled",
                  env ? env : "default");
    }
    return s_enabled;
}

/* Cop-chase siren on/off toggle (PORT ENHANCEMENT, user-requested 2026-05-30).
 *
 * In the original, the siren is gated on the horn control bit (0x200000):
 * UpdateVehicleLoopingAudioState @ 0x00440A30 is only *called* while the horn
 * is HELD (gate @ 0x0042C260), so releasing the horn lets the mixer timeout
 * fade the siren out. The port instead calls the looping-state update for
 * every slot every frame (td5_game.c), so the cop's siren was re-activated
 * unconditionally and could never be silenced ("can't deactivate sirens").
 *
 * The user prefers a press-to-toggle instead of hold-to-keep-on. This flag is
 * flipped by a horn-key edge in td5_input.c (td5_sound_toggle_siren); the
 * wanted-mode siren activation below is gated on it. Default 0 (off) — the
 * siren and its coupled flashing-light marker (AdvanceGlobalSkyRotation, in
 * the same branch) start silent and the player presses the horn to turn the
 * cop lights+siren on, matching the original's "press horn → siren on". */
static int s_siren_user_enabled;

/* [COP CHASE SIREN PER-COP 2026-06-27] Per-cop siren on/off state. Local MP cop
 * chase can have several human cops, each with its OWN siren — and the arrest
 * rule ("a cop can only arrest with its siren on", td5_ai_wanted_cop_hit) is
 * per-cop. The single global s_siren_user_enabled above stays as the OR of all
 * per-cop flags so the (single-channel) siren audio + the strobe-light marker
 * keep working unchanged; td5_sound_toggle_cop_siren maintains both. */
static uint8_t s_cop_siren_on[TD5_MAX_RACER_SLOTS];

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
 * [ITEM #12] Crash impact SFX — per-player-slot crash-sequence tracking.
 *
 * The physics module publishes a monotonically-increasing crash sequence id
 * per actor slot via td5_physics_get_crash_fx(slot, &mag, &age). Each frame
 * td5_sound_update_crash_sfx() polls every active player slot; when the
 * returned id exceeds the stored one a NEW heavy impact happened, so we fire
 * a spatial one-shot crash sound (the dedicated HHit* "heavy hit" bank that
 * the original loads for collisions) at the crashing car's world position,
 * with volume scaled by the impact magnitude. Gated by TD5RE_CRASH_SFX
 * (default ON); "0" reverts to no crash SFX. NOTE: this is PRESENTATION only
 * (audio playback) — it reads physics state but never writes it, so it has no
 * effect on simulation / netplay determinism.
 * ======================================================================== */

/* Heavy-impact ("HHit") bank: first slot + variant count.
 * From s_ambient_wav_names[]: HHit1..HHit4 live at ambient slots 27..30
 * (TD5_SOUND_AMBIENT_SLOT_BASE 18 + index 9). These are the original's
 * dedicated collision/impact one-shots. */
#define TD5_SOUND_CRASH_HHIT_SLOT   (TD5_SOUND_AMBIENT_SLOT_BASE + 9)  /* 27 */
#define TD5_SOUND_CRASH_HHIT_COUNT  4
/* Crash magnitude that maps to full-volume impact. The physics module only
 * records an event for ACUTE impacts (impact_mag > ~250000, see
 * td5_physics.c CRASH_FX_ACUTE_MAG), so reported crashes already clear the
 * floor; this reference (~4x the acute threshold) gives a "big vs huge"
 * volume gradient above it rather than always playing at max. */
#define TD5_SOUND_CRASH_MAG_REF     1000000

static uint32_t s_last_crash_seq[TD5_MAX_RACER_SLOTS];

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

/* [MP audio 2026-06-24] In 3+ split the mixer runs ONE listener pass (player 1's
 * camera), so a non-human car (AI opponent / traffic) far from player 1 but next
 * to player 4 would attenuate to silence and stay centred. This finds the human
 * player car nearest `src`, writing the scaled dx/dz, squared distance and that
 * car's velocity ptr; returns the human index (or -1 if none — caller falls back
 * to the pass listener). A car near ANY human plays loud and pans to that
 * human's pane, so every local player hears the cars around them. */
static int sound_nearest_human_listener(const TD5_Actor *src, int num_human,
                                        float *out_dx, float *out_dz,
                                        float *out_dist2, const int32_t **out_vel);

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
    /* Apply the configured SFX volume now. Previously the platform master volume
     * kept its hard-coded default (40) until the frontend Options screen pushed
     * the saved value — so any race entered before visiting Options (AutoRace,
     * or just driving straight in) mixed every SFX at 40% through the wrong part
     * of the volume curve, which made the whole mix sound weak/flat. */
    td5_plat_audio_set_master_volume(g_td5.ini.sfx_volume);
    /* Install the pluggable music seam (default backend = legacy CD path, so
     * no behavior change until a backend is registered). */
    td5_music_init();
    /* When enabled, the internet-radio backend becomes the active music source.
     * It connects lazily on the first play(); a failed/absent network just
     * leaves music silent (same as the CD path on a disc-less PC). If Media
     * Foundation is unavailable, td5_radio_get_backend() returns NULL and the
     * seam keeps the default CD backend. */
    if (g_td5.ini.radio_enabled) {
        td5_radio_init(g_td5.ini.radio_url, g_td5.ini.radio_volume);
        td5_music_set_backend(td5_radio_get_backend());
    }
    TD5_LOG_I(LOG_TAG, "audio subsystem initialized (sfx master=%d, radio=%d)",
              g_td5.ini.sfx_volume, g_td5.ini.radio_enabled);
    return 1;
}

void td5_sound_shutdown(void)
{
    td5_radio_shutdown();   /* stop the worker before the seam/sink go away */
    td5_music_shutdown();
    td5_plat_audio_shutdown();
}

void td5_sound_tick(void)
{
    td5_plat_audio_stream_refresh();
    td5_music_tick();   /* pump the active music backend (no-op for the default) */

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
/* [#8 PAUSE-MUTE STATE 2026-06-19] File-scope so the race-init below can reset it.
 * The pause-mute edge state used to be a function-local static in
 * td5_sound_set_paused(); a stale value carried across races meant the 2nd+ race's
 * pause hit the "no change" early-out and never re-applied the mute (engine kept
 * playing). Reset it per race + force audio audible on race start. */
static int s_sound_paused = 0;

int td5_sound_init_race_resources(void)
{
    /* [#8] Fresh audio state for this race: drop any leftover pause suspend from
     * a previous race that ended while paused (which would otherwise leave the
     * engine/SFX stuck or the next race silent), and clear the pause edge so the
     * next pause correctly re-applies the mute. */
    s_sound_paused = 0;
    td5_plat_audio_set_muted(0);
    td5_music_set_volume(g_td5.ini.music_volume);

    memset(s_engine_state, 0, sizeof(s_engine_state));
    memset(s_traffic_engine_state, 0, sizeof(s_traffic_engine_state));
    memset(s_tracked_audio_state, 0, sizeof(s_tracked_audio_state));
    memset(s_horn_state, 0, sizeof(s_horn_state));
    memset(s_reverb_flag, 0, sizeof(s_reverb_flag));
    memset(s_listener_pos, 0, sizeof(s_listener_pos));
    memset(s_listener_prev_pos, 0, sizeof(s_listener_prev_pos));
    memset(s_listener_vel, 0, sizeof(s_listener_vel));
    memset(s_gear_state, 0, sizeof(s_gear_state));
    memset(s_last_crash_seq, 0, sizeof(s_last_crash_seq));  /* [ITEM #12] crash-SFX edge */

    s_reverb_actor_index    = 0;
    s_tracked_veh_active    = 0;
    s_tracked_veh_active_p2 = 0;
    s_tracked_veh_fade_target = 0;
    s_tracked_veh_fade_level  = 0;
    s_tracked_veh_actor     = 0;
    s_siren_active_flag     = 0;
    s_siren_refreshed       = 0;
    s_siren_user_enabled    = 0;   /* siren starts off; press horn to enable */
    memset(s_cop_siren_on, 0, sizeof(s_cop_siren_on));   /* per-cop sirens off */
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
    s_traffic_audio_cap_logged = 0;

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
 *
 * [ARCH-DIVERGENCE: DXSound::Stop/Remove -> td5_plat_audio; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00441D50: orig stops slots 0x2c..0x57 (DXSound::Stop)
 *   then removes slots 0x00..0x2b (DXSound::Remove); returns 1. Port walks
 *   the same two ranges using slot_stop and td5_plat_audio_free
 *   (TD5_SOUND_DUP_OFFSET == 0x2c, TD5_SOUND_TOTAL_SLOTS == 0x58,
 *   TD5_SOUND_MAX_BASE_SLOTS == 0x2c -- match by definition), with the
 *   port also scrubbing s_slot_to_buffer / its duplicate mirror to track
 *   per-slot buffer ownership (no orig-side equivalent because DXSound
 *   tracked it internally). */
void td5_sound_release_race_channels(void)
{
    td5_plat_audio_log_stats("race-end:before");

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

    /* [S11] Backstop: force every remaining SFX voice to release so the voice
     * count returns to baseline (0) between races. The per-slot stops above only
     * release voices the slot->channel table still tracks; any voice that was
     * orphaned (e.g. a loop whose slot mapping was overwritten or a stolen
     * voice's stale handle) would otherwise linger and keep looping into the
     * menus or next race. stop_all also covers them. The silent keepalive buffer
     * is separate and stays running. */
    td5_plat_audio_stop_all();
    memset(s_slot_to_channel, 0xFF, sizeof(s_slot_to_channel));
    td5_plat_audio_log_stats("race-end:after");
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
 *
 * [ARCH-DIVERGENCE: 4 "99" markers collapsed to per-state array writes; L5 sweep 2026-05-21]
 *   Orig sets four 99-value markers on entry (one per array):
 *     (&DAT_004c3774)[param_2 * 2] = 99;
 *     (&g_engineRevLoopStateByActorView)[param_2 * 2] = 99;
 *     (&g_engineLoopStateByActorView)[param_2 * 2] = 99;
 *     (&g_sirenChannelPlayStateByActorView + param_2 * 8) = 99;
 *   Port consolidates audio per-slot state into s_engine_state / s_tracked_audio_state
 *   arrays and uses the ENGINE_STATE_STOPPED enum (=99) to mark uninitialized.
 *   The siren-channel marker is intentionally folded into s_siren_refreshed /
 *   s_siren_active_flag flag resets at the end of the function instead of an
 *   array-indexed marker write. */
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
    s_siren_user_enabled = 0;   /* siren off until the player presses the horn */
    memset(s_cop_siren_on, 0, sizeof(s_cop_siren_on));   /* per-cop sirens off */

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

    /* Load traffic engine loops for actors 6+.
     * [S11] Bound to the dedicated traffic slots (37..). Without this, a small
     * g_traffic_slot_base with many actors drives slot = 37+i past 43 into the
     * 44+ duplicate-buffer range, which sound_load_wav_from_zip then rejects one
     * by one (noise) — and which the mixer is now capped not to use anyway. */
    int total_actors = td5_game_get_total_actor_count();
    if (total_actors > g_traffic_slot_base) {
        int traffic_count = total_actors - g_traffic_slot_base;
        if (traffic_count > TD5_SOUND_TRAFFIC_AUDIO_MAX)
            traffic_count = TD5_SOUND_TRAFFIC_AUDIO_MAX;
        for (int i = 0; i < traffic_count; i++) {
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

    /* [PORT] audio mixes at most 2 listener passes (P1 base + P2 @slot+44);
     * N-way split renders >2 panes but audio stays a 2-listener mix. */
    int num_passes  = (g_td5.viewport_count > 1) ? 2 : 1;
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
 *
 * [CONFIRMED @ 0x00440A30 wanted-mode branch] — Ghidra-verified: orig
 * checks g_wantedModeEnabled && slotIndex == g_wantedTargetSlotIndex, and
 * when g_sirenRefreshedThisFrame==0 writes
 *   _g_trackedVehicleAudioActiveView0Mirror_PROVISIONAL = 1
 *   gTrackedVehicleAudioActive = 1
 *   gTrackedVehicleAudioActorIndex = slotIndex
 *   gTrackedVehicleAudioFadeTarget = 0x1000
 * then unconditionally calls AdvanceGlobalSkyRotation and sets
 * g_sirenActiveFlag=1, g_sirenRefreshedThisFrame=1. Port lines 575-588
 * match exactly: s_tracked_veh_active_p2/active/actor/fade_target writes
 * gated on s_siren_active_flag==0, td5_game_advance_sky_rotation, then
 * s_siren_refreshed=1 + s_siren_active_flag=1.
 *
 * [ARCH-DIVERGENCE: non-cop branch — DXSound::Status query → slot_is_playing
 *  poll; skid-loop state writes → engine-state writes] — orig fall-through
 *  queries DXSound::Status(slotIndex*3+3) (the buffer for actor+1's drive
 *  slot, or the duplicate ring) and, when idle (status==0), sets
 *  (&g_skidLoopFlagAlternativeByActorView_PROVISIONAL)[slotIndex*2]=1 and
 *  (&g_skidLoopStateByActorView)[slotIndex*2]=1 (i.e., requests a skid loop
 *  refresh). Port lines 589-594 instead poll slot_is_playing on the local
 *  engine slot (base_slot, base_slot+1) and resets s_engine_state to
 *  ENGINE_STATE_STOPPED when neither plays. Different semantics: orig
 *  reactivates skid loop when a different DX slot is idle, port resets
 *  engine looping marker. The DX-side queue model and the per-slot semantic
 *  of "playing" differ enough that a 1:1 port would also require porting
 *  the orig skid-loop state machine (currently consolidated into
 *  s_skid_playing). L5 promotion sweep 2026-05-21.
 */
void td5_sound_update_vehicle_looping_state(int actor_index)
{
    int base_slot = actor_index * 3;

    /* Check if wanted mode is active and this is the cop */
    if (td5_game_is_wanted_mode() && actor_index == td5_game_get_cop_actor_index()) {
        /* PORT ENHANCEMENT: gate on the press-to-toggle flag (s_siren_user_enabled,
         * flipped by the horn key in td5_input.c) instead of the original's
         * hold-the-horn gating. When the toggle is OFF we simply DON'T refresh
         * the siren this frame; the mixer's post-loop timeout (s_siren_active_flag
         * set but s_siren_refreshed clear) then fades it out and clears the flag.
         * AdvanceGlobalSkyRotation (the flashing-light marker intensity boost)
         * lives in this same branch in the original, so toggling off also lets
         * the cop-light pulse decay — siren and lights stay coupled, as in the
         * original where both are driven by this horn-gated call. */
        /* [COP-CHASE SIREN PAUSE FIX 2026-06-21] Do NOT refresh the siren while the
         * pause menu is open. The pause path also calls td5_sound_stop_tracked_vehicle_audio
         * (fade out) and the mixer then physically stops the siren channels + clears
         * s_tracked_veh_active. If we kept refreshing here, s_siren_active_flag would
         * stay latched at 1 every paused frame, so on resume the
         * `s_siren_active_flag == 0` re-activation branch below — the ONLY place that
         * re-arms s_tracked_veh_active=1 to re-issue slot_play — would never fire and
         * the siren would stay dead. Skipping the refresh during pause lets the mixer's
         * post-loop timeout clear the flag, so resume re-arms and re-plays cleanly.
         * Mirrors the pause gating already on td5_sound_update_police_siren. */
        if (s_siren_user_enabled && !td5_game_is_pause_menu_active()) {
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
        }
        return;
    }

    /* Keep the looping engine state latched to the existing channel rather than
     * requesting a fresh play every frame.
     *
     * [S11] Gate on slot_has_voice(), NOT slot_is_playing(). The engine Drive
     * loop plays at volume 0 under the audible Rev loop, and a volume-0 buffer is
     * parked by WASAPI so its DirectSound status reads not-PLAYING — even though
     * the voice is alive and being modulated. Using slot_is_playing() here saw
     * the parked loop as gone and reset the state to STOPPED every frame, which
     * made the mixer re-issue Drive+Rev every frame (the broken/muffled engine
     * and the churn that fed the overload). slot_has_voice() only resets when the
     * slot has genuinely lost its voice (handle released/stale), so a parked but
     * owned loop is left alone. */
    if (!slot_has_voice(base_slot) &&
        !slot_has_voice(base_slot + 1)) {
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

/**
 * [POLICE rewrite 2026-06-19] Drive the cop-chase siren from the rewritten
 * traffic chase. Called once per frame from the audio tick (after the listener
 * position is set). When the POLICE option is on and a cop is actively chasing,
 * it positions the (single) distance-attenuated siren channel on the NEAREST
 * chasing cop to the listener — so the siren swells as a pursuit closes on you
 * and fades as it pulls away (the existing sound_attenuate_volume in the mixer
 * does the distance math). When POLICE is off, or no cop is chasing, the siren
 * is simply not refreshed and the mixer's post-loop timeout fades it out.
 *
 * Purely cosmetic: runs in the per-frame audio path (never the sim tick) and
 * uses the LOCAL camera, so per-peer distance differences carry no netplay
 * desync risk. The which-cop-is-chasing state it reads is deterministic sim
 * state shared by all peers; only the local distance weighting differs.
 */
void td5_sound_update_police_siren(void)
{
    int nearest;
    /* POLICE option off -> cops are silent. */
    if (!g_td5.special_encounter_enabled) return;
    /* Cop Chase game mode drives the siren via its own horn-toggle path. */
    if (td5_game_is_wanted_mode()) return;

    nearest = td5_ai_nearest_chasing_cop(s_listener_pos[0][0], s_listener_pos[0][2]);
    if (nearest < 0) {
        /* No active chase -> stop the siren (begin fade-out). Just leaving it
         * un-refreshed didn't reliably silence it, so stop it explicitly. */
        td5_sound_stop_tracked_vehicle_audio();
        return;
    }

    if (s_siren_active_flag == 0) {
        s_tracked_veh_active_p2 = 1;
        s_tracked_veh_active    = 1;
    }
    /* Attenuate by the nearest chasing cop's distance to the listener. */
    s_tracked_veh_actor       = nearest;
    s_tracked_veh_fade_target = TD5_SOUND_SIREN_FADE_FULL;
    s_siren_refreshed   = 1;
    s_siren_active_flag = 1;
}

/**
 * td5_sound_toggle_siren (PORT ENHANCEMENT, user-requested 2026-05-30).
 *
 * Flip the cop-chase siren on/off. Called on a horn-key press edge from
 * td5_input.c. The actual fade-in/out is handled by the looping-state update
 * (which refreshes the siren only while s_siren_user_enabled is set) and the
 * mixer timeout (which fades it out when not refreshed). Returns the new
 * state so the caller can log it.
 */
int td5_sound_toggle_siren(void)
{
    s_siren_user_enabled = !s_siren_user_enabled;
    if (!s_siren_user_enabled) {
        /* Begin fade-out immediately so the toggle feels responsive rather
         * than waiting a frame for the looping-state/mixer timeout. */
        td5_sound_stop_tracked_vehicle_audio();
    }
    TD5_LOG_I(LOG_TAG, "siren toggle -> %s", s_siren_user_enabled ? "ON" : "OFF");
    return s_siren_user_enabled;
}

/** Query the current cop-chase siren toggle state (1 = on). */
int td5_sound_siren_is_enabled(void)
{
    return s_siren_user_enabled;
}

/* [COP CHASE SIREN PER-COP 2026-06-27] Toggle ONE cop's siren (local MP cop
 * chase — each human cop drives its own). Recomputes the global
 * s_siren_user_enabled as the OR of every per-cop flag so the shared siren
 * audio + strobe-light marker still light whenever any cop has its siren on.
 * Returns the new per-cop state. Mirrors td5_sound_toggle_siren's responsive
 * fade-out when the global drops to off. */
int td5_sound_toggle_cop_siren(int slot)
{
    if ((unsigned)slot >= (unsigned)TD5_MAX_RACER_SLOTS)
        return 0;
    s_cop_siren_on[slot] = !s_cop_siren_on[slot];

    int any = 0;
    for (int i = 0; i < TD5_MAX_RACER_SLOTS; i++)
        if (s_cop_siren_on[i]) { any = 1; break; }

    int was = s_siren_user_enabled;
    s_siren_user_enabled = any;
    if (was && !any) {
        /* Last siren just went off — fade out immediately like the single
         * toggle so it feels responsive. */
        td5_sound_stop_tracked_vehicle_audio();
    }
    TD5_LOG_I(LOG_TAG, "cop siren toggle slot=%d -> %s (global=%s)",
              slot, s_cop_siren_on[slot] ? "ON" : "OFF", any ? "ON" : "OFF");
    return s_cop_siren_on[slot];
}

/** Query ONE cop's per-cop siren state (1 = on). Drives the per-cop arrest gate
 * in td5_ai_wanted_cop_hit. */
int td5_sound_cop_siren_is_on(int slot)
{
    if ((unsigned)slot >= (unsigned)TD5_MAX_RACER_SLOTS)
        return 0;
    return s_cop_siren_on[slot];
}

/**
 * td5_sound_play_horn (REGULAR-CAR HORN — PORT ENHANCEMENT).
 *
 * Trigger the one-shot horn honk for a race vehicle. The original TD5 binary
 * loaded each car's Horn.wav into audio slot i*3+2 but NEVER played it — the
 * horn control bit 0x200000 only drove the cop siren + a remote-brake cheat
 * (RE-confirmed @ 0x00440a30 / 0x00441a80). The port already has the full
 * horn-playback block in td5_sound_update_audio_mix (gated on
 * s_horn_state[pass + veh*2] == 1) but nothing ever set the state, so the
 * block was dead. This sets it.
 *
 * s_horn_state is indexed [pass + veh*2] (6 vehicles x 2 viewport passes), so
 * arm BOTH passes for the vehicle — the mix renders each pass with its own
 * listener pan/volume, so a split-screen pane only hears the honk if its pass
 * is armed. Setting state=1 mid-play is harmless: the mix won't restart an
 * already-playing one-shot (it only slot_plays when the slot is idle), so a
 * second call while honking is a no-op rather than a stutter.
 */
void td5_sound_play_horn(int actor_index)
{
    if (actor_index < 0 || actor_index >= TD5_SOUND_MAX_RACE_VEHICLES) {
        return;
    }
    s_horn_state[actor_index * 2]     = 1;  /* viewport pass 0 */
    s_horn_state[actor_index * 2 + 1] = 1;  /* viewport pass 1 */
    TD5_LOG_I(LOG_TAG, "horn honk: vehicle slot=%d (Horn.wav slot=%d)",
              actor_index, actor_index * 3 + 2);
}

/* ========================================================================
 * [ITEM #12] Crash impact SFX trigger
 * ======================================================================== */

/* TD5RE_CRASH_SFX (default 1): 0 disables the per-slot crash impact sound. */
static int td5_sound_crash_sfx_enabled(void)
{
    static int s_mode = -1;
    if (s_mode < 0) {
        s_mode = td5_env_flag_on("TD5RE_CRASH_SFX");   /* default ON */
    }
    return s_mode;
}

/* Poll every active player slot for a NEW crash and fire the heavy-hit impact
 * sound at the crashing car. Called once per frame from the audio mix. */
static void td5_sound_update_crash_sfx(void)
{
    if (!td5_sound_crash_sfx_enabled()) return;
    if (s_race_end_flag) return;               /* no new SFX during fade-out */

    /* Crash sounds are for the local human cars; cap at the racer-slot count. */
    int num_human = g_td5.num_human_players;
    if (num_human < 1) num_human = 1;
    if (num_human > TD5_MAX_RACER_SLOTS) num_human = TD5_MAX_RACER_SLOTS;

    for (int slot = 0; slot < num_human; slot++) {
        int32_t mag = 0;
        int age = 0;
        uint32_t seq = td5_physics_get_crash_fx(slot, &mag, &age);
        if (seq == 0) continue;                 /* no crash yet */
        if (seq <= s_last_crash_seq[slot]) continue;  /* already handled */
        s_last_crash_seq[slot] = seq;           /* consume this crash */

        TD5_Actor *actor = td5_game_get_actor(slot);
        if (!actor) continue;

        /* Volume scales with impact magnitude, saturating at the reference
         * impact. Mapped into the play-at-position 0..0x1000 volume range. */
        if (mag < 0) mag = -mag;
        int vol = (int)(((int64_t)mag * 0x1000) / TD5_SOUND_CRASH_MAG_REF);
        if (vol > 0x1000) vol = 0x1000;
        if (vol < 0x200)  vol = 0x200;          /* audible floor for light hits */

        int32_t pos[3] = { actor->world_pos.x, actor->world_pos.y, actor->world_pos.z };

        /* Spatial one-shot from the heavy-hit bank (random HHit1..HHit4),
         * distance-attenuated + split-screen-aware by play_at_position. */
        td5_sound_play_at_position(TD5_SOUND_CRASH_HHIT_SLOT, vol,
                                   TD5_SOUND_FREQ_22050, pos,
                                   TD5_SOUND_CRASH_HHIT_COUNT);

        TD5_LOG_I(LOG_TAG, "crash SFX slot=%d seq=%u mag=%d age=%d vol=%d",
                  slot, seq, (int)mag, age, vol);
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
 *   H. Crash impact SFX [ITEM #12]
 * ======================================================================== */

void td5_sound_update_audio_mix(void)
{
    /* [ITEM #12] Detect + play per-slot crash impacts (new sequence id). */
    td5_sound_update_crash_sfx();

    /* ----------------------------------------------------------------
     * A. Compute listener velocity deltas (for Doppler)
     * ---------------------------------------------------------------- */
    /* [MP audio 2026-06-13] Velocity delta for EVERY human viewport (was just 2)
     * so the nearest-listener engine attenuation below has valid Doppler vel for
     * players 3+. Arrays are sized [TD5_MAX_VIEWPORTS]. */
    int n_listeners = g_td5.viewport_count;
    if (n_listeners < 1) n_listeners = 1;
    if (n_listeners > TD5_MAX_VIEWPORTS) n_listeners = TD5_MAX_VIEWPORTS;
    for (int vp = 0; vp < n_listeners; vp++) {
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
                  "Audio mix: engine_channels=%d voices=%d listener0=(%d,%d,%d)",
                  active_engine_channels,
                  td5_plat_audio_active_channels(),
                  (int)s_listener_pos[0][0],
                  (int)s_listener_pos[0][1],
                  (int)s_listener_pos[0][2]);
        /* [S11 diagnostic] Once-a-second voice-pool snapshot at INFO level
         * (self-flushing). A climbing active/peak or rising steal/fail count
         * means voice-pool pressure; a flat count while a sound is stuck points
         * elsewhere (a pinned loop, not exhaustion). Survives a force-close. */
        td5_plat_audio_log_stats("race");
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
        if (sound_police_audio_fix_enabled()) {
            /* #15 fix: ramp the fade level toward the target (~0.5s @ 30Hz)
             * instead of snapping, so the siren fades in on activation and
             * fades out smoothly when the chase ends. The slots are stopped
             * below only once the level reaches 0 (target stays 0). */
            if (s_tracked_veh_fade_level < s_tracked_veh_fade_target) {
                s_tracked_veh_fade_level += TD5_SOUND_SIREN_FADE_RAMP_STEP;
                if (s_tracked_veh_fade_level > s_tracked_veh_fade_target)
                    s_tracked_veh_fade_level = s_tracked_veh_fade_target;
            } else if (s_tracked_veh_fade_target < s_tracked_veh_fade_level) {
                s_tracked_veh_fade_level -= TD5_SOUND_SIREN_FADE_RAMP_STEP;
                if (s_tracked_veh_fade_level < s_tracked_veh_fade_target)
                    s_tracked_veh_fade_level = s_tracked_veh_fade_target;
            }
        } else {
            /* Snap fade level to target (instant binary fade) */
            if (s_tracked_veh_fade_level < s_tracked_veh_fade_target) {
                s_tracked_veh_fade_level = TD5_SOUND_SIREN_FADE_FULL;
            } else if (s_tracked_veh_fade_target < s_tracked_veh_fade_level) {
                s_tracked_veh_fade_level = 0;
            }
        }
    } else {
        /* Active but both fade level and target are zero: stop siren.
         * [S11] Stop the slots the play path actually started — 0x14/0x15 (P1)
         * and their P2 duplicates 0x40/0x41. The old code stopped 0x15/0x16 and
         * 0x41/0x42, so the Siren3 loop on 0x14 (and its dup 0x40) was NEVER
         * stopped and kept droning for the rest of the session after the siren
         * toggled off — a textbook "stuck on one sound" case during cop chases.
         * 0x16/0x42 were never played, so stopping them was a no-op. */
        slot_stop(0x14);
        slot_stop(0x15);
        slot_stop(0x40);
        slot_stop(0x41);
        s_tracked_veh_active_p2 = 0;
        s_tracked_veh_active    = 0;
    }

    /* ----------------------------------------------------------------
     * D. Determine viewport pass count and initial state
     * ---------------------------------------------------------------- */
    int num_passes;
    int viewer_vehicle;  /* which vehicle index is the "viewer" for this pass */
    int pan;

    if (g_td5.viewport_count <= 1) {
        num_passes     = 1;
        viewer_vehicle = s_reverb_actor_index;
        pan            = 0;
    } else if (g_td5.viewport_count == 2) {
        num_passes     = 2;  /* classic 2-player: base + duplicate slot range, L/R pan */
        viewer_vehicle = 0; /* first pass: vehicle 0, will increment */
        pan            = -10000;
    } else {
        /* [MP audio 2026-06-13] 3+ split-screen players: ONE engine voice per
         * car (single pass, base slot range only). The voice pool has just a
         * base + duplicate range, so the 2-pass scheme already plays every car
         * TWICE and half-exhausts the pool at 2 players; a 3rd/4th player then
         * steals voices and their cars go silent. With one pass + the
         * nearest-listener attenuation (below), each car is heard at the volume
         * of whichever human is closest, using half the voices — so every
         * player's car is audible. Center pan (per-ear panning is meaningless
         * across 3-4 panes on a single audio output). */
        num_passes     = 1;
        viewer_vehicle = 0;
        pan            = 0;
    }

    /* [MP audio 2026-06-13] 3+ split-screen players share a SINGLE pass (one
     * voice per car). Treat EVERY human car as "local" (full engine + idle loop,
     * like the viewer) so players 2/3/4 aren't quieter than player 1, and route
     * the one shared SkidBit voice to whichever human is drifting HARDEST so the
     * tyre screech isn't player-1-only. (1-/2-player keep the per-pass viewer
     * model unchanged.) */
    int multi_audio = (g_td5.viewport_count > 2);
    int num_human   = g_td5.num_human_players;
    if (num_human < 1) num_human = 1;
    if (num_human > TD5_SOUND_MAX_RACE_VEHICLES) num_human = TD5_SOUND_MAX_RACE_VEHICLES;
    int skid_human = 0;
    if (multi_audio) {
        int best_slip = -1;
        for (int h = 0; h < num_human; h++) {
            TD5_Actor *ha = td5_game_get_actor(h);
            if (!ha) continue;
            int sf = ha->front_axle_slip_excess, sr = ha->rear_axle_slip_excess;
            int sm = (sf > sr) ? sf : sr;
            if (sm > best_slip) { best_slip = sm; skid_human = h; }
        }
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

            /* Which car drives the shared SkidBit/tracked screech this pass:
             * the per-pass viewer normally, or the hardest-drifting human in 3+
             * split-screen. [MP audio 2026-06-13] */
            int is_skid_car = multi_audio ? (veh == skid_human)
                                          : ((int)veh == td5_game_get_player_slot(pass));
            /* Which cars get the full "local" engine treatment (direct volume +
             * idle loop): the per-pass viewer normally, every human in 3+. */
            int is_local_eng = multi_audio ? (veh < num_human)
                                           : ((int)veh == (int)viewer_vehicle);

            /* [MP audio 2026-06-24] Per-car stereo pan for 3+ split-screen.
             * The faithful 1-/2-player path keeps its per-pass `pan` untouched;
             * in 3+ split each HUMAN car is panned to the speaker matching its
             * own on-screen pane (the split actor_slot_map is identity, so the
             * human in viewport `veh` drives actor `veh`). A 2x2 4-player game
             * therefore puts the left-column players' engines on the left and
             * the right-column players' on the right — so player 4 (bottom-right)
             * finally hears their own car instead of it vanishing into a centred
             * 4-engine mush. Non-human cars are panned toward the nearest human's
             * pane in the spatial branch below. */
            int veh_pan = pan;
            if (multi_audio && veh < num_human) {
                veh_pan = td5_game_get_view_pan(veh);
            }

            /* ----------------------------------------------------------
             * Viewer vehicle horn/siren tracked audio (per-vehicle)
             * ---------------------------------------------------------- */
            if (is_skid_car) {
                /* Horn/siren tracked-audio mix for the viewer vehicle. The
                 * "dead vehicle" identity check is applied at the play/modify
                 * decision below (see comment there). */

                /* Get lateral slip for skid/horn volume */
                int slip_front = actor->front_axle_slip_excess;
                int slip_rear  = actor->rear_axle_slip_excess;
                int slip_max   = (slip_front > slip_rear) ? slip_front : slip_rear;

                /* [FAITHFUL @ 0x00440B00 D1]: when the surface-contact/slip flag
                 * scf (+0x376) is set (tyre laying marks / wheelspin), the screech
                 * input is forced to max (0x7fff) BEFORE the /3 scale -- so the
                 * screech is loudest exactly when the tyre marks appear. Order
                 * matters: scf-override first, then the tumble-zero below. */
                if (*((const uint8_t *)actor + 0x376) != 0) {
                    slip_max = 0x7fff;
                }
                /* Tumbling (all wheels airborne, +0x37C == 0x0F) silences it. */
                if (actor->wheel_contact_bitmask == 0x0F) {
                    slip_max = 0;
                }

                /* [FIX 2026-06-02 pre-race screech] Silence the tyre screech before
                 * the race starts. g_td5.paused == 1 during the start countdown
                 * (set in td5_game.c, cleared at GO) and during the in-race pause.
                 * Revving a held car on the grid spins the wheels (scf set), which
                 * forced slip_max to max above and screeched before the flag drop;
                 * the user does not want a pre-race screech. Trivially revertible
                 * if the original's pre-race burnout SFX is later wanted. */
                if (g_td5.paused) slip_max = 0;

                int horn_vol_raw = slip_max / 3;

                /* Clamp tracked audio volume to [0, 0x1000] */
                int tracked_vol;
                if (horn_vol_raw < 0)      tracked_vol = 0;
                else if (horn_vol_raw > 0x1000) tracked_vol = 0x1000;
                else                       tracked_vol = horn_vol_raw;

                /* Play SkidBit at its native 22050Hz so the screech sounds like
                 * a sharp tyre screech. The decomp's slip-derived 0x800-0x1000
                 * (2048-4096Hz) frequency plays the 22050Hz sample at ~0.1x speed
                 * -- a deep groan, not a screech. Volume still tracks slip
                 * (tracked_vol above): loud on a slide, silent when gripping. */
                int tracked_pitch = TD5_SOUND_FREQ_22050;

                /* Spatial audio for tracked sound (non-viewer only in original,
                 * but for the viewer vehicle this path computes direct) */
                float dx = (float)s_active_listener_pos[0] - (float)actor->world_pos.x;
                float dz = (float)s_active_listener_pos[2] - (float)actor->world_pos.z;
                dx *= TD5_SOUND_DISTANCE_SCALE;
                dz *= TD5_SOUND_DISTANCE_SCALE;
                float dist = sqrtf(dx * dx + dz * dz);
                /* [MP audio 2026-06-13] In 3+ split-screen the skid car is the
                 * hardest-drifting HUMAN, but s_active_listener_pos is player 1's
                 * camera — so the screech was attenuated by distance to player 1
                 * and went silent for everyone but player 1. It's that human's
                 * OWN drift in their OWN pane, so play it at full volume. */
                if (multi_audio) { dx = 0.0f; dz = 0.0f; dist = 0.0f; }

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

                /* Dead-vehicle identity check [CONFIRMED @ 0x00440cf3..0x00440d0e
                 * in UpdateVehicleAudioMix 0x00440B00]: an empty/dead actor slot
                 * has a contiguous 4-byte identity tag at +0x371..+0x374 set to
                 * all 0xFF (-1) by UpdateRaceActors 0x00436a70 /
                 * InitializeRaceVehicleRuntime 0x0042f140. When dead, the original
                 * stops the tracked siren channel (DXSound::Stop on the +0x14 slot)
                 * if it is playing and skips the play/modify. Active slots run the
                 * normal play-or-modify path. */
                const uint8_t *id_tag = (const uint8_t *)actor + 0x371;
                int veh_dead = (id_tag[0] == 0xFF && id_tag[1] == 0xFF &&
                                id_tag[2] == 0xFF && id_tag[3] == 0xFF);
                /* Original plays slot 0x13 then Modify/Stops 0x14 — in its M2DX
                 * slot system 0x14 is the *active handle* of the 0x13 sound. The
                 * port's flat slot map has the played SkidBit on 0x13, so we
                 * Modify/Stop 0x13 (the slot actually playing) to keep the screech
                 * volume tracking slip; targeting 0x14 here is a silent no-op. */
                if (veh_dead) {
                    if (s_tracked_audio_state[state_idx] != ENGINE_STATE_STOPPED) {
                        slot_stop(slot_offset + 0x13);
                        s_tracked_audio_state[state_idx] = ENGINE_STATE_STOPPED;
                    }
                } else if (s_tracked_audio_state[state_idx] == ENGINE_STATE_STOPPED) {
                    slot_play(slot_offset + 0x13, 1, vol_atten, veh_pan, final_pitch);
                    s_tracked_audio_state[state_idx] = 1;
                } else {
                    slot_modify(slot_offset + 0x13, vol_atten, veh_pan, final_pitch);
                }
            }

            /* ----------------------------------------------------------
             * Engine sound computation
             * ---------------------------------------------------------- */
            int raw_speed = actor->engine_speed_accum;
            int engine_target_state;
            int engine_vol;
            int engine_pitch;

            /* Stationary idle branch. [CONFIRMED @ 0x00440fe4:
             *   MOV ECX,[ESI*4 + 0x4c382c]; TEST ECX,ECX; JNZ <moving>]
             * The original takes the fixed-pitch idle (REV state, 22050Hz) only
             * when the per-actor reverb-mode flag == 0, i.e. for NON-reverb cars.
             * The player (slot 0) is loaded with the reverb flag SET
             * (td5_game.c: is_reverb = (i == 0)), so the player takes the moving
             * DRIVE branch even when stationary (low engine rumble + small rand
             * jitter), while AI cars get the fixed idle. The port had this
             * inverted -- testing s_reverb_flag[veh] (true) instead of
             * !s_reverb_flag[veh] -- so the player's stationary "rev sound" was
             * a wrong fixed 22050Hz whine instead of the idle rumble. */
            /* [FAITHFUL @ 0x00440B00]: idle is gated purely on RPM<1000 (+0x310)
             * and reverbMode==0. No road-speed gate in the original. */
            if (raw_speed < 1000 && !s_reverb_flag[veh]) {
                /* Non-reverb (AI) car idle: fixed low-frequency rev loop */
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
                    /* First time: local cars (viewer / every human in 3+) also
                     * start the idle loop so they sound as full as player 1. */
                    if (is_local_eng &&
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
             * Local car (viewer / every human in 3+): direct volume/pitch
             * (no distance attenuation) so all human cars are full volume.
             * ---------------------------------------------------------- */
            if (is_local_eng && !td5_game_is_replay_active()) {
                int steer_pan = veh_pan;
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

                /* The viewer's tyre screech (SkidBit, slot 0x13/0x14) is produced
                 * faithfully by the D1 slip-modulated block above: its volume and
                 * frequency track max(+0x31C,+0x320) slip-excess (forced to max on
                 * scf, zeroed when tumbling), distance-attenuated. The original
                 * 0x440B00 has NO separate intensity-gated skid loop here -- the
                 * port's old start/stop-on-intensity block was a divergence and is
                 * removed. */
            } else {
                /* ----------------------------------------------------------
                 * Non-viewer vehicle: spatial audio with distance + Doppler
                 * ---------------------------------------------------------- */
                int spatial_pan = pan;
                if (g_td5.split_screen_mode == 0) {
                    spatial_pan = 0;
                }

                /* [MP audio 2026-06-13] Attenuate by distance to the NEAREST
                 * HUMAN PLAYER'S CAR (slots 0..num_human_players-1), not the
                 * current pass's listener. Rationale: the mixer runs only 1-2
                 * listener passes (the slot pool has just a base + duplicate
                 * range), AND the camera only fills g_camWorldPos for viewports
                 * 0-1 — so listeners 2+ were stuck at the origin and players 3/4's
                 * cars attenuated to silence in their own panes. Using the human
                 * cars themselves as the listeners is robust (no dependence on
                 * the camera / viewport-slot maps): a car near ANY human plays
                 * loud, so every player hears their own car + nearby rivals.
                 * (1-player: the single human == the old listener; 2-player: the
                 * two human cars ≈ the two camera listeners — both unchanged.) */
                const int32_t *near_vel = s_active_listener_vel;
                float dx, dz, dist, dist2;
                int near_human = sound_nearest_human_listener(
                    actor, g_td5.num_human_players, &dx, &dz, &dist2, &near_vel);
                if (near_human < 0) {  /* no valid human actor — fall back to pass listener */
                    dx = ((float)s_active_listener_pos[0] - (float)actor->world_pos.x) * TD5_SOUND_DISTANCE_SCALE;
                    dz = ((float)s_active_listener_pos[2] - (float)actor->world_pos.z) * TD5_SOUND_DISTANCE_SCALE;
                    dist2 = dx * dx + dz * dz;
                }
                dist = sqrtf(dist2);
                if (td5_game_is_replay_active()) dist *= TD5_SOUND_REPLAY_DIST_SCALE;

                /* [MP audio 2026-06-24] 3+ split: pan the opponent/AI engine
                 * toward the pane of whichever human it is nearest to, so a rival
                 * beside player 4 sounds from player 4's side of the speakers —
                 * matching that player's own engine pan. */
                if (multi_audio && near_human >= 0)
                    spatial_pan = td5_game_get_view_pan(near_human);

                int vol_atten = sound_attenuate_volume(engine_vol, dist);

                int final_pitch = engine_pitch;
                if (vol_atten > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        near_vel,
                        &actor->linear_velocity_x,
                        dx, dz, dist);
                    final_pitch = sound_apply_doppler_pitch(engine_pitch, doppler);
                }

                /* The original Modifies base+state, but in M2DX that resolves to
                 * the instance started by Play(base+state-1) -- the same play/
                 * active off-by-one as the D1 screech. In the port's flat slot map
                 * the engine loop lives on the PLAYED slot base+(state-1):
                 * DRIVE(1)->base+0=Drive.wav, REV(2)->base+1=Rev.wav. Modify that
                 * slot so a DRIVING opponent plays Drive.wav, not Rev.wav. Silence
                 * the other engine sample so a leaked prior-state loop (the stop is
                 * off-by-one too) doesn't drone under it. */
                int eng_slot   = (engine_target_state - 1) + veh * 3 + slot_offset;
                int other_slot = ((engine_target_state - 1) ^ 1) + veh * 3 + slot_offset;
                slot_modify(eng_slot, vol_atten, spatial_pan, final_pitch);
                slot_modify(other_slot, 0, spatial_pan, final_pitch);
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
                                                horn_vol, veh_pan, TD5_SOUND_FREQ_22050);
                        }
                    } else {
                        slot_modify(horn_slot + 1, horn_vol,
                                              veh_pan, TD5_SOUND_FREQ_22050);
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

                /* #15 fix: the fade level is in the 0..0x1000 domain; pre-scale
                 * it by >> 5 into the 0..0x7F domain (clamped) before the
                 * distance attenuator, exactly like the engine (>> 5, ~1248)
                 * and traffic (>> 5, ~1505) loops do. Without this the raw
                 * 0x1000 level overwhelms sound_attenuate_volume() and the siren
                 * stays pinned at full volume regardless of distance. */
                int siren_level = s_tracked_veh_fade_level;
                if (sound_police_audio_fix_enabled()) {
                    siren_level >>= 5;
                    if (siren_level > 0x7F) siren_level = 0x7F;
                }
                int siren_vol = sound_attenuate_volume(siren_level, dist);
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
            /* [S11] Cap at the dedicated traffic engine slots (37..43). Beyond
             * this the play/modify slots would spill into the duplicate-buffer
             * range (44+) and the per-pass state index would overflow
             * s_traffic_engine_state[]. With N-way >6 players the actors >= 6 are
             * extra racers treated as traffic, so this cap is what previously
             * overflowed; bound it and note any dropped engines once per race. */
            int traffic_limit = total_actors;
            if (traffic_limit > 6 + TD5_SOUND_TRAFFIC_AUDIO_MAX)
                traffic_limit = 6 + TD5_SOUND_TRAFFIC_AUDIO_MAX;
            if (total_actors > traffic_limit && !s_traffic_audio_cap_logged) {
                TD5_LOG_W(LOG_TAG,
                          "traffic engine audio capped at %d voices (%d actors > 6); "
                          "%d extra actors get no dedicated engine loop",
                          TD5_SOUND_TRAFFIC_AUDIO_MAX, total_actors,
                          total_actors - traffic_limit);
                s_traffic_audio_cap_logged = 1;
            }
            int traffic_slot = slot_offset + 0x26; /* DXSound slot for first traffic */
            for (int t = 6; t < traffic_limit; t++) {
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
                    /* 0x441845: slot = abs_actor_index + slot_offset + 0x1F
                     * abs_actor_index for traffic[0] = 6, giving slot 37 = TD5_SOUND_TRAFFIC_SLOT_BASE */
                    int play_slot = t + 0x1F + slot_offset;
                    TD5_LOG_I(LOG_TAG, "traffic engine start: actor=%d play_slot=%d (pass=%d)", t, play_slot, pass);
                    slot_play(play_slot, 1, 0, 0, 1000);
                    s_traffic_engine_state[t_state_idx] = 1;
                }

                /* Spatial audio: distance attenuation + Doppler. In 3+ split,
                 * attenuate/pan toward the NEAREST human (same treatment as race
                 * opponents, via sound_nearest_human_listener) so traffic beside
                 * player 4 is audible and panned to their pane instead of being
                 * silenced/centred by the single player-1 listener pass.
                 * 1-/2-player keep the faithful pass-listener path. [MP audio
                 * 2026-06-24] */
                int traffic_pan = pan;
                const int32_t *t_listener_vel = s_active_listener_vel;
                float dx, dz, dist, dist2;
                int t_near = multi_audio
                    ? sound_nearest_human_listener(traffic, num_human,
                                                   &dx, &dz, &dist2, &t_listener_vel)
                    : -1;
                if (t_near < 0) {
                    dx = ((float)s_active_listener_pos[0] - (float)traffic->world_pos.x)
                         * TD5_SOUND_DISTANCE_SCALE;
                    dz = ((float)s_active_listener_pos[2] - (float)traffic->world_pos.z)
                         * TD5_SOUND_DISTANCE_SCALE;
                    dist2 = dx * dx + dz * dz;
                } else {
                    traffic_pan = td5_game_get_view_pan(t_near);
                }
                dist = sqrtf(dist2);

                int t_vol_atten = sound_attenuate_volume(t_vol, dist);
                /* [dynamic-traffic] engine loop fades with the car and is
                 * silent while the slot is parked (255 = identity). */
                {
                    int t_fade = td5_ai_traffic_get_draw_alpha(t);
                    if (t_fade < 255)
                        t_vol_atten = (t_vol_atten * t_fade) / 255;
                }
                int t_final_pitch = t_pitch;
                if (t_vol_atten > 0) {
                    float doppler = sound_compute_doppler_ratio(
                        t_listener_vel,
                        &traffic->linear_velocity_x,
                        dx, dz, dist);
                    t_final_pitch = sound_apply_doppler_pitch(t_pitch, doppler);
                }

                slot_modify(traffic_slot, t_vol_atten, traffic_pan, t_final_pitch);
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
 *
 * [CONFIRMED @ 0x00441D90] Byte-faithful with orig PlayVehicleSoundAtPosition.
 * SAR-RZ-class LSB fixes shipped 2026-05-18:
 *   1) Volume-clamp boundary: now `volume == 0x1000` exact-equality to match
 *      orig (values above 0x1000 pass through unchanged).
 *   2) FPU rounding: replaced `roundf` (C99 half-away-from-zero) with
 *      `lrintf` which under the x87 default control word (round-to-nearest-
 *      even) matches orig's FISTP ROUND macro at half-integer inputs.
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

    /* Clamp volume — orig 0x00441D90 tests exact equality with 0x1000
     * (not >=); values above 0x1000 pass through unchanged. */
    if (volume == 0x1000) volume = 0xFFF;
    else if (volume < 0)  volume = 0;

    /* Determine number of viewport passes */
    int num_passes;
    int start_pan;
    if (g_td5.viewport_count <= 1) {
        num_passes = 1;
        start_pan  = 0;
    } else {
        num_passes = 2;  /* [PORT] audio capped at 2 listener passes */
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

        /* Volume attenuation — orig uses FPU ROUND (FISTP, round-half-to-even
         * under default x87 control word). C99 `roundf` is half-away-from-zero
         * and diverges by 1 LSB at half-integer inputs; `lrintf` honors the
         * current rounding mode, which on x86 defaults to RNE. */
        int vol_atten = ((0x7F - ((int)lrintf(dist) >> 7)) * (volume >> 5)) / 0x7F;
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

            /* Orig at 0x00441D90 uses FPU ROUND (FISTP RNE). Use lrintf so
             * x87 default rounding (round-to-nearest-even) is preserved. */
            final_pitch = (int)lrintf(
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
 *
 * [ARCH-DIVERGENCE: DXSound -> td5_plat_audio + loose-file fallback; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x00414640: orig walks two parallel arrays (10 WAV
 *   paths, slots 1..10), opens each via OpenArchiveFileForRead("Front End\
 *   Sounds\Sounds.zip") and feeds DXSound::LoadBuffer(buf, slot, 0). Port
 *   mirrors the same 10-entry path/slot tables (s_frontend_sfx_paths /
 *   s_frontend_sfx_slots, byte-faithful order including the repeated
 *   Whoosh.wav/Crash1.wav entries) and the one-shot flag, but routes
 *   through td5_asset_open_and_read (which tries loose-file at
 *   re/assets/sounds/<name> before the ZIP) and td5_plat_audio_load_wav
 *   in place of the DDraw-era DXSound interface. Same slot mapping,
 *   same loop=0 flag. */
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
 * Music (CD-audio compatible) -- routed through the pluggable music seam.
 *
 * These keep their historic td5_sound_cd_* / td5_sound_set_music_volume names
 * and signatures so every existing caller (race start/stop in td5_game.c, the
 * frontend jukebox + Options screen) is unchanged, but they now forward to
 * td5_music_*, which dispatches to the active backend. With no third-party
 * backend registered the default backend forwards 1:1 to td5_plat_cd_*, so
 * behavior is identical to before this seam existed.
 * ======================================================================== */

void td5_sound_cd_play(int track) {
    td5_music_play(track);
}

void td5_sound_cd_stop(void) {
    td5_music_stop();
}
void td5_sound_cd_set_volume(int v) { td5_music_set_volume(v); }

/* ========================================================================
 * Master Volume
 * ======================================================================== */

void td5_sound_set_sfx_volume(int v) { td5_plat_audio_set_master_volume(v); }
void td5_sound_set_music_volume(int v) { td5_music_set_volume(v); }
void td5_sound_set_radio_volume(int v) { td5_radio_set_volume_pct(v); }

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

static int sound_nearest_human_listener(const TD5_Actor *src, int num_human,
                                        float *out_dx, float *out_dz,
                                        float *out_dist2, const int32_t **out_vel)
{
    int nh = num_human;
    if (nh < 1) nh = 1;
    if (nh > TD5_SOUND_MAX_RACE_VEHICLES) nh = TD5_SOUND_MAX_RACE_VEHICLES;

    float best = -1.0f, bdx = 0.0f, bdz = 0.0f;
    int near_h = -1;
    for (int hl = 0; hl < nh; hl++) {
        TD5_Actor *ha = td5_game_get_actor(hl);
        if (!ha) continue;
        float lx = ((float)ha->world_pos.x - (float)src->world_pos.x)
                   * TD5_SOUND_DISTANCE_SCALE;
        float lz = ((float)ha->world_pos.z - (float)src->world_pos.z)
                   * TD5_SOUND_DISTANCE_SCALE;
        float d2 = lx * lx + lz * lz;
        if (best < 0.0f || d2 < best) {
            best = d2; bdx = lx; bdz = lz; near_h = hl;
            if (out_vel) *out_vel = &ha->linear_velocity_x;
        }
    }
    *out_dx = bdx; *out_dz = bdz; *out_dist2 = best;
    return near_h;
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
        /* [DA-M3 audit 2026-05-22 — pending application]
         * This stores the SAME buffer_id at slot+44 (the "duplicate" slot)
         * which is the root cause of engine/skid SFX choppiness:
         *
         *   - Orig's nDup=2 sounds get a TRUE second DirectSound buffer
         *     (via DuplicateSoundBuffer at load time, per W1-D analysis of
         *     DXSound::Create @ 0x1000ce30 + g_soundDuplicateCountTable).
         *   - Orig's DXSound::Play @ 0x1000d380 walks the linked-list chain
         *     looking for an idle voice ("tail-steal" semantics) — never
         *     Stop+SetCurrentPosition(0) on a busy head.
         *   - Port currently stores the same buffer_id at both slots, so
         *     slot_play(slot+44) is functionally identical to slot_play(slot).
         *     When the gear FSM (td5_sound.c:893-913) flips DRIVE↔REV at
         *     high cycle rate, slot_stop releases the running buffer → click.
         *
         * Fix outline (DA-M3 Section E.1):
         *   1. Allocate a real second buffer_id when duplicates >= 2 (new
         *      td5_plat_audio_duplicate(buffer_id) wrapping
         *      IDirectSound8_DuplicateSoundBuffer at load-time).
         *   2. Add per-slot "next voice idx" rotating counter (2 entries).
         *   3. In slot_play(): if head is_playing AND dup is idle, route to
         *      dup channel; if both busy, replace older voice (tail-steal).
         *
         * Affected: Drive/Rev/Horn (veh*3+{0,1,2} slots 0..17), SkidBit
         * (slot 19), Siren3/5 (slot 20/21), traffic engine variants (37..43).
         * Scope: ~30 lines td5_sound.c + ~20 lines td5_platform_win32.c.
         * Not applied yet — needs A/B audio testing per affected SFX class. */
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

/* Mute (1) / unmute (0) all race SFX. Wired to the in-race pause menu so the
 * car/engine/skid loops go silent while paused (issue: "car sound keeps playing
 * on the pause menu"). The pause SOUND-volume row lifts the mute so its slider
 * previews the SFX volume. Music (CD) is on a separate path and keeps playing. */
void td5_sound_set_sfx_muted(int muted)
{
    td5_plat_audio_set_muted(muted);
}

/* Suspend (1) / resume (0) ALL race audio when the pause menu opens/closes.
 *
 * Companion to the window-focus mute path (td5_plat_audio_update_focus_mute in
 * td5_platform_win32.c, driven once per frame from main.c): focus-loss already
 * silences everything when the player alt-tabs away; this does the same the
 * moment the in-race pause menu comes up. Whereas td5_sound_set_sfx_muted only
 * gates the per-voice SFX (engine/skid/horn/crash/ambient one-shots), pausing
 * must ALSO take the music down. We cannot read the pause-menu-active state from
 * here -- it lives in a private static in td5_game.c (s_pause_menu_active) and
 * g_td5.paused does NOT track the menu -- so the pause code calls this setter
 * explicitly (see the porting note: pause open -> td5_sound_set_paused(1),
 * resume -> td5_sound_set_paused(0)).
 *
 * Suspend strategy (idempotent, edge-triggered, non-destructive so a resume is
 * always clean):
 *   - SFX  : td5_plat_audio_set_muted -- same lever the SOUND-row preview and
 *            the focus mute use, so they coexist (the per-frame per-row preview
 *            still wins while the menu is up).
 *   - Music: drop the CD/MCI device volume to 0 and restore the configured
 *            level on resume. Volume (not stop) keeps this independent of the
 *            pause code's own CD stop/replay -- no track number needed here, and
 *            either ordering ends at the saved volume.
 *
 * Gated by TD5RE_PAUSE_MUTE (default ON; "0" restores the old behavior where
 * pausing left the music playing and relied solely on the SFX mute). */
void td5_sound_set_paused(int paused)
{
    static int s_pause_mute_enabled = -1;   /* -1 = env not yet read */
    /* s_sound_paused is now file-scope (reset per race in init) — see [#8]. */

    if (s_pause_mute_enabled < 0) {
        const char *env = getenv("TD5RE_PAUSE_MUTE");
        s_pause_mute_enabled = (env && env[0] == '0') ? 0 : 1;
        TD5_LOG_I(LOG_TAG, "pause-mute %s (TD5RE_PAUSE_MUTE=%s)",
                  s_pause_mute_enabled ? "ENABLED" : "disabled",
                  env ? env : "default");
    }
    if (!s_pause_mute_enabled)
        return;                              /* old behavior: leave audio alone */

    paused = paused ? 1 : 0;
    if (paused == s_sound_paused)
        return;                              /* no change -> nothing to do */
    s_sound_paused = paused;

    if (paused) {
        td5_plat_audio_set_muted(1);         /* all per-voice SFX -> silent */
        td5_music_set_paused(1);             /* music -> ducked (backend pauses or vol 0) */
        TD5_LOG_I(LOG_TAG, "pause: all audio SUSPENDED (SFX muted, music ducked)");
    } else {
        td5_plat_audio_set_muted(0);         /* restore SFX */
        td5_music_set_paused(0);             /* music -> resumed (restores last volume) */
        TD5_LOG_I(LOG_TAG, "pause: audio RESUMED (SFX unmuted, music restored vol=%d)",
                  g_td5.ini.music_volume);
    }
}
