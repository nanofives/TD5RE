/**
 * td5_radio.h -- Internet-radio music backend.
 *
 * A concrete td5_music_backend that streams a live internet radio station
 * (Icecast/SHOUTcast MP3 or AAC, or any URL Media Foundation can open) and
 * plays it through the platform radio PCM sink (td5_plat_radio_*). Decoding
 * runs on a dedicated worker thread so network stalls never block the game's
 * fixed-tick loop.
 *
 * Register it with the music seam at startup:
 *     td5_radio_init("http://ice1.somafm.com/beatblender-128-mp3", 10);
 *     td5_music_set_backend(td5_radio_get_backend());
 *
 * Port-only feature -- NO original-binary RE basis.
 */
#ifndef TD5_RADIO_H
#define TD5_RADIO_H

#include "td5_music.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the radio backend with a stream URL (UTF-8) and a fixed output
 * volume 0-100 (the radio uses its own volume, independent of the music
 * slider). Does NOT connect yet -- the worker connects lazily on the first
 * td5_music_play(). Idempotent. */
void td5_radio_init(const char *stream_url, int volume);

/* Stop the worker (with a bounded join) and close the PCM sink. */
void td5_radio_shutdown(void);

/* Set the radio's fixed output volume 0-100 at runtime (e.g. the pause-menu
 * RADIO slider). Updates the level the backend re-asserts and applies it live.
 * Safe no-op if the radio was never initialised. */
void td5_radio_set_volume_pct(int volume);

/* ------------------------------------------------------------------------
 * Station selection + status (PORT-ONLY, drives the SOUND OPTIONS screen)
 * ------------------------------------------------------------------------ */

/* Expected station-URL format, shown verbatim as the on-screen hint so the
 * player knows what the field parses. */
#define TD5_RADIO_URL_FORMAT "http://host[:port]/mount"

/* Validate a station URL against TD5_RADIO_URL_FORMAT. Returns 1 if it is
 * accepted (http:// or https:// scheme, a non-empty host, no whitespace, fits
 * the 512-byte field), 0 otherwise. Pure check -- changes nothing. */
int td5_radio_url_valid(const char *url);

/* Switch to a different station at runtime. Validates `url`, stops and JOINS
 * the decode worker, re-points it and reconnects if music was playing.
 * Returns 1 on success, 0 if the URL was rejected, the radio never
 * initialised, the worker had already faulted, or the worker would not join
 * (in which case the station is left unchanged rather than mutated from under
 * a live worker). */
int td5_radio_set_url(const char *url);

/* Snapshot of what the radio is doing, for the SOUND OPTIONS details panel.
 *
 * NOTE ON `label`: this is the station's HOST, derived from the URL -- NOT an
 * Icecast/SHOUTcast track title. The stream is handed to Media Foundation as a
 * URL and MF does not surface ICY `StreamTitle` metadata, so per-track
 * title/artist is not available without a separate metadata HTTP client. */
typedef struct td5_radio_status {
    int  inited;        /* radio backend set up (MF started, sink open)    */
    int  connected;     /* worker currently has a live decoded stream      */
    int  aborted;       /* worker faulted in MF; radio off for the session  */
    int  playing;       /* worker thread exists (music started at least once) */
    int  volume;        /* 0..100                                          */
    int  rate;          /* PCM sample rate of the live stream, 0 if none   */
    int  channels;      /* PCM channel count, 0 if none                    */
    int  bits;          /* PCM bits per sample, 0 if none                  */
    char label[128];    /* station host (see NOTE above)                   */
    char url[512];      /* full stream URL                                 */
} td5_radio_status;

/* Fill `out` with the current radio state. Always writes every field (zeroed
 * when the radio was never initialised), so callers need no null checks. */
void td5_radio_get_status(td5_radio_status *out);

/* The backend vtable to hand to td5_music_set_backend(). Valid after init. */
const td5_music_backend *td5_radio_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_RADIO_H */
