/**
 * td5_asset_dump.h - Asset dump module for texture extraction
 *
 * Intercepts ReadArchiveEntry to save every file loaded from ZIP archives
 * to a structured dump directory. Files are saved in their original format
 * (TGA, DAT, etc.) organized by archive source. A Python post-processing
 * script converts image files to PNG.
 *
 * This module is independent of the rendering backend — works with
 * dgVoodoo2, D3D11 wrapper, or native DirectDraw.
 *
 * INI config:
 *   [AssetDump]
 *   Enable=1
 *   DumpDir=td5_dump       ; relative to CWD (data/)
 *   SkipExisting=1         ; don't re-dump files already on disk
 */

#ifndef TD5_ASSET_DUMP_H
#define TD5_ASSET_DUMP_H

#include <windows.h>
#include <stdint.h>

/**
 * Initialize the asset dump system. Call once at startup.
 * ini_path: full path to td5_mod.ini
 * Returns 1 on success (dump enabled), 0 if disabled or error.
 */
int AssetDump_Init(const char *ini_path);

/**
 * Called after ReadArchiveEntry returns with file data.
 * Saves the file to the dump directory.
 *
 * entryName: filename within the archive (e.g., "FORWSKY.TGA")
 * zipPath:   archive path (e.g., "level001.zip", "cars\\cam.zip")
 * data:      pointer to decompressed file data
 * dataSize:  number of bytes read
 */
void AssetDump_OnFileRead(const char *entryName, const char *zipPath,
                          const void *data, uint32_t dataSize);

/**
 * Write a summary manifest of all dumped files. Call at shutdown or on demand.
 */
void AssetDump_WriteManifest(void);

/**
 * Shutdown and flush. Call at DLL_PROCESS_DETACH.
 */
void AssetDump_Shutdown(void);

#endif /* TD5_ASSET_DUMP_H */
