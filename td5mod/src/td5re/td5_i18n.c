/* td5_i18n.c -- runtime string localization (PORT-ONLY). See td5_i18n.h.
 *
 * Catalog format (re/assets/frontend/lang/<lang>.txt):
 *   - UTF-8 text, one entry per line:  ENGLISH KEY=TRANSLATED VALUE
 *   - '#' at column 0 starts a comment line; blank lines ignored
 *   - escapes in keys and values: \n (newline), \\ (backslash)
 *   - keys are the EXACT English strings passed to TR() (format specifiers
 *     included); the loader drops any entry whose value's %-conversion
 *     sequence does not match its key's, so a bad translation degrades to
 *     English instead of feeding snprintf a mismatched format
 *   - values are decoded UTF-8 -> Latin-1 at load; codepoints > 0xFF become
 *     '?' (the TTF render path consumes one byte per glyph)
 *
 * Lookup is an FNV-1a open-addressing hash over interned key/value pairs in
 * a single arena. When the language is English the whole layer is a single
 * branch returning the input pointer, so the default build is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "td5_platform.h"   /* TD5_LOG_* */
#include "td5_i18n.h"

#define LOG_TAG "frontend"

#define I18N_HASH_N 2048    /* power of two; ~700 strings -> <40% load */

typedef struct {
    const char *key;        /* NULL = empty slot; points into s_arena */
    const char *val;
} I18nEntry;

typedef struct {
    int         ord;        /* TD5_LANG_* */
    const char *file;       /* catalog filename, NULL = built-in English */
    const char *name;       /* selector display name (Latin-1) */
} I18nLang;

static const I18nLang k_languages[TD5_LANG_COUNT] = {
    { TD5_LANG_ENGLISH, NULL,        "ENGLISH" },
    { TD5_LANG_ES_AR,   "es_AR.txt", "ESPA\xD1OL (AR)" },
};

#define LANG_DIR "re/assets/frontend/lang/"

static int       s_language = TD5_LANG_ENGLISH;
static unsigned  s_generation = 0;
static char     *s_arena = NULL;          /* interned key+value storage */
static I18nEntry s_table[I18N_HASH_N];
static int       s_entry_count = 0;

/* ---- Latin-1 uppercase table ---------------------------------------------
 * Identity except a-z -> A-Z and 0xE0-0xFE -> 0xC0-0xDE, minus 0xF7 (the
 * divide sign, whose "uppercase" 0xD7 is the multiply sign). 0xFF (y-diaeresis)
 * stays: its uppercase is outside Latin-1. */
const unsigned char td5_upper_latin1[256] = {
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
    0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F,
    0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,0x29,0x2A,0x2B,0x2C,0x2D,0x2E,0x2F,
    0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
    0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x5B,0x5C,0x5D,0x5E,0x5F,
    0x60,0x41,0x42,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F,
    0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5A,0x7B,0x7C,0x7D,0x7E,0x7F,
    0x80,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x8B,0x8C,0x8D,0x8E,0x8F,
    0x90,0x91,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0x9B,0x9C,0x9D,0x9E,0x9F,
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF,
    0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,0xBA,0xBB,0xBC,0xBD,0xBE,0xBF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xDF,
    0xC0,0xC1,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xCB,0xCC,0xCD,0xCE,0xCF,
    0xD0,0xD1,0xD2,0xD3,0xD4,0xD5,0xD6,0xF7,0xD8,0xD9,0xDA,0xDB,0xDC,0xDD,0xDE,0xFF,
};

/* ---- helpers ------------------------------------------------------------- */

static uint32_t i18n_hash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

/* Compare the ordered %-conversion sequences of key and value ("%d %s" vs
 * "%s %d" mismatches; %% is a literal). Coarse on purpose: it compares the
 * final conversion characters, which is what keeps snprintf memory-safe. */
static int i18n_fmt_specs_match(const char *key, const char *val)
{
    char ka[16], va[16];
    int kn = 0, vn = 0;
    const char *p;
    for (p = key; *p && kn < 16; p++)
        if (p[0] == '%') {
            if (p[1] == '%') { p++; continue; }
            while (p[1] && strchr("-+ #0123456789.*lhq", p[1])) p++;
            if (p[1]) ka[kn++] = p[1];
        }
    for (p = val; *p && vn < 16; p++)
        if (p[0] == '%') {
            if (p[1] == '%') { p++; continue; }
            while (p[1] && strchr("-+ #0123456789.*lhq", p[1])) p++;
            if (p[1]) va[vn++] = p[1];
        }
    return kn == vn && memcmp(ka, va, (size_t)kn) == 0;
}

/* In-place decode of one catalog field: process \n / \\ escapes and UTF-8 ->
 * Latin-1 (cp > 0xFF -> '?'). Output never exceeds input length. */
static void i18n_decode_field(char *s)
{
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    while (*r) {
        unsigned c = *r;
        if (c == '\\' && (r[1] == 'n' || r[1] == '\\')) {
            *w++ = (r[1] == 'n') ? '\n' : '\\';
            r += 2;
            continue;
        }
        if (c < 0x80) { *w++ = (unsigned char)c; r++; continue; }
        /* UTF-8 lead byte */
        unsigned cp = '?';
        int len = 1;
        if ((c & 0xE0) == 0xC0 && (r[1] & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (r[1] & 0x3F);
            len = 2;
        } else if ((c & 0xF0) == 0xE0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((r[1] & 0x3F) << 6) | (r[2] & 0x3F);
            len = 3;
        } else if ((c & 0xF8) == 0xF0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80 &&
                   (r[3] & 0xC0) == 0x80) {
            cp = 0x10000; /* forced out of Latin-1 range below */
            len = 4;
        }
        *w++ = (cp <= 0xFF) ? (unsigned char)cp : '?';
        r += len;
    }
    *w = '\0';
}

static void i18n_table_insert(const char *key, const char *val)
{
    uint32_t idx = i18n_hash(key) & (I18N_HASH_N - 1);
    for (;;) {
        if (!s_table[idx].key) {
            s_table[idx].key = key;
            s_table[idx].val = val;
            s_entry_count++;
            return;
        }
        if (strcmp(s_table[idx].key, key) == 0) {
            s_table[idx].val = val;   /* later entry wins */
            return;
        }
        idx = (idx + 1) & (I18N_HASH_N - 1);
    }
}

static void i18n_free_catalog(void)
{
    free(s_arena);
    s_arena = NULL;
    memset(s_table, 0, sizeof(s_table));
    s_entry_count = 0;
}

static int i18n_load_catalog(const char *file)
{
    char path[260];
    snprintf(path, sizeof(path), LANG_DIR "%s", file);

    FILE *f = fopen(path, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "i18n: catalog not found: %s", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 0; }
    fclose(f);
    buf[n] = '\0';

    i18n_free_catalog();
    s_arena = buf;   /* keys/values are parsed in place and point into it */

    int dropped = 0, empty = 0;
    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = '\0';

        if (line[0] && line[0] != '#') {
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = '\0';
                char *key = line, *val = eq + 1;
                i18n_decode_field(key);
                i18n_decode_field(val);
                if (!*val) {
                    empty++;              /* untranslated placeholder */
                } else if (!i18n_fmt_specs_match(key, val)) {
                    TD5_LOG_W(LOG_TAG, "i18n: %%-spec mismatch, entry dropped: \"%s\"", key);
                    dropped++;
                } else if (s_entry_count < I18N_HASH_N - 1) {
                    i18n_table_insert(key, val);
                }
            }
        }
        line = next;
    }

    TD5_LOG_I(LOG_TAG, "i18n: loaded %s (%d entries, %d untranslated, %d dropped)",
              path, s_entry_count, empty, dropped);
    return s_entry_count > 0;
}

/* ---- miss logging (dev diagnosis: the coverage report is a selftest screen
 * walk at Language=1 + grep frontend.log for "i18n miss"). Each key logs
 * once per session via a small hash set. ------------------------------- */
#ifndef TD5RE_RELEASE
#define MISS_SEEN_N 1024
static uint32_t s_miss_seen[MISS_SEEN_N];
static void i18n_note_miss(const char *en)
{
    uint32_t h = i18n_hash(en);
    if (!h) h = 1;
    uint32_t idx = h & (MISS_SEEN_N - 1);
    for (int probe = 0; probe < 32; probe++) {
        if (s_miss_seen[idx] == h) return;
        if (!s_miss_seen[idx]) {
            s_miss_seen[idx] = h;
            TD5_LOG_W(LOG_TAG, "i18n miss: \"%s\"", en);
            return;
        }
        idx = (idx + 1) & (MISS_SEEN_N - 1);
    }
}
#endif

/* ---- public -------------------------------------------------------------- */

const char *td5_tr(const char *en)
{
    if (s_language == TD5_LANG_ENGLISH || !en || !en[0])
        return en;
    uint32_t idx = i18n_hash(en) & (I18N_HASH_N - 1);
    while (s_table[idx].key) {
        if (strcmp(s_table[idx].key, en) == 0)
            return s_table[idx].val;
        idx = (idx + 1) & (I18N_HASH_N - 1);
    }
#ifndef TD5RE_RELEASE
    i18n_note_miss(en);
#endif
    return en;
}

int td5_i18n_set_language(int lang)
{
    if (lang < 0 || lang >= TD5_LANG_COUNT)
        lang = TD5_LANG_ENGLISH;

    if (lang == TD5_LANG_ENGLISH) {
        i18n_free_catalog();
        s_language = TD5_LANG_ENGLISH;
        s_generation++;
        return 1;
    }
    if (!i18n_load_catalog(k_languages[lang].file)) {
        i18n_free_catalog();
        s_language = TD5_LANG_ENGLISH;
        s_generation++;
        return 0;
    }
    s_language = lang;
    s_generation++;
    return 1;
}

int td5_i18n_language(void)
{
    return s_language;
}

const char *td5_i18n_language_name(int lang)
{
    if (lang < 0 || lang >= TD5_LANG_COUNT) return "?";
    return k_languages[lang].name;
}

unsigned td5_i18n_generation(void)
{
    return s_generation;
}
