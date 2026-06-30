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

/* The backend vtable to hand to td5_music_set_backend(). Valid after init. */
const td5_music_backend *td5_radio_get_backend(void);

#ifdef __cplusplus
}
#endif

#endif /* TD5_RADIO_H */
