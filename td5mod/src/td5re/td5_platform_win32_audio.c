/**
 * td5_platform_win32_audio.c -- Audio-device platform primitives (DirectSound8,
 * CD Audio via MCI, Radio PCM streaming) (C9 module split, third slice, see
 * REFACTOR_PLAN.md).
 *
 * Extracted verbatim from td5_platform_win32.c's "Audio -- DirectSound8",
 * "CD Audio (via MCI)", and "Radio PCM streaming sink" sections, plus the
 * module-level audio statics they exclusively use. 13 of those statics are
 * shared back to td5_platform_win32.c's td5_platform_win32_init() (which
 * resets them alongside its other module state) via td5_platform_internal.h.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "td5_platform.h"
#include "td5_platform_internal.h"
#include "td5_config.h"

#define LOG_TAG "platform"
#include "td5_color.h"

#define MAX_AUDIO_BUFFERS  256
#define MAX_AUDIO_CHANNELS 64

static LPDIRECTSOUND8          s_dsound      = NULL;
static LPDIRECTSOUNDBUFFER     s_ds_primary  = NULL;
/* Continuously-looping silent buffer that keeps the audio device/stream awake.
 * Without it the device idles when no sound is playing (as in the menus, where
 * SFX are sparse); the next sound then loses its first ~0.1-0.2s to wake-up
 * ramp, which entirely swallows short one-shot UI sounds (ping/whoosh). Kept
 * playing for the whole session so menu one-shots fire instantly. */
static LPDIRECTSOUNDBUFFER     s_ds_keepalive = NULL;
LPDIRECTSOUNDBUFFER     s_ds_buffers[MAX_AUDIO_BUFFERS];
LPDIRECTSOUNDBUFFER     s_ds_channels[MAX_AUDIO_CHANNELS];
static int                     s_ds_channel_buf[MAX_AUDIO_CHANNELS]; /* buffer index per channel */

/* Channel-handle generation/recycle bookkeeping (S11 sound-overload fix).
 *
 * A channel handle returned by td5_plat_audio_play is no longer the bare index
 * `i`. It is `(generation << AUDIO_CHANNEL_INDEX_BITS) | i`, where the generation
 * is bumped every time index `i` is (re)allocated. The TD5 sound module keeps a
 * slot->channel table (s_slot_to_channel) that is freely overwritten and can go
 * stale when a channel is recycled or stolen for a different sound. Without a
 * generation tag, a stale handle would resolve to whatever sound now occupies
 * that index — so a Stop()/Modify() meant for one slot would hit another's
 * voice (cross-talk), and in the worst case the whole mix could lock onto a
 * single stuck source. The generation lets td5_plat_audio_{stop,modify,is_playing}
 * detect a stale handle (gen mismatch) and treat it as a no-op. */
#define AUDIO_CHANNEL_INDEX_BITS 8
#define AUDIO_CHANNEL_INDEX_MASK 0xFF
#define AUDIO_CHANNEL_GEN_MAX    0x7FFFFF /* keep encoded handle within int >= 0 */
uint32_t                s_ds_channel_gen[MAX_AUDIO_CHANNELS];    /* gen per index */
uint32_t                s_ds_channel_serial[MAX_AUDIO_CHANNELS]; /* alloc order (steal pick) */
int                     s_ds_channel_loop[MAX_AUDIO_CHANNELS];   /* 1 = looping (steal last) */
uint32_t                s_audio_alloc_serial = 0;                /* monotonic alloc counter */

/* Per-voice volume table, reproduced byte-faithfully from M2DX
 * DXSound::Environment (0x1000cda0):
 *     table[v] = -(int)( (500/log10(2)) * log10(128/(v+1)) ),  v in 0..127
 * It maps the mixer's 0..127 volume to DirectSound centibels (hundredths of dB):
 * 0 dB at v=127, sloping down a log curve to a -35 dB FLOOR at v=0 — NOT silence.
 * DXSound::Modify sets each voice to table[vol] (no per-sound master scaling), and
 * the master SFX volume is a SEPARATE attenuation (DXSound::SetVolume applies
 * table[master>>9] to the primary buffer). The port previously used a different
 * 20*log10(vol/100) curve AND folded the master in per-sound (vol*master/100),
 * which (a) muted v=0 entirely so the original's faint engine/ambient layers
 * vanished (the player's own Drive loop runs at v=0 under the audible Reverb — a
 * real second engine layer in the original, dead silent in the port) and (b)
 * shifted the whole balance. We now match the table per-voice and apply the
 * master as an equivalent dB offset (adding dB == scaling the summed mix, which
 * is what the original's primary-buffer attenuation does). */
static int                     s_vol_table[128];
static int                     s_master_offset_cb = 0;   /* master attenuation, centibels (<=0) */
static void audio_build_vol_table(void);                 /* defined below, used in audio init */

/* Voice instrumentation (S11): confirm no monotonic leak and that counts return
 * to baseline between races. Active = currently-allocated DirectSound voices
 * (DuplicateSoundBuffer instances), excluding the primary/keepalive buffers. */
uint32_t                s_audio_alloc_count  = 0; /* total successful Play allocations */
uint32_t                s_audio_free_count   = 0; /* total channel releases */
uint32_t                s_audio_steal_count  = 0; /* Play allocations that stole a busy voice */
uint32_t                s_audio_fail_count   = 0; /* Play attempts that could not allocate */
int                     s_audio_active_count = 0; /* live voices right now */
int                     s_audio_peak_active  = 0; /* high-water mark */

DWORD                   s_ds_buffer_rates[MAX_AUDIO_BUFFERS];
static int                     s_audio_buf_count = 0;
static int                     s_master_volume   = 40;
/* When set, all SFX play/modify is forced to silence (e.g. while the in-race
 * pause menu is up). Looping buffers keep running so resume is click-free; the
 * per-frame audio mix simply re-applies volume 0 until unmuted. Music (CD) uses
 * a separate path and is unaffected. */
static int                     s_audio_muted     = 0;
/* When set, all audio is silenced because the game window is NOT the foreground
 * window (alt-tabbed away). Independent of s_audio_muted (the pause-menu mute):
 * they OR together in audio_effective_cb so neither clobbers the other. Driven
 * once per frame by td5_plat_audio_update_focus_mute(). */
static int                     s_audio_focus_muted = 0;

#define TD5_STREAM_BUFFER_BYTES 0x81600u

typedef struct TD5_StreamAudioState {
    FILE                *fp;
    LPDIRECTSOUNDBUFFER  buffer;
    WAVEFORMATEX         wfx;
    DWORD                data_offset;
    DWORD                data_size;
    DWORD                data_cursor;
    DWORD                buffer_bytes;
    DWORD                half_bytes;
    DWORD                last_play_cursor;
    int                  loop;
    int                  eof;
    int                  active;
} TD5_StreamAudioState;

static TD5_StreamAudioState s_audio_stream;

/* ========================================================================
 * Audio -- DirectSound8
 * ======================================================================== */

typedef struct TD5_WavFileInfo {
    WAVEFORMATEX wfx;
    DWORD        data_offset;
    DWORD        data_size;
} TD5_WavFileInfo;

static int td5_stream_parse_wav(FILE *fp, TD5_WavFileInfo *info)
{
    uint8_t riff[12];
    uint8_t chunk_header[8];
    uint8_t fmt_buf[40];
    int have_fmt = 0;
    int have_data = 0;

    if (!fp || !info) {
        return 0;
    }

    if (fseek(fp, 0, SEEK_SET) != 0 || fread(riff, 1, sizeof(riff), fp) != sizeof(riff)) {
        return 0;
    }
    if (memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        return 0;
    }

    ZeroMemory(info, sizeof(*info));

    while (fread(chunk_header, 1, sizeof(chunk_header), fp) == sizeof(chunk_header)) {
        DWORD chunk_size = *(DWORD *)(void *)(chunk_header + 4);
        long chunk_pos = ftell(fp);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            size_t copy_size = chunk_size < sizeof(fmt_buf) ? chunk_size : sizeof(fmt_buf);
            ZeroMemory(fmt_buf, sizeof(fmt_buf));
            if (fread(fmt_buf, 1, copy_size, fp) != copy_size) {
                return 0;
            }
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }

            ZeroMemory(&info->wfx, sizeof(info->wfx));
            memcpy(&info->wfx, fmt_buf,
                copy_size < sizeof(info->wfx) ? copy_size : sizeof(info->wfx));
            have_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            info->data_offset = (DWORD)chunk_pos;
            info->data_size = chunk_size;
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }
            have_data = 1;
        } else {
            if (fseek(fp, chunk_pos + (long)((chunk_size + 1u) & ~1u), SEEK_SET) != 0) {
                return 0;
            }
        }

        if (have_fmt && have_data) {
            break;
        }
    }

    if (!have_fmt || !have_data) {
        return 0;
    }

    return (fseek(fp, (long)info->data_offset, SEEK_SET) == 0);
}

static int td5_stream_rewind_data(void)
{
    if (!s_audio_stream.fp) {
        return 0;
    }
    if (fseek(s_audio_stream.fp, (long)s_audio_stream.data_offset, SEEK_SET) != 0) {
        return 0;
    }
    s_audio_stream.data_cursor = 0;
    s_audio_stream.eof = 0;
    return 1;
}

static void td5_stream_fill_half(int half_index)
{
    BYTE *ptr1 = NULL, *ptr2 = NULL;
    DWORD sz1 = 0, sz2 = 0;
    DWORD offset;
    DWORD total_size;
    DWORD written = 0;
    DWORD align;
    HRESULT hr;

    if (!s_audio_stream.buffer || !s_audio_stream.fp) {
        return;
    }

    offset = (DWORD)half_index * s_audio_stream.half_bytes;
    total_size = s_audio_stream.half_bytes;
    align = s_audio_stream.wfx.nBlockAlign ? s_audio_stream.wfx.nBlockAlign : 1;
    total_size -= total_size % align;
    if (total_size == 0) {
        total_size = s_audio_stream.half_bytes;
    }

    hr = IDirectSoundBuffer_Lock(s_audio_stream.buffer, offset, total_size,
        (void **)&ptr1, &sz1, (void **)&ptr2, &sz2, 0);
    if (FAILED(hr)) {
        return;
    }

    if (ptr1 && sz1) {
        ZeroMemory(ptr1, sz1);
    }
    if (ptr2 && sz2) {
        ZeroMemory(ptr2, sz2);
    }

    while (written < total_size) {
        DWORD remaining = s_audio_stream.data_size - s_audio_stream.data_cursor;
        DWORD request = total_size - written;
        BYTE *dst;
        DWORD segment_remaining;

        if (written < sz1) {
            dst = ptr1 + written;
            segment_remaining = sz1 - written;
        } else {
            dst = ptr2 + (written - sz1);
            segment_remaining = sz2 - (written - sz1);
        }
        if (request > segment_remaining) {
            request = segment_remaining;
        }

        if (remaining == 0) {
            if (!s_audio_stream.loop || !td5_stream_rewind_data()) {
                s_audio_stream.eof = 1;
                break;
            }
            remaining = s_audio_stream.data_size - s_audio_stream.data_cursor;
        }

        if (request > remaining) {
            request = remaining;
        }
        request -= request % align;
        if (request == 0) {
            request = remaining < align ? remaining : align;
        }

        if (fread(dst, 1, request, s_audio_stream.fp) != request) {
            s_audio_stream.eof = 1;
            break;
        }

        s_audio_stream.data_cursor += request;
        written += request;
    }

    IDirectSoundBuffer_Unlock(s_audio_stream.buffer, ptr1, sz1, ptr2, sz2);
}

int td5_plat_audio_init(void)
{
    HRESULT hr;
    DSBUFFERDESC desc;
    WAVEFORMATEX wfx;

    audio_build_vol_table();          /* faithful per-voice volume curve */

    hr = DirectSoundCreate8(NULL, &s_dsound, NULL);
    if (FAILED(hr) || !s_dsound) {
        s_dsound = NULL;
        return 0;
    }

    hr = IDirectSound8_SetCooperativeLevel(s_dsound,
        s_hwnd ? s_hwnd : GetDesktopWindow(),
        DSSCL_PRIORITY);
    if (FAILED(hr)) {
        IDirectSound8_Release(s_dsound);
        s_dsound = NULL;
        return 0;
    }

    /* Create primary buffer for format control */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize  = sizeof(DSBUFFERDESC);
    desc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &s_ds_primary, NULL);
    if (SUCCEEDED(hr) && s_ds_primary) {
        ZeroMemory(&wfx, sizeof(wfx));
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels        = 2;
        wfx.nSamplesPerSec   = 22050;
        wfx.wBitsPerSample   = 16;
        wfx.nBlockAlign       = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec   = wfx.nSamplesPerSec * wfx.nBlockAlign;
        IDirectSoundBuffer_SetFormat(s_ds_primary, &wfx);

        /* Keep the primary buffer — and therefore the DirectSound mixer —
         * running continuously. Without this the mixer goes idle whenever no
         * secondary buffer is playing; the next sound then incurs a wake-up
         * latency that swallows the start of short one-shots. In the frontend,
         * where only brief (~0.1s) selection/transition one-shots play with
         * gaps between them, that latency ate the whole sound, so menu SFX were
         * inaudible even though they were played at full volume. In a race the
         * continuous engine loops keep the mixer alive, so race SFX were never
         * affected — which is why the symptom was frontend-only. Playing the
         * primary looping (it outputs silence; it is the mix target) keeps the
         * pipeline hot and does not change any race behavior. */
        IDirectSoundBuffer_Play(s_ds_primary, 0, 0, DSBPLAY_LOOPING);
    }

    /* Silent keepalive: a continuously-looping SECONDARY buffer of pure silence.
     * This is what actually keeps the audio stream from idling between sounds —
     * playing the primary buffer alone proved insufficient on WDM/WASAPI, where
     * the render stream still ramps down when no secondary buffer is active. A
     * looping silent secondary keeps a live stream at all times, so short menu
     * one-shots (ping/whoosh) play instantly instead of being swallowed by the
     * device wake-up ramp. It is silence at minimum volume — inaudible, and it
     * changes nothing else (the race already stayed hot via its engine loops). */
    {
        DSBUFFERDESC kd;
        WAVEFORMATEX kwfx;
        const DWORD ka_bytes = 22050; /* ~0.5 s of 22050 Hz 16-bit mono silence */
        ZeroMemory(&kwfx, sizeof(kwfx));
        kwfx.wFormatTag      = WAVE_FORMAT_PCM;
        kwfx.nChannels       = 1;
        kwfx.nSamplesPerSec  = 22050;
        kwfx.wBitsPerSample  = 16;
        kwfx.nBlockAlign     = kwfx.nChannels * kwfx.wBitsPerSample / 8;
        kwfx.nAvgBytesPerSec = kwfx.nSamplesPerSec * kwfx.nBlockAlign;
        ZeroMemory(&kd, sizeof(kd));
        kd.dwSize        = sizeof(DSBUFFERDESC);
        kd.dwFlags       = DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
        kd.dwBufferBytes = ka_bytes;
        kd.lpwfxFormat   = &kwfx;
        if (SUCCEEDED(IDirectSound8_CreateSoundBuffer(s_dsound, &kd, &s_ds_keepalive, NULL))
            && s_ds_keepalive) {
            void *p1 = NULL, *p2 = NULL; DWORD b1 = 0, b2 = 0;
            if (SUCCEEDED(IDirectSoundBuffer_Lock(s_ds_keepalive, 0, ka_bytes,
                                                  &p1, &b1, &p2, &b2, 0))) {
                /* Fill with a tiny alternating ±1 signal, NOT zeros: a pure-zero
                 * or minimum-volume buffer is detected as silent by the WASAPI
                 * audio engine, which then idles the device anyway (defeating
                 * the keepalive). ±1 at 16-bit is ~-90 dB — inaudible — but a
                 * genuine non-zero stream the engine must keep rendering. */
                int16_t *s1 = (int16_t *)p1;
                int16_t *s2 = (int16_t *)p2;
                DWORD n;
                for (n = 0; n < b1 / 2; n++) s1[n] = (n & 1) ? 1 : -1;
                if (s2) for (n = 0; n < b2 / 2; n++) s2[n] = (n & 1) ? 1 : -1;
                IDirectSoundBuffer_Unlock(s_ds_keepalive, p1, b1, p2, b2);
            }
            /* Play at full volume (DSBVOLUME_MAX = 0). Setting it to MIN makes
             * the engine treat the stream as silent and idle the device — the
             * original cause of menu transition sounds being dropped after a
             * pause. The ±1 data is inaudible, so full volume is safe. */
            IDirectSoundBuffer_SetVolume(s_ds_keepalive, DSBVOLUME_MAX);
            IDirectSoundBuffer_Play(s_ds_keepalive, 0, 0, DSBPLAY_LOOPING);
            TD5_LOG_I(LOG_TAG, "audio keepalive buffer started (silent loop)");
        } else {
            TD5_LOG_W(LOG_TAG, "audio keepalive buffer creation failed");
        }
    }

    s_audio_buf_count = 0;
    ZeroMemory(s_ds_buffer_rates, sizeof(s_ds_buffer_rates));
    return 1;
}

void td5_plat_audio_shutdown(void)
{
    int i;

    td5_plat_audio_stream_stop();

    if (s_ds_keepalive) {
        IDirectSoundBuffer_Stop(s_ds_keepalive);
        IDirectSoundBuffer_Release(s_ds_keepalive);
        s_ds_keepalive = NULL;
    }

    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channels[i]) {
            IDirectSoundBuffer_Stop(s_ds_channels[i]);
            IDirectSoundBuffer_Release(s_ds_channels[i]);
            s_ds_channels[i] = NULL;
        }
    }
    for (i = 0; i < s_audio_buf_count; i++) {
        if (s_ds_buffers[i]) {
            IDirectSoundBuffer_Release(s_ds_buffers[i]);
            s_ds_buffers[i] = NULL;
        }
        s_ds_buffer_rates[i] = 0;
    }
    s_audio_buf_count = 0;

    if (s_ds_primary) {
        IDirectSoundBuffer_Release(s_ds_primary);
        s_ds_primary = NULL;
    }
    if (s_dsound) {
        IDirectSound8_Release(s_dsound);
        s_dsound = NULL;
    }
}

int td5_plat_audio_load_wav(const void *data, size_t size)
{
    const uint8_t *p = (const uint8_t *)data;
    const uint8_t *end = p + size;
    WAVEFORMATEX wfx;
    DSBUFFERDESC desc;
    LPDIRECTSOUNDBUFFER buf = NULL;
    HRESULT hr;
    const uint8_t *fmt_chunk = NULL;
    const uint8_t *data_chunk = NULL;
    uint32_t data_size = 0;
    void *ptr1 = NULL, *ptr2 = NULL;
    DWORD sz1 = 0, sz2 = 0;
    int idx;

    if (!s_dsound) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: DirectSound is NULL");
        return -1;
    }
    if (!data) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: data is NULL");
        return -1;
    }
    if (size < 44) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: WAV too small (%u bytes)",
                  (unsigned int)size);
        return -1;
    }
    /* Slot allocation: reuse a freed slot before bumping the high-water mark.
     * [SELFTEST FINDING 2026-07-02] The pool was a pure bump allocator —
     * td5_plat_audio_free() NULLed the slot but nothing ever reused it, so
     * ~40 WAVs per race exhausted the 256-slot pool after ~6 races in one
     * session and every later race lost its engine/collision audio. Freed
     * ids are safe to recycle: the sound layer scrubs its slot->buffer
     * tables (s_slot_to_buffer = -1) whenever it frees. */
    idx = -1;
    for (int scan = 0; scan < s_audio_buf_count; scan++) {
        if (!s_ds_buffers[scan]) { idx = scan; break; }
    }
    if (idx < 0 && s_audio_buf_count >= MAX_AUDIO_BUFFERS) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: buffer pool full (%d live)",
                  s_audio_buf_count);
        return -1;
    }

    /* Parse RIFF WAV header */
    if (memcmp(p, "RIFF", 4) != 0) return -1;
    if (memcmp(p + 8, "WAVE", 4) != 0) return -1;

    /* Find fmt and data chunks */
    p += 12;
    while (p + 8 <= end) {
        uint32_t chunk_size = *(const uint32_t *)(p + 4);
        const uint8_t *next = p + 8 + ((chunk_size + 1u) & ~1u);
        if (next < p || next > end) {
            TD5_LOG_E(LOG_TAG, "Audio load rejected: malformed WAV chunk overruns buffer");
            return -1;
        }
        if (memcmp(p, "fmt ", 4) == 0) {
            fmt_chunk = p + 8;
        } else if (memcmp(p, "data", 4) == 0) {
            data_chunk = p + 8;
            data_size = chunk_size;
        }
        p = next; /* chunk data + padding */
    }

    if (!fmt_chunk || !data_chunk || data_size == 0) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: missing fmt/data chunk");
        return -1;
    }
    if (fmt_chunk + 16 > end) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: truncated fmt chunk");
        return -1;
    }
    if (data_chunk + data_size > end) {
        TD5_LOG_E(LOG_TAG, "Audio load rejected: truncated data chunk size=%u file=%u",
                  (unsigned int)data_size, (unsigned int)size);
        return -1;
    }

    /* Build WAVEFORMATEX from fmt chunk */
    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag      = *(const uint16_t *)(fmt_chunk + 0);
    wfx.nChannels        = *(const uint16_t *)(fmt_chunk + 2);
    wfx.nSamplesPerSec   = *(const uint32_t *)(fmt_chunk + 4);
    wfx.nAvgBytesPerSec  = *(const uint32_t *)(fmt_chunk + 8);
    wfx.nBlockAlign       = *(const uint16_t *)(fmt_chunk + 12);
    wfx.wBitsPerSample   = *(const uint16_t *)(fmt_chunk + 14);

    /* Create sound buffer.
     * DSBCAPS_GLOBALFOCUS: keep SFX audible when the game window is not the
     * strict foreground window. Without it, DirectSound mutes these buffers
     * (and their DuplicateSoundBuffer copies) on focus loss — which silenced
     * the frontend selection/transition one-shots while the player was tabbing
     * between the (windowed) game and other windows, even though the buffers
     * were played at full volume. The streaming music buffer already sets this
     * flag; SFX must match it so menu and in-race SFX behave consistently. */
    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize          = sizeof(DSBUFFERDESC);
    desc.dwFlags         = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLPAN | DSBCAPS_CTRLFREQUENCY
                         | DSBCAPS_GLOBALFOCUS | DSBCAPS_STATIC;
    desc.dwBufferBytes   = data_size;
    desc.lpwfxFormat     = &wfx;

    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &buf, NULL);
    if (FAILED(hr) || !buf) {
        TD5_LOG_E(LOG_TAG, "CreateSoundBuffer failed hr=0x%08lX size=%u",
                  (unsigned long)hr, (unsigned int)data_size);
        return -1;
    }

    /* Copy WAV data into the buffer */
    hr = IDirectSoundBuffer_Lock(buf, 0, data_size, &ptr1, &sz1, &ptr2, &sz2, 0);
    if (FAILED(hr)) {
        TD5_LOG_E(LOG_TAG, "SoundBuffer lock failed hr=0x%08lX", (unsigned long)hr);
        IDirectSoundBuffer_Release(buf);
        return -1;
    }
    if (ptr1 && sz1) memcpy(ptr1, data_chunk, sz1 < data_size ? sz1 : data_size);
    if (ptr2 && sz2 && data_size > sz1) memcpy(ptr2, data_chunk + sz1, sz2);
    IDirectSoundBuffer_Unlock(buf, ptr1, sz1, ptr2, sz2);

    if (idx < 0) idx = s_audio_buf_count++;   /* no freed slot -> extend */
    s_ds_buffers[idx] = buf;
    s_ds_buffer_rates[idx] = wfx.nSamplesPerSec;
    TD5_LOG_I(LOG_TAG,
              "Audio buffer created: id=%d fmt=%uHz/%uch/%ubit size=%u",
              idx,
              (unsigned int)wfx.nSamplesPerSec,
              (unsigned int)wfx.nChannels,
              (unsigned int)wfx.wBitsPerSample,
              (unsigned int)data_size);
    return idx;
}

void td5_plat_audio_free(int buffer_index)
{
    if (buffer_index < 0 || buffer_index >= s_audio_buf_count) return;
    if (s_ds_buffers[buffer_index]) {
        IDirectSoundBuffer_Release(s_ds_buffers[buffer_index]);
        s_ds_buffers[buffer_index] = NULL;
    }
}

/* Stop + release the voice at channel index `i` and update the live count.
 * The generation is NOT bumped here — it is bumped on the next allocation of
 * this index, which is what invalidates any stale handle still pointing at it. */
static void audio_release_channel(int i)
{
    if (i < 0 || i >= MAX_AUDIO_CHANNELS) return;
    if (s_ds_channels[i]) {
        IDirectSoundBuffer_Stop(s_ds_channels[i]);
        IDirectSoundBuffer_Release(s_ds_channels[i]);
        s_ds_channels[i] = NULL;
        s_ds_channel_loop[i] = 0;
        if (s_audio_active_count > 0) s_audio_active_count--;
        s_audio_free_count++;
    }
}

/* Resolve a (generation,index) handle back to a live channel index, or -1 if
 * the handle is stale (the index was recycled/stolen for a different sound) or
 * the channel is no longer live. This is the guard that stops a stale
 * s_slot_to_channel entry from cross-talking onto another slot's voice. */
static int audio_resolve_channel(TD5_AudioChannel h)
{
    int idx;
    uint32_t gen;
    if (h < 0) return -1;
    idx = (int)((uint32_t)h & AUDIO_CHANNEL_INDEX_MASK);
    gen = (uint32_t)h >> AUDIO_CHANNEL_INDEX_BITS;
    if (idx < 0 || idx >= MAX_AUDIO_CHANNELS) return -1;
    if (!s_ds_channels[idx]) return -1;
    if (s_ds_channel_gen[idx] != gen) return -1;
    return idx;
}

/* Pick a channel index for a new voice.
 *
 *   1. an empty slot, else
 *   2. a finished (non-looping one-shot that stopped) slot — reclaimed lazily,
 *      else
 *   3. STEAL the least-important busy voice: the oldest non-looping voice if any
 *      is still playing, otherwise the oldest looping voice.
 *
 * Pass 3 is the critical S11 fix. The original M2DX DXSound::Play never fails on
 * a full bank — it walks the duplicate chain and steals the tail voice (see
 * re/analysis/wave4_deep_audits/da_m3_dxsound_polyphony.md §B). The old port
 * returned -1 once all 64 channels were busy; because looping engine/siren/
 * traffic voices never report "stopped", pass 2 then found nothing and every
 * later Play failed — the mix stuck on whatever loops already held the pool
 * (the "plays one sound for the rest of the session" symptom, made more likely
 * by >6 players spawning more loops). Stealing guarantees forward progress:
 * Play always returns a usable voice, and a stolen looping voice that is still
 * wanted simply re-Plays next frame (its old handle resolves stale, so
 * slot_is_playing() reports idle and the mixer restarts it).
 *
 * Victim choice (rare — only under genuine >64 concurrency): steal the OLDEST
 * one-shot first (it is the closest to finishing, so cutting it is least
 * audible), and only if every busy voice is looping, steal the NEWEST loop. The
 * long-lived loops carry the stable bed — the local player's own engine and the
 * ambient/rain/siren layers are started first (lowest serial), so stealing the
 * newest loop preserves them and sacrifices the most recently spawned overflow
 * voice instead. */
static int find_free_channel(void)
{
    int i;
    int steal = -1;
    uint32_t steal_serial = 0;

    /* Reap finished one-shots up front so they don't linger as "active" voices
     * and slowly creep the live count toward the cap over a long race (each
     * collision/hit allocates a one-shot voice that stays allocated until
     * reclaimed). Loops are intentionally NOT reaped here — they are meant to
     * keep playing; a momentarily-idle loop is recovered by the per-buffer dedup
     * in td5_plat_audio_play, not torn down. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channels[i] && !s_ds_channel_loop[i]) {
            DWORD status = 0;
            IDirectSoundBuffer_GetStatus(s_ds_channels[i], &status);
            if (!(status & DSBSTATUS_PLAYING)) {
                audio_release_channel(i);
            }
        }
    }

    /* Pass 1: an empty slot. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (!s_ds_channels[i]) return i;
    }
    /* Pass 2: steal the oldest still-playing ONE-SHOT (lowest serial, loop==0).
     *
     * [S11] Looping voices are NEVER reclaimed or stolen here. The mixer keeps
     * the slot->channel handle of every engine/skid/siren/traffic loop and
     * modulates it every frame via slot_modify(); if find_free_channel recycled
     * that channel for another sound, the slot's handle would resolve stale and
     * slot_modify() would silently no-op — leaving the loop frozen at its last
     * volume/pitch for the rest of the race (the "engine/skid sound locks into a
     * constant loop when I drift or get rear-ended" bug: a drift/collision spawns
     * one-shot hits that used to steal the engine/skid voice's channel). A
     * momentarily-idle loop (a volume-0 buffer WASAPI parked, a lost buffer) is
     * NOT torn down — the per-buffer dedup in td5_plat_audio_play restarts it in
     * place on the next re-issue, keeping its channel and handle stable. The
     * per-buffer dedup caps loops at ~one per distinct sound (well under 64), so
     * pinning them never starves one-shots. */
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        if (s_ds_channel_loop[i]) continue;
        if (steal < 0 || s_ds_channel_serial[i] < steal_serial) {
            steal = i;
            steal_serial = s_ds_channel_serial[i];
        }
    }
    if (steal >= 0) {
        s_audio_steal_count++;
        audio_release_channel(steal);
        return steal;
    }
    return -1;
}

/** Fill s_vol_table with the original M2DX per-voice attenuation curve. Safe to
 *  call before/after DirectSound init; uses only libm. */
static void audio_build_vol_table(void)
{
    int i;
    double k = 500.0 / log10(2.0);  /* == 1660.964..., the original's scale */
    for (i = 0; i < 128; i++) {
        /* (int) truncates toward zero, matching the original's __ftol(). */
        s_vol_table[i] = -(int)(k * log10(128.0 / (double)(i + 1)));
    }
}

/** Final DirectSound volume (centibels) for a mixer voice volume (0..127):
 *  faithful per-voice table value plus the separate master attenuation. */
static LONG audio_effective_cb(int vol)
{
    int cb;
    if (s_audio_muted || s_audio_focus_muted) return DSBVOLUME_MIN;
    if (vol < 0)   vol = 0;
    if (vol > 127) vol = 127;
    cb = s_vol_table[vol] + s_master_offset_cb;
    if (cb < DSBVOLUME_MIN) cb = DSBVOLUME_MIN;
    if (cb > DSBVOLUME_MAX) cb = DSBVOLUME_MAX;
    return (LONG)cb;
}

/** Pass through a native DirectSound-scale pan (-10000..+10000).
 *
 * The TD5 sound module (td5_sound.c) emits pans already in DirectSound's
 * native units, mirroring the original game's DXSound::Modify calls:
 *   0           = centered (most sounds; all non-split-screen AI/traffic)
 *   +/-~491 max = single-player steering pan (UpdateVehicleAudioMix 0x441160:
 *                 steering_command * -0x51EB851F >> 38, range +/-0x18000)
 *   +/-10000    = split-screen viewport sides (literal in the original)
 *
 * The previous implementation clamped the input to +/-100 then multiplied by
 * 100, which turned the subtle +/-491 steering pan into a full hard +/-10000
 * pan -- the "very aggressive stereo when steering" bug. (Split-screen +/-10000
 * survived only by coincidence: clamp to +/-100 then x100 = +/-10000.)
 * Clamp to the native DirectSound range and pass through unchanged. */
static LONG pan_to_ds(int pan)
{
    if (pan < -10000) pan = -10000;
    if (pan >  10000) pan =  10000;
    return (LONG)pan;
}

static DWORD td5_audio_translate_frequency(int buffer_index, int frequency)
{
    DWORD native_rate = 22050;
    double scaled;

    if (frequency <= 0) {
        return 0;
    }

    if (buffer_index >= 0 && buffer_index < MAX_AUDIO_BUFFERS &&
        s_ds_buffer_rates[buffer_index] != 0) {
        native_rate = s_ds_buffer_rates[buffer_index];
    }

    scaled = ((double)frequency * (double)native_rate) / 22050.0;
    if (scaled < (double)DSBFREQUENCY_MIN) scaled = (double)DSBFREQUENCY_MIN;
    if (scaled > (double)DSBFREQUENCY_MAX) scaled = (double)DSBFREQUENCY_MAX;
    return (DWORD)(scaled + 0.5);
}

TD5_AudioChannel td5_plat_audio_play(int buffer_index, int loop,
                                      int volume, int pan, int frequency)
{
    LPDIRECTSOUNDBUFFER src_buf, dup_buf = NULL;
    HRESULT hr;
    int ch;

    if (!s_dsound) return TD5_AUDIO_INVALID_CHANNEL;
    if (buffer_index < 0 || buffer_index >= s_audio_buf_count) return TD5_AUDIO_INVALID_CHANNEL;
    src_buf = s_ds_buffers[buffer_index];
    if (!src_buf) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    /* [S11] One looping voice per source buffer.
     *
     * The original M2DX kept a fixed buffer per sound and re-Played THAT buffer;
     * it never allocated a voice per Play(). The port instead DuplicateSoundBuffer's
     * a fresh voice every Play and only tracks the latest in the caller's
     * slot->channel table — so when the per-frame mixer re-issues a loop start
     * (which it does whenever GetStatus transiently reports the looping buffer
     * idle: a volume-0 buffer WASAPI parked, a lost buffer, a stolen voice), each
     * re-issue stranded the previous voice. With >6 racers those orphans piled up
     * ~2/frame until all 64 voices were copies of one engine loop, droning over
     * everything — the "overload / stuck on one sound" the user hit. Slot-level
     * stop-before-replay couldn't catch it (the slot's stored handle had already
     * gone stale). Deduping at the source buffer is reliable: a looping buffer
     * gets exactly ONE voice. A re-issue finds that voice and reuses it —
     * reapplying volume/pan/freq and restarting it only if it actually stopped
     * (which also recovers a lost buffer) — so nothing accumulates and an already-
     * playing loop is not even interrupted (no per-frame restart click). */
    if (loop) {
        for (ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
            LPDIRECTSOUNDBUFFER ex = s_ds_channels[ch];
            DWORD status;
            if (!ex || !s_ds_channel_loop[ch] || s_ds_channel_buf[ch] != buffer_index)
                continue;
            status = 0;
            IDirectSoundBuffer_GetStatus(ex, &status);
            if (status & DSBSTATUS_BUFFERLOST) {
                IDirectSoundBuffer_Restore(ex);
                status = 0;
                IDirectSoundBuffer_GetStatus(ex, &status);
            }
            IDirectSoundBuffer_SetVolume(ex, audio_effective_cb(volume));
            IDirectSoundBuffer_SetPan(ex, pan_to_ds(pan));
            if (frequency > 0)
                IDirectSoundBuffer_SetFrequency(ex, td5_audio_translate_frequency(buffer_index, frequency));
            if (!(status & DSBSTATUS_PLAYING))
                IDirectSoundBuffer_Play(ex, 0, 0, DSBPLAY_LOOPING);
            s_ds_channel_serial[ch] = ++s_audio_alloc_serial; /* refresh recency vs steal */
            return (TD5_AudioChannel)(((int)s_ds_channel_gen[ch] << AUDIO_CHANNEL_INDEX_BITS) | ch);
        }
    }

    ch = find_free_channel();
    if (ch < 0) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    /* Duplicate the buffer so multiple instances can play simultaneously */
    hr = IDirectSound8_DuplicateSoundBuffer(s_dsound, src_buf, &dup_buf);
    if (FAILED(hr) || !dup_buf) { s_audio_fail_count++; return TD5_AUDIO_INVALID_CHANNEL; }

    /* Claim the index: bump its generation so any stale handle that still names
     * this index resolves invalid, and record alloc order + loop flag for the
     * voice-steal policy. */
    s_ds_channel_gen[ch]++;
    if (s_ds_channel_gen[ch] > AUDIO_CHANNEL_GEN_MAX) s_ds_channel_gen[ch] = 1;
    s_ds_channels[ch] = dup_buf;
    s_ds_channel_buf[ch] = buffer_index;
    s_ds_channel_serial[ch] = ++s_audio_alloc_serial;
    s_ds_channel_loop[ch] = loop ? 1 : 0;
    s_audio_alloc_count++;
    s_audio_active_count++;
    if (s_audio_active_count > s_audio_peak_active) s_audio_peak_active = s_audio_active_count;

    /* Apply parameters */
    IDirectSoundBuffer_SetVolume(dup_buf, audio_effective_cb(volume));
    IDirectSoundBuffer_SetPan(dup_buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(dup_buf, td5_audio_translate_frequency(buffer_index, frequency));

    IDirectSoundBuffer_SetCurrentPosition(dup_buf, 0);
    IDirectSoundBuffer_Play(dup_buf, 0, 0, loop ? DSBPLAY_LOOPING : 0);

    TD5_LOG_I(LOG_TAG, "Audio channel play: channel=%d gen=%u buffer=%d loop=%d active=%d",
              ch, (unsigned)s_ds_channel_gen[ch], buffer_index, loop, s_audio_active_count);

    return (TD5_AudioChannel)(((int)s_ds_channel_gen[ch] << AUDIO_CHANNEL_INDEX_BITS) | ch);
}

void td5_plat_audio_stop(TD5_AudioChannel ch)
{
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return;
    TD5_LOG_I(LOG_TAG, "Audio channel stop: channel=%d buffer=%d active=%d",
              idx, s_ds_channel_buf[idx], s_audio_active_count - 1);
    audio_release_channel(idx);
}

void td5_plat_audio_modify(TD5_AudioChannel ch, int volume, int pan, int frequency)
{
    LPDIRECTSOUNDBUFFER buf;
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return;
    buf = s_ds_channels[idx];

    IDirectSoundBuffer_SetVolume(buf, audio_effective_cb(volume));
    IDirectSoundBuffer_SetPan(buf, pan_to_ds(pan));
    if (frequency > 0)
        IDirectSoundBuffer_SetFrequency(buf, td5_audio_translate_frequency(s_ds_channel_buf[idx], frequency));
}

int td5_plat_audio_is_playing(TD5_AudioChannel ch)
{
    DWORD status = 0;
    int idx = audio_resolve_channel(ch);
    if (idx < 0) return 0;
    IDirectSoundBuffer_GetStatus(s_ds_channels[idx], &status);
    return (status & DSBSTATUS_PLAYING) ? 1 : 0;
}

/* Does this handle still name a live voice this slot owns? Unlike is_playing,
 * this ignores DirectSound's transient PLAYING bit — a looping voice that was
 * momentarily parked (volume 0) or lost still counts as owned. Used so the mixer
 * doesn't tear down/re-trigger a loop it is still modulating just because the
 * backend briefly reported it idle. Returns 0 for a stale/released handle. */
int td5_plat_audio_channel_valid(TD5_AudioChannel ch)
{
    return audio_resolve_channel(ch) >= 0 ? 1 : 0;
}

/* Release every active SFX voice (NOT the primary or silent-keepalive buffers).
 * Called at race teardown so voice counts return to baseline between races and
 * no looping engine/siren/traffic voice can survive into the next race or the
 * menus. The keepalive stays running so menu one-shots remain instant. */
void td5_plat_audio_stop_all(void)
{
    int i;
    for (i = 0; i < MAX_AUDIO_CHANNELS; i++) {
        audio_release_channel(i);
    }
}

int td5_plat_audio_active_channels(void)
{
    return s_audio_active_count;
}

void td5_plat_audio_log_stats(const char *tag)
{
    TD5_LOG_I(LOG_TAG,
              "voice stats [%s]: active=%d peak=%d alloc=%u free=%u steal=%u fail=%u",
              tag ? tag : "",
              s_audio_active_count, s_audio_peak_active,
              (unsigned)s_audio_alloc_count, (unsigned)s_audio_free_count,
              (unsigned)s_audio_steal_count, (unsigned)s_audio_fail_count);
}

void td5_plat_audio_set_master_volume(int volume)
{
    int idx;
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_master_volume = volume;
    /* Master attenuation = table[master>>9], exactly as DXSound::SetVolume indexes
     * it (the 0..100 slider maps to the original's 0..0xFFFF range, then >>9 gives
     * the 0..127 table index). Applied as a dB offset on every voice, which is
     * equivalent to the original scaling the summed primary buffer. */
    idx = (volume * 0xFFFF / 100) >> 9;
    if (idx < 0)   idx = 0;
    if (idx > 127) idx = 127;
    if (s_vol_table[127] == 0 && s_vol_table[0] == 0) audio_build_vol_table();
    s_master_offset_cb = s_vol_table[idx];
}

void td5_plat_audio_set_muted(int muted)
{
    int m = muted ? 1 : 0;
    /* Edge-triggered, mirroring DXSound::MuteAll's `if (g_isSoundMuted == 0)`
     * guard (M2DX.dll @0x1000d7c0): act only on a real mute/unmute transition. */
    if (m == s_audio_muted)
        return;
    s_audio_muted = m;

    if (m) {
        /* [PAUSE-MUTE 2026-06-23] Faithful to DXSound::MuteAll @0x1000d7c0, which
         * iterates the whole sound-buffer table and immediately SetVolume(-10000)
         * on every live voice. The port's mute used to be purely LAZY (set the
         * flag and let each voice pick it up on its next per-frame volume
         * re-apply via audio_effective_cb). That silenced looping voices
         * (engine/tyre/rain — re-modulated every frame) and blocked NEW one-shots
         * (audio_effective_cb returns MIN at Play time), but voices ALREADY
         * playing at the instant of pause that are NOT re-modulated — one-shot
         * crash/horn/skid SFX — kept their pre-pause volume and stayed audible
         * under the pause menu ("pausing still plays some sounds"). Force them all
         * silent now. Unmute leaves restoration to the per-frame mixer
         * (audio_effective_cb re-applies looping-voice volumes), the port's
         * equivalent of UnMuteAll's per-buffer attenuation restore @0x1000d800. */
        int swept = 0;
        for (int i = 0; i < MAX_AUDIO_CHANNELS; i++) {
            if (s_ds_channels[i]) {
                IDirectSoundBuffer_SetVolume(s_ds_channels[i], DSBVOLUME_MIN);
                swept++;
            }
        }
        TD5_LOG_I(LOG_TAG,
                  "audio mute: forced %d live voice(s) to silence (MuteAll-faithful)",
                  swept);
    } else {
        TD5_LOG_I(LOG_TAG,
                  "audio unmute: mixer restores looping voices next frame");
    }
}

/* True when the game window currently holds the OS foreground (keyboard) focus.
 * Used to gate "press ESC to abort" in cinematic races (attract demo / view
 * replay): an attract demo must keep playing when the window is in the
 * background, and a stale/global ESC read while unfocused must not cut it short.
 * Returns 1 (focused) when there is no window handle so headless/standalone
 * behaviour is unchanged. [ATTRACT DEMO 2026-06-25] */
int td5_plat_window_is_focused(void)
{
    return (!s_hwnd) || (GetForegroundWindow() == s_hwnd);
}

/* Mute all DirectSound output while the game window is not the foreground
 * window; restore on refocus. Edge-triggered so it only acts on a focus
 * change. Per-voice SFX pick up s_audio_focus_muted via audio_effective_cb on
 * their next per-frame volume re-apply (looping engine/tyre voices update every
 * frame); the ambient stream plays at a fixed volume that bypasses
 * audio_effective_cb, so its buffer volume is set explicitly here. CD audio
 * (MCI cdaudio) is a separate device and is left alone. */
void td5_plat_audio_update_focus_mute(void)
{
    int focused = (!s_hwnd) || (GetForegroundWindow() == s_hwnd);
    int want    = focused ? 0 : 1;
    if (want == s_audio_focus_muted)
        return;                       /* no change -> nothing to do */
    s_audio_focus_muted = want;
    if (s_audio_stream.buffer)
        IDirectSoundBuffer_SetVolume(s_audio_stream.buffer,
                                     want ? DSBVOLUME_MIN : DSBVOLUME_MAX);
    td5_plat_radio_set_focus_muted(want);   /* mute the radio while minimized/alt-tabbed */
    TD5_LOG_I(LOG_TAG, "audio focus-mute: window %s -> %s",
              focused ? "focused" : "unfocused", want ? "MUTED" : "unmuted");
}

int td5_plat_audio_stream_play(const char *wav_path, int loop)
{
    TD5_WavFileInfo info;
    DSBUFFERDESC desc;
    FILE *fp;
    HRESULT hr;

    if (!s_dsound || !wav_path) {
        return 0;
    }

    td5_plat_audio_stream_stop();

    fp = fopen(wav_path, "rb");
    if (!fp) {
        return 0;
    }
    if (!td5_stream_parse_wav(fp, &info)) {
        fclose(fp);
        return 0;
    }

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize = sizeof(desc);
    desc.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS |
                   DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY;
    desc.dwBufferBytes = TD5_STREAM_BUFFER_BYTES;
    desc.lpwfxFormat = &info.wfx;

    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &s_audio_stream.buffer, NULL);
    if (FAILED(hr) || !s_audio_stream.buffer) {
        fclose(fp);
        return 0;
    }

    s_audio_stream.fp = fp;
    s_audio_stream.wfx = info.wfx;
    s_audio_stream.data_offset = info.data_offset;
    s_audio_stream.data_size = info.data_size;
    s_audio_stream.data_cursor = 0;
    s_audio_stream.buffer_bytes = TD5_STREAM_BUFFER_BYTES;
    s_audio_stream.half_bytes = TD5_STREAM_BUFFER_BYTES / 2;
    s_audio_stream.last_play_cursor = 0;
    s_audio_stream.loop = loop ? 1 : 0;
    s_audio_stream.eof = 0;
    s_audio_stream.active = 1;

    td5_stream_rewind_data();
    td5_stream_fill_half(0);
    td5_stream_fill_half(1);

    IDirectSoundBuffer_SetCurrentPosition(s_audio_stream.buffer, 0);
    if (s_audio_focus_muted)   /* started while alt-tabbed away: stay silent */
        IDirectSoundBuffer_SetVolume(s_audio_stream.buffer, DSBVOLUME_MIN);
    IDirectSoundBuffer_Play(s_audio_stream.buffer, 0, 0, DSBPLAY_LOOPING);
    TD5_LOG_I(LOG_TAG, "Audio stream open: path=%s loop=%d size=%u",
              wav_path, loop, (unsigned int)info.data_size);
    return 1;
}

void td5_plat_audio_stream_stop(void)
{
    if (s_audio_stream.active) {
        TD5_LOG_I(LOG_TAG, "Audio stream close: size=%u cursor=%u",
                  (unsigned int)s_audio_stream.data_size,
                  (unsigned int)s_audio_stream.data_cursor);
    }
    if (s_audio_stream.buffer) {
        IDirectSoundBuffer_Stop(s_audio_stream.buffer);
        IDirectSoundBuffer_Release(s_audio_stream.buffer);
    }
    if (s_audio_stream.fp) {
        fclose(s_audio_stream.fp);
    }
    ZeroMemory(&s_audio_stream, sizeof(s_audio_stream));
}

void td5_plat_audio_stream_refresh(void)
{
    DWORD status = 0;
    DWORD play_cursor = 0;
    int prev_half;
    int cur_half;

    if (!s_audio_stream.active || !s_audio_stream.buffer) {
        return;
    }

    IDirectSoundBuffer_GetStatus(s_audio_stream.buffer, &status);
    if (!(status & DSBSTATUS_PLAYING)) {
        return;
    }

    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(s_audio_stream.buffer, &play_cursor, NULL))) {
        return;
    }

    prev_half = (s_audio_stream.last_play_cursor < s_audio_stream.half_bytes) ? 0 : 1;
    cur_half = (play_cursor < s_audio_stream.half_bytes) ? 0 : 1;

    if (s_audio_stream.eof && !s_audio_stream.loop && play_cursor < s_audio_stream.last_play_cursor) {
        td5_plat_audio_stream_stop();
        return;
    }

    if (cur_half != prev_half) {
        td5_stream_fill_half(prev_half);
        TD5_LOG_D(LOG_TAG, "Audio stream refresh: play_cursor=%u filled_half=%d",
                  (unsigned int)play_cursor, prev_half);
    }

    s_audio_stream.last_play_cursor = play_cursor;
}

int td5_plat_audio_stream_is_playing(void)
{
    DWORD status = 0;

    if (!s_audio_stream.active || !s_audio_stream.buffer) {
        return 0;
    }
    IDirectSoundBuffer_GetStatus(s_audio_stream.buffer, &status);
    return (status & DSBSTATUS_PLAYING) ? 1 : 0;
}

/* ========================================================================
 * CD Audio (via MCI)
 * ======================================================================== */

static int s_cd_initialized = 0;

static void cd_ensure_init(void)
{
    if (!s_cd_initialized) {
        mciSendStringA("open cdaudio", NULL, 0, NULL);
        mciSendStringA("set cdaudio time format tmsf", NULL, 0, NULL);
        s_cd_initialized = 1;
    }
}

void td5_plat_cd_play(int track)
{
    char cmd[128];
    cd_ensure_init();
    snprintf(cmd, sizeof(cmd), "play cdaudio from %d to %d", track, track + 1);
    TD5_LOG_I(LOG_TAG, "CD play track=%d", track);
    mciSendStringA(cmd, NULL, 0, NULL);
}

void td5_plat_cd_stop(void)
{
    cd_ensure_init();
    TD5_LOG_I(LOG_TAG, "CD stop");
    mciSendStringA("stop cdaudio", NULL, 0, NULL);
}

void td5_plat_cd_set_volume(int volume)
{
    char cmd[128];
    cd_ensure_init();
    /* MCI audio volume: 0-1000 */
    snprintf(cmd, sizeof(cmd), "setaudio cdaudio volume to %d",
             (volume * 1000) / 100);
    mciSendStringA(cmd, NULL, 0, NULL);
}

/* ========================================================================
 * Radio PCM streaming sink (push-based) -- see td5_platform.h
 *
 * Worker thread (td5_radio.c) decodes a network stream and pushes PCM via
 * td5_plat_radio_submit() into a thread-safe ring buffer; the main-thread pump
 * feeds a looping DirectSound buffer from that ring. All DSound calls live on
 * the main thread; the worker only touches the ring under s_radio.cs.
 * ======================================================================== */

typedef struct TD5_RadioSink {
    LPDIRECTSOUNDBUFFER buffer;
    WAVEFORMATEX        wfx;
    DWORD               buffer_bytes;
    DWORD               half_bytes;
    DWORD               last_play_cursor;
    int                 created;
    int                 playing;            /* race-level play/stop (radio backend)   */
    int                 active;             /* in-race gate (per-frame from main.c)    */
    int                 focus_muted;        /* window minimized / not foreground       */
    int                 output_on;          /* DSound buffer currently playing         */
    int                 want_volume;        /* 0..100 (RadioVolume)                    */

    unsigned char      *ring;
    DWORD               ring_cap;
    DWORD               ring_head;           /* write offset (worker) */
    DWORD               ring_tail;           /* read offset (main)    */
    DWORD               ring_count;          /* bytes stored          */

    CRITICAL_SECTION    cs;
    int                 cs_init;
    volatile LONG       fmt_ready;
    int                 want_rate, want_ch, want_bits;
} TD5_RadioSink;

static TD5_RadioSink s_radio;

/* Map 0..100 to a DirectSound hundredths-of-a-dB attenuation (perceptual). */
static int radio_vol_to_dsound(int v)
{
    double db; long h;
    if (v <= 0)   return DSBVOLUME_MIN;
    if (v >= 100) return 0;                  /* DSBVOLUME_MAX == 0 */
    db = 20.0 * log10((double)v / 100.0);
    h  = (long)(db * 100.0);
    if (h < DSBVOLUME_MIN) h = DSBVOLUME_MIN;
    if (h > 0) h = 0;
    return (int)h;
}

void td5_plat_radio_open(void)
{
    if (!s_radio.cs_init) {
        InitializeCriticalSection(&s_radio.cs);
        s_radio.cs_init = 1;
    }
    s_radio.want_volume = 10;     /* low default; overridden by RadioVolume at init */
    s_radio.playing     = 0;
    s_radio.active      = 0;      /* not in a race until main.c says so */
    s_radio.focus_muted = 0;
    s_radio.output_on   = 0;
    TD5_LOG_I(LOG_TAG, "radio sink opened");
}

int td5_plat_radio_begin(int sample_rate, int channels, int bits_per_sample)
{
    DWORD cap;
    if (sample_rate <= 0 || channels <= 0 || bits_per_sample <= 0)
        return 0;
    if (!s_radio.cs_init) {                  /* defensive: open() should precede */
        InitializeCriticalSection(&s_radio.cs);
        s_radio.cs_init = 1;
    }
    cap = (DWORD)(sample_rate * channels * (bits_per_sample / 8)) * 4u; /* ~4 seconds */
    EnterCriticalSection(&s_radio.cs);
    s_radio.want_rate = sample_rate;
    s_radio.want_ch   = channels;
    s_radio.want_bits = bits_per_sample;
    if (s_radio.ring && s_radio.ring_cap != cap) {
        free(s_radio.ring); s_radio.ring = NULL;
    }
    if (!s_radio.ring) {
        s_radio.ring = (unsigned char *)malloc(cap);
        s_radio.ring_cap = s_radio.ring ? cap : 0;
    }
    s_radio.ring_head = s_radio.ring_tail = s_radio.ring_count = 0;
    LeaveCriticalSection(&s_radio.cs);
    if (!s_radio.ring) {
        TD5_LOG_E(LOG_TAG, "radio: ring alloc failed (%u bytes)", (unsigned)cap);
        return 0;
    }
    InterlockedExchange(&s_radio.fmt_ready, 1);
    TD5_LOG_I(LOG_TAG, "radio format: %d Hz %d ch %d-bit (ring %u bytes)",
              sample_rate, channels, bits_per_sample, (unsigned)cap);
    return 1;
}

int td5_plat_radio_submit(const void *pcm, int count)
{
    DWORD space, take, first;
    if (!pcm || count <= 0 || !s_radio.cs_init || !s_radio.ring)
        return 0;
    EnterCriticalSection(&s_radio.cs);
    if (!s_radio.ring) { LeaveCriticalSection(&s_radio.cs); return 0; }
    space = s_radio.ring_cap - s_radio.ring_count;
    take  = ((DWORD)count < space) ? (DWORD)count : space;
    first = s_radio.ring_cap - s_radio.ring_head;
    if (first > take) first = take;
    if (first)        memcpy(s_radio.ring + s_radio.ring_head, pcm, first);
    if (take > first) memcpy(s_radio.ring, (const unsigned char *)pcm + first, take - first);
    s_radio.ring_head  = (s_radio.ring_head + take) % s_radio.ring_cap;
    s_radio.ring_count += take;
    LeaveCriticalSection(&s_radio.cs);
    return (int)take;
}

/* Copy `bytes` from the ring into dst, padding the tail with silence on
 * underrun (PCM zero). Locks internally. */
static void radio_ring_read(unsigned char *dst, DWORD bytes)
{
    DWORD avail, take, first;
    EnterCriticalSection(&s_radio.cs);
    avail = s_radio.ring_count;
    take  = (avail < bytes) ? avail : bytes;
    first = s_radio.ring_cap - s_radio.ring_tail;
    if (first > take) first = take;
    if (take) {
        if (first)        memcpy(dst, s_radio.ring + s_radio.ring_tail, first);
        if (take > first) memcpy(dst + first, s_radio.ring, take - first);
        s_radio.ring_tail   = (s_radio.ring_tail + take) % s_radio.ring_cap;
        s_radio.ring_count -= take;
    }
    LeaveCriticalSection(&s_radio.cs);
    if (take < bytes)
        memset(dst + take, 0, bytes - take);
}

static void radio_fill_half(int half)
{
    void *p1 = NULL, *p2 = NULL; DWORD s1 = 0, s2 = 0;
    DWORD offset = (DWORD)half * s_radio.half_bytes;
    HRESULT hr;
    if (!s_radio.buffer) return;
    hr = IDirectSoundBuffer_Lock(s_radio.buffer, offset, s_radio.half_bytes,
                                 &p1, &s1, &p2, &s2, 0);
    if (FAILED(hr)) return;
    if (p1 && s1) radio_ring_read((unsigned char *)p1, s1);
    if (p2 && s2) radio_ring_read((unsigned char *)p2, s2);
    IDirectSoundBuffer_Unlock(s_radio.buffer, p1, s1, p2, s2);
}

/* Recompute audibility and Play/Stop the buffer to match. The radio is audible
 * only while playing (race-level) AND active (in a race) AND focused. On the
 * rising edge it flushes the ring and resumes at the live edge. */
static void radio_apply_output(void)
{
    int want;
    if (!s_radio.created || !s_radio.buffer) return;
    want = s_radio.playing && s_radio.active && !s_radio.focus_muted;
    if (want && !s_radio.output_on) {
        EnterCriticalSection(&s_radio.cs);
        s_radio.ring_head = s_radio.ring_tail = s_radio.ring_count = 0;
        LeaveCriticalSection(&s_radio.cs);
        radio_fill_half(0);
        radio_fill_half(1);
        IDirectSoundBuffer_SetCurrentPosition(s_radio.buffer, 0);
        s_radio.last_play_cursor = 0;
        IDirectSoundBuffer_SetVolume(s_radio.buffer, radio_vol_to_dsound(s_radio.want_volume));
        IDirectSoundBuffer_Play(s_radio.buffer, 0, 0, DSBPLAY_LOOPING);
        s_radio.output_on = 1;
    } else if (!want && s_radio.output_on) {
        IDirectSoundBuffer_Stop(s_radio.buffer);
        s_radio.output_on = 0;
    }
}

static void radio_create_buffer(void)
{
    DSBUFFERDESC desc;
    DWORD bps, align;
    HRESULT hr;
    if (s_radio.created || !s_dsound) return;

    ZeroMemory(&s_radio.wfx, sizeof(s_radio.wfx));
    s_radio.wfx.wFormatTag      = WAVE_FORMAT_PCM;
    s_radio.wfx.nChannels       = (WORD)s_radio.want_ch;
    s_radio.wfx.nSamplesPerSec  = (DWORD)s_radio.want_rate;
    s_radio.wfx.wBitsPerSample  = (WORD)s_radio.want_bits;
    s_radio.wfx.nBlockAlign     = (WORD)(s_radio.want_ch * (s_radio.want_bits / 8));
    s_radio.wfx.nAvgBytesPerSec = s_radio.wfx.nSamplesPerSec * s_radio.wfx.nBlockAlign;

    bps   = s_radio.wfx.nAvgBytesPerSec;     /* ~1 second */
    align = s_radio.wfx.nBlockAlign ? s_radio.wfx.nBlockAlign : 4;
    bps  -= (bps % (align * 2u));            /* keep both halves block-aligned */
    if (bps < align * 2u) bps = align * 2u;
    s_radio.buffer_bytes = bps;
    s_radio.half_bytes   = bps / 2u;

    ZeroMemory(&desc, sizeof(desc));
    desc.dwSize        = sizeof(desc);
    desc.dwFlags       = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLVOLUME;
    desc.dwBufferBytes = s_radio.buffer_bytes;
    desc.lpwfxFormat   = &s_radio.wfx;

    hr = IDirectSound8_CreateSoundBuffer(s_dsound, &desc, &s_radio.buffer, NULL);
    if (FAILED(hr) || !s_radio.buffer) {
        TD5_LOG_E(LOG_TAG, "radio: CreateSoundBuffer failed hr=0x%08lX", (unsigned long)hr);
        s_radio.buffer = NULL;
        return;
    }
    s_radio.created = 1;
    s_radio.output_on = 0;
    s_radio.last_play_cursor = 0;
    IDirectSoundBuffer_SetVolume(s_radio.buffer, radio_vol_to_dsound(s_radio.want_volume));
    TD5_LOG_I(LOG_TAG, "radio: DSound buffer created (%u bytes)", (unsigned)s_radio.buffer_bytes);
    radio_apply_output();   /* start now if all gates are open */
}

void td5_plat_radio_pump(void)
{
    DWORD status = 0, play_cursor = 0;
    int prev_half, cur_half;
    if (!s_radio.created) {
        if (s_radio.fmt_ready) radio_create_buffer();
        if (!s_radio.created) return;
    }
    if (!s_radio.output_on) return;
    IDirectSoundBuffer_GetStatus(s_radio.buffer, &status);
    if (!(status & DSBSTATUS_PLAYING)) {
        IDirectSoundBuffer_Play(s_radio.buffer, 0, 0, DSBPLAY_LOOPING);
        return;
    }
    if (FAILED(IDirectSoundBuffer_GetCurrentPosition(s_radio.buffer, &play_cursor, NULL)))
        return;
    prev_half = (s_radio.last_play_cursor < s_radio.half_bytes) ? 0 : 1;
    cur_half  = (play_cursor < s_radio.half_bytes) ? 0 : 1;
    if (cur_half != prev_half)
        radio_fill_half(prev_half);
    s_radio.last_play_cursor = play_cursor;
}

void td5_plat_radio_set_playing(int on)
{
    s_radio.playing = on ? 1 : 0;
    radio_apply_output();
}

void td5_plat_radio_set_active(int active)
{
    s_radio.active = active ? 1 : 0;
    radio_apply_output();
}

void td5_plat_radio_set_focus_muted(int muted)
{
    s_radio.focus_muted = muted ? 1 : 0;
    radio_apply_output();
}

int td5_plat_radio_wants_data(void)
{
    return s_radio.output_on;
}

void td5_plat_radio_set_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_radio.want_volume = volume;
    /* Only push to the device while audible; the rising-edge in radio_apply_output
     * sets the volume on resume, so this never un-mutes a gated radio. */
    if (s_radio.buffer && s_radio.output_on)
        IDirectSoundBuffer_SetVolume(s_radio.buffer, radio_vol_to_dsound(volume));
}

int td5_plat_radio_pending_bytes(void)
{
    int n;
    if (!s_radio.cs_init) return 0;
    EnterCriticalSection(&s_radio.cs);
    n = (int)s_radio.ring_count;
    LeaveCriticalSection(&s_radio.cs);
    return n;
}

void td5_plat_radio_close(void)
{
    if (s_radio.buffer) {
        IDirectSoundBuffer_Stop(s_radio.buffer);
        IDirectSoundBuffer_Release(s_radio.buffer);
        s_radio.buffer = NULL;
    }
    s_radio.created = 0;
    s_radio.playing = 0;
    s_radio.active = 0;
    s_radio.focus_muted = 0;
    s_radio.output_on = 0;
    InterlockedExchange(&s_radio.fmt_ready, 0);
    if (s_radio.cs_init) {
        EnterCriticalSection(&s_radio.cs);
        if (s_radio.ring) { free(s_radio.ring); s_radio.ring = NULL; }
        s_radio.ring_cap = s_radio.ring_head = s_radio.ring_tail = s_radio.ring_count = 0;
        LeaveCriticalSection(&s_radio.cs);
        DeleteCriticalSection(&s_radio.cs);
        s_radio.cs_init = 0;
    }
    TD5_LOG_I(LOG_TAG, "radio sink closed");
}

