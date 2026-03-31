/**
 * td5_sound.h -- Sound playback, vehicle audio, ambient, CD
 *
 * Two-tier architecture:
 *   DXSound (M2DX.dll): 44 base + 44 duplicate buffer slots, WAV streaming, CD
 *   TD5 Sound Manager: per-vehicle banks, spatial audio, Doppler, RPM-to-pitch
 *
 * Original functions (EXE-side):
 *   0x440A30  UpdateVehicleLoopingAudioState
 *   0x440AB0  StartTrackedVehicleAudio
 *   0x440AE0  StopTrackedVehicleAudio
 *   0x440B00  UpdateVehicleAudioMix
 *   0x441A80  LoadVehicleSoundBank
 *   0x441C60  LoadRaceAmbientSoundBuffers
 *   0x441D50  ReleaseRaceSoundChannels
 *   0x441D90  PlayVehicleSoundAtPosition
 *   0x414640  LoadFrontendSoundEffects
 *
 * DXSound API (M2DX.dll):
 *   DXSound::Environment, Create, Destroy
 *   DXSound::Load, Remove, Play, Stop, Modify, ModifyAll
 *   DXSound::CDPlay, CDStop, CDSetVolume
 *   DXSound::StreamPlay, StreamStop
 */

#ifndef TD5_SOUND_H
#define TD5_SOUND_H

#include "td5_types.h"

/* ========================================================================
 * Constants
 * ======================================================================== */

#define TD5_SOUND_MAX_BASE_SLOTS        44
#define TD5_SOUND_MAX_DUP_SLOTS         44
#define TD5_SOUND_TOTAL_SLOTS           88
#define TD5_SOUND_DUP_OFFSET            44  /* duplicate range starts here */

#define TD5_SOUND_MAX_RACE_VEHICLES     6
#define TD5_SOUND_CHANNELS_PER_VEHICLE  3   /* Drive, Rev/Reverb, Horn */
#define TD5_SOUND_VEHICLE_SLOT_END      18  /* 6 * 3 */

#define TD5_SOUND_AMBIENT_SLOT_BASE     18  /* 0x12 */
#define TD5_SOUND_AMBIENT_COUNT         19  /* slots 18-36 */
#define TD5_SOUND_TRAFFIC_SLOT_BASE     37  /* 0x25 */

#define TD5_SOUND_FRONTEND_SFX_COUNT    10

#define TD5_SOUND_FREQ_22050            0x5622  /* 22050 Hz native sample rate */
#define TD5_SOUND_SIREN_FADE_FULL       0x1000
#define TD5_SOUND_ENGINE_STOPPED        99

/* Horn volume table gear indices */
#define TD5_SOUND_HORN_VOL_DEFAULT      10
#define TD5_SOUND_HORN_VOL_GEAR1        90
#define TD5_SOUND_HORN_VOL_GEAR2        80
#define TD5_SOUND_HORN_VOL_GEAR3        60
#define TD5_SOUND_HORN_VOL_GEAR4        40
#define TD5_SOUND_HORN_VOL_GEAR5        20

/* Audio distance scale factor (world-to-audio-units, ~0.001817) */
#define TD5_SOUND_DISTANCE_SCALE        0.001817f

/* Doppler constants (from float globals at 0x45d5f4..0x45d798) */
#define TD5_SOUND_DOPPLER_EPSILON       1.0f        /* 0x45d5f4 */
#define TD5_SOUND_DOPPLER_VEL_SCALE     4.0f        /* 0x45d650 */
#define TD5_SOUND_DOPPLER_VEL_OFFSET    4096.0f     /* 0x45d604 */
#define TD5_SOUND_DOPPLER_ZERO          0.0f        /* 0x45d624 */
#define TD5_SOUND_DOPPLER_MAX           2.0f        /* 0x45d6d8 */
#define TD5_SOUND_DOPPLER_PITCH_SCALE   0.074074f   /* 0x45d798 */
#define TD5_SOUND_DOPPLER_PITCH_BASE    1.0f        /* 0x45d794 */
#define TD5_SOUND_REPLAY_DIST_SCALE     0.5f        /* 0x45d5d0 */

/* Siren base frequency */
#define TD5_SOUND_SIREN_FREQ            0x5622

/* ========================================================================
 * Module API
 * ======================================================================== */

int  td5_sound_init(void);
void td5_sound_shutdown(void);
void td5_sound_tick(void);

/* --- Race sound lifecycle --- */
int  td5_sound_init_race_resources(void);
void td5_sound_release_race_channels(void);

/* --- Vehicle audio --- */
void td5_sound_update_vehicle_looping_state(int actor_index);
void td5_sound_start_tracked_vehicle_audio(int actor_index);
void td5_sound_stop_tracked_vehicle_audio(void);
void td5_sound_update_audio_mix(void);
int  td5_sound_load_vehicle_bank(const char *car_dir, int vehicle_index, int is_reverb_vehicle);

/* --- Ambient --- */
int  td5_sound_load_ambient(void);
void td5_sound_update_ambient(void);

/* --- Positional one-shots --- */
void td5_sound_play_at_position(int base_slot, int volume, int pitch,
                                const int32_t *pos, int num_variants);

/* --- Frontend SFX --- */
int  td5_sound_load_frontend_sfx(void);
void td5_sound_play_frontend_sfx(int sfx_id);

/* --- CD audio --- */
void td5_sound_cd_play(int track);
void td5_sound_cd_stop(void);
void td5_sound_cd_set_volume(int volume);

/* --- Master volume --- */
void td5_sound_set_sfx_volume(int volume);
void td5_sound_set_music_volume(int volume);

/* --- State accessors (fed by camera / physics / game modules) --- */
void td5_sound_set_listener_pos(int viewport, int32_t x, int32_t y, int32_t z);
void td5_sound_set_skid_intensity(int viewport, int intensity);
void td5_sound_set_gear_state(int vehicle, int gear);
void td5_sound_set_race_end(int ended);

#endif /* TD5_SOUND_H */
