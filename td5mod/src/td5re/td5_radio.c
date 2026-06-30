/**
 * td5_radio.c -- Internet-radio music backend (see td5_radio.h).
 *
 * Streams a live internet radio station via Media Foundation (the same decode
 * stack td5_fmv.c uses for FMV) and plays it through the platform radio PCM
 * sink (td5_plat_radio_*). A dedicated worker thread owns the network connect +
 * decode loop so a slow/stalled stream never blocks the game's fixed-tick loop.
 *
 * Flow:
 *   td5_music_play()  -> radio_play()  : start worker (lazy connect) + output on
 *   td5_music_stop()  -> radio_stop()  : output off (worker keeps the stream live)
 *   td5_music_tick()  -> radio_tick()  : pump the DSound sink (main thread)
 *
 * Port-only feature -- NO original-binary RE basis.
 */

#include "td5_radio.h"
#include "td5_platform.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>
#include <mfidl.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <mferror.h>
#include <objbase.h>
#include <process.h>     /* _beginthreadex */
#include <string.h>
#include <stdlib.h>

#define LOG_TAG "sound"

/* Used when the INI provides no station. SomaFM is listener-supported and
 * explicitly permits third-party apps to stream its public Icecast mounts. */
#define TD5_RADIO_DEFAULT_URL "http://ice1.somafm.com/beatblender-128-mp3"

/* ========================================================================
 * State
 * ======================================================================== */

static volatile LONG s_stop;        /* worker should exit                  */
static volatile LONG s_playing;     /* output desired (worker submits PCM)  */
static HANDLE        s_thread;      /* decode worker                        */
static int           s_inited;
static int           s_mf_started;
static char          s_url[512];
static wchar_t       s_wurl[512];
static char          s_label[128];  /* station label for now-playing        */
static td5_music_backend s_backend;

/* ========================================================================
 * Helpers
 * ======================================================================== */

/* Derive a short station label from the URL: strip the scheme, keep the host. */
static void radio_make_label(const char *url, char *out, int cap)
{
    const char *p = url;
    int i = 0;
    if (cap <= 0) return;
    if (!strncmp(p, "http://", 7))  p += 7;
    else if (!strncmp(p, "https://", 8)) p += 8;
    while (*p && *p != '/' && *p != ':' && i < cap - 1)
        out[i++] = *p++;
    out[i] = '\0';
    if (i == 0) {                       /* fallback */
        strncpy(out, "Internet Radio", (size_t)cap - 1);
        out[cap - 1] = '\0';
    }
}

/* ========================================================================
 * Decode worker
 * ======================================================================== */

static unsigned __stdcall radio_worker(void *arg)
{
    int begun   = 0;
    int backoff = 1000;     /* ms between reconnect attempts, capped at 5s */
    (void)arg;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    while (!s_stop) {
        IMFSourceReader *reader = NULL;
        IMFMediaType    *pcm    = NULL;
        IMFMediaType    *cur    = NULL;
        UINT32 rate = 0, ch = 0, bits = 0;
        HRESULT hr;

        hr = MFCreateSourceReaderFromURL(s_wurl, NULL, &reader);
        if (FAILED(hr) || !reader) {
            TD5_LOG_W(LOG_TAG, "radio: open stream failed hr=0x%08X (retry in %dms)",
                      (unsigned)hr, backoff);
            td5_plat_sleep(backoff);
            if (backoff < 5000) backoff *= 2;
            continue;
        }

        /* Ask the reader for decoded PCM on the first audio stream. */
        hr = MFCreateMediaType(&pcm);
        if (SUCCEEDED(hr)) {
            IMFMediaType_SetGUID(pcm, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
            IMFMediaType_SetGUID(pcm, &MF_MT_SUBTYPE,    &MFAudioFormat_PCM);
            hr = IMFSourceReader_SetCurrentMediaType(reader,
                    (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, pcm);
            IMFMediaType_Release(pcm);
            pcm = NULL;
        }
        if (FAILED(hr)) {
            TD5_LOG_W(LOG_TAG, "radio: no decodable audio (PCM) hr=0x%08X", (unsigned)hr);
            IMFSourceReader_Release(reader);
            td5_plat_sleep(backoff);
            if (backoff < 5000) backoff *= 2;
            continue;
        }

        /* Read back the actual PCM format. */
        hr = IMFSourceReader_GetCurrentMediaType(reader,
                (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur);
        if (SUCCEEDED(hr) && cur) {
            IMFAttributes_GetUINT32((IMFAttributes *)cur, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            IMFAttributes_GetUINT32((IMFAttributes *)cur, &MF_MT_AUDIO_NUM_CHANNELS,       &ch);
            IMFAttributes_GetUINT32((IMFAttributes *)cur, &MF_MT_AUDIO_BITS_PER_SAMPLE,    &bits);
            IMFMediaType_Release(cur);
            cur = NULL;
        }
        if (bits == 0) bits = 16;
        if (rate == 0 || ch == 0) {
            TD5_LOG_W(LOG_TAG, "radio: unknown audio format (rate=%u ch=%u)",
                      (unsigned)rate, (unsigned)ch);
            IMFSourceReader_Release(reader);
            td5_plat_sleep(backoff);
            if (backoff < 5000) backoff *= 2;
            continue;
        }

        /* (Re)publish the format to the sink. begun stays set across reconnects;
         * a format change re-sizes the ring. */
        if (td5_plat_radio_begin((int)rate, (int)ch, (int)bits))
            begun = 1;
        TD5_LOG_I(LOG_TAG, "radio: connected '%s' (%u Hz %u ch %u-bit)",
                  s_label, (unsigned)rate, (unsigned)ch, (unsigned)bits);
        backoff = 1000;     /* successful connect resets the backoff */

        /* Decode loop. */
        while (!s_stop) {
            DWORD      stream_index = 0, flags = 0;
            LONGLONG   ts = 0;
            IMFSample *sample = NULL;
            IMFMediaBuffer *buf = NULL;

            hr = IMFSourceReader_ReadSample(reader,
                    (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                    &stream_index, &flags, &ts, &sample);
            if (FAILED(hr)) {
                TD5_LOG_W(LOG_TAG, "radio: ReadSample failed hr=0x%08X", (unsigned)hr);
                if (sample) IMFSample_Release(sample);
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (sample) IMFSample_Release(sample);
                break;
            }
            if (!sample) { td5_plat_sleep(2); continue; }

            if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf)) && buf) {
                BYTE *p = NULL; DWORD len = 0;
                if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len)) && p) {
                    /* Only enqueue while output is on. While stopped we still
                     * drain the live stream (discard) so resuming is current. */
                    if (s_playing) {
                        DWORD off = 0;
                        while (off < len && !s_stop && s_playing) {
                            int k = td5_plat_radio_submit(p + off, (int)(len - off));
                            if (k <= 0) td5_plat_sleep(8);   /* ring full: let pump drain */
                            else        off += (DWORD)k;
                        }
                    }
                    IMFMediaBuffer_Unlock(buf);
                }
                IMFMediaBuffer_Release(buf);
            }
            IMFSample_Release(sample);
        }

        IMFSourceReader_Release(reader);
        if (!s_stop) {
            TD5_LOG_W(LOG_TAG, "radio: stream ended/dropped; reconnecting");
            td5_plat_sleep(1500);
        }
    }

    CoUninitialize();
    return 0;
}

/* ========================================================================
 * Backend vtable
 * ======================================================================== */

static void radio_play(void *user, int track)
{
    (void)user; (void)track;
    if (!s_inited) return;
    InterlockedExchange(&s_playing, 1);
    td5_plat_radio_set_playing(1);
    if (!s_thread) {
        InterlockedExchange(&s_stop, 0);
        s_thread = (HANDLE)_beginthreadex(NULL, 0, radio_worker, NULL, 0, NULL);
        if (!s_thread) TD5_LOG_E(LOG_TAG, "radio: worker thread start failed");
        else           TD5_LOG_I(LOG_TAG, "radio: connecting to %s", s_url);
    }
}

static void radio_stop(void *user)
{
    (void)user;
    InterlockedExchange(&s_playing, 0);
    td5_plat_radio_set_playing(0);
}

static void radio_set_volume(void *user, int volume)
{
    (void)user;
    td5_plat_radio_set_volume(volume);
}

static void radio_tick(void *user)
{
    (void)user;
    td5_plat_radio_pump();
}

static int radio_now_playing(void *user, char *title, int title_cap,
                             char *artist, int artist_cap)
{
    (void)user;
    if (title && title_cap > 0) {
        strncpy(title, s_label, (size_t)title_cap - 1);
        title[title_cap - 1] = '\0';
    }
    if (artist && artist_cap > 0) {
        strncpy(artist, "Internet Radio", (size_t)artist_cap - 1);
        artist[artist_cap - 1] = '\0';
    }
    return 1;
}

/* ========================================================================
 * Public API
 * ======================================================================== */

void td5_radio_init(const char *stream_url)
{
    HRESULT hr;
    if (s_inited) return;

    if (!stream_url || !stream_url[0]) stream_url = TD5_RADIO_DEFAULT_URL;
    strncpy(s_url, stream_url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    MultiByteToWideChar(CP_UTF8, 0, s_url, -1, s_wurl,
                        (int)(sizeof(s_wurl) / sizeof(s_wurl[0])));
    radio_make_label(s_url, s_label, (int)sizeof(s_label));

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    s_mf_started = SUCCEEDED(hr);
    if (!s_mf_started)
        TD5_LOG_E(LOG_TAG, "radio: MFStartup failed hr=0x%08X (radio disabled)", (unsigned)hr);

    td5_plat_radio_open();

    s_backend.name        = "internet-radio";
    s_backend.user        = NULL;
    s_backend.play        = radio_play;
    s_backend.stop        = radio_stop;
    s_backend.set_volume  = radio_set_volume;
    s_backend.set_paused  = NULL;       /* seam ducks via set_volume on pause */
    s_backend.next        = NULL;       /* live stream: nothing to skip to */
    s_backend.now_playing = radio_now_playing;
    s_backend.tick        = radio_tick;

    s_inited = 1;
    TD5_LOG_I(LOG_TAG, "radio: initialized url=%s label=%s mf=%d",
              s_url, s_label, s_mf_started);
}

void td5_radio_shutdown(void)
{
    int joined = 1;
    if (!s_inited) return;

    InterlockedExchange(&s_stop, 1);
    InterlockedExchange(&s_playing, 0);

    if (s_thread) {
        /* The worker may be blocked inside MF (connect / ReadSample). Wait a
         * bounded time, then detach: at shutdown the process is exiting, and a
         * detached worker that only touches the ring must NOT have the ring
         * freed under it -- so on timeout we skip teardown to avoid a UAF. */
        if (WaitForSingleObject(s_thread, 2000) != WAIT_OBJECT_0) {
            TD5_LOG_W(LOG_TAG, "radio: worker join timeout; detaching (leaking sink)");
            joined = 0;
        }
        CloseHandle(s_thread);
        s_thread = NULL;
    }

    if (joined) {
        td5_plat_radio_close();
        if (s_mf_started) { MFShutdown(); s_mf_started = 0; }
    }
    s_inited = 0;
    TD5_LOG_I(LOG_TAG, "radio: shutdown (joined=%d)", joined);
}

const td5_music_backend *td5_radio_get_backend(void)
{
    /* NULL when Media Foundation didn't start -> the seam keeps the default
     * (CD) backend instead of a radio backend that can never produce audio. */
    return (s_inited && s_mf_started) ? &s_backend : NULL;
}

#else  /* !_WIN32 -- radio backend is Win32/Media-Foundation only */

void td5_radio_init(const char *stream_url) { (void)stream_url; }
void td5_radio_shutdown(void) {}
const td5_music_backend *td5_radio_get_backend(void) { return 0; }

#endif /* _WIN32 */
