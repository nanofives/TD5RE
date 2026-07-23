/*
 * td5_customcar.c -- drop-in custom-car discovery (see td5_customcar.h).
 *
 * One-time scan of re/assets/cars/ for folders named "custom_*" that contain a
 * himodel.bin/.dat. Each becomes an extra car slot (index 76+). The frontend
 * fills its s_car_zip_paths[] from this registry (td5_frontend_init); the
 * in-race vehicle loader (td5_asset.c) resolves index>=76 through here.
 */
#include "td5_customcar.h"
#include "td5_platform.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "asset"

static char s_zip[TD5_CUSTOM_CAR_MAX][96];  /* "cars/custom_<name>.zip" */
static int  s_count   = 0;
static int  s_scanned = 0;

/* Reserved suffixes (after the "custom_" prefix) that mark a folder as an
 * example/template shipped for authors to copy, not a playable car. These are
 * skipped by the scan so a template car never appears in the SP/MP car grid. */
static int customcar_is_template(const char *name)
{
    static const char *const reserved[] = {
        "custom_preset", "custom_template", "custom_example", "custom_sample",
    };
    for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++) {
        if (strcmp(name, reserved[i]) == 0) return 1;
    }
    return 0;
}

static int customcar_has_mesh(const char *dir)
{
    char p[300];
    snprintf(p, sizeof(p), "%s/himodel.bin", dir);
    if (td5_plat_file_exists(p)) return 1;
    snprintf(p, sizeof(p), "%s/himodel.dat", dir);
    return td5_plat_file_exists(p);
}

int td5_customcar_init(void)
{
    if (s_scanned) return s_count;
    s_scanned = 1;
    s_count = 0;

    DIR *d = opendir("re/assets/cars");
    if (!d) {
        TD5_LOG_I(LOG_TAG, "custom car scan: re/assets/cars not found");
        return 0;
    }

    /* Gather valid folder names, then sort, so the slot index of a given custom
     * car is identical on every machine (netplay/replay determinism). */
    char names[TD5_CUSTOM_CAR_MAX][64];
    int  n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < TD5_CUSTOM_CAR_MAX) {
        if (strncmp(e->d_name, "custom_", 7) != 0) continue;
        /* The zip->dir resolver (build_extracted_asset_path) strips at the first
         * '.', so a dotted folder name would not resolve -- skip it. */
        if (strchr(e->d_name, '.')) continue;
        /* Skip reserved example/template folders (never a playable car). */
        if (customcar_is_template(e->d_name)) continue;

        char dir[256];
        snprintf(dir, sizeof(dir), "re/assets/cars/%s", e->d_name);
        if (!customcar_has_mesh(dir)) continue;

        strncpy(names[n], e->d_name, sizeof(names[n]) - 1);
        names[n][sizeof(names[n]) - 1] = '\0';
        n++;
    }
    closedir(d);

    /* Stable insertion sort by name (deterministic ordering). */
    for (int i = 1; i < n; i++) {
        char tmp[64];
        strcpy(tmp, names[i]);
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], tmp) > 0) {
            strcpy(names[j + 1], names[j]);
            j--;
        }
        strcpy(names[j + 1], tmp);
    }

    for (int i = 0; i < n; i++) {
        snprintf(s_zip[i], sizeof(s_zip[i]), "cars/%s.zip", names[i]);
        TD5_LOG_I(LOG_TAG, "custom car slot %d: %s", i, s_zip[i]);
    }
    s_count = n;
    TD5_LOG_I(LOG_TAG, "custom car scan: %d found", s_count);
    return s_count;
}

int td5_customcar_count(void)
{
    if (!s_scanned) td5_customcar_init();
    return s_count;
}

const char *td5_customcar_zip_path(int ci)
{
    if (!s_scanned) td5_customcar_init();
    if (ci < 0 || ci >= s_count) return NULL;
    return s_zip[ci];
}
