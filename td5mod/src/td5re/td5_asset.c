/**
 * td5_asset.c -- ZIP archive reading, TGA decoding, mesh parsing, asset loading
 *
 * Translation of the original EXE's asset pipeline to clean C11.
 * The original ZIP reader is at 0x43FC80..0x440A23 and uses a custom streaming
 * DEFLATE decompressor. This source port uses miniz for inflate instead.
 *
 * Key design decisions:
 *   - Loose file override: always check filesystem before ZIP (original behavior)
 *   - Single-header miniz for DEFLATE decompression (no reimplementation)
 *   - TGA decoder matches original DecodeArchiveImageToRgb24 (0x442E00) exactly:
 *     supports types 1, 2, 9, 10 with 8/16/24/32-bit source and BGR24 output
 *   - Case-insensitive filename matching throughout (stricmp)
 *
 * Original globals referenced:
 *   0x4AEE4C  game heap handle
 *   0x4AEE50  game heap alloc total
 *   0x4C3760  64KB I/O buffer
 *   0x4CF97C  current ZIP file handle
 *   0x4CF984  central directory file offset
 *   0x4CF988  local header offset (result of central dir search)
 *   0x47B1D4  read cursor within 64KB buffer
 *   0x4C3764  output destination pointer for decompression
 *   0x47B1DC  expected CRC32 from ZIP header
 *   0x47B1D8  running CRC32 of decompressed data
 */

#include "td5_asset.h"
#include "td5_assetsrc.h" /* pack-on-load editable sources (step 0) */
#include "td5_customcar.h" /* drop-in custom car registry (slots 76+) */
#include "td5_track.h"
#include "td5_track_registry.h" /* custom-track registry: level/finish-span lookups */
#include "td5_platform.h"
#include "td5re.h"
#include "td5_render.h"
#include "td5_hud.h"    /* for TD5_AtlasEntry */
#include "td5_physics.h" /* for td5_physics_load_carparam */
#include "td5_ai.h"      /* for td5_ai_set_traffic_queue */
#include "td5_race_state.h"  /* [LAYERING 2026-07-06] read-only race queries (was td5_game.h) */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

/* ========================================================================
 * DEFLATE Decompression
 *
 * Uses td5_inflate for raw DEFLATE decompression. The implementation in
 * td5_inflate.c provides a self-contained decompressor, or can be
 * configured to bridge to miniz or zlib.
 * ======================================================================== */

#include "td5_inflate.h"
#include "td5_jobs.h"    /* Phase A: parallel texture-page decode */

#define TD5_TINFL_DECOMPRESS(out, out_len, in, in_len) \
    td5_inflate_mem_to_mem(out, out_len, in, in_len)

/* ========================================================================
 * Asset Globals (migrated from td5re_stubs.c — owned by this module)
 * ======================================================================== */

TD5_StaticHedEntry *g_static_hed_entries     = NULL;
int                 g_static_hed_entry_count = 0;
uint8_t            *g_track_environment_config = NULL;
static uint8_t      s_checkpoint_data[96];
static int          s_checkpoint_data_size = 0;

/* Per-page texture-source remap. Default identity; permuted in
 * reverse-direction mode using CHECKPT.NUM page-id pairs.
 *
 * Mirrors the runtime swap performed by the original at
 *   RemapCheckpointOrderForTrackDirection @ 0x0042FD70
 *   SwapIndexedRuntimeEntries             @ 0x0040B530
 * which exchanges entries in the texture-cache header arrays
 * (g_textureCacheRuntimeCount+4 page-pointer table and +0xc per-page descriptor
 * table). The port has no separate cache header — pages upload
 * straight to GPU slots keyed by raw page index. Equivalent effect:
 * when the upload loop emits slot S, source the bytes from page
 * s_page_remap[S]. Identity in forward mode; permuted in reverse so
 * the gantry mesh's baked page-id dereferences the swapped (start
 * ↔ finish) banner texture data. */
static int s_page_remap[1024];
static int s_page_remap_active = 0;

/* Holds the TRAFFIC.BUS payload for the active level. Owned by this TU;
 * freed on the next td5_asset_load_level call. td5_ai keeps a read-only
 * pointer into this buffer — do not free until a new level loads. */
static uint8_t     *s_traffic_queue_buf = NULL;

/* ========================================================================
 * CRC-32 Table (matches original at 0x00475160)
 * ======================================================================== */

static uint32_t s_crc32_table[256];
static int      s_crc32_ready = 0;
static int      s_fallback_texture_uploaded = 0;

#define TD5_FALLBACK_TEXTURE_PAGE 1021

static void crc32_init_table(void)
{
    if (s_crc32_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        s_crc32_table[i] = c;
    }
    s_crc32_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = s_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ========================================================================
 * String Helpers
 * ======================================================================== */

#ifdef _WIN32
#define td5_stricmp _stricmp
#else
#include <strings.h>
#define td5_stricmp strcasecmp
#endif

/** Strip directory prefix from a path, returning pointer to basename. */
static const char *strip_path(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/')
            base = p + 1;
    }
    return base;
}

static int build_extracted_level_path(char *out_path, size_t out_size,
                                      const char *entry_name,
                                      const char *zip_path,
                                      int level_offset)
{
    int level_num;
    const char *base;
    static const char *k_roots[] = {
        "re/assets/levels",
        "re/td5_dump/levels"
    };

    if (!out_path || out_size == 0 || !entry_name || !zip_path)
        return 0;

    base = strip_path(zip_path);
    if (sscanf(base, "level%d.zip", &level_num) != 1)
        return 0;

    level_num += level_offset;
    if (level_num < 0)
        return 0;

    for (int i = 0; i < (int)(sizeof(k_roots) / sizeof(k_roots[0])); i++) {
        int n = snprintf(out_path, out_size, "%s/level%03d/%s",
                         k_roots[i], level_num, entry_name);
        if (n > 0 && (size_t)n < out_size && td5_plat_file_exists(out_path))
            return 1;
    }

    return 0;
}

/* Forward declarations for loose file helpers (defined after Module State) */
static int try_loose_file_size(const char *name);
static int try_loose_file_read(const char *name, void *buf, int max_size);

/**
 * Generic archive-to-extracted-asset path resolver.
 * Maps any archive path (e.g. "cars/vip.zip", "SOUND/SOUND.ZIP",
 * "Front End/frontend.zip") to the corresponding re/assets/ subfolder
 * and checks if the loose file exists there.
 * Returns 1 if resolved path exists, 0 otherwise.
 */
static int build_extracted_asset_path(char *out_path, size_t out_size,
                                      const char *entry_name,
                                      const char *zip_path)
{
    char subfolder[128] = {0};
    const char *zp;
    int n;

    if (!out_path || out_size == 0 || !entry_name || !zip_path)
        return 0;

    zp = zip_path;
    while (*zp == '\\' || *zp == '/') zp++;

    /* levels are handled by build_extracted_level_path — skip here */
    if (_strnicmp(zp, "level", 5) == 0 && zp[5] >= '0' && zp[5] <= '9')
        return 0;

    if (_strnicmp(zp, "cars\\", 5) == 0 || _strnicmp(zp, "cars/", 5) == 0) {
        const char *base = zp + 5;
        n = 0;
        while (base[n] && base[n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "cars/%.*s", n, base);
    }
    else if (_strnicmp(zp, "static.zip", 10) == 0)
        strncpy(subfolder, "static", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "traffic.zip", 11) == 0)
        strncpy(subfolder, "traffic", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "environs.zip", 12) == 0)
        strncpy(subfolder, "environs", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "loading.zip", 11) == 0)
        strncpy(subfolder, "loading", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "legals.zip", 10) == 0)
        strncpy(subfolder, "legals", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "cup.zip", 7) == 0)
        strncpy(subfolder, "cup", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "sound", 5) == 0)
        strncpy(subfolder, "sound", sizeof(subfolder) - 1);
    else if (_strnicmp(zp, "Front", 5) == 0) {
        if (strstr(zp, "rontend") || strstr(zp, "rontEnd"))
            strncpy(subfolder, "frontend", sizeof(subfolder) - 1);
        else if (strstr(zp, "Extras.zip") || strstr(zp, "extras.zip"))
            strncpy(subfolder, "extras", sizeof(subfolder) - 1);
        else if (strstr(zp, "Mugshots") || strstr(zp, "mugshots"))
            strncpy(subfolder, "mugshots", sizeof(subfolder) - 1);
        else if (strstr(zp, "Sounds") || strstr(zp, "sounds"))
            strncpy(subfolder, "sounds", sizeof(subfolder) - 1);
        else if (strstr(zp, "Tracks") || strstr(zp, "tracks"))
            strncpy(subfolder, "tracks", sizeof(subfolder) - 1);
        else
            return 0;
    }
    else
        return 0;

    subfolder[sizeof(subfolder) - 1] = '\0';

    n = snprintf(out_path, out_size, "re/assets/%s/%s", subfolder, entry_name);
    if (n > 0 && (size_t)n < out_size && td5_plat_file_exists(out_path))
        return 1;

    return 0;
}

static int try_loose_file_size(const char *name);
static int try_loose_file_read(const char *name, void *buf, int max_size);

static int try_extracted_asset_file_size(const char *name, const char *zip_path)
{
    char candidate[512];
    if (build_extracted_asset_path(candidate, sizeof(candidate), name, zip_path))
        return try_loose_file_size(candidate);
    return -1;
}

static int try_extracted_asset_file_read(const char *name, const char *zip_path,
                                         void *buf, int max_size)
{
    char candidate[512];
    if (build_extracted_asset_path(candidate, sizeof(candidate), name, zip_path))
        return try_loose_file_read(candidate, buf, max_size);
    return -1;
}

/* Resolve an editable-source file (exact filename, e.g. "strip.json") for the
 * asset addressed by `zip_path`, reusing the same directory logic as the
 * loose/extracted loader steps: level zips -> re/assets/levels/levelNNN/ (with
 * the same level_num+1 probe), car/static/etc. zips -> re/assets/<sub>/. Fills
 * out_path and returns 1 if the source exists on disk, else 0. Consumed by the
 * pack-on-load layer (td5_assetsrc.c). */
int td5_asset_resolve_source_path(const char *source_name, const char *zip_path,
                                  char *out_path, size_t out_size)
{
    if (!source_name || !zip_path || !out_path || out_size == 0)
        return 0;

    /* build_extracted_*_path already check td5_plat_file_exists on the result. */
    for (int offset = 0; offset <= 1; offset++) {
        if (build_extracted_level_path(out_path, out_size, source_name,
                                       zip_path, offset))
            return 1;
    }
    if (build_extracted_asset_path(out_path, out_size, source_name, zip_path))
        return 1;

    return 0;
}

/* ========================================================================
 * Module State
 * ======================================================================== */

#define LOG_TAG "asset"

static int s_initialized = 0;

/* ========================================================================
 * Static Sprite Atlas
 *
 * Parsed from assets/static/static.hed at module init.
 *
 * File layout (little-endian):
 *   int32  page_count    -- number of tpageN.dat pages (e.g. 17)
 *   int32  entry_count   -- number of named sprite entries (e.g. 51)
 *   entry_count × 64-byte TD5_StaticHedEntry records:
 *     char[44]  name          (offset  0, null-terminated)
 *     int32     atlas_x       (offset 44)
 *     int32     atlas_y       (offset 48)
 *     int32     width         (offset 52)
 *     int32     height        (offset 56)
 *     int32     texture_slot  (offset 60, raw tpage index 0..N)
 *   page_count × 16-byte TD5_PageMetadata records:
 *     int32     transparency_flag  (0=opaque/LoadRGBS24, 2=alpha/LoadRGBS32)
 *     int32     image_type         (0=skip tpage load, 1=load tpageN.dat)
 *     int32     source_width       (page pixel width)
 *     int32     source_height      (page pixel height)
 *
 * We map texture_slot → D3D page (STATIC_ATLAS_BASE + texture_slot).
 * Pages with image_type==1 have their tpageN.dat loaded; others are loaded
 * by separate subsystems (car skins, sky, traffic, environment).
 * ======================================================================== */

#define STATIC_ATLAS_MAX   256
#define STATIC_ATLAS_BASE  700   /* first D3D page reserved for static atlas */
#define STATIC_PAGE_META_MAX 32  /* max page metadata entries */

typedef struct {
    char           name[44];
    TD5_AtlasEntry entry;
} S_AtlasRec;

static S_AtlasRec     s_atlas_table[STATIC_ATLAS_MAX];
static int            s_atlas_count = 0;
static TD5_AtlasEntry s_atlas_fallback = {0};   /* returned for unknown names */
static uint8_t        s_static_page_done[32];   /* per-slot: 0=none,1=real,2=placeholder */

/* Per-page metadata from static.hed (parsed after entry table).
 * Controls whether a tpageN.dat should be loaded and what pixel format to use. */
static TD5_PageMetadata s_page_metadata[STATIC_PAGE_META_MAX];
static int              s_page_metadata_count = 0;

/* Chassis sprite UV bounds (from "CHASSIS" static.hed entry).
 * Original stores these at g_chassisBoundsXMin-88 [CONFIRMED @ 0x00443336-0x0044336c].
 * UV = atlas_coord * (1.0 / 256.0) per axis. */
static float s_chassis_uv_x = 0.0f;   /* pos_x / 256 */
static float s_chassis_uv_y = 0.0f;   /* pos_y / 256 */
static float s_chassis_uv_w = 0.0f;   /* width / 256 */
static float s_chassis_uv_h = 0.0f;   /* height / 256 */
static int   s_chassis_page = -1;      /* D3D texture page */

/** Load tpageN.dat (256×256 BGRA32) and upload to GPU.
 *
 * Originally believed to be R5G6B5 256×512 (same byte count), but the
 * original engine normalizes UVs by multiplying by 1/256 for BOTH axes
 * (BuildSpriteQuadTemplate @ 0x432BD0, constant [0x4749D0] = 0.00390625).
 * A 256×512 page would require V normalization by 1/512, not 1/256.
 * Therefore the pages are 256×256 BGRA32 (4 bytes/pixel × 65536 px = 262144 bytes).
 */
static void apply_colorkey(void *pixels, int w, int h, TD5_ColorKeyMode mode);

/* Map static-atlas slot (0..31) to physical tpage file index.
 * The shipping static.zip ships tpageN.dat files indexed by the Nth
 * image_type=1 page in static.hed (NOT by the atlas slot number).
 * Per static.hed HEAD-time layout: slot 4 → tpage0, slot 5 → tpage1,
 * slot 12 → tpage2. */
static int static_tpage_file_index(int slot)
{
    int phys = 0;
    for (int i = 0; i < slot; i++) {
        if (i < s_page_metadata_count && s_page_metadata[i].image_type != 0)
            phys++;
    }
    return phys;
}

static int load_static_r5g6b5_tpage(int slot)
{
    char path[128];
    int w = 256, h = 256, npx = w * h;
    uint8_t *bgra;
    FILE *f;
    int file_idx = static_tpage_file_index(slot);

    snprintf(path, sizeof(path), "re/assets/static/tpage%d.dat", file_idx);
    f = fopen(path, "rb");
    if (!f) return 0;

    bgra = (uint8_t *)malloc((size_t)npx * 4);
    if (!bgra) { fclose(f); return 0; }

    if ((int)fread(bgra, 4, (size_t)npx, f) < npx) {
        free(bgra); fclose(f); return 0;
    }
    fclose(f);

    /* Static tpage .dat files are runtime dumps from UploadRaceTexturePage
     * (0x0040B590).  Two layouts are observed depending on the format_mode
     * the original passed to M2DX:
     *
     * [ARCH-DIVERGENCE: 0x0040B590 UploadRaceTexturePage — D3D3 LoadRGBS24/32
     *  → D3D11 direct upload; L5 promotion sweep 2026-05-21] — Ghidra-
     *  verified orig at 0x0040B590 dispatches to DXD3DTexture::LoadRGBS24
     *  (mode 0) or LoadRGBS32 (mode 1 with blend preset 7 / mode 2 with
     *  blend preset 8), with a pre-pass for format_mode==4 that walks
     *  0x10000 bytes setting byte[-2] to 0/0x80 based on byte[-1]+byte[0]+
     *  byte[1] tally (alpha keying for slot 4 speedo). Then writes the
     *  transparency type at DAT_0048DBAC + slot*4 (0=opaque,1=keyed,2=80%,
     *  3=additive). Port has no DXD3DTexture object — uploads happen via
     *  td5_plat_render_upload_texture into D3D11 SRVs directly; the slot-4
     *  alpha-rebake is preserved at lines 449-465 below, and the
     *  transparency table is preserved via td5_asset_set_page_transparency.
     *  The d3d_exref+0xa5c branch (16-vs-24-bit D3D3 driver caps) is folded
     *  away as the D3D11 backend exposes uniform BGRA8 formats.
     *
     * - "ARGB" layout (format_mode=1 dumps from older builds): every non-zero
     *   pixel has byte[0]=0xFF as the alpha/marker byte.
     *   Channel layout: [0]=A/X, [1]=R, [2]=G, [3]=B.
     *
     * - "GRXB" layout (format_mode=1 dumps from newer builds, e.g.
     *   tpage4.dat): every non-zero pixel has byte[2]=0xFF as the marker.
     *   Channel layout: [0]=G, [1]=R, [2]=X, [3]=B.  This is the layout the
     *   game's slot-4 alpha-fixup at 0x0040B59B then rewrites byte[0] of
     *   into 0x00/0x80 — so byte[0] is overwritten as alpha after fixup, but
     *   the original color byte[0] equals byte[1] for grayscale art (SMOKE,
     *   SPEEDO) where R≈G.
     *
     * - "BGRA" layout: legacy fallback when neither marker pattern holds.
     *
     * Detection is run over only the non-zero pixels — fully transparent
     * pixels (all four bytes 0) would otherwise disqualify the marker. */
    {
        int is_argb = 1;          /* byte[0] == 0xFF marker */
        int is_grxb = 1;          /* byte[2] == 0xFF marker */
        int nonzero_count = 0;
        for (int i = 0; i < npx; i++) {
            uint8_t b0 = bgra[i * 4 + 0];
            uint8_t b1 = bgra[i * 4 + 1];
            uint8_t b2 = bgra[i * 4 + 2];
            uint8_t b3 = bgra[i * 4 + 3];
            if (!b0 && !b1 && !b2 && !b3) continue;   /* transparent */
            nonzero_count++;
            if (b0 != 0xFF) is_argb = 0;
            if (b2 != 0xFF) is_grxb = 0;
        }
        /* GRXB takes precedence when both seem to match (rare): the actual
         * tpage4.dat shipped today is GRXB, never ARGB-with-byte[2]=0xFF. */
        const char *fmt_name = "BGRA";
        if (nonzero_count > 0 && is_grxb) fmt_name = "GRXB";
        else if (nonzero_count > 0 && is_argb) fmt_name = "ARGB";
        else { is_grxb = 0; is_argb = 0; }

        int is_alpha_page = (slot < s_page_metadata_count)
                            ? (s_page_metadata[slot].transparency_flag != 0)
                            : (slot == 4 || slot == 5 || slot == 12);

        for (int i = 0; i < npx; i++) {
            uint8_t r, g, b;
            uint8_t b0 = bgra[i * 4 + 0];
            uint8_t b1 = bgra[i * 4 + 1];
            uint8_t b2 = bgra[i * 4 + 2];
            uint8_t b3 = bgra[i * 4 + 3];
            if (is_grxb) {
                /* tpage4.dat / tpage5.dat / tpage12.dat layout:
                 * byte[2]=0xFF marker; bytes carry G,R,X,B. */
                g = b0;
                r = b1;
                b = b3;
            } else if (is_argb) {
                /* tpage0.dat / older dumps: byte[0]=0xFF, then R,G,B. */
                r = b1;
                g = b2;
                b = b3;
            } else {
                /* Plain BGRA (rare). */
                b = b0;
                g = b1;
                r = b2;
            }
            /* Alpha rule from UploadRaceTexturePage @ 0x0040B59B:
             *   slot 4: byte[1]+byte[2]+byte[3]==0 → alpha=0, else alpha=0x80
             * Apply same 0x80 rule to other format_mode=1 slots (5,12) per
             * is_alpha_page metadata; opaque pages get black colorkey. */
            uint8_t a;
            if (slot == 4) {
                a = ((int)b1 + (int)b2 + (int)b3 == 0) ? 0x00 : 0x80;
            } else if (is_alpha_page) {
                a = (r < 8 && g < 8 && b < 8) ? 0x00 : 0x80;
            } else {
                a = (r < 8 && g < 8 && b < 8) ? 0x00 : 0xFF;
            }
            bgra[i * 4 + 0] = b;
            bgra[i * 4 + 1] = g;
            bgra[i * 4 + 2] = r;
            bgra[i * 4 + 3] = a;
        }

        TD5_LOG_I(LOG_TAG, "static atlas: tpage%d.dat slot=%d format=%s nonzero=%d",
                  file_idx, slot, fmt_name, nonzero_count);
    }

    td5_plat_render_upload_texture(STATIC_ATLAS_BASE + slot, bgra, w, h, 2);
    free(bgra);
    TD5_LOG_D(LOG_TAG, "static atlas: uploaded tpage%d.dat (256x256 BGRA+colorkey) → D3D page %d",
              slot, STATIC_ATLAS_BASE + slot);
    return 1;
}

/** Try loading a static atlas page from re/assets PNG files.
 *  td5_asset_decode_png_rgba32 handles R↔B swap to BGRA internally. */
static int load_static_png_tpage(int slot)
{
    char path[128];
    void *pixels = NULL;
    int w = 0, h = 0;
    int file_idx = static_tpage_file_index(slot);

    snprintf(path, sizeof(path), "re/assets/static/tpage%d.png", file_idx);
    if (td5_asset_decode_png_rgba32(path, &pixels, &w, &h)) {
        /* Slot 12 layout:
         *   y=0..7   = SLIDER (256x8)  — gradient alpha is intentional, keep as-is
         *   y=8..15  = BLACKBOX(0,8 8x8) + BLACKBAR(8,8 8x8) — PNG alpha unreliable,
         *              force opaque so vertex color controls semi-transparency
         *   y=16..31 = SELBOX (256x16)  — gradient alpha is intentional, keep as-is
         *   y=32..255 = PAUSETXT font glyphs on cyan background
         */
        /* Slot 12 PAUSETXT: pristine tpage2.dat from static.zip stores alpha
         * directly in byte0 (ARGB format). The ARGB→RGBA PNG conversion in
         * re/tools recipe already puts the alpha in the PNG's A channel, so
         * no cyan colorkey is needed — it would destroy anti-aliased edges.
         * (The earlier cyan-key path was for the corrupted tpage4/5/12.dat
         * artifacts, which baked cyan-bg into RGB channels.) */
        /* Slot 4 (speedo gauge): original UploadRaceTexturePage @ 0x40B590
         * sets alpha=0x00 for pure-black pixels and alpha=0x80 for all others.
         * The gauge renders as a semi-transparent overlay, not an opaque surface. */
        /* Slot 4 speedo alpha keying (alpha=0x00 for RGB==0, 0x80 otherwise)
         * is now pre-baked into tpage0.png by re/tools/bake_transparency.py —
         * see reference_re_assets_recovery.md. */
        td5_plat_render_upload_texture(STATIC_ATLAS_BASE + slot, pixels, w, h, 2);
        stbi_image_free(pixels);
        TD5_LOG_I(LOG_TAG, "static atlas: loaded PNG %s -> D3D page %d (slot %d)",
                  path, STATIC_ATLAS_BASE + slot, slot);
        return 1;
    }
    return 0;
}

/* Original LoadRaceTexturePages @ 0x00442770: silently skips upload when the
 * static.hed pointer for a slot is zero — no placeholder, no fallback.
 * Port uploads a 1×1 transparent-black texel so the D3D11 slot is valid and
 * draws as invisible rather than crashing or using a stale handle. */
static void upload_atlas_placeholder(int slot)
{
    static const uint8_t k_null[4] = { 0x00, 0x00, 0x00, 0x00 };
    TD5_LOG_W(LOG_TAG, "static atlas: slot %d has no on-disk art — uploading null texel", slot);
    td5_plat_render_upload_texture(STATIC_ATLAS_BASE + slot, k_null, 1, 1, 2);
}

static void td5_asset_init_static_atlas(void)
{
    const char *hed_path = "re/assets/static/static.hed";
    int32_t page_count = 0, entry_count = 0;
    FILE *f;
    int i;

    f = fopen(hed_path, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "static atlas: %s not found", hed_path);
        return;
    }

    if (fread(&page_count,  4, 1, f) != 1 ||
        fread(&entry_count, 4, 1, f) != 1 ||
        entry_count <= 0 || entry_count > 256) {
        TD5_LOG_W(LOG_TAG, "static atlas: bad header (pages=%d entries=%d)",
                  page_count, entry_count);
        fclose(f);
        return;
    }

    for (i = 0; i < entry_count; i++) {
        uint8_t  rec[64];
        int32_t  pos_x, pos_y, w, h, tex_slot;
        S_AtlasRec *ar;

        if (fread(rec, 1, 64, f) != 64) break;

        /* TD5_StaticHedEntry: name[44] at 0, then pos_x/y/w/h/slot at 44..63 */
        if (rec[0] == '\0') continue;           /* skip blank entries */
        memcpy(&pos_x,    rec + 44, 4);
        memcpy(&pos_y,    rec + 48, 4);
        memcpy(&w,        rec + 52, 4);
        memcpy(&h,        rec + 56, 4);
        memcpy(&tex_slot, rec + 60, 4);

        if (tex_slot < 0 || tex_slot >= 32) continue;
        if (s_atlas_count >= STATIC_ATLAS_MAX)  continue;

        ar = &s_atlas_table[s_atlas_count++];
        memcpy(ar->name, rec, 43);
        ar->name[43] = '\0';
        ar->entry.atlas_x      = pos_x;
        ar->entry.atlas_y      = pos_y;
        ar->entry.width        = w;
        ar->entry.height       = h;
        ar->entry.texture_page = STATIC_ATLAS_BASE + tex_slot;
    }

    /* --- Parse per-page metadata [CONFIRMED @ 0x004425D0, 0x004427A9] ---
     * page_count × 16-byte TD5_PageMetadata records follow the entry table.
     * Controls which pages get tpageN.dat loaded and what pixel format. */
    s_page_metadata_count = 0;
    if (page_count > 0 && page_count <= STATIC_PAGE_META_MAX) {
        for (i = 0; i < page_count; i++) {
            uint8_t meta[16];
            if (fread(meta, 1, 16, f) != 16) break;
            memcpy(&s_page_metadata[i].transparency_flag, meta + 0,  4);
            memcpy(&s_page_metadata[i].image_type,        meta + 4,  4);
            memcpy(&s_page_metadata[i].source_width,      meta + 8,  4);
            memcpy(&s_page_metadata[i].source_height,     meta + 12, 4);
            s_page_metadata_count++;
        }
        TD5_LOG_I(LOG_TAG, "static atlas: parsed %d page metadata records", s_page_metadata_count);
    }

    fclose(f);

    /* --- Upload tpage textures using metadata to decide which to load ---
     * Pages with image_type==1 have actual tpageN.dat files.
     * Pages with image_type==0 are loaded by other subsystems (cars, sky, traffic). */
    for (i = 0; i < 32; i++) {
        if (s_static_page_done[i]) continue;

        int should_load = 0;
        if (i < s_page_metadata_count) {
            should_load = (s_page_metadata[i].image_type != 0);
        } else {
            /* No metadata — try loading anyway (backward compat) */
            should_load = 1;
        }

        if (!should_load) continue;

        if (load_static_png_tpage(i)) {
            s_static_page_done[i] = 1;
        } else if (load_static_r5g6b5_tpage(i)) {
            s_static_page_done[i] = 1;
        } else {
            upload_atlas_placeholder(i);
            s_static_page_done[i] = 2;
        }

        /* Register transparency for the static atlas page so the renderer
         * picks the right blend preset at bind time. The static.hed file
         * stores the same type byte as world tpages (0/1/2/3). */
        if (i < s_page_metadata_count) {
            td5_asset_set_page_transparency(STATIC_ATLAS_BASE + i,
                                            s_page_metadata[i].transparency_flag);
        }
    }

    /* --- Look up "CHASSIS" entry for underside/shadow sprite UV bounds
     * [CONFIRMED @ 0x00443324-0x0044336c] --- */
    {
        TD5_AtlasEntry *chassis = td5_asset_find_atlas_entry(NULL, "CHASSIS");
        if (chassis && (chassis->width > 0 || chassis->height > 0)) {
            s_chassis_uv_x = (float)chassis->atlas_x * (1.0f / 256.0f);
            s_chassis_uv_y = (float)chassis->atlas_y * (1.0f / 256.0f);
            s_chassis_uv_w = (float)chassis->width   * (1.0f / 256.0f);
            s_chassis_uv_h = (float)chassis->height  * (1.0f / 256.0f);
            s_chassis_page = chassis->texture_page;
            TD5_LOG_I(LOG_TAG, "chassis sprite: uv=(%.3f,%.3f) size=(%.3f,%.3f) page=%d",
                      s_chassis_uv_x, s_chassis_uv_y, s_chassis_uv_w, s_chassis_uv_h,
                      s_chassis_page);
        }
    }

    TD5_LOG_I(LOG_TAG,
              "static atlas: %d entries, %d page metadata from %s (D3D pages %d..%d)",
              s_atlas_count, s_page_metadata_count, hed_path,
              STATIC_ATLAS_BASE, STATIC_ATLAS_BASE + 31);
}

TD5_AtlasEntry *td5_asset_find_atlas_entry(void *context, const char *name)
{
    int i;
    (void)context;
    if (!name) return &s_atlas_fallback;

    for (i = 0; i < s_atlas_count; i++) {
        if (td5_stricmp(s_atlas_table[i].name, name) == 0)
            return &s_atlas_table[i].entry;
    }
    /* Always return non-NULL: HUD code dereferences without null checks */
    return &s_atlas_fallback;
}

int td5_asset_init(void)
{
    crc32_init_table();
    td5_asset_init_static_atlas();
    s_initialized = 1;
    TD5_LOG_I(LOG_TAG, "asset module initialized");
    return 1;
}

void td5_asset_shutdown(void)
{
    s_initialized = 0;
    TD5_LOG_I(LOG_TAG, "asset module shut down");
}

int td5_asset_static_tpage_is_real(int slot)
{
    if (slot < 0 || slot >= 32) return 0;
    return s_static_page_done[slot] == 1;
}

/* Per-tpage transparency type, indexed by page_id. -1 = unknown.
 * Mirrors the per-page metadata stored at g_textureCacheRuntimeCount[3]+7+i*8 in the
 * original BindRaceTexturePage @ 0x0040B660. */
#define TD5_PAGE_TRANSPARENCY_MAX 1024
static int8_t s_page_transparency[TD5_PAGE_TRANSPARENCY_MAX] = {
    [0 ... (TD5_PAGE_TRANSPARENCY_MAX - 1)] = -1
};

void td5_asset_set_page_transparency(int page_id, int transparency)
{
    if (page_id < 0 || page_id >= TD5_PAGE_TRANSPARENCY_MAX) return;
    s_page_transparency[page_id] = (int8_t)transparency;
}

int td5_asset_get_page_transparency(int page_id)
{
    if (page_id < 0 || page_id >= TD5_PAGE_TRANSPARENCY_MAX) return -1;
    return s_page_transparency[page_id];
}

/* ========================================================================
 * ZIP Central Directory Parser
 *
 * Reimplements ParseZipCentralDirectory (0x43FC80).
 *
 * The original reads the EOCD record, then streams through the central
 * directory byte-by-byte using a 64KB buffer. We simplify by reading the
 * entire central directory into memory (it is typically < 64KB for TD5
 * archives which have at most ~60 entries).
 *
 * [ARCH-DIVERGENCE: streaming -> bulk-read; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x0043FC80: orig walks central-directory entries
 *   through the chunked stream-reader (see ReadTrackStaticDataChunk @
 *   0x0043FB70 and DecompressTrackDataStream @ 0x0043FBC0 -- the same
 *   64KB refill state machine the orig uses for all archive reads).
 *   Port reads the whole central directory into a malloc'd buffer once
 *   then parses linearly, dropping the streaming refill state. EOCD
 *   signature (0x06054B50) and per-record field offsets / sizes are
 *   ZIP-format byte-faithful. */

/** Read a little-endian uint16 from a byte pointer. */
static inline uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

/** Read a little-endian uint32 from a byte pointer. */
static inline uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

/**
 * Locate the End of Central Directory Record in a ZIP file.
 * Searches backwards from the end of the file, up to 64KB + 22 bytes.
 * Returns the file offset of the EOCD, or -1 on failure.
 */
static int64_t find_eocd(TD5_File *f)
{
    int64_t file_size = td5_plat_file_size(f);
    if (file_size < 22) return -1;

    /* Try the most common case: no ZIP comment, EOCD is at end - 22 */
    uint8_t buf[22];
    td5_plat_file_seek(f, file_size - 22, 0);
    if (td5_plat_file_read(f, buf, 22) != 22) return -1;

    if (read_u32(buf) == 0x06054B50) {
        return file_size - 22;
    }

    /* Scan backwards through up to 64KB for the signature */
    int64_t scan_size = file_size;
    if (scan_size > 65557) scan_size = 65557; /* 64KB + 22 + comment */

    uint8_t *scan_buf = (uint8_t *)malloc((size_t)scan_size);
    if (!scan_buf) return -1;

    td5_plat_file_seek(f, file_size - scan_size, 0);
    size_t nread = td5_plat_file_read(f, scan_buf, (size_t)scan_size);

    int64_t eocd_off = -1;
    for (int64_t i = (int64_t)nread - 22; i >= 0; i--) {
        if (scan_buf[i] == 0x50 && read_u32(scan_buf + i) == 0x06054B50) {
            eocd_off = (file_size - scan_size) + i;
            break;
        }
    }

    free(scan_buf);
    return eocd_off;
}

TD5_Archive *td5_asset_open_archive(const char *path)
{
    if (!path) return NULL;

    TD5_File *f = td5_plat_file_open(path, "rb");
    if (!f) {
        TD5_LOG_W(LOG_TAG, "cannot open archive: %s", path);
        return NULL;
    }

    /* Find End of Central Directory */
    int64_t eocd_off = find_eocd(f);
    if (eocd_off < 0) {
        TD5_LOG_E(LOG_TAG, "no EOCD found in: %s", path);
        td5_plat_file_close(f);
        return NULL;
    }

    /* Read EOCD */
    uint8_t eocd[22];
    td5_plat_file_seek(f, eocd_off, 0);
    td5_plat_file_read(f, eocd, 22);

    uint16_t total_entries = read_u16(eocd + 10);
    uint32_t cd_size       = read_u32(eocd + 12);
    uint32_t cd_offset     = read_u32(eocd + 16);
    int64_t file_size      = td5_plat_file_size(f);

    if (file_size < 0) {
        TD5_LOG_E(LOG_TAG, "failed to query archive size: %s", path);
        td5_plat_file_close(f);
        return NULL;
    }

    if (total_entries == 0 || total_entries > TD5_ZIP_MAX_ENTRIES) {
        TD5_LOG_E(LOG_TAG, "invalid entry count %u in: %s",
                  (unsigned)total_entries, path);
        td5_plat_file_close(f);
        return NULL;
    }

    if ((int64_t)cd_offset > file_size ||
        (int64_t)cd_size > file_size ||
        ((int64_t)cd_offset + (int64_t)cd_size) > file_size) {
        TD5_LOG_E(LOG_TAG,
                  "central directory out of range: path=%s offset=%u size=%u file=%lld",
                  path,
                  (unsigned int)cd_offset,
                  (unsigned int)cd_size,
                  (long long)file_size);
        td5_plat_file_close(f);
        return NULL;
    }

    /* Read the entire central directory */
    uint8_t *cd_buf = (uint8_t *)malloc(cd_size);
    if (!cd_buf) {
        td5_plat_file_close(f);
        return NULL;
    }

    td5_plat_file_seek(f, cd_offset, 0);
    size_t cd_read = td5_plat_file_read(f, cd_buf, cd_size);
    td5_plat_file_close(f);

    if (cd_read < cd_size) {
        TD5_LOG_E(LOG_TAG, "truncated central directory in: %s", path);
        free(cd_buf);
        return NULL;
    }

    /* Allocate archive and entries */
    TD5_Archive *arc = (TD5_Archive *)calloc(1, sizeof(TD5_Archive));
    if (!arc) { free(cd_buf); return NULL; }

    arc->entries = (TD5_ZipEntry *)calloc(total_entries, sizeof(TD5_ZipEntry));
    if (!arc->entries) { free(arc); free(cd_buf); return NULL; }

    snprintf(arc->path, sizeof(arc->path), "%s", path);

    /* Parse central directory entries */
    const uint8_t *p = cd_buf;
    const uint8_t *cd_end = cd_buf + cd_size;
    int count = 0;

    for (uint16_t i = 0; i < total_entries && p + 46 <= cd_end; i++) {
        uint32_t sig = read_u32(p);
        if (sig != 0x02014B50) break;

        uint16_t flags       = read_u16(p + 8);
        uint16_t method      = read_u16(p + 10);
        uint32_t crc         = read_u32(p + 16);
        uint32_t comp_size   = read_u32(p + 20);
        uint32_t uncomp_size = read_u32(p + 24);
        uint16_t name_len    = read_u16(p + 28);
        uint16_t extra_len   = read_u16(p + 30);
        uint16_t comment_len = read_u16(p + 32);
        uint32_t local_off   = read_u32(p + 42);
        uint8_t  ext_attrs   = p[38]; /* external file attributes low byte */

        p += 46; /* advance past fixed-size fields */

        /* Match original filter: skip encrypted or entries with unusual attrs */
        int skip = 0;
        if (flags & 1) skip = 1;                        /* encrypted */
        if (ext_attrs != 0x00 && ext_attrs != 0x80 &&
            ext_attrs != 0x20) skip = 1;                /* not normal */
        if (comp_size == 0 && uncomp_size == 0) skip = 1; /* directory */

        if (!skip && name_len > 0 && name_len < TD5_ZIP_MAX_FILENAME &&
            p + name_len <= cd_end)
        {
            TD5_ZipEntry *e = &arc->entries[count];

            /* Copy filename, stripping any directory prefix (original behavior) */
            char raw_name[TD5_ZIP_MAX_FILENAME];
            memcpy(raw_name, p, name_len);
            raw_name[name_len] = '\0';

            const char *base = strip_path(raw_name);
            snprintf(e->name, sizeof(e->name), "%s", base);

            e->compressed_size    = comp_size;
            e->uncompressed_size  = uncomp_size;
            e->local_header_offset = local_off;
            e->compression_method = method;
            e->crc32              = crc;
            count++;
        }

        p += name_len + extra_len + comment_len;
    }

    arc->entry_count = count;
    free(cd_buf);

    TD5_LOG_D(LOG_TAG, "opened archive: %s (%d entries)", path, count);
    return arc;
}

void td5_asset_close_archive(TD5_Archive *arc)
{
    if (!arc) return;
    if (arc->entries) free(arc->entries);
    free(arc);
}

/* ========================================================================
 * ZIP Entry Lookup
 * ======================================================================== */

/**
 * Find an entry by basename (case-insensitive).
 * Returns the entry pointer or NULL.
 */
static TD5_ZipEntry *find_entry(TD5_Archive *arc, const char *name)
{
    if (!arc || !name) return NULL;

    const char *base = strip_path(name);

    for (int i = 0; i < arc->entry_count; i++) {
        if (td5_stricmp(arc->entries[i].name, base) == 0) {
            return &arc->entries[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * ZIP Entry Decompression
 *
 * Reimplements DecompressZipEntry (0x4405B0).
 *
 * Reads the local file header, then either copies (stored) or inflates
 * (deflate) the entry data.
 *
 * [ARCH-DIVERGENCE: streaming inflate -> td5_inflate_mem_to_mem; L5 sweep 2026-05-21]
 *   Ghidra-verified 0x004405B0: orig reads the local file header via the
 *   chunked stream-reader, then either memcpys "stored" entries through
 *   the 64KB refill buffer or feeds compressed bytes to its streaming
 *   inflate state machine (ReadTrackStaticDataChunk + DecompressTrack-
 *   DataStream consume one chunk at a time). Port reads the full local
 *   header + compressed payload into memory and decompresses via
 *   td5_inflate_mem_to_mem (zlib-backed when TD5_INFLATE_USE_ZLIB is set,
 *   own implementation otherwise). ZIP local-header field offsets,
 *   compression-method codes (0=stored, 8=deflate) and CRC32 checks are
 *   ZIP-format byte-faithful. */

/**
 * Decompress a single ZIP entry given an open file handle seeked to the
 * local file header. Returns uncompressed size, or 0 on failure.
 */
static int decompress_entry(TD5_File *f, TD5_ZipEntry *entry,
                            void *out_buf, int out_buf_size)
{
    int64_t file_size;
    int64_t data_offset;
    int64_t data_end;

    if (!f || !entry || !out_buf || out_buf_size <= 0) {
        TD5_LOG_E(LOG_TAG, "decompress_entry invalid args: f=%p entry=%p out=%p size=%d",
                  (void *)f, (void *)entry, out_buf, out_buf_size);
        return 0;
    }

    file_size = td5_plat_file_size(f);
    if (file_size <= 0) {
        TD5_LOG_E(LOG_TAG, "decompress_entry invalid file size for %s", entry->name);
        return 0;
    }

    /* Read local file header (30 bytes) */
    uint8_t lhdr[30];
    if (td5_plat_file_read(f, lhdr, 30) != 30) return 0;

    uint32_t sig = read_u32(lhdr);
    if (sig != 0x04034B50) {
        TD5_LOG_E(LOG_TAG, "bad local header signature: 0x%08X", sig);
        return 0;
    }

    uint16_t method     = read_u16(lhdr + 8);
    uint32_t exp_crc    = read_u32(lhdr + 14);
    uint32_t comp_sz    = read_u32(lhdr + 18);
    uint32_t uncomp_sz  = read_u32(lhdr + 22);
    uint16_t fname_len  = read_u16(lhdr + 26);
    uint16_t extra_len  = read_u16(lhdr + 28);

    /* Use central directory sizes if local header has zeros (data descriptor) */
    if (comp_sz == 0)   comp_sz   = entry->compressed_size;
    if (uncomp_sz == 0) uncomp_sz = entry->uncompressed_size;
    if (exp_crc == 0)   exp_crc   = entry->crc32;

    /* Skip filename and extra field */
    if (td5_plat_file_seek(f, (int64_t)(fname_len + extra_len), 1) != 0) {
        TD5_LOG_E(LOG_TAG, "failed to skip local header payload: %s", entry->name);
        return 0;
    }

    data_offset = td5_plat_file_tell(f);
    if (data_offset < 0) {
        TD5_LOG_E(LOG_TAG, "failed to query data offset: %s", entry->name);
        return 0;
    }
    data_end = data_offset + (int64_t)comp_sz;
    if (data_end > file_size) {
        TD5_LOG_E(LOG_TAG,
                  "entry overruns archive: %s data_offset=%lld comp=%u file=%lld",
                  entry->name,
                  (long long)data_offset,
                  (unsigned int)comp_sz,
                  (long long)file_size);
        return 0;
    }

    if ((int)uncomp_sz > out_buf_size) {
        TD5_LOG_E(LOG_TAG, "output buffer too small: need %u, have %d",
                  uncomp_sz, out_buf_size);
        return 0;
    }

    if (method == 0) {
        /* STORED -- direct copy */
        size_t nread = td5_plat_file_read(f, out_buf, uncomp_sz);
        if (nread != uncomp_sz) {
            TD5_LOG_E(LOG_TAG, "stored read short: got %u, expected %u",
                      (unsigned)nread, uncomp_sz);
            return 0;
        }

        /* Verify CRC32 (original does this for stored entries) */
        uint32_t actual_crc = crc32_update(0, (const uint8_t *)out_buf,
                                           uncomp_sz);
        if (actual_crc != exp_crc) {
            TD5_LOG_W(LOG_TAG, "CRC32 mismatch (stored): expected 0x%08X, "
                      "got 0x%08X", exp_crc, actual_crc);
            /* Continue anyway -- the original does too in some paths */
        }

        return (int)uncomp_sz;
    }
    else if (method == 8) {
        /* DEFLATE -- read compressed data, then inflate */
        uint8_t *comp_buf = (uint8_t *)malloc(comp_sz);
        if (!comp_buf) return 0;

        size_t nread = td5_plat_file_read(f, comp_buf, comp_sz);
        if (nread != comp_sz) {
            TD5_LOG_E(LOG_TAG, "deflate read short: got %u, expected %u",
                      (unsigned)nread, comp_sz);
            free(comp_buf);
            return 0;
        }

        size_t result = TD5_TINFL_DECOMPRESS(out_buf, uncomp_sz,
                                              comp_buf, comp_sz);
        free(comp_buf);

        if (result != uncomp_sz) {
            TD5_LOG_E(LOG_TAG, "inflate failed: got %u, expected %u",
                      (unsigned)result, uncomp_sz);
            return 0;
        }

        return (int)uncomp_sz;
    }
    else {
        TD5_LOG_E(LOG_TAG, "unsupported compression method: %u", method);
        return 0;
    }
}

/* ========================================================================
 * Loose File Override
 *
 * Both GetArchiveEntrySize and ReadArchiveEntry check the filesystem first.
 * If fopen(entryName) succeeds, the loose file is used instead of the ZIP.
 * This matches original behavior at 0x4409B0 and 0x440790.
 * ======================================================================== */

/**
 * Try to read a loose file. Returns size or -1 if not found.
 */
static int try_loose_file_size(const char *name)
{
    if (!td5_plat_file_exists(name)) return -1;

    TD5_File *f = td5_plat_file_open(name, "rb");
    if (!f) return -1;

    int64_t sz = td5_plat_file_size(f);
    td5_plat_file_close(f);
    return (sz >= 0) ? (int)sz : -1;
}

/**
 * Try to read a loose file into a buffer. Returns bytes read or -1.
 */
static int try_loose_file_read(const char *name, void *buf, int max_size)
{
    TD5_File *f = td5_plat_file_open(name, "rb");
    if (!f) return -1;

    size_t nread = td5_plat_file_read(f, buf, (size_t)max_size);
    td5_plat_file_close(f);
    return (int)nread;
}

static int try_extracted_level_file_size(const char *name, const char *zip_path)
{
    char candidate[512];

    for (int offset = 0; offset <= 1; offset++) {
        if (!build_extracted_level_path(candidate, sizeof(candidate), name, zip_path, offset))
            continue;
        if (td5_plat_file_exists(candidate))
            return try_loose_file_size(candidate);
    }

    return -1;
}

static int try_extracted_level_file_read(const char *name, const char *zip_path,
                                         void *buf, int max_size)
{
    char candidate[512];

    for (int offset = 0; offset <= 1; offset++) {
        if (!build_extracted_level_path(candidate, sizeof(candidate), name, zip_path, offset))
            continue;
        if (td5_plat_file_exists(candidate))
            return try_loose_file_read(candidate, buf, max_size);
    }

    return -1;
}

/* ========================================================================
 * Public ZIP API
 * ======================================================================== */

int td5_asset_get_entry_size(TD5_Archive *arc, const char *name)
{
    if (!name) return -1;

    /* Loose file override */
    int loose_sz = try_loose_file_size(name);
    if (loose_sz >= 0) return loose_sz;

    /* Search ZIP */
    TD5_ZipEntry *e = find_entry(arc, name);
    if (!e) return -1;

    return (int)e->uncompressed_size;
}

int td5_asset_read_entry(TD5_Archive *arc, const char *name,
                         void *buf, int max_size)
{
    if (!name || !buf || max_size <= 0) return -1;

    /* Loose file override */
    int loose = try_loose_file_read(name, buf, max_size);
    if (loose >= 0) {
        TD5_LOG_D(LOG_TAG, "read loose file: %s (%d bytes)", name, loose);
        return loose;
    }

    /* Search ZIP */
    if (!arc) return -1;
    TD5_ZipEntry *e = find_entry(arc, name);
    if (!e) {
        TD5_LOG_W(LOG_TAG, "entry not found: %s in %s", name, arc->path);
        return -1;
    }

    /* Open the ZIP file and seek to the local header */
    TD5_File *f = td5_plat_file_open(arc->path, "rb");
    if (!f) return -1;

    if ((int64_t)e->local_header_offset < 0 ||
        (int64_t)e->local_header_offset >= td5_plat_file_size(f)) {
        TD5_LOG_E(LOG_TAG, "entry local header out of range: %s in %s offset=%u",
                  name, arc->path, (unsigned int)e->local_header_offset);
        td5_plat_file_close(f);
        return -1;
    }

    td5_plat_file_seek(f, e->local_header_offset, 0);

    int result = decompress_entry(f, e, buf, max_size);
    td5_plat_file_close(f);

    if (result > 0) {
        TD5_LOG_D(LOG_TAG, "read entry: %s from %s (%d bytes)",
                  name, arc->path, result);
    }
    return result > 0 ? result : -1;
}

/* ========================================================================
 * Convenience Wrappers (match original calling convention)
 *
 * The original EXE passes (entryName, zipPath) to each function and
 * opens/parses/closes the ZIP each time. We cache nothing at this level
 * to match original behavior, but callers can use TD5_Archive directly
 * for bulk operations.
 * ======================================================================== */

int td5_asset_get_entry_size_from_path(const char *entry_name,
                                       const char *zip_path)
{
    /* Step 0: editable-source pack-on-load (report the PACKED size). */
    int src_sz = td5_assetsrc_packed_size(entry_name, zip_path);
    if (src_sz >= 0) return src_sz;

    /* Loose file override */
    int loose_sz = try_loose_file_size(entry_name);
    if (loose_sz >= 0) return loose_sz;

    loose_sz = try_extracted_level_file_size(entry_name, zip_path);
    if (loose_sz >= 0) return loose_sz;

    loose_sz = try_extracted_asset_file_size(entry_name, zip_path);
    if (loose_sz >= 0) return loose_sz;

    TD5_Archive *arc = td5_asset_open_archive(zip_path);
    if (!arc) return -1;

    int result = td5_asset_get_entry_size(arc, entry_name);
    td5_asset_close_archive(arc);
    return result;
}

int td5_asset_read_entry_from_path(const char *entry_name,
                                   const char *zip_path,
                                   void *buf, int max_size)
{
    /* Loose file override */
    int loose = try_loose_file_read(entry_name, buf, max_size);
    if (loose >= 0) return loose;

    loose = try_extracted_level_file_read(entry_name, zip_path, buf, max_size);
    if (loose >= 0) return loose;

    loose = try_extracted_asset_file_read(entry_name, zip_path, buf, max_size);
    if (loose >= 0) return loose;

    TD5_Archive *arc = td5_asset_open_archive(zip_path);
    if (!arc) return -1;

    int result = td5_asset_read_entry(arc, entry_name, buf, max_size);
    td5_asset_close_archive(arc);
    return result;
}

void *td5_asset_open_and_read(const char *entry_name,
                              const char *zip_path,
                              int *out_size)
{
    if (!entry_name || !zip_path) {
        TD5_LOG_E(LOG_TAG, "open_and_read invalid args: entry=%p zip=%p",
                  (const void *)entry_name, (const void *)zip_path);
        if (out_size) *out_size = 0;
        return NULL;
    }

    if (out_size) *out_size = 0;

    /* Step 0: editable-source pack-on-load (retires binary .DAT). If an
     * editable source exists for this entry, encode it to the .DAT layout in
     * memory and return it; otherwise fall through to the legacy paths. */
    {
        int   packed_sz = 0;
        void *packed    = td5_assetsrc_pack(entry_name, zip_path, &packed_sz);
        if (packed) {
            if (out_size) *out_size = packed_sz;
            return packed;
        }
    }

    /* Loose file override */
    int loose_sz = try_loose_file_size(entry_name);
    if (loose_sz >= 0) {
        void *buf = malloc((size_t)loose_sz);
        if (!buf) return NULL;
        int nread = try_loose_file_read(entry_name, buf, loose_sz);
        if (nread < 0) { free(buf); return NULL; }
        if (out_size) *out_size = nread;
        return buf;
    }

    loose_sz = try_extracted_level_file_size(entry_name, zip_path);
    if (loose_sz >= 0) {
        void *buf = malloc((size_t)loose_sz);
        if (!buf) return NULL;
        int nread = try_extracted_level_file_read(entry_name, zip_path, buf, loose_sz);
        if (nread < 0) { free(buf); return NULL; }
        if (out_size) *out_size = nread;
        return buf;
    }

    loose_sz = try_extracted_asset_file_size(entry_name, zip_path);
    if (loose_sz >= 0) {
        void *buf = malloc((size_t)loose_sz);
        if (!buf) return NULL;
        int nread = try_extracted_asset_file_read(entry_name, zip_path, buf, loose_sz);
        if (nread < 0) { free(buf); return NULL; }
        if (out_size) *out_size = nread;
        return buf;
    }

    /* ZIP path */
    TD5_Archive *arc = td5_asset_open_archive(zip_path);
    if (!arc) return NULL;

    int size = td5_asset_get_entry_size(arc, entry_name);
    if (size <= 0) {
        td5_asset_close_archive(arc);
        return NULL;
    }

    void *buf = malloc((size_t)size);
    if (!buf) {
        td5_asset_close_archive(arc);
        return NULL;
    }

    int result = td5_asset_read_entry(arc, entry_name, buf, size);
    td5_asset_close_archive(arc);

    if (result <= 0) {
        TD5_LOG_E(LOG_TAG, "open_and_read read failed: entry=%s zip=%s size=%d result=%d",
                  entry_name, zip_path, size, result);
        free(buf);
        return NULL;
    }

    if (out_size) *out_size = result;
    return buf;
}

/* ========================================================================
 * FindArchiveEntryByName (0x442CF0)
 *
 * Case-insensitive search through the static.hed named entry array.
 * The original uses stricmp (FUN_00449310) in a linear scan over 0x40-byte
 * entries pointed to by DAT_004c3cfc, with count DAT_004c3cf8.
 * ======================================================================== */

TD5_StaticHedEntry *td5_asset_find_entry_by_name(
    TD5_StaticHedEntry *entries, int entry_count, const char *name)
{
    if (!entries || !name || entry_count <= 0) return NULL;

    for (int i = 0; i < entry_count; i++) {
        if (td5_stricmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }

    /* Return first entry as fallback (matches original: returns base ptr) */
    return NULL;
}

/* TGA decoder removed — all assets now loaded as PNG from re/assets/ */

/* ========================================================================
 * Level Data Loading -- LoadTrackRuntimeData (0x42FB90)
 *
 * Loads 6 files from level%03d.zip into game globals. Direction-variant
 * filenames are selected based on gReverseTrackDirection.
 *
 * The actual global storage and track parsing are in td5_track module;
 * this function handles the ZIP I/O and buffer allocation.
 * ======================================================================== */

/* These are defined in td5_track / td5_game and declared extern here.
 * The actual linking happens at build time. For now we provide the
 * load function framework. */

/* Track-asset filenames — picked by direction, not as case-variant fallbacks.
 *
 * The original picks ONE filename per slot from the pointer table at
 * 0x004673B8 (forward) / 0x004673C8 (reverse), indexed by
 * gReverseTrackDirection @ 0x004AAF54. The ZIP always contains both B and
 * non-B variants, so a "load the first that exists" fallback silently
 * picks the forward file even when racing reverse — that's the bug the
 * original's per-direction pointer table avoids. Port mirrors that here.
 * [CONFIRMED @ 0x0042FB90 LoadTrackRuntimeData selector]
 */
static const char *s_strip_fwd[1]   = { "STRIP.DAT"   };
static const char *s_strip_rev[1]   = { "STRIPB.DAT"  };
static const char *s_left_fwd[1]    = { "LEFT.TRK"    };
static const char *s_left_rev[1]    = { "LEFTB.TRK"   };
static const char *s_right_fwd[1]   = { "RIGHT.TRK"   };
static const char *s_right_rev[1]   = { "RIGHTB.TRK"  };
static const char *s_traffic_fwd[1] = { "TRAFFIC.BUS" };
static const char *s_traffic_rev[1] = { "TRAFFICB.BUS" };
/** Wrapper texture-page limit. Must match td5_platform_win32.c. */
#define TD5_TRACK_TEXTURE_PAGE_LIMIT 1024

/* >0 when the active level is a substituted TD6 track (override knob OR a TD6
 * menu registry slot). Gates the TD6 engine fixes. Set in td5_asset_level_number. */
int g_active_td6_level = 0;

/* TD6 menu registry: schedule slot -> converted TD5 level number. One row per
 * migrated TD6 track. Single source of truth for both level resolution and the
 * "is this a TD6 track" gating. Extend as more TD6 tracks are migrated; keep in
 * sync with the frontend tables (s_track_schedule_to_tga_index / display names)
 * and re/tools/track_preview_render.py TD6_TRACKS. */
static const struct { int slot; int level; int start_span; int finish_span; float sky_pitch; } k_td6_menu_slots[] = {
    /* frontend schedule slot, converted TD5 level number
     * (re/assets/levels/levelNNN), per-track grid/start span, and (P2P only)
     * the finish span. TD6 tracks each have their own start span; the TD5
     * per-level start-span tables in td5_game.c are meaningless for them.
     * CIRCUITS: finish_span = 0 (lap-based, finish == start after N laps).
     * POINT-TO-POINT: finish_span > 0 (race ends when the player reaches it;
     * there is no TD6 checkpoint data, so this is the only finish trigger). */
    /* sky_pitch (radians): per-track sky-DOME pitch that drops the panorama
     * horizon toward eye level. Pitching a SPHERICAL dome curves the horizon at
     * the screen edges while steering (a tilted great-circle projects to a
     * conic), worse the larger the angle — so this is capped low. A full
     * distortion-free horizon fix needs regenerating the FORWSKY panoramas
     * (sky-dominant) instead of tilting the dome. 0 = no pitch. */
    { 26,  7, 312,    0, 0.08f },  /* PELTON RACEWAY  (TD6 level010, circuit)  */
    { 27, 18,  70,    0, 0.08f },  /* IRELAND         (TD6 level011, circuit)  */
    { 28, 19,  32,    0, 0.08f },  /* LAKE TAHOE      (TD6 level015, circuit)  */
    { 29, 20, 468,    0, 0.08f },  /* CAPE HATTERAS   (TD6 level016, circuit; start/finish banner @ span 468) */
    { 30, 21, 138,    0, 0.08f },  /* SWITZERLAND     (TD6 level017, circuit; start/finish banner @ span 138) */
    { 31, 22,  10,    0, 0.08f },  /* EGYPT           (TD6 level018, circuit)  */
    /* P2P start_span = the track's START-BANNER span (from the in-track 'start'
     * banner mesh). The grid spreads BACKWARD from start_span (offsets -3..-18,
     * s_staggered_span_offsets), so cars sit a few spans BEHIND the banner and
     * drive through it at the lights — matching the original game. Previously
     * all were span 20, which left a 40-56 span empty run-up before the banner
     * (the "starts too far from the banner" report). Rome ships no dedicated
     * 'start' banner, but its first checkpoint gantry (Check01a/b @ span 51) sits
     * right at the start and reads as the start banner, so Rome starts there
     * (span 51) — the old span 20 left a ~31-49 span empty run-up to it (the S29
     * "Rome starts too far from the start banner" report). London already starts
     * ~8 spans behind its Kstart banner (~28), so it keeps span 20.
     *
     * finish_span = the track's FINISH-BANNER span (in-track 'finish'/Kfin mesh).
     * Originally all were span_cnt-8 — i.e. 8 spans from the strip END (the cliff
     * into the void) — too close to the edge for the instant-end race to stop the
     * player before they run off the world ("out of bounds at the track end").
     * Ending at the finish banner (~50-90 spans earlier, well inside the track,
     * after the last checkpoint) ends the race at the visual finish line and
     * keeps the player on the road. Rome ships no finish gantry and its road
     * stays a walled 6-8 lanes to the end, so it keeps span_cnt-8 (2348). */
    { 32,  8,  76, 2762, 0.08f },  /* PARIS      (TD6 level000, P2P, start~76  finish 1finish~2762) */
    { 33,  9,  59, 2523, 0.22f },  /* NEW YORK   (TD6 level001, P2P, start~59  finish Finish~2523) — [#12 2026-06-19] pitch 0.08->0.22 to LOWER the too-high horizon further (both fwd+back); TD5RE_SKY_PITCH overrides at runtime */
    { 34, 10,  51, 2348, 0.08f },  /* ROME       (TD6 level002, P2P, start=Check01 gantry~51, walled to end) */
    { 35, 11,  70, 2014, 0.08f },  /* HONG KONG  (TD6 level003, P2P, start~70  finish 1finish~2014)  */
    { 36, 12,  20, 2083, 0.08f },  /* LONDON     (TD6 level004, P2P, start~28  finish Kfin~2083)     */
};

/* Converted level number for a TD6 menu slot, or 0 if track_index is not a
 * migrated TD6 track. */
int td5_asset_td6_level_for_slot(int track_index)
{
    size_t i;
    for (i = 0; i < sizeof(k_td6_menu_slots) / sizeof(k_td6_menu_slots[0]); i++)
        if (k_td6_menu_slots[i].slot == track_index)
            return k_td6_menu_slots[i].level;
    return 0;
}

/* Per-track grid / start-finish span for a migrated TD6 track by its converted
 * level number, or 0 if level_num is not a TD6 track. */
int td5_asset_td6_start_span_for_level(int level_num)
{
    size_t i;
    for (i = 0; i < sizeof(k_td6_menu_slots) / sizeof(k_td6_menu_slots[0]); i++)
        if (k_td6_menu_slots[i].level == level_num)
            return k_td6_menu_slots[i].start_span;
    /* Custom tracks carry their grid/start span in the registry manifest.
     * (A circuit start span of 0 reads as "not found" here; the spawn path in
     * td5_game.c honors a registry start span of 0 directly for custom levels.) */
    if (td5_track_registry_has_level(level_num)) {
        int ss = td5_track_registry_start_span_for_level(level_num);
        if (ss > 0)
            return ss;
    }
    return 0;
}

/* Per-track sky-dome pitch (radians) for a migrated TD6 track by converted
 * level number. >0 lowers the panorama horizon to eye level. Returns a sane
 * default for unknown levels (shouldn't happen for TD6). */
float td5_asset_td6_sky_pitch_for_level(int level_num)
{
    size_t i;
    for (i = 0; i < sizeof(k_td6_menu_slots) / sizeof(k_td6_menu_slots[0]); i++)
        if (k_td6_menu_slots[i].level == level_num)
            return k_td6_menu_slots[i].sky_pitch;
    return 0.12f;
}

/* Finish span for a migrated point-to-point TD6 track (race ends when the
 * player reaches it). 0 for circuits (lap-based) and non-TD6 levels. */
int td5_asset_td6_finish_span_for_level(int level_num)
{
    size_t i;
    for (i = 0; i < sizeof(k_td6_menu_slots) / sizeof(k_td6_menu_slots[0]); i++)
        if (k_td6_menu_slots[i].level == level_num)
            return k_td6_menu_slots[i].finish_span;
    /* Custom point-to-point tracks carry their finish span in the registry
     * manifest; circuits report 0 there (lap-based, no finish trigger). */
    if (td5_track_registry_has_level(level_num)) {
        int fs = td5_track_registry_finish_span_for_level(level_num);
        if (fs > 0)
            return fs;
    }
    return 0;
}

/* Synthesized checkpoint spans for a migrated TD6 track (converted level
 * number). Writes up to 5 ascending strip-span thresholds into out_spans[] and
 * returns the count (0 = no checkpoints on this track).
 *
 * NON-FAITHFUL by necessity: TD6.exe ships NO live checkpoint-trigger data
 * (RE'd 2026-06-04 — CHECKPT.NUM is loaded but never read; the banner-texture
 * tables and RINGO have zero code refs; the only per-track span table drives
 * fog/lighting). So the in-track checkpoint RING / numbered-BANNER meshes are
 * pure decoration. To make those visible banners FUNCTIONAL in the port we
 * SYNTHESIZE checkpoints from the numbered checkpoint-BANNER meshes' world
 * positions, mapped to the nearest strip span (re/tools/extract_td6_checkpoints.py).
 *
 * All 5 migrated point-to-point city tracks ship numbered checkpoint banners,
 * each city using its own art/naming convention (visually confirmed against the
 * extracted TGAs, 2026-06-04):
 *   Paris  : 1one/1two/1three/1four  (blue swirl banners)        -> 4 checkpoints
 *   NewYork: 1.tga..4.tga            (green/yellow oval banners)  -> 4
 *   Rome   : Check01a..Check05a      (green/orange checkered)     -> 5
 *   HongKong:1one/1two/1three/1four  (red/gold banners)           -> 4
 *   London : Kstage1..Kstage4        (cyan numbered banners)      -> 4
 * (My first pass mis-keyed on texture-NAME guesses — NY's "ringo" is a RINGO'S
 * storefront, Paris's "Post" are wall posters, London's "flag" are national
 * flags — all décor, not checkpoints. The banners above are the real signage.)
 * The 6 circuit tracks are lap-based and get no synthesized checkpoints.
 * Faithful TD5 tracks return 0 and are entirely unaffected. */
int td5_asset_td6_checkpoint_spans(int level_num, int out_spans[5])
{
    static const int s_paris[]  = { 641, 1113, 1685, 2211 };       /* 1one..1four      */
    static const int s_ny[]     = { 600, 1008, 1619, 1998 };       /* 1.tga..4.tga     */
    static const int s_rome[]   = { 51, 505, 1056, 1500, 1838 };   /* Check01..Check05 */
    static const int s_hk[]     = { 540, 832, 1196, 1567 };        /* 1one..1four      */
    static const int s_london[] = { 515, 906, 1289, 1692 };        /* Kstage1..Kstage4 */
    const int *src = 0;
    int n = 0, i;
    switch (level_num) {
    case  8: src = s_paris;  n = 4; break;   /* PARIS     (TD6 level000, P2P) */
    case  9: src = s_ny;     n = 4; break;   /* NEW YORK  (TD6 level001, P2P) */
    case 10: src = s_rome;   n = 5; break;   /* ROME      (TD6 level002, P2P) */
    case 11: src = s_hk;     n = 4; break;   /* HONG KONG (TD6 level003, P2P) */
    case 12: src = s_london; n = 4; break;   /* LONDON    (TD6 level004, P2P) */
    default: return 0;
    }
    for (i = 0; i < n; i++)
        out_spans[i] = src[i];
    return n;
}

int td5_asset_level_number(int track_index)
{
    /* TD6 track migration (Phase 1, NON-faithful dev knob): when
     * [Game] OverrideTrackZip / --OverrideTrackZip is > 0, force every level
     * lookup to that number so the loose-file loader resolves
     * re/assets/levels/level<NNN>/ for a converted TD6 track (see
     * convert_td6_tracks.py). Wins over the schedule/drag remap on purpose —
     * Phase 1 drives a single selected track via AutoRace. 0 = faithful.
     * One-shot log so the per-load call burst doesn't spam. */
    if (g_td5.ini.override_track_zip > 0) {
        static int s_logged_override = -1;
        if (s_logged_override != g_td5.ini.override_track_zip) {
            s_logged_override = g_td5.ini.override_track_zip;
            TD5_LOG_W(LOG_TAG, "OverrideTrackZip active: level_number forced to %d "
                      "(TD6 track migration; non-faithful)", g_td5.ini.override_track_zip);
        }
        g_active_td6_level = g_td5.ini.override_track_zip;
        return g_td5.ini.override_track_zip;
    }

    /* [TD6 MENU REGISTRY] Schedule slots beyond the 19 native tracks map to
     * converted TD6 levels (loose re/assets/levels/levelNNN/). This is how a
     * menu-selected TD6 track resolves its level WITHOUT the OverrideTrackZip
     * ini knob. Setting active_td6_level here makes the override-gated engine
     * fixes (seam/lap/grass/AI/render) fire for these tracks too. Extend the
     * table as more TD6 tracks are migrated (one row per added menu slot). */
    {
        int td6 = td5_asset_td6_level_for_slot(track_index);
        if (td6 > 0) {
            g_active_td6_level = td6;
            return td6;
        }
    }

    /* [CUSTOM TRACK REGISTRY] Slots >= 37 are user-built tracks from the runtime
     * manifest (re/assets/levels/custom_tracks.json, via td5_trackgen.py). They
     * resolve to their own levelNNN/ directory but are NOT TD6 -- leave
     * g_active_td6_level = 0 so the TD6-specific branch/seam band-aids stay off
     * (custom strip geometry is generated clean by the converter). */
    if (track_index >= TD5_CUSTOM_TRACK_SLOT_BASE) {
        int custom = td5_track_registry_level_for_slot(track_index);
        if (custom > 0) {
            g_active_td6_level = 0;
            return custom;
        }
    }

    g_active_td6_level = 0;   /* faithful TD5 track */

    /* Drag race hardcodes level030.zip [CONFIRMED @ InitializeRaceSession
     * 0x0042ad63-0x0042ad73]: when s_selected_track < 0 the original writes
     * MOV [0x004aaf3c], 0x1e (=30) directly, bypassing the schedule remap.
     * Check game_type (not drag_race_enabled) — game_type is explicitly
     * assigned in ConfigureGameTypeFlags for every race mode, so it won't
     * leak between sessions even if a flag reset is missed.
     *
     * [MP DRAG TRACK FIX 2026-07-04] The MP lobby (Screen_MpModeVote ->
     * mp_mode_config_apply_defaults) never calls ConfigureGameTypeFlags, so
     * game_type keeps whatever value the LAST race left it at (e.g. 0 after a
     * normal quick race). track_index for MP drag is schedule slot 19
     * (FE_QUICKRACE_DRAG_STRIP_SCHEDULE_INDEX, td5_fe_race.c CarSelect MP),
     * which is out of range for the 19-entry schedule table below and fell
     * through to its "return 1" fallback — level001.zip, which happens to be
     * schedule slot 10 (Keswick)'s zip. Hence "MP drag race launches Keswick
     * instead of the drag strip" after playing other tracks first (whatever
     * game_type those left behind). td5_game_drag_mp_active() is the
     * established mp_mode_config-based check used everywhere else in the
     * codebase for this exact SP-flag-vs-MP-mode gap (see td5_game.c
     * td5_game_drag_mp_active callers). */
    if (g_td5.game_type == TD5_GAMETYPE_DRAG_RACE || td5_game_drag_mp_active()) {
        TD5_LOG_I(LOG_TAG, "level_number: drag race (game_type=%d mp_drag=%d) -> level030.zip",
                  (int)g_td5.game_type, td5_game_drag_mp_active());
        return 30;
    }

    /* Two-step lookup from the original binary:
     * Step 1: schedule slot index -> pool index via gScheduleToPoolIndex (VA 0x466894).
     * Step 2: pool index -> level ZIP number via pool-to-ZIP table (VA 0x466D50). */
    static const uint8_t k_schedule_to_pool[19] = {
        11,  9,  7, 10, 13, 16, 15, 14,  6,  8,
         0,  1,  2,  3,  4,  5, 12, 18, 17
    };
    static const int k_pool_to_zip[19] = {
         1,  2,  3,  4,  5,  6, 13, 14, 15, 16,
        17, 23, 25, 26, 27, 28, 29, 37, 39
    };
    if (track_index < 0 || track_index >= 19)
        return 1;
    return k_pool_to_zip[k_schedule_to_pool[track_index]];
}

/* Fast narrow PNG decoder for the car-preview-style assets: 8-bit, color type 6
 * (RGBA), non-interlaced — the format our tools (PIL) emit for carpic/carpicpaint.
 * Inflates IDAT through the engine's zlib-backed td5_inflate, which is ~2x faster
 * than stb_image's built-in inflate on a big (1632x1120) car preview, getting a
 * carpic decode comfortably under the 16.6ms vsync frame budget so a car change
 * no longer drops a frame. Returns 1 + a malloc'd RGBA buffer (caller frees with
 * stbi_image_free == free), or 0 to fall back to stb_image for any other PNG
 * variant (palette / 16-bit / grayscale / interlaced). */
static int td5_png_decode_fast(const uint8_t *data, size_t size,
                               unsigned char **out, int *w_out, int *h_out)
{
    static const uint8_t PNG_SIG[8] = {137,80,78,71,13,10,26,10};
    if (!data || size < 57 || memcmp(data, PNG_SIG, 8) != 0) return 0;

    int width = 0, height = 0, bit_depth = 0, color_type = 0, interlace = -1, got_ihdr = 0;
    uint8_t *idat = NULL; size_t idat_len = 0, idat_cap = 0;
    size_t pos = 8;

    while (pos + 12 <= size) {
        uint32_t clen = ((uint32_t)data[pos]<<24)|((uint32_t)data[pos+1]<<16)|
                        ((uint32_t)data[pos+2]<<8)|(uint32_t)data[pos+3];
        const uint8_t *ctype = data + pos + 4;
        const uint8_t *cdata = data + pos + 8;
        if (pos + 12 + (size_t)clen > size) break;              /* malformed/truncated */
        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            width  = (int)(((uint32_t)cdata[0]<<24)|((uint32_t)cdata[1]<<16)|((uint32_t)cdata[2]<<8)|cdata[3]);
            height = (int)(((uint32_t)cdata[4]<<24)|((uint32_t)cdata[5]<<16)|((uint32_t)cdata[6]<<8)|cdata[7]);
            bit_depth  = cdata[8];
            color_type = cdata[9];
            interlace  = cdata[12];
            got_ihdr = 1;
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen > idat_cap) {
                size_t nc = (idat_len + clen) * 2 + 4096;
                uint8_t *ni = (uint8_t *)realloc(idat, nc);
                if (!ni) { free(idat); return 0; }
                idat = ni; idat_cap = nc;
            }
            memcpy(idat + idat_len, cdata, clen);
            idat_len += clen;
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + (size_t)clen;                                /* len + type + data + crc */
    }

    if (!got_ihdr || bit_depth != 8 || color_type != 6 || interlace != 0 ||
        width <= 0 || height <= 0 || idat_len < 3 ||
        (uint64_t)width * (uint64_t)height > 64ull*1024ull*1024ull) {
        free(idat); return 0;                                    /* not our format -> stb */
    }

    size_t stride  = (size_t)width * 4 + 1;                      /* filter byte + RGBA row */
    size_t raw_len = stride * (size_t)height;
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    if (!raw) { free(idat); return 0; }

    /* IDAT is a zlib stream; td5_inflate wants RAW deflate, so skip the 2-byte
     * zlib header (the deflate end-marker stops before the adler trailer). */
    size_t got = td5_inflate_mem_to_mem(raw, raw_len, idat + 2, idat_len - 2);
    free(idat);
    if (got != raw_len) { free(raw); return 0; }

    unsigned char *img = (unsigned char *)malloc((size_t)width * (size_t)height * 4);
    if (!img) { free(raw); return 0; }

    /* Unfilter with the per-line filter type hoisted OUT of the inner loop so
     * the common cases reduce to a memcpy / vectorizable add (the per-byte
     * switch dominated the carpic decode). bpp=4. */
    const size_t bpp = 4;
    size_t rowbytes = (size_t)width * 4;
    for (int y = 0; y < height; y++) {
        unsigned char filt = raw[(size_t)y * stride];
        const unsigned char *src = raw + (size_t)y * stride + 1;
        unsigned char *dst  = img + (size_t)y * rowbytes;
        const unsigned char *prev = (y > 0) ? (img + (size_t)(y - 1) * rowbytes) : NULL;
        size_t x;
        switch (filt) {
            case 0:                                              /* None */
                memcpy(dst, src, rowbytes);
                break;
            case 1:                                              /* Sub */
                for (x = 0; x < bpp; x++) dst[x] = src[x];
                for (; x < rowbytes; x++) dst[x] = (unsigned char)(src[x] + dst[x - bpp]);
                break;
            case 2:                                              /* Up */
                if (prev) for (x = 0; x < rowbytes; x++) dst[x] = (unsigned char)(src[x] + prev[x]);
                else memcpy(dst, src, rowbytes);
                break;
            case 3:                                              /* Average */
                for (x = 0; x < bpp; x++)
                    dst[x] = (unsigned char)(src[x] + ((prev ? prev[x] : 0) >> 1));
                for (; x < rowbytes; x++)
                    dst[x] = (unsigned char)(src[x] + ((dst[x - bpp] + (prev ? prev[x] : 0)) >> 1));
                break;
            case 4:                                              /* Paeth */
                if (!prev) {
                    for (x = 0; x < bpp; x++) dst[x] = src[x];
                    for (; x < rowbytes; x++) dst[x] = (unsigned char)(src[x] + dst[x - bpp]);
                } else {
                    for (x = 0; x < bpp; x++) dst[x] = (unsigned char)(src[x] + prev[x]);
                    for (; x < rowbytes; x++) {
                        int a = dst[x - bpp], b = prev[x], c = prev[x - bpp];
                        int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
                        int pred = (pa <= pb && pa <= pc) ? a : (pb <= pc) ? b : c;
                        dst[x] = (unsigned char)(src[x] + pred);
                    }
                }
                break;
            default: free(img); free(raw); return 0;             /* bad filter -> stb */
        }
    }
    free(raw);
    *out = img; *w_out = width; *h_out = height;
    return 1;
}

int td5_asset_decode_png_rgba32(const char *path,
                                       void **pixels_out,
                                       int *width_out,
                                       int *height_out)
{
    TD5_File *f = NULL;
    uint8_t *file_data = NULL;
    unsigned char *pixels = NULL;
    int width = 0, height = 0, channels = 0;
    int64_t file_size;
    size_t nread;

    if (pixels_out) *pixels_out = NULL;
    if (width_out) *width_out = 0;
    if (height_out) *height_out = 0;
    if (!path || !pixels_out)
        return 0;

    f = td5_plat_file_open(path, "rb");
    if (!f)
        return 0;

    file_size = td5_plat_file_size(f);
    if (file_size <= 0 || file_size > 64 * 1024 * 1024) {
        td5_plat_file_close(f);
        return 0;
    }

    file_data = (uint8_t *)malloc((size_t)file_size);
    if (!file_data) {
        td5_plat_file_close(f);
        return 0;
    }

    nread = td5_plat_file_read(f, file_data, (size_t)file_size);
    td5_plat_file_close(f);
    if (nread != (size_t)file_size) {
        free(file_data);
        return 0;
    }

    /* Fast path: zlib-backed decode for the common 8-bit RGBA PNGs (carpics,
     * skins). Falls through to stb_image for any unsupported variant. */
    if (td5_png_decode_fast(file_data, (size_t)file_size,
                            &pixels, &width, &height)) {
        free(file_data);
    } else {
        pixels = stbi_load_from_memory(file_data, (int)file_size,
                                       &width, &height, &channels, 4);
        free(file_data);
    }
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return 0;
    }

    /* stb_image / the fast path output RGBA; D3D11 format=2 upload expects BGRA
     * byte order. Swap R↔B here so all callers get GPU-ready pixel data. */
    {
        int count = width * height;
        int i;
        uint8_t *p = pixels;
        for (i = 0; i < count; i++, p += 4) {
            uint8_t tmp = p[0];  /* R */
            p[0] = p[2];        /* R = B */
            p[2] = tmp;         /* B = R */
        }
    }

    *pixels_out = pixels;
    if (width_out) *width_out = width;
    if (height_out) *height_out = height;
    return 1;
}

/* ========================================================================
 * Unified PNG Texture Loading
 * ======================================================================== */

/** Dilate RGB from opaque neighbors into alpha==0 pixels.
 *
 *  Why: the original .exe stores {R=0,G=0,B=0,A=0} at transparent texels
 *  (UploadRaceTexturePage @ 0x0040b590, LoadRaceTexturePages @ 0x00442770)
 *  and the D3D3/DDraw pipeline masks the dark-bleed artifact via (assumed)
 *  POINT sampling. The td5re D3D11 wrapper samples LINEAR by default
 *  (d3d11_backend.c LINEAR_WRAP), so near-zero RGB at alpha=0 bleeds into
 *  opaque neighbors as a dark halo. Fix: premultiplied-edge style bleed —
 *  replace RGB of every transparent pixel with the average of its opaque
 *  neighbors, iterated so the fill propagates inward. Alpha stays 0. */
static void alpha_bleed_rgb(uint8_t *pixels, int w, int h)
{
    size_t n, i;
    uint8_t *back;
    uint8_t *valid;
    int iter;

    if (!pixels || w < 2 || h < 2) return;
    n = (size_t)w * (size_t)h;
    back = (uint8_t *)malloc(n * 4);
    valid = (uint8_t *)malloc(n);
    if (!back || !valid) {
        free(back);
        free(valid);
        return;
    }

    for (i = 0; i < n; i++)
        valid[i] = (pixels[i * 4 + 3] != 0) ? 1 : 0;

    for (iter = 0; iter < 4; iter++) {
        int filled_any = 0;
        int y, x, dy, dx;
        memcpy(back, pixels, n * 4);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w; x++) {
                size_t idx = (size_t)y * w + x;
                uint32_t sb = 0, sg = 0, sr = 0, cnt = 0;
                uint8_t *p;
                if (valid[idx]) continue;
                for (dy = -1; dy <= 1; dy++) {
                    int ny = y + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (dx = -1; dx <= 1; dx++) {
                        int nx = x + dx;
                        size_t nidx;
                        uint8_t *np;
                        if ((dx | dy) == 0) continue;
                        if (nx < 0 || nx >= w) continue;
                        nidx = (size_t)ny * w + nx;
                        if (!valid[nidx]) continue;
                        np = &back[nidx * 4];
                        sb += np[0];
                        sg += np[1];
                        sr += np[2];
                        cnt++;
                    }
                }
                if (cnt == 0) continue;
                p = &pixels[idx * 4];
                p[0] = (uint8_t)(sb / cnt);
                p[1] = (uint8_t)(sg / cnt);
                p[2] = (uint8_t)(sr / cnt);
                /* p[3] stays 0 — this pixel remains transparent */
                filled_any = 1;
            }
        }
        if (!filled_any) break;
        for (i = 0; i < n; i++) {
            if (valid[i]) continue;
            if (pixels[i * 4] || pixels[i * 4 + 1] || pixels[i * 4 + 2])
                valid[i] = 1;
        }
    }

    free(valid);
    free(back);
}

/** Apply color keying to BGRA32 pixel buffer in-place.
 *  Byte order after decode: p[0]=B, p[1]=G, p[2]=R, p[3]=A. */
static void apply_colorkey(void *pixels, int w, int h, TD5_ColorKeyMode mode)
{
    uint8_t *p = (uint8_t *)pixels;
    int count = w * h;
    int i;
    int keyed = 0;
    if (mode == TD5_COLORKEY_NONE) return;
    for (i = 0; i < count; i++, p += 4) {
        uint8_t b = p[0], g = p[1], r = p[2];
        switch (mode) {
        case TD5_COLORKEY_BLACK:
            if (r < 8 && g < 8 && b < 8) {
                p[3] = 0;
                keyed++;
            }
            break;
        case TD5_COLORKEY_RED:
            if (r >= 248 && g < 8 && b < 8) {
                p[0] = p[1] = p[2] = p[3] = 0;
                keyed++;
            }
            break;
        case TD5_COLORKEY_BLUE88:
            if (r < 8 && g < 8 && b >= 80 && b <= 96) {
                p[3] = 0;
                keyed++;
            }
            break;
        case TD5_COLORKEY_CYAN:
            /* PAUSETXT: white glyphs on cyan bg.  R channel = glyph
             * intensity (255=text, 0=background, 1-254=anti-aliased edge).
             * Use R as alpha so edges blend smoothly, set RGB to white. */
            if (g > 240 && b > 240) {
                p[3] = r;
                p[0] = p[1] = p[2] = 255;
                if (r == 0) keyed++;
            }
            break;
        default:
            break;
        }
    }
    if (keyed > 0) {
        alpha_bleed_rgb((uint8_t *)pixels, w, h);
        TD5_LOG_D(LOG_TAG, "alpha_bleed: mode=%d keyed=%d size=%dx%d",
                  (int)mode, keyed, w, h);
    }
}

int td5_asset_load_png_to_buffer(const char *png_path, TD5_ColorKeyMode colorkey,
                                 void **pixels_out, int *w_out, int *h_out)
{
    if (!td5_asset_decode_png_rgba32(png_path, pixels_out, w_out, h_out))
        return 0;
    /* td5_asset_decode_png_rgba32 already swapped R↔B → data is now BGRA.
     * Colorkey operates on BGRA: p[0]=B, p[1]=G, p[2]=R, p[3]=A */
    apply_colorkey(*pixels_out, *w_out, *h_out, colorkey);
    return 1;
}

int td5_asset_load_png_texture(int page_index, const char *png_path,
                               TD5_ColorKeyMode colorkey)
{
    void *pixels = NULL;
    int w = 0, h = 0;
    int ok;

    if (!td5_asset_load_png_to_buffer(png_path, colorkey, &pixels, &w, &h))
        return 0;

    ok = td5_plat_render_upload_texture(page_index, pixels, w, h, 2);
    stbi_image_free(pixels);
    if (ok)
        TD5_LOG_D(LOG_TAG, "PNG texture: %s → page %d (%dx%d)", png_path, page_index, w, h);
    return ok;
}

int td5_asset_resolve_png_path(const char *entry_name, const char *archive,
                               char *out_path, size_t out_size)
{
    char subfolder[128] = {0};
    char stem[128];
    const char *dot;
    int n;
    const char *zp;

    if (!entry_name || !archive || !out_path || out_size < 16)
        return 0;

    /* --- map archive path to re/assets subfolder --- */
    zp = archive;
    while (*zp == '\\' || *zp == '/') zp++;

    if (_strnicmp(zp, "level", 5) == 0 && zp[5] >= '0' && zp[5] <= '9') {
        n = 0;
        while (zp[5 + n] && zp[5 + n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "levels/level%.*s", n, zp + 5);
    }
    else if (_strnicmp(zp, "cars\\", 5) == 0 || _strnicmp(zp, "cars/", 5) == 0) {
        const char *base = zp + 5;
        n = 0;
        while (base[n] && base[n] != '.') n++;
        snprintf(subfolder, sizeof(subfolder), "cars/%.*s", n, base);
    }
    else if (_strnicmp(zp, "static.zip", 10) == 0)
        strncpy(subfolder, "static", sizeof(subfolder));
    else if (_strnicmp(zp, "traffic.zip", 11) == 0)
        strncpy(subfolder, "traffic", sizeof(subfolder));
    else if (_strnicmp(zp, "environs.zip", 12) == 0)
        strncpy(subfolder, "environs", sizeof(subfolder));
    else if (_strnicmp(zp, "loading.zip", 11) == 0)
        strncpy(subfolder, "loading", sizeof(subfolder));
    else if (_strnicmp(zp, "legals.zip", 10) == 0)
        strncpy(subfolder, "legals", sizeof(subfolder));
    else if (_strnicmp(zp, "cup.zip", 7) == 0 || _strnicmp(zp, "Cup.zip", 7) == 0)
        strncpy(subfolder, "cup", sizeof(subfolder));
    else if (_strnicmp(zp, "sound", 5) == 0)
        strncpy(subfolder, "sounds", sizeof(subfolder));
    else if (_strnicmp(zp, "Front", 5) == 0) {
        /* Front End archives — match any variant of separators/case */
        if (strstr(zp, "rontend") || strstr(zp, "rontEnd"))
            strncpy(subfolder, "frontend", sizeof(subfolder));
        else if (strstr(zp, "Extras.zip") || strstr(zp, "extras.zip"))
            strncpy(subfolder, "extras", sizeof(subfolder));
        else if (strstr(zp, "Mugshots") || strstr(zp, "mugshots"))
            strncpy(subfolder, "mugshots", sizeof(subfolder));
        else if (strstr(zp, "Sounds") || strstr(zp, "sounds"))
            strncpy(subfolder, "sounds", sizeof(subfolder));
        else if (strstr(zp, "Tracks") || strstr(zp, "tracks"))
            strncpy(subfolder, "tracks", sizeof(subfolder));
        else
            return 0;
    }
    else
        return 0;

    subfolder[sizeof(subfolder) - 1] = '\0';

    /* --- strip extension from entry name, build stem.png --- */
    dot = strrchr(entry_name, '.');
    n = dot ? (int)(dot - entry_name) : (int)strlen(entry_name);
    if (n >= (int)sizeof(stem)) n = (int)sizeof(stem) - 1;
    memcpy(stem, entry_name, n);
    stem[n] = '\0';

    n = snprintf(out_path, out_size, "re/assets/%s/%s.png", subfolder, stem);
    if (n < 0 || (size_t)n >= out_size)
        return 0;

    /* Normalize backslashes */
    { char *p = out_path; while (*p) { if (*p == '\\') *p = '/'; p++; } }

    return td5_plat_file_exists(out_path);
}

static void td5_asset_build_level_zip_path(int track_index,
                                           char *out_path,
                                           size_t out_size)
{
    int level_number = td5_asset_level_number(track_index);
    snprintf(out_path, out_size, "level%03d.zip", level_number);
}

static void td5_asset_build_level_loose_path(int track_index,
                                             const char *entry_name,
                                             char *out_path,
                                             size_t out_size)
{
    int level_number = td5_asset_level_number(track_index);
    snprintf(out_path, out_size, "re/assets/levels/level%03d/%s",
             level_number, entry_name ? entry_name : "");
}

/* Does this track ship reverse-direction data (its level dir has STRIPB.DAT)?
 * Offset-free, per-level check. Deliberately does NOT route through
 * load_first_available_level_entry / td5_asset_open_and_read: their
 * try_extracted_level_file_* helpers also probe level_num+1
 * (build_extracted_level_path's level_offset 0..1 loop), so a forward-only
 * track would falsely match a NEIGHBOUR level's STRIPB.DAT (e.g. Newcastle
 * level029 -> level030 Drag Strip). Mirrors the original's data rule: only
 * point-to-point tracks ship reverse strips; circuit tracks are forward-only.
 * Used both by the frontend (hide the Direction toggle) and by
 * td5_asset_load_level (force forward when reverse is requested but absent). */
int td5_asset_track_has_reverse(int track_index)
{
    char rev_path[256];
    /* Reverse direction is available when the track ships a COMPLETE reverse
     * data set: STRIPB.DAT (reverse geometry) PLUS LEFTB.TRK/RIGHTB.TRK (the
     * reverse AI corridor — required for the spawn yaw and AI steering line in
     * reverse). This per-track DATA check replaces the old "every TD6 track is
     * forward-only" blanket gate: migrated TD6 tracks that ship STRIPB.DAT and
     * (re)generated reverse routes are now reverse-capable, while TD5 circuit
     * tracks (no STRIPB.DAT) and any TD6 track lacking reverse routes stay
     * forward-only. This mirrors the original's per-track "reverse offered" data
     * byte (gNpcRacerCheatFlags @0x4a2c9c, indexed by schedule slot) but is
     * driven by reverse-asset presence — which the port has, unlike the original's
     * unlock-progression model.
     *
     * Loose-path EXACT check (no level_num+1 probing — unlike
     * load_first_available_level_entry's try_extracted_level_file_* helpers) so a
     * forward-only track can't false-match a neighbour level's reverse files
     * (e.g. Newcastle level029 -> level030 Drag Strip). */
    static const char *const need[3] = { "STRIPB.DAT", "LEFTB.TRK", "RIGHTB.TRK" };
    /* Pack-on-load: the binary .DATs may be retired in favour of editable
     * sources (stripb.json / leftb.trk.csv / rightb.trk.csv). Accept either,
     * so the forward/reverse toggle stays correct after retirement. */
    static const char *const need_src[3] = { "stripb.json", "leftb.trk.csv",
                                             "rightb.trk.csv" };
    for (int i = 0; i < 3; i++) {
        td5_asset_build_level_loose_path(track_index, need[i],
                                         rev_path, sizeof(rev_path));
        if (td5_plat_file_exists(rev_path))
            continue;
        td5_asset_build_level_loose_path(track_index, need_src[i],
                                         rev_path, sizeof(rev_path));
        if (!td5_plat_file_exists(rev_path))
            return 0;
    }
    return 1;
}

static void *load_first_available_level_entry(int track_index,
                                              const char *const *names,
                                              int name_count,
                                              int *out_size,
                                              char *out_name,
                                              size_t out_name_size)
{
    char zip_path[256];
    char loose_path[256];

    td5_asset_build_level_zip_path(track_index, zip_path, sizeof(zip_path));
    if (out_size) *out_size = 0;
    if (out_name && out_name_size > 0)
        out_name[0] = '\0';

    for (int i = 0; i < name_count; i++) {
        int size = 0;
        void *data = NULL;

        td5_asset_build_level_loose_path(track_index, names[i],
                                         loose_path, sizeof(loose_path));
        if (td5_plat_file_exists(loose_path)) {
            data = td5_asset_open_and_read(loose_path, zip_path, &size);
            if (data && size > 0) {
                if (out_size) *out_size = size;
                if (out_name && out_name_size > 0)
                    snprintf(out_name, out_name_size, "%s", loose_path);
                return data;
            }
            free(data);
        }

        data = td5_asset_open_and_read(names[i], zip_path, &size);
        if (data && size > 0) {
            if (out_size) *out_size = size;
            if (out_name && out_name_size > 0)
                snprintf(out_name, out_name_size, "%s:%s", zip_path, names[i]);
            return data;
        }
        free(data);
    }

    return NULL;
}

/* [LAPS 2026-07-04] Lightweight circuit-vs-point-to-point query for the frontend
 * LAPS selector. Reads DWORD[0] of the track's LEVELINF.DAT — the SAME byte the
 * race uses to set g_td5.track_type (@0x42AE6B: 1 = circuit / laps, else P2P).
 * The frontend can't use td5_asset_track_has_reverse as a proxy: reverse-capable
 * lap circuits (e.g. Newcastle/029) have reverse data yet ARE circuits. Result is
 * cached per track index so track-select cycling doesn't re-open the level zip.
 * Returns 1 = circuit, 0 = point-to-point; a missing/short LEVELINF defaults to
 * circuit, matching the race-load default. Read-only (does not touch
 * g_td5.track_type / g_track_environment_config). */
int td5_asset_track_is_circuit(int track_index)
{
    static int8_t s_circuit_cache[64];
    static int    s_circuit_cache_init = 0;
    static const char *const names[1] = { "LEVELINF.DAT" };
    void *data;
    int   sz = 0, result;

    if (!s_circuit_cache_init) {
        int i;
        for (i = 0; i < 64; i++) s_circuit_cache[i] = -1;   /* -1 = not yet probed */
        s_circuit_cache_init = 1;
    }
    if (track_index < 0) return 1;                          /* random/unset: show laps */
    if (track_index < 64 && s_circuit_cache[track_index] >= 0)
        return s_circuit_cache[track_index];

    data = load_first_available_level_entry(track_index, names, 1, &sz, NULL, 0);
    if (data && sz >= 4) {
        int32_t circuit_flag;
        memcpy(&circuit_flag, data, sizeof(int32_t));
        result = (circuit_flag == 1) ? 1 : 0;
    } else {
        result = 1;   /* missing/short LEVELINF -> circuit (race-load default) */
    }
    free(data);
    if (track_index < 64) s_circuit_cache[track_index] = (int8_t)result;
    return result;
}

int td5_asset_load_level(int track_index)
{
    char zip_path[256];
    char strip_source[256];
    td5_asset_build_level_zip_path(track_index, zip_path, sizeof(zip_path));

    TD5_LOG_I(LOG_TAG, "td5_asset_load_level: track_index=%d -> zip=%s exists=%d",
              track_index, zip_path, td5_plat_file_exists(zip_path));

    int ok = 1;
    int strip_sz = 0;
    int left_sz = 0;
    int right_sz = 0;
    int models_sz = 0;
    int checkpoint_sz = 0;
    int levelinf_sz = 0;
    void *strip_data = NULL;
    void *left_data = NULL;
    void *right_data = NULL;
    void *models_data = NULL;
    void *levelinf_data = NULL;
    const char *checkpoint_name = "CHECKPT.NUM";
    const char *levelinf_name = "LEVELINF.DAT";

    /* g_track_environment_config is now defined at file scope above */

    /* Pick forward or reverse asset filename based on game state.
     * [CONFIRMED @ 0x0042FB90 LoadTrackRuntimeData + 0x0042AA10 InitializeRaceSession]
     * writers of gReverseTrackDirection land here through the frontend's
     * "Direction" toggle (stored into g_td5.reverse_direction). */
    int is_reverse = g_td5.reverse_direction ? 1 : 0;

    /* Forward-only track guard.
     * Not every TD5 track ships reverse-direction data. Reverse-capable
     * tracks include a STRIPB.DAT/LEFTB.TRK/RIGHTB.TRK/TRAFFICB.BUS set
     * alongside the forward files (e.g. Moscow/level023). Forward-only tracks
     * have NO STRIPB.DAT — confirmed by inspecting the original shipped
     * archives: level027 (Courmayeur) and level029 (Newcastle) contain only
     * strip.dat/left.trk/right.trk/traffic.bus, whereas level023 (Moscow)
     * additionally contains stripb.dat/leftb.trk/rightb.trk/trafficb.bus.
     *
     * Requesting reverse on a forward-only track made the STRIPB.DAT lookup
     * below return NULL, so the strip load fell through to
     * td5_track_load_strip(NULL,0) -> placeholder track with span_count=0.
     * Every per-slot spawn-span lookup in the grid loop then returned NULL
     * (td5_track_get_span -> NULL -> `continue`), so all six racers were left
     * at their memset-zero origin: the reported "all cars stacked on top of
     * each other out of bounds" on Courmayeur. (Runtime-confirmed: race.log
     * "Grid start ... span_count=0" with zero per-slot spawn lines.)
     *
     * Probe the reverse strip first; if it is absent, fall back to forward and
     * clear g_td5.reverse_direction so the ENTIRE race pipeline stays
     * forward-consistent — the reverse texture swap (td5_asset.c), the reverse
     * minimap/render span math (td5_render.c) and the reverse span-progress
     * logic (td5_track.c) all gate on g_td5.reverse_direction and would
     * otherwise operate on forward data. The frontend re-applies the user's
     * Direction choice on the next race entry (td5_frontend.c), so this clear
     * does not stick for reverse-capable tracks. */
    if (is_reverse && !td5_asset_track_has_reverse(track_index)) {
        /* Forward-only track requested in reverse. This is the safety net for
         * the INI / --DefaultReverse / AutoRace path that bypasses the
         * frontend (the frontend itself hides the Direction toggle for these
         * tracks — see td5_frontend.c). Force forward and clear
         * g_td5.reverse_direction so the WHOLE pipeline stays
         * forward-consistent: the reverse texture swap (below), the reverse
         * minimap/render span math (td5_render.c) and the reverse span-progress
         * logic (td5_track.c) all gate on g_td5.reverse_direction and would
         * otherwise operate on the forward data we actually load. The frontend
         * re-applies the user's Direction choice on the next race entry, so
         * this clear does not stick for reverse-capable tracks. */
        TD5_LOG_W(LOG_TAG,
                  "load_level: track_index=%d has no reverse strip; "
                  "forcing forward direction (track is forward-only)", track_index);
        is_reverse = 0;
        g_td5.reverse_direction = 0;
    }

    const char **strip_names   = is_reverse ? s_strip_rev   : s_strip_fwd;
    const char **left_names    = is_reverse ? s_left_rev    : s_left_fwd;
    const char **right_names   = is_reverse ? s_right_rev   : s_right_fwd;
    const char **traffic_names = is_reverse ? s_traffic_rev : s_traffic_fwd;
    TD5_LOG_I(LOG_TAG, "load_level: reverse_direction=%d track_index=%d — picking %s / %s / %s / %s",
              is_reverse, track_index,
              strip_names[0], left_names[0], right_names[0], traffic_names[0]);

    strip_data = load_first_available_level_entry(track_index, strip_names, 1,
                                                  &strip_sz, strip_source, sizeof(strip_source));
    if (!strip_data || strip_sz <= 0) {
        TD5_LOG_W(LOG_TAG, "no STRIP.DAT found in %s, using placeholder track", zip_path);
        ok = td5_track_load_strip(NULL, 0) ? ok : 0;
    } else {
        TD5_LOG_I(LOG_TAG, "loaded strip data from %s", strip_source);
        ok = td5_track_load_strip(strip_data, (size_t)strip_sz) ? ok : 0;
    }

    left_data = load_first_available_level_entry(track_index, left_names, 1, &left_sz, NULL, 0);
    right_data = load_first_available_level_entry(track_index, right_names, 1, &right_sz, NULL, 0);
    if ((left_data && left_sz > 0) || (right_data && right_sz > 0)) {
        if (!td5_track_load_routes(left_data, (size_t)left_sz,
                                   right_data, (size_t)right_sz)) {
            TD5_LOG_W(LOG_TAG, "failed to load route tables from %s", zip_path);
            ok = 0;
        }
    }

    {
        char models_source[256];
        static const char *s_models_names[1] = { "MODELS.DAT" };
        models_data = load_first_available_level_entry(track_index, s_models_names, 1,
                                                       &models_sz, models_source, sizeof(models_source));
        if (models_data && models_sz > 0) {
            int parsed = td5_track_parse_models_dat(models_data, (size_t)models_sz);
            TD5_LOG_I(LOG_TAG, "parsed MODELS.DAT: %d meshes from %s", parsed, models_source);
        }
    }

    {
        static const char *s_levelinf_names[1] = { "LEVELINF.DAT" };
        free(g_track_environment_config);
        g_track_environment_config = NULL;
        levelinf_data = load_first_available_level_entry(track_index, s_levelinf_names, 1,
                                                         &levelinf_sz, NULL, 0);
        if (levelinf_data && levelinf_sz >= 4) {
            int32_t circuit_flag;
            memcpy(&circuit_flag, (uint8_t *)levelinf_data, sizeof(int32_t));
            /* DWORD[0]==1 → circuit (show laps); DWORD[0]==0 → point-to-point (no laps).
             * Verified from original binary at 0x42AE6B: cmp %edi,(%eax); je → mov [0x466E94],edi(1) */
            g_td5.track_type = (circuit_flag == 1) ? TD5_TRACK_CIRCUIT : TD5_TRACK_POINT_TO_POINT;
            g_track_environment_config = (uint8_t *)levelinf_data;
            levelinf_data = NULL; /* ownership transferred; do not free */
            TD5_LOG_I(LOG_TAG, "LEVELINF.DAT loaded: track_type=%s (circuit_flag=%d)",
                      g_td5.track_type == TD5_TRACK_CIRCUIT ? "CIRCUIT" : "P2P",
                      circuit_flag);
        } else {
            TD5_LOG_W(LOG_TAG, "missing or short %s in %s, defaulting to circuit", levelinf_name, zip_path);
            g_td5.track_type = TD5_TRACK_CIRCUIT;
            free(levelinf_data);
            levelinf_data = NULL;
        }

        /* [task#14 2026-06-13] TD6 breakable props (LEVEL.TCL — invisible collision
         * volumes on the baked geometry; London ships ~199). load returns NULL on
         * native TD5 / tracks without props, which clears the prop table. */
        {
            /* [task#14 MOV] VISIBLE breakable furniture = level.mov (24-byte recs)
             * drawn as COL_NN.prr meshes — NOT the invisible level.tcl collision
             * footprints the port used to render. [#20 2026-06-17] LEVEL.MOV and
             * LEVELB.MOV are the SAME furniture for the two driving directions (~92%
             * identical positions), NOT a main + branch set — loading BOTH rendered
             * every bench twice ("duplicated benches"). Load only the active
             * direction's table: LEVEL.MOV forward, LEVELB.MOV reverse. */
            static const char *s_prop_names[1]  = { "LEVEL.MOV" };
            static const char *s_propb_names[1] = { "LEVELB.MOV" };
            const char **prop_names = g_td5.reverse_direction ? s_propb_names : s_prop_names;
            int prop_sz = 0;
            void *prop_data = load_first_available_level_entry(track_index, prop_names, 1,
                                                               &prop_sz, NULL, 0);
            td5_track_load_td6_props(prop_data, (size_t)(prop_sz > 0 ? prop_sz : 0));
            free(prop_data);
            /* PROPMESH.BIN = the 8 de-indexed COL furniture meshes the renderer
             * draws per MOV prop (model byte -> mesh). */
            {
                static const char *s_pmesh_names[1] = { "PROPMESH.BIN" };
                int pm_sz = 0;
                void *pm_data = load_first_available_level_entry(track_index, s_pmesh_names, 1,
                                                                 &pm_sz, NULL, 0);
                td5_render_load_td6_prop_meshes(pm_data, (size_t)(pm_sz > 0 ? pm_sz : 0));
                free(pm_data);
            }
        }

        /* [TD6 AUTHORITATIVE TRACK TYPE] For migrated TD6 tracks, derive circuit
         * vs point-to-point from the port's single source of truth
         * (k_td6_menu_slots: finish_span>0 = point-to-point, finish_span==0 =
         * circuit) rather than trusting the synthesized LEVELINF.DAT DWORD[0].
         * This guarantees P2P TD6 city tracks NEVER show the lap counter (the HUD
         * gates TD5_HUD_CIRCUIT_LAPS on g_track_is_circuit, set from track_type)
         * and circuit TD6 tracks always do, independent of whether the gitignored
         * level assets were regenerated with the right circuit flag. Faithful TD5
         * tracks (g_active_td6_level==0, set in td5_asset_level_number above) are
         * untouched and keep their LEVELINF value byte-faithfully. */
        if (g_active_td6_level > 0) {
            int td6_finish = td5_asset_td6_finish_span_for_level(g_active_td6_level);
            int prev = g_td5.track_type;
            g_td5.track_type = (td6_finish > 0) ? TD5_TRACK_POINT_TO_POINT
                                                : TD5_TRACK_CIRCUIT;
            if (g_td5.track_type != prev) {
                TD5_LOG_W(LOG_TAG,
                          "TD6 track_type override: level=%d finish_span=%d -> %s (LEVELINF said %s)",
                          g_active_td6_level, td6_finish,
                          g_td5.track_type == TD5_TRACK_CIRCUIT ? "CIRCUIT" : "P2P",
                          prev == TD5_TRACK_CIRCUIT ? "CIRCUIT" : "P2P");
            } else {
                TD5_LOG_I(LOG_TAG, "TD6 track_type confirmed: level=%d -> %s",
                          g_active_td6_level,
                          g_td5.track_type == TD5_TRACK_CIRCUIT ? "CIRCUIT" : "P2P");
            }
        }
    }
    {
        static const char *s_checkpoint_names[1] = { "CHECKPT.NUM" };
        void *cp_data = load_first_available_level_entry(track_index, s_checkpoint_names, 1,
                                                          &checkpoint_sz, NULL, 0);
        s_checkpoint_data_size = 0;
        memset(s_checkpoint_data, 0, sizeof(s_checkpoint_data));
        if (cp_data && checkpoint_sz > 0) {
            int copy_sz = checkpoint_sz > (int)sizeof(s_checkpoint_data)
                        ? (int)sizeof(s_checkpoint_data) : checkpoint_sz;
            memcpy(s_checkpoint_data, cp_data, (size_t)copy_sz);
            s_checkpoint_data_size = copy_sz;
            TD5_LOG_I(LOG_TAG, "CHECKPT.NUM loaded: %d bytes (count=%d initial_time=%d)",
                      copy_sz, (int)s_checkpoint_data[0],
                      copy_sz >= 4 ? (int)(*(uint16_t *)(s_checkpoint_data + 2)) : 0);
        } else {
            TD5_LOG_W(LOG_TAG, "missing optional entry: %s in %s", checkpoint_name, zip_path);
        }
        free(cp_data);
    }

    /* Bind the per-level forward/reverse boundary sentinel pair.
     * Mirrors LoadTrackRuntimeData @ 0x0042fb90: reads the 40-entry table
     * at original VA 0x00473820 (keyed on level_number - 1) and stores
     * both values in the track module so fwd_rev_handler can use them
     * instead of the wrong `(1, s_span_count - 2)` assumption. */
    td5_track_bind_boundary_sentinels(td5_asset_level_number(track_index));

    /* ---- TRAFFIC.BUS: per-level traffic spawn queue ----
     * 4-byte records: int16 span, u8 flags (bit0 = oncoming direction),
     * u8 lane — terminated by a span == -1 sentinel.
     * Consumed by td5_ai_init_traffic_actors to place slots 6..11 on the
     * track. Without this, traffic actors stay at world_pos (0,0,0) and
     * never render. Buffer is held across the race via s_traffic_queue_buf
     * and freed on the next td5_asset_load_level call. */
    {
        int bus_sz = 0;
        free(s_traffic_queue_buf);
        s_traffic_queue_buf = NULL;
        td5_ai_set_traffic_queue(NULL, 0);
        void *bus_data = load_first_available_level_entry(track_index, traffic_names, 1,
                                                          &bus_sz, NULL, 0);
        if (bus_data && bus_sz >= 4) {
            s_traffic_queue_buf = (uint8_t *)bus_data; /* ownership transferred */
            td5_ai_set_traffic_queue(s_traffic_queue_buf, bus_sz);
            TD5_LOG_I(LOG_TAG, "TRAFFIC.BUS loaded: %d bytes (%d records incl. sentinel)",
                      bus_sz, bus_sz / 4);
        } else {
            TD5_LOG_W(LOG_TAG, "TRAFFIC.BUS missing or short in %s — traffic will be invisible",
                      zip_path);
            free(bus_data);
        }
    }

    TD5_LOG_I(LOG_TAG, "level load complete: %s (strip=%d left=%d right=%d models=%d checkpoint=%d levelinf=%d)",
              zip_path, strip_sz, left_sz, right_sz, models_sz, checkpoint_sz, levelinf_sz);

    free(strip_data);
    free(left_data);
    free(right_data);
    free(models_data);
    return ok;
}

/* ========================================================================
 * Checkpoint Data Accessor (0x42FB90)
 *
 * Returns pointer to loaded CHECKPT.NUM data (up to 96 bytes).
 * First 24 bytes = active checkpoint record for this track.
 * ======================================================================== */

const void *td5_asset_get_checkpoint_data(int *out_size)
{
    if (out_size) *out_size = s_checkpoint_data_size;
    return s_checkpoint_data_size > 0 ? s_checkpoint_data : NULL;
}

/* ========================================================================
 * Reverse-Direction Texture Page Swap (0x0042FD70 + 0x0040B530)
 *
 * Builds s_page_remap[] from CHECKPT.NUM. Identity in forward mode.
 * In reverse mode, walks the 24-int CHECKPT.NUM image as 6 columns of
 * 4 entries (column-major access: ints[col*4 + row]). Discriminator at
 * int[20] selects:
 *   == -1  →  4-col variant: swap (col0[i], col4[i]) and (col1[i], col3[i])
 *   != -1  →  5-col variant: swap (col0[i], col5[i]),
 *                                 (col1[i], col4[i]),
 *                                 (col2[i], col3[i])
 * Each entry is a TEXTURES.DAT page index; -1 entries are skipped.
 *
 * [CONFIRMED @ 0x0042FD70 RemapCheckpointOrderForTrackDirection] —
 *   Ghidra-verified: orig walks &g_checkpointNumTable_PROVISIONAL (0x4aedb0)
 *   stride 1 int until 0x4aedc0 (4 rows). Reads gTrackDirectionSwitchFlag
 *   at 0x004aee00 (which is &table+0x50 = position 20 ints) — port's
 *   `cols[20]` reads the same byte. Pair selection identical (== -1: 4-col
 *   variant pairs [col0,col4]+[col1,col3]; != -1: 5-col variant pairs
 *   [col0,col5]+[col1,col4]+[col2,col3]). -1 skip identical.
 *
 * [ARCH-DIVERGENCE: 0x0040B530 SwapIndexedRuntimeEntries — two-table swap
 *   collapsed to single page_remap table] — orig swaps two parallel tables
 *   under DAT_0048DC40+4 (4-byte entries) and DAT_0048DC40+0xc (8-byte
 *   entries; per-page-descriptor cache header). Port has no separate cache
 *   header (D3D11 backend keys pages by raw index), so only s_page_remap[]
 *   is swapped; the 8-byte descriptor swap has no equivalent. Documented
 *   in file-header comment lines 67-83. L5 promotion sweep 2026-05-21.
 * ======================================================================== */

static void td5_asset_reset_texture_page_remap(void)
{
    int n = (int)(sizeof(s_page_remap) / sizeof(s_page_remap[0]));
    for (int i = 0; i < n; i++) s_page_remap[i] = i;
    s_page_remap_active = 0;
}

int td5_asset_texture_page_remap_source(int slot)
{
    int n = (int)(sizeof(s_page_remap) / sizeof(s_page_remap[0]));
    if (slot < 0 || slot >= n) return slot;
    return s_page_remap[slot];
}

static void td5_asset_swap_remap_pair(int a, int b)
{
    int n = (int)(sizeof(s_page_remap) / sizeof(s_page_remap[0]));
    if (a < 0 || b < 0 || a >= n || b >= n) return;
    int t = s_page_remap[a];
    s_page_remap[a] = s_page_remap[b];
    s_page_remap[b] = t;
}

void td5_asset_apply_reverse_texture_swap(void)
{
    td5_asset_reset_texture_page_remap();

    if (!g_td5.reverse_direction) return;
    if (s_checkpoint_data_size < 96) {
        TD5_LOG_W(LOG_TAG,
                  "reverse texture swap skipped: CHECKPT.NUM too short (%d bytes)",
                  s_checkpoint_data_size);
        return;
    }

    const int32_t *cols = (const int32_t *)s_checkpoint_data;  /* 24 ints */
    int discriminator = cols[20];
    int swap_count = 0;

    for (int row = 0; row < 4; row++) {
        int c0 = cols[0 * 4 + row];
        int c1 = cols[1 * 4 + row];
        int c2 = cols[2 * 4 + row];
        int c3 = cols[3 * 4 + row];
        int c4 = cols[4 * 4 + row];
        int c5 = cols[5 * 4 + row];

        if (discriminator == -1) {
            /* 4-col variant */
            if (c0 != -1 && c4 != -1) { td5_asset_swap_remap_pair(c0, c4); swap_count++; }
            if (c1 != -1 && c3 != -1) { td5_asset_swap_remap_pair(c1, c3); swap_count++; }
        } else {
            /* 5-col variant */
            if (c0 != -1 && c5 != -1) { td5_asset_swap_remap_pair(c0, c5); swap_count++; }
            if (c1 != -1 && c4 != -1) { td5_asset_swap_remap_pair(c1, c4); swap_count++; }
            if (c2 != -1 && c3 != -1) { td5_asset_swap_remap_pair(c2, c3); swap_count++; }
        }
    }

    s_page_remap_active = (swap_count > 0);
    TD5_LOG_I(LOG_TAG,
              "reverse texture swap: variant=%s pairs=%d (reverse_direction=%d, discriminator=%d)",
              discriminator == -1 ? "4col" : "5col",
              swap_count, g_td5.reverse_direction, discriminator);
}

/* ========================================================================
 * Track Texture Loading -- LoadTrackTextureSet (0x00442670)
 *
 * [ARCH-DIVERGENCE: static.hed pointer-rebase + BuildTrackTextureCache call
 *  removed; L5 promotion sweep 2026-05-21] — Ghidra-verified orig at
 *  0x00442670: (1) calls InitializeTrackStripMetadata; (2) reads TEXTURES.DAT
 *  via GetArchiveEntrySize+ReadArchiveEntry+HeapAllocTracked; (3) reads
 *  static.hed via the same archive path; (4) writes gTrackTextureCount with
 *  branch on g_vehicleProjectionEffectMode==2 (split storage between primary
 *  count and g_dualTextureSetSecondHalfBase_PROVISIONAL); (5) loops over
 *  gStaticHedEntryCount entries adding 0x400 to each entry's offset (rebases
 *  the data-section pointers); (6) calls BuildTrackTextureCache.
 *  Port td5_asset_load_track_textures retains the TEXTURES.DAT page-count
 *  and type-byte read (steps 1-2 conceptually) and immediately calls the
 *  reverse-direction page swap, but does NOT walk static.hed for pointer
 *  rebase (port has no separate cache header — pages are uploaded direct to
 *  GPU slots via td5_plat_render_upload_texture) and does NOT branch on the
 *  dual-texture-set effect mode (D3D11 backend treats both modes uniformly).
 *  The actual GPU upload happens later in td5_asset_load_race_texture_pages
 *  (the LoadRaceTexturePages @ 0x00442770 analog). See file-header comment
 *  on lines 67-83 for the cache-header arch-divergence rationale.
 * ======================================================================== */

int td5_asset_load_track_textures(int track_index)
{
    char zip_path[256];
    td5_asset_build_level_zip_path(track_index, zip_path, sizeof(zip_path));
    void *tex_data = NULL;
    int tex_sz = 0;
    int page_count = 0;

    tex_data = td5_asset_open_and_read("TEXTURES.DAT", zip_path, &tex_sz);
    if (tex_data && tex_sz >= 4) {
        page_count = (int)(*(uint32_t *)tex_data);
        if (page_count <= 0 || page_count > TD5_TRACK_TEXTURE_PAGE_LIMIT)
            page_count = 0;
    }
    if (page_count <= 0)
        page_count = TD5_TRACK_TEXTURE_PAGE_LIMIT;

    /* Build the reverse-direction page-remap table from CHECKPT.NUM.
     * Identity when reverse_direction == 0. Must run before any code
     * uses td5_asset_texture_page_remap_source() below. */
    td5_asset_apply_reverse_texture_swap();

    /* Even though pixel data comes from PNGs, the per-page transparency
     * type byte (raw TEXTURES.DAT byte +3 of each page descriptor) is
     * still needed by the renderer to pick the right blend preset.
     * Layout (BuildTrackTextureCacheImpl @ 0x0040B1D0):
     *   uint32 page_count
     *   uint32 offsets[page_count]
     *   each page: byte[3] padding | byte type | ...
     * Type 3 → additive blend (street lights, glows). */
    if (tex_data && tex_sz >= 4 + page_count * 4) {
        const uint32_t *offsets = (const uint32_t *)((const uint8_t *)tex_data + 4);
        int additive_count = 0;
        for (int p = 0; p < page_count; p++) {
            int src = td5_asset_texture_page_remap_source(p);
            if (src < 0 || src >= page_count) src = p;
            uint32_t off = offsets[src];
            if (off + 4 > (uint32_t)tex_sz) continue;
            uint8_t page_type = ((const uint8_t *)tex_data)[off + 3];
            td5_asset_set_page_transparency(p, (int)page_type);
            if (page_type == 3) additive_count++;
        }
        if (additive_count > 0) {
            TD5_LOG_I(LOG_TAG,
                      "track textures: %d type-3 (additive) pages registered",
                      additive_count);
        }
    }

    /* PNG-upload pass removed (was lines 2159-2177): the original's
     * BuildTrackTextureCacheImpl @ 0x0040B1D0 only writes per-page CPU-side
     * metadata (palette decode + transparency-type table) — it performs ZERO
     * GPU uploads. The actual GPU upload happens later in
     * td5_asset_load_race_texture_pages, which is the analog of
     * LoadRaceTexturePages @ 0x00442770. The PNG path here was producing
     * ~475 redundant CreateTexture2D+CreateSRV+UpdateSubresource cycles
     * (and another ~475 2x2 fallbacks for would-be missing pages) per
     * race start, all immediately overwritten by the .dat path.
     *
     * Faithful behavior: skip the upload pass entirely. Pages that the .dat
     * path doesn't cover are intentionally left unbound (the original treats
     * gStaticHedTextureData[i*0x10+4]==0 as "skip", not "fallback"). */
    TD5_LOG_I(LOG_TAG,
              "track textures: %s page_count=%d (metadata-only, GPU upload deferred to load_race_texture_pages — faithful to BuildTrackTextureCacheImpl @ 0x0040B1D0)",
              tex_data ? "TEXTURES.DAT" : "manifest-missing", page_count);

    int parsed = (tex_data != NULL);
    free(tex_data);
    return parsed;
}

/* ------------------------------------------------------------------------
 * [Phase A multithreading 2026-06-08] Parallel race-texture-page decode.
 *
 * The expensive per-page work — palette->BGRA32 expansion + alpha-bleed for the
 * DAT path, and the TD6 native-res PNG decode — is pure-CPU and per-page
 * independent, so it runs across the td5_jobs worker pool. The GPU upload, the
 * shared 1024-page table write (td5_plat_render_upload_texture), the per-page
 * transparency registration, and ALL logging stay on the MAIN thread in a
 * serial pass that consumes the decoded results in page order. The uploaded
 * bytes and the registration order are therefore byte-identical to the old
 * serial loop. Work is batched so peak decode-buffer memory stays bounded on
 * the 32-bit heap (a TD6 256x256 RGBA page is 256 KB; 1024 of them would be
 * 256 MB if decoded all at once).
 * ------------------------------------------------------------------------ */
#define TPAGE_DECODE_BATCH 64

typedef struct {
    int       page;          /* destination page index                       */
    uint8_t  *pixels;        /* malloc'd BGRA, ownership passes to upload pass*/
    int       w, h;
    int       type;          /* page transparency type byte                  */
    int       keyed_pixels;  /* type-1 keyed texel count (for logging)       */
    int       from_png;      /* 1 = TD6 native-res PNG path (for logging)    */
} tpage_result_t;

typedef struct {
    const uint8_t  *tex_data;
    const uint32_t *offsets;
    uint32_t        page_count;
    int             tex_size;
    int             level_number;
    int             td6;
    int             base;          /* batch base page index                  */
    tpage_result_t *results;       /* indexed [0, batch_n)                    */
} tpage_decode_ctx_t;

/* Decode exactly one page into results[i]. Runs on a worker thread: touches
 * only read-only shared inputs + its own result slot, and performs NO logging
 * and NO D3D11/context calls. Mirrors the byte-for-byte decode of the original
 * serial loop body (TD6 PNG path first, DAT palette path as fallback). */
static void tpage_decode_one(int i, void *vctx)
{
    tpage_decode_ctx_t *c = (tpage_decode_ctx_t *)vctx;
    int pg = c->base + i;
    tpage_result_t *r = &c->results[i];
    r->page = pg;
    r->pixels = NULL;
    r->w = r->h = 0;
    r->type = 0;
    r->keyed_pixels = 0;
    r->from_png = 0;

    /* TD6 native-res PNG path (forward-only, no reverse remap). */
    if (c->td6 > 0) {
        char png_path[256];
        void *png_px = NULL;
        int pw = 0, ph = 0;
        snprintf(png_path, sizeof(png_path),
                 "re/assets/levels/level%03d/textures/tex_%03d.png",
                 c->level_number, pg);
        if (td5_plat_file_exists(png_path) &&
            td5_asset_decode_png_rgba32(png_path, &png_px, &pw, &ph) && png_px) {
            uint32_t poff = c->offsets[pg];
            int ptype = (poff + 4 <= (uint32_t)c->tex_size) ? c->tex_data[poff + 3] : 0;
            r->pixels = (uint8_t *)png_px;
            r->w = pw; r->h = ph;
            r->type = ptype;
            r->from_png = 1;
            return;
        }
    }

    /* DAT palette path. */
    int src_i = td5_asset_texture_page_remap_source(pg);
    if (src_i < 0 || (uint32_t)src_i >= c->page_count) src_i = pg;
    uint32_t off = c->offsets[(uint32_t)src_i];
    if (off + 8 > (uint32_t)c->tex_size) return;

    const uint8_t *page_ptr = c->tex_data + off;
    uint8_t  page_type = page_ptr[3];
    int32_t  pal_count = *(const int32_t *)(page_ptr + 4);
    if (pal_count < 0 || pal_count > 256) return;

    const uint8_t *palette = page_ptr + 8;
    const uint8_t *indices = palette + pal_count * 3;
    if (indices + 4096 > c->tex_data + c->tex_size) return;

    uint8_t *rgba = (uint8_t *)malloc(64 * 64 * 4);
    if (!rgba) return;

    int keyed_pixel_count = 0;
    for (int px = 0; px < 4096; px++) {
        int idx = indices[px];
        int ci = idx * 3;
        uint8_t alpha;
        uint8_t b_val, g_val, r_val;

        if (ci + 2 < pal_count * 3) {
            b_val = palette[ci + 0];
            g_val = palette[ci + 1];
            r_val = palette[ci + 2];
        } else {
            b_val = g_val = r_val = 0;
        }

        switch (page_type) {
        case 1: /* Alpha-keyed: index 0 = transparent (RGB zeroed) */
            if (idx == 0) {
                alpha = 0x00;
                b_val = g_val = r_val = 0;
                keyed_pixel_count++;
            } else {
                alpha = 0xFF;
            }
            break;
        case 2: /* Color-key opaque, z-write off (identical PIXEL treatment to
                 * type 1; the z-write-off half is applied at draw time by
                 * TD5_PRESET_TRANSLUCENT_ANISO). BindRaceTexturePage @0x0040B6CC
                 * [CONFIRMED]: the original renders type 2 with the SAME D3D
                 * state as type 1 (SRCALPHA/INVSRCALPHA color-key), only ZWRITE
                 * disabled — it was NEVER a 50% blend. Baking a uniform 0x80
                 * here made every native type-2 page (e.g. the Montego START
                 * banner) render 50% translucent. Bake binary color-key alpha
                 * so opaque texels stay fully opaque under the blend. */
            if (idx == 0) {
                alpha = 0x00;
                b_val = g_val = r_val = 0;
                keyed_pixel_count++;
            } else {
                alpha = 0xFF;
            }
            break;
        case 3: /* Additive: index 0 -> alpha 0 (sprite background) */
            if (idx == 0) {
                alpha = 0x00;
                b_val = g_val = r_val = 0;
            } else {
                alpha = 0xFF;
            }
            break;
        default: /* 0: opaque */
            alpha = 0xFF;
            break;
        }

        rgba[px * 4 + 0] = b_val;
        rgba[px * 4 + 1] = g_val;
        rgba[px * 4 + 2] = r_val;
        rgba[px * 4 + 3] = alpha;
    }

    if (page_type == 1 || page_type == 3)
        alpha_bleed_rgb(rgba, 64, 64);

    r->pixels = rgba;
    r->w = 64; r->h = 64;
    r->type = (int)page_type;
    r->keyed_pixels = keyed_pixel_count;
}

int td5_asset_load_race_texture_pages(void)
{
    /*
     * Load raw 256x256 R5G6B5 texture pages from the level ZIP archive.
     * Original function: LoadRaceTexturePages @ 0x00442770.  Each "tpage%d.dat"
     * inside the level ZIP is 256*256*2 = 131072 bytes of raw 16-bit pixel data.
     *
     * Fallback-page upload removed: the original has no TD5_FALLBACK_TEXTURE_PAGE
     * concept. Pages whose static.hed entry-size is zero are simply skipped
     * (gated at 0x004427AB), leaving the slot unbound. We keep the
     * s_fallback_texture_uploaded flag at 0 so the early-return failure paths
     * below correctly report load failure rather than masking it.
     */

    /* Load textures.dat from level ZIP.
     * Format (from Ghidra FUN_0040b1d0):
     *   uint32 page_count
     *   uint32 offsets[page_count]  — byte offsets from file start to each page
     *   Each page at offset:
     *     byte[3] padding
     *     byte    type (0=opaque, 1=alpha-keyed, 2=semi-transparent, 3=opaque-alt)
     *     int32   palette_entry_count
     *     byte[count*3] palette (BGR, 3 bytes per entry)
     *     byte[4096]    pixel indices (64x64, 1 byte per pixel)
     *   Output: BGRA32 at 64x64 per page.
     */
    char zip_path[256];
    int level_number = td5_asset_level_number(g_td5.track_index);
    snprintf(zip_path, sizeof(zip_path), "level%03d.zip", level_number);

    int tex_size = 0;
    uint8_t *tex_data = (uint8_t *)td5_asset_open_and_read("TEXTURES.DAT", zip_path, &tex_size);
    if (!tex_data || tex_size < 8) {
        TD5_LOG_W(LOG_TAG, "TEXTURES.DAT not found or too small in %s", zip_path);
        if (tex_data) free(tex_data);
        return s_fallback_texture_uploaded;
    }

    uint32_t page_count = *(uint32_t *)tex_data;
    if (page_count == 0 || page_count > 1024 ||
        (uint32_t)tex_size < 4 + page_count * 4) {
        TD5_LOG_W(LOG_TAG, "TEXTURES.DAT invalid page_count=%u size=%d", page_count, tex_size);
        free(tex_data);
        return s_fallback_texture_uploaded;
    }

    uint32_t *offsets = (uint32_t *)(tex_data + 4);
    int loaded_count = 0;

    /* Apply CHECKPT.NUM-driven page swap when racing in reverse direction.
     * Mirrors RemapCheckpointOrderForTrackDirection @ 0x0042FD70 — see
     * td5_asset_apply_reverse_texture_swap. Identity when forward.
     * Runs BEFORE the parallel decode so workers only read the remap table.
     *
     * [TD6 NATIVE-RES TEXTURES] Migrated TD6 tracks ship textures at native
     * resolution as loose PNGs (textures/tex_NNN.png); the decoder uploads
     * those directly instead of the 64x64 8-bit palettized DAT page, falling
     * back to the DAT page when the PNG is absent. The per-page TYPE byte
     * (blend preset) still comes from the DAT. TD6 is forward-only (no remap).
     *
     * [Phase A] Decode is parallelized across the worker pool in batches; the
     * GPU upload + transparency registration + logging run serially on the main
     * thread in page order so output is byte-identical to the old serial loop. */
    td5_asset_apply_reverse_texture_swap();

    int td6 = (g_active_td6_level > 0);
    tpage_result_t results[TPAGE_DECODE_BATCH];
    uint32_t t_decode_start = td5_plat_time_ms();

    for (uint32_t base = 0; base < page_count; base += TPAGE_DECODE_BATCH) {
        uint32_t remain = page_count - base;
        int n = (int)(remain < TPAGE_DECODE_BATCH ? remain : TPAGE_DECODE_BATCH);

        tpage_decode_ctx_t ctx;
        ctx.tex_data     = tex_data;
        ctx.offsets      = offsets;
        ctx.page_count   = page_count;
        ctx.tex_size     = tex_size;
        ctx.level_number = level_number;
        ctx.td6          = td6;
        ctx.base         = (int)base;
        ctx.results      = results;

        /* Parallel decode of this batch (main thread participates). */
        td5_jobs_parallel_for(n, tpage_decode_one, &ctx);

        /* Serial upload + registration, in page order, on the main thread. */
        for (int i = 0; i < n; i++) {
            tpage_result_t *r = &results[i];
            if (!r->pixels)
                continue;
            td5_plat_render_upload_texture(r->page, r->pixels, r->w, r->h, 2);
            td5_asset_set_page_transparency(r->page, r->type);
            if (r->type == 1 && r->keyed_pixels > 0) {
                TD5_LOG_I(LOG_TAG,
                          "race tpage %d: type=1 keyed_pixels=%d/4096 (RGB zeroed)",
                          r->page, r->keyed_pixels);
            }
            if (r->type == 3) {
                TD5_LOG_I(LOG_TAG,
                          "race tpage %d: type=3 ADDITIVE (light/glow sprite)", r->page);
            }
            free(r->pixels);
            r->pixels = NULL;
            loaded_count++;
        }
    }

    uint32_t t_decode_ms = td5_plat_time_ms() - t_decode_start;
    free(tex_data);

    TD5_LOG_I(LOG_TAG, "race texture pages: level=%03d loaded=%d/%u fallback=%d workers=%d decode+upload=%ums",
              level_number, loaded_count, page_count, s_fallback_texture_uploaded,
              td5_jobs_worker_count(), t_decode_ms);
    return loaded_count > 0 || s_fallback_texture_uploaded;
}
/* ========================================================================
 * Environment / Reflection Texture Loading (0x42F990)
 *
 * [ARCH-DIVERGENCE: DDraw archive read + DXD3DTexture upload → PNG decode +
 *  D3D11 upload; L5 promotion sweep 2026-05-21] — Ghidra-verified orig walks
 *  *g_trackEnvironmentMetadataBlob entries, calls ReadArchiveEntry against
 *  environs.zip into a 0x20000 scratch, runs ResampleTexturePageToEntryDimensions
 *  on g_textureUsesTallPageFormat_PROVISIONAL==0 branch, then
 *  UploadRaceTexturePage with descriptor page-id (gStaticHedTextureData+0x3c).
 *  Port replaces the entire DDraw+D3D3 pipeline with PNG decode from
 *  re/assets/environs and direct td5_plat_render_upload_texture. The per-track
 *  metadata layout (count + 0x20-stride name entries) is preserved as a static
 *  table in td5_environs_table.inc.
 * ======================================================================== */

#include "td5_environs_table.inc"

int td5_asset_load_environs_pages(int level_number, int page_base, int max_pages, int *out_pages)
{
    /*
     * LoadEnvironmentTexturePages (0x42F990):
     * Loads the per-track environment textures for the chrome/reflection
     * projection effect on car bodies. The name list + count come from the
     * exe's per-track pointer table at 0x0046bb1c, mirrored in
     * td5_environs_table.inc by re/tools/extract_environs_table.py.
     *
     * `level_number` is the level ZIP number (1..39), i.e. the same index
     * LoadTrackTextureSet passes to InitializeTrackStripMetadata.
     */
    int loaded = 0;
    const TD5_EnvironsTrack *t;

    if (level_number < 0 || level_number >= TD5_ENVIRONS_TRACK_COUNT) {
        TD5_LOG_W(LOG_TAG, "environs: level %d out of range", level_number);
        return 0;
    }

    t = &td5_environs_per_track[level_number];
    if (t->count <= 0) {
        TD5_LOG_I(LOG_TAG, "environs: level %d has no entries", level_number);
        return 0;
    }

    for (int i = 0; i < t->count && i < max_pages && i < 4; i++) {
        void *pixels = NULL;
        int w = 0, h = 0;
        int page_id = page_base + i;
        char png_path[128];
        const char *tga = t->e[i].name;
        char stem[32];
        const char *dot;
        size_t n;

        if (!tga || !tga[0]) continue;

        /* Strip .tga/.TGA extension to get the stem, then build the PNG path. */
        dot = strrchr(tga, '.');
        n = dot ? (size_t)(dot - tga) : strlen(tga);
        if (n >= sizeof(stem)) n = sizeof(stem) - 1;
        memcpy(stem, tga, n);
        stem[n] = '\0';
        snprintf(png_path, sizeof(png_path), "re/assets/environs/%s.png", stem);

        if (!td5_asset_decode_png_rgba32(png_path, &pixels, &w, &h)) {
            TD5_LOG_W(LOG_TAG, "environs: failed to load %s", png_path);
            continue;
        }

        alpha_bleed_rgb((uint8_t *)pixels, w, h);

        if (td5_plat_render_upload_texture(page_id, pixels, w, h, 2)) {
            /* flag 3 (SUN*) uses the sun/alpha projection preset; flag 1 is
             * the default solid environment reflection. The port maps both
             * to transparency type 2 (50% alpha, TRANSLUCENT_ANISO blend).
             * Faithful flag→type dispatch is a follow-up (needs Ghidra RE of
             * the environs bind function to determine flag 1 vs 3 mapping). */
            td5_asset_set_page_transparency(page_id, 2);
            if (out_pages)
                out_pages[loaded] = page_id;
            loaded++;
            TD5_LOG_I(LOG_TAG, "environs: level=%d entry=%d %s -> page %d (%dx%d, flag=%d)",
                      level_number, i, png_path, page_id, w, h, t->e[i].flag);
        }

        stbi_image_free(pixels);
    }

    return loaded;
}

/* ========================================================================
 * Vehicle Asset Loading -- LoadRaceVehicleAssets (0x443280)
 *
 * Phase 1: Query himodel.dat sizes and compute a single allocation.
 * Phase 2: Load himodel.dat + carparam.dat per racer slot.
 * Phase 3: Patch UV coords for 2-car-per-page tiling.
 * Phase 4: Load traffic models from traffic.zip.
 * ======================================================================== */

/* Car ZIP archive paths indexed by car_index — MUST match td5_frontend.c order
 * (original binary pointer table at 0x00466edc, UI order at 0x00463e24). */
static const char *s_car_zip_paths[76] = {
    "cars/vip.zip",  /* 0  - VIPER            */
    "cars/97c.zip",  /* 1  - '97 CAMARO       */
    "cars/frd.zip",  /* 2  - SALEEN MUSTANG   */
    "cars/vet.zip",  /* 3  - '98 CORVETTE     */
    "cars/sky.zip",  /* 4  - SKYLINE          */
    "cars/tvr.zip",  /* 5  - CERBERA          */
    "cars/van.zip",  /* 6  - '98 VANTAGE      */
    "cars/xkr.zip",  /* 7  - XKR              */
    "cars/gto.zip",  /* 8  - GTO              */
    "cars/crg.zip",  /* 9  - '69 CHARGER      */
    "cars/chv.zip",  /* 10 - '70 CHEVELLE     */
    "cars/cud.zip",  /* 11 - CUDA             */
    "cars/cob.zip",  /* 12 - COBRA            */
    "cars/69v.zip",  /* 13 - '69 CORVETTE     */
    "cars/cam.zip",  /* 14 - '69 CAMARO       */
    "cars/mus.zip",  /* 15 - '68 MUSTANG      */
    "cars/atp.zip",  /* 16 - P.VANTAGE        */
    "cars/ss1.zip",  /* 17 - SERIES 1         */
    "cars/128.zip",  /* 18 - SPEED 12         */
    "cars/gtr.zip",  /* 19 - GTS-R            */
    "cars/jag.zip",  /* 20 - XJ220            */
    "cars/cat.zip",  /* 21 - SUPER 7          */
    "cars/sp4.zip",  /* 22 - R390             */
    "cars/c21.zip",  /* 23 - CAT 21           */
    "cars/day.zip",  /* 24 - DAYTONA          */
    "cars/fhm.zip",  /* 25 - '68 MUSTANG HR   */
    "cars/hot.zip",  /* 26 - '69 CAMARO HR    */
    "cars/sp3.zip",  /* 27 - '98 MUSTANG GT   */
    "cars/nis.zip",  /* 28 - HOT DOG          */
    "cars/sp1.zip",  /* 29 - MAUL             */
    "cars/sp8.zip",  /* 30 - PITBULL          */
    "cars/pit.zip",  /* 31 - BEAST            */
    "cars/sp2.zip",  /* 32 - WAGON            */
    "cars/cop.zip",  /* 33 - POLICE CERBERA   */
    "cars/sp5.zip",  /* 34 - POLICE MUSTANG   */
    "cars/sp6.zip",  /* 35 - POLICE CHARGER   */
    "cars/sp7.zip",  /* 36 - POLICE CAMARO    */
    /* --- Test Drive 6 cars (ported; indices 37-75, convert_td6_cars order) --- */
    "cars/390.zip", "cars/400.zip", "cars/atl.zip", "cars/att.zip", "cars/aud.zip",
    "cars/bmw.zip", "cars/cer.zip", "cars/chd.zip", "cars/chr.zip", "cars/cp1.zip",
    "cars/cp2.zip", "cars/cp3.zip", "cars/cp4.zip", "cars/db7.zip", "cars/eli.zip",
    "cars/esp.zip", "cars/flx.zip", "cars/g40.zip", "cars/grf.zip", "cars/gts.zip",
    "cars/lgt.zip", "cars/lit.zip", "cars/lot.zip", "cars/mam.zip", "cars/mcj.zip",
    "cars/mcl.zip", "cars/mgt.zip", "cars/pan.zip", "cars/pro.zip", "cars/pwr.zip",
    "cars/s12.zip", "cars/shl.zip", "cars/sub.zip", "cars/sup.zip", "cars/toy.zip",
    "cars/tur.zip", "cars/tus.zip", "cars/xjr.zip", "cars/xk1.zip",
};

/* [S23] Authored TD6 per-car rear (brake/tail) light positions, extracted from
 * Test Drive 6 param/<code>param.scr :CAR_LIGHTS0/1: by
 * re/tools/extract_td6_carlights.py. Model space, rear = -Z; light0 = +X (right),
 * light1 = -X (left). TD6.exe parses these .scr fields at runtime (CONFIRMED: the
 * exe contains the "CAR_LIGHTS"/"param.scr" parser strings). These REPLACE the
 * values the binary carparam.dat carries at +0x60/+0x68 for TD6 cars, which are a
 * DIFFERENT field and were burying the lights inside the body (.scr differs from
 * the binary +0x60 in 28/39 cars). Donor-param cars (aud/pro/xjr — no .scr) are
 * omitted and fall back to the cardef hardpoint. */
static const struct td6_car_lights {
    const char *code; int16_t l0[3]; int16_t l1[3];
} k_td6_car_lights[] = {
    { "390", {  180,  40, -750}, { -180,  40, -750} },
    { "400", {  208,  42, -743}, { -208,  42, -743} },
    { "atl", {  198,  40, -738}, { -198,  40, -738} },
    { "att", {  237,  24, -725}, { -237,  24, -725} },
    { "bmw", {  268,   7, -664}, { -268,   7, -664} },
    { "cer", {  210,  40, -710}, { -210,  40, -710} },
    { "chd", {  195,  61, -689}, { -195,  61, -689} },
    { "chr", {  180,  50, -790}, { -180,  50, -790} },
    { "cp1", {  196,  64, -750}, { -196,  64, -750} },
    { "cp2", {  200,   0, -760}, { -200,   0, -760} },
    { "cp3", {  180,  10, -670}, { -180,  10, -670} },
    { "cp4", {  210,   0, -690}, { -210,   0, -690} },
    { "db7", {  184,  32, -717}, { -184,  32, -717} },
    { "eli", {  120,  79, -685}, { -120,  79, -685} },
    { "esp", {  178,  26, -600}, { -178,  26, -600} },
    { "flx", {  236,  12, -664}, { -236,  12, -664} },
    { "g40", {  200,  50, -670}, { -200,  50, -670} },
    { "grf", {  194,  14, -740}, { -194,  14, -740} },
    { "gts", {  162,  46, -690}, { -162,  46, -690} },
    { "lgt", {  188,  40, -702}, { -188,  40, -702} },
    { "lit", {  264,  14, -690}, { -264,  14, -690} },
    { "lot", {  190,  55, -670}, { -190,  55, -670} },
    { "mam", {  203,  26, -677}, { -203,  26, -677} },
    { "mcj", {  210,  45, -790}, { -220,  45, -790} },
    { "mcl", {  179,  27, -620}, { -179,  27, -620} },
    { "mgt", {  180,  50, -700}, { -180,  50, -700} },
    { "pan", {  210,   0, -690}, { -210,   0, -690} },
    { "pwr", {  158,  15, -700}, { -158,  15, -700} },
    { "s12", {  231,  83, -700}, { -231,  83, -700} },
    { "shl", {  160,  70, -640}, { -160,  70, -640} },
    { "sub", {  227,  40, -720}, { -227,  40, -720} },
    { "sup", {  194,  51, -741}, { -194,  51, -741} },
    { "toy", {  202, -70, -700}, { -202, -70, -700} },
    { "tur", {  150,  65, -660}, { -150,  65, -660} },
    { "tus", {  210,   0, -690}, { -210,   0, -690} },
    { "xk1", {  128,  35, -735}, { -128,  35, -735} },
};

/* Resolve a "cars/<code>.zip" path to its authored TD6 light entry, or NULL. */
static const struct td6_car_lights *td6_lookup_car_lights(const char *zip_path)
{
    if (!zip_path) return NULL;
    const char *p = strstr(zip_path, "cars/");
    if (!p) return NULL;
    p += 5;
    char code[8];
    int i = 0;
    while (p[i] && p[i] != '.' && i < (int)sizeof(code) - 1) { code[i] = p[i]; i++; }
    code[i] = '\0';
    for (size_t k = 0; k < sizeof(k_td6_car_lights) / sizeof(k_td6_car_lights[0]); k++)
        if (strcmp(code, k_td6_car_lights[k].code) == 0)
            return &k_td6_car_lights[k];
    return NULL;
}

/* ========================================================================
 * Test Drive 6 car support
 *
 * TD6 (same Pitbull engine, one revision up) ships car meshes as
 * render_type 0x104 INDEXED triangle lists: 32-byte vertices
 * (pos.xyz f32 + normal.xyz f32 + uv f32), a single draw command, and an
 * unused normals_offset (0xCDCDCDCD). TD5's renderer instead consumes an
 * EXPANDED per-corner layout: 44-byte TD5_MeshVertex + a parallel 16-byte
 * TD5_VertexNormal stream, with total_vertex_count = sum(tri*3 + quad*4).
 *
 * render_type itself is read by NOTHING in the load/render path
 * [CONFIRMED @ 0x0040AC00 PrepareMeshResource, 0x004314B0 render dispatch],
 * so rather than branch the engine we transcode the geometry at load time:
 * de-index each triangle into 3 expanded corner-vertices, copying pos + uv
 * and the per-vertex normal (which drives td5_render_compute_vertex_lighting
 * @ td5_render.c). The result is a byte-standard TD5 mesh the existing
 * pipeline handles unchanged (prepare_mesh_resource, texture patch, draw).
 * ======================================================================== */
#define TD6_MESH_RENDER_TYPE   0x104
#define TD6_VERTEX_STRIDE      32      /* pos(12) + normal(12) + uv(8) */
#define TD6_CMD_STRIDE         24

static int32_t td6_rd_i32(const uint8_t *p, int off)
{
    int32_t v; memcpy(&v, p + off, 4); return v;
}
static float td6_rd_f32(const uint8_t *p, int off)
{
    float v; memcpy(&v, p + off, 4); return v;
}

/* Returns a newly-malloc'd TD5-format mesh (file-relative offsets, ready for
 * td5_track_prepare_mesh_resource) transcoded from a TD6 render_type 0x104
 * himodel, or NULL if src is not TD6 format / is malformed. *out_size is set
 * on success. Caller owns the returned buffer. */
static void *td5_asset_transcode_td6_mesh(const void *src_data, int src_size,
                                          int *out_size)
{
    const uint8_t *src = (const uint8_t *)src_data;
    if (!src || src_size < 0x40) return NULL;

    int16_t render_type;
    memcpy(&render_type, src + 0, 2);
    if (render_type != TD6_MESH_RENDER_TYPE) return NULL;   /* not a TD6 mesh */

    int32_t  cmd_count  = td6_rd_i32(src, 0x04);
    int32_t  vsrc_count = td6_rd_i32(src, 0x08);
    float    radius     = td6_rd_f32(src, 0x0C);
    float    cx = td6_rd_f32(src, 0x10), cy = td6_rd_f32(src, 0x14), cz = td6_rd_f32(src, 0x18);
    float    ox = td6_rd_f32(src, 0x1C), oy = td6_rd_f32(src, 0x20), oz = td6_rd_f32(src, 0x24);
    uint32_t cmds_off  = (uint32_t)td6_rd_i32(src, 0x2C);
    uint32_t verts_off = (uint32_t)td6_rd_i32(src, 0x30);

    if (cmd_count < 1 || cmd_count > 64) return NULL;
    if (vsrc_count <= 0 || vsrc_count > 65535) return NULL;
    if ((uint64_t)verts_off + (uint64_t)vsrc_count * TD6_VERTEX_STRIDE > (uint64_t)src_size)
        return NULL;
    if ((uint64_t)cmds_off + (uint64_t)cmd_count * TD6_CMD_STRIDE > (uint64_t)src_size)
        return NULL;

    /* Pass 1: total expanded vertex (== total index) count across all commands. */
    int total_idx = 0;
    for (int c = 0; c < cmd_count; c++) {
        const uint8_t *cmd = src + cmds_off + (size_t)c * TD6_CMD_STRIDE;
        int32_t idx_count = td6_rd_i32(cmd, 0x0C);
        int32_t idx_off   = td6_rd_i32(cmd, 0x14);
        if (idx_count < 0 || idx_count % 3 != 0) return NULL;
        if (idx_off < 0 ||
            (uint64_t)idx_off + (uint64_t)idx_count * 2 > (uint64_t)src_size) return NULL;
        total_idx += idx_count;
    }
    if (total_idx <= 0 || total_idx > 200000) return NULL;

    /* TD5 layout: header + ONE command + expanded verts(44 each) + a parallel
     * normal stream(16 each). Use sizeof() for the header (0x38) and command
     * (0x10) — the header runs through normals_offset at +0x34, so placing the
     * command any earlier would clobber the offset fields. All TD6 cars use a
     * single body texture, so merging every command's triangles into one
     * output command is exact. */
    size_t header_sz = sizeof(TD5_MeshHeader), cmd_sz = sizeof(TD5_PrimitiveCmd);
    size_t verts_sz  = (size_t)total_idx * sizeof(TD5_MeshVertex);
    size_t norms_sz  = (size_t)total_idx * sizeof(TD5_VertexNormal);
    size_t total_sz  = header_sz + cmd_sz + verts_sz + norms_sz;

    uint8_t *out = (uint8_t *)calloc(1, total_sz);
    if (!out) return NULL;

    uint32_t out_cmds_off  = (uint32_t)header_sz;
    uint32_t out_verts_off = (uint32_t)(header_sz + cmd_sz);
    uint32_t out_norms_off = (uint32_t)(header_sz + cmd_sz + verts_sz);

    TD5_MeshHeader *h = (TD5_MeshHeader *)out;
    h->render_type        = 0x103;       /* present as a native TD5 mesh */
    h->texture_page_id    = 0;           /* patched to skin_page by the caller */
    h->command_count      = 1;
    h->total_vertex_count = total_idx;
    h->bounding_radius    = radius;
    h->bounding_center_x  = cx;
    h->bounding_center_y  = cy;
    h->bounding_center_z  = cz;
    h->origin_x           = ox;
    h->origin_y           = oy;
    h->origin_z           = oz;
    h->reserved_28        = 0;
    h->commands_offset    = out_cmds_off;
    h->vertices_offset    = out_verts_off;
    h->normals_offset     = out_norms_off;

    TD5_PrimitiveCmd *ocmd = (TD5_PrimitiveCmd *)(out + out_cmds_off);
    ocmd->dispatch_type   = 0;
    ocmd->texture_page_id = 7;           /* body id -> caller patches to skin_page */
    ocmd->reserved_04     = 0;
    ocmd->triangle_count  = (uint16_t)(total_idx / 3);
    ocmd->quad_count      = 0;
    ocmd->vertex_data_ptr = 0;

    TD5_MeshVertex   *overts = (TD5_MeshVertex   *)(out + out_verts_off);
    TD5_VertexNormal *onorms = (TD5_VertexNormal *)(out + out_norms_off);

    int w = 0;   /* expanded write cursor */
    for (int c = 0; c < cmd_count; c++) {
        const uint8_t *cmd = src + cmds_off + (size_t)c * TD6_CMD_STRIDE;
        int32_t idx_count = td6_rd_i32(cmd, 0x0C);
        int32_t idx_off   = td6_rd_i32(cmd, 0x14);
        const uint8_t *idx = src + idx_off;
        for (int k = 0; k < idx_count; k++) {
            uint16_t vi;
            memcpy(&vi, idx + (size_t)k * 2, 2);
            if (vi >= vsrc_count) { free(out); return NULL; }
            const uint8_t *sv = src + verts_off + (size_t)vi * TD6_VERTEX_STRIDE;
            overts[w].pos_x  = td6_rd_f32(sv, 0x00);
            overts[w].pos_y  = td6_rd_f32(sv, 0x04);
            overts[w].pos_z  = td6_rd_f32(sv, 0x08);
            overts[w].view_x = overts[w].view_y = overts[w].view_z = 0.0f;
            overts[w].lighting = 0xFF;   /* overwritten by compute_vertex_lighting */
            overts[w].tex_u  = td6_rd_f32(sv, 0x18);
            overts[w].tex_v  = td6_rd_f32(sv, 0x1C);
            overts[w].proj_u = overts[w].proj_v = 0.0f;
            onorms[w].nx = td6_rd_f32(sv, 0x0C);
            onorms[w].ny = td6_rd_f32(sv, 0x10);
            onorms[w].nz = td6_rd_f32(sv, 0x14);
            onorms[w].visible_flag = 1;
            w++;
        }
    }

    if (out_size) *out_size = (int)total_sz;
    return out;
}

/* Player-slot car override (TD6-car test hook). When set to a 3-letter archive
 * code, the player (slot 0) loads cars/<code>.zip -> re/assets/cars/<code>/
 * regardless of the menu / DefaultCar selection. AI slots are unaffected, so
 * the frontend's 37-car roster is untouched (no menu regression). */
static char s_player_car_override[16] = {0};

/* Per-race-slot TD6 body-colour override (0xRRGGBB), -1 = no override.
 * Set by the frontend's simultaneous-multiplayer car select so EACH human's
 * ported-TD6 car is painted that player's chosen colour, instead of the single
 * global INI colour (slot 0) / the hashed AI palette (slots >=1). Consulted in
 * td5_asset_load_vehicle when baking the carmask paint. -1 leaves the default
 * behaviour intact for single-player / AI. [PORT ENHANCEMENT 2026-06-07] */
static int32_t s_human_td6_color[TD5_MAX_RACER_SLOTS];
/* [SECONDARY PAINT 2026-06-29] Companion per-slot secondary colour + pattern.
 * -1 secondary / 0 pattern (SOLID) = "single solid colour" (the legacy
 * behaviour), so MP/net slots set only via td5_asset_set_human_td6_color stay
 * solid. The full picker (single-player) drives these through the INI path in
 * td5_asset_load_vehicle instead. */
static int32_t s_human_td6_color2[TD5_MAX_RACER_SLOTS];
static int32_t s_human_td6_pattern[TD5_MAX_RACER_SLOTS];
static int     s_human_td6_color_init;

static void td5_asset_human_td6_init(void)
{
    int i;
    if (s_human_td6_color_init) return;
    for (i = 0; i < TD5_MAX_RACER_SLOTS; i++) {
        s_human_td6_color[i]   = -1;
        s_human_td6_color2[i]  = -1;
        s_human_td6_pattern[i] = 0;
    }
    s_human_td6_color_init = 1;
}

void td5_asset_set_human_td6_color(int slot, int rgb)
{
    td5_asset_human_td6_init();
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    s_human_td6_color[slot]   = rgb;
    s_human_td6_color2[slot]  = -1;   /* solid unless td5_asset_set_human_td6_paint follows */
    s_human_td6_pattern[slot] = 0;
}

/* Richer per-slot paint setter: primary + secondary + pattern. rgb2<0 or
 * pattern==SOLID leaves the slot rendering a single solid colour. */
void td5_asset_set_human_td6_paint(int slot, int rgb, int rgb2, int pattern)
{
    td5_asset_human_td6_init();
    if (slot < 0 || slot >= TD5_MAX_RACER_SLOTS) return;
    s_human_td6_color[slot]   = rgb;
    s_human_td6_color2[slot]  = rgb2;
    s_human_td6_pattern[slot] = (pattern >= 0 && pattern <= 3) ? pattern : 0;
}

void td5_asset_set_player_car_override(const char *code)
{
    if (code && code[0])
        snprintf(s_player_car_override, sizeof(s_player_car_override), "%s", code);
    else
        s_player_car_override[0] = '\0';
}

int td5_asset_player_override_active(void)
{
    return s_player_car_override[0] != '\0';
}

const char *td5_asset_get_player_override_zip(void)
{
    static char buf[32];
    if (!s_player_car_override[0]) return NULL;
    snprintf(buf, sizeof(buf), "cars/%s.zip", s_player_car_override);
    return buf;
}

/* [SECONDARY PAINT 2026-06-29] Pattern blend: given a body texel's position
 * NORMALISED to the body's bounding box (fx,fy in 0..1) return the blend weight
 * toward the SECONDARY colour (0 = primary, 1 = secondary). The carskin is a
 * per-car UV atlas, so these texture-space splits read as approximate two-tones
 * (front->rear maps L->R, upper->lower maps top->bottom on most cars). The menu
 * preview applies the same rule in the cleaner carpicpaint photo space. Keep
 * thresholds in sync with TD6_PAT_* in td5_frontend_internal.h. */
static float td5_asset_paint_pattern_t(int pattern, float fx, float fy)
{
    switch (pattern) {
        case 1: /* TWO-TONE: upper body = primary, lower = secondary */
            return (fy >= 0.50f) ? 1.0f : 0.0f;
        case 2: /* STRIPES: centre band = secondary */
            return (fx >= 0.42f && fx <= 0.58f) ? 1.0f : 0.0f;
        case 3: /* SPLIT: front-ish = primary, rear-ish = secondary */
            return (fx >= 0.50f) ? 1.0f : 0.0f;
        default: /* SOLID */
            return 0.0f;
    }
}

/* TD6 player paint: load the (body-grayscale + fixed-colour) carskin and the
 * carmask, multiply ONLY the masked body texels by the paint colour, and upload.
 * Glass/lights/chrome/tyres (mask=0) keep their original colour. Returns 0 on
 * any failure so the caller falls back to a plain skin load. The mesh is then
 * drawn with NO per-vertex tint, so the body colour comes solely from here.
 *
 * [SECONDARY PAINT 2026-06-29] When pattern != SOLID the masked body is split
 * between `paint` (primary) and `paint2` (secondary) by td5_asset_paint_pattern_t
 * over the body's bounding box. pattern==SOLID ignores paint2 (legacy path). */
static int td5_asset_load_vehicle_skin_painted(int page, const char *skin_path,
                                               const char *mask_path, uint32_t paint,
                                               uint32_t paint2, int pattern)
{
    void *spix = NULL, *mpix = NULL;
    int sw = 0, sh = 0, mw = 0, mh = 0;
    if (!td5_asset_load_png_to_buffer(skin_path, TD5_COLORKEY_NONE, &spix, &sw, &sh))
        return 0;
    if (!td5_asset_load_png_to_buffer(mask_path, TD5_COLORKEY_NONE, &mpix, &mw, &mh)) {
        stbi_image_free(spix);
        return 0;
    }
    if (mw != sw || mh != sh) {
        stbi_image_free(spix); stbi_image_free(mpix);
        return 0;
    }
    unsigned char *s = (unsigned char *)spix;
    unsigned char *m = (unsigned char *)mpix;
    int tr  = (paint  >> 16) & 0xFF, tg  = (paint  >> 8) & 0xFF, tb  = paint  & 0xFF;
    int tr2 = (paint2 >> 16) & 0xFF, tg2 = (paint2 >> 8) & 0xFF, tb2 = paint2 & 0xFF;
    if (pattern < 0 || pattern > 3) pattern = 0;

    /* Body bounding box (mask>127) for normalised pattern coords. Only needed
     * for non-SOLID patterns. */
    int x0 = sw, y0 = sh, x1 = -1, y1 = -1;
    if (pattern != 0) {
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++)
                if (m[(y * sw + x) * 4] > 127) {
                    if (x < x0) x0 = x;
                    if (x > x1) x1 = x;
                    if (y < y0) y0 = y;
                    if (y > y1) y1 = y;
                }
        if (x1 < x0 || y1 < y0) pattern = 0;   /* no body texels -> solid */
    }
    float inv_w = (pattern != 0 && x1 > x0) ? 1.0f / (float)(x1 - x0) : 0.0f;
    float inv_h = (pattern != 0 && y1 > y0) ? 1.0f / (float)(y1 - y0) : 0.0f;

    /* td5_asset_decode_png_rgba32 returns BGRA buffers (byte0=B, byte1=G,
     * byte2=R) to match the engine's B8G8R8A8 textures, so write tb->byte0 and
     * tr->byte2 (NOT the other way) or blue paint comes out red/orange. */
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            int i = y * sw + x;
            if (m[i * 4] > 127) {          /* body texel (grayscale) -> lum * tint */
                int g = s[i * 4];          /* body is R==G==B, so any byte is the luminance */
                int cr = tr, cg = tg, cb = tb;
                if (pattern != 0) {
                    float fx = (float)(x - x0) * inv_w;
                    float fy = (float)(y - y0) * inv_h;
                    float t  = td5_asset_paint_pattern_t(pattern, fx, fy);
                    if (t > 0.5f) { cr = tr2; cg = tg2; cb = tb2; }
                }
                s[i * 4 + 0] = (unsigned char)((g * cb) / 255);   /* B */
                s[i * 4 + 1] = (unsigned char)((g * cg) / 255);   /* G */
                s[i * 4 + 2] = (unsigned char)((g * cr) / 255);   /* R */
            }
            s[i * 4 + 3] = 255;            /* keep opaque (mask alpha unused on GPU) */
        }
    }
    int ok = td5_plat_render_upload_texture(page, spix, sw, sh, 2);
    stbi_image_free(spix); stbi_image_free(mpix);
    return ok;
}

/* Pick a body colour for a TD6 AI opponent. TD6 carskin0..3 are all the same
 * grey/white base (the real game tints the masked body at runtime), so without
 * a tint every TD6 AI car renders white/grey. Deterministic from
 * (car_index, slot, variant): the slot term spreads the palette so adjacent
 * opponents get distinct colours, and the per-race-random variant rotates the
 * field race-to-race. Returns 0xRRGGBB (never pure white, so it always paints). */
static uint32_t td5_asset_pick_ai_td6_color(int car_index, int slot, int variant)
{
    static const uint32_t k_ai_td6_palette[] = {
        0xD02020, /* red      */ 0x2048D0, /* blue     */ 0x1F9F30, /* green    */
        0xE0C020, /* yellow   */ 0xE07020, /* orange   */ 0x8C28C0, /* purple   */
        0x20B8B8, /* cyan     */ 0xE040A0, /* pink     */ 0xD8D8D8, /* silver   */
        0x687888, /* slate    */ 0x303848, /* gunmetal */ 0x86D020, /* lime     */
        0x208878, /* teal     */ 0x903020, /* maroon   */ 0x203088, /* navy     */
        0xC0A038, /* gold     */
    };
    int n = (int)(sizeof(k_ai_td6_palette) / sizeof(k_ai_td6_palette[0]));
    unsigned h = (unsigned)(car_index * 3 + slot * 5 + variant * 7);
    return k_ai_td6_palette[h % (unsigned)n];
}

int td5_asset_load_vehicle(int car_index, int slot, int paint)
{
    char zip_path[256];
    char skin_tga[32];
    char hub_tga[32];
    /* 4 paint schemes per car (carskin0..3 / carhub0..3); clamp out-of-range to
     * the default. Cop/police indices keep paint 0 because the car-select screen
     * gates their paint cycle to 0, so a 0 arrives here for them. */
    if (paint < 0 || paint > 3) paint = 0;
    snprintf(skin_tga, sizeof(skin_tga), "carskin%d.tga", paint);
    snprintf(hub_tga,  sizeof(hub_tga),  "carhub%d.tga",  paint);
    if (slot == 0 && s_player_car_override[0]) {
        /* TD6-car test hook: force the player into a ported TD6 archive. */
        snprintf(zip_path, sizeof(zip_path), "cars/%s.zip", s_player_car_override);
        TD5_LOG_I(LOG_TAG, "vehicle slot=0: TD6 player override -> %s", zip_path);
    } else if (car_index >= 0 &&
               car_index < (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0]))) {
        snprintf(zip_path, sizeof(zip_path), "%s", s_car_zip_paths[car_index]);
    } else if (car_index >= (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])) &&
               td5_customcar_zip_path(car_index -
                   (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])))) {
        /* Drop-in custom car (re/assets/cars/custom_<name>/) at roster slot 76+. */
        const char *cz = td5_customcar_zip_path(car_index -
                   (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0])));
        snprintf(zip_path, sizeof(zip_path), "%s", cz);
        TD5_LOG_I(LOG_TAG, "vehicle slot=%d: custom car index=%d -> %s",
                  slot, car_index, cz);
    } else {
        snprintf(zip_path, sizeof(zip_path), "cars/car%02d.zip", car_index);
    }

    /* --- Load himodel.dat ------------------------------------------------ */
    int mesh_size = 0;
    void *mesh_data = td5_asset_open_and_read("himodel.dat", zip_path, &mesh_size);
    if (!mesh_data) {
        TD5_LOG_W(LOG_TAG, "vehicle slot=%d car=%d: himodel.dat not found in %s",
                  slot, car_index, zip_path);
        return 0;
    }

    if (mesh_size < (int)sizeof(TD5_MeshHeader)) {
        TD5_LOG_W(LOG_TAG, "vehicle slot=%d car=%d: himodel.dat too small (%d bytes)",
                  slot, car_index, mesh_size);
        free(mesh_data);
        return 0;
    }

    /* TD6 cars ship a render_type 0x104 indexed mesh — transcode it to the
     * TD5 expanded-vertex format the renderer consumes. A non-TD6 mesh
     * (render_type 0x103) returns NULL here and is used as-is. */
    int is_td6_mesh = 0;
    {
        int td6_size = 0;
        void *td6_mesh = td5_asset_transcode_td6_mesh(mesh_data, mesh_size, &td6_size);
        if (td6_mesh) {
            free(mesh_data);
            mesh_data = td6_mesh;
            mesh_size = td6_size;
            is_td6_mesh = 1;
            TD5_LOG_I(LOG_TAG,
                      "vehicle slot=%d car=%d: transcoded TD6 mesh (%d expanded verts)",
                      slot, car_index, ((TD5_MeshHeader *)mesh_data)->total_vertex_count);
        }
    }

    /* The buffer is kept alive -- the render system holds a pointer to it. */
    TD5_MeshHeader *mesh = (TD5_MeshHeader *)mesh_data;

    /* Relocate internal offsets (commands, vertices, normals) to absolute ptrs */
    td5_track_prepare_mesh_resource(mesh);

    /* Vehicle mesh data is in integer-coord float space, same as
     * MODELS.DAT and the camera/render coordinate system.
     * No rescaling needed. */

    /* Register mesh with render system */
    td5_render_set_vehicle_mesh(slot, mesh);
    /* Flag TD6 cars so the renderer skips the chrome/projection reflection
     * overlay (misrenders as a "lights shader" on their grayscale body). Must
     * follow set_vehicle_mesh, which resets the flag. */
    td5_render_set_vehicle_is_td6(slot, is_td6_mesh);

    /* [S23] Install the authored TD6 rear/brake-light positions for this slot.
     * The binary carparam.dat the converter copied has the WRONG values at
     * +0x60/+0x68 for TD6 cars (that is not TD6's CAR_LIGHTS field), which buried
     * the lights inside the body. Use the authored :CAR_LIGHTS0/1: from the code
     * table; TD5 cars and donor-param TD6 cars (no .scr) fall back to the cardef
     * hardpoint (set_vehicle_mesh already cleared the override). */
    if (is_td6_mesh) {
        const struct td6_car_lights *cl = td6_lookup_car_lights(zip_path);
        if (cl) {
            td5_render_set_vehicle_taillights(slot, cl->l0, cl->l1);
            TD5_LOG_I(LOG_TAG,
                      "vehicle slot=%d: TD6 CAR_LIGHTS %s -> (%d,%d,%d)/(%d,%d,%d)",
                      slot, cl->code, cl->l0[0], cl->l0[1], cl->l0[2],
                      cl->l1[0], cl->l1[1], cl->l1[2]);
        }
    }

    TD5_LOG_I(LOG_TAG,
              "vehicle slot=%d car=%d: himodel.dat loaded (%d bytes, %d verts, %d cmds)",
              slot, car_index, mesh_size,
              mesh->total_vertex_count, mesh->command_count);

    /* --- Car textures ---------------------------------------------------- */
    /* All car meshes use exactly 2 PrimitiveCmd page IDs:
     *   7 → carskin0.tga  (body/paint)
     *   8 → carhub0.tga   (wheel hub)
     * These are track-relative indices in himodel.dat and must be patched to
     * dedicated car texture pages so they don't show track geometry textures.
     *
     * Allocation: pages 800 + slot*2 = skin, 800 + slot*2 + 1 = hub.
     * 6 slots → pages 800-811, well above any track's page_count (<600). */
#define TD5_CAR_TEXTURE_PAGE_BASE 800
#define TD5_CAR_MESH_HUB_ID   8
    {
        int skin_page = TD5_CAR_TEXTURE_PAGE_BASE + slot * 2;
        int hub_page  = TD5_CAR_TEXTURE_PAGE_BASE + slot * 2 + 1;

        /* Car skin */
        {
            char png_skin[256];
            int skin_ok = 0;
            if (td5_asset_resolve_png_path(skin_tga, zip_path, png_skin, sizeof(png_skin))) {
                /* TD6 cars: bake a paint colour into the body texels only
                 * (carmask.png), leaving glass/lights/chrome/tyres untouched.
                 *   slot 0 (player)  -> the chosen INI colour (white = leave grey)
                 *   slot >=1 (AI)     -> a per-car colour so the TD6 opponent
                 *                        field isn't all white/grey (the carskin
                 *                        variants are identical grey base art).
                 * The booth (photo-booth) is skipped so its carpic keeps the grey
                 * body. Non-TD6 cars have no carmask -> plain load with their own
                 * pre-painted carskinN. */
                char png_mask[256];
                if (!td5_render_photobooth_active() &&
                    td5_asset_resolve_png_path("carmask.png", zip_path, png_mask, sizeof(png_mask))) {
                    uint32_t paint_rgb;
                    /* [SECONDARY PAINT 2026-06-29] secondary colour + pattern.
                     * Default = solid (paint2 unused, pattern SOLID). The full
                     * picker only drives slot 0 via the INI; per-slot MP overrides
                     * stay solid unless set via td5_asset_set_human_td6_paint. */
                    uint32_t paint_rgb2 = 0x00FFFFFFu;
                    int      paint_pat  = 0;
                    if (s_human_td6_color_init && slot >= 0 &&
                        slot < TD5_MAX_RACER_SLOTS && s_human_td6_color[slot] >= 0) {
                        /* Per-player chosen colour (simultaneous-MP car select). */
                        paint_rgb = (uint32_t)s_human_td6_color[slot] & 0x00FFFFFFu;
                        if (s_human_td6_color2[slot] >= 0) {
                            paint_rgb2 = (uint32_t)s_human_td6_color2[slot] & 0x00FFFFFFu;
                            paint_pat  = s_human_td6_pattern[slot];
                        }
                    } else if (slot == 0) {
                        paint_rgb  = (uint32_t)g_td5.ini.td6_paint_color  & 0x00FFFFFFu;
                        paint_rgb2 = (uint32_t)g_td5.ini.td6_paint_color2 & 0x00FFFFFFu;
                        paint_pat  = g_td5.ini.td6_paint_pattern;
                    } else {
                        paint_rgb = td5_asset_pick_ai_td6_color(car_index, slot, paint);
                    }
                    /* Bake when the body actually changes: a non-white primary,
                     * OR a non-SOLID pattern with a distinct secondary. */
                    if (paint_rgb != 0x00FFFFFFu ||
                        (paint_pat != 0 && paint_rgb2 != paint_rgb)) {
                        skin_ok = td5_asset_load_vehicle_skin_painted(skin_page, png_skin,
                                                                     png_mask, paint_rgb,
                                                                     paint_rgb2, paint_pat);
                        if (skin_ok)
                            TD5_LOG_I(LOG_TAG,
                                      "vehicle slot=%d: TD6 body painted %06X/%06X pat=%d (mask)",
                                      slot, paint_rgb, paint_rgb2, paint_pat);
                    }
                }
                if (!skin_ok)
                    skin_ok = td5_asset_load_png_texture(skin_page, png_skin, TD5_COLORKEY_NONE);
            }
            if (!skin_ok)
                TD5_LOG_W(LOG_TAG, "vehicle slot=%d: %s PNG not found in %s", slot, skin_tga, zip_path);
        }

        /* Hub-cap: Pitbull's carhubN.png is ALREADY a 4-frame motion-blur
         * sheet — 64×64 RGBA containing four 32×32 sub-frames in a 2×2 layout.
         * carhub0..carhub3.png are byte-identical (md5 verified) — likely 4
         * file aliases for the 4 wheels. Load carhub0.png as a single 64×64
         * page and the renderer samples one 32×32 sub-tile per spin frame.
         *
         * Commit 259c57b incorrectly composited all 4 (identical) PNGs into
         * 128×128 treating each PNG as ONE frame — each "tile" then contained
         * the whole 4-frame sheet, rendering as "4 wheels in one" with golden
         * tint from LINEAR sampling across sub-frame boundaries. */
        {
            char png_hub[256];
            int hub_ok = 0;
            /* carhub0..3 are byte-identical per car (md5-verified) — the hub is
             * paint-independent — so carhub%d resolves to the same pixels as
             * carhub0; building %d here matches the original's "CARHUB%d.TGA"
             * [CONFIRMED @ 0x00442B12] without changing the rendered result. */
            if (td5_asset_resolve_png_path(hub_tga, zip_path,
                                           png_hub, sizeof(png_hub))) {
                hub_ok = td5_asset_load_png_texture(hub_page, png_hub, TD5_COLORKEY_NONE);
            }
            if (!hub_ok)
                TD5_LOG_W(LOG_TAG, "vehicle slot=%d: %s load failed for %s",
                          slot, hub_tga, zip_path);
            else
                TD5_LOG_I(LOG_TAG, "vehicle slot=%d: %s page=%d (64x64, 2x2 sub-frames)",
                          slot, hub_tga, hub_page);
        }

        /* Patch PrimitiveCmd page IDs: cmd[0]→skin, cmd[1]→chassis underside.
         * [CONFIRMED @ 0x443280 LoadRaceVehicleAssets] original patches cmd[0]
         * and cmd[1] by INDEX, not by existing page_id — cmd[0] gets the skin
         * atlas page (`uVar2 + slot/2`), cmd[1] gets the CHASSIS sprite page
         * via PatchModelUVCoordsForTrackLighting @ 0x44374D. The ss1 (Shelby
         * Series 1) himodel.dat ships with cmd[0].page_id=36 instead of the
         * usual 7 — an index-based patch handles it, a page_id==7 check does
         * not (body would render with track texture 36 instead of carskin0). */
        mesh->texture_page_id = (int16_t)skin_page;
        TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
        TD5_MeshVertex *base_verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
        int vert_cursor = 0;
        for (int c = 0; c < mesh->command_count; c++) {
            int vert_count = cmds[c].triangle_count * 3 + cmds[c].quad_count * 4;
            if (cmds[c].texture_page_id == TD5_CAR_MESH_HUB_ID) {
                /* Underside polygons → CHASSIS sprite on static atlas page
                 * [CONFIRMED @ 0x44374D, 0x443794-0x4437B9] */
                cmds[c].texture_page_id = (int16_t)s_chassis_page;

                /* Remap vertex UVs into the CHASSIS sub-region.
                 * vertex_data_ptr is 0 at load time (relocated by renderer),
                 * so we walk base_verts with a running cursor. */
                TD5_MeshVertex *cmd_verts = base_verts + vert_cursor;
                for (int v = 0; v < vert_count; v++) {
                    cmd_verts[v].tex_u = s_chassis_uv_w * cmd_verts[v].tex_u + s_chassis_uv_x;
                    cmd_verts[v].tex_v = s_chassis_uv_h * cmd_verts[v].tex_v + s_chassis_uv_y;
                }
                TD5_LOG_I(LOG_TAG, "vehicle slot=%d cmd[%d]: remapped %d verts to chassis page=%d uv=(%.3f,%.3f)+(%.3f,%.3f)",
                          slot, c, vert_count, s_chassis_page,
                          s_chassis_uv_x, s_chassis_uv_y, s_chassis_uv_w, s_chassis_uv_h);
            } else {
                /* Every non-chassis command is the body skin. Most cars ship
                 * with page_id=7 here; ss1 ships with 36. Patch unconditionally
                 * so the mesh always points at the per-slot skin page. */
                cmds[c].texture_page_id = (int16_t)skin_page;
            }
            vert_cursor += vert_count;
        }
    }

    /* --- carparam.dat ---------------------------------------------------- */
    /* Layout (0x10C = 268 bytes):
     *   0x00..0x8B: car definition table (bounding box, collision geometry)
     *   0x8C..0x10B: physics tuning table (torque curve, gear ratios, damping, etc.)
     * See re/analysis/archive-and-asset-loading.md for full layout docs. */
    {
        int cp_size = 0;
        void *cp_data = td5_asset_open_and_read("carparam.dat", zip_path, &cp_size);
        if (cp_data && cp_size >= 0x10C) {
            td5_physics_load_carparam(slot, (const uint8_t *)cp_data);
            TD5_LOG_I(LOG_TAG, "vehicle slot=%d car=%d: carparam.dat loaded (%d bytes)",
                      slot, car_index, cp_size);
            free(cp_data);
        } else {
            TD5_LOG_W(LOG_TAG, "vehicle slot=%d car=%d: carparam.dat not found or too small (%d bytes), using defaults",
                      slot, car_index, cp_size);
            if (cp_data) free(cp_data);
        }
    }

    return 1;
}

const char *td5_asset_get_car_zip_path(int car_index)
{
    int builtin = (int)(sizeof(s_car_zip_paths) / sizeof(s_car_zip_paths[0]));
    if (car_index < 0)
        return NULL;
    if (car_index < builtin)
        return s_car_zip_paths[car_index];
    return td5_customcar_zip_path(car_index - builtin);   /* NULL if out of range */
}

/* ========================================================================
 * Traffic Vehicle Loading -- LoadRaceVehicleAssets Phase 4 (0x00443280)
 *
 * Reads model%d.prr + skin%d.tga from traffic.zip and registers the mesh
 * against the given actor slot. Each traffic slot gets its own dedicated
 * skin texture page so models can be visually distinct.
 *
 * The traffic.zip archive holds 31 models (model0.prr..model30.prr).
 * model_index selects which one to load for this slot.
 * ======================================================================== */
#define TD5_TRAFFIC_TEXTURE_PAGE_BASE 820
#define TD5_TRAFFIC_ZIP               "traffic.zip"
/* model0..30 = original TD5 traffic cars; 31..60 = the imported REAL per-city
 * TD6 traffic cars (convert_td6_traffic.py): London 31-36, Paris 37-42,
 * Rome 43-48, New York 49-54, Hong Kong 55-60. */
#define TD5_TRAFFIC_MODEL_COUNT       61

int td5_asset_load_traffic_model(int model_index, int slot)
{
    char mesh_name[32];
    char skin_name[32];

    if (slot < 0 || slot >= TD5_MAX_TOTAL_ACTORS) {
        TD5_LOG_W(LOG_TAG, "traffic slot=%d out of range", slot);
        return 0;
    }
    if (model_index < 0 || model_index >= TD5_TRAFFIC_MODEL_COUNT) {
        TD5_LOG_W(LOG_TAG, "traffic slot=%d model_index=%d out of range",
                  slot, model_index);
        return 0;
    }

    snprintf(mesh_name, sizeof(mesh_name), "model%d.prr", model_index);
    snprintf(skin_name, sizeof(skin_name), "skin%d.tga",  model_index);

    /* --- Load mesh ------------------------------------------------------- */
    int mesh_size = 0;
    void *mesh_data = td5_asset_open_and_read(mesh_name, TD5_TRAFFIC_ZIP, &mesh_size);
    if (!mesh_data) {
        TD5_LOG_W(LOG_TAG, "traffic slot=%d: %s not found in %s",
                  slot, mesh_name, TD5_TRAFFIC_ZIP);
        return 0;
    }
    if (mesh_size < (int)sizeof(TD5_MeshHeader)) {
        TD5_LOG_W(LOG_TAG, "traffic slot=%d: %s too small (%d bytes)",
                  slot, mesh_name, mesh_size);
        free(mesh_data);
        return 0;
    }

    TD5_MeshHeader *mesh = (TD5_MeshHeader *)mesh_data;
    td5_track_prepare_mesh_resource(mesh);

    /* --- Load skin texture + patch primitive page IDs -------------------- */
    /* Traffic slots live in [6..11]; subtract 6 so we get a 0..5 index into
     * the dedicated traffic texture page block. */
    int traffic_idx = (slot >= g_traffic_slot_base) ? (slot - g_traffic_slot_base) : slot;
    /* Only 6 dedicated traffic texture pages exist; slots past the 6th reuse them
     * (matches the model_slot wrap in InitRace), so 16 traffic cars share 6 skins. */
    int skin_page   = TD5_TRAFFIC_TEXTURE_PAGE_BASE + (traffic_idx % 6);

    char png_path[256];
    int skin_ok = 0;
    if (td5_asset_resolve_png_path(skin_name, TD5_TRAFFIC_ZIP, png_path, sizeof(png_path))) {
        skin_ok = td5_asset_load_png_texture(skin_page, png_path, TD5_COLORKEY_NONE);
    }
    if (!skin_ok) {
        TD5_LOG_W(LOG_TAG, "traffic slot=%d: skin%d PNG not found in %s (model will draw untextured)",
                  slot, model_index, TD5_TRAFFIC_ZIP);
    }

    /* Patch mesh header + all primitive cmd page IDs to the traffic skin
     * page. Traffic cars use a single skin texture (no separate hub/body)
     * so we replace every command's texture_page_id unconditionally. */
    mesh->texture_page_id = (int16_t)skin_page;
    TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
    for (int c = 0; c < mesh->command_count; c++) {
        cmds[c].texture_page_id = (int16_t)skin_page;
    }

    td5_render_set_vehicle_mesh(slot, mesh);

    TD5_LOG_I(LOG_TAG,
              "traffic slot=%d model=%d: loaded (%d bytes, %d verts, %d cmds, skin_page=%d)",
              slot, model_index, mesh_size,
              mesh->total_vertex_count, mesh->command_count, skin_page);
    return 1;
}

/* [POLICE rewrite 2026-06-19] Load a dedicated POLICE mesh for cop cars.
 *
 * Unlike td5_asset_load_traffic_model this is NOT bound to an actor slot: it
 * loads model<N>.prr + skin<N>.tga into a DEDICATED texture page (clear of the
 * 6 shared traffic pages, so loading the police skin can't recolour ordinary
 * traffic) and RETURNS the prepared mesh. td5_render keeps the pointer and
 * draws it over any slot that is a cop (td5_ai_actor_is_cop). Loaded once per
 * race; the result is cached per model index so a same-model reload is free
 * and never leaks. Returns NULL if the police model isn't present (caller then
 * falls back to the slot's ordinary mesh). */
#define TD5_COP_TEXTURE_PAGE 850   /* free: between traffic (820-825) and FE (900+) */
TD5_MeshHeader *td5_asset_load_cop_mesh(int model_index)
{
    static TD5_MeshHeader *s_cached_mesh = NULL;
    static int             s_cached_index = -1;

    if (model_index < 0 || model_index >= TD5_TRAFFIC_MODEL_COUNT)
        return NULL;
    if (s_cached_mesh && s_cached_index == model_index)
        return s_cached_mesh;   /* already loaded this race */

    char mesh_name[32], skin_name[32];
    snprintf(mesh_name, sizeof(mesh_name), "model%d.prr", model_index);
    snprintf(skin_name, sizeof(skin_name), "skin%d.tga",  model_index);

    int mesh_size = 0;
    void *mesh_data = td5_asset_open_and_read(mesh_name, TD5_TRAFFIC_ZIP, &mesh_size);
    if (!mesh_data || mesh_size < (int)sizeof(TD5_MeshHeader)) {
        if (mesh_data) free(mesh_data);
        TD5_LOG_W(LOG_TAG, "cop mesh: model%d.prr not found/too small in %s",
                  model_index, TD5_TRAFFIC_ZIP);
        return NULL;
    }

    TD5_MeshHeader *mesh = (TD5_MeshHeader *)mesh_data;
    td5_track_prepare_mesh_resource(mesh);

    char png_path[256];
    int skin_ok = 0;
    if (td5_asset_resolve_png_path(skin_name, TD5_TRAFFIC_ZIP, png_path, sizeof(png_path)))
        skin_ok = td5_asset_load_png_texture(TD5_COP_TEXTURE_PAGE, png_path, TD5_COLORKEY_NONE);
    if (!skin_ok)
        TD5_LOG_W(LOG_TAG, "cop mesh: skin%d PNG not found (cop will draw untextured)",
                  model_index);

    mesh->texture_page_id = (int16_t)TD5_COP_TEXTURE_PAGE;
    TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
    for (int c = 0; c < mesh->command_count; c++)
        cmds[c].texture_page_id = (int16_t)TD5_COP_TEXTURE_PAGE;

    /* Replace any previously-cached cop mesh (different model / new race). */
    if (s_cached_mesh && s_cached_mesh != mesh) {
        free(s_cached_mesh);
    }
    s_cached_mesh  = mesh;
    s_cached_index = model_index;
    TD5_LOG_I(LOG_TAG, "cop mesh: model=%d loaded (%d verts, %d cmds, page=%d)",
              model_index, mesh->total_vertex_count, mesh->command_count,
              TD5_COP_TEXTURE_PAGE);
    return mesh;
}

/* ========================================================================
 * Traffic Model Selection -- chain from track_index to model_index
 *
 * Mirrors the two-level lookup inside LoadRaceVehicleAssets @ 0x00443280:
 *
 *   schedule_index = track_index  (port maps 1:1, -1 = drag strip)
 *   pool_row       = gScheduleToPoolIndex[schedule_index]        (0x00466894)
 *   pool_idx       = gTrackPoolSpanCountTable[pool_row]          (0x00466D50, forward)
 *                    or gTrackPoolReverseSpanCountTable[pool_row] (0x00466E3C, reverse)
 *   row            = g_trafficVehicleSkinTable[pool_idx]                      (0x00474D74)
 *   model_index    = g_trafficVehicleVariantTable[row][slot_in_pool]             (0x00474CE8)
 *
 * Original gates the load with `pool_idx < 0x19` (@ 0x004435ad region): when
 * the pool index is >= 25, the per-slot size query returns 0 and the traffic
 * archive read is skipped. The port's resolver returns -1 for that case so
 * the caller can skip loading.
 *
 * All tables are byte-for-byte transcribed from memory_read of TD5_d3d.exe.
 * ======================================================================== */

/* gScheduleToPoolIndex @ 0x00466894 — 20 signed-byte entries.
 * Original decomp casts to (char), so these are int8_t. Observed values are
 * all positive, but signedness could matter for hypothetical higher entries. */
static const int8_t s_schedule_to_pool_index[20] = {
    /*  0 MOSCOW     */ 11, /*  1 EDINBURGH  */  9, /*  2 SYDNEY     */  7,
    /*  3 BLUE RIDGE */ 10, /*  4 JARASH     */ 13, /*  5 NEWCASTLE  */ 16,
    /*  6 MAUI       */ 15, /*  7 COURMAYEUR */ 14, /*  8 HONOLULU   */  6,
    /*  9 TOKYO      */  8, /* 10 KESWICK    */  0, /* 11 SF         */  1,
    /* 12 BERN       */  2, /* 13 KYOTO      */  3, /* 14 WASHINGTON */  4,
    /* 15 MUNICH     */  5, /* 16 CHEDDAR    */ 12, /* 17 MONTEGO    */ 18,
    /* 18 BEZ        */ 17, /* 19 (invalid)  */ 19,
};

/* gTrackPoolSpanCountTable @ 0x00466D50 — 18 int32 entries. */
static const int32_t s_track_pool_span_count_table[18] = {
     1,  2,  3,  4,  5,  6, 13, 14, 15,
    16, 17, 23, 25, 26, 27, 28, 29, 37,
};

/* gTrackPoolReverseSpanCountTable @ 0x00466E3C — 15 int32 entries,
 * with -1 sentinels at indices 12..14 (reverse is unavailable for those). */
static const int32_t s_track_pool_reverse_span_count_table[15] = {
     7,  8,  9, 10, 11, 12,
    18, 19, 20, 21, 22, 24,
    -1, -1, -1,
};

/* g_trafficVehicleSkinTable @ 0x00474D74 — 25 int32 entries mapping pool_idx to
 * a row in s_traffic_model_table[][]. Entry [0] = 30 is the benchmark
 * sentinel (out-of-range against the 6-row model table). */
static const int32_t s_traffic_pool_to_row[25] = {
    30,  0,  1,  2,  3,  4,  5,  0,  1,  2,  3,  4,  5,
     1,  5,  3,  0,  4,  1,  5,  3,  0,  4,  2,  2,
};

/* g_trafficVehicleVariantTable @ 0x00474CE8 — 6 rows of 6 traffic model indices
 * (values resolve to "model%d.prr" inside traffic.zip).
 * Row 5's last dword (0x1e) aliases s_traffic_pool_to_row[0]=30 at 0x00474D74. */
static const int32_t s_traffic_model_table[6][6] = {
    /* row 0 */ {  1,  2,  3,  0,  4,  5 },
    /* row 1 */ {  7,  8,  9,  6, 10, 11 },
    /* row 2 */ { 13, 14, 15, 12, 16, 17 },
    /* row 3 */ {  7, 19,  9, 18, 11, 20 },
    /* row 4 */ { 22, 20, 23, 21, 19, 24 },
    /* row 5 */ { 26, 27, 28, 25, 29, 30 },
};

/* TD6 CITY traffic: the migrated P2P city tracks have no row in the TD5 6-row
 * traffic-model table, so they borrowed track-0 (Moscow) cars. We imported the
 * REAL per-city TD6 traffic models (convert_td6_traffic.py) at high indices;
 * map the active TD6 level -> the city's 6-model base. Returns -1 when the
 * level is not one of the 5 TD6 cities (keep the TD5 table / track-0 borrow).
 * Level numbers are the converted-asset level (g_active_td6_level): Paris=8,
 * NewYork=9, Rome=10, HongKong=11, London=12 (per k_td6_menu_slots). */
int td5_asset_td6_city_traffic_base(int td6_level)
{
    switch (td6_level) {
    case  8: return 37;   /* Paris     (level000) -> model37..42 */
    case  9: return 49;   /* New York  (level001) -> model49..54 */
    case 10: return 43;   /* Rome      (level002) -> model43..48 */
    case 11: return 55;   /* Hong Kong (level003) -> model55..60 */
    case 12: return 31;   /* London    (level004) -> model31..36 */
    default: return -1;
    }
}

int td5_asset_resolve_traffic_model_index(int track_index, int reverse, int slot_in_pool)
{
    if (slot_in_pool < 0) {
        TD5_LOG_W(LOG_TAG, "traffic resolve: slot_in_pool=%d out of range", slot_in_pool);
        return -1;
    }
    slot_in_pool %= 6;   /* only 6 models per track; extra traffic slots reuse them */

    /* Drag strip path @ 0x0042AD21: schedule_index < 0 forces g_trackPoolIndex = 30,
     * which fails the `< 0x19` traffic gate — no models loaded. */
    if (track_index < 0) {
        return -1;
    }
    if (track_index >= (int)(sizeof(s_schedule_to_pool_index) / sizeof(s_schedule_to_pool_index[0]))) {
        TD5_LOG_W(LOG_TAG, "traffic resolve: track_index=%d past schedule table", track_index);
        return -1;
    }

    int pool_row = (int)s_schedule_to_pool_index[track_index];
    int pool_idx;

    if (reverse) {
        if (pool_row < 0 || pool_row >= (int)(sizeof(s_track_pool_reverse_span_count_table) / sizeof(int32_t))) {
            TD5_LOG_W(LOG_TAG, "traffic resolve: track=%d reverse pool_row=%d OOB",
                      track_index, pool_row);
            return -1;
        }
        pool_idx = s_track_pool_reverse_span_count_table[pool_row];
        if (pool_idx < 0) {
            /* -1 sentinel: reverse unavailable for this track. */
            return -1;
        }
    } else {
        if (pool_row < 0 || pool_row >= (int)(sizeof(s_track_pool_span_count_table) / sizeof(int32_t))) {
            TD5_LOG_W(LOG_TAG, "traffic resolve: track=%d forward pool_row=%d OOB",
                      track_index, pool_row);
            return -1;
        }
        pool_idx = s_track_pool_span_count_table[pool_row];
    }

    /* Original guard: `if (iVar15 < 0x19)` — pool_idx >= 25 means no traffic
     * models are loaded (size query returns 0, archive read is skipped). */
    if (pool_idx >= 25) {
        return -1;
    }
    if (pool_idx < 0) {
        return -1;
    }

    int row = s_traffic_pool_to_row[pool_idx];
    if (row < 0 || row >= 6) {
        /* Out of range of the 6-row model table. Treat as no-traffic. */
        return -1;
    }

    return s_traffic_model_table[row][slot_in_pool];
}

/* Returns the 1-based track-pool record number ("world_x" =
 * gTrackPoolSpanCountTable[gScheduleToPoolIndex[track]] forward, or the reverse
 * table when reverse) — the same value InitializeRaceSession feeds to
 * LoadTrackRuntimeData @0x42fb90 to select the trackside-camera profile table
 * (profile index = return - 1, via td5_camera_bind_trackside_profiles). Returns
 * 0 when the track/direction is invalid or reverse is unavailable (-1 sentinel),
 * so the caller binds no profiles and the replay camera falls back to chase. */
int td5_asset_track_pool_index(int track_index, int reverse)
{
    if (track_index < 0 ||
        track_index >= (int)(sizeof(s_schedule_to_pool_index) /
                             sizeof(s_schedule_to_pool_index[0]))) {
        return 0;
    }
    int pool_row = (int)s_schedule_to_pool_index[track_index];
    int world_x;
    if (reverse) {
        if (pool_row < 0 ||
            pool_row >= (int)(sizeof(s_track_pool_reverse_span_count_table) /
                              sizeof(int32_t))) {
            return 0;
        }
        world_x = s_track_pool_reverse_span_count_table[pool_row];
        if (world_x < 0) {
            return 0;   /* reverse unavailable for this track */
        }
    } else {
        if (pool_row < 0 ||
            pool_row >= (int)(sizeof(s_track_pool_span_count_table) /
                              sizeof(int32_t))) {
            return 0;
        }
        world_x = s_track_pool_span_count_table[pool_row];
    }
    return world_x;
}

/* ========================================================================
 * Checkpoint record index resolution (direction-aware).
 *
 * The original derives the per-track checkpoint-timing record via the SAME
 * two-stage pool lookup the traffic resolver uses, then `record = pool_idx - 1`
 * (see k_schedule_to_checkpoint_index comment in td5_game.c). Crucially the
 * REVERSE direction selects the pool index from gTrackPoolReverseSpanCountTable
 * (0x00466E3C) instead of the forward gTrackPoolSpanCountTable (0x00466D50),
 * so reverse tracks get a DIFFERENT checkpoint record — different span
 * thresholds, initial_time, and time bonuses.
 *
 * [CONFIRMED via runtime dump of g_raceCheckpointTablePtr @0x4AED88 on the
 *  original TD5_d3d.exe: reverse Sydney (track 2, pool_row 7) yields
 *  record_idx = reverse_table[7]-1 = 19-1 = 18 = {665,1081,1679,2250,2635}
 *  init=15419, matching k_checkpoint_table[18] exactly. Forward Sydney =
 *  reverse_table-free path = forward_table[7]-1 = 14-1 = 13.]
 *
 * Returns the checkpoint record index, or -1 when:
 *   - track_index is out of the schedule range, or
 *   - reverse was requested but this track ships no reverse data (-1 sentinel
 *     in the reverse pool table). Callers fall back to the forward record.
 * ======================================================================== */
int td5_asset_resolve_checkpoint_record_index(int track_index, int reverse)
{
    if (track_index < 0 ||
        track_index >= (int)(sizeof(s_schedule_to_pool_index) / sizeof(s_schedule_to_pool_index[0]))) {
        return -1;
    }

    int pool_row = (int)s_schedule_to_pool_index[track_index];
    int pool_idx;

    if (reverse) {
        if (pool_row < 0 ||
            pool_row >= (int)(sizeof(s_track_pool_reverse_span_count_table) / sizeof(int32_t))) {
            return -1;
        }
        pool_idx = s_track_pool_reverse_span_count_table[pool_row];
        if (pool_idx < 0) {
            /* -1 sentinel: reverse unavailable for this track. */
            return -1;
        }
    } else {
        if (pool_row < 0 ||
            pool_row >= (int)(sizeof(s_track_pool_span_count_table) / sizeof(int32_t))) {
            return -1;
        }
        pool_idx = s_track_pool_span_count_table[pool_row];
    }

    /* record_idx = pool_idx - 1 (the original's per-track checkpoint record). */
    int record_idx = pool_idx - 1;
    if (record_idx < 0)
        return -1;
    return record_idx;
}

/* ========================================================================
 * Mipmap Builder -- ParseAndDecodeCompressedTrackData (0x430D30)
 *
 * Generates a mipmap chain by box-filtering from source dimensions
 * down through each power-of-two level.
 *
 * [ARCH-DIVERGENCE: orig 0x430D30 is a software pixel-format quantizer,
 *  port replaces with box-filter mipmap builder; L5 promotion sweep
 *  2026-05-21] — Ghidra-verified: the orig function at 0x430D30 is NOT a
 *  mipmap builder. It is a software RGBA→packed-pixel converter that:
 *    1) Computes per-channel LSB shift + bit-length from 4 channel masks
 *       (param_1+0x0c/0x10/0x14/0x18) via trailing-zero / set-bit counts.
 *    2) Iterates a per-level dimension table at &DAT_00473b70 (param_1+0x24
 *       indexes down to +0x20).
 *    3) Per pixel, reads 4 floats via __ftol, clamps each to 0xfe→0xff,
 *       then packs into ushort (≤16 total bits) or uint (>16 bits) via the
 *       precomputed channel shifts. This targets D3D3 surface formats
 *       (RGB565 / ARGB1555 / RGBA8888) determined at runtime from the
 *       active DDraw surface caps.
 *  The D3D11 backend doesn't need runtime pixel-format conversion (uses a
 *  fixed B8G8R8A8_UNORM swap chain), so the port replaces this entry point
 *  with a mipmap builder that the D3D11 SRV creation flow can consume. The
 *  per-level iteration outer loop is shared in shape but the inner pixel
 *  conversion is gone; output buffer layout (consecutive mip levels) is
 *  preserved.
 * ======================================================================== */

int td5_asset_build_mipmaps(const void *src, int width, int height, int format,
                             void *dst, int *dst_size)
{
    if (!src || !dst || !dst_size) return 0;
    if (width <= 0 || height <= 0) return 0;

    /*
     * The original iterates from the source dimension down to 1x1,
     * averaging 2x2 pixel blocks at each level. The pixel format
     * (16-bit or 32-bit) is determined by the current D3D surface format.
     *
     * For the source port, we generate RGBA32 mipmaps:
     */
    int bpp = (format == 0) ? 3 : 4; /* RGB24 or RGBA32 */
    const uint8_t *sp = (const uint8_t *)src;
    uint8_t *dp = (uint8_t *)dst;
    int total = 0;

    int mip_w = width;
    int mip_h = height;

    /* Copy level 0 */
    int level0_size = mip_w * mip_h * bpp;
    memcpy(dp, sp, (size_t)level0_size);
    dp += level0_size;
    total += level0_size;

    /* Generate subsequent levels */
    const uint8_t *prev = (const uint8_t *)dst;
    while (mip_w > 1 || mip_h > 1) {
        int prev_w = mip_w;
        if (mip_w > 1) mip_w >>= 1;
        if (mip_h > 1) mip_h >>= 1;

        for (int y = 0; y < mip_h; y++) {
            for (int x = 0; x < mip_w; x++) {
                int sx = x * 2;
                int sy = y * 2;
                for (int c = 0; c < bpp; c++) {
                    int sum = 0;
                    sum += prev[(sy * prev_w + sx) * bpp + c];
                    sum += prev[(sy * prev_w + sx + 1) * bpp + c];
                    sum += prev[((sy + 1) * prev_w + sx) * bpp + c];
                    sum += prev[((sy + 1) * prev_w + sx + 1) * bpp + c];
                    *dp++ = (uint8_t)((sum + 2) >> 2);
                }
            }
        }
        int level_size = mip_w * mip_h * bpp;
        total += level_size;
        prev = dp - level_size;
    }

    *dst_size = total;
    return 1;
}

/* ========================================================================
 * Memory Helpers
 * ======================================================================== */

void *td5_asset_alloc(size_t size)
{
    return td5_plat_heap_alloc(size);
}

void *td5_asset_alloc_aligned(size_t size, size_t align)
{
    return td5_plat_alloc_aligned(size, align);
}

void td5_asset_free(void *ptr)
{
    td5_plat_heap_free(ptr);
}

/* ============================================================
 * [CITATION-SWEEP 2026-05-21] Phase 1 audit-header refresh
 *
 * The following L3 Ghidra functions are ported (or folded) into
 * this file but were missed by build_confidence_map.py's
 * 2026-05-18 citation scan due to snake_case rename or
 * multi-line comment wraps. Listed here so the next confidence-
 * map run promotes them L3 -> L4 (cited without precision
 * keywords). Per-function audits remain a separate Phase 4 task.
 *
 * Source: re/analysis/l3_triage_2026-05-21.csv +
 *         re/analysis/phase1_manifest_assignment.csv
 *
 *   0x0040AEF0  GetEnvironmentTexturePageMode  [ARCH-DIVERGENCE: D3D3 device-state query removed; L5 sweep 2026-05-21]
 *     Ghidra-verified 0x0040AEF0: returns 2 when *(d3d_exref + 0xa5c) == 0
 *     else 0; an old D3D3 driver-caps query (16- vs 24-bit color page mode).
 *     Port targets D3D11 which exposes uniform RGBA8 page formats, so the
 *     query has no equivalent and is folded away at every call site.
 *   0x0040BAE0  PreloadLevelTexturePages  [ARCH-DIVERGENCE: on-demand texture streaming removed; L5 sweep 2026-05-21]
 *     Ghidra-verified 0x0040BAE0: orig calls AdvanceTextureStreamingScheduler
 *     (0x0040B830) for each level page when the texture-cache has capacity
 *     headroom (DAT_0048DC40[7] < DAT_0048DC40[4]) and marks per-page
 *     residency flags. Port loads all level textures upfront in
 *     td5_asset_load_track_texture_set (no streaming scheduler -- a
 *     deliberate simplification under the D3D11 backend), so the
 *     scheduler-advance loop has no equivalent.
 *     [2026-05-24 re-audit] The bind-side LRU equivalent lives in
 *     td5_render_bind_texture_page (td5_render.c:2808) -- 600 cache slots
 *     vs orig's 64, so eviction rarely fires and pop-in risk is lower than
 *     orig. Verdict: CONFIRMED_D3D11_BACKEND, no port needed. See
 *     re/analysis/advance_texture_streaming_scheduler_reaudit_2026-05-24.md.
 *   0x00412030  LoadFrontendTgaSurfaceFromArchive  [ARCH-DIVERGENCE: TGA/DDraw -> PNG/D3D11; L5 sweep 2026-05-21]
 *     Ghidra-verified 0x00412030: orig opens a frontend TGA from an
 *     archive via OpenArchiveFileForRead, allocates a 0x1d4c00-byte
 *     conversion buffer through DX::Allocate, decodes via DX::ImageProTGA
 *     using the active DDraw color masks (g_tgaDecodeRedMask/Green/Blue),
 *     then creates a tracked DDraw surface via CreateTrackedFrontendSurface
 *     and copies the decoded pixels into it. Port uses the asset PNG
 *     pipeline (td5_asset_load_png / fmv_load_png) into a malloc'd BGRA32
 *     buffer that the D3D11 backend uploads as a texture; the entire
 *     DDraw-surface object model is gone. Asset-source change documented
 *     in td5_fmv.c file header for the legal-screens path.
 *   0x00442D30  ResampleTexturePageToEntryDimensions  [ARCH-DIVERGENCE: page-resize removed; L5 sweep 2026-05-21]
 *     Ghidra-verified 0x00442D30: orig resizes a decoded texture page to
 *     match the dimensions recorded in the archive entry descriptor at
 *     gStaticHedTextureData (handles both 3- and 4-channel pixel formats,
 *     scaling via integer iVar2 / iVar7 ratio). Port's PNG asset
 *     pipeline produces textures at their natural size; D3D11 accepts any
 *     dimensions, so the runtime resampler is folded away.
 */
