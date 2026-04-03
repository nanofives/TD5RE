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
#include "td5_track.h"
#include "td5_platform.h"
#include "td5re.h"
#include "td5_render.h"
#include "td5_hud.h"    /* for TD5_AtlasEntry */

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

#define TD5_TINFL_DECOMPRESS(out, out_len, in, in_len) \
    td5_inflate_mem_to_mem(out, out_len, in, in_len)

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
        "../re/assets/levels",
        "../re/td5_dump/levels"
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
 *
 * We map texture_slot → D3D page (STATIC_ATLAS_BASE + texture_slot).
 * tpage0-2.dat exist on disk as 256×512 R5G6B5 images and are uploaded.
 * Higher-index pages get a magenta 2×2 placeholder.
 * ======================================================================== */

#define STATIC_ATLAS_MAX   64
#define STATIC_ATLAS_BASE  700   /* first D3D page reserved for static atlas */

typedef struct {
    char           name[44];
    TD5_AtlasEntry entry;
} S_AtlasRec;

static S_AtlasRec     s_atlas_table[STATIC_ATLAS_MAX];
static int            s_atlas_count = 0;
static TD5_AtlasEntry s_atlas_fallback = {0};   /* returned for unknown names */
static uint8_t        s_static_page_done[32];   /* per-slot: 0=none,1=real,2=placeholder */

/** Load tpageN.dat (256×256 BGRA32) and upload to GPU.
 *
 * Originally believed to be R5G6B5 256×512 (same byte count), but the
 * original engine normalizes UVs by multiplying by 1/256 for BOTH axes
 * (BuildSpriteQuadTemplate @ 0x432BD0, constant [0x4749D0] = 0.00390625).
 * A 256×512 page would require V normalization by 1/512, not 1/256.
 * Therefore the pages are 256×256 BGRA32 (4 bytes/pixel × 65536 px = 262144 bytes).
 */
static int load_static_r5g6b5_tpage(int slot)
{
    char path[128];
    int w = 256, h = 256, npx = w * h;
    uint8_t *bgra;
    FILE *f;

    snprintf(path, sizeof(path), "../re/assets/static/tpage%d.dat", slot);
    f = fopen(path, "rb");
    if (!f) return 0;

    bgra = (uint8_t *)malloc((size_t)npx * 4);
    if (!bgra) { fclose(f); return 0; }

    if ((int)fread(bgra, 4, (size_t)npx, f) < npx) {
        free(bgra); fclose(f); return 0;
    }
    fclose(f);

    td5_plat_render_upload_texture(STATIC_ATLAS_BASE + slot, bgra, w, h, 2);
    free(bgra);
    TD5_LOG_D(LOG_TAG, "static atlas: uploaded tpage%d.dat (256x256 BGRA) → D3D page %d",
              slot, STATIC_ATLAS_BASE + slot);
    return 1;
}

/** Upload a 2×2 magenta placeholder for tpage slots without on-disk art. */
static void upload_atlas_placeholder(int slot)
{
    static const uint8_t k_ph[16] = {
        0xFF, 0x00, 0xFF, 0xFF,  0xFF, 0x00, 0xFF, 0xFF,
        0xFF, 0x00, 0xFF, 0xFF,  0xFF, 0x00, 0xFF, 0xFF
    };
    td5_plat_render_upload_texture(STATIC_ATLAS_BASE + slot, k_ph, 2, 2, 2);
}

static void td5_asset_init_static_atlas(void)
{
    const char *hed_path = "../re/assets/static/static.hed";
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

        /* Upload the page the first time we encounter this slot */
        if (!s_static_page_done[tex_slot]) {
            if (tex_slot <= 2 && load_static_r5g6b5_tpage(tex_slot)) {
                s_static_page_done[tex_slot] = 1;
            } else {
                upload_atlas_placeholder(tex_slot);
                s_static_page_done[tex_slot] = 2;
            }
        }
    }

    fclose(f);
    TD5_LOG_I(LOG_TAG,
              "static atlas: %d entries loaded from %s (D3D pages %d..%d)",
              s_atlas_count, hed_path, STATIC_ATLAS_BASE, STATIC_ATLAS_BASE + 31);
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

/* ========================================================================
 * ZIP Central Directory Parser
 *
 * Reimplements ParseZipCentralDirectory (0x43FC80).
 *
 * The original reads the EOCD record, then streams through the central
 * directory byte-by-byte using a 64KB buffer. We simplify by reading the
 * entire central directory into memory (it is typically < 64KB for TD5
 * archives which have at most ~60 entries).
 * ======================================================================== */

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
 * ======================================================================== */

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
    /* Loose file override */
    int loose_sz = try_loose_file_size(entry_name);
    if (loose_sz >= 0) return loose_sz;

    loose_sz = try_extracted_level_file_size(entry_name, zip_path);
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

/* ========================================================================
 * TGA Decoder -- DecodeArchiveImageToRgb24 (0x442E00)
 *
 * The original is a full TGA decoder supporting types 1 (uncompressed
 * color-mapped), 2 (uncompressed truecolor), 9 (RLE color-mapped), and
 * 10 (RLE truecolor). Output is always 24-bit BGR.
 *
 * TGA header layout (18 bytes):
 *   [0]     id_length
 *   [1]     color_map_type (0=none, 1=present)
 *   [2]     image_type (1,2,9,10)
 *   [3..4]  color_map_first_entry
 *   [5..6]  color_map_length
 *   [7]     color_map_entry_size (bits)
 *   [8..9]  x_origin
 *   [10..11] y_origin
 *   [12..13] width
 *   [14..15] height
 *   [16]    bits_per_pixel
 *   [17]    image_descriptor (bit 4=right-to-left, bit 5=top-to-bottom)
 * ======================================================================== */

void td5_asset_decode_tga_to_rgb24(const void *tga_data, void *rgb_out)
{
    const uint8_t *src = (const uint8_t *)tga_data;
    uint8_t *dst = (uint8_t *)rgb_out;

    uint8_t  id_length     = src[0];
    uint8_t  cmap_type     = src[1];
    uint8_t  image_type    = src[2];
    uint16_t cmap_length   = read_u16(src + 5);
    uint8_t  cmap_depth    = src[7]; /* bits per palette entry (0, 15, 16, 24, or 32) */
    int      cmap_entry_bytes = (cmap_depth >= 8) ? (cmap_depth + 7) / 8 : 3; /* default 3 */
    uint16_t width         = read_u16(src + 12);
    uint16_t height        = read_u16(src + 14);
    uint8_t  bpp           = src[16];
    uint8_t  descriptor    = src[17];

    /* Palette pointer (after header + id) */
    const uint8_t *palette = NULL;
    int pixel_data_offset  = 18 + id_length;

    if (cmap_type != 0) {
        palette = src + pixel_data_offset;
        pixel_data_offset += cmap_length * cmap_entry_bytes;
    }

    const uint8_t *pixel_data = src + pixel_data_offset;
    int total_pixels = (int)width * (int)height;
    int total_bytes  = total_pixels * 3;

    switch (image_type) {
    case 1: {
        /* Uncompressed color-mapped (indexed, 8bpp) */
        const uint8_t *idx = pixel_data;
        uint8_t *out = dst;
        for (int i = 0; i < total_bytes; i += 3) {
            int ci = (*idx++) * cmap_entry_bytes;
            out[0] = palette[ci + 2]; /* R -> B */
            out[1] = palette[ci + 1]; /* G */
            out[2] = palette[ci + 0]; /* B -> R */
            out += 3;
        }
        break;
    }

    case 2: {
        /* Uncompressed truecolor */
        const uint8_t *in = pixel_data;
        uint8_t *out = dst;
        int npx = total_pixels;

        if (bpp == 16) {
            for (int i = 0; i < npx; i++) {
                uint16_t v = read_u16(in);
                in += 2;
                out[0] = (uint8_t)((v >> 7) & 0xF8); /* R */
                out[1] = (uint8_t)((v >> 2) & 0xF8); /* G */
                out[2] = (uint8_t)((v << 3) & 0xF8); /* B */
                out += 3;
            }
        } else if (bpp == 24) {
            for (int i = 0; i < npx; i++) {
                out[0] = in[2]; /* R <- B */
                out[1] = in[1]; /* G */
                out[2] = in[0]; /* B <- R */
                in += 3;
                out += 3;
            }
        } else if (bpp == 32) {
            for (int i = 0; i < npx; i++) {
                out[0] = in[2]; /* R */
                out[1] = in[1]; /* G */
                out[2] = in[0]; /* B */
                in += 4;        /* skip alpha */
                out += 3;
            }
        }
        break;
    }

    case 9: {
        /* RLE color-mapped */
        const uint8_t *in = pixel_data;
        uint8_t *out = dst;
        int remaining = total_bytes;

        while (remaining > 0) {
            uint8_t packet = *in++;

            if (packet < 0x80) {
                /* Raw run: (packet + 1) pixels */
                int count = (int)(packet + 1);
                for (int j = 0; j < count && remaining > 0; j++) {
                    int ci = (*in++) * cmap_entry_bytes;
                    out[0] = palette[ci + 2];
                    out[1] = palette[ci + 1];
                    out[2] = palette[ci + 0];
                    out += 3;
                    remaining -= 3;
                }
            } else {
                /* RLE run: (packet - 127) pixels of the same index */
                int count = (int)(packet - 127);
                int ci = (*in++) * cmap_entry_bytes;
                uint8_t r = palette[ci + 2];
                uint8_t g = palette[ci + 1];
                uint8_t b = palette[ci + 0];
                for (int j = 0; j < count && remaining > 0; j++) {
                    out[0] = r;
                    out[1] = g;
                    out[2] = b;
                    out += 3;
                    remaining -= 3;
                }
            }
        }
        break;
    }

    case 10: {
        /* RLE truecolor */
        const uint8_t *in = pixel_data;
        uint8_t *out = dst;
        int remaining = total_bytes;

        if (bpp == 24) {
            while (remaining > 0) {
                uint8_t packet = *in++;

                if (packet < 0x80) {
                    int count = (int)(packet + 1);
                    for (int j = 0; j < count && remaining > 0; j++) {
                        out[0] = in[2]; /* R */
                        out[1] = in[1]; /* G */
                        out[2] = in[0]; /* B */
                        in += 3;
                        out += 3;
                        remaining -= 3;
                    }
                } else {
                    int count = (int)(packet - 127);
                    uint8_t r = in[2], g = in[1], b = in[0];
                    in += 3;
                    for (int j = 0; j < count && remaining > 0; j++) {
                        out[0] = r;
                        out[1] = g;
                        out[2] = b;
                        out += 3;
                        remaining -= 3;
                    }
                }
            }
        }
        /* bpp==32 RLE truecolor: similar but skip alpha byte */
        else if (bpp == 32) {
            while (remaining > 0) {
                uint8_t packet = *in++;

                if (packet < 0x80) {
                    int count = (int)(packet + 1);
                    for (int j = 0; j < count && remaining > 0; j++) {
                        out[0] = in[2];
                        out[1] = in[1];
                        out[2] = in[0];
                        in += 4;
                        out += 3;
                        remaining -= 3;
                    }
                } else {
                    int count = (int)(packet - 127);
                    uint8_t r = in[2], g = in[1], b = in[0];
                    in += 4;
                    for (int j = 0; j < count && remaining > 0; j++) {
                        out[0] = r;
                        out[1] = g;
                        out[2] = b;
                        out += 3;
                        remaining -= 3;
                    }
                }
            }
        }
        break;
    }

    default:
        TD5_LOG_E(LOG_TAG, "unsupported TGA type: %d", image_type);
        memset(dst, 0, (size_t)total_bytes);
        break;
    }

    /*
     * Apply vertical flip if origin is bottom-left (bit 5 of descriptor = 0).
     * The original checks descriptor & 0x20 and flips rows if not set.
     */
    int row_bytes = (int)width * 3;

    if (!(descriptor & 0x20)) {
        /* Flip vertically (bottom-to-top -> top-to-bottom) */
        uint8_t *top = dst;
        uint8_t *bot = dst + (height - 1) * row_bytes;
        for (int y = 0; y < height / 2; y++) {
            /* Swap rows in 4-byte chunks for speed (matching original) */
            for (int x = 0; x < row_bytes / 4; x++) {
                uint32_t tmp = ((uint32_t *)top)[x];
                ((uint32_t *)top)[x] = ((uint32_t *)bot)[x];
                ((uint32_t *)bot)[x] = tmp;
            }
            /* Handle remaining bytes */
            for (int x = (row_bytes / 4) * 4; x < row_bytes; x++) {
                uint8_t tmp = top[x];
                top[x] = bot[x];
                bot[x] = tmp;
            }
            top += row_bytes;
            bot -= row_bytes;
        }
    }

    /* Apply horizontal flip if bit 4 of descriptor is set */
    if (descriptor & 0x10) {
        for (int y = 0; y < height; y++) {
            uint8_t *row_start = dst + y * row_bytes;
            uint8_t *left = row_start;
            uint8_t *right = row_start + row_bytes - 3;
            for (int x = 0; x < width / 2; x++) {
                uint8_t t0 = left[0], t1 = left[1], t2 = left[2];
                left[0] = right[0]; left[1] = right[1]; left[2] = right[2];
                right[0] = t0; right[1] = t1; right[2] = t2;
                left += 3;
                right -= 3;
            }
        }
    }
}

int td5_asset_decode_tga(const void *data, size_t size, void **pixels_out,
                          int *width_out, int *height_out)
{
    if (!data || size < 18 || !pixels_out) return 0;

    const uint8_t *src = (const uint8_t *)data;
    uint16_t w = read_u16(src + 12);
    uint16_t h = read_u16(src + 14);

    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 0;

    /* Allocate RGBA32 output */
    size_t rgb_size = (size_t)w * (size_t)h * 3;
    uint8_t *rgb = (uint8_t *)malloc(rgb_size);
    if (!rgb) return 0;

    td5_asset_decode_tga_to_rgb24(data, rgb);

    /* Convert RGB24 to BGRA32 (D3D11 B8G8R8A8_UNORM native format) */
    size_t rgba_size = (size_t)w * (size_t)h * 4;
    uint8_t *rgba = (uint8_t *)malloc(rgba_size);
    if (!rgba) { free(rgb); return 0; }

    for (int i = 0; i < w * h; i++) {
        uint8_t r = rgb[i * 3 + 0];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        rgba[i * 4 + 0] = b;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = r;
        /* Black (0,0,0) is the color key for all frontend TGA sprites */
        rgba[i * 4 + 3] = (r == 0 && g == 0 && b == 0) ? 0 : 0xFF;
    }

    free(rgb);

    *pixels_out = rgba;
    if (width_out)  *width_out  = w;
    if (height_out) *height_out = h;
    return 1;
}

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

/* Forward-direction filenames (original string constants) */
static const char *s_strip_names[2]   = { "STRIP.DAT",   "STRIPB.DAT"   };
static const char *s_left_names[2]    = { "LEFT.TRK",    "LEFTB.TRK"    };
static const char *s_right_names[2]   = { "RIGHT.TRK",   "RIGHTB.TRK"   };
/** Wrapper texture-page limit. Must match td5_platform_win32.c. */
#define TD5_TRACK_TEXTURE_PAGE_LIMIT 1024

int td5_asset_level_number(int track_index)
{
    /* Two-step lookup from the original binary:
     * Step 1: schedule slot index -> pool index via gScheduleToPoolIndex (VA 0x466894).
     * Step 2: pool index -> level ZIP number via pool-to-ZIP table (VA 0x466D50). */
    static const uint8_t k_schedule_to_pool[20] = {
        11,  9,  7, 10, 13, 16, 15, 14,  6,  8,
         0,  1,  2,  3,  4,  5, 12, 18, 17, 19
    };
    /* Pool 0-18 mapped to level zip numbers; pool 19 (drag strip) uses level064
     * which is not shipped -- fall back to level001 for that slot. */
    static const int k_pool_to_zip[19] = {
         1,  2,  3,  4,  5,  6, 13, 14, 15, 16,
        17, 23, 25, 26, 27, 28, 29, 37, 39
    };
    int pool;
    if (track_index < 0)
        return 1;
    if (track_index >= 20)
        return 1;
    pool = k_schedule_to_pool[track_index];
    if (pool >= 19)
        return 1; /* pool 19 = drag strip (level064 not shipped) */
    return k_pool_to_zip[pool];
}

static int td5_asset_build_track_texture_png_path(int track_index,
                                                  int page_index,
                                                  char *out_path,
                                                  size_t out_size)
{
    int level_number = td5_asset_level_number(track_index);
    int n;

    if (!out_path || out_size == 0 || page_index < 0)
        return 0;

    n = snprintf(out_path, out_size,
                 "../re/td5_png_clean/levels/level%03d/textures/tex_%03d.png",
                 level_number, page_index);
    if (n > 0 && (size_t)n < out_size && td5_plat_file_exists(out_path))
        return 1;

    n = snprintf(out_path, out_size,
                 "../re/assets/levels/level%03d/textures/tex_%03d.png",
                 level_number, page_index);
    if (n > 0 && (size_t)n < out_size && td5_plat_file_exists(out_path))
        return 1;

    return 0;
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

    pixels = stbi_load_from_memory(file_data, (int)file_size,
                                   &width, &height, &channels, 4);
    free(file_data);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return 0;
    }

    *pixels_out = pixels;
    if (width_out) *width_out = width;
    if (height_out) *height_out = height;
    return 1;
}

static int td5_asset_upload_png_texture_page(int page_index,
                                             const char *path,
                                             uint32_t *loaded_count)
{
    void *pixels = NULL;
    int width = 0, height = 0;
    int ok = 0;

    if (td5_asset_decode_png_rgba32(path, &pixels, &width, &height)) {
        ok = td5_plat_render_upload_texture(page_index, pixels, width, height, 2);
        stbi_image_free(pixels);
        if (ok && loaded_count)
            (*loaded_count)++;
        return ok;
    }

    return 0;
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
    snprintf(out_path, out_size, "../re/assets/levels/level%03d/%s",
             level_number, entry_name ? entry_name : "");
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

int td5_asset_load_level(int track_index)
{
    char zip_path[256];
    char strip_source[256];
    td5_asset_build_level_zip_path(track_index, zip_path, sizeof(zip_path));

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

    extern uint8_t *g_track_environment_config; /* 0x4AEE20 */

    /*
     * The reverse direction flag would come from the game state.
     * For now we load the forward files if present and fall back to a
     * renderer-owned placeholder display list when the archive is missing.
     */

    strip_data = load_first_available_level_entry(track_index, s_strip_names, 2,
                                                  &strip_sz, strip_source, sizeof(strip_source));
    if (!strip_data || strip_sz <= 0) {
        TD5_LOG_W(LOG_TAG, "no STRIP.DAT found in %s, using placeholder track", zip_path);
        ok = td5_track_load_strip(NULL, 0) ? ok : 0;
    } else {
        TD5_LOG_I(LOG_TAG, "loaded strip data from %s", strip_source);
        ok = td5_track_load_strip(strip_data, (size_t)strip_sz) ? ok : 0;
    }

    left_data = load_first_available_level_entry(track_index, s_left_names, 2, &left_sz, NULL, 0);
    right_data = load_first_available_level_entry(track_index, s_right_names, 2, &right_sz, NULL, 0);
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
            TD5_LOG_I(LOG_TAG, "LEVELINF.DAT loaded: track_type=%d (circuit_flag=%d)",
                      g_td5.track_type, circuit_flag);
        } else {
            TD5_LOG_W(LOG_TAG, "missing or short %s in %s, defaulting to circuit", levelinf_name, zip_path);
            g_td5.track_type = TD5_TRACK_CIRCUIT;
            free(levelinf_data);
            levelinf_data = NULL;
        }
    }
    {
        char checkpoint_path[256];
        td5_asset_build_level_loose_path(track_index, checkpoint_name,
                                         checkpoint_path, sizeof(checkpoint_path));
        checkpoint_sz = td5_asset_get_entry_size_from_path(checkpoint_path, zip_path);
        if (checkpoint_sz < 0)
            checkpoint_sz = td5_asset_get_entry_size_from_path(checkpoint_name, zip_path);
        if (checkpoint_sz < 0)
            TD5_LOG_W(LOG_TAG, "missing optional entry: %s in %s", checkpoint_name, zip_path);
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
 * Track Texture Loading
 * ======================================================================== */

int td5_asset_load_track_textures(int track_index)
{
    char zip_path[256];
    td5_asset_build_level_zip_path(track_index, zip_path, sizeof(zip_path));
    const uint32_t k_white_tex[4] = {
        0xFFFFFFFFu, 0xFFFFFFFFu,
        0xFFFFFFFFu, 0xFFFFFFFFu
    };
    void *tex_data = NULL;
    int tex_sz = 0;
    int page_count = 0;
    int loaded_count = 0;
    int missing_pages = 0;

    tex_data = td5_asset_open_and_read("TEXTURES.DAT", zip_path, &tex_sz);
    if (tex_data && tex_sz >= 4) {
        page_count = (int)(*(uint32_t *)tex_data);
        if (page_count <= 0 || page_count > TD5_TRACK_TEXTURE_PAGE_LIMIT)
            page_count = 0;
    }
    if (page_count <= 0)
        page_count = TD5_TRACK_TEXTURE_PAGE_LIMIT;

    for (int page = 0; page < page_count && page < TD5_TRACK_TEXTURE_PAGE_LIMIT; page++) {
        char png_path[512];
        if (td5_asset_build_track_texture_png_path(track_index, page,
                                                   png_path, sizeof(png_path))) {
            if (td5_asset_upload_png_texture_page(page, png_path, NULL)) {
                loaded_count++;
                missing_pages = 0;
                continue;
            }
        }

        /* Missing or undecodable page: keep the slot visible with a white fallback. */
        td5_plat_render_upload_texture(page, k_white_tex, 2, 2, 2);
        missing_pages++;
        if (tex_sz <= 0 && missing_pages >= 8 && page > 0)
            break;
    }

    TD5_LOG_I(LOG_TAG,
              "track textures: %s page_count=%d loaded=%d map=page %d -> td5_png_clean/levels/level%03d/textures/tex_%%03d.png",
              tex_data ? "TEXTURES.DAT" : "manifest-missing",
              page_count, loaded_count, 0, td5_asset_level_number(track_index));

    free(tex_data);
    return loaded_count > 0;
}

int td5_asset_load_race_texture_pages(void)
{
    /*
     * Load raw 256x256 R5G6B5 texture pages from the level ZIP archive.
     * Original function at 0x441E40.  Each "tpage%d.dat" inside the level
     * ZIP is 256*256*2 = 131072 bytes of raw 16-bit pixel data.
     */
    static const uint32_t k_white_tex[4] = {
        0xFF5D544Cu, 0xFF6E655Cu,
        0xFF4A433Du, 0xFF5B534Cu
    };

    /* Ensure fallback texture is always available */
    if (!s_fallback_texture_uploaded) {
        s_fallback_texture_uploaded = td5_plat_render_upload_texture(
            TD5_FALLBACK_TEXTURE_PAGE, k_white_tex, 2, 2, 2);
    }

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

    /* Temp BGRA buffer for one 64x64 page */
    uint8_t *rgba = (uint8_t *)malloc(64 * 64 * 4);
    if (!rgba) { free(tex_data); return 0; }

    for (uint32_t pg = 0; pg < page_count; pg++) {
        uint32_t off = offsets[pg];
        if (off + 8 > (uint32_t)tex_size) continue;

        uint8_t *page_ptr = tex_data + off;
        uint8_t  page_type = page_ptr[3];
        int32_t  pal_count = *(int32_t *)(page_ptr + 4);
        if (pal_count < 0 || pal_count > 256) continue;

        uint8_t *palette = page_ptr + 8;
        uint8_t *indices = palette + pal_count * 3;

        if (indices + 4096 > tex_data + tex_size) continue;

        /* Decode 4096 palette-indexed pixels to BGRA32 */
        for (int px = 0; px < 4096; px++) {
            int idx = indices[px];
            int ci = idx * 3;
            uint8_t alpha;
            uint8_t b_val, g_val, r_val;

            if (ci + 2 < pal_count * 3) {
                /* Palette is BGR: pal[0]=B, pal[1]=G, pal[2]=R */
                b_val = palette[ci + 0];
                g_val = palette[ci + 1];
                r_val = palette[ci + 2];
            } else {
                b_val = g_val = r_val = 0;
            }

            switch (page_type) {
            case 1: /* Alpha-keyed: index 0 = transparent */
                alpha = (idx == 0) ? 0x00 : 0xFF;
                break;
            case 2: /* Semi-transparent */
                alpha = 0x80;
                break;
            default: /* 0, 3: opaque */
                alpha = 0xFF; /* Note: type 0 originally uses alpha=0 but
                               * D3D11 alpha blend treats 0 as invisible.
                               * Force opaque for correct rendering. */
                break;
            }

            /* BGRA byte order for D3D11 B8G8R8A8_UNORM */
            rgba[px * 4 + 0] = b_val;
            rgba[px * 4 + 1] = g_val;
            rgba[px * 4 + 2] = r_val;
            rgba[px * 4 + 3] = alpha;
        }

        td5_plat_render_upload_texture((int)pg, rgba, 64, 64, 2);
        loaded_count++;
    }

    free(rgba);
    free(tex_data);

    TD5_LOG_I(LOG_TAG, "race texture pages: level=%03d loaded=%d/%u fallback=%d",
              level_number, loaded_count, page_count, s_fallback_texture_uploaded);
    return loaded_count > 0 || s_fallback_texture_uploaded;
}
/* ========================================================================
 * Vehicle Asset Loading -- LoadRaceVehicleAssets (0x443280)
 *
 * Phase 1: Query himodel.dat sizes and compute a single allocation.
 * Phase 2: Load himodel.dat + carparam.dat per racer slot.
 * Phase 3: Patch UV coords for 2-car-per-page tiling.
 * Phase 4: Load traffic models from traffic.zip.
 * ======================================================================== */

/* Car ZIP archive paths indexed by car_index (matches frontend table) */
static const char *s_car_zip_paths[37] = {
    "cars/128.zip", "cars/69v.zip", "cars/97c.zip", "cars/atp.zip", "cars/c21.zip",
    "cars/cam.zip", "cars/cat.zip", "cars/chv.zip", "cars/cob.zip", "cars/cop.zip",
    "cars/crg.zip", "cars/cud.zip", "cars/day.zip", "cars/fhm.zip", "cars/frd.zip",
    "cars/gto.zip", "cars/gtr.zip", "cars/hot.zip", "cars/jag.zip", "cars/mus.zip",
    "cars/nis.zip", "cars/pit.zip", "cars/sky.zip", "cars/sp1.zip", "cars/sp2.zip",
    "cars/sp3.zip", "cars/sp4.zip", "cars/sp5.zip", "cars/sp6.zip", "cars/sp7.zip",
    "cars/sp8.zip", "cars/ss1.zip", "cars/tvr.zip", "cars/van.zip", "cars/vet.zip",
    "cars/vip.zip", "cars/xkr.zip"
};

int td5_asset_load_vehicle(int car_index, int slot)
{
    char zip_path[256];
    if (car_index >= 0 && car_index < 37) {
        snprintf(zip_path, sizeof(zip_path), "%s", s_car_zip_paths[car_index]);
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

    /* The buffer is kept alive -- the render system holds a pointer to it. */
    TD5_MeshHeader *mesh = (TD5_MeshHeader *)mesh_data;

    /* Relocate internal offsets (commands, vertices, normals) to absolute ptrs */
    td5_track_prepare_mesh_resource(mesh);

    /* Scale from integer coordinate system (256 units = 1 world unit) to the
     * 1/256 render scale, matching td5_track_parse_models_dat processing. */
    {
        const float inv256 = 1.0f / 256.0f;
        mesh->origin_x          *= inv256;
        mesh->origin_y          *= inv256;
        mesh->origin_z          *= inv256;
        mesh->bounding_center_x *= inv256;
        mesh->bounding_center_y *= inv256;
        mesh->bounding_center_z *= inv256;
        mesh->bounding_radius   *= inv256;

        TD5_MeshVertex *verts = (TD5_MeshVertex *)(uintptr_t)mesh->vertices_offset;
        if (verts && mesh->total_vertex_count > 0) {
            for (int v = 0; v < mesh->total_vertex_count; v++) {
                verts[v].pos_x *= inv256;
                verts[v].pos_y *= inv256;
                verts[v].pos_z *= inv256;
            }
        }
    }

    /* Register mesh with render system */
    td5_render_set_vehicle_mesh(slot, mesh);

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
#define TD5_CAR_MESH_SKIN_ID  7
#define TD5_CAR_MESH_HUB_ID   8
    {
        int skin_page = TD5_CAR_TEXTURE_PAGE_BASE + slot * 2;
        int hub_page  = TD5_CAR_TEXTURE_PAGE_BASE + slot * 2 + 1;

        /* Load and upload carskin0.tga */
        int tga_size = 0;
        void *tga_data = td5_asset_open_and_read("carskin0.tga", zip_path, &tga_size);
        if (tga_data) {
            void *pixels = NULL; int tw = 0, th = 0;
            if (td5_asset_decode_tga(tga_data, (size_t)tga_size, &pixels, &tw, &th)) {
                td5_plat_render_upload_texture(skin_page, pixels, tw, th, 2);
                TD5_LOG_I(LOG_TAG, "vehicle slot=%d: carskin0 %dx%d -> page %d", slot, tw, th, skin_page);
                free(pixels);
            }
            free(tga_data);
        } else {
            TD5_LOG_W(LOG_TAG, "vehicle slot=%d: carskin0.tga not found in %s", slot, zip_path);
        }

        /* Load and upload carhub0.tga */
        tga_data = td5_asset_open_and_read("carhub0.tga", zip_path, &tga_size);
        if (tga_data) {
            void *pixels = NULL; int tw = 0, th = 0;
            if (td5_asset_decode_tga(tga_data, (size_t)tga_size, &pixels, &tw, &th)) {
                td5_plat_render_upload_texture(hub_page, pixels, tw, th, 2);
                TD5_LOG_I(LOG_TAG, "vehicle slot=%d: carhub0 %dx%d -> page %d", slot, tw, th, hub_page);
                free(pixels);
            }
            free(tga_data);
        } else {
            TD5_LOG_W(LOG_TAG, "vehicle slot=%d: carhub0.tga not found in %s", slot, zip_path);
        }

        /* Patch PrimitiveCmd page IDs: 7→skin, 8→hub */
        mesh->texture_page_id = (int16_t)skin_page;
        TD5_PrimitiveCmd *cmds = (TD5_PrimitiveCmd *)(uintptr_t)mesh->commands_offset;
        for (int c = 0; c < mesh->command_count; c++) {
            if (cmds[c].texture_page_id == TD5_CAR_MESH_SKIN_ID)
                cmds[c].texture_page_id = (int16_t)skin_page;
            else if (cmds[c].texture_page_id == TD5_CAR_MESH_HUB_ID)
                cmds[c].texture_page_id = (int16_t)hub_page;
        }
    }

    /* --- carparam.dat (deferred) ----------------------------------------- */
    TD5_LOG_I(LOG_TAG, "vehicle slot=%d car=%d: carparam.dat loading deferred (using defaults)",
              slot, car_index);

    return 1;
}

/* ========================================================================
 * Mipmap Builder -- ParseAndDecodeCompressedTrackData (0x430D30)
 *
 * Generates a mipmap chain by box-filtering from source dimensions
 * down through each power-of-two level.
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
