/* td5_i18n.h -- runtime string localization (PORT-ONLY). See td5_i18n.c.
 *
 * English-keyed lookup: TR("QUICK RACE") returns the active language's
 * translation, or the input pointer verbatim when the language is English
 * or the key is missing (free fallback, zero-copy). Format strings are
 * looked up BEFORE snprintf: TR("%d LAPS") -> "%d VUELTAS".
 *
 * Catalogs live at re/assets/frontend/lang/<lang>.txt as UTF-8 KEY=VALUE
 * lines; values are decoded to Latin-1 bytes at load so the byte-per-glyph
 * render path works unchanged (all Spanish glyphs fit in Latin-1).
 */
#ifndef TD5_I18N_H
#define TD5_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

/* Language ordinals — the [Game] Language INI value and the LANGUAGE screen
 * selector index. Keep in sync with k_languages[] in td5_i18n.c. */
enum {
    TD5_LANG_ENGLISH = 0,
    TD5_LANG_ES_AR   = 1,
    TD5_LANG_COUNT
};

/* Translate an English UI string. Returns the input pointer when the active
 * language is English or the key is untranslated. Safe on NULL (returns NULL). */
const char *td5_tr(const char *en);
#define TR(s) td5_tr(s)

/* Switch language (loads/frees the catalog). Returns 1 on success; a missing
 * or unparsable catalog leaves the game in English and returns 0. */
int td5_i18n_set_language(int lang);
int td5_i18n_language(void);

/* Display name for the LANGUAGE selector row (already Latin-1). */
const char *td5_i18n_language_name(int lang);

/* Bumped on every successful language switch — screens that cache composed
 * text can compare generations to detect staleness. */
unsigned td5_i18n_generation(void);

/* Latin-1-aware uppercase (a-z and 0xE0-0xFE except 0xF7 divide sign).
 * Replaces C toupper() in the draw/measure paths: locale-independent and
 * maps a-acute -> A-acute etc., which C toupper never does. */
extern const unsigned char td5_upper_latin1[256];
#define TD5_TOUPPER(c) ((int)td5_upper_latin1[(unsigned char)(c)])

#ifdef __cplusplus
}
#endif

#endif /* TD5_I18N_H */
