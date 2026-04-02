/**
 * td5_png_replace.c - PNG-to-TGA replacement module
 *
 * When the game requests a .TGA file from a ZIP, check for a matching
 * .png in the PNG directory. If found, decode with stb_image and write
 * an uncompressed type-2 TGA (24-bit BGR) into the game's buffer.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "td5_png_replace.h"

/* =========================================================================
 * Configuration
 * ========================================================================= */

static struct {
    int  enabled;
    char png_dir[MAX_PATH];
} s_cfg;

static int s_replace_count = 0;

/* =========================================================================
 * Path utilities
 * ========================================================================= */

static int IsTgaFile(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (_stricmp(dot, ".tga") == 0);
}

/**
 * Map zipPath to subfolder (same logic as AssetOverride / AssetDump).
 * Returns 0 on success, -1 if unknown.
 */
static int ZipToSubfolder(const char *zipPath, char *subfolder, int sf_size) {
    const char *zp = zipPath;
    while (*zp == '\\' || *zp == '/') zp++;

    if (_strnicmp(zp, "level", 5) == 0 && zp[5] >= '0' && zp[5] <= '9') {
        int n = 0;
        while (zp[5 + n] && zp[5 + n] != '.') n++;
        snprintf(subfolder, sf_size, "levels/level%.*s", n, zp + 5);
    }
    else if (_strnicmp(zp, "cars\\", 5) == 0 || _strnicmp(zp, "cars/", 5) == 0) {
        const char *base = zp + 5;
        int n = 0;
        while (base[n] && base[n] != '.') n++;
        snprintf(subfolder, sf_size, "cars/%.*s", n, base);
    }
    else if (_strnicmp(zp, "static.zip", 10) == 0)
        strncpy(subfolder, "static", sf_size);
    else if (_strnicmp(zp, "traffic.zip", 11) == 0)
        strncpy(subfolder, "traffic", sf_size);
    else if (_strnicmp(zp, "environs.zip", 12) == 0)
        strncpy(subfolder, "environs", sf_size);
    else if (_strnicmp(zp, "loading.zip", 11) == 0)
        strncpy(subfolder, "loading", sf_size);
    else if (_strnicmp(zp, "legals.zip", 10) == 0)
        strncpy(subfolder, "legals", sf_size);
    else if (_strnicmp(zp, "Cup.zip", 7) == 0 || _strnicmp(zp, "cup.zip", 7) == 0)
        strncpy(subfolder, "cup", sf_size);
    else if (_strnicmp(zp, "sound\\sound.zip", 15) == 0 ||
             _strnicmp(zp, "sound/sound.zip", 15) == 0)
        strncpy(subfolder, "sound", sf_size);
    else if (_strnicmp(zp, "Front End\\FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/FrontEnd.zip", 22) == 0 ||
             _strnicmp(zp, "Front End\\frontend.zip", 22) == 0 ||
             _strnicmp(zp, "Front End/frontend.zip", 22) == 0)
        strncpy(subfolder, "frontend", sf_size);
    else if (_strnicmp(zp, "Front End\\Extras\\Extras.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Extras/Extras.zip", 27) == 0)
        strncpy(subfolder, "extras", sf_size);
    else if (_strnicmp(zp, "Front End\\Extras\\Mugshots.zip", 29) == 0 ||
             _strnicmp(zp, "Front End/Extras/Mugshots.zip", 29) == 0)
        strncpy(subfolder, "mugshots", sf_size);
    else if (_strnicmp(zp, "Front End\\Sounds\\Sounds.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Sounds/Sounds.zip", 27) == 0)
        strncpy(subfolder, "sounds", sf_size);
    else if (_strnicmp(zp, "Front End\\Tracks\\Tracks.zip", 27) == 0 ||
             _strnicmp(zp, "Front End/Tracks/Tracks.zip", 27) == 0)
        strncpy(subfolder, "tracks", sf_size);
    else
        return -1;

    subfolder[sf_size - 1] = '\0';
    return 0;
}

/**
 * Build PNG path: <png_dir>/<subfolder>/<stem>.png
 * entryName is e.g. "CARSKIN0.TGA" -> stem "CARSKIN0", output "CARSKIN0.png"
 */
static int ResolvePngPath(const char *entryName, const char *zipPath,
                          char *out_path, int out_size) {
    char subfolder[128];
    char stem[128];
    const char *dot;
    int n;

    if (!IsTgaFile(entryName)) return -1;
    if (ZipToSubfolder(zipPath, subfolder, sizeof(subfolder)) != 0) return -1;

    /* Extract stem (filename without extension) */
    dot = strrchr(entryName, '.');
    n = dot ? (int)(dot - entryName) : (int)strlen(entryName);
    if (n >= (int)sizeof(stem)) n = (int)sizeof(stem) - 1;
    memcpy(stem, entryName, n);
    stem[n] = '\0';

    n = snprintf(out_path, out_size, "%s/%s/%s.png", s_cfg.png_dir, subfolder, stem);
    if (n < 0 || n >= out_size) return -1;

    /* Normalize backslashes */
    { char *p = out_path; while (*p) { if (*p == '\\') *p = '/'; p++; } }
    return 0;
}

/* =========================================================================
 * PNG decode → TGA encode
 * ========================================================================= */

/**
 * Read PNG file, decode, write uncompressed type-2 24-bit BGR TGA into destBuf.
 * Returns TGA size or 0 on failure.
 */
static uint32_t DecodePngToTga(const char *png_path, char *destBuf, uint32_t maxSize) {
    FILE *f;
    long png_file_size;
    unsigned char *png_data;
    unsigned char *pixels;
    int w, h, channels;
    uint32_t tga_size;

    f = fopen(png_path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    png_file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    png_data = (unsigned char *)malloc(png_file_size);
    if (!png_data) { fclose(f); return 0; }
    fread(png_data, 1, png_file_size, f);
    fclose(f);

    /* Decode PNG to RGB (3 channels) */
    pixels = stbi_load_from_memory(png_data, (int)png_file_size, &w, &h, &channels, 3);
    free(png_data);
    if (!pixels) return 0;

    /* TGA: 18-byte header + w*h*3 pixel data */
    tga_size = 18 + (uint32_t)(w * h * 3);
    if (tga_size > maxSize) {
        stbi_image_free(pixels);
        return 0;
    }

    /* Write TGA header */
    memset(destBuf, 0, 18);
    destBuf[2]  = 2;                        /* image_type = uncompressed true-color */
    destBuf[12] = (char)(w & 0xFF);         /* width low */
    destBuf[13] = (char)((w >> 8) & 0xFF);  /* width high */
    destBuf[14] = (char)(h & 0xFF);         /* height low */
    destBuf[15] = (char)((h >> 8) & 0xFF);  /* height high */
    destBuf[16] = 24;                       /* bits per pixel */
    destBuf[17] = 0x20;                     /* descriptor: bit 5 = top-to-bottom */

    /* Convert RGB (stb output) to BGR (TGA pixel order) */
    {
        const unsigned char *src = pixels;
        unsigned char *dst = (unsigned char *)destBuf + 18;
        int i, count = w * h;
        for (i = 0; i < count; i++) {
            dst[0] = src[2];   /* B */
            dst[1] = src[1];   /* G */
            dst[2] = src[0];   /* R */
            src += 3;
            dst += 3;
        }
    }

    stbi_image_free(pixels);
    return tga_size;
}

/**
 * Get dimensions of a PNG without full decode. Returns TGA size or 0.
 */
static uint32_t GetPngTgaSize(const char *png_path) {
    FILE *f;
    long png_file_size;
    unsigned char *png_data;
    int w, h, channels;

    f = fopen(png_path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    png_file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    png_data = (unsigned char *)malloc(png_file_size);
    if (!png_data) { fclose(f); return 0; }
    fread(png_data, 1, png_file_size, f);
    fclose(f);

    if (stbi_info_from_memory(png_data, (int)png_file_size, &w, &h, &channels)) {
        free(png_data);
        return 18 + (uint32_t)(w * h * 3);
    }

    free(png_data);
    return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int PngReplace_Init(const char *ini_path) {
    s_cfg.enabled = GetPrivateProfileIntA("PngReplace", "Enable", 0, ini_path);
    GetPrivateProfileStringA("PngReplace", "PngDir", "td5_png_clean",
                             s_cfg.png_dir, sizeof(s_cfg.png_dir), ini_path);

    if (!s_cfg.enabled) return 0;

    {
        char msg[256];
        snprintf(msg, sizeof(msg), "[TD5MOD] PngReplace: enabled, dir=%s\n", s_cfg.png_dir);
        OutputDebugStringA(msg);
    }
    return 1;
}

uint32_t PngReplace_TryReplace(const char *entryName, const char *zipPath,
                                char *destBuf, uint32_t maxSize) {
    char png_path[MAX_PATH];
    uint32_t tga_size;

    if (!s_cfg.enabled) return 0;
    if (!entryName || !zipPath) return 0;
    if (ResolvePngPath(entryName, zipPath, png_path, sizeof(png_path)) != 0)
        return 0;

    tga_size = DecodePngToTga(png_path, destBuf, maxSize);
    if (tga_size > 0) {
        s_replace_count++;
        if ((s_replace_count % 20) == 1) {
            char msg[320];
            snprintf(msg, sizeof(msg), "[TD5MOD] PngReplace: #%d %s -> %s\n",
                     s_replace_count, entryName, png_path);
            OutputDebugStringA(msg);
        }
    }
    return tga_size;
}

uint32_t PngReplace_GetTgaSize(const char *entryName, const char *zipPath) {
    char png_path[MAX_PATH];

    if (!s_cfg.enabled) return 0;
    if (!entryName || !zipPath) return 0;
    if (ResolvePngPath(entryName, zipPath, png_path, sizeof(png_path)) != 0)
        return 0;

    return GetPngTgaSize(png_path);
}

void PngReplace_Shutdown(void) {
    if (s_cfg.enabled && s_replace_count > 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[TD5MOD] PngReplace: %d textures replaced\n",
                 s_replace_count);
        OutputDebugStringA(msg);
    }
}
