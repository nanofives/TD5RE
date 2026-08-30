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
#include "td5_config.h"  /* td5_env_flag_on (dev fault-injection knob) */

#define LOG_TAG "sound"

/* Used when the INI provides no station. SomaFM is listener-supported and
 * explicitly permits third-party apps to stream its public Icecast mounts. */
#define TD5_RADIO_DEFAULT_URL "http://ice1.somafm.com/beatblender-128-mp3"

/* ========================================================================
 * State
 * ======================================================================== */

static volatile LONG s_stop;        /* worker should exit                  */
static HANDLE        s_thread;      /* decode worker                        */
static int           s_inited;
static int           s_mf_started;
static int           s_volume = 10; /* fixed radio output volume 0..100      */
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
 * Worker fault containment
 *
 * WHY. The stream is handed to Media Foundation as a URL, and MF's network
 * source drives WinHTTP internally, so a long-lived stream spends its life
 * inside Microsoft's HTTP stack. Observed 2026-08-30: an access violation deep
 * in webio.dll (near-null read at +0x2C1) on a stream that had connected ONCE
 * and never reconnected -- i.e. a fault in the MF/WinHTTP read path, not in
 * this file. That is not code we can patch, but it took the whole GAME down,
 * which is the part we own: background music must never be able to kill a race.
 *
 * HOW. MinGW's GCC does not implement MSVC's __try/__except, so the guard is a
 * vectored handler instead. It fires ONLY for an access violation ON THE RADIO
 * WORKER THREAD, and redirects that thread to radio_worker_abort(), which exits
 * it. Everything else -- every other thread, every other exception code -- is
 * passed straight through untouched so the normal crash handler still reports
 * real bugs. The radio then stays silent for the rest of the session.
 *
 * DELIBERATELY NOT DONE in the abort path: no MF calls, no radio critical
 * section, no sink teardown. The faulting thread may hold MF-internal locks,
 * so re-entering MF (or anything that waits on it) could deadlock. We leak the
 * MF objects and the sink on purpose -- the same trade the shutdown join-timeout
 * path already makes, and for the same reason. s_aborted makes that permanent.
 * ======================================================================== */

static volatile LONG s_worker_tid;   /* GetCurrentThreadId of the worker  */
static volatile LONG s_aborted;      /* worker died in MF; never re-enter */
static PVOID         s_veh;

/* Redirect target. Runs on the worker's own stack with a fresh frame, so it is
 * safe to log; it must not touch anything MF might have locked. */
static void radio_worker_abort(void)
{
    InterlockedExchange(&s_aborted, 1);
    InterlockedExchange(&s_stop, 1);
    InterlockedExchange(&s_worker_tid, 0);
    TD5_LOG_E(LOG_TAG, "radio: FAULT inside the Media Foundation / WinHTTP read "
                       "path -- radio thread terminated, game continues (music "
                       "off for this session)");
    _endthreadex(0);
}

static LONG CALLBACK radio_veh(EXCEPTION_POINTERS *ep)
{
    ULONG64 sp;
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    /* Only this thread, only a genuine AV, and only once. */
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    if ((LONG)GetCurrentThreadId() != s_worker_tid || s_aborted)
        return EXCEPTION_CONTINUE_SEARCH;

    /* Enter radio_worker_abort as if it had been CALLed: x64 wants RSP+8
     * 16-byte aligned at function entry, so align down then bias by 8. */
    sp = (ep->ContextRecord->Rsp & ~(ULONG64)15) - 8;
    ep->ContextRecord->Rsp = sp;
    ep->ContextRecord->Rip = (ULONG64)(ULONG_PTR)&radio_worker_abort;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* ========================================================================
 * Decode worker
 * ======================================================================== */

static unsigned __stdcall radio_worker(void *arg)
{
    int backoff = 1000;     /* ms between reconnect attempts, capped at 5s */
    (void)arg;

    InterlockedExchange(&s_worker_tid, (LONG)GetCurrentThreadId());
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

#ifndef TD5RE_RELEASE
    /* Dev-only fault injection, to PROVE the guard above actually catches a
     * worker AV rather than assuming it does. TD5RE_RADIO_FAULT_TEST=1 reads
     * the same near-null address the real webio.dll fault reported (+0x2C1).
     * With the guard working, the game survives and the radio goes silent. */
    if (td5_env_flag_on("TD5RE_RADIO_FAULT_TEST")) {
        /* Address kept in a volatile so the compiler cannot fold it and warn
         * (-Warray-bounds) about the deliberate bad access. */
        static volatile ULONG_PTR s_fault_addr = 0x2C1;
        volatile int *boom = (volatile int *)s_fault_addr;
        TD5_LOG_W(LOG_TAG, "radio: FAULT TEST -- deliberately faulting the worker");
        (void)*boom;
        TD5_LOG_E(LOG_TAG, "radio: FAULT TEST did not fault (unexpected)");
    }
#endif

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

        /* (Re)publish the format to the sink; a format change re-sizes the ring. */
        td5_plat_radio_begin((int)rate, (int)ch, (int)bits);
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
                    /* Only enqueue while the output is actually on (in race +
                     * focused + playing). Otherwise drain the live stream and
                     * discard, so resuming jumps to the current live edge. */
                    if (td5_plat_radio_wants_data()) {
                        DWORD off = 0;
                        while (off < len && !s_stop && td5_plat_radio_wants_data()) {
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

    InterlockedExchange(&s_worker_tid, 0);   /* stop guarding before we unwind */
    CoUninitialize();
    return 0;
}

/* ========================================================================
 * Backend vtable
 * ======================================================================== */

static void radio_play(void *user, int track)
{
    (void)user; (void)track;
    if (!s_inited || s_aborted) return;   /* a faulted radio stays off */
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
    td5_plat_radio_set_playing(0);
}

static void radio_set_volume(void *user, int volume)
{
    /* The radio runs at its own fixed RadioVolume, independent of the music
     * slider -- so the seam's set_volume (called with the music volume at race
     * init) does not blast it. Re-assert RadioVolume. */
    (void)user; (void)volume;
    td5_plat_radio_set_volume(s_volume);
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

void td5_radio_init(const char *stream_url, int volume)
{
    HRESULT hr;
    if (s_inited) return;

    if (!stream_url || !stream_url[0]) stream_url = TD5_RADIO_DEFAULT_URL;
    strncpy(s_url, stream_url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    MultiByteToWideChar(CP_UTF8, 0, s_url, -1, s_wurl,
                        (int)(sizeof(s_wurl) / sizeof(s_wurl[0])));
    radio_make_label(s_url, s_label, (int)sizeof(s_label));

    s_volume = (volume < 0) ? 0 : (volume > 100 ? 100 : volume);

    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    s_mf_started = SUCCEEDED(hr);
    if (!s_mf_started)
        TD5_LOG_E(LOG_TAG, "radio: MFStartup failed hr=0x%08X (radio disabled)", (unsigned)hr);

    td5_plat_radio_open();
    td5_plat_radio_set_volume(s_volume);

    s_backend.name        = "internet-radio";
    s_backend.user        = NULL;
    s_backend.play        = radio_play;
    s_backend.stop        = radio_stop;
    s_backend.set_volume  = radio_set_volume;
    s_backend.set_paused  = NULL;       /* seam ducks via set_volume on pause */
    s_backend.next        = NULL;       /* live stream: nothing to skip to */
    s_backend.now_playing = radio_now_playing;
    s_backend.tick        = radio_tick;

    /* First in the chain, so we see the worker's AV before anything else does.
     * It no-ops for every thread but the worker (see radio_veh). */
    if (!s_veh) s_veh = AddVectoredExceptionHandler(1, radio_veh);

    s_inited = 1;
    TD5_LOG_I(LOG_TAG, "radio: initialized url=%s label=%s vol=%d mf=%d guard=%d",
              s_url, s_label, s_volume, s_mf_started, s_veh ? 1 : 0);
}

void td5_radio_shutdown(void)
{
    int joined = 1;
    if (!s_inited) return;

    InterlockedExchange(&s_stop, 1);

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

    /* s_aborted: the worker died inside MF and may have left MF-internal locks
     * held, so calling MFShutdown() here could hang the exit. Skip teardown for
     * the same reason the join-timeout path does -- the process is going away. */
    if (joined && !s_aborted) {
        td5_plat_radio_close();
        if (s_mf_started) { MFShutdown(); s_mf_started = 0; }
    }
    if (s_veh) { RemoveVectoredExceptionHandler(s_veh); s_veh = NULL; }
    s_inited = 0;
    TD5_LOG_I(LOG_TAG, "radio: shutdown (joined=%d aborted=%d)", joined, (int)s_aborted);
}

const td5_music_backend *td5_radio_get_backend(void)
{
    /* NULL when Media Foundation didn't start -> the seam keeps the default
     * (CD) backend instead of a radio backend that can never produce audio. */
    return (s_inited && s_mf_started) ? &s_backend : NULL;
}

void td5_radio_set_volume_pct(int volume)
{
    s_volume = (volume < 0) ? 0 : (volume > 100 ? 100 : volume);
    td5_plat_radio_set_volume(s_volume);
}

#else  /* !_WIN32 -- radio backend is Win32/Media-Foundation only */

void td5_radio_init(const char *stream_url, int volume) { (void)stream_url; (void)volume; }
void td5_radio_shutdown(void) {}
void td5_radio_set_volume_pct(int volume) { (void)volume; }
const td5_music_backend *td5_radio_get_backend(void) { return 0; }

#endif /* _WIN32 */
