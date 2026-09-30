/*
 * td5_horns.h -- selectable car-horn catalogue (PORT-ONLY).
 *
 * The stock game has no horn choice: td5_sound_load_vehicle_bank() hardcodes
 * "Horn.wav" into slot voice*3+2, so a racer's horn is whatever its car archive
 * happens to ship. This module turns the horn into a per-local-player setting by
 * publishing a catalogue of selectable horns in three categories:
 *
 *   TD5   - the original Test Drive 5 horns, read from original/cars/<code>.zip.
 *           NOTE the deliberate use of the original/ archives rather than the
 *           usual "cars/<code>.zip" runtime path: re/tools/extract_td6_horns.py
 *           OVERWRITES every re/assets/cars/<code>/horn.wav with a Test Drive 6
 *           horn, so the normal path cannot be trusted to still be a TD5 sound.
 *           Going straight to the original archive is what keeps this tab honest.
 *   TD6   - the Test Drive 6 driver horns already extracted to
 *           re/assets/sound/td6_horns/. Requested as entry "td6_horns/<file>"
 *           with zip "sound.zip", which build_extracted_asset_path() maps onto
 *           that folder with no loader change.
 *   MEME  - drop-in: every *.wav the user drops into horns/memes/ next to the
 *           executable. Nothing ships in the repo; the folder is scanned once.
 *
 * An entry is identified by a stable string id, never an index: ids are what get
 * written into the profile store, so inserting or reordering catalogue entries
 * must not silently repoint someone's saved choice at a different sound. An id
 * that no longer resolves (a deleted meme file) degrades to "no override", i.e.
 * the car's own horn.
 *
 * Horn WAVs are mono 22050 Hz 16-bit PCM in both games, so TD5 and TD6 entries
 * are byte-compatible drop-ins for the same sound slot.
 */
#ifndef TD5_HORNS_H
#define TD5_HORNS_H

/* Catalogue tabs, in display order. */
typedef enum {
    TD5_HORN_CAT_TD5 = 0,
    TD5_HORN_CAT_TD6,
    TD5_HORN_CAT_MEME,
    TD5_HORN_CAT_COUNT
} TD5_HornCat;

/* Max drop-in meme horns picked up from horns/memes/. */
#define TD5_HORN_MEME_MAX 64

/* Folder scanned for drop-in horns, relative to the working directory. */
#define TD5_HORN_MEME_DIR "horns/memes"

/* Longest accepted horn id, including the terminator. Sized to match the
 * profile-store field so an id can be copied between them without truncation. */
#define TD5_HORN_ID_MAX 32

typedef struct {
    char id[TD5_HORN_ID_MAX]; /* stable, persisted: "td5:vip", "td6:ViperHorn", "meme:<file>" */
    char label[24];           /* uppercase display text */
    char wav[128];            /* entry name handed to td5_asset_open_and_read */
    char zip[64];             /* archive/zip path handed alongside it */
} TD5_HornEntry;

/* Scan the drop-in folder once (idempotent). Returns the meme count. The static
 * TD5/TD6 tabs need no scan; every accessor self-initialises, so calling this
 * explicitly at startup only moves the one-time directory walk off the first
 * frontend draw. */
int td5_horns_init(void);

/* Number of entries in `cat`, or 0 if the category is out of range. */
int td5_horns_count(TD5_HornCat cat);

/* Entry `idx` of `cat`, or NULL if out of range. */
const TD5_HornEntry *td5_horns_get(TD5_HornCat cat, int idx);

/* Resolve a persisted id. Returns the entry and, when the out params are
 * non-NULL, its category and index. NULL for an empty, unknown or stale id --
 * callers treat that as "use the car's own horn". */
const TD5_HornEntry *td5_horns_find(const char *id, TD5_HornCat *out_cat, int *out_idx);

/* Display label for a persisted id, or a caller-supplied fallback ("DEFAULT")
 * when the id is empty or no longer resolves. Never returns NULL. */
const char *td5_horns_label_for(const char *id, const char *fallback);

#endif /* TD5_HORNS_H */
