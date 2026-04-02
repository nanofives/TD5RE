/**
 * png_loader.c - Runtime PNG texture override + dump for D3D11 wrapper
 *
 * Two modes:
 *   1. REPLACE: Load PNG textures with authored 8-bit alpha to replace the
 *      game's 16-bit textures. Identified by CRC32 of full R5G6B5 surface.
 *   2. DUMP: Save every texture that passes through Texture_Load as a PNG.
 *      Filenames encode dimensions + CRC for the replace workflow.
 *
 * Dump-and-replace workflow:
 *   - Run with td5_png/dump_textures.flag present -> dumps all textures
 *   - Edit dumped PNGs to add alpha, fix transparency, etc.
 *   - Run td5_build_index.py to generate index.dat from edited PNGs
 *   - Run normally -> edited PNGs replace originals via CRC match
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <stdint.h>
#include <stdio.h>

/* stb_image: single-header PNG decoder (define implementation in this TU) */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* stb_image_write: single-header PNG encoder (for texture dump) */
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "wrapper.h"

/* ========================================================================
 * Index data structures
 * ======================================================================== */

#define PNG_INDEX_MAGIC "TD5PNG\x01\x00"
#define PNG_INDEX_MAGIC_LEN 8

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t crc32;
    char    *path;          /* Relative to CWD (allocated) */
} PngIndexEntry;

typedef struct {
    int          initialized;
    int          entry_count;
    PngIndexEntry *entries;
} PngIndex;

static PngIndex g_png_index = {0, 0, NULL};

/* ========================================================================
 * PNG texture SRV cache — avoids re-decoding the same PNG every time
 * the game's texture streaming re-loads a texture slot.
 * ======================================================================== */

#define PNG_CACHE_MAX 256

typedef struct {
    uint32_t crc32;         /* Texture CRC (same as index entry) */
    ID3D11ShaderResourceView *srv;
    int      has_alpha;     /* -1=unknown, 0=no, 1=yes */
} PngCacheEntry;

static PngCacheEntry g_png_cache[PNG_CACHE_MAX];
static int g_png_cache_count = 0;

/* ========================================================================
 * CRC32 (same polynomial as zlib)
 * ======================================================================== */

static uint32_t s_crc_table[256];
static int s_crc_table_built = 0;

static void build_crc_table(void)
{
    uint32_t c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++) {
            if (c & 1)
                c = 0xEDB88320UL ^ (c >> 1);
            else
                c = c >> 1;
        }
        s_crc_table[n] = c;
    }
    s_crc_table_built = 1;
}

static uint32_t compute_crc32(const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t crc = 0xFFFFFFFFUL;
    size_t i;
    if (!s_crc_table_built) build_crc_table();
    for (i = 0; i < len; i++)
        crc = s_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

/* ========================================================================
 * Index loading
 * ======================================================================== */

void PngOverride_Init(void)
{
    FILE *f;
    char magic[PNG_INDEX_MAGIC_LEN];
    uint32_t count;
    uint32_t i;

    if (g_png_index.initialized) return;

    f = fopen("td5_png/index.dat", "rb");
    if (!f) {
        WRAPPER_LOG("PngOverride: no index.dat found (PNG overrides disabled)");
        g_png_index.initialized = 1;
        return;
    }

    if (fread(magic, 1, PNG_INDEX_MAGIC_LEN, f) != PNG_INDEX_MAGIC_LEN ||
        memcmp(magic, PNG_INDEX_MAGIC, PNG_INDEX_MAGIC_LEN) != 0) {
        WRAPPER_LOG("PngOverride: bad magic in index.dat");
        fclose(f);
        g_png_index.initialized = 1;
        return;
    }

    if (fread(&count, 4, 1, f) != 1) {
        fclose(f);
        g_png_index.initialized = 1;
        return;
    }

    g_png_index.entries = (PngIndexEntry *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY, count * sizeof(PngIndexEntry));
    if (!g_png_index.entries) {
        fclose(f);
        g_png_index.initialized = 1;
        return;
    }

    for (i = 0; i < count; i++) {
        PngIndexEntry *e = &g_png_index.entries[i];
        uint16_t path_len;

        if (fread(&e->width, 4, 1, f) != 1) break;
        if (fread(&e->height, 4, 1, f) != 1) break;
        if (fread(&e->crc32, 4, 1, f) != 1) break;
        if (fread(&path_len, 2, 1, f) != 1) break;

        e->path = (char *)HeapAlloc(GetProcessHeap(), 0, path_len);
        if (!e->path) break;
        if (fread(e->path, 1, path_len, f) != path_len) break;

        g_png_index.entry_count++;
    }

    fclose(f);
    g_png_index.initialized = 1;
    WRAPPER_LOG("PngOverride: loaded %d entries from index.dat", g_png_index.entry_count);
}

void PngOverride_Shutdown(void)
{
    int i;
    if (g_png_index.entries) {
        for (i = 0; i < g_png_index.entry_count; i++) {
            if (g_png_index.entries[i].path)
                HeapFree(GetProcessHeap(), 0, g_png_index.entries[i].path);
        }
        HeapFree(GetProcessHeap(), 0, g_png_index.entries);
    }
    g_png_index.entries = NULL;
    g_png_index.entry_count = 0;
    g_png_index.initialized = 0;
}

/* ========================================================================
 * Lookup: match source texture by CRC32 + dimensions
 * ======================================================================== */

const char *PngOverride_Lookup(DWORD width, DWORD height,
                               const void *pixel_data, LONG pitch)
{
    uint32_t crc;
    int i;

    if (!g_png_index.initialized || g_png_index.entry_count == 0)
        return NULL;

    /* CRC32 of the full texture surface (R5G6B5, 2 bytes/pixel).
     * If pitch == width*2 (no padding), hash the whole buffer directly.
     * Otherwise, hash row by row skipping pitch padding. */
    {
        DWORD row_bytes = width * 2;
        if ((LONG)row_bytes == pitch) {
            crc = compute_crc32(pixel_data, (size_t)(row_bytes * height));
        } else {
            /* Incremental CRC across rows with padding gaps */
            const unsigned char *row = (const unsigned char *)pixel_data;
            DWORD y;
            crc = 0xFFFFFFFFUL;
            if (!s_crc_table_built) build_crc_table();
            for (y = 0; y < height; y++) {
                size_t j;
                for (j = 0; j < row_bytes; j++)
                    crc = s_crc_table[(crc ^ row[j]) & 0xFF] ^ (crc >> 8);
                row += pitch;
            }
            crc ^= 0xFFFFFFFFUL;
        }
    }

    for (i = 0; i < g_png_index.entry_count; i++) {
        PngIndexEntry *e = &g_png_index.entries[i];
        if (e->width == width && e->height == height && e->crc32 == crc)
            return e->path;
    }
    return NULL;
}

/* ========================================================================
 * PNG texture cache helpers
 * ======================================================================== */

/* Look up cached SRV by CRC32, return with AddRef if found */
static ID3D11ShaderResourceView *PngCache_Lookup(uint32_t crc)
{
    int i;
    for (i = 0; i < g_png_cache_count; i++) {
        if (g_png_cache[i].crc32 == crc && g_png_cache[i].srv) {
            ID3D11ShaderResourceView_AddRef(g_png_cache[i].srv);
            return g_png_cache[i].srv;
        }
    }
    return NULL;
}

/* Store SRV in cache (AddRefs internally) */
static void PngCache_Store(uint32_t crc, ID3D11ShaderResourceView *srv, int has_alpha)
{
    if (g_png_cache_count < PNG_CACHE_MAX) {
        ID3D11ShaderResourceView_AddRef(srv);
        g_png_cache[g_png_cache_count].crc32 = crc;
        g_png_cache[g_png_cache_count].srv = srv;
        g_png_cache[g_png_cache_count].has_alpha = has_alpha;
        g_png_cache_count++;
    }
}

/* Look up cached has_alpha result by CRC32 (-1 = not cached) */
static int PngCache_LookupAlpha(uint32_t crc)
{
    int i;
    for (i = 0; i < g_png_cache_count; i++) {
        if (g_png_cache[i].crc32 == crc)
            return g_png_cache[i].has_alpha;
    }
    return -1;
}

/* ========================================================================
 * Check if a PNG file has authored alpha (any pixel with A != 255)
 * ======================================================================== */

int PngOverride_HasAlpha(const char *path)
{
    FILE *f;
    long file_size;
    unsigned char *file_data;
    int img_w, img_h, img_channels;
    unsigned char *pixels;
    int has_alpha = 0;
    int i;
    uint32_t path_crc;

    /* Check cache first */
    path_crc = compute_crc32(path, strlen(path));
    {
        int cached = PngCache_LookupAlpha(path_crc);
        if (cached >= 0) return cached;
    }

    f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    file_data = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, file_size);
    if (!file_data) { fclose(f); return 0; }

    fread(file_data, 1, file_size, f);
    fclose(f);

    pixels = stbi_load_from_memory(file_data, (int)file_size,
                                    &img_w, &img_h, &img_channels, 4);
    HeapFree(GetProcessHeap(), 0, file_data);
    if (!pixels) return 0;

    for (i = 3; i < img_w * img_h * 4; i += 4) {
        if (pixels[i] != 255) { has_alpha = 1; break; }
    }

    stbi_image_free(pixels);

    /* Cache the result for future calls */
    if (g_png_cache_count < PNG_CACHE_MAX) {
        g_png_cache[g_png_cache_count].crc32 = path_crc;
        g_png_cache[g_png_cache_count].srv = NULL;
        g_png_cache[g_png_cache_count].has_alpha = has_alpha;
        g_png_cache_count++;
    }

    return has_alpha;
}

int PngOverride_WriteToSurface(const char *path, DWORD width, DWORD height,
                               void *pixel_data, LONG pitch)
{
    FILE *f;
    long file_size;
    unsigned char *file_data;
    int img_w, img_h, img_channels;
    unsigned char *pixels;
    DWORD x, y;

    f = fopen(path, "rb");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    file_data = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, file_size);
    if (!file_data) { fclose(f); return 0; }

    fread(file_data, 1, file_size, f);
    fclose(f);

    pixels = stbi_load_from_memory(file_data, (int)file_size,
                                    &img_w, &img_h, &img_channels, 4);
    HeapFree(GetProcessHeap(), 0, file_data);
    if (!pixels) return 0;

    /* Verify dimensions match */
    if ((DWORD)img_w != width || (DWORD)img_h != height) {
        WRAPPER_LOG("PngOverride: size mismatch %s (%dx%d vs %lux%lu)",
                    path, img_w, img_h, (unsigned long)width, (unsigned long)height);
        stbi_image_free(pixels);
        return 0;
    }

    /* Write RGBA pixels as R5G6B5 into the existing surface buffer */
    for (y = 0; y < height; y++) {
        const unsigned char *src_row = pixels + y * width * 4;
        unsigned char *dst_row = (unsigned char *)pixel_data + y * pitch;
        for (x = 0; x < width; x++) {
            uint16_t val = ((src_row[x*4+0] >> 3) << 11)
                         | ((src_row[x*4+1] >> 2) << 5)
                         |  (src_row[x*4+2] >> 3);
            dst_row[x * 2 + 0] = (unsigned char)(val & 0xFF);
            dst_row[x * 2 + 1] = (unsigned char)(val >> 8);
        }
    }

    stbi_image_free(pixels);
    WRAPPER_LOG("PngOverride: wrote %s to surface (%lux%lu R5G6B5)",
                path, (unsigned long)width, (unsigned long)height);
    return 1;
}

ID3D11ShaderResourceView *PngOverride_LoadToTexture(const char *path)
{
    FILE *f;
    long file_size;
    unsigned char *file_data;
    int img_w, img_h, img_channels;
    unsigned char *pixels;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    HRESULT hr;
    uint32_t path_crc;

    if (!g_backend.device) return NULL;

    /* Check cache first (use path CRC as key since same CRC = same texture) */
    path_crc = compute_crc32(path, strlen(path));
    srv = PngCache_Lookup(path_crc);
    if (srv) {
        WRAPPER_LOG("PngOverride: cache hit for %s", path);
        return srv;
    }

    f = fopen(path, "rb");
    if (!f) {
        WRAPPER_LOG("PngOverride: failed to open %s: %s", path, stbi_failure_reason());
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    file_data = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, file_size);
    if (!file_data) { fclose(f); return NULL; }

    fread(file_data, 1, file_size, f);
    fclose(f);

    /* Decode PNG to RGBA */
    pixels = stbi_load_from_memory(file_data, (int)file_size,
                                    &img_w, &img_h, &img_channels, 4);
    HeapFree(GetProcessHeap(), 0, file_data);

    if (!pixels) {
        WRAPPER_LOG("PngOverride: stbi decode failed for %s: %s", path, stbi_failure_reason());
        return NULL;
    }

    /* Create B8G8R8A8 texture -- this function is only called for PNGs with alpha */
    ZeroMemory(&td, sizeof(td));
    td.Width = img_w;
    td.Height = img_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    /* stb_image gives RGBA, but D3D11 wants BGRA for B8G8R8A8_UNORM */
    {
        int i;
        for (i = 0; i < img_w * img_h; i++) {
            unsigned char *p = pixels + i * 4;
            unsigned char tmp = p[0]; p[0] = p[2]; p[2] = tmp;  /* swap R<->B */
        }
    }

    init.pSysMem = pixels;
    init.SysMemPitch = img_w * 4;
    init.SysMemSlicePitch = 0;

    hr = ID3D11Device_CreateTexture2D(g_backend.device, &td, &init, &tex);
    if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateShaderResourceView(g_backend.device,
                                                    (ID3D11Resource *)tex,
                                                    NULL, &srv);
        if (FAILED(hr)) {
            WRAPPER_LOG("PngOverride: CreateShaderResourceView failed hr=0x%08lX", hr);
            srv = NULL;
        }
        /* The SRV holds a ref to the texture, so release our ref */
        ID3D11Texture2D_Release(tex);
    } else {
        WRAPPER_LOG("PngOverride: CreateTexture2D B8G8R8A8 failed hr=0x%08lX", hr);
    }

    stbi_image_free(pixels);
    if (srv) {
        PngCache_Store(path_crc, srv, 1);  /* Alpha PNGs always have alpha */
        WRAPPER_LOG("PngOverride: loaded %s (%dx%d) [cached, total=%d]",
                    path, img_w, img_h, g_png_cache_count);
    }
    return srv;
}

/* ========================================================================
 * Texture Dump — save R5G6B5 surfaces as PNG for the dump-and-replace workflow
 *
 * Enabled when td5_png/dump_textures.flag exists. Creates td5_png/_dump/
 * with PNGs named <W>x<H>_<CRC32>.png. Also writes _dump/manifest.txt
 * mapping CRC → dump path for the index builder script.
 *
 * Each unique CRC is only dumped once per session to avoid thousands of
 * duplicate writes from the texture streaming cache.
 * ======================================================================== */

#define DUMP_DIR "td5_png/_dump"
#define DUMP_FLAG "td5_png/dump_textures.flag"
#define DUMP_MANIFEST "td5_png/_dump/manifest.txt"
#define MAX_DUMP_TRACKED 4096
#define MAX_SOURCE_TRACKED 4096

static int g_dump_enabled = -1;  /* -1 = not checked, 0 = off, 1 = on */
static FILE *g_dump_manifest = NULL;

/* Dump tracking: which CRCs have been written as PNGs this session */
static uint32_t g_dumped_crcs[MAX_DUMP_TRACKED];
static int g_dump_count = 0;

/* Source tracking: CRC → archive source mapping (separate from dump) */
static uint32_t g_source_crcs[MAX_SOURCE_TRACKED];
static char     g_source_names[MAX_SOURCE_TRACKED][256];
static int g_source_count = 0;

static int dump_already_saved(uint32_t crc)
{
    int i;
    for (i = 0; i < g_dump_count; i++) {
        if (g_dumped_crcs[i] == crc) return 1;
    }
    return 0;
}

static void dump_mark_saved(uint32_t crc)
{
    if (g_dump_count < MAX_DUMP_TRACKED)
        g_dumped_crcs[g_dump_count++] = crc;
}

/** Record CRC → source mapping. Updates existing entry if source was unknown. */
static void source_record(uint32_t crc, const char *source)
{
    int i;
    if (!source || !source[0]) return;
    for (i = 0; i < g_source_count; i++) {
        if (g_source_crcs[i] == crc) {
            if (!g_source_names[i][0]) {
                strncpy(g_source_names[i], source, 255);
                g_source_names[i][255] = '\0';
            }
            return;
        }
    }
    if (g_source_count < MAX_SOURCE_TRACKED) {
        g_source_crcs[g_source_count] = crc;
        strncpy(g_source_names[g_source_count], source, 255);
        g_source_names[g_source_count][255] = '\0';
        g_source_count++;
    }
}

/** Look up source for a CRC. */
static const char *source_find(uint32_t crc)
{
    int i;
    for (i = 0; i < g_source_count; i++) {
        if (g_source_crcs[i] == crc && g_source_names[i][0])
            return g_source_names[i];
    }
    return NULL;
}

void PngOverride_DumpTexture(DWORD width, DWORD height,
                             const void *pixel_data, LONG pitch,
                             uint32_t crc)
{
    char path[MAX_PATH];
    unsigned char *rgb;
    DWORD x, y;
    const unsigned char *row;
    DWORD row_bytes;

    /* Compute CRC if not provided */
    if (crc == 0) {
        row_bytes = width * 2;
        if ((LONG)row_bytes == pitch) {
            crc = compute_crc32(pixel_data, (size_t)(row_bytes * height));
        } else {
            const unsigned char *r = (const unsigned char *)pixel_data;
            DWORD i;
            crc = 0xFFFFFFFFUL;
            if (!s_crc_table_built) build_crc_table();
            for (i = 0; i < height; i++) {
                size_t j;
                for (j = 0; j < row_bytes; j++)
                    crc = s_crc_table[(crc ^ r[j]) & 0xFF] ^ (crc >> 8);
                r += pitch;
            }
            crc ^= 0xFFFFFFFFUL;
        }
    }

    /* Lazy init: check if dump flag file exists */
    if (g_dump_enabled < 0) {
        FILE *f = fopen(DUMP_FLAG, "r");
        if (f) {
            fclose(f);
            g_dump_enabled = 1;
            CreateDirectoryA("td5_png", NULL);
            CreateDirectoryA(DUMP_DIR, NULL);
            /* Append to manifest (persists across sessions) */
            g_dump_manifest = fopen(DUMP_MANIFEST, "a");
            WRAPPER_LOG("PngDump: enabled, writing to " DUMP_DIR "/");
        } else {
            g_dump_enabled = 0;
        }
    }

    /* Read fresh source from side-channel (consume to prevent stale data) */
    {
        char fresh[512] = {0};
        if (GetEnvironmentVariableA("TD5_LAST_ARCHIVE", fresh, sizeof(fresh)) > 0) {
            SetEnvironmentVariableA("TD5_LAST_ARCHIVE", NULL);
            source_record(crc, fresh);
        }
    }

    if (!g_dump_enabled) return;
    if (dump_already_saved(crc)) return;

    /* Skip if PNG already exists on disk (from a previous session) */
    snprintf(path, sizeof(path), DUMP_DIR "/%lux%lu_%08X.png",
             (unsigned long)width, (unsigned long)height, crc);
    {
        FILE *existing = fopen(path, "rb");
        if (existing) {
            fclose(existing);
            dump_mark_saved(crc);
            return;
        }
    }

    /* Convert R5G6B5 to RGB888 for PNG output */
    rgb = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, width * height * 3);
    if (!rgb) return;

    row_bytes = width * 2;
    row = (const unsigned char *)pixel_data;
    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint16_t px = row[x * 2] | (row[x * 2 + 1] << 8);
            unsigned char r = ((px >> 11) & 0x1F) * 255 / 31;
            unsigned char g = ((px >> 5) & 0x3F) * 255 / 63;
            unsigned char b = (px & 0x1F) * 255 / 31;
            unsigned int idx = (y * width + x) * 3;
            rgb[idx + 0] = r;
            rgb[idx + 1] = g;
            rgb[idx + 2] = b;
        }
        row += pitch;
    }

    /* path was already set earlier (for the existence check) */
    stbi_write_png(path, (int)width, (int)height, 3, rgb, (int)(width * 3));
    HeapFree(GetProcessHeap(), 0, rgb);

    /* Record in manifest using the persistent CRC→source mapping */
    if (g_dump_manifest) {
        const char *source = source_find(crc);
        fprintf(g_dump_manifest, "%08X %4lu %4lu %s %s\n",
                crc, (unsigned long)width, (unsigned long)height, path,
                (source && source[0]) ? source : "unknown|unknown");
        fflush(g_dump_manifest);
    }

    dump_mark_saved(crc);
    WRAPPER_LOG("PngDump: saved %s", path);
}
