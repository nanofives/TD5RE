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
#include <stdbool.h>

/* ========================================================================
 * Module lifecycle
 * ======================================================================== */

int  td5_asset_init(void);
void td5_asset_shutdown(void);

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

/* ========================================================================
 * TGA Decoding
 *
 * Reimplements DecodeArchiveImageToRgb24 (0x442E00) which is a custom
 * TGA decoder supporting types 1, 2, 9, 10 (uncompressed/RLE, indexed/RGB).
 * Output is always 24-bit RGB (B,G,R byte order as per original).
 * ======================================================================== */

/**
 * Decode a TGA image from raw file data to 24-bit RGB pixels.
 *
 * @param tga_data    Pointer to raw TGA file bytes.
 * @param rgb_out     Output buffer for decoded RGB24 pixels (must be
 *                    width * height * 3 bytes). Pixels are in BGR order
 *                    matching the original engine convention.
 *
 * Handles TGA types: 1 (indexed), 2 (truecolor), 9 (RLE indexed),
 * 10 (RLE truecolor). Supports 16/24/32-bit source formats.
 * Applies vertical/horizontal flip per TGA descriptor flags.
 */
void td5_asset_decode_tga_to_rgb24(const void *tga_data, void *rgb_out);

/**
 * Decode a TGA to RGBA32. Returns malloc'd buffer; caller frees.
 * Sets *width_out and *height_out. Returns 0 on failure.
 */
int td5_asset_decode_tga(const void *data, size_t size, void **pixels_out,
                          int *width_out, int *height_out);

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
int  td5_asset_decode_png_rgba32(const char *path,
                                  void **pixels_out, int *w_out, int *h_out);
int  td5_asset_decode_tga(const void *data, size_t size,
                           void **pixels_out, int *w_out, int *h_out);

#endif /* TD5_ASSET_H */
