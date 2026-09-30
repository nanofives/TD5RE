/*
 * td5_horns.c -- selectable car-horn catalogue (see td5_horns.h).
 *
 * Two static tabs (TD5, TD6) plus one scanned tab (MEME). The scan is a single
 * directory walk modelled on td5_customcar.c: gather, validate, sort by name,
 * publish. Sorting matters for the same reason it does there -- a given meme
 * file keeps the same list position on every machine and across runs, so the
 * on-screen order does not shuffle when an unrelated file is added.
 */
#include "td5_horns.h"
#include "td5_platform.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "sound"

/* ------------------------------------------------------------------ */
/* Static tabs                                                        */
/* ------------------------------------------------------------------ */

/* Original TD5 horns, pulled straight from the shipped car archives. Labels
 * describe the CHARACTER of the sound rather than naming the donor car: the
 * point of the list is "which horn do I want", and a spread across vehicle
 * classes is what makes the entries tell each other apart by ear. */
static const TD5_HornEntry k_td5[] = {
    { "td5:vip", "SPORTS",   "Horn.wav", "original/cars/vip.zip" },
    { "td5:cob", "ROADSTER", "Horn.wav", "original/cars/cob.zip" },
    { "td5:mus", "MUSCLE",   "Horn.wav", "original/cars/mus.zip" },
    { "td5:69v", "CLASSIC",  "Horn.wav", "original/cars/69v.zip" },
    { "td5:jag", "GRAND",    "Horn.wav", "original/cars/jag.zip" },
    { "td5:van", "TOURER",   "Horn.wav", "original/cars/van.zip" },
    { "td5:pit", "TRUCK",    "Horn.wav", "original/cars/pit.zip" },
    { "td5:cop", "PATROL",   "Horn.wav", "original/cars/cop.zip" },
};

/* TD6 driver horns already extracted to re/assets/sound/td6_horns/. The
 * "sound.zip" companion path is what routes these through the extracted-asset
 * branch of build_extracted_asset_path(). */
static const TD5_HornEntry k_td6[] = {
    { "td6:HornLp2",     "STANDARD", "td6_horns/HornLp2.WAV",     "sound.zip" },
    { "td6:ViperHorn",   "SPORTS",   "td6_horns/ViperHorn.WAV",   "sound.zip" },
    { "td6:AstonHorn",   "GT",       "td6_horns/AstonHorn.WAV",   "sound.zip" },
    { "td6:MoparHorn",   "MUSCLE",   "td6_horns/MoparHorn.WAV",   "sound.zip" },
    { "td6:PontiacHorn", "COUPE",    "td6_horns/PontiacHorn.WAV", "sound.zip" },
    { "td6:TankHorn",    "HEAVY",    "td6_horns/TankHorn.WAV",    "sound.zip" },
};

#define TD5_HORN_TD5_COUNT ((int)(sizeof(k_td5) / sizeof(k_td5[0])))
#define TD5_HORN_TD6_COUNT ((int)(sizeof(k_td6) / sizeof(k_td6[0])))

/* ------------------------------------------------------------------ */
/* Drop-in tab                                                        */
/* ------------------------------------------------------------------ */

static TD5_HornEntry s_meme[TD5_HORN_MEME_MAX];
static int           s_meme_count = 0;
static int           s_scanned    = 0;

/* Longest meme FILENAME that still fits an id of the form "meme:<file>". A
 * longer name is skipped rather than truncated: truncation could make two
 * different files share an id, which would silently repoint a saved choice. */
#define TD5_HORN_MEME_NAME_MAX (TD5_HORN_ID_MAX - 6) /* 32 - strlen("meme:") - 1 */

static int horn_has_wav_ext(const char *name)
{
    size_t n = strlen(name);
    if (n < 5) return 0;
    return _stricmp(name + n - 4, ".wav") == 0;
}

/* Cheap header sniff: a RIFF/WAVE container with room for a format chunk. Keeps
 * a stray text file or a 0-byte placeholder out of the list without paying for
 * a full decode of every candidate at scan time. */
static int horn_looks_like_wav(const char *path)
{
    unsigned char hdr[12];
    size_t got;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got != sizeof(hdr)) return 0;
    return memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0;
}

/* Filename -> display label: drop the extension, uppercase, and turn separators
 * into spaces so "air_horn.wav" reads as "AIR HORN". */
static void horn_label_from_filename(const char *name, char *out, size_t out_size)
{
    size_t n = strlen(name);
    size_t i;
    if (n > 4) n -= 4; /* strip ".wav" */
    if (n > out_size - 1) n = out_size - 1;
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (c == '_' || c == '-' || c == '.') c = ' ';
        else if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[i] = c;
    }
    out[i] = '\0';
}

int td5_horns_init(void)
{
    char names[TD5_HORN_MEME_MAX][TD5_HORN_MEME_NAME_MAX + 1];
    int  n = 0;
    int  i;
    int  skipped_long = 0;
    DIR *d;
    struct dirent *e;

    if (s_scanned) return s_meme_count;
    s_scanned    = 1;
    s_meme_count = 0;

    d = opendir(TD5_HORN_MEME_DIR);
    if (!d) {
        TD5_LOG_I(LOG_TAG, "horn scan: %s not found (no meme horns)", TD5_HORN_MEME_DIR);
        return 0;
    }

    while ((e = readdir(d)) != NULL && n < TD5_HORN_MEME_MAX) {
        char   path[300];
        size_t nl = strlen(e->d_name);

        if (!horn_has_wav_ext(e->d_name)) continue;
        if (nl > TD5_HORN_MEME_NAME_MAX) { skipped_long++; continue; }

        snprintf(path, sizeof(path), "%s/%s", TD5_HORN_MEME_DIR, e->d_name);
        if (!horn_looks_like_wav(path)) {
            TD5_LOG_W(LOG_TAG, "horn scan: skipping %s (not a RIFF/WAVE file)", path);
            continue;
        }

        /* memcpy of an already-bounded length rather than strncpy: the length
         * check above is the real guard, and spelling it this way keeps the
         * compiler from flagging a truncation that cannot happen. */
        memcpy(names[n], e->d_name, nl);
        names[n][nl] = '\0';
        n++;
    }
    closedir(d);

    /* Stable insertion sort by name -- same determinism argument as the custom
     * car scan: a file keeps its list position run to run. */
    for (i = 1; i < n; i++) {
        char tmp[TD5_HORN_MEME_NAME_MAX + 1];
        int  j = i - 1;
        strcpy(tmp, names[i]);
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strcpy(names[j + 1], names[j]);
            j--;
        }
        strcpy(names[j + 1], tmp);
    }

    for (i = 0; i < n; i++) {
        TD5_HornEntry *h  = &s_meme[i];
        size_t         nl = strlen(names[i]);          /* <= TD5_HORN_MEME_NAME_MAX */
        size_t         dl = strlen(TD5_HORN_MEME_DIR);

        /* Both strings are assembled by hand from already-bounded parts rather
         * than with snprintf. The sizes are exact by construction --
         * "meme:" + 26 + NUL == 32 == sizeof(id) -- and writing it this way
         * proves that to the compiler instead of asking it to trust a runtime
         * check it cannot see. */
        memcpy(h->id, "meme:", 5);
        memcpy(h->id + 5, names[i], nl);
        h->id[5 + nl] = '\0';

        horn_label_from_filename(names[i], h->label, sizeof(h->label));

        memcpy(h->wav, TD5_HORN_MEME_DIR, dl);
        h->wav[dl] = '/';
        memcpy(h->wav + dl + 1, names[i], nl);
        h->wav[dl + 1 + nl] = '\0';
        /* Absolute/loose path: the asset loader's loose-file branch picks this
         * up before any archive lookup, so the zip field is only a placeholder. */
        strncpy(h->zip, TD5_HORN_MEME_DIR, sizeof(h->zip) - 1);
        h->zip[sizeof(h->zip) - 1] = '\0';
    }
    s_meme_count = n;

    if (skipped_long > 0) {
        TD5_LOG_W(LOG_TAG, "horn scan: %d file(s) skipped, name longer than %d chars",
                  skipped_long, TD5_HORN_MEME_NAME_MAX);
    }
    TD5_LOG_I(LOG_TAG, "horn scan: %d meme horn(s) in %s", s_meme_count, TD5_HORN_MEME_DIR);
    return s_meme_count;
}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

int td5_horns_count(TD5_HornCat cat)
{
    switch (cat) {
        case TD5_HORN_CAT_TD5:  return TD5_HORN_TD5_COUNT;
        case TD5_HORN_CAT_TD6:  return TD5_HORN_TD6_COUNT;
        case TD5_HORN_CAT_MEME: if (!s_scanned) td5_horns_init(); return s_meme_count;
        default:                return 0;
    }
}

const TD5_HornEntry *td5_horns_get(TD5_HornCat cat, int idx)
{
    if (idx < 0 || idx >= td5_horns_count(cat)) return NULL;
    switch (cat) {
        case TD5_HORN_CAT_TD5:  return &k_td5[idx];
        case TD5_HORN_CAT_TD6:  return &k_td6[idx];
        case TD5_HORN_CAT_MEME: return &s_meme[idx];
        default:                return NULL;
    }
}

const TD5_HornEntry *td5_horns_find(const char *id, TD5_HornCat *out_cat, int *out_idx)
{
    int c;
    if (!id || id[0] == '\0') return NULL;
    for (c = 0; c < TD5_HORN_CAT_COUNT; c++) {
        int n = td5_horns_count((TD5_HornCat)c);
        int i;
        for (i = 0; i < n; i++) {
            const TD5_HornEntry *h = td5_horns_get((TD5_HornCat)c, i);
            if (h && strcmp(h->id, id) == 0) {
                if (out_cat) *out_cat = (TD5_HornCat)c;
                if (out_idx) *out_idx = i;
                return h;
            }
        }
    }
    return NULL;
}

const char *td5_horns_label_for(const char *id, const char *fallback)
{
    const TD5_HornEntry *h = td5_horns_find(id, NULL, NULL);
    if (h) return h->label;
    return fallback ? fallback : "";
}
