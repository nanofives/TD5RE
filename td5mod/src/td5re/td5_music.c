/**
 * td5_music.c -- Pluggable music-backend seam (see td5_music.h).
 *
 * Single chokepoint between the game's music calls and whatever produces sound.
 * The default backend forwards 1:1 to the legacy MCI CD path so behavior is
 * unchanged until a third-party backend is registered via td5_music_set_backend().
 *
 * NO ORIGINAL-BINARY BASIS: port-only architecture seam. No RE-derived
 * constants here -- every track index / volume comes from the existing call
 * sites unchanged.
 */

#include "td5_music.h"
#include "td5_platform.h"   /* td5_plat_cd_*, TD5_LOG_* */

/* Music belongs to the audio subsystem; share the sound tag so its log lines
 * land in the same file as the rest of the audio backend (engine.log). */
#define LOG_TAG "sound"

/* ========================================================================
 * Default backend: legacy MCI CD passthrough (td5_plat_cd_*).
 *
 * This is exactly the behavior the game had before the seam existed: faithful,
 * and silent on modern hardware with no disc in the drive -- which is precisely
 * the gap a registered third-party backend fills.
 * ======================================================================== */

static void default_cd_play(void *user, int track)     { (void)user; td5_plat_cd_play(track); }
static void default_cd_stop(void *user)                { (void)user; td5_plat_cd_stop(); }
static void default_cd_set_volume(void *user, int vol) { (void)user; td5_plat_cd_set_volume(vol); }

static const td5_music_backend k_default_backend = {
    "cd-mci",               /* name */
    (void *)0,              /* user */
    default_cd_play,        /* play */
    default_cd_stop,        /* stop */
    default_cd_set_volume,  /* set_volume */
    (void *)0,              /* set_paused  -> seam ducks via set_volume */
    (void *)0,              /* next */
    (void *)0,              /* now_playing */
    (void *)0               /* tick */
};

/* ========================================================================
 * Seam state
 * ======================================================================== */

static const td5_music_backend *s_backend = &k_default_backend;
static int s_initialized = 0;
static int s_last_volume = -1;   /* last commanded music volume (0..100), -1 = unset */
static int s_paused      = 0;    /* 1 = ducked/paused */

void td5_music_init(void)
{
    if (s_initialized) return;
    s_backend     = &k_default_backend;
    s_last_volume = -1;
    s_paused      = 0;
    s_initialized = 1;
    TD5_LOG_I(LOG_TAG, "music seam initialized (backend='%s')", s_backend->name);
}

void td5_music_shutdown(void)
{
    if (!s_initialized) return;
    if (s_backend && s_backend->stop) s_backend->stop(s_backend->user);
    s_backend     = &k_default_backend;
    s_initialized = 0;
    TD5_LOG_I(LOG_TAG, "music seam shut down");
}

void td5_music_set_backend(const td5_music_backend *backend)
{
    if (!s_initialized) td5_music_init();

    /* Stop whatever is playing on the outgoing backend so a swap never leaves
     * an orphaned stream running. */
    if (s_backend && s_backend->stop) s_backend->stop(s_backend->user);

    s_backend = backend ? backend : &k_default_backend;

    /* Re-apply the last known volume to the incoming backend so a mid-session
     * swap doesn't reset the player's music level. */
    if (s_last_volume >= 0 && s_backend->set_volume)
        s_backend->set_volume(s_backend->user, s_paused ? 0 : s_last_volume);

    TD5_LOG_I(LOG_TAG, "music backend set -> '%s'%s",
              s_backend->name ? s_backend->name : "(unnamed)",
              backend ? "" : " (default)");
}

const td5_music_backend *td5_music_get_backend(void)
{
    if (!s_initialized) td5_music_init();
    return s_backend;
}

/* ========================================================================
 * Game-facing API (forwards to the active backend)
 * ======================================================================== */

void td5_music_play(int track)
{
    if (!s_initialized) td5_music_init();
    s_paused = 0;
    TD5_LOG_I(LOG_TAG, "music play track=%d backend='%s'", track, s_backend->name);
    if (s_backend->play) s_backend->play(s_backend->user, track);
}

void td5_music_stop(void)
{
    if (!s_initialized) td5_music_init();
    if (s_backend->stop) s_backend->stop(s_backend->user);
}

void td5_music_set_volume(int volume)
{
    if (!s_initialized) td5_music_init();
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    s_last_volume = volume;
    /* While ducked, remember the requested level but keep output silent; the
     * resume path re-applies s_last_volume. */
    if (s_paused) return;
    if (s_backend->set_volume) s_backend->set_volume(s_backend->user, volume);
}

void td5_music_set_paused(int paused)
{
    if (!s_initialized) td5_music_init();
    paused = paused ? 1 : 0;
    if (paused == s_paused) return;   /* edge-triggered */
    s_paused = paused;

    if (s_backend->set_paused) {
        s_backend->set_paused(s_backend->user, paused);
    } else if (s_backend->set_volume) {
        /* Fallback: duck to silence on pause, restore last volume on resume.
         * Matches the legacy pause path (td5_plat_cd_set_volume 0 / restore). */
        s_backend->set_volume(s_backend->user,
                              paused ? 0 : (s_last_volume >= 0 ? s_last_volume : 0));
    }
}

void td5_music_next(void)
{
    if (!s_initialized) td5_music_init();
    if (s_backend->next) s_backend->next(s_backend->user);
}

int td5_music_now_playing(char *title, int title_cap, char *artist, int artist_cap)
{
    if (!s_initialized) td5_music_init();
    if (title  && title_cap  > 0) title[0]  = '\0';
    if (artist && artist_cap > 0) artist[0] = '\0';
    if (s_backend->now_playing)
        return s_backend->now_playing(s_backend->user, title, title_cap, artist, artist_cap);
    return 0;
}

void td5_music_tick(void)
{
    if (!s_initialized) return;
    if (s_backend->tick) s_backend->tick(s_backend->user);
}
