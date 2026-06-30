/**
 * td5_music.h -- Pluggable music-backend seam.
 *
 * The 1999 original played Redbook CD audio off the game disc via MCI
 * (td5_plat_cd_*). On modern hardware there is no disc in the drive, so that
 * path is silent. This module inserts a backend seam between the game's music
 * calls (race start/stop, music-volume, pause-duck, frontend jukebox) and
 * whatever actually produces sound. A third-party music source -- a local-file
 * player, a streaming-service bridge, an external companion app, etc. -- can be
 * registered at runtime via td5_music_set_backend() WITHOUT touching any call
 * site.
 *
 * NO ORIGINAL-BINARY BASIS: this is a port-only architecture seam (third-party
 * music streaming did not exist in TD5). The DEFAULT backend forwards 1:1 to
 * the legacy MCI CD path (td5_plat_cd_*), so with nothing registered the
 * behavior is byte-identical to before this seam existed.
 *
 * ---------------------------------------------------------------------------
 * Wiring a third-party backend (the "hookup point"):
 *
 *     static void my_play(void *user, int track) { ... start a song ... }
 *     static void my_stop(void *user)            { ... stop ... }
 *     static void my_volume(void *user, int v)   { ... 0..100 ... }
 *     static void my_tick(void *user)            { ... refill stream ... }
 *
 *     static const td5_music_backend k_my_backend = {
 *         "spotify-bridge",          // name (for logs)
 *         &my_state,                 // user cookie handed back to callbacks
 *         my_play, my_stop, my_volume,
 *         NULL,                      // set_paused  (NULL => seam ducks volume)
 *         NULL,                      // next        (optional)
 *         NULL,                      // now_playing (optional)
 *         my_tick                    // tick        (optional, per-frame pump)
 *     };
 *
 *     td5_music_set_backend(&k_my_backend);   // from now on the whole game's
 *                                             // music drives your backend.
 *
 * The backend pointer must stay valid until it is replaced or the game shuts
 * down. Pass NULL to td5_music_set_backend() to fall back to the default.
 * ---------------------------------------------------------------------------
 */
#ifndef TD5_MUSIC_H
#define TD5_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* A music backend. Every callback except play/stop/set_volume is OPTIONAL
 * (NULL = "not supported"; the seam no-ops it or falls back). 'user' is an
 * opaque cookie handed back to every callback so a backend can carry state
 * without globals. The seam stores the pointer; it does not copy the struct. */
typedef struct td5_music_backend {
    const char *name;                            /* short id, for logs */
    void       *user;                            /* opaque, passed to callbacks */

    /* Start playing a logical music track. 'track' is the same index the game
     * historically passed to CDPlay (track % 10 + 1 for races, jukebox idx + 2
     * in the Music Test screen). A backend may map it onto a playlist however
     * it likes, or ignore it and play its own queue. */
    void (*play)(void *user, int track);

    /* Stop music playback entirely. */
    void (*stop)(void *user);

    /* Set music volume, 0..100 (already clamped by the seam). */
    void (*set_volume)(void *user, int volume);

    /* OPTIONAL: pause/duck (1) or resume (0) without losing the current track.
     * NULL => the seam falls back to set_volume(0) on pause and
     * set_volume(last_volume) on resume. */
    void (*set_paused)(void *user, int paused);

    /* OPTIONAL: skip to the next track in the backend's own playlist.
     * NULL => no-op. */
    void (*next)(void *user);

    /* OPTIONAL: fill now-playing metadata. Returns 1 if title/artist were
     * written, 0 if nothing is playing / unsupported. Either buffer may be
     * NULL. NULL callback => returns 0. */
    int  (*now_playing)(void *user, char *title, int title_cap,
                        char *artist, int artist_cap);

    /* OPTIONAL: per-frame pump for backends that stream/decode incrementally
     * (e.g. refill a double-buffered stream). NULL => no-op. Called once per
     * frame from the audio tick. */
    void (*tick)(void *user);
} td5_music_backend;

/* Initialise the seam and install the default (legacy CD) backend. Called from
 * td5_sound_init(). Idempotent. */
void td5_music_init(void);

/* Tear down the seam (stops playback, drops the active backend). Called from
 * td5_sound_shutdown(). */
void td5_music_shutdown(void);

/* Register a third-party backend. Stops the outgoing backend first, then
 * re-applies the last commanded volume to the new one. Passing NULL restores
 * the built-in default (legacy CD) backend. The pointer must remain valid
 * until replaced or until td5_music_shutdown(). */
void td5_music_set_backend(const td5_music_backend *backend);

/* The currently active backend (never NULL once the seam is initialised). */
const td5_music_backend *td5_music_get_backend(void);

/* ---- Game-facing music API. These forward to the active backend. The
 * existing td5_sound_* music wrappers now route through these, so the whole
 * game already drives the seam. ---- */
void td5_music_play(int track);
void td5_music_stop(void);
void td5_music_set_volume(int volume);     /* 0..100; remembered for pause/resume + swaps */
void td5_music_set_paused(int paused);     /* pause-duck; restores last volume on resume */
void td5_music_next(void);
int  td5_music_now_playing(char *title, int title_cap, char *artist, int artist_cap);
void td5_music_tick(void);                 /* pumped once per frame from td5_sound_tick */

#ifdef __cplusplus
}
#endif

#endif /* TD5_MUSIC_H */
