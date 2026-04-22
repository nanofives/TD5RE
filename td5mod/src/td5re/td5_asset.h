/**
 * td5_asset.h -- ZIP archive reading, TGA decoding, mesh parsing, asset loading
 *
 * Original functions:
 *   0x43FC80  ParseZipCentralDirectory
 *   0x4405B0  DecompressZipEntry
 *   0x440790  ReadArchiveEntry
 *   0x440860  OpenArchiveFileForRead
 *   0x4409B0  GetArchiveEntrySize
 *   0x442CF0  FindArchiveEntryByName
 *   0x442E00  DecodeArchiveImageToRgb24
 *   0x42FB90  LoadTrackRuntimeData
 *   0x443280  LoadRaceVehicleAssets
 *   0x442670  LoadTrackTextureSet
 *   0x442770  LoadRaceTexturePages
 *   0x430D30  ParseAndDecodeCompressedTrackData (mipmap builder)
 *   DX::ImageProTGA (TGA decoder in M2DX.dll)
 */

#ifndef TD5_ASSET_H
#define TD5_ASSET_H

#include "td5_types.h"
#include "td5_hud.h"   /* for TD5_AtlasEntry */
#include <stdbool.h>

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_asset_init(void);
void td5_asset_shutdown(void);

/* Returns 1 if static atlas slot was loaded from a real .dat file, 0 otherwise.
 * Use to guard synthetic texture generation so it only runs when the .dat is absent. */
int  td5_asset_static_tpage_is_real(int slot);

/* Per-tpage transparency type (raw byte +3 of original tpage descriptor):
 *   0 = opaque, 1 = color-keyed, 2 = semi-transparent (alpha 0x80), 3 = additive
 *   -1 = unknown / not registered (caller should treat as 0).
 * Setter is called by the texture loaders right after upload; the renderer
 * queries it during td5_render_bind_texture_page to pick the blend preset.
 * Mirrors the per-page metadata in BindRaceTexturePage @ 0x0040B660. */
void td5_asset_set_page_transparency(int page_id, int transparency);
int  td5_asset_get_page_transparency(int page_id);

/* ========================================================================
 * ZIP Archive System
 *
 * The original EXE has its own direct ZIP reader (not using M2DX VFS for
 * game assets). The source port reimplements this using miniz for DEFLATE.
 *
 * Both GetArchiveEntrySize and ReadArchiveEntry check for a loose file on
 * disk first (fopen the entry name directly). If found, the loose file is
 * used instead of the ZIP. This provides a trivial modding override path.
 * ======================================================================== */

/** Opaque archive handle. */
typedef struct TD5_Archive TD5_Archive;

/** Maximum filename length inside a ZIP entry. */
#define TD5_ZIP_MAX_FILENAME  260

/** Maximum number of entries in a single archive (generous upper bound). */
#define TD5_ZIP_MAX_ENTRIES   4096

/**
 * Central directory entry (parsed from ZIP).
 * Stored in the TD5_Archive after opening.
 */
typedef struct TD5_ZipEntry {
    char     name[TD5_ZIP_MAX_FILENAME];
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint32_t local_header_offset;
    uint16_t compression_method;    /* 0 = stored, 8 = deflate */
    uint32_t crc32;
} TD5_ZipEntry;

/**
 * Archive handle -- holds the path to the ZIP and its parsed central directory.
 */
struct TD5_Archive {
    char          path[TD5_ZIP_MAX_FILENAME];
    TD5_ZipEntry *entries;
    int           entry_count;
};

/**
 * Open a ZIP archive and parse its central directory.
 * Returns NULL on failure.
 */
TD5_Archive *td5_asset_open_archive(const char *path);

/**
 * Close an archive and free all associated memory.
 */
void td5_asset_close_archive(TD5_Archive *arc);

/**
 * Get the uncompressed size of an entry by name.
 * Checks for a loose file override first.
 * Returns the size, or -1 if not found.
 */
int td5_asset_get_entry_size(TD5_Archive *arc, const char *name);

/**
 * Read and decompress an entry into the provided buffer.
 * Checks for a loose file override first.
 * Returns the number of bytes written, or -1 on failure.
 */
int td5_asset_read_entry(TD5_Archive *arc, const char *name,
                         void *buf, int max_size);

/**
 * Convenience: get entry size by archive path (opens/closes automatically).
 * Matches original GetArchiveEntrySize (0x4409B0) signature.
 */
int td5_asset_get_entry_size_from_path(const char *entry_name,
                                       const char *zip_path);

/**
 * Convenience: read entry by archive path (opens/closes automatically).
 * Matches original ReadArchiveEntry (0x440790) signature.
 */
int td5_asset_read_entry_from_path(const char *entry_name,
                                   const char *zip_path,
                                   void *buf, int max_size);

/**
 * Allocate and read an entry, returning the buffer (caller must free).
 * Matches original OpenArchiveFileForRead (0x440860).
 * Sets *out_size to the uncompressed size.
 */
void *td5_asset_open_and_read(const char *entry_name,
                              const char *zip_path,
                              int *out_size);

/* ========================================================================
 * Static HED Texture Directory (from static.zip)
 *
 * Parsed from static.hed at race init time.
 * ======================================================================== */

/** Named texture entry from static.hed (0x40 = 64 bytes). */
typedef struct TD5_StaticHedEntry {
    char     name[44];          /* +0x00: null-terminated ASCII */
    int32_t  pos_x;             /* +0x2C */
    int32_t  pos_y;             /* +0x30 */
    int32_t  width;             /* +0x34 */
    int32_t  height;            /* +0x38 */
    int32_t  texture_slot;      /* +0x3C: biased by +0x400 at load time */
} TD5_StaticHedEntry;

/** Per-page metadata from static.hed (0x10 = 16 bytes). */
typedef struct TD5_PageMetadata {
    int32_t  transparency_flag; /* 0=opaque (3bpp), 2=alpha (4bpp) */
    int32_t  image_type;        /* 0=LoadRGBS24, 1=LoadRGBS32/7, 2=LoadRGBS32/8 */
    int32_t  source_width;
    int32_t  source_height;
} TD5_PageMetadata;

/**
 * Find a named entry in the static.hed entry array.
 * Case-insensitive search matching original FindArchiveEntryByName (0x442CF0).
 * Returns pointer to the entry, or NULL if not found.
 */
TD5_StaticHedEntry *td5_asset_find_entry_by_name(
    TD5_StaticHedEntry *entries, int entry_count, const char *name);

/**
 * Look up a named HUD/UI sprite in the static atlas (loaded from static.hed).
 * Case-insensitive. Always returns non-NULL (fallback zeroed entry on miss).
 * Matches original FindArchiveEntryByName (0x442CF0) used by HUD module.
 */
TD5_AtlasEntry *td5_asset_find_atlas_entry(void *context, const char *name);

/* ========================================================================
 * Level Data Loading
 * ======================================================================== */

/**
 * Load all track runtime data files from level%03d.zip.
 * Matches LoadTrackRuntimeData (0x42FB90).
 *
 * Loads: STRIP.DAT/STRIPB.DAT, LEFT.TRK/LEFTB.TRK, RIGHT.TRK/RIGHTB.TRK,
 *        TRAFFIC.BUS/TRAFFICB.BUS, CHECKPT.NUM, LEVELINF.DAT
 */
int td5_asset_load_level(int track_index);

/**
 * Load track texture definitions (TEXTURES.DAT + static.hed).
 * Matches LoadTrackTextureSet (0x442670).
 */
int td5_asset_load_track_textures(int track_index);

/**
 * Upload all texture pages to GPU (track, sky, car skins, wheels, traffic, env).
 * Matches LoadRaceTexturePages (0x442770).
 */
int td5_asset_load_race_texture_pages(void);

/**
 * Load environment/reflection texture PNGs from re/assets/environs/.
 * Uploads up to max_pages PNGs to GPU starting at page_base.
 * Returns number of pages loaded, fills out_pages[] with D3D page IDs.
 */
int td5_asset_load_environs_pages(int page_base, int max_pages, int *out_pages);

/**
 * Get loaded CHECKPT.NUM data (up to 96 bytes, first 24 = active record).
 * Returns NULL if no checkpoint data loaded for current track.
 */
const void *td5_asset_get_checkpoint_data(int *out_size);

/* ========================================================================
 * Vehicle Asset Loading
 * ======================================================================== */

/**
 * Load all vehicle models, tuning data, and sounds for the current race.
 * Matches LoadRaceVehicleAssets (0x443280).
 *
 * Phase 1: Query sizes and allocate a single 32-aligned buffer for all models.
 * Phase 2: Load himodel.dat + carparam.dat per racer slot.
 * Phase 3: Patch UV coords for 2-car-per-page tiling.
 * Phase 4: Load traffic vehicle models from traffic.zip.
 */
int td5_asset_load_vehicle(int car_index, int slot);

/** Get the ZIP archive path for a car by index (0-36). Returns NULL if out of range. */
const char *td5_asset_get_car_zip_path(int car_index);

/**
 * Load a traffic vehicle model (model%d.prr + skin%d.png) from traffic.zip into
 * the given actor slot (expected range 6..11). Mirrors Phase 4 of
 * LoadRaceVehicleAssets (0x00443280) which populates the traffic half of the
 * 12-slot race actor table. Returns 1 on success, 0 on failure.
 */
int td5_asset_load_traffic_model(int model_index, int slot);

/**
 * Resolve the traffic model index for a given race slot (0..5) based on the
 * active track. Mirrors the two-level lookup chain inside
 * LoadRaceVehicleAssets @ 0x00443280:
 *   gScheduleToPoolIndex[track_index] -> pool_row
 *   gTrackPoolSpanCountTable[pool_row] (or Reverse variant) -> pool_idx
 *   DAT_00474d74[pool_idx] -> row
 *   DAT_00474ce8[row][slot_in_pool] -> model_index
 *
 * Returns the model index (0..30) in traffic.zip, or -1 when the track has
 * no traffic pool (original behavior: pool_idx >= 25 gates the size query
 * and causes zero-sized allocation, effectively skipping model load).
 */
int td5_asset_resolve_traffic_model_index(int track_index, int reverse, int slot_in_pool);

/* ========================================================================
 * Mipmap Generation
 * ======================================================================== */

/**
 * Generate a mipmap chain from source pixel data.
 * Matches ParseAndDecodeCompressedTrackData (0x430D30).
 */
int td5_asset_build_mipmaps(const void *src, int width, int height, int format,
                             void *dst, int *dst_size);

/* ========================================================================
 * Memory Helpers
 * ======================================================================== */

void *td5_asset_alloc(size_t size);
void *td5_asset_alloc_aligned(size_t size, size_t align);
void  td5_asset_free(void *ptr);

/* ========================================================================
 * Utilities (used by render sky, etc.)
 * ======================================================================== */

int  td5_asset_level_number(int track_index);
/** Load PNG and return BGRA32 pixels (R↔B swapped for D3D11 B8G8R8A8_UNORM).
 *  Despite the legacy name, output is BGRA, not RGBA. */
int  td5_asset_decode_png_rgba32(const char *path,
                                  void **pixels_out, int *w_out, int *h_out);


/* ========================================================================
 * Unified PNG Texture Loading
 *
 * All source-port texture loads should go through these functions.
 * Canonical texture directory: re/assets/
 * ======================================================================== */

/** Color-key modes for PNG texture loading. */
typedef enum {
    TD5_COLORKEY_NONE   = 0,  /* No transparency keying */
    TD5_COLORKEY_BLACK  = 1,  /* Near-black (R<8,G<8,B<8) → alpha=0 */
    TD5_COLORKEY_RED    = 2,  /* Pure red (R=255,G=0,B=0) → alpha=0 */
    TD5_COLORKEY_BLUE88 = 3,  /* Dark blue (0,0,88) → alpha=0 (car previews) */
    TD5_COLORKEY_CYAN   = 4,  /* Cyan (R<16,G>240,B>240) → alpha=0 (PAUSETXT bg) */
} TD5_ColorKeyMode;

/**
 * Load a PNG texture, apply color keying, and upload to a GPU texture page.
 * Returns 1 on success, 0 on failure (file not found, decode error, etc.).
 */
int td5_asset_load_png_texture(int page_index, const char *png_path,
                               TD5_ColorKeyMode colorkey);

/**
 * Load a PNG texture into a caller-owned pixel buffer (BGRA32, GPU-ready).
 * Applies color keying in-place. Caller must free *pixels_out with free().
 * Returns 1 on success, 0 on failure.
 */
int td5_asset_load_png_to_buffer(const char *png_path, TD5_ColorKeyMode colorkey,
                                 void **pixels_out, int *w_out, int *h_out);

/**
 * Resolve a ZIP entry name + archive path to a re/assets PNG path.
 * Strips .tga extension, maps archive to subfolder, checks file exists.
 * Returns 1 if resolved PNG exists, 0 otherwise.
 */
int td5_asset_resolve_png_path(const char *entry_name, const char *archive,
                               char *out_path, size_t out_size);

#endif /* TD5_ASSET_H */
