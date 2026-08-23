/**
 * td5_track_registry.c -- runtime registry for custom (user-built) tracks.
 *
 * Parses re/assets/levels/custom_tracks.json (manifest written by
 * re/tools/td5_trackgen.py) into a small static table queried by the frontend
 * (by slot) and the asset loader (by level). Absent / malformed manifest is
 * non-fatal: the game runs with zero custom tracks.
 *
 * Manifest schema:
 *   { "_format":"td5_custom_tracks", "_version":1,
 *     "tracks": [
 *       { "slot":37, "level":40, "name":"TEST OVAL", "circuit":true,
 *         "start_span":0, "finish_span":0, "sky_pitch":0.08, "tga":-1 }, ...
 *     ] }
 */
#include "td5_track_registry.h"
#include "td5_platform.h"
#include "deps/cjson/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "asset"

#define TD5_CUSTOM_TRACK_MANIFEST "re/assets/levels/custom_tracks.json"

typedef struct {
    int   slot;
    int   level;
    char  name[40];
    int   circuit;       /* 1 = circuit (lap), 0 = point-to-point */
    int   start_span;
    int   finish_span;
    float sky_pitch;
    int   tga;           /* preview trak TGA index, or -1 */
} TD5_CustomTrack;

static TD5_CustomTrack s_tracks[TD5_CUSTOM_TRACK_MAX];
static int s_track_count = 0;
static int s_slot_max = TD5_CUSTOM_TRACK_SLOT_BASE;

/* The procedurally generated track lives OUTSIDE the manifest table. It used
 * to take a row via set_auto, which silently failed once a user's
 * custom_tracks.json filled all TD5_CUSTOM_TRACK_MAX rows -- the auto slot
 * then never registered and its level number fell through to an unrelated
 * shipped level (observed: slot 60 loading level023.zip). Dedicated storage
 * removes the contention entirely. */
static TD5_CustomTrack s_auto;
static int s_auto_valid = 0;

/* Read a whole file into a malloc'd, NUL-terminated buffer (for cJSON_Parse).
 * Mirrors td5_assetsrc.c's reader (which is file-static there). */
static char *registry_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    size_t nread;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

static int json_int(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(n)) return (int)n->valuedouble;
    return dflt;
}

static int json_bool(const cJSON *obj, const char *key, int dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n) ? 1 : 0;
    if (cJSON_IsNumber(n)) return n->valuedouble != 0.0 ? 1 : 0;
    return dflt;
}

static float json_float(const cJSON *obj, const char *key, float dflt)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(n)) return (float)n->valuedouble;
    return dflt;
}

int td5_track_registry_init(void)
{
    char *text;
    cJSON *root, *tracks, *item;

    s_track_count = 0;
    s_slot_max = TD5_CUSTOM_TRACK_SLOT_BASE;

    text = registry_read_file(TD5_CUSTOM_TRACK_MANIFEST);
    if (!text) {
        /* No manifest -- perfectly normal; no custom tracks installed. */
        TD5_LOG_I(LOG_TAG, "track registry: no custom_tracks.json (0 custom tracks)");
        return 1;
    }

    root = cJSON_Parse(text);
    free(text);
    if (!root) {
        TD5_LOG_W(LOG_TAG, "track registry: %s failed to parse; ignoring",
                  TD5_CUSTOM_TRACK_MANIFEST);
        return 1;   /* non-fatal */
    }

    tracks = cJSON_GetObjectItemCaseSensitive(root, "tracks");
    if (cJSON_IsArray(tracks)) {
        cJSON_ArrayForEach(item, tracks) {
            const cJSON *name;
            TD5_CustomTrack *t;
            int slot;
            if (!cJSON_IsObject(item)) continue;
            if (s_track_count >= TD5_CUSTOM_TRACK_MAX) {
                TD5_LOG_W(LOG_TAG, "track registry: more than %d custom tracks; "
                          "ignoring the rest", TD5_CUSTOM_TRACK_MAX);
                break;
            }
            slot = json_int(item, "slot", -1);
            if (slot < TD5_CUSTOM_TRACK_SLOT_BASE) {
                TD5_LOG_W(LOG_TAG, "track registry: skipping entry with invalid "
                          "slot %d (must be >= %d)", slot, TD5_CUSTOM_TRACK_SLOT_BASE);
                continue;
            }
            t = &s_tracks[s_track_count++];
            t->slot       = slot;
            t->level      = json_int(item, "level", -1);
            t->circuit    = json_bool(item, "circuit", 1);
            t->start_span = json_int(item, "start_span", 0);
            t->finish_span = json_int(item, "finish_span", 0);
            t->sky_pitch  = json_float(item, "sky_pitch", 0.08f);
            t->tga        = json_int(item, "tga", -1);
            name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(name) && name->valuestring) {
                strncpy(t->name, name->valuestring, sizeof(t->name) - 1);
                t->name[sizeof(t->name) - 1] = '\0';
            } else {
                snprintf(t->name, sizeof(t->name), "CUSTOM TRACK %d", slot);
            }
            if (slot + 1 > s_slot_max) s_slot_max = slot + 1;
            TD5_LOG_I(LOG_TAG, "track registry: slot %d -> level %d '%s' (%s, "
                      "start=%d finish=%d)", t->slot, t->level, t->name,
                      t->circuit ? "circuit" : "p2p", t->start_span, t->finish_span);
        }
    }
    cJSON_Delete(root);
    TD5_LOG_I(LOG_TAG, "track registry: %d custom track(s) loaded", s_track_count);
    return 1;
}

void td5_track_registry_shutdown(void)
{
    s_auto_valid = 0;
    s_track_count = 0;
    s_slot_max = TD5_CUSTOM_TRACK_SLOT_BASE;
}

int td5_track_registry_count(void) { return s_track_count; }
int td5_track_registry_slot_max(void) { return s_slot_max; }

int td5_track_registry_set_auto(int slot, int level, const char *name,
                                int circuit, int start_span, int finish_span)
{
    TD5_CustomTrack *t = &s_auto;

    if (slot < TD5_CUSTOM_TRACK_SLOT_BASE) {
        TD5_LOG_W(LOG_TAG, "track registry: set_auto rejected slot %d "
                  "(must be >= %d)", slot, TD5_CUSTOM_TRACK_SLOT_BASE);
        return 0;
    }
    /* Dedicated storage: never consumes a manifest row, so a full
     * custom_tracks.json cannot stop the auto track registering. Re-calling
     * overwrites in place, which is what a per-race regenerate needs. */
    s_auto_valid = 1;

    t->slot        = slot;
    t->level       = level;
    t->circuit     = circuit ? 1 : 0;
    t->start_span  = start_span;
    t->finish_span = finish_span;
    t->sky_pitch   = 0.08f;
    t->tga         = -1;
    if (name && name[0]) {
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    } else {
        snprintf(t->name, sizeof(t->name), "AUTO TRACK %d", slot);
    }
    if (slot + 1 > s_slot_max) s_slot_max = slot + 1;

    TD5_LOG_I(LOG_TAG, "track registry: set_auto slot %d -> level %d '%s' "
              "(%s, start=%d finish=%d)", t->slot, t->level, t->name,
              t->circuit ? "circuit" : "p2p", t->start_span, t->finish_span);
    return 1;
}

static const TD5_CustomTrack *find_by_slot(int slot)
{
    int i;
    if (s_auto_valid && s_auto.slot == slot) return &s_auto;
    for (i = 0; i < s_track_count; i++)
        if (s_tracks[i].slot == slot) return &s_tracks[i];
    return NULL;
}

static const TD5_CustomTrack *find_by_level(int level)
{
    int i;
    if (s_auto_valid && s_auto.level == level) return &s_auto;
    for (i = 0; i < s_track_count; i++)
        if (s_tracks[i].level == level) return &s_tracks[i];
    return NULL;
}

int td5_track_registry_has_slot(int slot)
{
    return find_by_slot(slot) != NULL;
}

/* FNV-1a over the manifest fields that decide WHICH track loads. Fixed
 * byte-at-a-time order, so the value is identical on every build/arch --
 * it goes on the wire. sky_pitch and tga are deliberately excluded: they are
 * cosmetic and would make two visually-identical tracks fail the net check. */
unsigned int td5_track_registry_fingerprint_for_slot(int slot)
{
    const TD5_CustomTrack *t = find_by_slot(slot);
    unsigned int h = 2166136261u;
    int fields[4];
    int i, b;
    const char *p;

    if (!t) return 0u;

    fields[0] = t->level;
    fields[1] = t->circuit;
    fields[2] = t->start_span;
    fields[3] = t->finish_span;
    for (i = 0; i < 4; i++) {
        for (b = 0; b < 4; b++) {
            h ^= (unsigned int)((fields[i] >> (b * 8)) & 0xFF);
            h *= 16777619u;
        }
    }
    for (p = t->name; *p; p++) {
        h ^= (unsigned int)(unsigned char)*p;
        h *= 16777619u;
    }
    /* Never hand back 0 -- callers use 0 as "no fingerprint". */
    return h ? h : 1u;
}

const char *td5_track_registry_name_for_slot(int slot)
{
    const TD5_CustomTrack *t = find_by_slot(slot);
    return t ? t->name : NULL;
}

int td5_track_registry_level_for_slot(int slot)
{
    const TD5_CustomTrack *t = find_by_slot(slot);
    return t ? t->level : -1;
}

int td5_track_registry_tga_for_slot(int slot)
{
    const TD5_CustomTrack *t = find_by_slot(slot);
    return t ? t->tga : -1;
}

int td5_track_registry_has_level(int level_num)
{
    return find_by_level(level_num) != NULL;
}

int td5_track_registry_is_circuit_for_level(int level_num)
{
    const TD5_CustomTrack *t = find_by_level(level_num);
    return t ? t->circuit : -1;
}

int td5_track_registry_start_span_for_level(int level_num)
{
    const TD5_CustomTrack *t = find_by_level(level_num);
    return t ? t->start_span : -1;
}

int td5_track_registry_finish_span_for_level(int level_num)
{
    const TD5_CustomTrack *t = find_by_level(level_num);
    return t ? t->finish_span : -1;
}

float td5_track_registry_sky_pitch_for_level(int level_num)
{
    const TD5_CustomTrack *t = find_by_level(level_num);
    return t ? t->sky_pitch : -1.0f;
}
