/**
 * td5_asset_dump.c - Asset dump module for texture extraction
 *
 * Saves every file loaded via ReadArchiveEntry to disk, organized by
 * archive source. Works independently of rendering backend.
 *
 * Dump directory layout mirrors the asset override structure:
 *   td5_dump/
 *     levels/level001/TEXTURES.DAT
 *     levels/level001/FORWSKY.TGA
 *     static/tpage0.dat
 *     cars/cam/CARSKIN0.TGA
 *     traffic/skin0.tga
 *     frontend/TEXTURE.TGA
 *     ...
 *     manifest.txt          (entryName | zipPath | size | dumpPath)
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <direct.h>   /* _mkdir */
#include <sys/stat.h> /* _stat */

#include "td5_asset_dump.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

static struct {
    int      enabled;
    int      skip_existing;
    char     dump_dir[MAX_PATH];
} s_config;

static FILE *s_manifest = NULL;
static int   s_dump_count = 0;
static int   s_skip_count = 0;

/* =========================================================================
 * Path utilities
 * ========================================================================= */

/**
 * Recursively create directories for a path.
 * path is modified temporarily but restored.
 */
static void MakeDirsForFile(char *filepath) {
    char *p;
    for (p = filepath; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            _mkdir(filepath);
            *p = saved;
        }
    }
}

/**
 * Check if a file exists and has non-zero size.
 */
static int FileExists(const char *path) {
    struct _stat st;
    if (_stat(path, &st) == 0 && st.st_size > 0)
        return 1;
    return 0;
}

/**
 * Map zipPath to subfolder name (same logic as AssetOverride_ResolvePath).
 * Returns pointer to static buffer, or NULL if unknown.
 */
static const char *ZipToSubfolder(const char *zipPath) {
    static char subfolder[128];
    const char *zp = zipPath;

    /* Skip leading path separators */
    while (*zp == '\\' || *zp == '/') zp++;

    if (_strnicmp(zp, "level", 5) == 0 && zp[5] >= '0' && zp[5] <= '9') {
        int n = 0;
        while (zp[5 + n] && zp[5 + n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "levels/level%.*s", n, zp + 5);
    }
    else if (_strnicmp(zp, "cars\\", 5) == 0 || _strnicmp(zp, "cars/", 5) == 0) {
        const char *basename = zp + 5;
        int n = 0;
        while (basename[n] && basename[n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "cars/%.*s", n, basename);
    }
    else if (_strnicmp(zp, "static.zip", 10) == 0)
        strcpy(subfolder, "static");
    else if (_strnicmp(zp, "traffic.zip", 11) == 0)
        strcpy(subfolder, "traffic");
    else if (_strnicmp(zp, "environs.zip", 12) == 0)
        strcpy(subfolder, "environs");
    else if (_strnicmp(zp, "loading.zip", 11) == 0)
        strcpy(subfolder, "loading");
    else if (_strnicmp(zp, "legals.zip", 10) == 0)
        strcpy(subfolder, "legals");
    else if (_strnicmp(zp, "Cup.zip", 7) == 0 || _strnicmp(zp, "cup.zip", 7) == 0)
        strcpy(subfolder, "cup");
    else if (_strnicmp(zp, "sound\\sound.zip", 15) == 0 ||
             _strnicmp(zp, "sound/sound.zip", 15) == 0)
        strcpy(subfolder, "sound");
    else if (_strnicmp(zp, "Front End\\FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End\\frontend.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/frontend.zip", 22) == 0)
        strcpy(subfolder, "frontend");
    else if (_strnicmp(zp, "Front End\\Extras\\Extras.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Extras/Extras.zip", 27) == 0)
        strcpy(subfolder, "extras");
    else if (_strnicmp(zp, "Front End\\Extras\\Mugshots.zip", 29) == 0 ||
             _strnicmp(zp, "Front End/Extras/Mugshots.zip", 29) == 0)
        strcpy(subfolder, "mugshots");
    else if (_strnicmp(zp, "Front End\\Sounds\\Sounds.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Sounds/Sounds.zip", 27) == 0)
        strcpy(subfolder, "sounds");
    else if (_strnicmp(zp, "Front End\\Tracks\\Tracks.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Tracks/Tracks.zip", 27) == 0)
        strcpy(subfolder, "tracks");
    else {
        /* Unknown ZIP — use the zip filename as subfolder */
        const char *name = zp;
        const char *sep;
        int n;
        /* Find last path separator */
        sep = strrchr(zp, '\\');
        if (!sep) sep = strrchr(zp, '/');
        if (sep) name = sep + 1;
        /* Strip .zip extension */
        n = 0;
        while (name[n] && name[n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "_unknown/%.*s", n, name);
    }

    return subfolder;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int AssetDump_Init(const char *ini_path) {
    s_config.enabled = GetPrivateProfileIntA("AssetDump", "Enable", 0, ini_path);
    s_config.skip_existing = GetPrivateProfileIntA("AssetDump", "SkipExisting", 1, ini_path);
    GetPrivateProfileStringA("AssetDump", "DumpDir", "td5_dump",
                             s_config.dump_dir, sizeof(s_config.dump_dir), ini_path);

    if (!s_config.enabled)
        return 0;

    /* Create dump root directory */
    _mkdir(s_config.dump_dir);

    /* Open manifest for appending */
    {
        char manifest_path[MAX_PATH];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.txt", s_config.dump_dir);
        s_manifest = fopen(manifest_path, "a");
        if (s_manifest)
            setbuf(s_manifest, NULL);  /* unbuffered for crash safety */
    }

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "[TD5MOD] AssetDump: enabled, dir=%s skip_existing=%d\n",
                 s_config.dump_dir, s_config.skip_existing);
        OutputDebugStringA(msg);
    }

    return 1;
}

void AssetDump_OnFileRead(const char *entryName, const char *zipPath,
                          const void *data, uint32_t dataSize) {
    char dump_path[MAX_PATH];
    const char *subfolder;
    FILE *f;

    if (!s_config.enabled || !data || dataSize == 0)
        return;
    if (!entryName || !entryName[0] || !zipPath || !zipPath[0])
        return;

    subfolder = ZipToSubfolder(zipPath);
    snprintf(dump_path, sizeof(dump_path), "%s/%s/%s",
             s_config.dump_dir, subfolder, entryName);

    /* Normalize path separators */
    {
        char *p = dump_path;
        while (*p) { if (*p == '\\') *p = '/'; p++; }
    }

    /* Skip if file already exists */
    if (s_config.skip_existing && FileExists(dump_path)) {
        s_skip_count++;
        return;
    }

    /* Create directories */
    MakeDirsForFile(dump_path);

    /* Write file */
    f = fopen(dump_path, "wb");
    if (!f) return;
    fwrite(data, 1, dataSize, f);
    fclose(f);

    s_dump_count++;

    /* Log to manifest */
    if (s_manifest) {
        fprintf(s_manifest, "%s|%s|%u|%s\n", entryName, zipPath, dataSize, dump_path);
    }

    /* Periodic status to debug output */
    if ((s_dump_count % 50) == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[TD5MOD] AssetDump: %d files dumped, %d skipped\n",
                 s_dump_count, s_skip_count);
        OutputDebugStringA(msg);
    }
}

void AssetDump_WriteManifest(void) {
    if (s_manifest) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[TD5MOD] AssetDump: total %d dumped, %d skipped\n",
                 s_dump_count, s_skip_count);
        OutputDebugStringA(msg);
        fprintf(s_manifest, "# TOTAL: %d dumped, %d skipped\n", s_dump_count, s_skip_count);
    }
}

void AssetDump_Shutdown(void) {
    AssetDump_WriteManifest();
    if (s_manifest) {
        fclose(s_manifest);
        s_manifest = NULL;
    }
}
